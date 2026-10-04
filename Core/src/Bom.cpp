#include "sieda/Bom.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <tuple>

#include "sieda/Library.hpp"
#include "sieda/Simulator.hpp"
#include "sieda/Units.hpp"

namespace sieda {

namespace {
std::string footprintLabel(const Component& c) {
    if (c.pcb.embedded()) return "Embedded, layer " + std::to_string(c.pcb.embeddedLayer + 1);
    const FootprintDef* fp = Library::instance().footprint(c.footprintName());
    if (!fp) return c.footprintName();
    return fp->label.empty() ? fp->name : fp->label;
}

/// "0805" from "R_0805", "SOT-23" from "SOT23_BJT" …
std::string packageCode(const Component& c) {
    std::string fp = c.footprintName();
    for (const char* size : {"0402", "0603", "0805", "1206", "1210", "2512"})
        if (fp.find(size) != std::string::npos) return size;
    if (fp.rfind("SOT23", 0) == 0) return "SOT-23";
    if (fp.rfind("SOD123", 0) == 0 || fp.find("SOD123") != std::string::npos) return "SOD-123";
    if (fp == "D_SMA") return "SMA";
    if (fp == "D_DO41_THT") return "DO-41";
    if (fp == "CP_Tant_A") return "Tantalum A";
    if (fp == "CP_Tant_B") return "Tantalum B";
    if (fp == "CP_Radial_THT") return "Radial";
    if (fp == "R_Axial_THT") return "Axial";
    if (fp == "C_Disc_THT") return "Disc";
    if (fp.rfind("SOIC8", 0) == 0) return "SOIC-8";
    if (fp.rfind("PinHeader", 0) == 0) return "2.54 mm header";
    return footprintLabel(c);
}

/// Natural order: R2 before R10.
bool refLess(const std::string& a, const std::string& b) {
    auto split = [](const std::string& s) {
        size_t i = 0;
        while (i < s.size() && !std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
        long n = i < s.size() ? std::strtol(s.c_str() + i, nullptr, 10) : -1;
        return std::make_pair(s.substr(0, i), n);
    };
    auto pa = split(a), pb = split(b);
    if (pa.first != pb.first) return pa.first < pb.first;
    if (pa.second != pb.second) return pa.second < pb.second;
    return a < b;
}

/// Highest supply voltage on the board (DC magnitude, sine peak or pulse level).
double maxSupplyVolts(const Schematic& sch) {
    double v = 0;
    for (const auto& c : sch.components()) {
        if (!isVoltageSourceKind(c.kind)) continue;
        auto spec = SourceSpec::parse(c.value);
        if (!spec) continue;
        switch (spec->kind) {
            case SourceSpec::Kind::DC: v = std::max(v, std::fabs(spec->dc)); break;
            case SourceSpec::Kind::Sine: v = std::max(v, std::fabs(spec->offset) + std::fabs(spec->amplitude)); break;
            case SourceSpec::Kind::Pulse: v = std::max({v, std::fabs(spec->v1), std::fabs(spec->v2)}); break;
        }
    }
    return v;
}

std::string fmt(const char* f, double v) {
    char b[48];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

std::string upper(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

/// Looks like a part number (letters and digits, e.g. BC847, 1N4148, LM358), not a plain value or a word.
bool looksLikePartNumber(const std::string& v) {
    bool letter = false, digit = false;
    for (char ch : v) {
        if (std::isalpha(static_cast<unsigned char>(ch))) letter = true;
        else if (std::isdigit(static_cast<unsigned char>(ch))) digit = true;
        else if (ch != '-' && ch != '/' && ch != '.') return false;
    }
    return letter && digit && v.size() >= 4;
}
}  // namespace

std::pair<std::string, std::string> suggestedPart(const Component& c) {
    if (c.kind == ComponentKind::Resistor) {
        // Yageo RC series thick-film, 1 %: RC<size>FR-07<value>L, value code 330R / 4K7 / 10K / 1M.
        std::string size = packageCode(c);
        if (size.size() != 4) return {};
        auto ohms = parseEngineeringValue(primaryValue(c.value));
        if (!ohms || *ohms <= 0 || *ohms >= 1e7) return {};
        double v = *ohms;
        std::string mult = "R";
        if (v >= 1e6) v /= 1e6, mult = "M";
        else if (v >= 1e3) v /= 1e3, mult = "K";
        double whole = std::floor(v + 1e-9);
        double frac = v - whole;
        std::string code;
        if (frac < 1e-6) code = fmt("%.0f", whole) + mult;
        else {
            int digit = static_cast<int>(std::lround(frac * 10));
            if (std::fabs(frac * 10 - digit) > 1e-6 || digit >= 10) return {};  // more than one decimal: no code
            code = fmt("%.0f", whole) + mult + std::to_string(digit);
            if (whole == 0) code = "0" + mult + std::to_string(digit);
        }
        return {"Yageo", "RC" + size + "FR-07" + code + "L"};
    }
    switch (c.kind) {
        case ComponentKind::Diode:
        case ComponentKind::NPN:
        case ComponentKind::NMOS:
        case ComponentKind::OpAmp:
        case ComponentKind::IC8:
        case ComponentKind::Custom:
            if (looksLikePartNumber(c.value)) return {"", upper(c.value)};
            break;
        default: break;
    }
    return {};
}

std::vector<BomLine> buildBom(const Schematic& sch) {
    const double supply = maxSupplyVolts(sch);
    using Key = std::tuple<std::string, std::string, std::string, std::string, std::string, std::string, double, bool>;
    std::map<Key, BomLine> groups;
    for (const auto& c : sch.components()) {
        if (!c.hasFootprint() || isNetSymbolKind(c.kind)) continue;
        const Sourcing& s = c.sourcing;
        Key key{c.def().name, c.value, footprintLabel(c), s.manufacturer, s.mpn, s.supplierPart, s.unitPrice, s.dnp};
        BomLine& line = groups[key];
        if (line.refs.empty()) {
            line.type = c.def().name;
            line.value = c.value;
            line.footprint = footprintLabel(c);
            line.sourcing = s;
            auto [maker, mpn] = suggestedPart(c);
            line.suggestedManufacturer = maker;
            line.suggestedMpn = mpn;
            const std::string pkg = packageCode(c);
            line.embedded = c.pcb.embedded();
            if (line.embedded) {
                // Formed by the board house inside the stack-up: nothing to buy or place.
                line.description = c.kind == ComponentKind::Resistor
                                       ? "Embedded thin-film resistor " + primaryValue(c.value) + "Ω ±10 % (in the PCB)"
                                       : "Embedded buried capacitor " + primaryValue(c.value) + "F (in the PCB)";
                line.refs.push_back(c.ref);
                line.componentIds.push_back(c.id);
                continue;
            }
            switch (c.kind) {
                case ComponentKind::Resistor: {
                    auto rated = powerRating(c.value);
                    line.description = "Resistor " + primaryValue(c.value) + "Ω ±1% " + pkg;
                    double watts = rated ? *rated : (pkg == "0402" ? 0.0625 : pkg == "0603" ? 0.1 : pkg == "0805" ? 0.125
                                                     : pkg == "1206" ? 0.25 : pkg == "2512" ? 1.0 : 0.125);
                    line.rating = fmt("%g W", watts);
                    break;
                }
                case ComponentKind::Capacitor: {
                    line.description = "Capacitor " + primaryValue(c.value) + "F X7R ±10% " + pkg;
                    if (supply > 0) {
                        double need = supply * 2;  // ceramic: rate at twice the working voltage (DC-bias derating)
                        double rated = 0;
                        for (double v : {6.3, 10.0, 16.0, 25.0, 35.0, 50.0, 63.0, 100.0, 250.0, 450.0, 630.0, 1000.0})
                            if (v >= need) { rated = v; break; }
                        line.rating = rated > 0 ? "≥ " + fmt("%g V", rated) : "≥ " + fmt("%g V", need);
                    }
                    break;
                }
                case ComponentKind::Inductor: line.description = "Inductor " + primaryValue(c.value) + "H " + pkg; break;
                case ComponentKind::LED: line.description = "LED " + c.value + " " + pkg; break;
                case ComponentKind::Diode: line.description = "Diode " + c.value + " " + pkg; break;
                case ComponentKind::NPN: line.description = "NPN transistor " + c.value + " " + pkg; break;
                case ComponentKind::NMOS: line.description = "N-channel MOSFET " + c.value + " " + pkg; break;
                case ComponentKind::OpAmp: line.description = "Op-amp " + c.value + " " + pkg; break;
                case ComponentKind::Fuse:
                    line.description = "Fuse " + primaryValue(c.value) + "A " + pkg;
                    line.rating = primaryValue(c.value) + "A";
                    break;
                case ComponentKind::Switch: line.description = "Tactile / slide switch, SPST " + pkg; break;
                case ComponentKind::Connector: line.description = "Pin header, " + pkg; break;
                case ComponentKind::VoltageSource:
                case ComponentKind::Battery:
                case ComponentKind::ACSource:
                case ComponentKind::CurrentSource:
                    line.description = "Power input connector (" + c.def().name + " " + c.value + c.def().unit + "), " + pkg;
                    break;
                default: line.description = c.def().name + " " + c.value + " " + pkg; break;
            }
        }
        line.refs.push_back(c.ref);
        line.componentIds.push_back(c.id);
    }
    std::vector<BomLine> lines;
    for (auto& entry : groups) {
        const BomLine& line = entry.second;
        std::vector<size_t> order(line.refs.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return refLess(line.refs[a], line.refs[b]); });
        BomLine sorted = line;
        for (size_t i = 0; i < order.size(); ++i) {
            sorted.refs[i] = line.refs[order[i]];
            sorted.componentIds[i] = line.componentIds[order[i]];
        }
        if (!sorted.sourcing.dnp && !sorted.embedded) {
            if (sorted.sourcing.mpn.empty())
                sorted.notes.push_back(sorted.suggestedMpn.empty() ? "No manufacturer part number"
                                                                   : "No part number yet (suggested: " + sorted.suggestedMpn + ")");
            if (sorted.sourcing.unitPrice <= 0) sorted.notes.push_back("No price");
        }
        lines.push_back(std::move(sorted));
    }
    std::sort(lines.begin(), lines.end(), [](const BomLine& a, const BomLine& b) { return refLess(a.refs[0], b.refs[0]); });
    for (size_t i = 0; i < lines.size(); ++i) lines[i].item = static_cast<int>(i) + 1;
    return lines;
}

BomSummary summarizeBom(const std::vector<BomLine>& lines) {
    BomSummary s;
    s.lines = static_cast<int>(lines.size());
    for (const auto& l : lines) {
        if (l.sourcing.dnp) {
            s.dnp += l.quantity();
            continue;
        }
        if (l.embedded) {
            s.embedded += l.quantity();
            continue;
        }
        s.placements += l.quantity();
        if (l.sourcing.mpn.empty()) ++s.missingMpn;
        if (l.sourcing.unitPrice <= 0) ++s.unpriced;
        s.costPerBoard += l.lineCost();
    }
    return s;
}

Json bomJson(const Schematic& sch, int buildQuantity) {
    auto lines = buildBom(sch);
    BomSummary sum = summarizeBom(lines);
    Json root = Json::object();
    Json arr = Json::array();
    for (const auto& l : lines) {
        Json j = Json::object();
        j["item"] = l.item;
        Json refs = Json::array();
        for (const auto& r : l.refs) refs.push(r);
        j["refs"] = refs;
        Json ids = Json::array();
        for (int id : l.componentIds) ids.push(id);
        j["componentIds"] = ids;
        j["quantity"] = l.quantity();
        j["type"] = l.type;
        j["value"] = l.value;
        j["footprint"] = l.footprint;
        j["description"] = l.description;
        j["rating"] = l.rating;
        j["manufacturer"] = l.sourcing.manufacturer;
        j["mpn"] = l.sourcing.mpn;
        j["supplierPart"] = l.sourcing.supplierPart;
        j["unitPrice"] = l.sourcing.unitPrice;
        j["dnp"] = l.sourcing.dnp;
        j["embedded"] = l.embedded;
        j["lineCost"] = l.lineCost();
        j["suggestedManufacturer"] = l.suggestedManufacturer;
        j["suggestedMpn"] = l.suggestedMpn;
        Json notes = Json::array();
        for (const auto& n : l.notes) notes.push(n);
        j["notes"] = notes;
        arr.push(j);
    }
    root["lines"] = arr;
    Json s = Json::object();
    s["lines"] = sum.lines;
    s["placements"] = sum.placements;
    s["dnp"] = sum.dnp;
    s["embedded"] = sum.embedded;
    s["missingMpn"] = sum.missingMpn;
    s["unpriced"] = sum.unpriced;
    s["costPerBoard"] = sum.costPerBoard;
    root["summary"] = s;
    root["buildQuantity"] = std::max(1, buildQuantity);
    root["orderCost"] = sum.costPerBoard * std::max(1, buildQuantity);
    return root;
}

}  // namespace sieda
