// SiEDA Core — OLE compound files (MS-CFB) and Altium .SchLib / .PcbLib records. See AltiumLibrary.hpp.
#include "sieda/AltiumLibrary.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <set>

namespace sieda {

namespace {
constexpr uint32_t kFreeSect = 0xFFFFFFFFu, kEndOfChain = 0xFFFFFFFEu, kMaxRegSect = 0xFFFFFFFAu;
constexpr size_t kMaxEntries = 65536;
constexpr size_t kMaxRecords = 200000;

uint16_t u16(const std::string& b, size_t at) {
    if (at + 2 > b.size()) throw CfbError("the file is cut short");
    return static_cast<uint16_t>(static_cast<unsigned char>(b[at]) | (static_cast<unsigned char>(b[at + 1]) << 8));
}

uint32_t u32(const std::string& b, size_t at) {
    if (at + 4 > b.size()) throw CfbError("the file is cut short");
    uint32_t v = 0;
    for (int k = 3; k >= 0; --k) v = (v << 8) | static_cast<unsigned char>(b[at + static_cast<size_t>(k)]);
    return v;
}

int32_t i32(const std::string& b, size_t at) {
    const uint32_t v = u32(b, at);
    int32_t r;
    std::memcpy(&r, &v, 4);
    return r;
}

int16_t i16(const std::string& b, size_t at) {
    const uint16_t v = u16(b, at);
    int16_t r;
    std::memcpy(&r, &v, 2);
    return r;
}

double f64(const std::string& b, size_t at) {
    if (at + 8 > b.size()) throw CfbError("the record is cut short");
    uint64_t v = 0;
    for (int k = 7; k >= 0; --k) v = (v << 8) | static_cast<unsigned char>(b[at + static_cast<size_t>(k)]);
    double d;
    std::memcpy(&d, &v, 8);
    return d;
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

/// Windows-1252 / Latin-1 text (Altium's 8-bit strings) as UTF-8; control characters dropped.
std::string latin1(const std::string& s) {
    std::string out;
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7F) continue;
        if (c >= 0x80 && c < 0xA0) {
            out += c == 0x80 ? "\xE2\x82\xAC" : "?";  // € and the rest of the 1252 block
            continue;
        }
        appendUtf8(out, c);
    }
    return out;
}

/// Valid UTF-8 with control characters dropped (for %UTF8% values).
std::string validUtf8(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 && c >= 0xC2 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 && c <= 0xF4 ? 4 : 0;
        bool ok = len > 0 && i + len <= s.size();
        for (size_t k = 1; ok && k < len; ++k) ok = (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
        if (!ok) {
            out += '?';
            ++i;
            continue;
        }
        if (!(len == 1 && (c < 0x20 || c == 0x7F))) out.append(s, i, len);
        i += len;
    }
    return out;
}

std::string upperAscii(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

// ------------------------------------------------------------------ compound file

bool CompoundFile::looksLikeCompoundFile(const std::string& b) { return b.compare(0, 8, "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8) == 0; }

CompoundFile::CompoundFile(const std::string& bytes) : b_(bytes) {
    if (bytes.size() < 512 || !looksLikeCompoundFile(bytes)) throw CfbError("not an OLE compound file");
    if (bytes.size() > (256u << 20)) throw CfbError("the file is larger than 256 MB");
    const uint16_t major = u16(b_, 0x1A), shift = u16(b_, 0x1E), miniShift = u16(b_, 0x20);
    if (u16(b_, 0x1C) != 0xFFFE) throw CfbError("bad byte-order mark");
    if (major == 3 && shift == 9) sectorSize_ = 512;
    else if (major == 4 && shift == 12) sectorSize_ = 4096;
    else throw CfbError("unsupported version " + std::to_string(major) + " / sector shift " + std::to_string(shift));
    if (miniShift != 6) throw CfbError("unsupported mini sector size");
    miniSectorSize_ = 64;
    miniCutoff_ = u32(b_, 0x38);
    if (miniCutoff_ != 4096) throw CfbError("unsupported mini stream cutoff");
    if (b_.size() < sectorSize_) throw CfbError("the file is cut short");
    sectorCount_ = (b_.size() - sectorSize_) / sectorSize_ + ((b_.size() - sectorSize_) % sectorSize_ ? 1 : 0);
    const uint32_t numFat = u32(b_, 0x2C), firstDir = u32(b_, 0x30), firstMiniFat = u32(b_, 0x3C);
    const uint32_t numMiniFat = u32(b_, 0x40), firstDifat = u32(b_, 0x44), numDifat = u32(b_, 0x48);
    if (numFat > sectorCount_ || numMiniFat > sectorCount_ || numDifat > sectorCount_)
        throw CfbError("the sector tables are larger than the file");
    auto sectorAt = [&](uint32_t s) -> size_t {
        if (s >= sectorCount_) throw CfbError("a sector number points past the end of the file");
        return (static_cast<size_t>(s) + 1) * sectorSize_;
    };
    // DIFAT: 109 entries in the header, then a chain of DIFAT sectors.
    std::vector<uint32_t> fatSectors;
    for (int i = 0; i < 109 && fatSectors.size() < numFat; ++i) {
        const uint32_t s = u32(b_, 0x4C + 4 * static_cast<size_t>(i));
        if (s == kFreeSect) break;
        fatSectors.push_back(s);
    }
    uint32_t difat = firstDifat;
    const size_t perDifat = sectorSize_ / 4 - 1;
    for (uint32_t n = 0; n < numDifat && fatSectors.size() < numFat; ++n) {
        if (difat > kMaxRegSect) throw CfbError("the DIFAT chain ends early");
        const size_t at = sectorAt(difat);
        for (size_t i = 0; i < perDifat && fatSectors.size() < numFat; ++i) {
            const uint32_t s = u32(b_, at + 4 * i);
            if (s == kFreeSect) break;
            fatSectors.push_back(s);
        }
        difat = u32(b_, at + 4 * perDifat);
    }
    if (fatSectors.size() != numFat) throw CfbError("the FAT is incomplete");
    for (uint32_t s : fatSectors) {
        const size_t at = sectorAt(s);
        for (size_t i = 0; i < sectorSize_ / 4; ++i) fat_.push_back(u32(b_, at + 4 * i));
    }
    // Directory.
    const auto dirSectors = chain(firstDir, fat_, kMaxEntries * 128 / sectorSize_ + 1);
    if (dirSectors.empty()) throw CfbError("the file has no directory");
    for (uint32_t s : dirSectors) {
        const size_t at = sectorAt(s);
        for (size_t e = 0; e + 128 <= sectorSize_; e += 128) {
            const size_t p = at + e;
            Entry entry;
            if (p + 128 > b_.size()) throw CfbError("the file is cut short");  // a partial last sector
            const uint8_t type = static_cast<uint8_t>(b_[p + 0x42]);
            entry.type = type == 1 ? EntryType::Storage : type == 2 ? EntryType::Stream : type == 5 ? EntryType::Root
                                                                                                   : EntryType::Empty;
            const uint16_t nameLen = u16(b_, p + 0x40);
            if (entry.type != EntryType::Empty) {
                if (nameLen > 64 || nameLen % 2) throw CfbError("a directory entry has a bad name length");
                for (size_t k = 0; k + 2 < nameLen + 0u; k += 2) {
                    uint32_t cp = u16(b_, p + k);
                    if (cp >= 0xD800 && cp <= 0xDBFF && k + 4 < nameLen) {
                        const uint32_t lo = u16(b_, p + k + 2);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            k += 2;
                        }
                    }
                    if (cp == 0) break;
                    if (cp < 0x20 || (cp >= 0xD800 && cp <= 0xDFFF)) cp = '?';
                    appendUtf8(entry.name, cp);
                }
            }
            entry.start = u32(b_, p + 0x74);
            entry.size = sectorSize_ == 512 ? u32(b_, p + 0x78) : (static_cast<uint64_t>(u32(b_, p + 0x7C)) << 32) | u32(b_, p + 0x78);
            entries_.push_back(entry);
            // Sibling and child links are resolved below; keep them alongside.
            links_.push_back({u32(b_, p + 0x44), u32(b_, p + 0x48), u32(b_, p + 0x4C)});
            if (entries_.size() > kMaxEntries) throw CfbError("the directory has too many entries");
        }
    }
    if (entries_.empty() || entries_[0].type != EntryType::Root) throw CfbError("the first directory entry is not the root");
    // Children of each storage: walk the red-black tree of its child, each entry claimed once (no cycles).
    std::vector<char> claimed(entries_.size(), 0);
    claimed[0] = 1;
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].type != EntryType::Storage && entries_[i].type != EntryType::Root) continue;
        std::vector<uint32_t> stack;
        uint32_t node = links_[i][2];
        // In-order traversal: left subtree, node, right subtree.
        size_t guard = 0;
        while ((node < entries_.size() || !stack.empty()) && ++guard < 4 * kMaxEntries) {
            while (node < entries_.size()) {
                if (claimed[node]) {
                    node = kFreeSect;  // a loop or a shared subtree: stop this branch
                    break;
                }
                claimed[node] = 1;
                stack.push_back(node);
                node = links_[node][0];
            }
            if (stack.empty()) break;
            const uint32_t top = stack.back();
            stack.pop_back();
            if (entries_[top].type != EntryType::Empty) entries_[i].children.push_back(static_cast<int>(top));
            node = links_[top][1];
        }
    }
    // Mini stream (held by the root entry) and the mini FAT.
    const Entry& root = entries_[0];
    if (root.size > 0) {
        if (root.size > b_.size()) throw CfbError("the mini stream is larger than the file");
        const auto sectors = chain(root.start, fat_, static_cast<size_t>((root.size + sectorSize_ - 1) / sectorSize_));
        for (uint32_t s : sectors) miniStream_.append(b_, sectorAt(s), std::min<size_t>(sectorSize_, b_.size() - sectorAt(s)));
        if (miniStream_.size() < root.size) throw CfbError("the mini stream is cut short");
        miniStream_.resize(static_cast<size_t>(root.size));
    }
    if (numMiniFat > 0) {
        const auto sectors = chain(firstMiniFat, fat_, numMiniFat);
        for (uint32_t s : sectors) {
            const size_t at = sectorAt(s);
            for (size_t i = 0; i < sectorSize_ / 4; ++i) miniFat_.push_back(u32(b_, at + 4 * i));
        }
    }
}

std::vector<uint32_t> CompoundFile::chain(uint32_t start, const std::vector<uint32_t>& table, size_t maxLength) const {
    std::vector<uint32_t> out;
    uint32_t cur = start;
    while (cur != kEndOfChain && out.size() < maxLength) {
        if (cur >= table.size() || cur > kMaxRegSect) throw CfbError("a sector chain is broken");
        out.push_back(cur);
        cur = table[cur];
    }
    return out;
}

int CompoundFile::child(int storage, const std::string& name) const {
    if (storage < 0 || static_cast<size_t>(storage) >= entries_.size()) return -1;
    const std::string want = upperAscii(name);
    for (int c : entries_[static_cast<size_t>(storage)].children)
        if (upperAscii(entries_[static_cast<size_t>(c)].name) == want) return c;
    return -1;
}

std::string CompoundFile::read(int index, size_t limit) const {
    if (index < 0 || static_cast<size_t>(index) >= entries_.size()) throw CfbError("no such stream");
    const Entry& e = entries_[static_cast<size_t>(index)];
    if (e.type != EntryType::Stream) throw CfbError("not a stream");
    if (e.size > limit || e.size > b_.size()) throw CfbError("the stream '" + e.name + "' is too large");
    const size_t size = static_cast<size_t>(e.size);
    std::string out;
    out.reserve(size);
    if (size == 0) return out;
    if (e.size < miniCutoff_) {
        const auto sectors = chain(e.start, miniFat_, (size + miniSectorSize_ - 1) / miniSectorSize_);
        for (uint32_t s : sectors) {
            const size_t at = static_cast<size_t>(s) * miniSectorSize_;
            if (at >= miniStream_.size()) throw CfbError("a mini sector points past the mini stream");
            out.append(miniStream_, at, std::min<size_t>(miniSectorSize_, miniStream_.size() - at));
        }
    } else {
        const auto sectors = chain(e.start, fat_, (size + sectorSize_ - 1) / sectorSize_);
        for (uint32_t s : sectors) {
            if (s >= sectorCount_) throw CfbError("a sector number points past the end of the file");
            const size_t at = (static_cast<size_t>(s) + 1) * sectorSize_;
            out.append(b_, at, std::min<size_t>(sectorSize_, b_.size() - at));
        }
    }
    if (out.size() < size) throw CfbError("the stream '" + e.name + "' is cut short");
    out.resize(size);
    return out;
}

// ------------------------------------------------------------------ Altium records

std::map<std::string, std::string> altiumProperties(const std::string& raw) {
    std::map<std::string, std::string> plain, utf8;
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t end = raw.find('|', pos);
        if (end == std::string::npos) end = raw.size();
        const std::string item = raw.substr(pos, end - pos);
        pos = end + 1;
        const size_t eq = item.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::string key = upperAscii(item.substr(0, eq));
        std::string value = item.substr(eq + 1);
        while (!value.empty() && value.back() == '\0') value.pop_back();
        if (key.size() > 64 || plain.size() > 4096) continue;
        if (value.size() > 4096) value.resize(4096);
        if (key.rfind("%UTF8%", 0) == 0) utf8[key.substr(6)] = validUtf8(value);
        else plain[latin1(key)] = latin1(value);
    }
    for (auto& kv : utf8) plain[kv.first] = kv.second;
    return plain;
}

namespace {
struct Record {
    bool binary = false;
    std::string payload;
};

/// Schematic "Data" stream: [u32: length (low 24 bits) | type << 24][payload]…
std::vector<Record> readRecords(const std::string& data, std::vector<std::string>& warnings) {
    std::vector<Record> out;
    size_t pos = 0;
    while (pos + 4 <= data.size()) {
        const uint32_t header = u32(data, pos);
        const size_t len = header & 0xFFFFFF;
        const unsigned type = header >> 24;
        pos += 4;
        if (len > data.size() - pos) {
            warnings.push_back("a record runs past the end of its stream; the rest was skipped");
            break;
        }
        if (type <= 1) out.push_back({type == 1, data.substr(pos, len)});
        pos += len;
        if (out.size() > kMaxRecords) throw CfbError("too many records");
    }
    return out;
}

int intOf(const std::map<std::string, std::string>& p, const char* key, int def) {
    auto it = p.find(key);
    if (it == p.end() || it->second.empty()) return def;
    char* end = nullptr;
    const long v = std::strtol(it->second.c_str(), &end, 10);
    if (end == it->second.c_str()) return def;
    return static_cast<int>(std::clamp<long>(v, -10000000, 10000000));
}

std::string strOf(const std::map<std::string, std::string>& p, const char* key) {
    auto it = p.find(key);
    return it == p.end() ? std::string() : it->second;
}

constexpr double kSchUnit = 0.254;  // schematic coordinates: 10 mil

/// The connection point (mm, y up) and the side of a pin from its body-end location, length and orientation.
void placePin(AltiumPin& pin, double locX, double locY, double length, int orientation) {
    static const int dx[4] = {1, 0, -1, 0}, dy[4] = {0, 1, 0, -1};
    static const char side[4] = {'R', 'T', 'L', 'B'};
    const int o = orientation & 3;
    pin.x = (locX + dx[o] * length) * kSchUnit;
    pin.y = (locY + dy[o] * length) * kSchUnit;
    pin.side = side[o];
}

/// Altium marks an overbar with a backslash after each character ("R\E\S\E\T\"): an "n" prefix here.
std::string pinName(const std::string& raw) {
    if (raw.find('\\') == std::string::npos) return raw;
    std::string out;
    for (char c : raw)
        if (c != '\\') out += c;
    return out.empty() ? out : "n" + out;
}

/// Reads a length-prefixed (u8) string inside a binary record.
std::string pascal(const std::string& b, size_t& pos) {
    if (pos >= b.size()) throw CfbError("a pin record is cut short");
    const size_t n = static_cast<unsigned char>(b[pos++]);
    if (pos + n > b.size()) throw CfbError("a pin record is cut short");
    std::string s = latin1(b.substr(pos, n));
    pos += n;
    return s;
}

/// Binary pin record (the form current SchLibs store pins in).
bool binaryPin(const std::string& b, AltiumPin& pin, int& displayMode) {
    size_t pos = 0;
    if (i32(b, 0) != 2) return false;  // not a pin
    pos = 4 + 1;                          // record id, unknown byte
    pin.part = i16(b, pos);
    pos += 2;
    displayMode = static_cast<unsigned char>(b.at(pos));
    pos += 1 + 4;  // display mode, inner / outer edge and inside / outside symbols
    pascal(b, pos);  // description
    pos += 1;        // formal type
    if (pos + 2 > b.size()) throw CfbError("a pin record is cut short");
    pin.electrical = static_cast<unsigned char>(b[pos]);
    const int conglomerate = static_cast<unsigned char>(b[pos + 1]);
    pos += 2;
    const int length = i16(b, pos), x = i16(b, pos + 2), y = i16(b, pos + 4);
    pos += 6 + 4;  // length, x, y, colour
    pin.name = pinName(pascal(b, pos));
    pin.designator = pascal(b, pos);
    pin.hidden = (conglomerate & 0x04) != 0;
    placePin(pin, x, y, length, conglomerate);
    return true;
}
}  // namespace

std::string altiumLibraryKind(const CompoundFile& cfb, const std::string& fileName) {
    const int header = cfb.child(CompoundFile::root(), "FileHeader");
    std::string text;
    if (header >= 0) {
        try {
            text = upperAscii(cfb.read(header, 1u << 20).substr(0, 512));
        } catch (const CfbError&) {
        }
    }
    if (text.find("SCHEMATIC LIBRARY") != std::string::npos) return "schlib";
    if (text.find("PCB") != std::string::npos && text.find("LIBRARY") != std::string::npos) return "pcblib";
    std::string ext;
    const size_t dot = fileName.rfind('.');
    if (dot != std::string::npos) ext = upperAscii(fileName.substr(dot + 1));
    if (ext == "INTLIB" || cfb.child(CompoundFile::root(), "LibCrossRef.Txt") >= 0) return "intlib";
    if (ext == "SCHLIB") return "schlib";
    if (ext == "PCBLIB") return "pcblib";
    return {};
}

std::vector<AltiumSymbol> readAltiumSchLib(const CompoundFile& cfb) {
    std::vector<AltiumSymbol> out;
    for (int storage : cfb.entries()[0].children) {
        const auto& e = cfb.entries()[static_cast<size_t>(storage)];
        if (e.type != CompoundFile::EntryType::Storage) continue;
        const int data = cfb.child(storage, "Data");
        if (data < 0) continue;
        AltiumSymbol sym;
        std::vector<Record> records;
        try {
            records = readRecords(cfb.read(data), sym.warnings);
        } catch (const CfbError& err) {
            sym.name = e.name;
            sym.warnings.push_back(std::string("cannot be read: ") + err.what());
            out.push_back(sym);
            continue;
        }
        bool component = false;
        int skippedModes = 0, badPins = 0;
        bool currentModel = false;
        for (const auto& r : records) {
            AltiumPin pin;
            int mode = 0;
            if (r.binary) {
                try {
                    if (!binaryPin(r.payload, pin, mode)) continue;
                } catch (const std::exception&) {
                    ++badPins;
                    continue;
                }
            } else {
                const auto p = altiumProperties(r.payload);
                const int record = intOf(p, "RECORD", -1);
                if (record == 1 && !component) {
                    component = true;
                    sym.name = strOf(p, "LIBREFERENCE");
                    if (sym.name.empty()) sym.name = strOf(p, "DESIGNITEMID");
                    sym.description = strOf(p, "COMPONENTDESCRIPTION");
                    sym.partCount = std::clamp(intOf(p, "PARTCOUNT", 2) - 1, 1, 64);
                    continue;
                }
                if (record == 34) {  // designator ("U?")
                    std::string prefix;
                    for (char c : strOf(p, "TEXT"))
                        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') prefix += c;
                        else break;
                    if (!prefix.empty() && prefix.size() <= 8) sym.designatorPrefix = prefix;
                    continue;
                }
                if (record == 41) {  // parameter
                    const std::string name = strOf(p, "NAME"), text = strOf(p, "TEXT");
                    if (!name.empty() && !text.empty() && text != "*" && sym.parameters.size() < 64) sym.parameters[name] = text;
                    continue;
                }
                if (record == 45) {  // implementation: a footprint model
                    if (upperAscii(strOf(p, "MODELTYPE")) != "PCBLIB") continue;
                    const std::string model = strOf(p, "MODELNAME");
                    if (model.empty()) continue;
                    if (sym.footprints.size() < 32) sym.footprints.push_back(model);
                    const bool current = upperAscii(strOf(p, "ISCURRENT")) == "T";
                    if (sym.footprint.empty() || (current && !currentModel)) sym.footprint = model;
                    currentModel = currentModel || current;
                    continue;
                }
                if (record != 2) continue;
                pin.part = intOf(p, "OWNERPARTID", 1);
                mode = intOf(p, "OWNERPARTDISPLAYMODE", 0);
                pin.name = pinName(strOf(p, "NAME"));
                pin.designator = strOf(p, "DESIGNATOR");
                pin.electrical = intOf(p, "ELECTRICAL", 4);
                const int conglomerate = intOf(p, "PINCONGLOMERATE", 0);
                pin.hidden = (conglomerate & 0x04) != 0;
                const double x = intOf(p, "LOCATION.X", 0) + intOf(p, "LOCATION.X_FRAC", 0) / 100000.0;
                const double y = intOf(p, "LOCATION.Y", 0) + intOf(p, "LOCATION.Y_FRAC", 0) / 100000.0;
                const double len = intOf(p, "PINLENGTH", 0) + intOf(p, "PINLENGTH_FRAC", 0) / 100000.0;
                placePin(pin, x, y, len, conglomerate);
            }
            if (mode != 0) {  // De Morgan / alternate display modes
                ++skippedModes;
                continue;
            }
            if (pin.designator.empty()) pin.designator = pin.name;
            if (pin.designator.empty() || sym.pins.size() >= 1024) {
                ++badPins;
                continue;
            }
            if (pin.part < 1) pin.part = 1;
            pin.electrical = std::clamp(pin.electrical, 0, 7);
            sym.pins.push_back(pin);
        }
        if (!component && sym.pins.empty()) continue;  // not a component's storage
        if (sym.name.empty()) sym.name = e.name;
        if (!component) sym.warnings.push_back("no component record: read as " + e.name);
        if (skippedModes) sym.warnings.push_back(std::to_string(skippedModes) + " pin(s) of alternate display modes skipped");
        if (badPins) sym.warnings.push_back(std::to_string(badPins) + " unreadable pin record(s) skipped");
        out.push_back(std::move(sym));
        if (out.size() >= 2000) break;
    }
    return out;
}

// ------------------------------------------------------------------ PCB library

namespace {
constexpr double kPcbUnit = 2.54e-6;  // mm per internal unit (1/10000 mil)

/// One length-prefixed subrecord at `pos`.
std::string subrecord(const std::string& d, size_t& pos) {
    const uint32_t len = u32(d, pos);
    pos += 4;
    if (len > d.size() - pos) throw CfbError("a primitive runs past the end of its stream");
    std::string s = d.substr(pos, len);
    pos += len;
    return s;
}

bool sane(double mm) { return std::isfinite(mm) && std::fabs(mm) <= 1000; }
}  // namespace

std::vector<AltiumFootprint> readAltiumPcbLib(const CompoundFile& cfb) {
    std::vector<AltiumFootprint> out;
    for (int storage : cfb.entries()[0].children) {
        const auto& e = cfb.entries()[static_cast<size_t>(storage)];
        if (e.type != CompoundFile::EntryType::Storage || upperAscii(e.name) == "LIBRARY") continue;
        const int data = cfb.child(storage, "Data");
        if (data < 0) continue;
        AltiumFootprint fp;
        fp.name = e.name;
        try {
            const int params = cfb.child(storage, "Parameters");
            if (params >= 0) {
                const std::string raw = cfb.read(params, 1u << 20);
                const auto p = altiumProperties(raw.size() > 4 ? raw.substr(4) : raw);
                if (!strOf(p, "PATTERN").empty()) fp.name = strOf(p, "PATTERN");
                fp.description = strOf(p, "DESCRIPTION");
            }
            const std::string d = cfb.read(data);
            size_t pos = 0;
            {
                const std::string head = subrecord(d, pos);  // footprint name
                if (!head.empty()) {
                    const size_t n = static_cast<unsigned char>(head[0]);
                    if (n + 1 <= head.size() && n > 0) fp.name = latin1(head.substr(1, n));
                }
            }
            double ox0 = 1e9, oy0 = 1e9, ox1 = -1e9, oy1 = -1e9;
            while (pos < d.size()) {
                const unsigned type = static_cast<unsigned char>(d[pos++]);
                if (type == 0) break;
                if (type == 2) {  // pad: six subrecords
                    AltiumPad pad;
                    const std::string s1 = subrecord(d, pos);
                    if (!s1.empty()) {
                        const size_t n = static_cast<unsigned char>(s1[0]);
                        if (n + 1 <= s1.size()) pad.name = latin1(s1.substr(1, n));
                    }
                    subrecord(d, pos);
                    subrecord(d, pos);
                    subrecord(d, pos);
                    const std::string g = subrecord(d, pos);
                    subrecord(d, pos);  // size and shape per layer (optional data)
                    if (g.size() < 62) {
                        fp.warnings.push_back("a pad record is too short and was skipped");
                        continue;
                    }
                    pad.layer = static_cast<unsigned char>(g[0]);
                    pad.x = i32(g, 13) * kPcbUnit;
                    pad.y = -i32(g, 17) * kPcbUnit;  // Altium's y points up
                    pad.w = i32(g, 21) * kPcbUnit;
                    pad.h = i32(g, 25) * kPcbUnit;
                    pad.drill = i32(g, 45) * kPcbUnit;
                    pad.shape = static_cast<unsigned char>(g[49]);
                    pad.rotation = f64(g, 52);
                    pad.plated = g[60] != 0;
                    if (!sane(pad.x) || !sane(pad.y) || !(pad.w > 0) || !(pad.h > 0) || pad.w > 100 || pad.h > 100 ||
                        !(pad.drill >= 0) || pad.drill > 100 || !std::isfinite(pad.rotation)) {
                        fp.warnings.push_back("pad " + pad.name + " has impossible geometry and was skipped");
                        continue;
                    }
                    if (fp.pads.size() >= 1024) throw CfbError("the footprint has too many pads");
                    fp.pads.push_back(pad);
                } else if (type == 4) {  // track: the top overlay outline gives the body
                    const std::string t = subrecord(d, pos);
                    if (t.size() >= 33 && static_cast<unsigned char>(t[0]) == 33) {
                        const double x0 = i32(t, 13) * kPcbUnit, y0 = -i32(t, 17) * kPcbUnit;
                        const double x1 = i32(t, 21) * kPcbUnit, y1 = -i32(t, 25) * kPcbUnit;
                        if (sane(x0) && sane(y0) && sane(x1) && sane(y1)) {
                            ox0 = std::min({ox0, x0, x1});
                            oy0 = std::min({oy0, y0, y1});
                            ox1 = std::max({ox1, x0, x1});
                            oy1 = std::max({oy1, y0, y1});
                        }
                    }
                    ++fp.skippedPrimitives;
                } else if (type == 5) {  // text: properties and the string
                    subrecord(d, pos);
                    subrecord(d, pos);
                    ++fp.skippedPrimitives;
                } else if (type == 1 || type == 3 || type == 6 || type == 11 || type == 12) {
                    subrecord(d, pos);  // arc, via, fill, region, 3D body
                    ++fp.skippedPrimitives;
                } else {
                    fp.warnings.push_back("unknown primitive type " + std::to_string(type) + ": the rest of the footprint was skipped");
                    break;
                }
            }
            if (ox1 > ox0 && oy1 > oy0) {
                fp.hasOutline = true;
                fp.outlineX0 = ox0;
                fp.outlineY0 = oy0;
                fp.outlineX1 = ox1;
                fp.outlineY1 = oy1;
            }
        } catch (const CfbError& err) {
            fp.warnings.push_back(std::string("read stopped: ") + err.what());
        }
        out.push_back(std::move(fp));
        if (out.size() >= 2000) break;
    }
    return out;
}

}  // namespace sieda
