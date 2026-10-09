# AI back-ends

SiEDA's agents (analyst, designer, reviewer) ask one model back-end for structured JSON. Pick it in **Settings → AI**:
Anthropic Claude (default), OpenAI or any compatible endpoint, Google Gemini, OpenRouter, **Built-in (on this Mac)**,
Ollama, or the Offline Designer (reference circuits, no model).

## Super Intelligence window

**Window → Super Intelligence…** (⌥⌘I) walks through the set-up one step per tab, each tab marked ✓ once done and a
`n / 6` count at the bottom: **1 Provider** (turn AI on, pick the back-end; a link to the account / key settings when
one is missing) → **2 Local Model** (the built-in model list: download, import, choose — skipped for cloud providers)
→ **3 GPU** (Metal on / off and the GPU found) → **4 Test** (asks the model for a requirements summary and shows its
answer and time) → **5 Design** (opens Prompt Studio) → **6 Agent Clients (MCP)** (three steps to let Claude Desktop,
Claude Code, Cursor or VS Code drive the open design through the live MCP server, with the server settings and each
client's configuration to copy; ✓ after the client's first call — see docs/MCP.md). `SiEDA/AI/IntelligenceWindow.swift`.

## Built-in model

The built-in back-end runs an open model on this Mac with SiEDA's own engine — no Ollama or other app to install, no
API key, and no network once the model file is downloaded.

- **Models.** Settings → AI → Built-in lists the GGUF files in `~/Library/Application Support/SiEDA/Models`, like
  `ollama list`. *Download* fetches one of the suggested open-licence (Apache 2.0) models — Qwen 2.5 0.5B, 1.5B or 7B
  Instruct — with progress and cancel; *Model URL* downloads any other `.gguf` link (`ollama pull`); *Import GGUF
  File…* copies a file you already have; *Delete* removes one (`ollama rm`); *Use* picks the model the agents run.
- **What runs.** GGUF v2 / v3 files of the Llama family: Llama 2 / 3 / 3.1 / 3.2, Mistral, Qwen 2 / 2.5 / 3, in F32,
  F16, BF16, Q4_0, Q4_1, Q5_0, Q5_1, Q8_0, Q4_K, Q5_K or Q6_K (so the usual Q4_K_M, Q5_K_M and Q8_0 downloads). Other
  architectures (Gemma, Phi, mixture-of-experts …) are refused with a message.
- **How.** `Core/src/LocalModel.cpp`: the file is memory-mapped and stays quantised (one row is expanded at a time), the
  KV cache is f16, the prompt runs in batches of 64 tokens, and matrix rows are spread over the CPU cores (the result
  does not depend on the thread count). The chat format (ChatML, Llama 3, Mistral) comes from the file's template;
  the reply is constrained to start with `{` and stops when the JSON object closes. Requests ask for JSON following
  the agent's schema, written into the prompt.
- **GPU.** On a Mac with Metal (every Apple Silicon Mac), *Use the GPU (Metal)* (on by default) runs the matrix
  products — nearly all of the work — on the GPU (`SiEDA/AI/MetalMatmul.swift`): one no-copy buffer over the mapped
  file, one 32-wide threadgroup per weight row and per 8 inputs, the same block arithmetic as the CPU (the test
  `testMetalKernelsMatchTheCPU` holds every format to the CPU's result). Attention, norms and sampling stay on the
  CPU; a format or file the GPU cannot take (or a Mac without Metal) falls back to the CPU. The core sees the GPU only
  through `LocalModel::setAccelerator` / `sieda_llm_set_accelerator`, so it stays portable.
- **Speed and memory.** CPU numbers (the GPU is faster on a Mac, not measured here). Memory is about the file size plus a small KV cache. Measured on a 4-core x86 cloud
  machine with a 0.5B-size Q4_K_M model: about 55 prompt tokens / s and 7 generated tokens / s (13 with AVX2).
  Expect larger models to be proportionally slower; a 1.5B model is a good default for small circuits, a 7B one
  designs better but takes minutes per plan.
- **Accuracy.** `local_model_matches_llama_cpp` holds the engine's tokens equal to llama.cpp and its logits within
  0.5 % of an exact float64 computation on models quantised by llama.cpp (`tools/make_llm_fixtures.py`).

The same engine is available as `sieda-cli --chat model.gguf "question"` and through the C API (`sieda_llm_open`,
`sieda_llm_chat` with a streaming callback, `sieda_llm_close`).
