#include "sieda/CustomParts.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
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
    return {"SOIC", "TSSOP", "DIP", "QFN", "LQFP", "SOT23", "HEADER", "HEADER2", "TO220"};
}

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
    if (has("TSSOP") || has("MSOP") || has("SSOP")) type = "TSSOP";
    else if (has("SOIC") || has("SOP") || has("SO-") || u == "SO") type = "SOIC";
    else if (has("DIP")) type = "DIP";
    else if (has("QFN") || has("DFN") || has("MLF")) type = "QFN";
    else if (has("QFP")) type = "LQFP";
    else if (has("SOT")) type = "SOT23";
    else if (has("TO220") || has("TO-220") || has("TO-92") || has("TO92")) type = "TO220";
    else if (has("HEADER2") || has("2X") || has("IDC") || has("BOX HEADER") || has("DUAL ROW")) type = "HEADER2";
    else if (has("HEADER") || has("SIP") || has("CONN") || has("1X")) type = "HEADER";
    if (!digits.empty() && type != "TO220") {
        int n = std::stoi(digits.size() > 3 ? digits.substr(digits.size() - 3) : digits);
        if (type == "SOT23" && n == 23) n = 0;  // "SOT-23" alone means 3 pins
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
    const Json& m = j.get("model");
    if (m.isObject()) {
        const Json& rj = m.get("regulator");
        if (rj.isObject()) {
            RegulatorModel& r = s.model.regulator;
            r.in = trim(rj.get("in").asString(""));
            r.out = trim(rj.get("out").asString(""));
            r.ref = trim(rj.get("ref").asString(""));
            r.vout = rj.get("vout").asNumber(0);
            r.dropout = std::max(0.0, rj.get("dropout").asNumber(0.3));
            r.iq = std::max(0.0, rj.get("iq").asNumber(0));
            r.ilimit = std::max(1e-6, rj.get("ilimit").asNumber(1.0));
            r.maxPower = std::max(1e-3, rj.get("maxPower").asNumber(0.5));
            r.charger = rj.get("charger").asBool(false);
            s.model.hasRegulator = true;
            if (s.pinIndex(r.in) < 0 || s.pinIndex(r.out) < 0 || s.pinIndex(r.ref) < 0)
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

struct PadPlacement {
    Vec2 offset, size;
    bool tht = false, round = false;
    double drill = 0;
};

/// Pad positions for pad numbers 1..n of a package (index 0 = pad 1).
std::vector<PadPlacement> packagePads(const std::string& type, int n, double pitchIn, double bodyIn, BodyDef& body,
                                      double& courtW, double& courtH) {
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
        double x0 = -(n - 1) * 2.54 / 2;
        for (int i = 0; i < n; ++i) pads.push_back({{x0 + i * 2.54, 0}, {1.9, 2.5}, true, false, 1.2});
        body = {10.0, 4.5, 15.0, false, 0.10f, 0.10f, 0.11f};
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
    if (spec.pins.size() > 256) throw JsonError("Components are limited to 256 pins.");
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
         spec.package.type == "HEADER2") && pc % 2)
        ++pc;
    if ((spec.package.type == "QFN" || spec.package.type == "LQFP") && pc % 4) pc += 4 - pc % 4;
    if (spec.package.type == "SOT23" && pc > 6) throw JsonError("SOT-23 packages have at most 6 pins.");
    if (spec.package.type == "TO220" && pc > 7) throw JsonError("TO-220 packages have at most 7 pins.");
    if (pc > 256) throw JsonError("Package pin count is too large.");

    auto part = std::make_shared<CustomPart>();
    part->spec = spec;
    part->id = makeId(spec);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = parts_.find(part->id);
        if (it != parts_.end()) return it->second;
    }

    // --- Schematic symbol: DIP-style box, pins ordered by number down the left then up the right.
    std::vector<size_t> order(spec.pins.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return numericOrder(spec.pins[a].number) < numericOrder(spec.pins[b].number);
    });
    size_t n = order.size();
    size_t leftCount = (n + 1) / 2, rightCount = n - leftCount;
    size_t maxName = 0;
    for (const auto& p : spec.pins) maxName = std::max(maxName, p.name.size());
    double halfW = std::clamp(std::ceil((static_cast<double>(maxName) * 3.6 + 14) / 10) * 10, 30.0, 120.0);
    size_t rows = std::max<size_t>(leftCount, std::max<size_t>(rightCount, 1));
    double halfH = static_cast<double>(rows) * 10 + 10;
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
    double top = -static_cast<double>(rows - 1) * 10;
    for (size_t k = 0; k < n; ++k) {
        size_t pinIdx = order[k];
        bool left = k < leftCount;
        size_t row = left ? k : (n - 1 - k);
        PinDef pd;
        pd.name = spec.pins[pinIdx].name;
        pd.number = spec.pins[pinIdx].number;
        pd.type = static_cast<int>(spec.pins[pinIdx].type);
        pd.offset = {left ? -(halfW + 20) : (halfW + 20), top + static_cast<double>(row) * 20};
        def.pins[pinIdx] = pd;  // pin index == position in the spec's pin list
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
    std::vector<PadPlacement> placements = packagePads(spec.package.type, pc, spec.package.pitch, spec.package.bodySize,
                                                       fp.body, fp.courtyardW, fp.courtyardH);
    for (size_t i = 0; i < placements.size(); ++i) {
        PadDef pad;
        pad.offset = placements[i].offset;
        pad.size = placements[i].size;
        pad.throughHole = placements[i].tht;
        pad.round = placements[i].round;
        pad.drill = placements[i].drill;
        std::string number = std::to_string(i + 1);
        for (size_t p = 0; p < spec.pins.size(); ++p)
            if (spec.pins[p].number == number) pad.pinIndex = static_cast<int>(p);
        fp.pads.push_back(pad);
    }
    // Exposed thermal pad for QFN when the datasheet lists one ("EP", "PAD", "TAB", "THERMAL").
    if (spec.package.type == "QFN") {
        for (size_t p = 0; p < spec.pins.size(); ++p) {
            std::string u = upper(spec.pins[p].number);
            if (u == "EP" || u == "PAD" || u == "TAB" || u == "THERMAL" || u == std::to_string(pc + 1)) {
                PadDef ep;
                ep.pinIndex = static_cast<int>(p);
                double s = fp.body.width * 0.55;
                ep.size = {s, s};
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
