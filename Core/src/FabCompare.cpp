// SiEDA Core — fabrication package comparison: two Gerber / Excellon packages (folders or .zip archives) compared
// layer by layer on a raster grid (copper, mask, silk … added and removed, with where) and hole by hole (drills added,
// removed, resized), plus the other files that changed. Readers: a .zip archive (stored or deflated entries; the
// inflate is RFC 1951, bounds-checked, with an output limit) and the Gerber / Excellon reader of Documents.cpp.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "sieda/Documents.hpp"

namespace sieda {

namespace {

// ---- inflate (RFC 1951) ----

struct BitReader {
    const uint8_t* data;
    size_t size, pos = 0;
    uint32_t buf = 0;
    int count = 0;
    int bits(int need) {
        uint32_t v = buf;
        while (count < need) {
            if (pos >= size) throw std::runtime_error("deflate data ends early");
            v |= static_cast<uint32_t>(data[pos++]) << count;
            count += 8;
        }
        buf = v >> need;
        count -= need;
        return static_cast<int>(v & ((1u << need) - 1));
    }
};

struct Huffman {
    short count[16] = {};
    std::vector<short> symbol;
};

// Canonical code from code lengths; false when the lengths over-subscribe the code.
bool buildHuffman(Huffman& h, const short* lengths, int n) {
    std::fill(std::begin(h.count), std::end(h.count), 0);
    for (int s = 0; s < n; ++s) ++h.count[lengths[s]];
    if (h.count[0] == n) {
        h.symbol.assign(static_cast<size_t>(n), 0);
        return true;
    }
    int left = 1;
    for (int len = 1; len < 16; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return false;
    }
    short offs[16] = {};
    for (int len = 1; len < 15; ++len) offs[len + 1] = static_cast<short>(offs[len] + h.count[len]);
    h.symbol.assign(static_cast<size_t>(n), 0);
    for (int s = 0; s < n; ++s)
        if (lengths[s] != 0) h.symbol[static_cast<size_t>(offs[lengths[s]]++)] = static_cast<short>(s);
    return true;
}

int decodeSymbol(BitReader& in, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
        code |= in.bits(1);
        const int c = h.count[len];
        if (code - c < first) {
            const size_t at = static_cast<size_t>(index + (code - first));
            if (at >= h.symbol.size()) throw std::runtime_error("bad deflate code");
            return h.symbol[at];
        }
        index += c;
        first += c;
        first <<= 1;
        code <<= 1;
    }
    throw std::runtime_error("bad deflate code");
}

const short kLengthBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const short kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const short kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049,
                             3073, 4097, 6145, 8193, 12289, 16385, 24577};
const short kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void inflateBlock(BitReader& in, std::string& out, const Huffman& lencode, const Huffman& distcode, size_t limit) {
    for (;;) {
        int sym = decodeSymbol(in, lencode);
        if (sym < 256) {
            if (out.size() >= limit) throw std::runtime_error("archive entry too large");
            out += static_cast<char>(sym);
        } else if (sym == 256) {
            return;
        } else {
            sym -= 257;
            if (sym >= 29) throw std::runtime_error("bad deflate length");
            const size_t len = static_cast<size_t>(kLengthBase[sym] + in.bits(kLengthExtra[sym]));
            const int ds = decodeSymbol(in, distcode);
            if (ds >= 30) throw std::runtime_error("bad deflate distance");
            const size_t dist = static_cast<size_t>(kDistBase[ds] + in.bits(kDistExtra[ds]));
            if (dist > out.size()) throw std::runtime_error("deflate distance too far back");
            if (out.size() + len > limit) throw std::runtime_error("archive entry too large");
            for (size_t k = 0; k < len; ++k) out += out[out.size() - dist];
        }
    }
}

std::string inflateRaw(const std::string& data, size_t limit) {
    BitReader in{reinterpret_cast<const uint8_t*>(data.data()), data.size()};
    std::string out;
    int last = 0;
    while (!last) {
        last = in.bits(1);
        const int type = in.bits(2);
        if (type == 0) {  // stored
            in.buf = 0, in.count = 0;
            if (in.pos + 4 > in.size) throw std::runtime_error("deflate data ends early");
            const unsigned len = in.data[in.pos] | (in.data[in.pos + 1] << 8);
            const unsigned nlen = in.data[in.pos + 2] | (in.data[in.pos + 3] << 8);
            in.pos += 4;
            if (len != (~nlen & 0xFFFFu)) throw std::runtime_error("bad stored block");
            if (in.pos + len > in.size) throw std::runtime_error("deflate data ends early");
            if (out.size() + len > limit) throw std::runtime_error("archive entry too large");
            out.append(reinterpret_cast<const char*>(in.data + in.pos), len);
            in.pos += len;
        } else if (type == 1) {  // fixed codes
            struct Fixed {
                Huffman len, dist;
                Fixed() {
                    short lengths[288];
                    int s = 0;
                    for (; s < 144; ++s) lengths[s] = 8;
                    for (; s < 256; ++s) lengths[s] = 9;
                    for (; s < 280; ++s) lengths[s] = 7;
                    for (; s < 288; ++s) lengths[s] = 8;
                    buildHuffman(len, lengths, 288);
                    for (s = 0; s < 30; ++s) lengths[s] = 5;
                    buildHuffman(dist, lengths, 30);
                }
            };
            static const Fixed fixed;  // built once, thread-safe
            const Huffman &fixedLen = fixed.len, &fixedDist = fixed.dist;
            inflateBlock(in, out, fixedLen, fixedDist, limit);
        } else if (type == 2) {  // dynamic codes
            const int nlen = in.bits(5) + 257, ndist = in.bits(5) + 1, ncode = in.bits(4) + 4;
            if (nlen > 286 || ndist > 30) throw std::runtime_error("bad deflate counts");
            static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            short lengths[320] = {};
            for (int k = 0; k < ncode; ++k) lengths[order[k]] = static_cast<short>(in.bits(3));
            Huffman lencode, distcode;
            if (!buildHuffman(lencode, lengths, 19)) throw std::runtime_error("bad deflate code lengths");
            int index = 0;
            while (index < nlen + ndist) {
                int sym = decodeSymbol(in, lencode);
                if (sym < 16) {
                    lengths[index++] = static_cast<short>(sym);
                } else {
                    short len = 0;
                    int repeat;
                    if (sym == 16) {
                        if (index == 0) throw std::runtime_error("bad deflate repeat");
                        len = lengths[index - 1];
                        repeat = 3 + in.bits(2);
                    } else if (sym == 17) {
                        repeat = 3 + in.bits(3);
                    } else {
                        repeat = 11 + in.bits(7);
                    }
                    if (index + repeat > nlen + ndist) throw std::runtime_error("bad deflate repeat");
                    while (repeat--) lengths[index++] = len;
                }
            }
            if (lengths[256] == 0) throw std::runtime_error("deflate block has no end code");
            if (!buildHuffman(lencode, lengths, nlen) || !buildHuffman(distcode, lengths + nlen, ndist))
                throw std::runtime_error("bad deflate code lengths");
            inflateBlock(in, out, lencode, distcode, limit);
        } else {
            throw std::runtime_error("bad deflate block type");
        }
    }
    return out;
}

uint32_t le16(const std::string& s, size_t at) {
    if (at + 2 > s.size()) throw std::runtime_error("archive ends early");
    return static_cast<uint8_t>(s[at]) | (static_cast<uint8_t>(s[at + 1]) << 8);
}
uint32_t le32(const std::string& s, size_t at) { return le16(s, at) | (le16(s, at + 2) << 16); }

// ---- layers ----

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string baseName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// A package's layers by key: the Gerber X2 file function when there is one (the same layer whoever named the file),
// else the file name without the part every file shares in front (the board's base name).
struct Layer {
    std::string file;
    GerberImage image;
};

std::map<std::string, Layer> packageLayers(const std::map<std::string, std::string>& files, std::map<std::string, std::string>& others) {
    std::vector<std::pair<std::string, GerberImage>> parsed;
    for (const auto& [path, bytes] : files) {
        const std::string ext = lower(path.substr(path.find_last_of('.') == std::string::npos ? path.size() : path.find_last_of('.')));
        static const std::vector<std::string> notArtwork = {".pdf", ".zip", ".csv", ".md", ".txt", ".html", ".htm", ".step",
                                                            ".stp", ".stl", ".png", ".jpg", ".json", ".xml", ".tgz", ".cir",
                                                            ".dxf", ".idf", ".emn", ".emp", ".py", ".fcmacro", ".siedaproj"};
        bool artwork = std::find(notArtwork.begin(), notArtwork.end(), ext) == notArtwork.end();
        if (ext == ".txt" && bytes.find("M48") != std::string::npos) artwork = true;  // an Excellon drill named .txt
        if (artwork) {
            GerberImage img = parseGerber(bytes);
            if (img.error.empty() && (!img.shapes.empty() || bytes.find("%FS") != std::string::npos || bytes.find("M48") != std::string::npos)) {
                parsed.push_back({path, std::move(img)});
                continue;
            }
        }
        others[path] = bytes;
    }
    std::string common;
    bool first = true;
    for (const auto& [path, img] : parsed) {
        const std::string name = baseName(path);
        if (first) common = name, first = false;
        else {
            size_t k = 0;
            while (k < common.size() && k < name.size() && common[k] == name[k]) ++k;
            common.resize(k);
        }
    }
    if (parsed.size() < 2) common.clear();
    std::map<std::string, Layer> out;
    for (auto& [path, img] : parsed) {
        const std::string& bytes = files.at(path);
        std::string key;
        if (const size_t at = bytes.find(".FileFunction,"); at != std::string::npos) {
            const size_t end = bytes.find_first_of("*\r\n", at);
            key = bytes.substr(at + 14, end == std::string::npos ? std::string::npos : end - at - 14);
            if (img.kind == "drill") key = "drill " + key;
        } else {
            key = lower(baseName(path).substr(common.size()));
        }
        while (out.count(key)) key += "'";
        out[key] = Layer{path, std::move(img)};
    }
    return out;
}

// ---- raster ----

struct Grid {
    double x0, y0, res;
    int w, h;
};

void paint(std::vector<uint8_t>& cells, const Grid& g, const GerberShape& s) {
    const uint8_t v = s.clear ? 0 : 1;
    auto span = [&](double lo, double hi, int n, double o) {
        return std::pair<int, int>{std::max(0, static_cast<int>(std::floor((lo - o) / g.res))),
                                   std::min(n - 1, static_cast<int>(std::ceil((hi - o) / g.res)))};
    };
    auto cell = [&](int i, int j) { return Vec2{g.x0 + (i + 0.5) * g.res, g.y0 + (j + 0.5) * g.res}; };
    if (s.kind == GerberShape::Region) {
        if (s.points.size() < 3) return;
        double ylo = 1e18, yhi = -1e18;
        for (Vec2 p : s.points) ylo = std::min(ylo, p.y), yhi = std::max(yhi, p.y);
        const auto [j0, j1] = span(ylo, yhi, g.h, g.y0);
        std::vector<double> xs;
        for (int j = j0; j <= j1; ++j) {
            const double y = g.y0 + (j + 0.5) * g.res;
            xs.clear();
            for (size_t k = 0; k < s.points.size(); ++k) {
                const Vec2 a = s.points[k], b = s.points[(k + 1) % s.points.size()];
                if ((a.y <= y) != (b.y <= y)) xs.push_back(a.x + (y - a.y) / (b.y - a.y) * (b.x - a.x));
            }
            std::sort(xs.begin(), xs.end());
            for (size_t k = 0; k + 1 < xs.size(); k += 2) {
                const int i0 = std::max(0, static_cast<int>(std::ceil((xs[k] - g.x0) / g.res - 0.5)));
                const int i1 = std::min(g.w - 1, static_cast<int>(std::floor((xs[k + 1] - g.x0) / g.res - 0.5)));
                for (int i = i0; i <= i1; ++i) cells[static_cast<size_t>(j) * g.w + i] = v;
            }
        }
        return;
    }
    if (s.kind == GerberShape::Stroke) {
        const double r = s.width / 2;
        const auto [i0, i1] = span(std::min(s.a.x, s.b.x) - r, std::max(s.a.x, s.b.x) + r, g.w, g.x0);
        const auto [j0, j1] = span(std::min(s.a.y, s.b.y) - r, std::max(s.a.y, s.b.y) + r, g.h, g.y0);
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i)
                if (pointSegmentDistance(cell(i, j), s.a, s.b) <= r) cells[static_cast<size_t>(j) * g.w + i] = v;
        return;
    }
    const double hw = s.width / 2, hh = (s.height > 0 ? s.height : s.width) / 2;
    const auto [i0, i1] = span(s.a.x - hw, s.a.x + hw, g.w, g.x0);
    const auto [j0, j1] = span(s.a.y - hh, s.a.y + hh, g.h, g.y0);
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) {
            const Vec2 c = cell(i, j);
            const double dx = std::fabs(c.x - s.a.x), dy = std::fabs(c.y - s.a.y);
            bool in;
            if (s.aperture == 'R') in = dx <= hw && dy <= hh;
            else if (s.aperture == 'O') {
                const double r = std::min(hw, hh);
                in = pointSegmentDistance(c, s.a + Vec2{hw > hh ? -(hw - r) : 0, hh > hw ? -(hh - r) : 0},
                                          s.a + Vec2{hw > hh ? hw - r : 0, hh > hw ? hh - r : 0}) <= r;
            } else in = dx * dx + dy * dy <= hw * hw;
            if (in) cells[static_cast<size_t>(j) * g.w + i] = v;
        }
}

}  // namespace

std::map<std::string, std::string> readZipArchive(const std::string& bytes) {
    const size_t kMaxEntries = 2000, kMaxTotal = size_t(256) << 20;
    if (bytes.size() < 22) throw std::runtime_error("not a zip archive");
    size_t eocd = std::string::npos;
    for (size_t k = bytes.size() - 22 + 1; k-- > 0 && bytes.size() - k <= 65557;)
        if (le32(bytes, k) == 0x06054b50) {
            eocd = k;
            break;
        }
    if (eocd == std::string::npos) throw std::runtime_error("not a zip archive (no end of central directory)");
    const uint32_t entries = le16(bytes, eocd + 10), cdOffset = le32(bytes, eocd + 16);
    if (entries == 0xFFFF || cdOffset == 0xFFFFFFFFu) throw std::runtime_error("ZIP64 archives are not read");
    if (entries > kMaxEntries) throw std::runtime_error("too many entries in the archive");
    std::map<std::string, std::string> out;
    size_t total = 0, at = cdOffset;
    for (uint32_t e = 0; e < entries; ++e) {
        if (le32(bytes, at) != 0x02014b50) throw std::runtime_error("bad central directory");
        const uint32_t method = le16(bytes, at + 10), csize = le32(bytes, at + 20), usize = le32(bytes, at + 24);
        const uint32_t nlen = le16(bytes, at + 28), xlen = le16(bytes, at + 30), clen = le16(bytes, at + 32);
        const uint32_t local = le32(bytes, at + 42);
        if (at + 46 + nlen > bytes.size()) throw std::runtime_error("archive ends early");
        const std::string name = bytes.substr(at + 46, nlen);
        at += 46 + static_cast<size_t>(nlen) + xlen + clen;
        if (name.empty() || name.back() == '/') continue;  // a folder
        if (le32(bytes, local) != 0x04034b50) throw std::runtime_error("bad local header");
        const size_t data = local + 30 + static_cast<size_t>(le16(bytes, local + 26)) + le16(bytes, local + 28);
        if (data + csize > bytes.size()) throw std::runtime_error("archive ends early");
        if (usize > kMaxTotal || total + usize > kMaxTotal) throw std::runtime_error("archive unpacks too large");
        std::string content;
        if (method == 0) content = bytes.substr(data, csize);
        else if (method == 8) content = inflateRaw(bytes.substr(data, csize), kMaxTotal - total);
        else throw std::runtime_error("archive entry " + name + " uses an unsupported compression");
        total += content.size();
        out[name] = std::move(content);
    }
    return out;
}

std::vector<uint8_t> rasterizeGerber(const GerberImage& image, double x0, double y0, double res, int w, int h) {
    std::vector<uint8_t> cells(static_cast<size_t>(w) * h, 0);
    const Grid g{x0, y0, res, w, h};
    for (const auto& s : image.shapes) paint(cells, g, s);
    return cells;
}

Json compareFabPackages(const std::map<std::string, std::string>& a, const std::map<std::string, std::string>& b, double resolution) {
    resolution = std::clamp(std::isfinite(resolution) ? resolution : 0.05, 0.01, 1.0);
    std::map<std::string, std::string> othersA, othersB;
    const auto la = packageLayers(a, othersA), lb = packageLayers(b, othersB);
    Json layers = Json::array();
    bool identical = true;
    std::set<std::string> keys;
    for (const auto& [k, l] : la) keys.insert(k);
    for (const auto& [k, l] : lb) keys.insert(k);
    for (const auto& key : keys) {
        Json j = Json::object();
        j["layer"] = key;
        const Layer* A = la.count(key) ? &la.at(key) : nullptr;
        const Layer* B = lb.count(key) ? &lb.at(key) : nullptr;
        if (A) j["fileA"] = A->file;
        if (B) j["fileB"] = B->file;
        if (!A || !B) {
            j["status"] = A ? "removed" : "added";
            identical = false;
            layers.push(j);
            continue;
        }
        if (A->image.kind == "drill" && B->image.kind == "drill") {  // hole by hole
            struct Hole { Vec2 at; double d; };
            auto holes = [](const GerberImage& img) {
                std::vector<Hole> h;
                for (const auto& s : img.shapes)
                    if (s.kind == GerberShape::Flash) h.push_back({s.a, s.width});
                    else if (s.kind == GerberShape::Stroke) h.push_back({(s.a + s.b) * 0.5, s.width});  // a routed slot
                return h;
            };
            std::vector<Hole> ha = holes(A->image), hb = holes(B->image);
            std::vector<char> usedB(hb.size(), 0);
            Json added = Json::array(), removed = Json::array(), resized = Json::array();  // the first 50 of each
            int nAdded = 0, nRemoved = 0, nResized = 0;
            auto hole = [](const Hole& h) {
                Json o = Json::array();
                o.push(std::round(h.at.x * 1000) / 1000), o.push(std::round(h.at.y * 1000) / 1000), o.push(std::round(h.d * 1000) / 1000);
                return o;
            };
            for (const Hole& h : ha) {
                size_t match = SIZE_MAX;
                for (size_t k = 0; k < hb.size(); ++k)
                    if (!usedB[k] && (hb[k].at - h.at).length() < 0.005 && (match == SIZE_MAX || std::fabs(hb[k].d - h.d) < 1e-4)) match = k;
                if (match == SIZE_MAX) {
                    if (nRemoved++ < 50) removed.push(hole(h));
                    continue;
                }
                usedB[match] = 1;
                if (std::fabs(hb[match].d - h.d) > 1e-4 && nResized++ < 50) {
                    Json o = hole(h);
                    o.push(std::round(hb[match].d * 1000) / 1000);
                    resized.push(o);
                }
            }
            for (size_t k = 0; k < hb.size(); ++k)
                if (!usedB[k] && nAdded++ < 50) added.push(hole(hb[k]));
            const bool same = nAdded == 0 && nRemoved == 0 && nResized == 0;
            j["status"] = same ? "same" : "changed";
            j["holesA"] = static_cast<int>(ha.size()), j["holesB"] = static_cast<int>(hb.size());
            if (!same) {
                identical = false;
                j["holesAdded"] = nAdded, j["holesRemoved"] = nRemoved, j["holesResized"] = nResized;
                j["added"] = added, j["removed"] = removed, j["resized"] = resized;  // [x, y, d (, new d)] mm, Y up
            }
            layers.push(j);
            continue;
        }
        if (a.at(A->file) == b.at(B->file)) {
            j["status"] = "same";
            layers.push(j);
            continue;
        }
        // Artwork: both images on one grid over their joined bounds.
        const double x0 = std::min(A->image.x0, B->image.x0) - 1, y0 = std::min(A->image.y0, B->image.y0) - 1;
        const double x1 = std::max(A->image.x1, B->image.x1) + 1, y1 = std::max(A->image.y1, B->image.y1) + 1;
        double res = resolution;
        while ((x1 - x0) / res * ((y1 - y0) / res) > 16e6) res *= 1.5;  // at most ~16 M cells a layer
        const int w = std::max(1, static_cast<int>(std::ceil((x1 - x0) / res))), h = std::max(1, static_cast<int>(std::ceil((y1 - y0) / res)));
        const auto ca = rasterizeGerber(A->image, x0, y0, res, w, h), cb = rasterizeGerber(B->image, x0, y0, res, w, h);
        size_t add = 0, rem = 0, both = 0;
        std::vector<uint8_t> diff(ca.size(), 0);  // 1 removed, 2 added
        for (size_t k = 0; k < ca.size(); ++k) {
            if (ca[k] && cb[k]) ++both;
            else if (ca[k]) ++rem, diff[k] = 1;
            else if (cb[k]) ++add, diff[k] = 2;
        }
        const double cellArea = res * res;
        j["resolution"] = res;
        j["areaA"] = std::round((both + rem) * cellArea * 100) / 100, j["areaB"] = std::round((both + add) * cellArea * 100) / 100;
        if (add == 0 && rem == 0) {
            j["status"] = "same";
            layers.push(j);
            continue;
        }
        identical = false;
        j["status"] = "changed";
        j["addedMm2"] = std::round(add * cellArea * 100) / 100, j["removedMm2"] = std::round(rem * cellArea * 100) / 100;
        // Where: each patch of changed cells (8-connected), largest first.
        struct Patch { int i0, j0, i1, j1; size_t added = 0, removed = 0; };
        std::vector<Patch> patches;
        std::vector<int> stack;
        std::vector<uint8_t> seen(diff.size(), 0);
        for (int sj = 0; sj < h; ++sj)
            for (int si = 0; si < w; ++si) {
                const size_t s0 = static_cast<size_t>(sj) * w + si;
                if (!diff[s0] || seen[s0]) continue;
                Patch p{si, sj, si, sj};
                seen[s0] = 1;
                stack.assign(1, static_cast<int>(s0));
                while (!stack.empty()) {
                    const int c = stack.back();
                    stack.pop_back();
                    const int ci = c % w, cj = c / w;
                    (diff[static_cast<size_t>(c)] == 2 ? p.added : p.removed)++;
                    p.i0 = std::min(p.i0, ci), p.i1 = std::max(p.i1, ci), p.j0 = std::min(p.j0, cj), p.j1 = std::max(p.j1, cj);
                    for (int dj = -1; dj <= 1; ++dj)
                        for (int di = -1; di <= 1; ++di) {
                            const int ni = ci + di, nj = cj + dj;
                            if (ni < 0 || nj < 0 || ni >= w || nj >= h) continue;
                            const size_t n = static_cast<size_t>(nj) * w + ni;
                            if (diff[n] && !seen[n]) seen[n] = 1, stack.push_back(static_cast<int>(n));
                        }
                }
                patches.push_back(p);
            }
        std::sort(patches.begin(), patches.end(), [](const Patch& p, const Patch& q) { return p.added + p.removed > q.added + q.removed; });
        Json where = Json::array();
        for (size_t k = 0; k < patches.size() && k < 50; ++k) {
            const Patch& p = patches[k];
            Json o = Json::object();
            o["x"] = std::round((x0 + (p.i0 + p.i1 + 1) * res / 2) * 100) / 100;
            o["y"] = std::round((y0 + (p.j0 + p.j1 + 1) * res / 2) * 100) / 100;
            o["w"] = std::round((p.i1 - p.i0 + 1) * res * 100) / 100, o["h"] = std::round((p.j1 - p.j0 + 1) * res * 100) / 100;
            if (p.added) o["addedMm2"] = std::round(p.added * cellArea * 1000) / 1000;
            if (p.removed) o["removedMm2"] = std::round(p.removed * cellArea * 1000) / 1000;
            where.push(o);
        }
        j["changes"] = static_cast<int>(patches.size());
        j["where"] = where;  // centre x / y and size w / h (mm, Y up as in the files), largest first
        layers.push(j);
    }
    Json files = Json::array();  // the other files (BOM, notes, drawings …): changed or not by their bytes
    std::set<std::string> names;
    for (const auto& [n, v] : othersA) names.insert(n);
    for (const auto& [n, v] : othersB) names.insert(n);
    auto undated = [](const std::string& text) {  // a job file's CreationDate is when it was written, not what
        std::string out;
        for (size_t at = 0; at < text.size();) {
            const size_t end = std::min(text.find('\n', at), text.size());
            if (text.substr(at, end - at).find("CreationDate") == std::string::npos) out.append(text, at, end - at + 1);
            at = end + 1;
        }
        return out;
    };
    for (const auto& n : names) {
        const bool inA = othersA.count(n), inB = othersB.count(n);
        if (inA && inB && (othersA.at(n) == othersB.at(n) || undated(othersA.at(n)) == undated(othersB.at(n)))) continue;
        Json o = Json::object();
        o["file"] = n;
        o["status"] = !inA ? "added" : !inB ? "removed" : "changed";
        files.push(o);
        identical = false;
    }
    Json out = Json::object();
    out["identical"] = identical;
    out["layers"] = layers;
    if (files.size()) out["otherFiles"] = files;
    return out;
}

}  // namespace sieda
