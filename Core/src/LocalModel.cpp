// SiEDA Core — on-device language model (see LocalModel.hpp).
//
// GGUF v2 / v3 reader, block dequantisation of the common llama.cpp formats, a Llama-family transformer (RMSNorm,
// rotary positions — interleaved for Llama / Mistral, half-split for Qwen —, grouped-query attention with an f16 KV
// cache, SwiGLU feed-forward; Qwen 2 biases and Qwen 3 q / k norms), byte-level BPE (GPT-2 style: Llama 3, Qwen) and
// SentencePiece (Llama 2, Mistral) tokenizers, and greedy / top-p sampling. Matrix rows are split across a small
// thread pool; each row is computed by one thread, so the result does not depend on the thread count.
#include "sieda/LocalModel.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>

#include "sieda/Json.hpp"

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sieda {

namespace {

float half(uint16_t h) {
    const uint32_t sign = (h & 0x8000u) << 16, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
    uint32_t bits;
    if (exp == 0) {
        if (man == 0) {
            bits = sign;
        } else {  // subnormal
            int e = -1;
            uint32_t m = man;
            do { ++e; m <<= 1; } while ((m & 0x400) == 0);
            bits = sign | ((127 - 15 - e) << 23) | ((m & 0x3FF) << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (man << 13);
    } else {
        bits = sign | ((exp + 127 - 15) << 23) | (man << 13);
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

/// f16 → f32 through a table (the attention reads the f16 KV cache a lot).
const float* halfTable() {
    static const std::vector<float> table = [] {
        std::vector<float> t(65536);
        for (uint32_t i = 0; i < 65536; ++i) t[i] = half(static_cast<uint16_t>(i));
        return t;
    }();
    return table.data();
}

uint16_t toHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000;
    int exp = static_cast<int>((x >> 23) & 0xFF) - 127 + 15;
    uint32_t man = x & 0x7FFFFF;
    if (((x >> 23) & 0xFF) == 0xFF) return static_cast<uint16_t>(sign | 0x7C00 | (man ? 0x200 : 0));
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00);
    if (exp <= 0) {
        if (exp < -10) return static_cast<uint16_t>(sign);
        man |= 0x800000;
        const int shift = 14 - exp;
        uint32_t h = man >> shift;
        if ((man >> (shift - 1)) & 1) ++h;
        return static_cast<uint16_t>(sign | h);
    }
    uint32_t h = sign | (static_cast<uint32_t>(exp) << 10) | (man >> 13);
    if (man & 0x1000) ++h;  // round half up (ties are rare enough for a cache)
    return static_cast<uint16_t>(h);
}

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

// ---- tensor formats ----------------------------------------------------------------------------------------------

struct Format {
    int block, bytes;
};

Format formatOf(int type) {
    switch (type) {
    case 0: return {1, 4};       // F32
    case 1: return {1, 2};       // F16
    case 30: return {1, 2};      // BF16
    case 2: return {32, 18};     // Q4_0
    case 3: return {32, 20};     // Q4_1
    case 6: return {32, 22};     // Q5_0
    case 7: return {32, 24};     // Q5_1
    case 8: return {32, 34};     // Q8_0
    case 12: return {256, 144};  // Q4_K
    case 13: return {256, 176};  // Q5_K
    case 14: return {256, 210};  // Q6_K
    default: return {0, 0};
    }
}

const char* formatName(int type) {
    switch (type) {
    case 0: return "F32";
    case 1: return "F16";
    case 30: return "BF16";
    case 2: return "Q4_0";
    case 3: return "Q4_1";
    case 6: return "Q5_0";
    case 7: return "Q5_1";
    case 8: return "Q8_0";
    case 12: return "Q4_K";
    case 13: return "Q5_K";
    case 14: return "Q6_K";
    default: return "?";
    }
}

void scaleMin(int j, const uint8_t* q, uint8_t* d, uint8_t* m) {
    if (j < 4) {
        *d = q[j] & 63;
        *m = q[j + 4] & 63;
    } else {
        *d = static_cast<uint8_t>((q[j + 4] & 0xF) | ((q[j - 4] >> 6) << 4));
        *m = static_cast<uint8_t>((q[j + 4] >> 4) | ((q[j] >> 6) << 4));
    }
}

/// Expands `n` values (a whole number of blocks) of format `type` at `p` into `y`.
void dequantize(int type, const uint8_t* p, int64_t n, float* y) {
    switch (type) {
    case 0: std::memcpy(y, p, static_cast<size_t>(n) * 4); return;
    case 1: for (int64_t i = 0; i < n; ++i) y[i] = half(rd16(p + 2 * i)); return;
    case 30:
        for (int64_t i = 0; i < n; ++i) {
            const uint32_t bits = static_cast<uint32_t>(rd16(p + 2 * i)) << 16;
            std::memcpy(y + i, &bits, 4);
        }
        return;
    default: break;
    }
    const Format f = formatOf(type);
    for (int64_t b = 0; b < n / f.block; ++b, p += f.bytes, y += f.block) {
        switch (type) {
        case 2: case 3: {  // Q4_0, Q4_1
            const float d = half(rd16(p)), m = type == 3 ? half(rd16(p + 2)) : -8 * d;
            const uint8_t* qs = p + (type == 3 ? 4 : 2);
            for (int j = 0; j < 16; ++j) {
                y[j] = (qs[j] & 0xF) * d + m;
                y[j + 16] = (qs[j] >> 4) * d + m;
            }
            break;
        }
        case 6: case 7: {  // Q5_0, Q5_1
            const float d = half(rd16(p)), m = type == 7 ? half(rd16(p + 2)) : -16 * d;
            const uint8_t* qh8 = p + (type == 7 ? 4 : 2);
            const uint32_t qh = static_cast<uint32_t>(qh8[0] | (qh8[1] << 8) | (qh8[2] << 16)) | (static_cast<uint32_t>(qh8[3]) << 24);
            const uint8_t* qs = qh8 + 4;
            for (int j = 0; j < 16; ++j) {
                const int h0 = ((qh >> j) << 4) & 0x10, h1 = (qh >> (j + 12)) & 0x10;
                y[j] = ((qs[j] & 0xF) | h0) * d + m;
                y[j + 16] = ((qs[j] >> 4) | h1) * d + m;
            }
            break;
        }
        case 8: {  // Q8_0
            const float d = half(rd16(p));
            for (int j = 0; j < 32; ++j) y[j] = static_cast<int8_t>(p[2 + j]) * d;
            break;
        }
        case 12: case 13: {  // Q4_K, Q5_K
            const float d = half(rd16(p)), dmin = half(rd16(p + 2));
            const uint8_t* sc = p + 4;
            const uint8_t* qh = p + 16;
            const uint8_t* q = p + (type == 13 ? 48 : 16);
            float* out = y;
            uint8_t u1 = 1, u2 = 2;
            for (int j = 0, is = 0; j < 256; j += 64, is += 2, q += 32, u1 <<= 2, u2 <<= 2) {
                uint8_t s, m;
                scaleMin(is, sc, &s, &m);
                const float d1 = d * s, m1 = dmin * m;
                scaleMin(is + 1, sc, &s, &m);
                const float d2 = d * s, m2 = dmin * m;
                for (int l = 0; l < 32; ++l) *out++ = d1 * ((q[l] & 0xF) + (type == 13 && (qh[l] & u1) ? 16 : 0)) - m1;
                for (int l = 0; l < 32; ++l) *out++ = d2 * ((q[l] >> 4) + (type == 13 && (qh[l] & u2) ? 16 : 0)) - m2;
            }
            break;
        }
        case 14: {  // Q6_K
            const uint8_t* ql = p;
            const uint8_t* qh = p + 128;
            const int8_t* sc = reinterpret_cast<const int8_t*>(p + 192);
            const float d = half(rd16(p + 208));
            float* out = y;
            for (int n0 = 0; n0 < 256; n0 += 128, ql += 64, qh += 32, sc += 8, out += 128) {
                for (int l = 0; l < 32; ++l) {
                    const int is = l / 16;
                    out[l] = d * sc[is] * (((ql[l] & 0xF) | (((qh[l] >> 0) & 3) << 4)) - 32);
                    out[l + 32] = d * sc[is + 2] * (((ql[l + 32] & 0xF) | (((qh[l] >> 2) & 3) << 4)) - 32);
                    out[l + 64] = d * sc[is + 4] * (((ql[l] >> 4) | (((qh[l] >> 4) & 3) << 4)) - 32);
                    out[l + 96] = d * sc[is + 6] * (((ql[l + 32] >> 4) | (((qh[l] >> 6) & 3) << 4)) - 32);
                }
            }
            break;
        }
        default: break;
        }
    }
}

float dot(const float* a, const float* b, int64_t n) {
    float s[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int64_t i = 0;
    for (; i + 8 <= n; i += 8)
        for (int k = 0; k < 8; ++k) s[k] += a[i + k] * b[i + k];
    float t = ((s[0] + s[1]) + (s[2] + s[3])) + ((s[4] + s[5]) + (s[6] + s[7]));
    for (; i < n; ++i) t += a[i] * b[i];
    return t;
}

// ---- thread pool -------------------------------------------------------------------------------------------------

class Pool {
public:
    explicit Pool(int n) : n_(std::max(1, n)) {
        for (int k = 1; k < n_; ++k) threads_.emplace_back([this, k] { work(k); });
    }
    ~Pool() {
        {
            std::lock_guard<std::mutex> l(m_);
            stop_ = true;
        }
        wake_.notify_all();
        for (auto& t : threads_) t.join();
    }
    int size() const { return n_; }
    /// Runs fn(begin, end) over [0, count) split into one slice per thread.
    void run(int64_t count, const std::function<void(int64_t, int64_t)>& fn) {
        if (n_ == 1 || count < 2) {
            fn(0, count);
            return;
        }
        {
            std::lock_guard<std::mutex> l(m_);
            fn_ = &fn;
            count_ = count;
            pending_ = n_ - 1;
            ++generation_;
        }
        wake_.notify_all();
        fn(0, count / n_);
        std::unique_lock<std::mutex> l(m_);
        done_.wait(l, [this] { return pending_ == 0; });
        fn_ = nullptr;
    }

private:
    void work(int k) {
        uint64_t seen = 0;
        for (;;) {
            std::unique_lock<std::mutex> l(m_);
            wake_.wait(l, [&] { return stop_ || generation_ != seen; });
            if (stop_) return;
            seen = generation_;
            const auto* fn = fn_;
            const int64_t count = count_;
            l.unlock();
            (*fn)(count * k / n_, count * (k + 1) / n_);
            l.lock();
            if (--pending_ == 0) done_.notify_one();
        }
    }
    int n_;
    std::vector<std::thread> threads_;
    std::mutex m_;
    std::condition_variable wake_, done_;
    const std::function<void(int64_t, int64_t)>* fn_ = nullptr;
    int64_t count_ = 0;
    int pending_ = 0;
    uint64_t generation_ = 0;
    bool stop_ = false;
};

// ---- UTF-8 -------------------------------------------------------------------------------------------------------

std::vector<uint32_t> decodeUtf8(const std::string& s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        uint32_t cp = len == 1 ? c : c & (0x7F >> len);
        for (int k = 1; k < len && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        out.push_back(cp);
        i += static_cast<size_t>(len);
    }
    return out;
}

std::string encodeUtf8(uint32_t cp) {
    std::string s;
    if (cp < 0x80) {
        s += static_cast<char>(cp);
    } else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        s += static_cast<char>(0xF0 | (cp >> 18));
        s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return s;
}

/// Bytes of `s` that end on a whole UTF-8 character (the rest waits for the next piece).
size_t completeUtf8(const std::string& s) {
    size_t i = s.size();
    for (int back = 0; back < 4 && i > 0; ++back) {
        const unsigned char c = static_cast<unsigned char>(s[i - 1]);
        if ((c & 0xC0) == 0x80) { --i; continue; }
        const size_t need = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        return s.size() - (i - 1) >= need ? s.size() : i - 1;
    }
    return s.size();
}

bool isSpace(uint32_t c) {
    return c == ' ' || (c >= 9 && c <= 13) || c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
           c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}
bool isNumber(uint32_t c) {
    return (c >= '0' && c <= '9') || (c >= 0x660 && c <= 0x669) || (c >= 0x966 && c <= 0x96F) || (c >= 0xFF10 && c <= 0xFF19);
}
bool isLetter(uint32_t c) {
    if (c < 0x80) return (c | 0x20) >= 'a' && (c | 0x20) <= 'z';
    if (isSpace(c) || isNumber(c)) return false;
    // Punctuation and symbol blocks (an approximation of \p{L}: other scripts count as letters).
    return !((c >= 0xA1 && c <= 0xBF && c != 0xAA && c != 0xB5 && c != 0xBA) || c == 0xD7 || c == 0xF7 ||
             (c >= 0x2010 && c <= 0x2BFF) || (c >= 0x3001 && c <= 0x303F) || (c >= 0xFF01 && c <= 0xFF0F) ||
             (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0x1F000 && c <= 0x1FAFF));
}

// ---- GGUF metadata -----------------------------------------------------------------------------------------------

struct Value {
    double num = 0;
    std::string str;
    std::vector<std::string> strs;
    std::vector<double> nums;
};

struct Reader {
    const uint8_t* p;
    size_t n, at = 0;
    bool ok = true;
    template <class T>
    T get() {
        T v{};
        if (!ok || n - at < sizeof(T)) {
            ok = false;
            return v;
        }
        std::memcpy(&v, p + at, sizeof(T));
        at += sizeof(T);
        return v;
    }
    std::string str() {
        const uint64_t len = get<uint64_t>();
        if (!ok || len > n - at) {
            ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(p + at), static_cast<size_t>(len));
        at += static_cast<size_t>(len);
        return s;
    }
    double number(uint32_t type) {
        switch (type) {
        case 0: return get<uint8_t>();
        case 1: return get<int8_t>();
        case 2: return get<uint16_t>();
        case 3: return get<int16_t>();
        case 4: return get<uint32_t>();
        case 5: return get<int32_t>();
        case 6: return get<float>();
        case 7: return get<uint8_t>() != 0;
        case 10: return static_cast<double>(get<uint64_t>());
        case 11: return static_cast<double>(get<int64_t>());
        case 12: return get<double>();
        default: ok = false; return 0;
        }
    }
    void value(uint32_t type, Value& v, int depth) {
        if (type == 8) {
            v.str = str();
        } else if (type == 9) {
            const uint32_t elem = get<uint32_t>();
            const uint64_t count = get<uint64_t>();
            if (!ok || count > n - at || depth > 2) {
                ok = false;
                return;
            }
            for (uint64_t i = 0; i < count && ok; ++i) {
                if (elem == 8) v.strs.push_back(str());
                else if (elem == 9) { Value skip; value(9, skip, depth + 1); }
                else v.nums.push_back(number(elem));
            }
        } else {
            v.num = number(type);
        }
    }
};

struct Tensor {
    const uint8_t* data = nullptr;
    int type = 0;
    int64_t ne0 = 0, ne1 = 0;
    size_t rowBytes = 0;
    bool present() const { return data != nullptr; }
};

struct Layer {
    Tensor attnNorm, wq, wk, wv, wo, bq, bk, bv, qNorm, kNorm, ffnNorm, gate, up, down;
};

const std::string kSpm = "\xE2\x96\x81";  // ▁

}  // namespace

// ---- the model ---------------------------------------------------------------------------------------------------

struct LocalModel::Impl {
    std::string owned;  // bytes when loaded from memory (or read without mmap)
    const uint8_t* base = nullptr;
    size_t size = 0;
    void* mapped = nullptr;
    std::map<std::string, Value> meta;
    std::map<std::string, Tensor> tensors;

    std::string arch, name;
    int nEmbd = 0, nLayer = 0, nHead = 0, nHeadKv = 0, nFf = 0, headDim = 0, ropeDim = 0, trainCtx = 0;
    float eps = 1e-5f, ropeBase = 10000;
    bool neox = false;
    std::vector<float> ropeFactors;
    Tensor tokEmbd, outNorm, output, ropeFreqs;
    std::vector<Layer> layers;

    // tokenizer
    bool bpe = true;
    int digits = 3;  // \p{N}{1,digits}; 0 = the GPT-2 pattern
    std::vector<std::string> vocab;
    std::vector<float> scores;
    std::vector<int> types;
    std::unordered_map<std::string, int> ids;
    std::unordered_map<std::string, int> ranks;
    std::vector<int> specials;  // control / user-defined tokens, longest first
    std::string byteChar[256];
    std::unordered_map<uint32_t, int> charByte;
    int bos = -1, eos = -1;
    bool addBos = false, addSpace = true;
    std::vector<int> stops;

    // runtime
    LocalModel::Accelerator accelerator;
    std::unique_ptr<Pool> pool;
    std::vector<uint16_t> kCache, vCache;
    int ctx = 0;

    ~Impl() {
#if !defined(_WIN32)
        if (mapped) munmap(mapped, size);
#endif
    }

    const Value* find(const std::string& key) const {
        const auto it = meta.find(key);
        return it == meta.end() ? nullptr : &it->second;
    }
    double num(const std::string& key, double fallback) const {
        const Value* v = find(key);
        return v && std::isfinite(v->num) ? v->num : fallback;
    }
    /// An integer setting clamped to a safe range (a damaged file may hold anything).
    int count(const std::string& key, int fallback) const { return static_cast<int>(std::clamp(num(key, fallback), -1.0, 1e9)); }
    Tensor tensor(const std::string& key) const {
        const auto it = tensors.find(key);
        return it == tensors.end() ? Tensor{} : it->second;
    }

    bool parse(std::string* error) {
        auto fail = [&](const std::string& why) {
            if (error) *error = why;
            return false;
        };
        Reader r{base, size};
        if (size < 24 || std::memcmp(base, "GGUF", 4) != 0) return fail("Not a GGUF model file.");
        r.at = 4;
        const uint32_t version = r.get<uint32_t>();
        if (version < 2 || version > 3) return fail("Unsupported GGUF version " + std::to_string(version) + ".");
        const uint64_t nTensors = r.get<uint64_t>(), nKv = r.get<uint64_t>();
        if (nTensors > 100000 || nKv > 100000) return fail("The model file is damaged.");
        for (uint64_t i = 0; i < nKv && r.ok; ++i) {
            std::string key = r.str();
            const uint32_t type = r.get<uint32_t>();
            Value v;
            r.value(type, v, 0);
            meta[std::move(key)] = std::move(v);
        }
        struct Info {
            std::string name;
            int type;
            int64_t ne[4];
            uint64_t offset;
        };
        std::vector<Info> infos;
        for (uint64_t i = 0; i < nTensors && r.ok; ++i) {
            Info t{r.str(), 0, {1, 1, 1, 1}, 0};
            const uint32_t dims = r.get<uint32_t>();
            if (dims > 4) return fail("The model file is damaged.");
            for (uint32_t d = 0; d < dims; ++d) t.ne[d] = static_cast<int64_t>(r.get<uint64_t>());
            t.type = static_cast<int>(r.get<uint32_t>());
            t.offset = r.get<uint64_t>();
            infos.push_back(std::move(t));
        }
        if (!r.ok) return fail("The model file is cut short or damaged.");
        const uint64_t align = static_cast<uint64_t>(count("general.alignment", 32));
        if (align == 0 || align > 4096) return fail("The model file is damaged.");
        const size_t data = static_cast<size_t>((r.at + align - 1) / align * align);
        for (const Info& t : infos) {
            const Format f = formatOf(t.type);
            if (f.block == 0) return fail(std::string("Tensor ") + t.name + " uses a format this engine does not run (type " +
                                          std::to_string(t.type) + "); try a Q4_K_M, Q5_K_M, Q8_0 or F16 file.");
            if (t.ne[0] <= 0 || t.ne[0] % f.block != 0 || t.ne[1] <= 0 || t.ne[2] <= 0 || t.ne[3] <= 0)
                return fail("Tensor " + t.name + " has an unexpected shape.");
            Tensor x;
            x.type = t.type;
            x.ne0 = t.ne[0];
            x.ne1 = t.ne[1] * t.ne[2] * t.ne[3];
            x.rowBytes = static_cast<size_t>(t.ne[0] / f.block * f.bytes);
            const double bytes = static_cast<double>(x.rowBytes) * static_cast<double>(x.ne1);
            if (data > size || t.offset > size - data || bytes > static_cast<double>(size - data - t.offset))
                return fail("The model file is cut short (tensor " + t.name + ").");
            x.data = base + data + t.offset;
            tensors[t.name] = x;
        }
        return setup(error);
    }

    bool setup(std::string* error) {
        auto fail = [&](const std::string& why) {
            if (error) *error = why;
            return false;
        };
        const Value* a = find("general.architecture");
        arch = a ? a->str : "";
        if (const Value* n = find("general.name")) name = n->str;
        if (arch != "llama" && arch != "qwen2" && arch != "qwen3" && arch != "mistral")
            return fail("Model architecture '" + arch + "' is not supported (Llama, Mistral, Qwen 2 / 2.5 / 3 are).");
        neox = arch == "qwen2" || arch == "qwen3";
        const std::string p = arch + ".";
        nEmbd = count(p + "embedding_length", 0);
        nLayer = count(p + "block_count", 0);
        nHead = count(p + "attention.head_count", 0);
        nHeadKv = count(p + "attention.head_count_kv", nHead);
        nFf = count(p + "feed_forward_length", 0);
        trainCtx = count(p + "context_length", 4096);
        eps = static_cast<float>(num(p + "attention.layer_norm_rms_epsilon", 1e-5));
        ropeBase = static_cast<float>(num(p + "rope.freq_base", 10000));
        if (nEmbd <= 0 || nLayer <= 0 || nLayer > 512 || nHead <= 0 || nHeadKv <= 0 || nHead % nHeadKv != 0 || nFf <= 0)
            return fail("The model's hyper-parameters are missing or invalid.");
        headDim = count(p + "attention.key_length", nEmbd / nHead);
        ropeDim = count(p + "rope.dimension_count", headDim);
        if (headDim <= 0 || headDim % 2 || ropeDim > headDim || ropeDim % 2) return fail("Unsupported attention shape.");
        trainCtx = std::max(16, std::min(trainCtx, 131072));
        tokEmbd = tensor("token_embd.weight");
        outNorm = tensor("output_norm.weight");
        output = tensor("output.weight");
        if (!output.present()) output = tokEmbd;  // tied embeddings
        ropeFreqs = tensor("rope_freqs.weight");
        auto need = [&](const Tensor& t, int64_t ne0, int64_t ne1) { return t.present() && t.ne0 == ne0 && t.ne1 == ne1; };
        const int qDim = nHead * headDim, kvDim = nHeadKv * headDim;
        if (!tokEmbd.present() || !need(outNorm, nEmbd, 1) || output.ne0 != nEmbd)
            return fail("The model is missing its embedding or output tensors.");
        for (int l = 0; l < nLayer; ++l) {
            const std::string b = "blk." + std::to_string(l) + ".";
            Layer L;
            L.attnNorm = tensor(b + "attn_norm.weight");
            L.wq = tensor(b + "attn_q.weight");
            L.wk = tensor(b + "attn_k.weight");
            L.wv = tensor(b + "attn_v.weight");
            L.wo = tensor(b + "attn_output.weight");
            L.bq = tensor(b + "attn_q.bias");
            L.bk = tensor(b + "attn_k.bias");
            L.bv = tensor(b + "attn_v.bias");
            L.qNorm = tensor(b + "attn_q_norm.weight");
            L.kNorm = tensor(b + "attn_k_norm.weight");
            L.ffnNorm = tensor(b + "ffn_norm.weight");
            L.gate = tensor(b + "ffn_gate.weight");
            L.up = tensor(b + "ffn_up.weight");
            L.down = tensor(b + "ffn_down.weight");
            const bool ok = need(L.attnNorm, nEmbd, 1) && need(L.wq, nEmbd, qDim) && need(L.wk, nEmbd, kvDim) &&
                            need(L.wv, nEmbd, kvDim) && need(L.wo, qDim, nEmbd) && need(L.ffnNorm, nEmbd, 1) &&
                            need(L.gate, nEmbd, nFf) && need(L.up, nEmbd, nFf) && need(L.down, nFf, nEmbd) &&
                            (!L.bq.present() || need(L.bq, qDim, 1)) && (!L.bk.present() || need(L.bk, kvDim, 1)) &&
                            (!L.bv.present() || need(L.bv, kvDim, 1)) && (!L.qNorm.present() || need(L.qNorm, headDim, 1)) &&
                            (!L.kNorm.present() || need(L.kNorm, headDim, 1));
            if (!ok) return fail("Layer " + std::to_string(l) + " of the model is missing tensors or has unexpected shapes.");
            layers.push_back(L);
        }
        if (ropeFreqs.present()) {
            if (ropeFreqs.ne0 != ropeDim / 2) return fail("Unexpected rope_freqs tensor.");
            ropeFactors.resize(static_cast<size_t>(ropeDim / 2));
            dequantize(ropeFreqs.type, ropeFreqs.data, ropeDim / 2, ropeFactors.data());
        }
        return setupTokenizer(fail);
    }

    template <class Fail>
    bool setupTokenizer(Fail fail) {
        const Value* model = find("tokenizer.ggml.model");
        const Value* toks = find("tokenizer.ggml.tokens");
        if (!model || !toks || toks->strs.empty()) return fail("The model has no tokenizer.");
        if (model->str == "gpt2") bpe = true;
        else if (model->str == "llama") bpe = false;
        else return fail("Tokenizer '" + model->str + "' is not supported (GPT-2 BPE and SentencePiece are).");
        vocab = toks->strs;
        if (static_cast<int64_t>(vocab.size()) != output.ne1 || tokEmbd.ne1 != output.ne1)
            return fail("The tokenizer and the model disagree on the vocabulary size.");
        if (const Value* s = find("tokenizer.ggml.scores")) scores.assign(s->nums.begin(), s->nums.end());
        if (const Value* t = find("tokenizer.ggml.token_type")) types.assign(t->nums.begin(), t->nums.end());
        types.resize(vocab.size(), 1);
        scores.resize(vocab.size(), 0);
        for (size_t i = 0; i < vocab.size(); ++i) {
            ids.emplace(vocab[i], static_cast<int>(i));
            if (types[i] == 3 || types[i] == 4) specials.push_back(static_cast<int>(i));
        }
        std::sort(specials.begin(), specials.end(), [&](int x, int y) { return vocab[static_cast<size_t>(x)].size() > vocab[static_cast<size_t>(y)].size(); });
        if (const Value* m = find("tokenizer.ggml.merges"))
            for (size_t i = 0; i < m->strs.size(); ++i) ranks.emplace(m->strs[i], static_cast<int>(i));
        const int nv = static_cast<int>(vocab.size());
        auto idOf = [&](const char* key, int fallback) {
            const int v = count(key, fallback);
            return v >= 0 && v < nv ? v : -1;
        };
        bos = idOf("tokenizer.ggml.bos_token_id", -1);
        eos = idOf("tokenizer.ggml.eos_token_id", -1);
        addBos = num("tokenizer.ggml.add_bos_token", bpe ? 0 : 1) != 0 && bos >= 0;
        addSpace = num("tokenizer.ggml.add_space_prefix", bpe ? 0 : 1) != 0;
        const Value* pre = find("tokenizer.ggml.pre");
        const std::string preName = pre ? pre->str : "default";
        digits = preName == "qwen2" ? 1 : (preName == "default" || preName == "gpt2") ? 0 : 3;
        if (eos >= 0) stops.push_back(eos);
        for (const char* s : {"<|im_end|>", "<|eot_id|>", "<|endoftext|>", "<|end_of_text|>", "</s>", "<|end|>"}) {
            const auto it = ids.find(s);
            if (it != ids.end() && std::find(stops.begin(), stops.end(), it->second) == stops.end()) stops.push_back(it->second);
        }
        // GPT-2 byte <-> printable character map.
        std::vector<int> printable;
        for (int b = '!'; b <= '~'; ++b) printable.push_back(b);
        for (int b = 0xA1; b <= 0xAC; ++b) printable.push_back(b);
        for (int b = 0xAE; b <= 0xFF; ++b) printable.push_back(b);
        int extra = 0;
        for (int b = 0; b < 256; ++b) {
            const bool direct = std::find(printable.begin(), printable.end(), b) != printable.end();
            const uint32_t cp = direct ? static_cast<uint32_t>(b) : static_cast<uint32_t>(256 + extra++);
            byteChar[b] = encodeUtf8(cp);
            charByte[cp] = b;
        }
        return true;
    }

    // ---- tokenizer ----

    /// End of the pre-token starting at i (the Llama 3 / Qwen 2 split pattern, or GPT-2's when digits == 0).
    size_t pretoken(const std::vector<uint32_t>& c, size_t i) const {
        const size_t n = c.size();
        auto L = [&](size_t k) { return k < n && isLetter(c[k]); };
        auto N = [&](size_t k) { return k < n && isNumber(c[k]); };
        auto S = [&](size_t k) { return k < n && isSpace(c[k]); };
        auto P = [&](size_t k) { return k < n && !isSpace(c[k]) && !isLetter(c[k]) && !isNumber(c[k]); };
        auto lower = [&](size_t k) { return k < n ? (digits ? (c[k] | 0x20) : c[k]) : 0u; };
        if (c[i] == '\'') {
            const uint32_t a = lower(i + 1), b = lower(i + 2);
            if ((a == 'r' && b == 'e') || (a == 'v' && b == 'e') || (a == 'l' && b == 'l')) return i + 3;
            if (a == 's' || a == 't' || a == 'm' || a == 'd') return i + 2;
        }
        size_t k = i;
        if (digits) {
            if ((L(i) || (c[i] != '\r' && c[i] != '\n' && !isLetter(c[i]) && !isNumber(c[i]) && L(i + 1)))) {
                k = L(i) ? i : i + 1;
                while (L(k)) ++k;
                return k;
            }
            if (N(i)) {
                while (N(k) && k - i < static_cast<size_t>(digits)) ++k;
                return k;
            }
            k = c[i] == ' ' ? i + 1 : i;
            if (P(k)) {
                while (P(k)) ++k;
                while (k < n && (c[k] == '\r' || c[k] == '\n')) ++k;
                return k;
            }
            size_t e = i, lastNl = n;
            while (S(e)) {
                if (c[e] == '\r' || c[e] == '\n') lastNl = e;
                ++e;
            }
            if (lastNl != n) return lastNl + 1;
        } else {
            k = c[i] == ' ' ? i + 1 : i;
            if (L(k)) { while (L(k)) ++k; return k; }
            if (N(k)) { while (N(k)) ++k; return k; }
            if (P(k)) { while (P(k)) ++k; return k; }
        }
        size_t e = i;
        while (S(e)) ++e;
        if (e == i) return i + 1;
        if (e < n && e - i > 1) return e - 1;  // \s+(?!\S): leave one space for the next word
        return e;
    }

    void bpeWord(const std::string& word, std::vector<int>& out) const {
        std::vector<std::string> sym;
        for (unsigned char ch : word) sym.push_back(byteChar[ch]);
        while (sym.size() > 1) {
            int best = -1, bestRank = 0;
            for (size_t i = 0; i + 1 < sym.size(); ++i) {
                const auto it = ranks.find(sym[i] + " " + sym[i + 1]);
                if (it != ranks.end() && (best < 0 || it->second < bestRank)) {
                    best = static_cast<int>(i);
                    bestRank = it->second;
                }
            }
            if (best < 0) break;
            sym[static_cast<size_t>(best)] += sym[static_cast<size_t>(best) + 1];
            sym.erase(sym.begin() + best + 1);
        }
        for (const std::string& s : sym) {
            const auto it = ids.find(s);
            if (it != ids.end()) {
                out.push_back(it->second);
            } else {  // not in the vocabulary: one token per byte character
                for (uint32_t cp : decodeUtf8(s)) {
                    const auto b = ids.find(encodeUtf8(cp));
                    if (b != ids.end()) out.push_back(b->second);
                }
            }
        }
    }

    void spmText(const std::string& text, std::vector<int>& out, bool first) const {
        std::string s = first && addSpace ? " " + text : text;
        std::string t;
        for (char ch : s) t += ch == ' ' ? kSpm : std::string(1, ch);
        // Words start at each ▁; merges run inside a word (highest-scoring pair first).
        std::vector<std::string> words;
        for (size_t i = 0; i < t.size();) {
            size_t j = t.find(kSpm, i + (t.compare(i, 3, kSpm) == 0 ? 3 : 0));
            if (j == std::string::npos) j = t.size();
            words.push_back(t.substr(i, j - i));
            i = j;
        }
        for (const std::string& w : words) {
            std::vector<std::string> sym;
            for (uint32_t cp : decodeUtf8(w)) sym.push_back(encodeUtf8(cp));
            while (sym.size() > 1) {
                int best = -1;
                float bestScore = 0;
                for (size_t i = 0; i + 1 < sym.size(); ++i) {
                    const auto it = ids.find(sym[i] + sym[i + 1]);
                    if (it != ids.end() && (best < 0 || scores[static_cast<size_t>(it->second)] > bestScore)) {
                        best = static_cast<int>(i);
                        bestScore = scores[static_cast<size_t>(it->second)];
                    }
                }
                if (best < 0) break;
                sym[static_cast<size_t>(best)] += sym[static_cast<size_t>(best) + 1];
                sym.erase(sym.begin() + best + 1);
            }
            for (const std::string& x : sym) {
                const auto it = ids.find(x);
                if (it != ids.end()) {
                    out.push_back(it->second);
                    continue;
                }
                for (unsigned char b : x) {  // byte fallback
                    char hex[8];
                    std::snprintf(hex, sizeof hex, "<0x%02X>", b);
                    const auto h = ids.find(hex);
                    if (h != ids.end()) out.push_back(h->second);
                }
            }
        }
    }

    std::vector<int> tokenize(const std::string& text, bool special) const {
        std::vector<int> out;
        bool first = true;
        auto plain = [&](const std::string& s) {
            if (s.empty()) return;
            if (!bpe) {
                spmText(s, out, first);
            } else {
                const auto c = decodeUtf8(s);
                for (size_t i = 0; i < c.size();) {
                    const size_t j = std::max(i + 1, pretoken(c, i));
                    std::string word;
                    for (size_t k = i; k < j; ++k) word += encodeUtf8(c[k]);
                    bpeWord(word, out);
                    i = j;
                }
            }
            first = false;
        };
        size_t from = 0;
        for (size_t i = 0; special && i < text.size(); ++i) {
            if (text[i] != '<' && text[i] != '[') continue;  // control tokens look like <|…|>, <s>, [INST]
            for (int id : specials) {
                const std::string& tok = vocab[static_cast<size_t>(id)];
                if (!tok.empty() && text.compare(i, tok.size(), tok) == 0) {
                    plain(text.substr(from, i - from));
                    out.push_back(id);
                    first = false;
                    from = i + tok.size();
                    i = from - 1;
                    break;
                }
            }
        }
        plain(text.substr(from));
        return out;
    }

    std::string piece(int id) const {
        if (id < 0 || id >= static_cast<int>(vocab.size()) || types[static_cast<size_t>(id)] == 3) return {};
        const std::string& t = vocab[static_cast<size_t>(id)];
        if (types[static_cast<size_t>(id)] == 4) return t;
        if (bpe) {
            std::string s;
            for (uint32_t cp : decodeUtf8(t)) {
                const auto it = charByte.find(cp);
                if (it != charByte.end()) s += static_cast<char>(it->second);
                else s += encodeUtf8(cp);
            }
            return s;
        }
        if (types[static_cast<size_t>(id)] == 6 && t.size() == 6) return std::string(1, static_cast<char>(std::strtol(t.substr(3, 2).c_str(), nullptr, 16)));
        std::string s;
        for (size_t i = 0; i < t.size();) {
            if (t.compare(i, 3, kSpm) == 0) { s += ' '; i += 3; }
            else s += t[i++];
        }
        return s;
    }

    // ---- inference ----

    void matmul(const Tensor& w, const float* x, int batch, float* y, int64_t rows = -1) {
        if (rows < 0) rows = w.ne1;
        const int64_t n = w.ne0;
        if (accelerator && accelerator(static_cast<uint64_t>(w.data - base), w.type, n, rows, w.rowBytes, x, batch, y)) return;
        pool->run(rows, [&](int64_t r0, int64_t r1) {
            std::vector<float> row(static_cast<size_t>(w.type == 0 ? 0 : n));
            for (int64_t r = r0; r < r1; ++r) {
                const uint8_t* src = w.data + static_cast<size_t>(r) * w.rowBytes;
                const float* wr = reinterpret_cast<const float*>(src);
                if (w.type != 0) {
                    dequantize(w.type, src, n, row.data());
                    wr = row.data();
                }
                for (int b = 0; b < batch; ++b)
                    y[static_cast<size_t>(b) * static_cast<size_t>(rows) + static_cast<size_t>(r)] = dot(wr, x + static_cast<size_t>(b) * static_cast<size_t>(n), n);
            }
        });
    }

    std::vector<float> vec(const Tensor& t) const {
        std::vector<float> v(static_cast<size_t>(t.ne0));
        if (t.present()) dequantize(t.type, t.data, t.ne0, v.data());
        return v;
    }

    void rmsnorm(float* out, const float* x, const std::vector<float>& w, int n) const {
        double ss = 0;
        for (int i = 0; i < n; ++i) ss += static_cast<double>(x[i]) * x[i];
        const float scale = static_cast<float>(1.0 / std::sqrt(ss / n + eps));
        for (int i = 0; i < n; ++i) out[i] = x[i] * scale * w[static_cast<size_t>(i)];
    }

    void rope(float* v, int pos) const {
        for (int i = 0; i < ropeDim / 2; ++i) {
            double theta = pos * std::pow(static_cast<double>(ropeBase), -2.0 * i / ropeDim);
            if (!ropeFactors.empty()) theta /= ropeFactors[static_cast<size_t>(i)];
            const float c = static_cast<float>(std::cos(theta)), s = static_cast<float>(std::sin(theta));
            float& a = v[neox ? i : 2 * i];
            float& b = v[neox ? i + ropeDim / 2 : 2 * i + 1];
            const float x0 = a, x1 = b;
            a = x0 * c - x1 * s;
            b = x0 * s + x1 * c;
        }
    }

    void reserve(int tokens) {
        if (tokens <= ctx) return;
        ctx = std::min(trainCtx, std::max(tokens, 256));
        const size_t n = static_cast<size_t>(nLayer) * static_cast<size_t>(ctx) * static_cast<size_t>(nHeadKv * headDim);
        kCache.assign(n, 0);
        vCache.assign(n, 0);
    }

    /// Runs `tokens` at positions pos0… through the network; returns the logits of the last one.
    std::vector<float> forward(const std::vector<int>& tokens, int pos0) {
        const int B = static_cast<int>(tokens.size()), E = nEmbd, qDim = nHead * headDim, kvDim = nHeadKv * headDim;
        if (pos0 + B > ctx) throw std::runtime_error("The conversation is longer than the model's context.");
        std::vector<float> x(static_cast<size_t>(B * E)), xb(x.size()), q(static_cast<size_t>(B * qDim)),
            k(static_cast<size_t>(B * kvDim)), v(k.size()), att(q.size()), g(static_cast<size_t>(B) * nFf), u(g.size()), tmp(x.size());
        for (int b = 0; b < B; ++b) {
            const int t = std::clamp(tokens[static_cast<size_t>(b)], 0, static_cast<int>(tokEmbd.ne1) - 1);
            dequantize(tokEmbd.type, tokEmbd.data + static_cast<size_t>(t) * tokEmbd.rowBytes, E, &x[static_cast<size_t>(b * E)]);
        }
        const size_t layerStride = static_cast<size_t>(ctx) * static_cast<size_t>(kvDim);
        const float scale = 1.0f / std::sqrt(static_cast<float>(headDim));
        for (int l = 0; l < nLayer; ++l) {
            const Layer& L = layers[static_cast<size_t>(l)];
            const auto an = vec(L.attnNorm), fn = vec(L.ffnNorm);
            for (int b = 0; b < B; ++b) rmsnorm(&xb[static_cast<size_t>(b * E)], &x[static_cast<size_t>(b * E)], an, E);
            matmul(L.wq, xb.data(), B, q.data());
            matmul(L.wk, xb.data(), B, k.data());
            matmul(L.wv, xb.data(), B, v.data());
            const auto bq = vec(L.bq), bk = vec(L.bk), bv = vec(L.bv), qn = vec(L.qNorm), kn = vec(L.kNorm);
            uint16_t* kc = kCache.data() + static_cast<size_t>(l) * layerStride;
            uint16_t* vc = vCache.data() + static_cast<size_t>(l) * layerStride;
            for (int b = 0; b < B; ++b) {
                float* qb = &q[static_cast<size_t>(b * qDim)];
                float* kb = &k[static_cast<size_t>(b * kvDim)];
                float* vb = &v[static_cast<size_t>(b * kvDim)];
                for (int i = 0; L.bq.present() && i < qDim; ++i) qb[i] += bq[static_cast<size_t>(i)];
                for (int i = 0; L.bk.present() && i < kvDim; ++i) kb[i] += bk[static_cast<size_t>(i)];
                for (int i = 0; L.bv.present() && i < kvDim; ++i) vb[i] += bv[static_cast<size_t>(i)];
                for (int h = 0; h < nHead; ++h) {
                    if (L.qNorm.present()) rmsnorm(qb + h * headDim, qb + h * headDim, qn, headDim);
                    rope(qb + h * headDim, pos0 + b);
                }
                for (int h = 0; h < nHeadKv; ++h) {
                    if (L.kNorm.present()) rmsnorm(kb + h * headDim, kb + h * headDim, kn, headDim);
                    rope(kb + h * headDim, pos0 + b);
                }
                const size_t at = static_cast<size_t>(pos0 + b) * static_cast<size_t>(kvDim);
                for (int i = 0; i < kvDim; ++i) {
                    kc[at + static_cast<size_t>(i)] = toHalf(kb[i]);
                    vc[at + static_cast<size_t>(i)] = toHalf(vb[i]);
                }
            }
            const int group = nHead / nHeadKv;
            const float* h16 = halfTable();
            pool->run(static_cast<int64_t>(B) * nHead, [&](int64_t w0, int64_t w1) {
                std::vector<float> s(static_cast<size_t>(pos0 + B)), kv(static_cast<size_t>(headDim));
                for (int64_t w = w0; w < w1; ++w) {
                    const int b = static_cast<int>(w / nHead), h = static_cast<int>(w % nHead), kvh = h / group, last = pos0 + b;
                    const float* qh = &q[static_cast<size_t>(b * qDim + h * headDim)];
                    float mx = -1e30f;
                    for (int t = 0; t <= last; ++t) {
                        const uint16_t* kr = kc + static_cast<size_t>(t) * static_cast<size_t>(kvDim) + static_cast<size_t>(kvh * headDim);
                        for (int i = 0; i < headDim; ++i) kv[static_cast<size_t>(i)] = h16[kr[i]];
                        s[static_cast<size_t>(t)] = dot(qh, kv.data(), headDim) * scale;
                        mx = std::max(mx, s[static_cast<size_t>(t)]);
                    }
                    double sum = 0;
                    for (int t = 0; t <= last; ++t) sum += (s[static_cast<size_t>(t)] = std::exp(s[static_cast<size_t>(t)] - mx));
                    float* o = &att[static_cast<size_t>(b * qDim + h * headDim)];
                    std::fill(o, o + headDim, 0.0f);
                    for (int t = 0; t <= last; ++t) {
                        const float p = static_cast<float>(s[static_cast<size_t>(t)] / sum);
                        const uint16_t* vr = vc + static_cast<size_t>(t) * static_cast<size_t>(kvDim) + static_cast<size_t>(kvh * headDim);
                        for (int i = 0; i < headDim; ++i) o[i] += p * h16[vr[i]];
                    }
                }
            });
            matmul(L.wo, att.data(), B, tmp.data());
            for (size_t i = 0; i < x.size(); ++i) x[i] += tmp[i];
            for (int b = 0; b < B; ++b) rmsnorm(&xb[static_cast<size_t>(b * E)], &x[static_cast<size_t>(b * E)], fn, E);
            matmul(L.gate, xb.data(), B, g.data());
            matmul(L.up, xb.data(), B, u.data());
            for (size_t i = 0; i < g.size(); ++i) g[i] = g[i] / (1.0f + std::exp(-g[i])) * u[i];
            matmul(L.down, g.data(), B, tmp.data());
            for (size_t i = 0; i < x.size(); ++i) x[i] += tmp[i];
        }
        std::vector<float> last(static_cast<size_t>(E)), logits(static_cast<size_t>(output.ne1));
        rmsnorm(last.data(), &x[static_cast<size_t>((B - 1) * E)], vec(outNorm), E);
        matmul(output, last.data(), 1, logits.data());
        return logits;
    }

    void threads(int n) {
        if (n <= 0) n = static_cast<int>(std::max(1u, std::min(16u, std::thread::hardware_concurrency())));
        if (!pool || pool->size() != n) pool = std::make_unique<Pool>(n);
    }
};

LocalModel::LocalModel() = default;
LocalModel::~LocalModel() = default;

bool LocalModel::loadBytes(std::string bytes, std::string* error) {
    auto impl = std::make_unique<Impl>();
    impl->owned = std::move(bytes);
    impl->base = reinterpret_cast<const uint8_t*>(impl->owned.data());
    impl->size = impl->owned.size();
    if (!impl->parse(error)) return false;
    impl_ = std::move(impl);
    return true;
}

bool LocalModel::load(const std::string& path, std::string* error) {
#if !defined(_WIN32)
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (error) *error = "Cannot open " + path + ".";
        return false;
    }
    struct stat st {};
    void* map = MAP_FAILED;
    if (fstat(fd, &st) == 0 && st.st_size > 0) map = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (map == MAP_FAILED) {
        if (error) *error = "Cannot read " + path + ".";
        return false;
    }
    auto impl = std::make_unique<Impl>();
    impl->mapped = map;
    impl->base = static_cast<const uint8_t*>(map);
    impl->size = static_cast<size_t>(st.st_size);
    if (!impl->parse(error)) return false;
    impl_ = std::move(impl);
    return true;
#else
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "Cannot open " + path + ".";
        return false;
    }
    return loadBytes(std::string(std::istreambuf_iterator<char>(in), {}), error);
#endif
}

bool LocalModel::valid() const { return impl_ != nullptr; }

void LocalModel::setAccelerator(Accelerator accelerator) {
    if (impl_) impl_->accelerator = std::move(accelerator);
}

const uint8_t* LocalModel::fileData(size_t* size) const {
    if (size) *size = impl_ ? impl_->size : 0;
    return impl_ ? impl_->base : nullptr;
}

bool LocalModel::cpuMatmul(const uint8_t* weights, int type, int64_t cols, int64_t rows, size_t rowBytes, const float* x,
                           int batch, float* y) {
    const Format f = formatOf(type);
    if (!weights || f.block == 0 || cols <= 0 || cols % f.block != 0 || rows < 0 || batch < 0 ||
        rowBytes != static_cast<size_t>(cols / f.block * f.bytes))
        return false;
    std::vector<float> row(static_cast<size_t>(cols));
    for (int64_t r = 0; r < rows; ++r) {
        dequantize(type, weights + static_cast<size_t>(r) * rowBytes, cols, row.data());
        for (int b = 0; b < batch; ++b)
            y[static_cast<size_t>(b) * static_cast<size_t>(rows) + static_cast<size_t>(r)] = dot(row.data(), x + static_cast<size_t>(b) * static_cast<size_t>(cols), cols);
    }
    return true;
}

std::string LocalModel::infoJson() const {
    if (!impl_) return "{}";
    const Impl& m = *impl_;
    double params = 0;
    std::map<std::string, double> byFormat;
    for (const auto& [name, t] : m.tensors) {
        params += static_cast<double>(t.ne0) * static_cast<double>(t.ne1);
        byFormat[formatName(t.type)] += static_cast<double>(t.rowBytes) * static_cast<double>(t.ne1);
    }
    std::string quant = m.tensors.empty() ? "" : std::max_element(byFormat.begin(), byFormat.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
    Json j = Json::object();
    j["architecture"] = m.arch;
    j["name"] = m.name;
    j["parameters"] = params;
    j["layers"] = m.nLayer;
    j["context"] = m.trainCtx;
    j["vocabulary"] = static_cast<int64_t>(m.vocab.size());
    j["quantization"] = quant;
    j["bytes"] = static_cast<double>(m.size);
    return j.dump();
}

std::vector<int> LocalModel::tokenize(const std::string& text, bool special) const {
    return impl_ ? impl_->tokenize(text, special) : std::vector<int>{};
}

std::string LocalModel::detokenize(const std::vector<int>& tokens) const {
    std::string s;
    for (int t : tokens) s += impl_ ? impl_->piece(t) : "";
    return s;
}

std::string LocalModel::chatPrompt(const std::string& system, const std::string& user) const {
    const Value* t = impl_ ? impl_->find("tokenizer.chat_template") : nullptr;
    const std::string tmpl = t ? t->str : "";
    if (tmpl.find("<|start_header_id|>") != std::string::npos)
        return "<|start_header_id|>system<|end_header_id|>\n\n" + system + "<|eot_id|><|start_header_id|>user<|end_header_id|>\n\n" +
               user + "<|eot_id|><|start_header_id|>assistant<|end_header_id|>\n\n";
    if (tmpl.find("[INST]") != std::string::npos && tmpl.find("<|im_start|>") == std::string::npos)
        return "[INST] " + (system.empty() ? "" : system + "\n\n") + user + " [/INST]";
    std::string s = (system.empty() ? "" : "<|im_start|>system\n" + system + "<|im_end|>\n") + "<|im_start|>user\n" + user +
                    "<|im_end|>\n<|im_start|>assistant\n";
    if (tmpl.find("<think>") != std::string::npos) s += "<think>\n\n</think>\n\n";  // Qwen 3: answer without thinking
    return s;
}

std::vector<float> LocalModel::logits(const std::vector<int>& tokens, int threads) {
    if (!impl_ || tokens.empty()) return {};
    impl_->threads(threads);
    impl_->reserve(static_cast<int>(tokens.size()));
    return impl_->forward(tokens, 0);
}

std::string LocalModel::generate(const std::string& prompt, const Options& options,
                                 const std::function<bool(const std::string&)>& onText) {
    if (!impl_) throw std::runtime_error("No model is loaded.");
    Impl& m = *impl_;
    m.threads(options.threads);
    std::vector<int> tokens;
    if (m.addBos) tokens.push_back(m.bos);
    for (int t : m.tokenize(options.json ? prompt + "{" : prompt, true)) tokens.push_back(t);
    const int maxTokens = std::max(1, options.maxTokens);
    if (static_cast<int>(tokens.size()) + 1 >= m.trainCtx) throw std::runtime_error("The prompt is longer than the model's context.");
    m.reserve(std::min(m.trainCtx, static_cast<int>(tokens.size()) + maxTokens));
    std::vector<float> logits;
    for (size_t i = 0; i < tokens.size(); i += 64) {  // the prompt in batches: each weight row is read once per batch
        const std::vector<int> chunk(tokens.begin() + static_cast<std::ptrdiff_t>(i),
                                     tokens.begin() + static_cast<std::ptrdiff_t>(std::min(tokens.size(), i + 64)));
        logits = m.forward(chunk, static_cast<int>(i));
    }
    std::string out = options.json ? "{" : "", pending;
    if (options.json && onText && !onText("{")) return out;
    int depth = options.json ? 1 : 0;
    bool inString = false, escaped = false;
    uint64_t rng = options.seed * 0x9E3779B97F4A7C15ull + 1;
    int pos = static_cast<int>(tokens.size());
    for (int n = 0; n < maxTokens && pos < m.ctx; ++n) {
        int next = 0;
        if (options.temperature <= 0) {
            next = static_cast<int>(std::max_element(logits.begin(), logits.end()) - logits.begin());
        } else {
            std::vector<std::pair<float, int>> p;
            const float mx = *std::max_element(logits.begin(), logits.end());
            double sum = 0;
            for (size_t i = 0; i < logits.size(); ++i) {
                const float e = static_cast<float>(std::exp((logits[i] - mx) / options.temperature));
                if (e > 1e-7f) { p.emplace_back(e, static_cast<int>(i)); sum += e; }
            }
            std::sort(p.begin(), p.end(), std::greater<>());
            rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
            double cut = 0, keep = 0;
            size_t k = 0;
            for (; k < p.size() && keep < options.topP * sum; ++k) keep += p[k].first;
            const double r = static_cast<double>(rng >> 11) / 9007199254740992.0 * keep;
            next = p.empty() ? 0 : p[0].second;
            for (size_t i = 0; i < k; ++i) {
                cut += p[i].first;
                if (r < cut) { next = p[i].second; break; }
            }
        }
        if (std::find(m.stops.begin(), m.stops.end(), next) != m.stops.end()) break;
        pending += m.piece(next);
        const size_t ready = completeUtf8(pending);
        std::string text = pending.substr(0, ready);
        pending.erase(0, ready);
        bool closed = false;
        if (options.json) {
            for (size_t i = 0; i < text.size(); ++i) {
                const char c = text[i];
                if (inString) {
                    if (escaped) escaped = false;
                    else if (c == '\\') escaped = true;
                    else if (c == '"') inString = false;
                } else if (c == '"') {
                    inString = true;
                } else if (c == '{' || c == '[') {
                    ++depth;
                } else if ((c == '}' || c == ']') && --depth == 0) {
                    text.resize(i + 1);
                    closed = true;
                    break;
                }
            }
        }
        out += text;
        if (onText && !text.empty() && !onText(text)) break;
        if (closed) break;
        logits = m.forward({next}, pos++);
    }
    return out + pending;
}

}  // namespace sieda
