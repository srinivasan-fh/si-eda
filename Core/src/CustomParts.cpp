#include "sieda/CustomParts.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace sieda {

// ------------------------------------------------------------------ pin types

const char* pinTypeName(PinType t) {
    switch (t) {
        case PinType::Passive: return "passive";
        case PinType::Input: return "input";
        case PinType::Output: return "output";
        case PinType::Bidirectional: return "bidirectional";
        case PinType::PowerIn: return "power_in";
        case PinType::PowerOut: return "power_out";
        case PinType::OpenCollector: return "open_collector";
        case PinType::NoConnect: return "no_connect";
    }
    return "passive";
}

PinType pinTypeFromName(const std::string& raw) {
    std::string s;
    for (char c : raw)
        if (std::isalnum(static_cast<unsigned char>(c))) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s.empty() || s == "passive" || s == "analog" || s == "p") return PinType::Passive;
    if (s == "input" || s == "in" || s == "i" || s == "digitalinput") return PinType::Input;
    if (s == "output" || s == "out" || s == "o" || s == "digitaloutput") return PinType::Output;
    if (s == "bidirectional" || s == "io" || s == "inout" || s == "bidir" || s == "gpio") return PinType::Bidirectional;
    if (s == "powerin" || s == "power" || s == "pwr" || s == "supply" || s == "vcc" || s == "gnd" || s == "ground")
        return PinType::PowerIn;
    if (s == "powerout" || s == "pwrout" || s == "vout") return PinType::PowerOut;
    if (s == "opencollector" || s == "opendrain" || s == "od" || s == "oc") return PinType::OpenCollector;
    if (s == "noconnect" || s == "nc" || s == "notconnected" || s == "unused") return PinType::NoConnect;
    return PinType::Passive;
}

std::vector<std::string> supportedPackages() {
    return {"SOIC", "TSSOP", "DIP", "QFN", "LQFP", "SOT23", "HEADER", "HEADER2", "TO220", "HC49", "DISC", "MODULE",
            "BGA", "TO263", "SOT223", "MULTIWATT", "SON", "XTAL3225", "XTAL3215", "XTALCYL", "LGA", "CUSTOM"};
}

bool usesLandPattern(const std::string& packageType) { return packageType == "LGA" || packageType == "CUSTOM"; }

namespace {
/// Pin number (upper case) of land `i`: its explicit pin, else its own number i + 1; "-" = mechanical.
std::string landPin(const PackageSpec& pkg, size_t i) {
    const std::string& p = pkg.lands[i].pin;
    if (p.empty()) return std::to_string(i + 1);
    std::string u = p;
    for (auto& c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return u;
}
}  // namespace

// ------------------------------------------------------------------ JSON

Json customPartSpecToJson(const CustomPartSpec& s) {
    Json j = Json::object();
    j["name"] = s.name;
    j["manufacturer"] = s.manufacturer;
    j["description"] = s.description;
    j["refPrefix"] = s.refPrefix;
    j["defaultValue"] = s.defaultValue;
    j["datasheet"] = s.datasheet;
    Json pkg = Json::object();
    pkg["type"] = s.package.type;
    pkg["pinCount"] = s.package.pinCount;
    if (s.package.pitch > 0) pkg["pitch"] = s.package.pitch;  // only when set: older parts keep their ids
    if (s.package.bodySize > 0) pkg["bodySize"] = s.package.bodySize;
    if (s.package.bodyDepth > 0) pkg["bodyDepth"] = s.package.bodyDepth;
    if (!s.package.lands.empty()) {
        Json lands = Json::array();
        for (const auto& l : s.package.lands) {
            Json lj = Json::array();
            for (double v : {l.x, l.y, l.w, l.h}) lj.push(v);
            if (l.drill > 0 || l.round || !l.pin.empty()) {  // [x, y, w, h, drill, round(, pin)]
                lj.push(l.drill);
                lj.push(l.round ? 1 : 0);
            }
            if (!l.pin.empty()) lj.push(l.pin);
            lands.push(lj);
        }
        pkg["lands"] = lands;
    }
    j["package"] = pkg;
    Json pins = Json::array();
    for (const auto& p : s.pins) {
        Json pj = Json::object();
        pj["number"] = p.number;
        pj["name"] = p.name;
        pj["type"] = pinTypeName(p.type);
        pj["description"] = p.description;
        pins.push(pj);
    }
    j["pins"] = pins;
    if (!s.symbol.empty()) {  // only when laid out, so ids of generated-symbol parts stay stable
        Json sym = Json::object();
        if (s.symbol.width > 0) sym["width"] = s.symbol.width;
        Json sp = Json::array();
        for (const auto& p : s.symbol.pins) {
            Json pj = Json::object();
            pj["number"] = p.number;
            pj["side"] = std::string(1, p.side);
            pj["slot"] = p.slot;
            sp.push(pj);
        }
        sym["pins"] = sp;
        j["symbolLayout"] = sym;  // "symbol" is the generated geometry in customPartToJson
    }
    if (!s.model.empty()) {  // only when present, so ids of model-less parts stay stable
        Json m = Json::object();
        if (s.model.hasRegulator) {
            const auto& r = s.model.regulator;
            Json rj = Json::object();
            rj["in"] = r.in;
            rj["out"] = r.out;
            rj["ref"] = r.ref;
            rj["vout"] = r.vout;
            rj["dropout"] = r.dropout;
            rj["iq"] = r.iq;
            rj["ilimit"] = r.ilimit;
            rj["maxPower"] = r.maxPower;
            rj["charger"] = r.charger;
            if (r.loadSwitch) rj["loadSwitch"] = true;
            if (r.isolated()) {
                rj["inReturn"] = r.inReturn;
                rj["efficiency"] = r.efficiency;
            }
            m["regulator"] = rj;
        }
        if (!s.model.loads.empty()) {
            Json loads = Json::array();
            for (const auto& l : s.model.loads) {
                Json lj = Json::object();
                lj["supply"] = l.supply;
                lj["ret"] = l.ret;
                lj["current"] = l.current;
                loads.push(lj);
            }
            m["loads"] = loads;
        }
        j["model"] = m;
    }
    return j;
}

int CustomPartSpec::pinIndex(const std::string& key) const {
    for (size_t i = 0; i < pins.size(); ++i)
        if (pins[i].number == key) return static_cast<int>(i);
    for (size_t i = 0; i < pins.size(); ++i)
        if (pins[i].name == key) return static_cast<int>(i);
    return -1;
}

namespace {
std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

/// Accepts "SOIC-8", "soic", "DIP14", "PDIP", "SOT-23-5", "TO-220", "QFN-16", "1x04 header"…
std::string normalizePackage(const std::string& raw, int& pinsFromName) {
    std::string u = upper(raw);
    pinsFromName = 0;
    std::string digits;
    for (size_t i = u.size(); i-- > 0;) {
        if (std::isdigit(static_cast<unsigned char>(u[i]))) digits.insert(digits.begin(), u[i]);
        else if (!digits.empty()) break;
    }
    auto has = [&](const char* k) { return u.find(k) != std::string::npos; };
    std::string type;
    if (u.rfind("LGA", 0) == 0 || has("LAND PATTERN")) type = "LGA";
    else if (u.rfind("CUSTOM", 0) == 0) type = "CUSTOM";
    else if (has("BGA") || has("CSG") || has("FBG")) type = "BGA";
    else if (has("TO-263") || has("TO263") || has("D2PAK") || has("DDPAK")) type = "TO263";
    else if (has("SOT-223") || has("SOT223")) type = "SOT223";
    else if (has("MULTIWATT") || has("TO-220-15") || has("TO-220-11")) type = "MULTIWATT";
    else if (has("VSON") || has("TDSON") || has("WSON") || u.rfind("SON", 0) == 0) type = "SON";
    else if (has("TSSOP") || has("MSOP") || has("SSOP")) type = "TSSOP";
    else if (has("SOIC") || has("SOP") || has("SO-") || u == "SO") type = "SOIC";
    else if (has("DIP")) type = "DIP";
    else if (has("QFN") || has("DFN") || has("MLF")) type = "QFN";
    else if (has("QFP")) type = "LQFP";
    else if (has("SOT")) type = "SOT23";
    else if (has("TO220") || has("TO-220") || has("TO-92") || has("TO92")) type = "TO220";
    else if (has("3225") || has("3.2X2.5")) type = "XTAL3225";
    else if (has("3215") || has("3.2X1.5")) type = "XTAL3215";
    else if (has("CYLINDER") || has("TC26") || has("TC38")) type = "XTALCYL";
    else if (has("HC49") || has("HC-49") || has("XTAL") || has("CRYSTAL")) type = "HC49";
    else if (u.rfind("DISC", 0) == 0 || has("RADIAL DISC")) type = "DISC";
    else if (has("CASTELLATED") || has("WROOM") || u.rfind("MODULE", 0) == 0) type = "MODULE";
    else if (has("HEADER2") || has("2X") || has("IDC") || has("BOX HEADER") || has("DUAL ROW")) type = "HEADER2";
    else if (has("HEADER") || has("SIP") || has("CONN") || has("1X")) type = "HEADER";
    if (!digits.empty() && type != "TO220" && type != "HC49" && type != "DISC" && type != "MODULE" && type != "SOT223" &&
        type.rfind("XTAL", 0) != 0) {
        int n = std::stoi(digits.size() > 3 ? digits.substr(digits.size() - 3) : digits);
        if (type == "SOT23" && n == 23) n = 0;  // "SOT-23" alone means 3 pins
        if (type == "TO263" && n == 263) n = 0;  // "TO-263" alone: the pin count comes from the pin list
        pinsFromName = n;
    }
    return type;
}

bool isNumber(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}
}  // namespace

CustomPartSpec customPartSpecFromJson(const Json& j) {
    if (!j.isObject()) throw JsonError("Component definition must be a JSON object");
    CustomPartSpec s;
    s.name = trim(j.get("name").asString(""));
    s.manufacturer = trim(j.get("manufacturer").asString(""));
    s.description = trim(j.get("description").asString(""));
    s.refPrefix = trim(j.get("refPrefix").asString(j.get("ref_prefix").asString("U")));
    if (s.refPrefix.empty()) s.refPrefix = "U";
    s.defaultValue = trim(j.get("defaultValue").asString(j.get("default_value").asString(s.name)));
    s.datasheet = j.get("datasheet").asString("");
    const Json& pkg = j.get("package");
    int pinsFromName = 0;
    if (pkg.isObject()) {
        s.package.type = normalizePackage(pkg.get("type").asString("SOIC"), pinsFromName);
        s.package.pinCount = pkg.get("pinCount").asInt(pkg.get("pin_count").asInt(0));
        double pitch = pkg.get("pitch").asNumber(0), body = pkg.get("bodySize").asNumber(0);
        s.package.pitch = std::isfinite(pitch) && pitch > 0.2 && pitch <= 5.08 ? pitch : 0;
        s.package.bodySize = std::isfinite(body) && body > 0.5 && body <= 60 ? body : 0;
        const double depth = pkg.get("bodyDepth").asNumber(0);
        s.package.bodyDepth = std::isfinite(depth) && depth > 0.5 && depth <= 60 ? depth : 0;
        for (const auto& lj : pkg.get("lands").items()) {
            if (!lj.isArray() || (lj.size() != 4 && lj.size() != 6 && lj.size() != 7))
                throw JsonError("Each land is [x, y, width, height] or [x, y, width, height, drill, round, pin] in mm.");
            PackageSpec::Land l{lj[0].asNumber(0), lj[1].asNumber(0), lj[2].asNumber(0), lj[3].asNumber(0)};
            if (lj.size() >= 6) {
                l.drill = lj[4].asNumber(0);
                l.round = lj[5].asNumber(0) != 0;
            }
            if (lj.size() == 7) l.pin = trim(lj[6].asString(""));
            const bool ok = std::isfinite(l.x) && std::isfinite(l.y) && std::fabs(l.x) <= 60 && std::fabs(l.y) <= 60 &&
                            l.w > 0.05 && l.w <= 30 && l.h > 0.05 && l.h <= 30;
            if (!ok) throw JsonError("Land pattern pads must lie within 60 mm and be 0.05…30 mm in size.");
            if (!std::isfinite(l.drill) || l.drill < 0 || (l.drill > 0 && l.drill >= std::min(l.w, l.h)))
                throw JsonError("A pad's drill must be smaller than the pad.");
            if (s.package.lands.size() >= 512) throw JsonError("Land pattern has too many pads.");
            s.package.lands.push_back(l);
        }
    } else {
        s.package.type = normalizePackage(pkg.asString(j.get("package_type").asString("SOIC")), pinsFromName);
        s.package.pinCount = j.get("pin_count").asInt(0);
    }
    if (s.package.type.empty()) s.package.type = "SOIC";
    if (s.package.pinCount <= 0) s.package.pinCount = pinsFromName;
    for (const auto& pj : j.get("pins").items()) {
        CustomPin p;
        p.number = trim(pj.get("number").isNumber() ? std::to_string(pj.get("number").asInt()) : pj.get("number").asString(""));
        p.name = trim(pj.get("name").asString(""));
        p.type = pinTypeFromName(pj.get("type").asString("passive"));
        p.description = trim(pj.get("description").asString(""));
        if (p.number.empty() && p.name.empty()) continue;
        if (p.name.empty()) p.name = p.number;
        if (p.number.empty()) p.number = std::to_string(s.pins.size() + 1);
        s.pins.push_back(p);
    }
    const Json& sym = j.get("symbolLayout");
    if (sym.isObject()) {
        const double width = sym.get("width").asNumber(0);
        if (!std::isfinite(width) || width < 0 || width > 400) throw JsonError("Symbol width must be 0…400.");
        s.symbol.width = width;
        for (const auto& pj : sym.get("pins").items()) {
            SymbolPin p;
            p.number = trim(pj.get("number").isNumber() ? std::to_string(pj.get("number").asInt()) : pj.get("number").asString(""));
            const std::string side = upper(pj.get("side").asString("L"));
            p.side = side.empty() ? 'L' : side[0];
            p.slot = pj.get("slot").asInt(0);
            if (p.number.empty()) throw JsonError("Every symbol pin needs a pin number.");
            if (side.size() != 1 || std::string("LRTB").find(p.side) == std::string::npos)
                throw JsonError("Symbol pin sides are L, R, T or B.");
            if (p.slot < 0 || p.slot > 511) throw JsonError("Symbol pin slots are 0…511.");
            if (s.symbol.pins.size() >= 1024) throw JsonError("Symbol has too many pins.");
            s.symbol.pins.push_back(p);
        }
    }
    const Json& m = j.get("model");
    if (m.isObject()) {
        const Json& rj = m.get("regulator");
        if (rj.isObject()) {
            RegulatorModel& r = s.model.regulator;
            r.in = trim(rj.get("in").asString(""));
            r.out = trim(rj.get("out").asString(""));
            r.ref = trim(rj.get("ref").asString(""));
            r.vout = rj.get("vout").asNumber(0);
            r.dropout = rj.get("dropout").asNumber(0.3);  // isolated converters: minimum input − vout (may be < 0)
            r.iq = std::max(0.0, rj.get("iq").asNumber(0));
            r.ilimit = std::max(1e-6, rj.get("ilimit").asNumber(1.0));
            r.maxPower = std::max(1e-3, rj.get("maxPower").asNumber(0.5));
            r.charger = rj.get("charger").asBool(false);
            r.inReturn = trim(rj.get("inReturn").asString(""));
            r.efficiency = std::clamp(rj.get("efficiency").asNumber(0.8), 0.05, 1.0);
            r.loadSwitch = rj.get("loadSwitch").asBool(false);
            if (!r.isolated()) r.dropout = std::max(0.0, r.dropout);
            s.model.hasRegulator = true;
            // An empty `ref` references circuit ground (a floating off-line buck such as LinkSwitch-TN has no ground pin).
            if (s.pinIndex(r.in) < 0 || s.pinIndex(r.out) < 0 || (!r.ref.empty() && s.pinIndex(r.ref) < 0) ||
                (r.isolated() && s.pinIndex(r.inReturn) < 0))
                throw JsonError("Regulator model of " + s.name + " references a pin that does not exist.");
            if (!(r.vout > 0)) throw JsonError("Regulator model of " + s.name + " needs a positive vout.");
        }
        for (const auto& lj : m.get("loads").items()) {
            SupplyLoad l{trim(lj.get("supply").asString("")), trim(lj.get("ret").asString("")), lj.get("current").asNumber(0)};
            if (l.current <= 0) continue;
            if (s.pinIndex(l.supply) < 0 || s.pinIndex(l.ret) < 0)
                throw JsonError("Supply load of " + s.name + " references a pin that does not exist.");
            s.model.loads.push_back(l);
        }
    }
    return s;
}

// ------------------------------------------------------------------ geometry generation

namespace {
uint64_t fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

std::string makeId(const CustomPartSpec& spec) {
    std::string base;
    for (char c : spec.name)
        base += std::isalnum(static_cast<unsigned char>(c)) ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : '_';
    if (base.empty()) base = "PART";
    char hex[17];
    std::snprintf(hex, sizeof hex, "%08llx",
                  static_cast<unsigned long long>(fnv1a(customPartSpecToJson(spec).dump()) & 0xffffffffULL));
    return base + "-" + hex;
}

/// JEDEC ball name of pad index `i` in a square grid holding `n` balls: rows A…Z without I, O, Q, S, X, Z (then
/// AA, AB…), columns from 1.
std::string bgaBallName(int i, int n) {
    static const std::string letters = "ABCDEFGHJKLMNPRTUVWY";
    const int grid = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n)))));
    int row = i / grid;
    std::string name;
    if (row >= static_cast<int>(letters.size())) name += letters[row / static_cast<int>(letters.size()) - 1];
    name += letters[row % letters.size()];
    return name + std::to_string(i % grid + 1);
}

/// Row and column (0-based) of a JEDEC ball name ("A1", "K7", "AB12"; rows skip I, O, Q, S, X, Z).
bool parseBallName(const std::string& name, int& row, int& col) {
    static const std::string letters = "ABCDEFGHJKLMNPRTUVWY";
    size_t k = 0;
    while (k < name.size() && std::isalpha(static_cast<unsigned char>(name[k]))) ++k;
    if (k == 0 || k > 2 || k == name.size()) return false;
    for (size_t i = k; i < name.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) return false;
    const size_t a = letters.find(static_cast<char>(std::toupper(static_cast<unsigned char>(name[0]))));
    if (a == std::string::npos) return false;
    row = static_cast<int>(a);
    if (k == 2) {
        const size_t b = letters.find(static_cast<char>(std::toupper(static_cast<unsigned char>(name[1]))));
        if (b == std::string::npos) return false;
        row = (row + 1) * static_cast<int>(letters.size()) + static_cast<int>(b);
    }
    col = std::stoi(name.substr(k)) - 1;
    return col >= 0 && col < 64;
}

struct PadPlacement {
    Vec2 offset, size;
    bool tht = false, round = false;
    double drill = 0;
};

/// Pad positions for pad numbers 1..n of a package (index 0 = pad 1).
std::vector<PadPlacement> packagePads(const std::string& type, int n, double pitchIn, double bodyIn, BodyDef& body,
                                      double& courtW, double& courtH, const PackageSpec* exact = nullptr) {
    std::vector<PadPlacement> pads;
    auto dual = [&](double pitch, double rowX, Vec2 size, bool tht, double drill) {
        int perSide = (n + 1) / 2;
        double y0 = -(perSide - 1) * pitch / 2;
        for (int i = 0; i < n; ++i) {
            bool left = i < perSide;
            int k = left ? i : (n - 1 - i);  // DIP numbering: down the left, up the right
            PadPlacement p;
            p.offset = {left ? -rowX : rowX, y0 + k * pitch};
            p.size = size;
            p.tht = tht;
            p.round = tht && i != 0;
            p.drill = drill;
            pads.push_back(p);
        }
        return perSide;
    };
    auto quad = [&](double pitch, double padCentre, Vec2 size) {
        int perSide = std::max(1, n / 4);
        double s0 = -(perSide - 1) * pitch / 2;
        for (int i = 0; i < n; ++i) {
            int side = std::min(3, i / perSide), k = i % perSide;
            PadPlacement p;
            switch (side) {
                case 0: p.offset = {-padCentre, s0 + k * pitch}; p.size = size; break;                    // left, top→bottom
                case 1: p.offset = {s0 + k * pitch, padCentre}; p.size = {size.y, size.x}; break;         // bottom, left→right
                case 2: p.offset = {padCentre, -(s0 + k * pitch)}; p.size = size; break;                  // right, bottom→top
                default: p.offset = {-(s0 + k * pitch), -padCentre}; p.size = {size.y, size.x}; break;    // top, right→left
            }
            pads.push_back(p);
        }
        return perSide;
    };

    // Pad width across the lead pitch: ~55 % of the pitch, within what the package's pitch allows.
    auto padWidth = [](double pitch, double lo, double hi) { return std::clamp(pitch * 0.55, lo, hi); };
    if (type == "TSSOP") {
        double pitch = pitchIn > 0 ? pitchIn : 0.65, bw = bodyIn > 0 ? bodyIn : 4.4;
        int per = dual(pitch, bw / 2 + 0.7, {1.5, padWidth(pitch, 0.2, 0.45)}, false, 0);
        body = {bw, per * pitch + 0.5, 1.1, false, 0.12f, 0.12f, 0.13f};
    } else if (type == "DIP") {
        double row = bodyIn > 0 ? bodyIn / 2 : (n > 20 ? 7.62 : 3.81);  // bodySize = row spacing (7.62 / 15.24)
        int per = dual(2.54, row, {1.6, 1.6}, true, 0.8);
        body = {row * 2 - 1.3, per * 2.54, 3.5, false, 0.10f, 0.10f, 0.11f};
    } else if (type == "QFN") {
        int per = std::max(1, n / 4);
        double pitch = pitchIn > 0 ? pitchIn : 0.5;
        double size = bodyIn > 0 ? bodyIn : std::max(3.0, per * pitch + 1.6);  // keeps corner pads ≥ 0.3 mm apart
        quad(pitch, size / 2 - 0.2, {0.8, padWidth(pitch, 0.18, 0.35)});
        body = {size, size, 0.9, false, 0.14f, 0.14f, 0.15f};
    } else if (type == "LQFP") {
        int per = std::max(1, n / 4);
        double pitch = pitchIn > 0 ? pitchIn : 0.5;
        double size = bodyIn > 0 ? bodyIn : std::max(5.0, per * pitch + 1.5);
        quad(pitch, size / 2 + 0.75, {1.5, padWidth(pitch, 0.22, 0.5)});
        body = {size, size, 1.4, false, 0.12f, 0.12f, 0.13f};
    } else if (type == "SOT23") {
        if (n <= 3) {
            const Vec2 pos[3] = {{-0.95, 1.1}, {0.95, 1.1}, {0.0, -1.1}};
            for (int i = 0; i < n; ++i) pads.push_back({pos[i], {0.9, 0.8}, false, false, 0});
        } else {
            const Vec2 pos[6] = {{-0.95, 1.2}, {0.0, 1.2}, {0.95, 1.2}, {0.95, -1.2}, {0.0, -1.2}, {-0.95, -1.2}};
            for (int i = 0; i < std::min(n, 6); ++i) {
                Vec2 p = pos[i];
                if (n == 5 && i >= 3) p = i == 3 ? Vec2{0.95, -1.2} : Vec2{-0.95, -1.2};
                pads.push_back({p, {0.6, 1.1}, false, false, 0});
            }
        }
        body = {2.9, 1.6, 1.1, false, 0.12f, 0.12f, 0.13f};
    } else if (type == "HC49") {  // HC-49/US crystal: two leads 4.88 mm apart, 11 × 4.7 mm can
        for (int i = 0; i < n; ++i) pads.push_back({{i == 0 ? -2.44 : 2.44, 0}, {1.5, 1.5}, true, true, 0.8});
        body = {11.0, 4.7, 3.5, false, 0.75f, 0.75f, 0.78f};
    } else if (type == "XTAL3225") {
        // 3.2 × 2.5 mm SMD crystal: pads 1 and 3 are the crystal, 2 and 4 the lid / ground (counter-clockwise from
        // pad 1 at bottom left).
        const Vec2 pos[4] = {{-1.1, 0.8}, {1.1, 0.8}, {1.1, -0.8}, {-1.1, -0.8}};
        for (int i = 0; i < std::min(n, 4); ++i) pads.push_back({pos[i], {1.4, 1.15}, false, false, 0});
        body = {3.2, 2.5, 0.8, false, 0.78f, 0.78f, 0.80f};
    } else if (type == "XTAL3215") {  // 3.2 × 1.5 mm 32.768 kHz tuning-fork crystal: two end pads
        for (int i = 0; i < n; ++i) pads.push_back({{i == 0 ? -1.25 : 1.25, 0}, {1.0, 1.8}, false, false, 0});
        body = {3.2, 1.5, 0.9, false, 0.78f, 0.78f, 0.80f};
    } else if (type == "XTALCYL") {  // 2 × 6 mm cylinder can (watch crystal), leads 1.9 mm apart, lying on the board
        for (int i = 0; i < n; ++i) pads.push_back({{i == 0 ? -0.95 : 0.95, 0}, {1.2, 1.2}, true, true, 0.6});
        body = {2.0, 6.0, 2.0, false, 0.78f, 0.78f, 0.80f};
    } else if (type == "DISC") {  // radial disc (varistor, gas discharge tube): two leads 7.5 mm apart
        const double half = bodyIn > 0 ? bodyIn / 2 : 3.75;
        for (int i = 0; i < n; ++i) pads.push_back({{i == 0 ? -half : half, 0}, {2.0, 2.0}, true, true, 1.0});
        body = {half * 2 + 6.0, 5.0, 14.0, false, 0.20f, 0.35f, 0.65f};
    } else if (type == "MODULE") {
        // Castellated RF module (ESP32-WROOM class): pads down both long sides and across the bottom; the antenna end
        // (−y, 6 mm) carries no pads and overhangs the board edge or sits over a copper keep-out.
        const double pitch = pitchIn > 0 ? pitchIn : 1.27;
        const int side = n * 3 / 8, bottom = n - 2 * side;
        const double w = bodyIn > 0 ? bodyIn : std::max(10.0, (bottom + 1) * pitch + 2.5);
        const double antenna = 6.0, h = side * pitch + antenna + 2.0;
        const double yTop = -h / 2 + antenna + 1.0 + pitch / 2;
        for (int i = 0; i < side; ++i) pads.push_back({{-w / 2, yTop + i * pitch}, {1.5, 0.9}, false, false, 0});
        const double x0 = -(bottom - 1) * pitch / 2;
        for (int k = 0; k < bottom; ++k) pads.push_back({{x0 + k * pitch, h / 2}, {0.9, 1.5}, false, false, 0});
        for (int k = 0; k < side; ++k) pads.push_back({{w / 2, yTop + (side - 1 - k) * pitch}, {1.5, 0.9}, false, false, 0});
        body = {w, h, 3.1, false, 0.16f, 0.17f, 0.19f};
    } else if (type == "HEADER") {
        double y0 = -(n - 1) * 2.54 / 2;
        for (int i = 0; i < n; ++i) pads.push_back({{0, y0 + i * 2.54}, {1.7, 1.7}, true, i != 0, 1.0});
        body = {2.54, n * 2.54, 2.5, false, 0.08f, 0.08f, 0.08f};
    } else if (type == "HEADER2") {  // 2 × N, 2.54 mm, IDC numbering (1 2 / 3 4 / …)
        int rows = (n + 1) / 2;
        double y0 = -(rows - 1) * 2.54 / 2;
        for (int i = 0; i < n; ++i)
            pads.push_back({{i % 2 ? 1.27 : -1.27, y0 + (i / 2) * 2.54}, {1.7, 1.7}, true, i != 0, 1.0});
        body = {5.08, rows * 2.54, 2.5, false, 0.08f, 0.08f, 0.08f};
    } else if (type == "TO220") {
        // Lead pitch 2.54 mm, or wider for leads formed out for mains creepage (e.g. 5.08 mm on a triac).
        const double pitch = pitchIn > 2.54 ? pitchIn : 2.54;
        double x0 = -(n - 1) * pitch / 2;
        for (int i = 0; i < n; ++i) pads.push_back({{x0 + i * pitch, 0}, {1.9, 2.5}, true, false, 1.2});
        body = {10.0, 4.5, 15.0, false, 0.10f, 0.10f, 0.11f};
    } else if (type == "BGA") {
        // Ball grid: rows A, B, C… (JEDEC: no I, O, Q, S, X, Z), columns 1…N; pads in row-major order.
        const int grid = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n)))));
        const double pitch = pitchIn > 0 ? pitchIn : 0.8, x0 = -(grid - 1) * pitch / 2;
        const double ball = pitch * 0.45;
        for (int i = 0; i < n; ++i)
            pads.push_back({{x0 + (i % grid) * pitch, x0 + (i / grid) * pitch}, {ball, ball}, false, true, 0});
        const double size = bodyIn > 0 ? bodyIn : grid * pitch + 1.0;
        body = {size, size, 1.2, false, 0.12f, 0.12f, 0.13f};
    } else if (type == "TO263") {
        // D²PAK: the leads on one side (1.7 mm pitch) and the big tab soldered down opposite them.
        const int leads = n;
        const double pitch = pitchIn > 0 ? pitchIn : (leads > 3 ? 1.7 : 2.54);
        const double x0 = -(leads - 1) * pitch / 2;
        for (int i = 0; i < leads; ++i) pads.push_back({{x0 + i * pitch, 5.0}, {pitch * 0.6, 3.2}, false, false, 0});
        body = {10.2, 9.0, 4.4, false, 0.10f, 0.10f, 0.11f};
    } else if (type == "SOT223") {
        // Three leads at 2.3 mm on one side, the wide tab (pad 4) on the other.
        for (int i = 0; i < std::min(n, 3); ++i) pads.push_back({{(i - 1) * 2.3, 3.15}, {1.0, 2.0}, false, false, 0});
        body = {6.5, 3.5, 1.7, false, 0.12f, 0.12f, 0.13f};
    } else if (type == "MULTIWATT") {
        // Multiwatt / TO-220-15: leads at 1.27 mm, alternate leads bent to a second row 5.08 mm back (staggered).
        const double pitch = pitchIn > 0 ? pitchIn : 1.27, x0 = -(n - 1) * pitch / 2;
        for (int i = 0; i < n; ++i)
            pads.push_back({{x0 + i * pitch, i % 2 ? 2.54 : -2.54}, {1.6, 1.6}, true, i != 0, 1.0});
        body = {std::max(10.0, n * pitch + 1.5), 5.0, 17.5, false, 0.10f, 0.10f, 0.11f};
    } else if (type == "SON") {
        // Small-outline no-lead (VSON / TDSON power packages): two rows of short pads under the body edges.
        const double pitch = pitchIn > 0 ? pitchIn : 1.27, bw = bodyIn > 0 ? bodyIn : 5.0;
        int per = dual(pitch, bw / 2 - 0.3, {0.9, padWidth(pitch, 0.3, 0.65)}, false, 0);
        body = {bw, std::max(per * pitch + 0.8, bw * 1.2), 1.0, false, 0.12f, 0.12f, 0.13f};
    } else if (usesLandPattern(type) && exact) {
        // Exact land pattern: pad k is land k; the body is the declared outline, else the pads' extent.
        double xMax = 0, yMax = 0;
        for (int i = 0; i < n && i < static_cast<int>(exact->lands.size()); ++i) {
            const auto& l = exact->lands[i];
            pads.push_back({{l.x, l.y}, {l.w, l.h}, l.drill > 0, l.round, l.drill});
            xMax = std::max(xMax, std::fabs(l.x) + l.w / 2);
            yMax = std::max(yMax, std::fabs(l.y) + l.h / 2);
        }
        const double bw = bodyIn > 0 ? bodyIn : 2 * xMax, bd = exact->bodyDepth > 0 ? exact->bodyDepth : (bodyIn > 0 ? bodyIn : 2 * yMax);
        body = {bw, bd, 1.0, false, 0.14f, 0.14f, 0.15f};
    } else {  // SOIC
        double pitch = pitchIn > 0 ? pitchIn : 1.27;
        double bw = bodyIn > 0 ? bodyIn : (n > 16 ? 7.5 : 3.9);
        double row = bodyIn > 0 ? bw / 2 + 0.75 : (n > 16 ? 4.65 : 2.7);
        int per = dual(pitch, row, {1.55, padWidth(pitch, 0.25, 0.6)}, false, 0);
        body = {bw, per * pitch + 0.6, 1.5, false, 0.12f, 0.12f, 0.13f};
    }
    // Courtyard: union of pads and body plus 0.25 mm.
    double xMax = body.width / 2, yMax = body.depth / 2;
    for (const auto& p : pads) {
        xMax = std::max(xMax, std::fabs(p.offset.x) + p.size.x / 2);
        yMax = std::max(yMax, std::fabs(p.offset.y) + p.size.y / 2);
    }
    courtW = 2 * xMax + 0.5;
    courtH = 2 * yMax + 0.5;
    return pads;
}

int numericOrder(const std::string& number) { return isNumber(number) ? std::stoi(number) : 100000; }
}  // namespace

std::shared_ptr<const CustomPart> CustomPartRegistry::registerPart(const CustomPartSpec& specIn) {
    CustomPartSpec spec = specIn;
    if (spec.name.empty()) throw JsonError("The component needs a name.");
    if (spec.pins.empty()) throw JsonError("The component needs at least one pin.");
    if (spec.pins.size() > 512) throw JsonError("Components are limited to 512 pins.");
    std::set<std::string> numbers;
    int maxNumber = 0;
    for (const auto& p : spec.pins) {
        if (!numbers.insert(upper(p.number)).second) throw JsonError("Duplicate pin number " + p.number + ".");
        if (isNumber(p.number)) maxNumber = std::max(maxNumber, std::stoi(p.number));
    }
    const auto packages = supportedPackages();
    if (std::find(packages.begin(), packages.end(), spec.package.type) == packages.end())
        throw JsonError("Unsupported package '" + spec.package.type + "'.");
    int numeric = static_cast<int>(std::count_if(spec.pins.begin(), spec.pins.end(), [](const CustomPin& p) { return isNumber(p.number); }));
    if (spec.package.pinCount <= 0) spec.package.pinCount = std::max(maxNumber, numeric);
    spec.package.pinCount = std::max(spec.package.pinCount, maxNumber);
    int& pc = spec.package.pinCount;
    if ((spec.package.type == "SOIC" || spec.package.type == "TSSOP" || spec.package.type == "DIP" ||
         spec.package.type == "HEADER2" || spec.package.type == "SON") && pc % 2)
        ++pc;
    if ((spec.package.type == "QFN" || spec.package.type == "LQFP") && pc % 4) pc += 4 - pc % 4;
    if (spec.package.type == "SOT23" && pc > 6) throw JsonError("SOT-23 packages have at most 6 pins.");
    if (spec.package.type == "TO220" && pc > 7) throw JsonError("TO-220 packages have at most 7 pins.");
    if (spec.package.type == "HC49" && pc != 2) throw JsonError("HC-49 crystals have 2 pins.");
    if (spec.package.type == "XTAL3225" && pc != 4) throw JsonError("3225 crystals have 4 pads.");
    if ((spec.package.type == "XTAL3215" || spec.package.type == "XTALCYL") && pc != 2)
        throw JsonError("This crystal package has 2 pins.");
    if (spec.package.type == "DISC" && pc != 2) throw JsonError("Radial disc parts have 2 pins.");
    if (spec.package.type == "SOT223" && pc > 4) throw JsonError("SOT-223 packages have 3 leads and a tab.");
    if (spec.package.type == "TO263" && pc > 8) throw JsonError("TO-263 packages have at most 7 leads and a tab.");
    if (usesLandPattern(spec.package.type)) {
        if (spec.package.lands.empty())
            throw JsonError(spec.package.type + " packages need their land pattern (one pad per pin).");
        std::set<std::string> covered;
        for (size_t i = 0; i < spec.package.lands.size(); ++i) covered.insert(landPin(spec.package, i));
        for (const auto& p : spec.pins)
            if (!covered.count(upper(p.number))) throw JsonError("Pin " + p.number + " has no pad in the land pattern.");
        pc = static_cast<int>(spec.package.lands.size());
    }
    if (pc > 512) throw JsonError("Package pin count is too large.");

    auto part = std::make_shared<CustomPart>();
    part->spec = spec;
    part->id = makeId(spec);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = parts_.find(part->id);
        if (it != parts_.end()) return it->second;
    }

    // --- Schematic symbol: the part's symbol layout, or the generated one (pins in number order down the left, then
    // up the right). Each pin leaves the body from its side; pins 2 grid units apart, all on the 10-unit grid.
    const size_t n = spec.pins.size();
    std::vector<SymbolPin> placement(n);
    if (spec.symbol.empty()) {
        std::vector<size_t> order(n);
        for (size_t i = 0; i < n; ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return numericOrder(spec.pins[a].number) < numericOrder(spec.pins[b].number);
        });
        const size_t leftCount = (n + 1) / 2;
        for (size_t k = 0; k < n; ++k) {
            const bool left = k < leftCount;
            placement[order[k]] = {spec.pins[order[k]].number, left ? 'L' : 'R', static_cast<int>(left ? k : n - 1 - k)};
        }
    } else {
        for (const auto& issue : checkSymbol(spec))
            if (issue.severity == "error") throw JsonError("Symbol: " + issue.message);
        for (const auto& sp : spec.symbol.pins) placement[static_cast<size_t>(spec.pinIndex(sp.number))] = sp;
    }
    int slots[4] = {0, 0, 0, 0};  // L R T B: slot count (highest slot + 1)
    size_t nameLR = 0, nameTB = 0;
    auto sideIndex = [](char side) { return side == 'L' ? 0 : side == 'R' ? 1 : side == 'T' ? 2 : 3; };
    for (size_t i = 0; i < n; ++i) {
        const int k = sideIndex(placement[i].side);
        slots[k] = std::max(slots[k], placement[i].slot + 1);
        (k < 2 ? nameLR : nameTB) = std::max(k < 2 ? nameLR : nameTB, spec.pins[i].name.size());
    }
    double halfW = std::clamp(std::ceil((static_cast<double>(nameLR) * 3.6 + 14) / 10) * 10, 30.0, 400.0);
    halfW = std::max(halfW, static_cast<double>(std::max(slots[2], slots[3])) * 10 + 10);  // room for top / bottom pins
    if (spec.symbol.width > 0) halfW = std::max(halfW, std::ceil(spec.symbol.width / 20) * 10);
    const int rows = std::max(1, std::max(slots[0], slots[1]));
    // Top and bottom pin names read vertically inside the body: leave them that much room above / below the side pins.
    const double vertical = nameTB > 0 ? std::ceil((static_cast<double>(nameTB) * 3.6 + 6) / 10) * 10 : 0;
    const double halfH = static_cast<double>(rows - 1) * 10 + 10 + std::max(10.0, vertical);
    part->symbolHalfWidth = halfW;
    part->symbolHalfHeight = halfH;

    ComponentDef def;
    def.kind = ComponentKind::Custom;
    def.name = spec.name;
    def.refPrefix = spec.refPrefix;
    def.defaultValue = spec.defaultValue.empty() ? spec.name : spec.defaultValue;
    def.unit = "";
    def.footprint = "CUSTOM:" + part->id;
    def.simulated = false;
    def.pins.resize(n);
    const double top = -static_cast<double>(rows - 1) * 10;
    for (size_t i = 0; i < n; ++i) {
        const SymbolPin& sp = placement[i];
        PinDef pd;
        pd.name = spec.pins[i].name;
        pd.number = spec.pins[i].number;
        pd.type = static_cast<int>(spec.pins[i].type);
        pd.side = sp.side;
        const double along = static_cast<double>(sp.slot) * 20;
        switch (sp.side) {
            case 'L': pd.offset = {-(halfW + 20), top + along}; break;
            case 'R': pd.offset = {halfW + 20, top + along}; break;
            case 'T': pd.offset = {-static_cast<double>(slots[2] - 1) * 10 + along, -(halfH + 20)}; break;
            default: pd.offset = {-static_cast<double>(slots[3] - 1) * 10 + along, halfH + 20}; break;
        }
        def.pins[i] = pd;  // pin index == position in the spec's pin list
    }
    part->def = def;

    // --- Footprint.
    FootprintDef fp;
    fp.name = def.footprint;
    fp.label = spec.package.type + "-" + std::to_string(pc);
    if (spec.package.pitch > 0) {
        char pitch[32];
        std::snprintf(pitch, sizeof pitch, " %gmm pitch", spec.package.pitch);
        fp.label += pitch;
    }
    // Packages with a soldered tab (TO-263, SOT-223): the leads are the numbered pins; the tab is the pin numbered
    // past the last lead ("TAB" / n+1) or, as on most such parts, the middle lead's pin.
    const bool tabbed = spec.package.type == "TO263" || spec.package.type == "SOT223";
    int leadCount = pc;
    std::string tabNumber;  // the pin soldered to the tab, when the datasheet numbers it ("TAB" or past the leads)
    if (tabbed) {
        const int declared = specIn.package.pinCount;
        for (const auto& p : spec.pins)
            if (upper(p.number) == "TAB" || (declared > 0 && isNumber(p.number) && std::stoi(p.number) > declared))
                tabNumber = p.number;
        if (spec.package.type == "SOT223") leadCount = 3;
        else if (!tabNumber.empty() && declared > 0) leadCount = declared;
    }
    std::vector<PadPlacement> placements = packagePads(spec.package.type, tabbed ? leadCount : pc, spec.package.pitch,
                                                       spec.package.bodySize, fp.body, fp.courtyardW, fp.courtyardH,
                                                       &spec.package);
    // A depopulated or rectangular ball grid (DDR3 / DDR4 DRAM: 9 × 16 balls with columns 4–6 empty) is laid out
    // from the balls' own names; a full square grid keeps the row-major layout.
    std::vector<std::string> ballNumbers;
    if (spec.package.type == "BGA" && !tabbed && placements.size() == spec.pins.size()) {
        std::set<std::string> square;
        for (int i = 0; i < pc; ++i) square.insert(bgaBallName(i, pc));
        bool named = true, squareGrid = true;
        int rows = 0, cols = 0;
        std::vector<std::pair<int, int>> at;
        for (const auto& pin : spec.pins) {
            int r = 0, c = 0;
            if (!parseBallName(pin.number, r, c)) {
                named = false;
                break;
            }
            squareGrid = squareGrid && square.count(upper(pin.number));
            at.emplace_back(r, c);
            rows = std::max(rows, r + 1);
            cols = std::max(cols, c + 1);
        }
        if (named && !squareGrid) {
            const double pitch = spec.package.pitch > 0 ? spec.package.pitch : 0.8, ball = pitch * 0.45;
            for (size_t i = 0; i < placements.size(); ++i) {
                placements[i].offset = {(at[i].second - (cols - 1) / 2.0) * pitch, (at[i].first - (rows - 1) / 2.0) * pitch};
                placements[i].size = {ball, ball};
                ballNumbers.push_back(upper(spec.pins[i].number));
            }
            fp.body.width = spec.package.bodySize > 0 ? spec.package.bodySize : cols * pitch + 1.0;
            fp.body.depth = spec.package.bodyDepth > 0 ? spec.package.bodyDepth : rows * pitch + 1.0;
            fp.courtyardW = std::max(fp.body.width, cols * pitch) + 0.5;
            fp.courtyardH = std::max(fp.body.depth, rows * pitch) + 0.5;
        }
    }
    for (size_t i = 0; i < placements.size(); ++i) {
        PadDef pad;
        pad.offset = placements[i].offset;
        pad.size = placements[i].size;
        pad.throughHole = placements[i].tht;
        pad.round = placements[i].round;
        pad.drill = placements[i].drill;
        std::string number = !ballNumbers.empty()         ? ballNumbers[i]
                             : spec.package.type == "BGA" ? bgaBallName(static_cast<int>(i), pc)
                                                          : std::to_string(i + 1);
        if (usesLandPattern(spec.package.type)) number = landPin(spec.package, i);
        for (size_t p = 0; p < spec.pins.size(); ++p)
            if (upper(spec.pins[p].number) == number) pad.pinIndex = static_cast<int>(p);
        fp.pads.push_back(pad);
    }
    if (tabbed && !fp.pads.empty()) {
        PadDef tab;
        tab.pinIndex = fp.pads[fp.pads.size() / 2].pinIndex;  // middle lead (TabPin2 / TabPin3)
        for (size_t p = 0; p < spec.pins.size(); ++p)
            if (!tabNumber.empty() && spec.pins[p].number == tabNumber) tab.pinIndex = static_cast<int>(p);
        const bool d2pak = spec.package.type == "TO263";
        tab.size = d2pak ? Vec2{10.8, 9.4} : Vec2{3.6, 2.0};
        tab.offset = d2pak ? Vec2{0, -1.9} : Vec2{0, -3.15};
        fp.pads.push_back(tab);
        fp.courtyardH = std::max(fp.courtyardH, 2 * (std::fabs(tab.offset.y) + tab.size.y / 2) + 0.5);
        fp.courtyardW = std::max(fp.courtyardW, tab.size.x + 0.5);
    }
    // Exposed thermal pad for QFN and TSSOP (HTSSOP PowerPAD) when the datasheet lists one ("EP", "PAD", "TAB",
    // "THERMAL").
    if (spec.package.type == "QFN" || spec.package.type == "TSSOP" || spec.package.type == "MODULE" ||
        spec.package.type == "LQFP" || spec.package.type == "SON") {
        for (size_t p = 0; p < spec.pins.size(); ++p) {
            std::string u = upper(spec.pins[p].number);
            if (u == "EP" || u == "PAD" || u == "TAB" || u == "THERMAL" || u == std::to_string(pc + 1)) {
                PadDef ep;
                ep.pinIndex = static_cast<int>(p);
                double s = fp.body.width * 0.55;
                ep.size = {s, s};
                if (spec.package.type == "TSSOP") ep.size = {fp.body.width * 0.55, std::max(1.0, fp.body.depth - 2.0)};
                if (spec.package.type == "SON") ep.size = {fp.body.width * 0.6, fp.body.depth * 0.7};
                if (spec.package.type == "MODULE") {  // ground paddle under the shield, below the antenna end
                    ep.size = {fp.body.width * 0.28, fp.body.width * 0.28};
                    ep.offset = {0, fp.body.depth * 0.1};
                }
                fp.pads.push_back(ep);
                break;
            }
        }
    }
    part->footprint = fp;

    std::lock_guard<std::mutex> lock(mutex_);
    auto [it, inserted] = parts_.emplace(part->id, part);
    return it->second;
}

namespace {
bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

/// Ground / negative-supply pin names: GND, AGND, DGND, PGND, VSS, AVSS, VEE, V-, 0V, GND/EP, EP (on a ground part).
bool groundPinName(const std::string& upperName) {
    for (const char* p : {"GND", "AGND", "DGND", "PGND", "SGND", "CGND", "VSS", "AVSS", "DVSS", "VEE", "V-", "0V", "-VS"})
        if (startsWith(upperName, p)) return true;
    return false;
}

/// Reset, clock, boot and debug pins: kept together at the top left of an MCU symbol.
bool controlPinName(const std::string& u) {
    for (const char* p : {"RST", "RESET", "NRST", "MCLR", "XTAL", "OSC", "XIN", "XOUT", "XI", "XO", "CLKIN", "CLKOUT",
                          "BOOT", "SWD", "SWCLK", "SWDIO", "SWO", "TCK", "TMS", "TDI", "TDO", "NTRST", "JTAG", "UPDI",
                          "PDI", "TEST", "PROG", "EN", "CHIP_EN"})
        if (u == p || (startsWith(u, p) && std::strlen(p) >= 3)) return true;
    return false;
}

/// MCU port of a pin name and its bit: "PA5" → ("PA", 5), "P1.3" / "P1_3" → ("P1", 3), "GPIO12" → ("GPIO", 12),
/// "RB7" → ("RB", 7), "PA5/ADC1_5" → ("PA", 5). Empty port when the name is not a port pin.
std::pair<std::string, int> portOf(const std::string& u) {
    std::string base = u.substr(0, u.find_first_of("/ ("));
    auto digitsAt = [&](size_t i) {
        size_t j = i;
        while (j < base.size() && std::isdigit(static_cast<unsigned char>(base[j]))) ++j;
        return j;
    };
    if (base.size() >= 3 && (base[0] == 'P' || base[0] == 'R') && std::isalpha(static_cast<unsigned char>(base[1])) &&
        std::isdigit(static_cast<unsigned char>(base[2])) && digitsAt(2) == base.size())
        return {base.substr(0, 2), std::stoi(base.substr(2))};
    if (base.size() >= 4 && base[0] == 'P' && std::isdigit(static_cast<unsigned char>(base[1]))) {
        size_t j = digitsAt(1);
        if (j < base.size() && (base[j] == '.' || base[j] == '_') && j + 1 < base.size() && digitsAt(j + 1) == base.size())
            return {base.substr(0, j), std::stoi(base.substr(j + 1))};
    }
    if (startsWith(base, "GPIO") && base.size() > 4 && digitsAt(4) == base.size()) return {"GPIO", std::stoi(base.substr(4))};
    if (startsWith(base, "IO") && base.size() > 2 && digitsAt(2) == base.size()) return {"IO", std::stoi(base.substr(2))};
    return {"", 0};
}
}  // namespace

SymbolSpec autoArrangeSymbol(const CustomPartSpec& spec, bool stackDuplicates) {
    // Groups per side, each a list of pin indices; groups are separated by an empty slot.
    struct Group {
        std::vector<size_t> pins;
    };
    std::vector<Group> left, right, top, bottom;
    Group control, inputs, outputs, nc, supplies, grounds, other;
    std::map<std::string, Group> ports;
    std::vector<std::string> portOrder;
    for (size_t i = 0; i < spec.pins.size(); ++i) {
        const CustomPin& p = spec.pins[i];
        const std::string u = upper(p.name);
        const auto [port, bit] = portOf(u);
        (void)bit;
        if (p.type == PinType::NoConnect) nc.pins.push_back(i);
        else if (groundPinName(u)) grounds.pins.push_back(i);
        else if (p.type == PinType::PowerIn) supplies.pins.push_back(i);
        else if (controlPinName(u)) control.pins.push_back(i);
        else if (!port.empty() && (p.type == PinType::Bidirectional || p.type == PinType::Passive ||
                                   p.type == PinType::Input || p.type == PinType::Output)) {
            if (!ports.count(port)) portOrder.push_back(port);
            ports[port].pins.push_back(i);
        } else if (p.type == PinType::Input) inputs.pins.push_back(i);
        else if (p.type == PinType::Output || p.type == PinType::OpenCollector || p.type == PinType::PowerOut)
            outputs.pins.push_back(i);
        else other.pins.push_back(i);
    }
    // Port pins in bit order.
    for (auto& [name, g] : ports)
        std::stable_sort(g.pins.begin(), g.pins.end(), [&](size_t a, size_t b) {
            return portOf(upper(spec.pins[a].name)).second < portOf(upper(spec.pins[b].name)).second;
        });
    auto count = [](const std::vector<Group>& side) {
        size_t n = 0;
        for (const auto& g : side) n += g.pins.size() + 1;
        return n;
    };
    if (!control.pins.empty()) left.push_back(control);
    if (!inputs.pins.empty()) left.push_back(inputs);
    if (!outputs.pins.empty()) right.push_back(outputs);
    // Ports and the remaining bidirectional / passive pins balance the two sides (each goes to the shorter side).
    // A long port is cut into groups of 8 bits (PA0–7, PA8–15) so both sides stay balanced.
    for (const auto& name : portOrder) {
        const auto& all = ports[name].pins;
        for (size_t k = 0; k < all.size(); k += 8) {
            Group chunk;
            chunk.pins.assign(all.begin() + static_cast<long>(k), all.begin() + static_cast<long>(std::min(all.size(), k + 8)));
            (count(left) <= count(right) ? left : right).push_back(chunk);
        }
    }
    if (!other.pins.empty()) {
        // Split a plain bidirectional / passive group across both sides (e.g. an 8-pin passive part).
        Group a, b;
        for (size_t k = 0; k < other.pins.size(); ++k) (k < (other.pins.size() + 1) / 2 ? a : b).pins.push_back(other.pins[k]);
        (count(left) <= count(right) ? left : right).push_back(a);
        if (!b.pins.empty()) (count(left) <= count(right) ? left : right).push_back(b);
    }
    if (!nc.pins.empty()) right.push_back(nc);
    if (!supplies.pins.empty()) top.push_back(supplies);
    if (!grounds.pins.empty()) bottom.push_back(grounds);

    SymbolSpec out;
    auto place = [&](const std::vector<Group>& groups, char side, bool stack) {
        int slot = 0;
        std::map<std::string, int> stacked;  // name → slot of its first pin
        for (size_t g = 0; g < groups.size(); ++g) {
            if (g > 0) ++slot;  // gap between groups
            for (size_t i : groups[g].pins) {
                const std::string& name = spec.pins[i].name;
                if (stack) {
                    auto it = stacked.find(name);
                    if (it != stacked.end()) {
                        out.pins.push_back({spec.pins[i].number, side, it->second});
                        continue;
                    }
                    stacked[name] = slot;
                }
                out.pins.push_back({spec.pins[i].number, side, slot++});
            }
        }
    };
    place(left, 'L', false);
    place(right, 'R', false);
    place(top, 'T', stackDuplicates);
    place(bottom, 'B', stackDuplicates);
    return out;
}

std::vector<SymbolIssue> checkSymbol(const CustomPartSpec& spec) {
    std::vector<SymbolIssue> out;
    if (spec.symbol.empty()) return out;
    std::map<std::string, size_t> byNumber;  // upper-case pin number → index in the pin list
    for (size_t i = 0; i < spec.pins.size(); ++i) byNumber[upper(spec.pins[i].number)] = i;
    std::map<std::string, int> placed;
    std::map<std::pair<char, int>, std::vector<size_t>> spots;  // (side, slot) → pin indices
    for (const auto& sp : spec.symbol.pins) {
        const std::string key = upper(sp.number);
        auto it = byNumber.find(key);
        if (it == byNumber.end()) {
            out.push_back({"error", "SYM_UNKNOWN", "Symbol places pin " + sp.number + ", which the part does not have.",
                           {sp.number}});
            continue;
        }
        if (placed[key]++ == 1)
            out.push_back({"error", "SYM_DUPLICATE", "Pin " + sp.number + " is placed more than once.", {sp.number}});
        spots[{sp.side, sp.slot}].push_back(it->second);
    }
    for (const auto& p : spec.pins)
        if (!placed.count(upper(p.number)))
            out.push_back({"error", "SYM_MISSING", "Pin " + p.number + " (" + p.name + ") is not on the symbol.", {p.number}});
    for (const auto& [spot, pins] : spots) {
        if (pins.size() < 2) continue;
        std::vector<std::string> numbers;
        bool sameName = true, power = true;
        for (size_t i : pins) {
            numbers.push_back(spec.pins[i].number);
            sameName &= spec.pins[i].name == spec.pins[pins[0]].name;
            const PinType t = spec.pins[i].type;
            power &= t == PinType::PowerIn || t == PinType::PowerOut || groundPinName(upper(spec.pins[i].name));
        }
        std::string list;
        for (const auto& num : numbers) list += (list.empty() ? "" : ", ") + num;
        if (!sameName) {
            out.push_back({"error", "SYM_OVERLAP", "Pins " + list + " have different names but sit on the same spot.", numbers});
        } else if (!power) {
            out.push_back({"warning", "SYM_STACK_SIGNAL",
                           "Pins " + list + " (" + spec.pins[pins[0]].name +
                               ") are stacked: they will be joined into one net. Stack only pins that are connected "
                               "inside the part.",
                           numbers});
        } else {
            out.push_back({"info", "SYM_STACK",
                           "Pins " + list + " (" + spec.pins[pins[0]].name + ") are stacked and join one net.", numbers});
        }
    }
    return out;
}

CustomPartSpec landPatternFromFootprint(const CustomPartSpec& spec) {
    if (usesLandPattern(spec.package.type) && !spec.package.lands.empty()) return spec;
    auto part = CustomPartRegistry::instance().registerPart(spec);
    CustomPartSpec out = part->spec;  // normalised (pin count, package type)
    out.package.type = "CUSTOM";
    out.package.pitch = 0;
    out.package.bodySize = part->footprint.body.width;
    out.package.bodyDepth = part->footprint.body.depth;
    out.package.lands.clear();
    const auto& pads = part->footprint.pads;
    for (size_t k = 0; k < pads.size(); ++k) {
        const PadDef& pad = pads[k];
        PackageSpec::Land l{pad.offset.x, pad.offset.y, pad.size.x, pad.size.y, pad.throughHole ? pad.drill : 0,
                            pad.round, ""};
        if (pad.pinIndex < 0) {
            l.pin = "-";
        } else {
            const std::string& number = part->def.pins[static_cast<size_t>(pad.pinIndex)].number;
            if (upper(number) != std::to_string(k + 1)) l.pin = number;
        }
        out.package.lands.push_back(l);
    }
    out.package.pinCount = static_cast<int>(out.package.lands.size());
    return out;
}

std::vector<LandIssue> checkLandPattern(const CustomPartSpec& spec, double minGap) {
    std::vector<LandIssue> out;
    const auto& lands = spec.package.lands;
    std::set<std::string> pinNumbers;
    for (const auto& p : spec.pins) pinNumbers.insert(upper(p.number));
    std::set<std::string> covered;
    for (size_t i = 0; i < lands.size(); ++i) {
        const std::string pin = landPin(spec.package, i);
        covered.insert(pin);
        const int n = static_cast<int>(i) + 1;
        if (pin != "-" && !pinNumbers.count(pin))
            out.push_back({"warning", "LAND_NO_PIN", "Pad " + std::to_string(n) + " belongs to no pin (pin " + pin +
                                                     " is not in the pin list); mark it \"-\" if it is mechanical.", {n}});
        const auto& l = lands[i];
        if (l.drill > 0 && (std::min(l.w, l.h) - l.drill) / 2 < 0.1 - 1e-9)
            out.push_back({"warning", "LAND_ANNULAR",
                           "Pad " + std::to_string(n) + ": annular ring below 0.1 mm (grow the pad or shrink the drill).", {n}});
    }
    for (const auto& p : spec.pins)
        if (!covered.count(upper(p.number)))
            out.push_back({"error", "LAND_NO_PAD", "Pin " + p.number + " (" + p.name + ") has no pad.", {}});
    auto gap = [](const PackageSpec::Land& a, const PackageSpec::Land& b) {
        if (a.round && b.round && std::fabs(a.w - a.h) < 1e-9 && std::fabs(b.w - b.h) < 1e-9)
            return std::hypot(a.x - b.x, a.y - b.y) - (a.w + b.w) / 2;
        const double dx = std::fabs(a.x - b.x) - (a.w + b.w) / 2, dy = std::fabs(a.y - b.y) - (a.h + b.h) / 2;
        if (dx < 0 && dy < 0) return std::max(dx, dy);  // overlapping: negative
        return std::hypot(std::max(dx, 0.0), std::max(dy, 0.0));
    };
    for (size_t i = 0; i < lands.size(); ++i)
        for (size_t j = i + 1; j < lands.size(); ++j) {
            const std::string pi = landPin(spec.package, i), pj = landPin(spec.package, j);
            if (pi == pj && pi != "-") continue;  // pads of one pin may touch (split exposed pad, tab)
            const double g = gap(lands[i], lands[j]);
            const int a = static_cast<int>(i) + 1, b = static_cast<int>(j) + 1;
            char text[160];
            if (g < 0) {
                std::snprintf(text, sizeof text, "Pads %d and %d overlap.", a, b);
                out.push_back({"error", "LAND_OVERLAP", text, {a, b}});
            } else if (g < minGap - 1e-9) {
                std::snprintf(text, sizeof text, "Pads %d and %d are %.3f mm apart (minimum %.2f mm).", a, b, g, minGap);
                out.push_back({"warning", "LAND_GAP", text, {a, b}});
            }
        }
    return out;
}

CustomPartRegistry& CustomPartRegistry::instance() {
    static CustomPartRegistry registry;
    return registry;
}

std::shared_ptr<const CustomPart> CustomPartRegistry::get(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = parts_.find(id);
    return it == parts_.end() ? nullptr : it->second;
}

const CustomPart* CustomPartRegistry::find(const std::string& id) const { return get(id).get(); }

Json customPartToJson(const CustomPart& part) {
    Json j = customPartSpecToJson(part.spec);
    j["id"] = part.id;
    j["footprint"] = part.footprint.label;
    Json symbol = Json::object();
    symbol["halfWidth"] = part.symbolHalfWidth;
    symbol["halfHeight"] = part.symbolHalfHeight;
    Json pins = Json::array();
    for (const auto& p : part.def.pins) {
        Json pj = Json::object();
        pj["name"] = p.name;
        pj["number"] = p.number;
        pj["type"] = pinTypeName(static_cast<PinType>(p.type));
        pj["x"] = p.offset.x;
        pj["y"] = p.offset.y;
        pj["side"] = std::string(1, p.side ? p.side : (p.offset.x < 0 ? 'L' : 'R'));
        pins.push(pj);
    }
    symbol["pins"] = pins;
    j["symbol"] = symbol;
    Json pads = Json::array();
    int number = 1;
    for (const auto& p : part.footprint.pads) {
        Json pj = Json::object();
        pj["number"] = number++;
        pj["pin"] = p.pinIndex;
        pj["x"] = p.offset.x;
        pj["y"] = p.offset.y;
        pj["w"] = p.size.x;
        pj["h"] = p.size.y;
        pj["throughHole"] = p.throughHole;
        pj["round"] = p.round;
        pads.push(pj);
    }
    Json fpj = Json::object();
    fpj["label"] = part.footprint.label;
    fpj["pads"] = pads;
    fpj["courtyardW"] = part.footprint.courtyardW;
    fpj["courtyardH"] = part.footprint.courtyardH;
    fpj["bodyW"] = part.footprint.body.width;
    fpj["bodyD"] = part.footprint.body.depth;
    fpj["bodyH"] = part.footprint.body.height;
    j["footprintGeometry"] = fpj;
    return j;
}

}  // namespace sieda
