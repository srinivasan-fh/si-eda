// SiEDA Core — C ABI of the built-in language model (LocalModel.hpp). Every entry point is exception-safe.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include "sieda/Json.hpp"
#include "sieda/LocalModel.hpp"
#include "sieda/sieda_c.h"

struct SiedaLlm {
    sieda::LocalModel model;
};

namespace {
char* dupLlm(const std::string& s) {
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (out) std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}
void setError(char** error_out, const std::string& message) {
    if (error_out) *error_out = dupLlm(message);
}
}  // namespace

extern "C" {

SiedaLlm* sieda_llm_open(const char* path, char** error_out) {
    try {
        auto* m = new SiedaLlm();
        std::string error;
        if (path && m->model.load(path, &error)) return m;
        delete m;
        setError(error_out, path ? error : "No model file given.");
    } catch (const std::exception& e) {
        setError(error_out, e.what());
    }
    return nullptr;
}

void sieda_llm_close(SiedaLlm* model) { delete model; }

char* sieda_llm_info_json(const SiedaLlm* model) { return dupLlm(model ? model->model.infoJson() : "{}"); }

char* sieda_llm_chat(SiedaLlm* model, const char* system, const char* user, const char* options_json,
                     int32_t (*on_text)(const char* piece, void* context), void* context, char** error_out) {
    if (!model || !user) {
        setError(error_out, "No model or prompt.");
        return nullptr;
    }
    try {
        sieda::LocalModel::Options o;
        if (options_json && *options_json) {
            const sieda::Json j = sieda::Json::parse(options_json);
            auto clamped = [&](const char* key, double fallback, double lo, double hi) {
                const double v = j.get(key).asNumber(fallback);
                return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
            };
            o.maxTokens = static_cast<int>(clamped("maxTokens", o.maxTokens, 1, 1e6));
            o.temperature = clamped("temperature", 0, 0, 5);
            o.topP = clamped("topP", 0.95, 0.01, 1);
            o.seed = static_cast<uint64_t>(clamped("seed", 1, 0, 1e15));
            o.threads = static_cast<int>(clamped("threads", 0, 0, 256));
            o.json = j.get("json").asBool(false);
        }
        const std::string prompt = model->model.chatPrompt(system ? system : "", user);
        return dupLlm(model->model.generate(prompt, o, [&](const std::string& piece) {
            return !on_text || on_text(piece.c_str(), context) != 0;
        }));
    } catch (const std::exception& e) {
        setError(error_out, e.what());
    }
    return nullptr;
}

void sieda_llm_set_accelerator(SiedaLlm* model, SiedaLlmMatmul fn, void* context) {
    if (!model) return;
    if (!fn) {
        model->model.setAccelerator({});
        return;
    }
    model->model.setAccelerator([fn, context](uint64_t offset, int type, int64_t cols, int64_t rows, size_t rowBytes,
                                              const float* x, int batch, float* y) {
        return fn(context, offset, type, cols, rows, rowBytes, x, batch, y) != 0;
    });
}

const void* sieda_llm_file(const SiedaLlm* model, uint64_t* size) {
    size_t n = 0;
    const void* data = model ? model->model.fileData(&n) : nullptr;
    if (size) *size = n;
    return data;
}

int32_t sieda_llm_cpu_matmul(const void* weights, int32_t type, int64_t cols, int64_t rows, uint64_t row_bytes,
                             const float* x, int32_t batch, float* y) {
    return sieda::LocalModel::cpuMatmul(static_cast<const uint8_t*>(weights), type, cols, rows, row_bytes, x, batch, y) ? 1 : 0;
}

char* sieda_circuit_plan_values(const char* request, const char* plan_json) {
    try {
        return dupLlm(sieda::circuitPlanValues(request ? request : "", plan_json ? plan_json : ""));
    } catch (const std::exception&) {
        return dupLlm(plan_json ? plan_json : "");
    }
}

}  // extern "C"
