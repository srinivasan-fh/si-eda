// SiEDA Core — an on-device language model: runs GGUF models (Llama 2 / 3, Mistral, Qwen 2 / 2.5 and others of the
// Llama family) on the CPU with no outside library, so the app's AI works offline (docs/AI.md, "Built-in model").
//
// The file is read in place (memory-mapped); weights stay quantised (F32, F16, BF16, Q4_0, Q4_1, Q5_0, Q8_0, Q4_K,
// Q5_K, Q6_K) and are expanded one block at a time. Every read is bounds-checked: a damaged file does not load.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sieda {

class LocalModel {
public:
    struct Options {
        int maxTokens = 2048;
        double temperature = 0;  // 0 = always the most likely token (repeatable)
        double topP = 0.95;
        uint64_t seed = 1;
        int threads = 0;   // 0 = all cores
        bool json = false; // the reply is one JSON object: starts at '{', stops when it closes
    };

    LocalModel();
    ~LocalModel();
    /// Opens a .gguf file. False (with `error`) when it is not a model this engine runs.
    bool load(const std::string& path, std::string* error = nullptr);
    /// The same from bytes in memory (tests).
    bool loadBytes(std::string bytes, std::string* error = nullptr);
    bool valid() const;
    /// {"architecture", "name", "parameters", "layers", "context", "vocabulary", "quantization", "bytes"}.
    std::string infoJson() const;

    /// Token ids of `text`; `special` reads control tokens such as <|im_start|> written in the text.
    std::vector<int> tokenize(const std::string& text, bool special = false) const;
    std::string detokenize(const std::vector<int>& tokens) const;
    /// The prompt for one system + user turn in the model's own chat format (ChatML, Llama 3, Mistral / Llama 2).
    std::string chatPrompt(const std::string& system, const std::string& user) const;

    /// Continues `prompt` (read with special tokens) and returns the generated text. `onText` receives each piece as
    /// it is made; returning false stops early.
    std::string generate(const std::string& prompt, const Options& options,
                         const std::function<bool(const std::string&)>& onText = {});
    /// Logits after the last of `tokens`, run from an empty context (tests).
    std::vector<float> logits(const std::vector<int>& tokens, int threads = 0);

    /// A GPU (or other) back-end for the matrix products: gets the weight rows `offset` bytes into the model file
    /// (fileData), their GGUF type, row length `cols`, `rows`, `rowBytes`, and `batch` input vectors x (batch × cols);
    /// writes y (batch × rows) and returns true, or false to let the CPU do it.
    using Accelerator = std::function<bool(uint64_t offset, int type, int64_t cols, int64_t rows, size_t rowBytes,
                                           const float* x, int batch, float* y)>;
    void setAccelerator(Accelerator accelerator);
    /// The model file in memory (memory-mapped when opened with load), for a no-copy GPU buffer.
    const uint8_t* fileData(size_t* size) const;
    /// The CPU's product for the same arguments (one thread; tests of an accelerator). False for an unknown type or
    /// a row length that is not a whole number of blocks.
    static bool cpuMatmul(const uint8_t* weights, int type, int64_t cols, int64_t rows, size_t rowBytes, const float* x,
                          int batch, float* y);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// SiEDA's circuit model (tools/circuit_lm) writes a plan; the values its request fixes — LED series resistor, divider,
/// RC cut-off, op-amp gain, NPN LED driver — are recomputed here (E12, the training data's rules) and the title follows.
/// Any other plan, or text that is not a plan, comes back unchanged. Core/src/CircuitValues.cpp.
std::string circuitPlanValues(const std::string& request, const std::string& planJson);

}  // namespace sieda
