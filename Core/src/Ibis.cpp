#include "sieda/Ibis.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <sstream>

#include "sieda/Units.hpp"

namespace sieda {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

/// Keyword names compare case-insensitively with '_' and ' ' equivalent (IBIS §3).
std::string keyword(const std::string& raw) {
    std::string out;
    for (char c : lower(trim(raw))) {
        char ch = c == '_' ? ' ' : c;
        if (ch == ' ' && (out.empty() || out.back() == ' ')) continue;
        out += ch;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::vector<std::string> tokens(const std::string& line) {
    std::vector<std::string> out;
    std::istringstream in(line);
    std::string t;
    while (in >> t) out.push_back(t);
    return out;
}

/// IBIS number: optional scale suffix (T G M k m u n p f), trailing units ignored, "NA" = missing.
double number(const std::string& token) {
    std::string t = trim(token);
    if (t.empty() || lower(t) == "na") return kNaN;
    auto v = parseEngineeringValue(t);
    return v ? *v : kNaN;
}

IbisTriple triple(const std::vector<std::string>& t, size_t first) {
    IbisTriple r;
    if (t.size() > first) r.typ = number(t[first]);
    if (t.size() > first + 1) r.min = number(t[first + 1]);
    if (t.size() > first + 2) r.max = number(t[first + 2]);
    return r;
}

/// "1.57/0.36n" → (1.57, 0.36e-9).
std::pair<double, double> ratio(const std::string& token) {
    size_t slash = token.find('/');
    if (slash == std::string::npos) return {kNaN, kNaN};
    return {number(token.substr(0, slash)), number(token.substr(slash + 1))};
}

/// "Vinl = 0.8V" / "Vinl 0.8" → ("vinl", "0.8V"), or empty name.
std::pair<std::string, std::string> assignment(const std::string& line) {
    std::string l = trim(line);
    size_t eq = l.find('=');
    if (eq != std::string::npos) return {keyword(l.substr(0, eq)), trim(l.substr(eq + 1))};
    auto t = tokens(l);
    if (t.size() >= 2) return {keyword(t[0]), trim(l.substr(t[0].size()))};
    return {"", ""};
}

}  // namespace

IbisTriple::IbisTriple() : typ(kNaN), min(kNaN), max(kNaN) {}

double IbisTriple::at(int corner) const {
    const double v[3] = {typ, min, max};
    if (corner >= 0 && corner < 3 && std::isfinite(v[corner])) return v[corner];
    for (double x : v)
        if (std::isfinite(x)) return x;
    return kNaN;
}

bool IbisTriple::valid() const { return std::isfinite(typ) || std::isfinite(min) || std::isfinite(max); }

IbisModel::IbisModel() : vinl(kNaN), vinh(kNaN) {}

bool IbisModel::canDrive() const {
    std::string t = lower(type);
    return t.find("output") != std::string::npos || t.find("i/o") != std::string::npos || t.find("3-state") != std::string::npos ||
           t.find("open") != std::string::npos;
}

IbisPin::IbisPin() : rPin(kNaN), lPin(kNaN), cPin(kNaN) {}

const IbisModel* IbisFile::findModel(const std::string& name) const {
    const std::string key = lower(name);
    for (const auto& m : models)
        if (lower(m.name) == key) return &m;
    for (const auto& [sel, list] : selectors)
        if (lower(sel) == key && !list.empty())
            for (const auto& m : models)
                if (lower(m.name) == lower(list.front())) return &m;
    return nullptr;
}

IbisFile parseIbis(const std::string& text) {
    IbisFile f;
    char comment = '|';
    std::string section;  // current keyword (normalised)
    IbisModel* model = nullptr;
    IbisComponent* component = nullptr;
    IbisWaveform* waveform = nullptr;
    std::string selector;
    std::vector<IbisIvPoint>* table = nullptr;
    std::istringstream in(text);
    std::string raw;
    int lineNo = 0;
    bool ended = false;
    while (!ended && std::getline(in, raw)) {
        ++lineNo;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        // [Comment Char] must be read before comments are stripped: "[Comment Char] #_char".
        std::string probe = trim(raw);
        if (!probe.empty() && probe[0] == '[') {
            size_t close = probe.find(']');
            if (close != std::string::npos && keyword(probe.substr(1, close - 1)) == "comment char") {
                std::string arg = trim(probe.substr(close + 1));
                if (!arg.empty()) comment = arg[0];
                continue;
            }
        }
        size_t c = raw.find(comment);
        std::string line = trim(c == std::string::npos ? raw : raw.substr(0, c));
        if (line.empty()) continue;
        if (line[0] == '[') {
            size_t close = line.find(']');
            if (close == std::string::npos) {
                f.warnings.push_back("Line " + std::to_string(lineNo) + ": unterminated keyword");
                continue;
            }
            section = keyword(line.substr(1, close - 1));
            const std::string arg = trim(line.substr(close + 1));
            table = nullptr;
            waveform = nullptr;
            if (section == "ibis ver") f.version = arg;
            else if (section == "file name") f.fileName = arg;
            else if (section == "end") ended = true;
            else if (section == "component") {
                f.components.emplace_back();
                component = &f.components.back();
                component->name = arg;
                model = nullptr;
            } else if (section == "manufacturer") {
                if (component) component->manufacturer = arg;
            } else if (section == "model selector") {
                selector = arg;
                f.selectors[selector];
            } else if (section == "model") {
                f.models.emplace_back();
                model = &f.models.back();
                model->name = arg;
                component = nullptr;
            } else if (model && (section == "pullup" || section == "pulldown" || section == "gnd clamp" || section == "power clamp")) {
                table = section == "pullup" ? &model->pullup
                        : section == "pulldown" ? &model->pulldown
                        : section == "gnd clamp" ? &model->gndClamp
                                                 : &model->powerClamp;
            } else if (model && (section == "rising waveform" || section == "falling waveform")) {
                auto& list = section == "rising waveform" ? model->rising : model->falling;
                list.emplace_back();
                waveform = &list.back();
            } else if (model && (section == "voltage range" || section == "pullup reference" || section == "pulldown reference")) {
                IbisTriple t = triple(tokens(arg), 0);
                (section == "voltage range" ? model->voltageRange
                 : section == "pullup reference" ? model->pullupReference
                                                 : model->pulldownReference) = t;
            } else if (model && section == "c comp") {
                model->cComp = triple(tokens(arg), 0);
            }
            continue;
        }
        const auto t = tokens(line);
        if (t.empty()) continue;
        if (section == "package" && component) {
            const std::string k = keyword(t[0]);
            if (k == "r pkg") component->rPkg = triple(t, 1);
            else if (k == "l pkg") component->lPkg = triple(t, 1);
            else if (k == "c pkg") component->cPkg = triple(t, 1);
        } else if (section == "pin" && component) {
            if (t.size() < 3) continue;
            IbisPin p;
            p.pin = t[0];
            p.signal = t[1];
            p.model = t[2];
            if (t.size() >= 6) {
                p.rPin = number(t[3]);
                p.lPin = number(t[4]);
                p.cPin = number(t[5]);
            }
            component->pins.push_back(p);
        } else if (section == "model selector") {
            f.selectors[selector].push_back(t[0]);
        } else if (section == "model" && model) {
            auto [k, v] = assignment(line);
            if (k == "model type") model->type = tokens(v).empty() ? v : tokens(v)[0];
            else if (k == "c comp") model->cComp = triple(tokens(v), 0);
            else if (k == "vinl") model->vinl = number(tokens(v).empty() ? v : tokens(v)[0]);
            else if (k == "vinh") model->vinh = number(tokens(v).empty() ? v : tokens(v)[0]);
        } else if ((section == "model spec" || section == "receiver thresholds") && model) {
            auto [k, v] = assignment(line);
            auto vt = tokens(v);
            double x = vt.empty() ? kNaN : number(vt[0]);
            if ((k == "vinh" || k == "vinh dc") && !std::isfinite(model->vinh)) model->vinh = x;
            if ((k == "vinl" || k == "vinl dc") && !std::isfinite(model->vinl)) model->vinl = x;
        } else if (section == "ramp" && model) {
            const std::string k = keyword(t[0]);
            if (k.rfind("r load", 0) == 0) {
                auto [n, v] = assignment(line);
                double r = n == "r load" ? number(v) : kNaN;
                if (std::isfinite(r) && r > 0) model->rLoad = r;
            } else if (k == "dv/dt r" || k == "dv/dt f") {
                IbisTriple dv, dt;
                double* dvs[3] = {&dv.typ, &dv.min, &dv.max};
                double* dts[3] = {&dt.typ, &dt.min, &dt.max};
                for (size_t i = 0; i < 3 && i + 1 < t.size(); ++i) {
                    auto [a, b] = ratio(t[i + 1]);
                    *dvs[i] = a;
                    *dts[i] = b;
                }
                if (k == "dv/dt r") {
                    model->rampRiseDv = dv;
                    model->rampRiseDt = dt;
                } else {
                    model->rampFallDv = dv;
                    model->rampFallDt = dt;
                }
            }
        } else if (table) {
            if (t.size() < 2) continue;
            double v = number(t[0]);
            if (!std::isfinite(v)) continue;
            IbisIvPoint p;
            p.v = v;
            p.i = triple(t, 1);
            table->push_back(p);
        } else if (waveform) {
            const std::string k = keyword(t[0]);
            if (k.rfind("r fixture", 0) == 0 || k.rfind("v fixture", 0) == 0) {
                auto [n, v] = assignment(line);
                double x = number(v);
                if (n == "r fixture" && std::isfinite(x)) waveform->rFixture = x;
                if (n == "v fixture" && std::isfinite(x)) waveform->vFixture = x;
                continue;
            }
            if (k.find('=') != std::string::npos || t.size() < 2) continue;  // C_fixture, L_dut, …
            double time = number(t[0]);
            if (!std::isfinite(time)) continue;
            waveform->points.push_back({time, triple(t, 1)});
        }
    }
    if (f.models.empty()) throw IbisError("Not an IBIS file: no [Model] keyword found");
    if (f.version.empty()) f.warnings.push_back("[IBIS Ver] missing");
    for (const auto& comp : f.components)
        for (const auto& p : comp.pins) {
            const std::string m = lower(p.model);
            if (m == "power" || m == "gnd" || m == "nc" || m == "nomodel") continue;
            if (!f.findModel(p.model)) f.warnings.push_back("Pin " + p.pin + " uses model " + p.model + ", which is not in the file");
        }
    return f;
}

namespace {

int cornerIndex(const std::string& corner) {
    const std::string c = lower(corner);
    return c == "min" || c == "slow" ? 1 : c == "max" || c == "fast" ? 2 : 0;
}

/// Least-squares resistance through the origin: R = Σv² / Σ(v·i) over 0 < v ≤ vMax (and i of the given sign).
double slopeResistance(const std::vector<IbisIvPoint>& table, int corner, double vMax, double sign) {
    double vv = 0, vi = 0;
    for (const auto& p : table) {
        if (p.v <= 1e-6 || p.v > vMax) continue;
        double i = sign * p.i.at(corner);
        if (!std::isfinite(i) || i <= 0) continue;
        vv += p.v * p.v;
        vi += p.v * i;
    }
    return vi > 0 ? vv / vi : 0;
}

/// 20 % → 80 % time of a waveform table (s), 0 when it cannot be measured.
double waveformEdge(const IbisWaveform& w, int corner) {
    if (w.points.size() < 3) return 0;
    const double v0 = w.points.front().second.at(corner), v1 = w.points.back().second.at(corner);
    if (!std::isfinite(v0) || !std::isfinite(v1) || std::fabs(v1 - v0) < 1e-6) return 0;
    auto crossing = [&](double frac) {
        const double level = v0 + frac * (v1 - v0);
        for (size_t k = 1; k < w.points.size(); ++k) {
            double a = w.points[k - 1].second.at(corner) - level, b = w.points[k].second.at(corner) - level;
            if ((a <= 0 && b >= 0) || (a >= 0 && b <= 0)) {
                double t0 = w.points[k - 1].first, t1 = w.points[k].first;
                return std::fabs(b - a) < 1e-15 ? t0 : t0 + (t1 - t0) * (-a) / (b - a);
            }
        }
        return kNaN;
    };
    double t20 = crossing(0.2), t80 = crossing(0.8);
    return std::isfinite(t20) && std::isfinite(t80) && t80 > t20 ? t80 - t20 : 0;
}

double firstFinite(std::initializer_list<double> values, double fallback) {
    for (double v : values)
        if (std::isfinite(v) && v > 0) return v;
    return fallback;
}

}  // namespace

DriverModel driverFromIbis(const IbisModel& m, const std::string& corner, const IbisComponent* comp, const IbisPin* pin) {
    const int k = cornerIndex(corner);
    DriverModel d;
    d.id = "ibis:" + m.name;
    d.name = m.name + (comp && !comp->name.empty() ? " (" + comp->name + ")" : "");
    d.source = "ibis";
    d.type = m.canDrive() ? (lower(m.type).find("i/o") != std::string::npos ? "io" : "output") : "input";
    d.vHigh = firstFinite({m.voltageRange.at(k), m.pullupReference.at(k)}, 3.3);
    d.cComp = firstFinite({m.cComp.at(k)}, 3e-12);
    d.cIn = d.cComp;
    // Edge: 10–90 % of a linear ramp is (0.8 / 0.6) × its 20–80 % time.
    const double kEdge = 0.8 / 0.6;
    double rise = m.rampRiseDt.at(k), fall = m.rampFallDt.at(k);
    if (!(rise > 0))
        for (const auto& w : m.rising)
            if (double e = waveformEdge(w, k); e > 0) {
                rise = e;
                break;
            }
    if (!(fall > 0))
        for (const auto& w : m.falling)
            if (double e = waveformEdge(w, k); e > 0) {
                fall = e;
                break;
            }
    std::vector<std::string> notes;
    if (rise > 0) d.riseTime = rise * kEdge;
    else if (d.canDrive()) notes.push_back("no [Ramp] or waveform: 1 ns edge assumed");
    d.fallTime = fall > 0 ? fall * kEdge : d.riseTime;
    // Output resistance near the rails ([Pullup] voltages are Vcc − Vpin; its current flows out of the pin).
    const double span = 0.3 * d.vHigh;
    const double rd = slopeResistance(m.pulldown, k, span, 1.0), ru = slopeResistance(m.pullup, k, span, -1.0);
    if (rd > 0 && ru > 0) d.rOut = 0.5 * (rd + ru);
    else if (rd > 0 || ru > 0) d.rOut = std::max(rd, ru);
    else if (d.canDrive()) notes.push_back("no usable I-V table: 30 Ω output assumed");
    d.vih = std::isfinite(m.vinh) ? m.vinh : 0.7 * d.vHigh;
    d.vil = std::isfinite(m.vinl) ? m.vinl : 0.3 * d.vHigh;
    if (comp) {
        d.rPkg = firstFinite({comp->rPkg.at(0)}, 0);
        d.lPkg = firstFinite({comp->lPkg.at(0)}, 0);
        d.cPkg = firstFinite({comp->cPkg.at(0)}, 0);
    }
    if (pin) {
        if (std::isfinite(pin->rPin)) d.rPkg = pin->rPin;
        if (std::isfinite(pin->lPin)) d.lPkg = pin->lPin;
        if (std::isfinite(pin->cPin)) d.cPkg = pin->cPin;
    }
    for (size_t i = 0; i < notes.size(); ++i) d.note += (i ? "; " : "") + notes[i];
    return d;
}

const std::vector<DriverModel>& logicFamilies() {
    // Typical values from the JEDEC interface standards (JESD8-x thresholds, JESD79-3/-4 drive and ODT) and common
    // datasheet edge rates. vHigh is the open-circuit source level; rTerm models on-die termination at a receiver.
    static const std::vector<DriverModel> families = [] {
        std::vector<DriverModel> v;
        auto add = [&](const char* id, const char* name, double vh, double tr, double rout, double cin, double vih,
                       double vil, double rterm, double vterm, const char* note) {
            DriverModel d;
            d.id = id;
            d.name = name;
            d.source = "family";
            d.type = "io";
            d.vHigh = vh;
            d.riseTime = d.fallTime = tr;
            d.rOut = rout;
            d.cComp = d.cIn = cin;
            d.vih = vih;
            d.vil = vil;
            d.rTerm = rterm;
            d.vTerm = vterm;
            d.note = note;
            v.push_back(d);
        };
        add("cmos5", "5 V CMOS (74HC / 74AC)", 5.0, 2.5e-9, 40, 5e-12, 3.5, 1.5, 0, 0, "74AC-class edge, 74HC thresholds");
        add("lvcmos33", "LVCMOS 3.3 V", 3.3, 1.0e-9, 30, 4e-12, 2.0, 0.8, 0, 0, "JESD8C; MCU / FPGA GPIO, fast slew");
        add("lvcmos25", "LVCMOS 2.5 V", 2.5, 0.8e-9, 35, 4e-12, 1.7, 0.7, 0, 0, "JESD8-5");
        add("lvcmos18", "LVCMOS 1.8 V", 1.8, 0.7e-9, 40, 3e-12, 1.17, 0.63, 0, 0, "JESD8-7 (65 % / 35 % of VDD)");
        add("lvcmos12", "LVCMOS 1.2 V", 1.2, 0.6e-9, 45, 3e-12, 0.78, 0.42, 0, 0, "JESD8-12");
        add("sstl15", "SSTL-15 (DDR3)", 1.5, 0.3e-9, 34, 2e-12, 0.85, 0.65, 60, 0.75, "JESD79-3: 34 Ω drive, 60 Ω ODT to VTT");
        add("sstl135", "SSTL-135 (DDR3L)", 1.35, 0.3e-9, 34, 2e-12, 0.765, 0.585, 60, 0.675, "JESD79-3-1: 34 Ω drive, 60 Ω ODT");
        add("pod12", "POD-12 (DDR4)", 1.2, 0.25e-9, 34, 1.1e-12, 0.915, 0.765, 48, 1.2, "JESD79-4: 34 Ω drive, 48 Ω ODT to VDDQ");
        add("usb2hs", "USB 2.0 high-speed (per leg)", 0.8, 0.5e-9, 45, 1e-12, 0.3, 0.1, 45, 0,
            "45 Ω source and 45 Ω receiver termination: 400 mV at the receiver");
        return v;
    }();
    return families;
}

const DriverModel* findLogicFamily(const std::string& id) {
    for (const auto& f : logicFamilies())
        if (f.id == id) return &f;
    return nullptr;
}

Json driverModelToJson(const DriverModel& m) {
    Json j = Json::object();
    j["id"] = m.id;
    j["name"] = m.name;
    j["source"] = m.source;
    j["type"] = m.type;
    j["vHigh"] = m.vHigh;
    j["riseTime"] = m.riseTime;
    j["fallTime"] = m.fallTime;
    j["rOut"] = m.rOut;
    j["cComp"] = m.cComp;
    j["cIn"] = m.cIn;
    j["vih"] = m.vih;
    j["vil"] = m.vil;
    j["rTerm"] = m.rTerm;
    j["vTerm"] = m.vTerm;
    j["rPkg"] = m.rPkg;
    j["lPkg"] = m.lPkg;
    j["cPkg"] = m.cPkg;
    j["note"] = m.note;
    return j;
}

DriverModel driverModelFromJson(const Json& j) {
    DriverModel m;
    m.id = j.get("id").asString("");
    m.name = j.get("name").asString(m.id);
    m.source = j.get("source").asString("ibis");
    m.type = j.get("type").asString("io");
    auto num = [&](const char* k, double def, double lo, double hi) {
        double v = j.get(k).asNumber(def);
        return std::isfinite(v) ? std::clamp(v, lo, hi) : def;
    };
    m.vHigh = num("vHigh", 3.3, 0.05, 60);
    m.riseTime = num("riseTime", 1e-9, 1e-12, 1e-6);
    m.fallTime = num("fallTime", m.riseTime, 1e-12, 1e-6);
    m.rOut = num("rOut", 30, 0.1, 1e4);
    m.cComp = num("cComp", 3e-12, 0, 1e-9);
    m.cIn = num("cIn", m.cComp, 0, 1e-9);
    m.vih = num("vih", 0.7 * m.vHigh, 0, 60);
    m.vil = num("vil", 0.3 * m.vHigh, 0, 60);
    m.rTerm = num("rTerm", 0, 0, 1e6);
    m.vTerm = num("vTerm", 0, -60, 60);
    m.rPkg = num("rPkg", 0, 0, 100);
    m.lPkg = num("lPkg", 0, 0, 1e-7);
    m.cPkg = num("cPkg", 0, 0, 1e-9);
    m.note = j.get("note").asString("");
    return m;
}

Json ibisFileJson(const IbisFile& f, const std::string& corner) {
    Json root = Json::object();
    root["version"] = f.version;
    root["fileName"] = f.fileName;
    Json comps = Json::array();
    for (const auto& c : f.components) {
        Json cj = Json::object();
        cj["name"] = c.name;
        cj["manufacturer"] = c.manufacturer;
        Json pins = Json::array();
        for (const auto& p : c.pins) {
            Json pj = Json::object();
            pj["pin"] = p.pin;
            pj["signal"] = p.signal;
            pj["model"] = p.model;
            pins.push(pj);
        }
        cj["pins"] = pins;
        comps.push(cj);
    }
    root["components"] = comps;
    Json models = Json::array();
    const IbisComponent* comp = f.components.empty() ? nullptr : &f.components.front();
    for (const auto& m : f.models) {
        Json mj = driverModelToJson(driverFromIbis(m, corner, comp));
        mj["modelType"] = m.type;
        models.push(mj);
    }
    root["models"] = models;
    Json warnings = Json::array();
    for (const auto& w : f.warnings) warnings.push(w);
    root["warnings"] = warnings;
    return root;
}

}  // namespace sieda
