#!/usr/bin/env python3
"""Quantises the trained circuit model, scores it with SiEDA's own engine and writes what the app ships.

    python3 tools/circuit_lm/finish.py data out [sieda-cli]   # → SiEDA/Resources/Models/sieda-circuit-v1.{gguf,json}

Scoring runs every held-out request through `sieda-cli --circuit` (the same engine, chat format and value rules as
the app): "usable" = the reply is a plan whose links all name its parts, "exact" = the same parts and links as the reference
plan, in any order ("modelExact": the model's own plan, before the request's values are computed).
"""
import json
import os
import subprocess
import sys

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")


def usable(plan):
    comps = plan.get("components") or []
    refs = [c.get("ref") for c in comps]
    if not comps or len(set(refs)) != len(refs):
        return False
    return all(str(e).split(".")[0] in refs for c in plan.get("connections", []) for e in (c.get("from"), c.get("to")))


def same_circuit(plan, ref):
    """The same parts (in any order) and the same links (in any order, either direction)."""
    parts = lambda p: sorted(json.dumps(c, sort_keys=True) for c in p.get("components", []))
    links = lambda p: sorted("|".join(sorted((str(c.get("from")), str(c.get("to"))))) for c in p.get("connections", []))
    return parts(plan) == parts(ref) and links(plan) == links(ref)


def main():
    data, out = sys.argv[1], sys.argv[2]
    cli = sys.argv[3] if len(sys.argv) > 3 else os.path.join(ROOT, "build", "sieda-cli")
    import llama_cpp
    src, dst = os.path.join(out, "sieda-circuit-v1-f32.gguf"), os.path.join(out, "sieda-circuit-v1.gguf")
    params = llama_cpp.llama_model_quantize_default_params()
    params.ftype = llama_cpp.LLAMA_FTYPE_MOSTLY_Q8_0
    if llama_cpp.llama_model_quantize(src.encode(), dst.encode(), params) != 0:
        sys.exit("quantisation failed")
    test = [json.loads(l) for l in open(os.path.join(data, "test.jsonl"))]
    valid = exact = raw_exact = 0
    results = []
    for r in test:
        run = subprocess.run([cli, "--circuit", dst, r["prompt"]], capture_output=True, text=True, timeout=600)
        ref = json.loads(r["plan"])
        plans = []
        for text in (run.stdout.strip(), run.stderr.strip().splitlines()[-1] if run.stderr.strip() else ""):
            try:
                plans.append(json.loads(text))
            except ValueError:
                plans.append(None)
        plan, raw = plans
        same = lambda p: p is not None and usable(p) and same_circuit(p, ref)
        ok = plan is not None and usable(plan)
        valid += ok
        exact += same(plan)
        raw_exact += same(raw)
        results.append({"prompt": r["prompt"], "usable": ok, "exact": same(plan), "modelExact": same(raw)})
    metrics = json.load(open(os.path.join(out, "metrics.json")))
    card = json.load(open(os.path.join(data, "card.json")))
    # llama.cpp reads the same file and, greedy, writes the same first plan from the same ChatML prompt (a cross-check
    # of the engine; its chat helper formats the turn its own way, so the prompt is given as text).
    llm = llama_cpp.Llama(model_path=dst, n_ctx=2048, verbose=False)
    first = test[0]["prompt"]
    prompt = f"<|im_start|>system\nYou are an electronics design assistant.<|im_end|>\n<|im_start|>user\n{first}<|im_end|>\n<|im_start|>assistant\n"
    theirs = llm(prompt, temperature=0, max_tokens=2000)["choices"][0]["text"].strip()
    ours = subprocess.run([cli, "--chat", dst, first], capture_output=True, text=True).stdout.strip()
    agree = theirs == ours
    shipped = {
        "parameters": metrics["parameters"], "trainPairs": metrics["train_pairs"], "testPairs": len(test),
        "testLoss": round(metrics["test_loss"], 4), "validPlans": round(valid / len(test), 4),
        "exactPlans": round(exact / len(test), 4), "modelExactPlans": round(raw_exact / len(test), 4), "llamaCppAgrees": agree,
        "circuits": card["circuits"], "kinds": card["kinds"],
        "benchmark": [{"prompt": r["prompt"], "plan": r["plan"]} for r in test[:20]],
    }
    models = os.path.join(ROOT, "SiEDA", "Resources", "Models")
    os.makedirs(models, exist_ok=True)
    os.replace(dst, os.path.join(models, "sieda-circuit-v1.gguf"))
    json.dump(shipped, open(os.path.join(models, "sieda-circuit-v1.json"), "w"), indent=1, ensure_ascii=False)
    json.dump(results, open(os.path.join(out, "test_results.json"), "w"), indent=1)
    print(f"usable {valid}/{len(test)}, exact {exact}/{len(test)} (model alone {raw_exact}), llama.cpp agrees: {agree}")


if __name__ == "__main__":
    main()
