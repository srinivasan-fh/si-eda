#!/usr/bin/env python3
"""Trains SiEDA's own circuit model and writes it as GGUF for the built-in engine (Core/src/LocalModel.cpp).

    pip install torch tokenizers gguf numpy        # (llama-cpp-python optional: Q8_0 quantisation + cross-check)
    python3 tools/circuit_lm/make_dataset.py data
    python3 tools/circuit_lm/train.py data out     # → out/sieda-circuit-v1.gguf, out/metrics.json

A byte-level BPE tokenizer learned on the plans, and a small Qwen 2-style decoder (RMSNorm, rotary positions,
grouped-query attention, SwiGLU, tied embeddings) trained from scratch on the request → plan pairs; the loss covers the
plan only. Runs on a CPU (about an hour on 4 cores).
"""
import hashlib
import json
import math
import os
import random
import sys
import time

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
from tokenizers import ByteLevelBPETokenizer

SYSTEM = "You are an electronics design assistant."  # what sieda-cli --chat and the app send
SPECIALS = ["<|endoftext|>", "<|im_start|>", "<|im_end|>"]
CFG = dict(vocab=2048, embd=256, layers=6, heads=8, kv_heads=4, ff=704, ctx=2048, rope=10000.0, eps=1e-6)


def chat_prefix(prompt):
    return f"<|im_start|>system\n{SYSTEM}<|im_end|>\n<|im_start|>user\n{prompt}<|im_end|>\n<|im_start|>assistant\n"


class RMSNorm(nn.Module):
    def __init__(self, n):
        super().__init__()
        self.weight = nn.Parameter(torch.ones(n))

    def forward(self, x):
        return x * torch.rsqrt(x.pow(2).mean(-1, keepdim=True) + CFG["eps"]) * self.weight


class Block(nn.Module):
    def __init__(self):
        super().__init__()
        e, h, kv = CFG["embd"], CFG["heads"], CFG["kv_heads"]
        hd = e // h
        self.attn_norm, self.ffn_norm = RMSNorm(e), RMSNorm(e)
        self.q, self.k, self.v = nn.Linear(e, h * hd), nn.Linear(e, kv * hd), nn.Linear(e, kv * hd)
        self.o = nn.Linear(h * hd, e, bias=False)
        self.gate, self.up = nn.Linear(e, CFG["ff"], bias=False), nn.Linear(e, CFG["ff"], bias=False)
        self.down = nn.Linear(CFG["ff"], e, bias=False)

    def forward(self, x, cos, sin):
        b, t, e = x.shape
        h, kv = CFG["heads"], CFG["kv_heads"]
        hd = e // h
        y = self.attn_norm(x)
        q = self.q(y).view(b, t, h, hd).transpose(1, 2)
        k = self.k(y).view(b, t, kv, hd).transpose(1, 2)
        v = self.v(y).view(b, t, kv, hd).transpose(1, 2)

        def rope(z):  # half-split (Qwen / NeoX) rotary positions
            z1, z2 = z[..., : hd // 2], z[..., hd // 2:]
            return torch.cat([z1 * cos - z2 * sin, z1 * sin + z2 * cos], -1)

        q, k = rope(q), rope(k)
        k, v = k.repeat_interleave(h // kv, 1), v.repeat_interleave(h // kv, 1)
        a = F.scaled_dot_product_attention(q, k, v, is_causal=True)
        x = x + self.o(a.transpose(1, 2).reshape(b, t, e))
        y = self.ffn_norm(x)
        return x + self.down(F.silu(self.gate(y)) * self.up(y))


class Model(nn.Module):
    def __init__(self, vocab):
        super().__init__()
        self.emb = nn.Embedding(vocab, CFG["embd"])
        self.blocks = nn.ModuleList(Block() for _ in range(CFG["layers"]))
        self.norm = RMSNorm(CFG["embd"])
        hd = CFG["embd"] // CFG["heads"]
        inv = CFG["rope"] ** (-torch.arange(0, hd, 2).float() / hd)
        ang = torch.arange(CFG["ctx"]).float()[:, None] * inv[None]
        self.register_buffer("cos", ang.cos(), persistent=False)
        self.register_buffer("sin", ang.sin(), persistent=False)
        nn.init.normal_(self.emb.weight, std=0.02)

    def forward(self, ids):
        t = ids.shape[1]
        x = self.emb(ids)
        for blk in self.blocks:
            x = blk(x, self.cos[:t], self.sin[:t])
        return self.norm(x) @ self.emb.weight.T  # tied output


def write_gguf(model, tok, path):
    import gguf
    vocab = tok.get_vocab()
    tokens = [None] * len(vocab)
    for s, i in vocab.items():
        tokens[i] = s
    merges = json.loads(tok._tokenizer.to_str())["model"]["merges"]
    merges = [m if isinstance(m, str) else " ".join(m) for m in merges]
    w = gguf.GGUFWriter(path, "qwen2")
    w.add_name("SiEDA Circuit v1")
    w.add_context_length(CFG["ctx"])
    w.add_embedding_length(CFG["embd"])
    w.add_block_count(CFG["layers"])
    w.add_feed_forward_length(CFG["ff"])
    w.add_head_count(CFG["heads"])
    w.add_head_count_kv(CFG["kv_heads"])
    w.add_layer_norm_rms_eps(CFG["eps"])
    w.add_rope_freq_base(CFG["rope"])
    w.add_file_type(gguf.LlamaFileType.ALL_F32)
    w.add_tokenizer_model("gpt2")
    w.add_tokenizer_pre("default")  # the GPT-2 split (llama.cpp's name)
    w.add_token_list(tokens)
    w.add_token_types([3 if t in SPECIALS else 1 for t in tokens])
    w.add_token_merges(merges)
    w.add_bos_token_id(vocab["<|endoftext|>"])
    w.add_eos_token_id(vocab["<|im_end|>"])
    w.add_add_bos_token(False)
    w.add_chat_template("{% for m in messages %}<|im_start|>{{ m.role }}\n{{ m.content }}<|im_end|>\n{% endfor %}"
                        "<|im_start|>assistant\n")
    sd = {k: v.detach().float().numpy() for k, v in model.state_dict().items()}
    w.add_tensor("token_embd.weight", sd["emb.weight"])
    w.add_tensor("output_norm.weight", sd["norm.weight"])
    names = {"attn_norm.weight": "attn_norm.weight", "q.weight": "attn_q.weight", "q.bias": "attn_q.bias",
             "k.weight": "attn_k.weight", "k.bias": "attn_k.bias", "v.weight": "attn_v.weight", "v.bias": "attn_v.bias",
             "o.weight": "attn_output.weight", "ffn_norm.weight": "ffn_norm.weight", "gate.weight": "ffn_gate.weight",
             "up.weight": "ffn_up.weight", "down.weight": "ffn_down.weight"}
    for i in range(CFG["layers"]):
        for src, dst in names.items():
            w.add_tensor(f"blk.{i}.{dst}", sd[f"blocks.{i}.{src}"])
    w.write_header_to_file()
    w.write_kv_data_to_file()
    w.write_tensors_to_file()
    w.close()


def main():
    data, out = sys.argv[1], sys.argv[2]
    epochs = float(os.environ.get("EPOCHS", "10"))
    os.makedirs(out, exist_ok=True)
    torch.manual_seed(0)
    random.seed(0)
    rows = [json.loads(l) for l in open(os.path.join(data, "train.jsonl"))]
    test = [json.loads(l) for l in open(os.path.join(data, "test.jsonl"))]
    tok = ByteLevelBPETokenizer(add_prefix_space=False)
    tok.train_from_iterator([chat_prefix(r["prompt"]) + r["plan"] for r in rows], vocab_size=CFG["vocab"], min_frequency=2,
                            special_tokens=SPECIALS)
    end = tok.token_to_id("<|im_end|>")
    seqs = []
    for r in rows:
        a = tok.encode(chat_prefix(r["prompt"])).ids
        b = tok.encode(r["plan"]).ids + [end]
        if len(a) + len(b) <= CFG["ctx"]:
            seqs.append((a, b))
    print(f"{len(seqs)} sequences, vocab {tok.get_vocab_size()}, tokens {sum(len(a) + len(b) for a, b in seqs)}", flush=True)
    model = Model(tok.get_vocab_size())
    print(f"{sum(p.numel() for p in model.parameters()) / 1e6:.2f} M parameters", flush=True)
    opt = torch.optim.AdamW(model.parameters(), lr=2e-3, betas=(0.9, 0.95), weight_decay=0.05)
    budget = 6000  # tokens per step
    seqs.sort(key=lambda s: len(s[0]) + len(s[1]))
    batches, cur = [], []
    for s in seqs:
        if cur and (len(cur) + 1) * (len(s[0]) + len(s[1])) > budget:
            batches.append(cur)
            cur = []
        cur.append(s)
    batches.append(cur)
    total = int(len(batches) * epochs)
    step, t0 = 0, time.time()
    # A checkpoint every 200 steps: run the same command again to resume after an interruption.
    ckpt, vocab_id = os.path.join(out, "checkpoint.pt"), hashlib.sha1((tok._tokenizer.to_str() + json.dumps(CFG) + str(epochs)).encode()).hexdigest()
    if os.path.exists(ckpt):
        state = torch.load(ckpt)
        if state["vocab"] == vocab_id and state["total"] == total:
            model.load_state_dict(state["model"])
            opt.load_state_dict(state["opt"])
            step = state["step"]
            print(f"resumed at step {step}", flush=True)
    while step < total:
        random.shuffle(batches)
        for bt in batches:
            if step >= total:
                break
            n = max(len(a) + len(b) for a, b in bt)
            ids = torch.full((len(bt), n), end, dtype=torch.long)
            tgt = torch.full((len(bt), n), -100, dtype=torch.long)
            for i, (a, b) in enumerate(bt):
                s = a + b
                ids[i, : len(s)] = torch.tensor(s)
                tgt[i, len(a) - 1: len(s) - 1] = torch.tensor(b)  # predict the plan only
            lr = 2e-3 * min(1, (step + 1) / 100) * 0.5 * (1 + math.cos(math.pi * step / total))
            for g in opt.param_groups:
                g["lr"] = lr
            loss = F.cross_entropy(model(ids).view(-1, tok.get_vocab_size()), tgt.view(-1), ignore_index=-100)
            opt.zero_grad()
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            opt.step()
            step += 1
            if step % 50 == 0 or step == total:
                print(f"step {step}/{total} loss {loss.item():.4f} lr {lr:.2e} {time.time() - t0:.0f}s", flush=True)
            if step % 200 == 0:
                torch.save({"model": model.state_dict(), "opt": opt.state_dict(), "step": step, "total": total,
                            "vocab": vocab_id}, ckpt + ".tmp")
                os.replace(ckpt + ".tmp", ckpt)
    model.eval()
    path = os.path.join(out, "sieda-circuit-v1-f32.gguf")
    write_gguf(model, tok, path)
    if os.path.exists(ckpt):
        os.remove(ckpt)  # finished: the next run starts fresh
    with torch.no_grad():  # held-out loss
        losses = []
        for r in test:
            a = tok.encode(chat_prefix(r["prompt"])).ids
            b = tok.encode(r["plan"]).ids + [end]
            if len(a) + len(b) > CFG["ctx"]:
                continue
            ids = torch.tensor([a + b])
            logits = model(ids)[0, len(a) - 1: len(a) + len(b) - 1]
            losses.append(F.cross_entropy(logits, torch.tensor(b)).item())
    json.dump({"steps": total, "train_loss": loss.item(), "test_loss": float(np.mean(losses)), "train_pairs": len(seqs),
               "test_pairs": len(test), "parameters": sum(p.numel() for p in model.parameters()), "seconds": time.time() - t0},
              open(os.path.join(out, "metrics.json"), "w"), indent=1)
    print("wrote", path, "test loss", np.mean(losses))


if __name__ == "__main__":
    main()
