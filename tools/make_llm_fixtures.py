#!/usr/bin/env python3
"""Writes the tiny GGUF models and the llama.cpp reference results the built-in model's tests compare against.

    pip install gguf numpy llama-cpp-python==0.3.2
    python3 tools/make_llm_fixtures.py            # → Core/tests/fixtures/llm/

Two random-weight models: a Qwen 2 one (byte-level BPE tokenizer, quantised to Q4_K_M by llama.cpp itself) and a
Llama one (SentencePiece tokenizer, Q8_0). expected.json holds llama.cpp's tokens and logits for a few prompts.
"""
import json
import os
import sys

import numpy as np
import gguf
import llama_cpp

OUT = os.path.join(os.path.dirname(__file__), "..", "Core", "tests", "fixtures", "llm")
TEXT = ("The board has 4 layers, 0.2 mm tracks and a 3.3V regulator. Route the USB pair at 90 ohm!\n"
        "Place C1 near U1's VDD pin; keep the crystal close.  Don't route under it.\n") * 3
PROMPTS = ["Route the USB pair at 90 ohm", "C1 near U1's VDD pin, 3.3V!\n  keep 123456 mm", "<|im_start|>user\nhi<|im_end|>"]


def byte_chars():
    bs = list(range(ord("!"), ord("~") + 1)) + list(range(0xA1, 0xAD)) + list(range(0xAE, 0x100))
    cs, n = bs[:], 0
    for b in range(256):
        if b not in bs:
            bs.append(b)
            cs.append(256 + n)
            n += 1
    return {b: chr(c) for b, c in zip(bs, cs)}


def train_bpe(words, merges_wanted):
    seqs = [list(w) for w in words]
    merges = []
    for _ in range(merges_wanted):
        counts = {}
        for s in seqs:
            for a, b in zip(s, s[1:]):
                counts[(a, b)] = counts.get((a, b), 0) + 1
        if not counts:
            break
        (a, b), _ = max(counts.items(), key=lambda kv: (kv[1], kv[0]))
        merges.append((a, b))
        for s in seqs:
            i = 0
            while i < len(s) - 1:
                if s[i] == a and s[i + 1] == b:
                    s[i:i + 2] = [a + b]
                else:
                    i += 1
    return merges


def weights(rng, shape, scale=0.08):
    return (rng.standard_normal(shape) * scale).astype(np.float32)


def write_model(path, arch, E, L, H, HKV, F, tokens, types, extra):
    rng = np.random.default_rng(7 if arch == "qwen2" else 11)
    w = gguf.GGUFWriter(path, arch)
    w.add_name(f"tiny-{arch}")
    w.add_context_length(256)
    w.add_embedding_length(E)
    w.add_block_count(L)
    w.add_feed_forward_length(F)
    w.add_head_count(H)
    w.add_head_count_kv(HKV)
    w.add_layer_norm_rms_eps(1e-6)
    w.add_rope_freq_base(10000.0)
    w.add_file_type(gguf.LlamaFileType.ALL_F32)
    extra(w)
    w.add_token_list(tokens)
    w.add_token_types(types)
    V, hd = len(tokens), E // H
    w.add_tensor("token_embd.weight", weights(rng, (V, E), 0.5))
    w.add_tensor("output_norm.weight", 1 + weights(rng, (E,), 0.1))
    if arch == "llama":
        w.add_tensor("output.weight", weights(rng, (V, E), 0.2))
    for l in range(L):
        b = f"blk.{l}."
        w.add_tensor(b + "attn_norm.weight", 1 + weights(rng, (E,), 0.1))
        w.add_tensor(b + "attn_q.weight", weights(rng, (H * hd, E)))
        w.add_tensor(b + "attn_k.weight", weights(rng, (HKV * hd, E)))
        w.add_tensor(b + "attn_v.weight", weights(rng, (HKV * hd, E)))
        if arch == "qwen2":
            w.add_tensor(b + "attn_q.bias", weights(rng, (H * hd,), 0.3))
            w.add_tensor(b + "attn_k.bias", weights(rng, (HKV * hd,), 0.3))
            w.add_tensor(b + "attn_v.bias", weights(rng, (HKV * hd,), 0.3))
        w.add_tensor(b + "attn_output.weight", weights(rng, (E, H * hd)))
        w.add_tensor(b + "ffn_norm.weight", 1 + weights(rng, (E,), 0.1))
        w.add_tensor(b + "ffn_gate.weight", weights(rng, (F, E)))
        w.add_tensor(b + "ffn_up.weight", weights(rng, (F, E)))
        w.add_tensor(b + "ffn_down.weight", weights(rng, (E, F)))
    w.write_header_to_file()
    w.write_kv_data_to_file()
    w.write_tensors_to_file()
    w.close()


def quantize(src, dst, ftype):
    params = llama_cpp.llama_model_quantize_default_params()
    params.ftype = ftype
    if llama_cpp.llama_model_quantize(src.encode(), dst.encode(), params) != 0:
        sys.exit(f"quantize {dst} failed")
    os.remove(src)


def exact(path, arch, tokens):
    """Logits in float64 from the dequantised weights (llama.cpp rounds activations to 8 bits, so it is ~1 % off)."""
    r = gguf.GGUFReader(path)
    T = {t.name: (np.array(t.data) if t.tensor_type.name == "F32" else gguf.quants.dequantize(t.data, t.tensor_type))
         .reshape([int(x) for x in reversed(t.shape)]).astype(np.float64) for t in r.tensors}
    kv = lambda k: r.fields[k].parts[r.fields[k].data[0]][0]
    E, L = int(kv(f"{arch}.embedding_length")), int(kv(f"{arch}.block_count"))
    H, HK = int(kv(f"{arch}.attention.head_count")), int(kv(f"{arch}.attention.head_count_kv"))
    eps, hd, n = float(kv(f"{arch}.attention.layer_norm_rms_epsilon")), E // int(kv(f"{arch}.attention.head_count")), len(tokens)
    rms = lambda x, w: x / np.sqrt((x * x).mean(-1, keepdims=True) + eps) * w
    i = np.arange(hd // 2)
    theta = np.arange(n)[:, None] * 10000.0 ** (-2 * i / hd)
    cos, sin = np.cos(theta)[:, None, :], np.sin(theta)[:, None, :]

    def rope(v):
        a, b = (v[..., :hd // 2], v[..., hd // 2:]) if arch == "qwen2" else (v[..., 0::2], v[..., 1::2])
        a, b = a * cos - b * sin, a * sin + b * cos
        return np.concatenate([a, b], -1) if arch == "qwen2" else np.stack([a, b], -1).reshape(v.shape)

    x = T["token_embd.weight"][tokens]
    for l in range(L):
        b = f"blk.{l}."
        xb = rms(x, T[b + "attn_norm.weight"])
        q, k, v = (xb @ T[b + f"attn_{w}.weight"].T + T.get(b + f"attn_{w}.bias", 0) for w in "qkv")
        q, k, v = rope(q.reshape(n, H, hd)), rope(k.reshape(n, HK, hd)), v.reshape(n, HK, hd)
        out = np.zeros((n, H, hd))
        for h in range(H):
            s = q[:, h] @ k[:, h // (H // HK)].T / np.sqrt(hd) + np.triu(np.full((n, n), -1e30), 1)
            s = np.exp(s - s.max(1, keepdims=True))
            out[:, h] = (s / s.sum(1, keepdims=True)) @ v[:, h // (H // HK)]
        x = x + out.reshape(n, E) @ T[b + "attn_output.weight"].T
        xb = rms(x, T[b + "ffn_norm.weight"])
        g, u = xb @ T[b + "ffn_gate.weight"].T, xb @ T[b + "ffn_up.weight"].T
        x = x + (g / (1 + np.exp(-g)) * u) @ T[b + "ffn_down.weight"].T
    return rms(x[-1], T["output_norm.weight"]) @ T.get("output.weight", T["token_embd.weight"]).T


def reference(path, add_bos, arch):
    llm = llama_cpp.Llama(model_path=path, n_ctx=256, n_threads=1, n_batch=256, verbose=False, logits_all=True)
    cases = []
    for p in PROMPTS:
        toks = llm.tokenize(p.encode(), add_bos=add_bos, special=True)
        llm.reset()
        llm.eval(toks)
        logits = np.array(llm.scores[llm.n_tokens - 1], dtype=np.float64)
        greedy = []
        for _ in range(8):
            t = int(np.argmax(llm.scores[llm.n_tokens - 1]))
            greedy.append(t)
            llm.eval([t])
        cases.append({"text": p, "tokens": toks, "logits": [round(float(x), 5) for x in logits],
                      "exact": [round(float(x), 5) for x in exact(path, arch, toks)], "greedy": greedy})
    return cases


def main():
    os.makedirs(OUT, exist_ok=True)
    bc = byte_chars()
    # Qwen 2 style: byte-level BPE.
    words, cur = [], ""
    for ch in TEXT.encode():
        c = bc[ch]
        if ch in b" \n" and cur:
            words.append(cur)
            cur = ""
        cur += c
    merges = train_bpe(words + [cur], 120)
    tokens = [bc[b] for b in range(256)] + [a + b for a, b in merges]
    tokens = list(dict.fromkeys(tokens))
    types = [1] * len(tokens)
    for s in ["<|endoftext|>", "<|im_start|>", "<|im_end|>"]:
        tokens.append(s)
        types.append(3)

    def qwen_extra(w):
        w.add_tokenizer_model("gpt2")
        w.add_tokenizer_pre("qwen2")
        w.add_token_merges([f"{a} {b}" for a, b in merges])
        w.add_eos_token_id(tokens.index("<|im_end|>"))
        w.add_bos_token_id(tokens.index("<|endoftext|>"))
        w.add_add_bos_token(False)
        w.add_chat_template("{% for m in messages %}<|im_start|>{{ m.role }}\n{{ m.content }}<|im_end|>\n{% endfor %}")

    f32 = os.path.join(OUT, "tmp-qwen2.gguf")
    write_model(f32, "qwen2", 256, 2, 4, 2, 256, tokens, types, qwen_extra)
    qwen = os.path.join(OUT, "tiny-qwen2-q4_k_m.gguf")
    quantize(f32, qwen, llama_cpp.LLAMA_FTYPE_MOSTLY_Q4_K_M)

    # Llama style: SentencePiece with byte fallback.
    sp = ["<unk>", "<s>", "</s>"] + [f"<0x{b:02X}>" for b in range(256)]
    sp_types = [2, 3, 3] + [6] * 256
    pieces = {}
    for word in TEXT.replace("\n", " ").split(" "):
        word = "▁" + word
        for i in range(len(word)):
            for j in range(i + 1, min(len(word), i + 6) + 1):
                pieces[word[i:j]] = pieces.get(word[i:j], 0) + 1
    best = sorted(pieces.items(), key=lambda kv: (-kv[1] * len(kv[0]), kv[0]))[:220]
    scores = [0.0, 0.0, 0.0] + [0.0] * 256
    for i, (piece, count) in enumerate(best):
        sp.append(piece)
        sp_types.append(1)
        scores.append(-float(i) / 10)

    def llama_extra(w):
        w.add_tokenizer_model("llama")
        w.add_token_scores(scores)
        w.add_bos_token_id(1)
        w.add_eos_token_id(2)
        w.add_add_bos_token(True)
        w.add_chat_template("[INST] {{ messages[0].content }} [/INST]")

    f32 = os.path.join(OUT, "tmp-llama.gguf")
    write_model(f32, "llama", 64, 2, 4, 2, 128, sp, sp_types, llama_extra)
    llama = os.path.join(OUT, "tiny-llama-q8_0.gguf")
    quantize(f32, llama, llama_cpp.LLAMA_FTYPE_MOSTLY_Q8_0)

    expected = {"qwen2": reference(qwen, False, "qwen2"), "llama": reference(llama, True, "llama")}
    with open(os.path.join(OUT, "expected.json"), "w") as f:
        json.dump(expected, f, separators=(",", ":"))
    print("wrote", OUT)


if __name__ == "__main__":
    main()
