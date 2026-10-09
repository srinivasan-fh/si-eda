#!/usr/bin/env python3
"""Quantises the trained circuit model, scores it with SiEDA's own engine and writes what the app ships.

    python3 tools/circuit_lm/finish.py data out [sieda-cli]   # → SiEDA/Resources/Models/sieda-circuit-v1.{gguf,json}

Scoring runs every held-out request through `sieda-cli --chat` (the same engine and chat format as the app):
"usable" = the reply is a plan whose links all name its parts, "exact" = parts and links equal the reference plan.
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
    valid = exact = 0
    results = []
    for r in test:
        reply = subprocess.run([cli, "--chat", dst, r["prompt"]], capture_output=True, text=True, timeout=300).stdout.strip()
        try:
            plan = json.loads(reply)
        except ValueError:
            plan = None
        ok = plan is not None and usable(plan)
        ref = json.loads(r["plan"])
        same = ok and plan["components"] == ref["components"] and plan["connections"] == ref["connections"]
        valid += ok
        exact += same
        results.append({"prompt": r["prompt"], "usable": ok, "exact": same})
    metrics = json.load(open(os.path.join(out, "metrics.json")))
    card = json.load(open(os.path.join(data, "card.json")))
    # llama.cpp reads the same file and, greedy, writes the same first plan (a cross-check of engine and format).
    llm = llama_cpp.Llama(model_path=dst, n_ctx=1024, verbose=False)
    first = test[0]["prompt"]
    theirs = llm.create_chat_completion(messages=[{"role": "system", "content": "You are an electronics design assistant."},
                                                  {"role": "user", "content": first}], temperature=0, max_tokens=1000)
    ours = subprocess.run([cli, "--chat", dst, first], capture_output=True, text=True).stdout.strip()
    agree = theirs["choices"][0]["message"]["content"].strip() == ours
    shipped = {
        "parameters": metrics["parameters"], "trainPairs": metrics["train_pairs"], "testPairs": len(test),
        "testLoss": round(metrics["test_loss"], 4), "validPlans": round(valid / len(test), 4),
        "exactPlans": round(exact / len(test), 4), "llamaCppAgrees": agree,
        "circuits": card["circuits"], "kinds": card["kinds"],
        "benchmark": [{"prompt": r["prompt"], "plan": r["plan"]} for r in test[:20]],
    }
    models = os.path.join(ROOT, "SiEDA", "Resources", "Models")
    os.makedirs(models, exist_ok=True)
    os.replace(dst, os.path.join(models, "sieda-circuit-v1.gguf"))
    json.dump(shipped, open(os.path.join(models, "sieda-circuit-v1.json"), "w"), indent=1, ensure_ascii=False)
    json.dump(results, open(os.path.join(out, "test_results.json"), "w"), indent=1)
    print(f"usable {valid}/{len(test)}, exact {exact}/{len(test)}, llama.cpp agrees: {agree}")


if __name__ == "__main__":
    main()
