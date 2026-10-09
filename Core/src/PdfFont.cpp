// SiEDA Core — TrueType reading and subsetting for PDF output (see PdfFont.hpp).
//
// Only what a PDF needs is read: the table directory, head / hhea / maxp (sizes), hmtx (advance widths), cmap
// (formats 4 and 12, Unicode subtables) and loca / glyf (to copy the glyphs used). The subset keeps the original
// glyph numbering (unused glyphs become empty), so a PDF can address glyphs directly (CIDToGIDMap /Identity).
#include "sieda/PdfFont.hpp"

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>

#ifdef SIEDA_HAVE_HARFBUZZ
#include <hb.h>
#endif

namespace sieda {

namespace {
int16_t asSigned(uint16_t v) { return static_cast<int16_t>(v); }

void put16(std::string& s, uint16_t v) {
    s += static_cast<char>(v >> 8);
    s += static_cast<char>(v & 0xFF);
}
void put32(std::string& s, uint32_t v) {
    put16(s, static_cast<uint16_t>(v >> 16));
    put16(s, static_cast<uint16_t>(v & 0xFFFF));
}
void set16(std::string& s, size_t at, uint16_t v) {
    if (at + 2 > s.size()) return;
    s[at] = static_cast<char>(v >> 8);
    s[at + 1] = static_cast<char>(v & 0xFF);
}
void set32(std::string& s, size_t at, uint32_t v) {
    set16(s, at, static_cast<uint16_t>(v >> 16));
    set16(s, at + 2, static_cast<uint16_t>(v & 0xFFFF));
}
uint32_t checksum(const std::string& s) {
    uint32_t sum = 0;
    for (size_t i = 0; i < s.size(); i += 4) {
        uint32_t word = 0;
        for (size_t k = 0; k < 4; ++k) word = (word << 8) | (i + k < s.size() ? static_cast<unsigned char>(s[i + k]) : 0u);
        sum += word;
    }
    return sum;
}
}  // namespace

uint16_t TrueTypeFont::u16(size_t at) const {
    if (at + 2 > data_.size()) return 0;
    return static_cast<uint16_t>((static_cast<unsigned char>(data_[at]) << 8) | static_cast<unsigned char>(data_[at + 1]));
}

uint32_t TrueTypeFont::u32(size_t at) const {
    if (at + 4 > data_.size()) return 0;
    return (static_cast<uint32_t>(u16(at)) << 16) | u16(at + 2);
}

TrueTypeFont::Table TrueTypeFont::table(const char* tag) const {
    for (const auto& [t, info] : tables_)
        if (t == tag) return info;
    return Table{};
}

bool TrueTypeFont::load(std::string bytes) {
    valid_ = false;
    tables_.clear();
    shaper_.reset();
    data_ = std::move(bytes);
    if (data_.size() < 12 || data_.size() > (64u << 20)) return false;
    size_t base = 0;
    if (data_.compare(0, 4, "ttcf") == 0) {
        if (u32(8) == 0) return false;
        base = u32(12);
    }
    const uint32_t version = u32(base);
    if (version != 0x00010000 && version != 0x74727565) return false;  // CFF ('OTTO') outlines are not read
    const int numTables = u16(base + 4);
    if (numTables < 1 || numTables > 200) return false;
    for (int i = 0; i < numTables; ++i) {
        const size_t rec = base + 12 + 16 * static_cast<size_t>(i);
        if (rec + 16 > data_.size()) return false;
        Table t;
        t.offset = u32(rec + 8);
        t.length = u32(rec + 12);
        t.present = static_cast<uint64_t>(t.offset) + t.length <= data_.size();
        if (t.present) tables_.push_back({data_.substr(rec, 4), t});
    }
    const Table head = table("head"), hhea = table("hhea"), maxp = table("maxp"), hmtx = table("hmtx"), loca = table("loca"),
                glyf = table("glyf"), cmap = table("cmap");
    for (const Table* t : {&head, &hhea, &maxp, &hmtx, &loca, &glyf, &cmap})
        if (!t->present) return false;
    if (head.length < 54 || hhea.length < 36 || maxp.length < 6) return false;
    unitsPerEm_ = u16(head.offset + 18);
    if (unitsPerEm_ < 16 || unitsPerEm_ > 16384) return false;
    for (int k = 0; k < 4; ++k) bbox_[k] = asSigned(u16(head.offset + 36 + 2 * static_cast<size_t>(k)));
    longLoca_ = asSigned(u16(head.offset + 50)) == 1;
    numGlyphs_ = u16(maxp.offset + 4);
    ascent_ = asSigned(u16(hhea.offset + 4));
    descent_ = asSigned(u16(hhea.offset + 6));
    numHMetrics_ = u16(hhea.offset + 34);
    if (numGlyphs_ < 1 || numHMetrics_ < 1 || numHMetrics_ > numGlyphs_) return false;
    if (hmtx.length < 4u * static_cast<uint32_t>(numHMetrics_)) return false;
    if (loca.length < static_cast<uint32_t>(numGlyphs_ + 1) * (longLoca_ ? 4u : 2u)) return false;
    // Unicode cmap: a full-repertoire subtable (format 12) when there is one, else the BMP one (format 4).
    cmapFormat_ = 0;
    const int subtables = u16(cmap.offset + 2);
    int bestRank = 0;
    for (int i = 0; i < subtables && i < 64; ++i) {
        const size_t rec = cmap.offset + 4 + 8 * static_cast<size_t>(i);
        if (rec + 8 > static_cast<size_t>(cmap.offset) + cmap.length) break;
        const int platform = u16(rec), encoding = u16(rec + 2);
        const uint32_t off = u32(rec + 4);
        if (off >= cmap.length) continue;
        const uint32_t at = cmap.offset + off;
        const int format = u16(at);
        const bool unicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
        if (!unicode) continue;
        const int rank = format == 12 ? 2 : format == 4 ? 1 : 0;
        if (rank > bestRank) {
            bestRank = rank;
            cmapFormat_ = format;
            cmapOffset_ = at;
        }
    }
    if (cmapFormat_ == 0) return false;
    valid_ = true;
    return true;
}

int TrueTypeFont::glyph(uint32_t cp) const {
    if (!valid_) return 0;
    const Table cmap = table("cmap");
    const size_t end = static_cast<size_t>(cmap.offset) + cmap.length;
    int g = 0;
    if (cmapFormat_ == 12) {
        const uint32_t groups = u32(cmapOffset_ + 12);
        const size_t first = cmapOffset_ + 16;
        if (first + 12ull * groups > end) return 0;
        size_t lo = 0, hi = groups;
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            const size_t at = first + 12 * mid;
            const uint32_t s = u32(at), e = u32(at + 4);
            if (cp < s) hi = mid;
            else if (cp > e) lo = mid + 1;
            else {
                g = static_cast<int>(u32(at + 8) + (cp - s));
                break;
            }
        }
    } else {
        if (cp > 0xFFFF) return 0;
        const size_t segX2 = u16(cmapOffset_ + 6);
        const size_t ends = cmapOffset_ + 14, starts = ends + segX2 + 2, deltas = starts + segX2, ranges = deltas + segX2;
        if (segX2 == 0 || ranges + segX2 > end) return 0;
        for (size_t k = 0; k < segX2; k += 2) {
            if (u16(ends + k) < cp) continue;
            const uint16_t start = u16(starts + k);
            if (cp < start) break;
            const uint16_t delta = u16(deltas + k), ro = u16(ranges + k);
            if (ro == 0) {
                g = (cp + delta) & 0xFFFF;
            } else {
                const size_t addr = ranges + k + ro + 2 * (cp - start);
                if (addr + 2 > end) return 0;
                const uint16_t raw = u16(addr);
                g = raw == 0 ? 0 : (raw + delta) & 0xFFFF;
            }
            break;
        }
    }
    return g > 0 && g < numGlyphs_ ? g : 0;
}

bool TrueTypeFont::canShape() {
#ifdef SIEDA_HAVE_HARFBUZZ
    return true;
#else
    return false;
#endif
}

std::vector<TrueTypeFont::Shaped> TrueTypeFont::shape(const std::vector<uint32_t>& cps) const {
    std::vector<Shaped> out;
#ifdef SIEDA_HAVE_HARFBUZZ
    if (valid_ && !cps.empty()) {
        if (!shaper_) {
            hb_blob_t* blob = hb_blob_create(data_.data(), static_cast<unsigned>(data_.size()), HB_MEMORY_MODE_DUPLICATE, nullptr, nullptr);
            hb_face_t* face = hb_face_create(blob, 0);
            hb_blob_destroy(blob);
            shaper_ = std::shared_ptr<void>(hb_font_create(face), [](void* f) { hb_font_destroy(static_cast<hb_font_t*>(f)); });
            hb_face_destroy(face);
        }
        hb_buffer_t* buf = hb_buffer_create();
        hb_buffer_add_codepoints(buf, cps.data(), static_cast<int>(cps.size()), 0, static_cast<int>(cps.size()));
        hb_buffer_guess_segment_properties(buf);
        hb_shape(static_cast<hb_font_t*>(shaper_.get()), buf, nullptr, 0);
        unsigned n = 0;
        const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &n);
        const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, nullptr);
        const double em = hb_face_get_upem(hb_font_get_face(static_cast<hb_font_t*>(shaper_.get())));
        for (unsigned i = 0; i < n; ++i)
            out.push_back({info[i].codepoint < static_cast<unsigned>(numGlyphs_) ? static_cast<int>(info[i].codepoint) : 0,
                           std::min<size_t>(info[i].cluster, cps.size() - 1), pos[i].x_offset / em, pos[i].y_offset / em,
                           pos[i].x_advance / em});
        hb_buffer_destroy(buf);
        return out;
    }
#endif
    for (size_t i = 0; i < cps.size(); ++i) {
        const int g = glyph(cps[i]);
        out.push_back({g, i, 0, 0, advance(g)});
    }
    return out;
}

double TrueTypeFont::advance(int glyph) const {
    if (!valid_ || glyph < 0) return 0.5;
    const int idx = std::min(glyph, numHMetrics_ - 1);
    return static_cast<double>(u16(table("hmtx").offset + 4 * static_cast<size_t>(idx))) / unitsPerEm_;
}

uint32_t TrueTypeFont::glyphOffset(int g, uint32_t* length) const {
    *length = 0;
    const Table loca = table("loca"), glyf = table("glyf");
    if (g < 0 || g >= numGlyphs_) return 0;
    const size_t i = static_cast<size_t>(g);
    const uint32_t a = longLoca_ ? u32(loca.offset + 4 * i) : 2u * u16(loca.offset + 2 * i);
    const uint32_t b = longLoca_ ? u32(loca.offset + 4 * (i + 1)) : 2u * u16(loca.offset + 2 * (i + 1));
    if (b < a || b > glyf.length) return 0;
    *length = b - a;
    return glyf.offset + a;
}

std::string TrueTypeFont::subset(const std::set<int>& wanted) const {
    if (!valid_) return {};
    // The glyphs to keep: those asked for, glyph 0 and every part of a composite glyph.
    std::set<int> keep{0};
    std::deque<int> queue;
    for (int g : wanted)
        if (g > 0 && g < numGlyphs_ && keep.insert(g).second) queue.push_back(g);
    while (!queue.empty() && keep.size() < 70000) {
        const int g = queue.front();
        queue.pop_front();
        uint32_t len = 0;
        const uint32_t at = glyphOffset(g, &len);
        if (len < 10 || asSigned(u16(at)) >= 0) continue;
        size_t p = at + 10;
        const size_t end = static_cast<size_t>(at) + len;
        for (int guard = 0; guard < 64 && p + 4 <= end; ++guard) {
            const uint16_t flags = u16(p), part = u16(p + 2);
            p += 4 + ((flags & 0x0001) ? 4 : 2);
            if (flags & 0x0008) p += 2;
            else if (flags & 0x0040) p += 4;
            else if (flags & 0x0080) p += 8;
            if (part < numGlyphs_ && keep.insert(part).second) queue.push_back(part);
            if (!(flags & 0x0020)) break;
        }
    }
    std::string glyf, loca;
    for (int g = 0; g < numGlyphs_; ++g) {
        put32(loca, static_cast<uint32_t>(glyf.size()));
        if (!keep.count(g)) continue;
        uint32_t len = 0;
        const uint32_t at = glyphOffset(g, &len);
        if (len == 0) continue;
        glyf.append(data_, at, len);
        while (glyf.size() % 4) glyf += '\0';
    }
    put32(loca, static_cast<uint32_t>(glyf.size()));
    // Tables: the sizes and metrics as they are, the new glyphs (long offsets), hinting programs when present.
    std::map<std::string, std::string> out;
    for (const char* tag : {"head", "hhea", "maxp", "hmtx", "cmap", "cvt ", "fpgm", "prep"}) {
        const Table t = table(tag);
        if (t.present) out[tag] = data_.substr(t.offset, t.length);
    }
    set16(out["head"], 50, 1);
    set32(out["head"], 8, 0);
    out["glyf"] = glyf;
    out["loca"] = loca;
    const uint16_t n = static_cast<uint16_t>(out.size());
    uint16_t pow2 = 1, log2 = 0;
    while (pow2 * 2 <= n) {
        pow2 = static_cast<uint16_t>(pow2 * 2);
        ++log2;
    }
    std::string file;
    put32(file, 0x00010000);
    put16(file, n);
    put16(file, static_cast<uint16_t>(pow2 * 16));
    put16(file, log2);
    put16(file, static_cast<uint16_t>(n * 16 - pow2 * 16));
    size_t offset = 12 + 16 * static_cast<size_t>(n);
    std::string body;
    size_t headAt = 0;
    for (const auto& [tag, bytes] : out) {  // std::map: tags in ascending order, as the directory needs
        file += tag;
        put32(file, checksum(bytes));
        put32(file, static_cast<uint32_t>(offset + body.size()));
        put32(file, static_cast<uint32_t>(bytes.size()));
        if (tag == "head") headAt = offset + body.size();
        body += bytes;
        while (body.size() % 4) body += '\0';
    }
    file += body;
    set32(file, headAt + 8, 0xB1B0AFBAu - checksum(file));
    return file;
}

}  // namespace sieda
