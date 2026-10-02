#include "sieda/Library.hpp"

#include "sieda/CustomParts.hpp"

#include <algorithm>
#include <stdexcept>

namespace sieda {

namespace {
FootprintDef twoPadSmd(const std::string& name, double pitch, double padW, double padH, double bodyW, double bodyD,
                       double bodyH, float r, float g, float b, bool cylinder = false) {
    FootprintDef fp;
    fp.name = name;
    fp.pads.push_back({0, {-pitch / 2, 0}, {padW, padH}, false, 0.0, false});
    fp.pads.push_back({1, {pitch / 2, 0}, {padW, padH}, false, 0.0, false});
    fp.courtyardW = pitch + padW + 0.6;
    fp.courtyardH = std::max(padH, bodyD) + 0.6;
    fp.body = {bodyW, bodyD, bodyH, cylinder, r, g, b};
    return fp;
}

FootprintDef soic8(const std::string& name, const int (&padToPin)[8]) {
    FootprintDef fp;
    fp.name = name;
    const double ys[4] = {-1.905, -0.635, 0.635, 1.905};
    for (int i = 0; i < 4; ++i) fp.pads.push_back({padToPin[i], {-2.7, ys[i]}, {1.55, 0.6}, false, 0.0, false});
    for (int i = 0; i < 4; ++i) fp.pads.push_back({padToPin[4 + i], {2.7, ys[3 - i]}, {1.55, 0.6}, false, 0.0, false});
    fp.courtyardW = 7.0;
    fp.courtyardH = 5.4;
    fp.body = {3.9, 4.9, 1.5, false, 0.12f, 0.12f, 0.13f};
    return fp;
}

FootprintDef sot23(const std::string& name) {
    // Pin order in the schematic is (B,C,E) / (G,D,S): pad 1 = B/G, pad 2 = E/S, pad 3 = C/D.
    FootprintDef fp;
    fp.name = name;
    fp.pads.push_back({0, {-0.95, 1.1}, {0.9, 0.8}, false, 0.0, false});
    fp.pads.push_back({2, {0.95, 1.1}, {0.9, 0.8}, false, 0.0, false});
    fp.pads.push_back({1, {0.0, -1.1}, {0.9, 0.8}, false, 0.0, false});
    fp.courtyardW = 3.4;
    fp.courtyardH = 3.6;
    fp.body = {2.9, 1.3, 1.0, false, 0.12f, 0.12f, 0.13f};
    return fp;
}

FootprintDef header2(const std::string& name) {
    FootprintDef fp;
    fp.name = name;
    fp.pads.push_back({0, {0, -1.27}, {1.7, 1.7}, true, 1.0, false});
    fp.pads.push_back({1, {0, 1.27}, {1.7, 1.7}, true, 1.0, true});
    fp.courtyardW = 3.0;
    fp.courtyardH = 5.6;
    fp.body = {2.54, 5.08, 2.5, false, 0.08f, 0.08f, 0.08f};
    return fp;
}
}  // namespace

Library::Library() {
    const std::vector<PinDef> twoH = {{"1", {-30, 0}}, {"2", {30, 0}}};
    const std::vector<PinDef> diode = {{"A", {-30, 0}}, {"K", {30, 0}}};
    const std::vector<PinDef> source = {{"+", {0, -30}}, {"-", {0, 30}}};

    components_ = {
        {ComponentKind::Resistor, "Resistor", "R", "10k", "Ω", twoH, "R_0805", true},
        {ComponentKind::Capacitor, "Capacitor", "C", "100n", "F", twoH, "C_0805", true},
        {ComponentKind::Inductor, "Inductor", "L", "10u", "H", twoH, "L_0805", true},
        {ComponentKind::Diode, "Diode", "D", "1N4148", "", diode, "D_SOD123", true},
        {ComponentKind::LED, "LED", "D", "Red", "", diode, "LED_0805", true},
        {ComponentKind::VoltageSource, "Voltage Source", "V", "5", "V", source, "PinHeader_1x02", true},
        {ComponentKind::CurrentSource, "Current Source", "I", "1m", "A", source, "PinHeader_1x02", true},
        {ComponentKind::Ground, "Ground", "GND", "0", "", {{"GND", {0, 0}}}, "", true},
        {ComponentKind::NPN, "NPN Transistor", "Q", "BC847", "", {{"B", {-30, 0}}, {"C", {20, -30}}, {"E", {20, 30}}},
         "SOT23_BJT", true},
        {ComponentKind::NMOS, "N-MOSFET", "Q", "2N7002", "", {{"G", {-30, 0}}, {"D", {20, -30}}, {"S", {20, 30}}},
         "SOT23_FET", true},
        {ComponentKind::OpAmp, "Op-Amp", "U", "LM358", "", {{"IN+", {-40, 10}}, {"IN-", {-40, -10}}, {"OUT", {40, 0}}},
         "SOIC8_OPAMP", true},
        {ComponentKind::Switch, "Switch", "SW", "on", "", twoH, "SW_SMD", true},
        {ComponentKind::Connector, "Connector", "J", "Conn_01x02", "", {{"1", {-20, -10}}, {"2", {-20, 10}}},
         "PinHeader_1x02", false},
        {ComponentKind::IC8, "IC (8-pin)", "U", "IC", "",
         {{"1", {-40, -30}}, {"2", {-40, -10}}, {"3", {-40, 10}}, {"4", {-40, 30}},
          {"5", {40, 30}}, {"6", {40, 10}}, {"7", {40, -10}}, {"8", {40, -30}}},
         "SOIC8", false},
        {ComponentKind::Fuse, "Fuse", "F", "500m", "A", twoH, "Fuse_1206", true},
        {ComponentKind::NetLabel, "Net Label", "NL", "VCC", "", {{"N", {0, 0}}}, "", true},
        {ComponentKind::Custom, "Custom Part", "U", "", "", {}, "", false},
        {ComponentKind::Battery, "Battery", "BT", "9", "V", source, "PinHeader_1x02", true},
        {ComponentKind::ACSource, "AC Source", "VAC", "SIN(0 17 50)", "V", source, "PinHeader_1x02", true},
    };

    footprints_.push_back(twoPadSmd("R_0805", 1.9, 1.0, 1.3, 2.0, 1.25, 0.5, 0.10f, 0.10f, 0.11f));
    footprints_.push_back(twoPadSmd("C_0805", 1.9, 1.0, 1.3, 2.0, 1.25, 1.0, 0.78f, 0.62f, 0.42f));
    footprints_.push_back(twoPadSmd("L_0805", 1.9, 1.0, 1.3, 2.0, 1.25, 1.0, 0.35f, 0.35f, 0.38f));
    footprints_.push_back(twoPadSmd("D_SOD123", 3.3, 0.9, 1.2, 2.7, 1.6, 1.1, 0.10f, 0.10f, 0.11f));
    footprints_.push_back(twoPadSmd("LED_0805", 1.9, 1.0, 1.3, 2.0, 1.25, 0.8, 0.92f, 0.15f, 0.12f));
    footprints_.push_back(twoPadSmd("Fuse_1206", 2.8, 1.2, 1.7, 3.2, 1.6, 0.9, 0.85f, 0.85f, 0.80f));
    footprints_.push_back(twoPadSmd("SW_SMD", 4.0, 1.5, 1.0, 6.0, 3.5, 2.5, 0.55f, 0.55f, 0.58f));
    footprints_.push_back(header2("PinHeader_1x02"));
    footprints_.push_back(sot23("SOT23_BJT"));
    footprints_.push_back(sot23("SOT23_FET"));
    // Single op-amp SOIC-8: pad1 OUT, pad2 IN-, pad3 IN+, pad4 V-, pad8 V+ (supply pads unused by the ideal model).
    const int opampMap[8] = {2, 1, 0, -1, -1, -1, -1, -1};
    footprints_.push_back(soic8("SOIC8_OPAMP", opampMap));
    // Pads ordered 1..4 down the left, then 5..8 up the right; soic8() places index 4 at the bottom right.
    const int icMap[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    footprints_.push_back(soic8("SOIC8", icMap));
}

const Library& Library::instance() {
    static const Library lib;
    return lib;
}

const ComponentDef& Library::component(ComponentKind kind) const {
    int k = static_cast<int>(kind);
    if (!isValidKind(k)) throw std::out_of_range("invalid component kind");
    return components_[static_cast<size_t>(k)];
}

const FootprintDef* Library::footprint(const std::string& name) const {
    if (name.rfind("CUSTOM:", 0) == 0) {
        const CustomPart* part = CustomPartRegistry::instance().find(name.substr(7));
        return part ? &part->footprint : nullptr;
    }
    for (const auto& fp : footprints_)
        if (fp.name == name) return &fp;
    return nullptr;
}

}  // namespace sieda
