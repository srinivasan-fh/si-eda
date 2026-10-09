// SiEDA Core — a TrueType font for PDF output: Unicode to glyph lookup, advance widths and a subset of the glyphs a
// document uses, so schematic PDFs can carry any script the font covers (docs/SCHEMATIC.md, "PDF").
#pragma once

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace sieda {

/// A glyf-outline TrueType font (.ttf, or the first font of a .ttc). Every read is bounds-checked: a damaged or
/// unsupported file (CFF outlines, missing tables) simply does not load.
class TrueTypeFont {
public:
    /// Parses font bytes (kept by the object). False — and an unusable font — when the file is not a usable font.
    bool load(std::string bytes);
    bool valid() const { return valid_; }
    /// Glyph of a Unicode code point (0 = the font has none).
    int glyph(uint32_t codepoint) const;
    /// Advance width of a glyph in em (1 = the font's em square).
    double advance(int glyph) const;
    int numGlyphs() const { return numGlyphs_; }
    int unitsPerEm() const { return unitsPerEm_; }
    int ascent() const { return ascent_; }    // font units
    int descent() const { return descent_; }  // font units (negative)
    const int* bbox() const { return bbox_; }  // xMin, yMin, xMax, yMax in font units
    /// A font file with the same glyph numbering holding only `glyphs` (with the parts of composite glyphs and glyph
    /// 0); every other glyph is empty. Empty string when the font is not valid.
    std::string subset(const std::set<int>& glyphs) const;

    /// A glyph of shaped text: offsets and advance in em, `cluster` = index of its first code point.
    struct Shaped {
        int glyph = 0;
        size_t cluster = 0;
        double dx = 0, dy = 0, advance = 0;
    };
    /// Shapes one run of text with HarfBuzz (ligatures, Indic conjuncts and reordering, Arabic joining, right-to-left
    /// runs in visual order) when the core is built with it (`canShape`); otherwise one glyph per code point.
    std::vector<Shaped> shape(const std::vector<uint32_t>& codepoints) const;
    static bool canShape();

private:
    struct Table {
        uint32_t offset = 0, length = 0;
        bool present = false;
    };
    uint32_t u32(size_t at) const;
    uint16_t u16(size_t at) const;
    Table table(const char* tag) const;
    uint32_t glyphOffset(int glyph, uint32_t* length) const;

    std::string data_;
    mutable std::shared_ptr<void> shaper_;  // hb_font_t, made on first use
    std::vector<std::pair<std::string, Table>> tables_;
    bool valid_ = false;
    int numGlyphs_ = 0, unitsPerEm_ = 1000, ascent_ = 800, descent_ = -200, numHMetrics_ = 0;
    int bbox_[4] = {0, 0, 0, 0};
    bool longLoca_ = false;
    uint32_t cmapOffset_ = 0;  // the Unicode subtable used (format 4 or 12)
    int cmapFormat_ = 0;
};

}  // namespace sieda
