#include "sieda/Library.hpp"

#include "sieda/CustomParts.hpp"

#include <algorithm>
#include <map>
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

/// Two-lead through-hole part (axial resistor / diode lying flat, ceramic disc, radial electrolytic standing up).
FootprintDef twoLeadTht(const std::string& name, double pitch, double drill, double pad, double bodyW, double bodyD,
                        double bodyH, float r, float g, float b, bool cylinder = false, int squarePad = 0,
                        const char* label = "") {
    FootprintDef fp;
    fp.name = name;
    fp.label = label;
    // squarePad: the pad drawn square (0 = pin 1: anode side / +; 1 = pin 2: a diode's cathode, as KiCad marks it;
    // -1 = both round, an axial resistor or inductor).
    fp.pads.push_back({0, {-pitch / 2, 0}, {pad, pad}, true, drill, squarePad != 0});
    fp.pads.push_back({1, {pitch / 2, 0}, {pad, pad}, true, drill, squarePad != 1});
    fp.courtyardW = std::max(pitch + pad, bodyW) + 0.6;
    fp.courtyardH = std::max(pad, bodyD) + 0.6;
    fp.body = {bodyW, bodyD, bodyH, cylinder, r, g, b};
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
        {ComponentKind::Junction, "Junction", "JN", "", "", {{"J", {0, 0}}}, "", false},
        {ComponentKind::PartUnit, "Part Unit", "U", "", "", {}, "", false},  // pins come from the part's unit
    };

    footprints_.push_back(twoPadSmd("R_0805", 1.9, 1.0, 1.3, 2.0, 1.25, 0.5, 0.10f, 0.10f, 0.11f));
    footprints_.push_back(twoPadSmd("C_0805", 1.9, 1.0, 1.3, 2.0, 1.25, 1.0, 0.78f, 0.62f, 0.42f));
    footprints_.push_back(twoPadSmd("L_0805", 1.9, 1.0, 1.3, 2.0, 1.25, 1.0, 0.35f, 0.35f, 0.38f));
    footprints_.push_back(twoPadSmd("D_SOD123", 3.3, 0.9, 1.2, 2.7, 1.6, 1.1, 0.10f, 0.10f, 0.11f));
    footprints_.push_back(twoPadSmd("LED_0805", 1.9, 1.0, 1.3, 2.0, 1.25, 0.8, 0.92f, 0.15f, 0.12f));
    footprints_.push_back(twoPadSmd("Fuse_1206", 2.8, 1.2, 1.7, 3.2, 1.6, 0.9, 0.85f, 0.85f, 0.80f));
    footprints_.push_back(twoPadSmd("SW_SMD", 4.0, 1.5, 1.0, 6.0, 3.5, 2.5, 0.55f, 0.55f, 0.58f));
    // Package variants a part can be switched to (Component::package): chip sizes, through-hole, tantalum and
    // electrolytic capacitors, power diodes. IPC-7351 nominal lands.
    footprints_.push_back(twoPadSmd("R_0402", 1.0, 0.6, 0.6, 1.0, 0.5, 0.35, 0.10f, 0.10f, 0.11f));
    footprints_.push_back(twoPadSmd("R_0603", 1.6, 0.9, 0.95, 1.6, 0.8, 0.45, 0.10f, 0.10f, 0.11f));
    footprints_.push_back(twoPadSmd("R_1206", 3.0, 1.15, 1.8, 3.2, 1.6, 0.55, 0.10f, 0.10f, 0.11f));
    footprints_.push_back(twoLeadTht("R_Axial_THT", 10.16, 0.8, 1.6, 6.3, 2.5, 2.5, 0.82f, 0.71f, 0.52f));
    footprints_.push_back(twoPadSmd("C_0402", 1.0, 0.6, 0.6, 1.0, 0.5, 0.5, 0.78f, 0.62f, 0.42f));
    footprints_.push_back(twoPadSmd("C_0603", 1.6, 0.9, 0.95, 1.6, 0.8, 0.8, 0.78f, 0.62f, 0.42f));
    footprints_.push_back(twoPadSmd("C_1206", 3.0, 1.15, 1.8, 3.2, 1.6, 1.2, 0.78f, 0.62f, 0.42f));
    footprints_.push_back(twoLeadTht("C_Disc_THT", 5.08, 0.8, 1.6, 5.0, 2.5, 6.0, 0.86f, 0.55f, 0.15f));
    footprints_.push_back(twoPadSmd("CP_Tant_A", 2.8, 1.4, 1.4, 3.2, 1.6, 1.6, 0.90f, 0.62f, 0.10f));
    footprints_.push_back(twoPadSmd("CP_Tant_B", 3.1, 1.6, 2.4, 3.5, 2.8, 1.9, 0.90f, 0.62f, 0.10f));
    footprints_.push_back(twoLeadTht("CP_Radial_THT", 2.5, 0.8, 1.6, 6.3, 6.3, 11.0, 0.10f, 0.14f, 0.34f, true));
    footprints_.push_back(twoPadSmd("L_0603", 1.6, 0.9, 0.95, 1.6, 0.8, 0.8, 0.35f, 0.35f, 0.38f));
    footprints_.push_back(twoPadSmd("L_1206", 3.0, 1.15, 1.8, 3.2, 1.6, 1.1, 0.35f, 0.35f, 0.38f));
    footprints_.push_back(twoPadSmd("D_SMA", 4.0, 1.6, 2.0, 4.3, 2.6, 2.1, 0.12f, 0.12f, 0.13f));
    footprints_.push_back(twoLeadTht("D_DO41_THT", 10.16, 1.0, 2.0, 5.2, 2.7, 2.7, 0.08f, 0.08f, 0.09f));
    footprints_.push_back(twoPadSmd("LED_0603", 1.6, 0.9, 0.95, 1.6, 0.8, 0.6, 0.92f, 0.15f, 0.12f));
    // More through-hole sizes, pad / drill / pitch as KiCad's footprints of the same name (Resistor_THT,
    // Capacitor_THT, Inductor_THT, Diode_THT, LED_THT); the body from the name, its height a typical part's.
    struct Tht { const char* name; double pitch, drill, pad, bodyW, bodyD, bodyH; float r, g, b; bool cyl; int square;
                 const char* label; };
    const float rr = 0.82f, rg = 0.71f, rb = 0.52f;  // resistor body
    const Tht tht[] = {
        {"R_Axial_DIN0204_L3.6mm_D1.6mm_P5.08mm_Horizontal", 5.08, 0.7, 1.4, 3.6, 1.6, 1.6, rr, rg, rb, false, -1, "Axial DIN0204 (1/8 W), 5.08 mm"},
        {"R_Axial_DIN0204_L3.6mm_D1.6mm_P7.62mm_Horizontal", 7.62, 0.7, 1.4, 3.6, 1.6, 1.6, rr, rg, rb, false, -1, "Axial DIN0204 (1/8 W), 7.62 mm"},
        {"R_Axial_DIN0207_L6.3mm_D2.5mm_P7.62mm_Horizontal", 7.62, 0.8, 1.6, 6.3, 2.5, 2.5, rr, rg, rb, false, -1, "Axial DIN0207 (1/4 W), 7.62 mm"},
        {"R_Axial_DIN0309_L9.0mm_D3.2mm_P12.70mm_Horizontal", 12.70, 0.8, 1.6, 9.0, 3.2, 3.2, rr, rg, rb, false, -1, "Axial DIN0309 (1/2 W), 12.7 mm"},
        {"R_Axial_DIN0411_L9.9mm_D3.6mm_P15.24mm_Horizontal", 15.24, 1.2, 2.4, 9.9, 3.6, 3.6, rr, rg, rb, false, -1, "Axial DIN0411 (1 W), 15.24 mm"},
        {"R_Axial_DIN0414_L11.9mm_D4.5mm_P20.32mm_Horizontal", 20.32, 1.2, 2.4, 11.9, 4.5, 4.5, rr, rg, rb, false, -1, "Axial DIN0414 (2 W), 20.32 mm"},
        {"R_Axial_DIN0516_L15.5mm_D5.0mm_P25.40mm_Horizontal", 25.40, 1.2, 2.4, 15.5, 5.0, 5.0, rr, rg, rb, false, -1, "Axial DIN0516 (3 W), 25.4 mm"},
        {"C_Disc_D3.0mm_W1.6mm_P2.50mm", 2.5, 0.8, 1.6, 3.0, 1.6, 4.0, 0.86f, 0.55f, 0.15f, false, -1, "Ceramic disc 3 mm, 2.5 mm"},
        {"C_Disc_D5.0mm_W2.5mm_P2.50mm", 2.5, 0.8, 1.6, 5.0, 2.5, 6.0, 0.86f, 0.55f, 0.15f, false, -1, "Ceramic disc 5 mm, 2.5 mm"},
        {"C_Disc_D7.5mm_W2.5mm_P5.00mm", 5.0, 1.0, 2.0, 7.5, 2.5, 8.5, 0.86f, 0.55f, 0.15f, false, -1, "Ceramic disc 7.5 mm, 5 mm"},
        {"C_Rect_L7.2mm_W2.5mm_P5.00mm", 5.0, 0.75, 1.5, 7.2, 2.5, 6.5, 0.85f, 0.20f, 0.15f, false, -1, "Film box 7.2 × 2.5 mm, 5 mm"},
        {"C_Rect_L10.0mm_W4.0mm_P7.50mm_MKS4", 7.5, 1.0, 2.0, 10.0, 4.0, 9.0, 0.85f, 0.20f, 0.15f, false, -1, "Film box 10 × 4 mm, 7.5 mm"},
        {"C_Rect_L13.0mm_W5.0mm_P10.00mm_FKS3_FKP3_MKS4", 10.0, 1.0, 2.0, 13.0, 5.0, 11.0, 0.85f, 0.20f, 0.15f, false, -1, "Film box 13 × 5 mm, 10 mm"},
        {"C_Rect_L18.0mm_W7.0mm_P15.00mm_FKS3_FKP3", 15.0, 1.2, 2.4, 18.0, 7.0, 14.0, 0.85f, 0.20f, 0.15f, false, -1, "Film box 18 × 7 mm, 15 mm"},
        {"CP_Radial_D5.0mm_P2.00mm", 2.0, 0.8, 1.6, 5.0, 5.0, 11.0, 0.10f, 0.14f, 0.34f, true, 0, "Electrolytic radial 5 mm, 2 mm"},
        {"CP_Radial_D8.0mm_P3.50mm", 3.5, 0.8, 1.6, 8.0, 8.0, 11.5, 0.10f, 0.14f, 0.34f, true, 0, "Electrolytic radial 8 mm, 3.5 mm"},
        {"CP_Radial_D10.0mm_P5.00mm", 5.0, 1.0, 2.0, 10.0, 10.0, 12.5, 0.10f, 0.14f, 0.34f, true, 0, "Electrolytic radial 10 mm, 5 mm"},
        {"CP_Radial_D12.5mm_P5.00mm", 5.0, 1.2, 2.4, 12.5, 12.5, 20.0, 0.10f, 0.14f, 0.34f, true, 0, "Electrolytic radial 12.5 mm, 5 mm"},
        {"CP_Radial_D16.0mm_P7.50mm", 7.5, 1.2, 2.4, 16.0, 16.0, 25.0, 0.10f, 0.14f, 0.34f, true, 0, "Electrolytic radial 16 mm, 7.5 mm"},
        {"CP_Radial_D18.0mm_P7.50mm", 7.5, 1.2, 2.4, 18.0, 18.0, 35.5, 0.10f, 0.14f, 0.34f, true, 0, "Electrolytic radial 18 mm, 7.5 mm"},
        {"L_Axial_L5.3mm_D2.2mm_P10.16mm_Horizontal_Vishay_IM-1", 10.16, 0.8, 1.6, 5.3, 2.2, 2.2, 0.35f, 0.35f, 0.38f, false, -1, "Axial 5.3 × 2.2 mm, 10.16 mm"},
        {"L_Axial_L9.5mm_D4.0mm_P12.70mm_Horizontal_Fastron_SMCC", 12.70, 1.0, 2.0, 9.5, 4.0, 4.0, 0.35f, 0.35f, 0.38f, false, -1, "Axial 9.5 × 4 mm, 12.7 mm"},
        {"L_Radial_D10.0mm_P5.00mm_Fastron_07P", 5.0, 1.3, 2.6, 10.0, 10.0, 12.0, 0.35f, 0.35f, 0.38f, true, -1, "Radial 10 mm, 5 mm"},
        {"L_Radial_D12.0mm_P5.00mm_Fastron_11P", 5.0, 1.3, 2.6, 12.0, 12.0, 14.0, 0.35f, 0.35f, 0.38f, true, -1, "Radial 12 mm, 5 mm"},
        {"D_DO-35_SOD27_P7.62mm_Horizontal", 7.62, 0.8, 1.6, 4.0, 2.0, 2.0, 0.85f, 0.35f, 0.20f, false, 1, "DO-35 THT (1N4148), 7.62 mm"},
        {"D_DO-15_P12.70mm_Horizontal", 12.70, 1.2, 2.4, 7.6, 3.6, 3.6, 0.08f, 0.08f, 0.09f, false, 1, "DO-15 THT, 12.7 mm"},
        {"D_DO-201AD_P15.24mm_Horizontal", 15.24, 1.6, 3.2, 9.5, 5.3, 5.3, 0.08f, 0.08f, 0.09f, false, 1, "DO-201AD THT (1N540x), 15.24 mm"},
        {"LED_D3.0mm", 2.54, 0.9, 1.8, 3.0, 3.0, 5.3, 0.92f, 0.15f, 0.12f, true, 1, "3 mm THT"},
        {"LED_D5.0mm", 2.54, 0.9, 1.8, 5.0, 5.0, 8.6, 0.92f, 0.15f, 0.12f, true, 1, "5 mm THT"},
    };
    for (const Tht& t : tht)
        footprints_.push_back(twoLeadTht(t.name, t.pitch, t.drill, t.pad, t.bodyW, t.bodyD, t.bodyH, t.r, t.g, t.b, t.cyl,
                                         t.square, t.label));
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

std::vector<std::string> Library::packageVariants(ComponentKind kind) {
    switch (kind) {
        case ComponentKind::Resistor:
            return {"R_0402", "R_0603", "R_0805", "R_1206", "R_Axial_THT",
                    "R_Axial_DIN0204_L3.6mm_D1.6mm_P5.08mm_Horizontal", "R_Axial_DIN0204_L3.6mm_D1.6mm_P7.62mm_Horizontal",
                    "R_Axial_DIN0207_L6.3mm_D2.5mm_P7.62mm_Horizontal", "R_Axial_DIN0309_L9.0mm_D3.2mm_P12.70mm_Horizontal",
                    "R_Axial_DIN0411_L9.9mm_D3.6mm_P15.24mm_Horizontal", "R_Axial_DIN0414_L11.9mm_D4.5mm_P20.32mm_Horizontal",
                    "R_Axial_DIN0516_L15.5mm_D5.0mm_P25.40mm_Horizontal"};
        case ComponentKind::Capacitor:
            return {"C_0402", "C_0603", "C_0805", "C_1206", "C_Disc_THT", "CP_Tant_A", "CP_Tant_B", "CP_Radial_THT",
                    "C_Disc_D3.0mm_W1.6mm_P2.50mm", "C_Disc_D5.0mm_W2.5mm_P2.50mm", "C_Disc_D7.5mm_W2.5mm_P5.00mm",
                    "C_Rect_L7.2mm_W2.5mm_P5.00mm", "C_Rect_L10.0mm_W4.0mm_P7.50mm_MKS4",
                    "C_Rect_L13.0mm_W5.0mm_P10.00mm_FKS3_FKP3_MKS4", "C_Rect_L18.0mm_W7.0mm_P15.00mm_FKS3_FKP3",
                    "CP_Radial_D5.0mm_P2.00mm", "CP_Radial_D8.0mm_P3.50mm", "CP_Radial_D10.0mm_P5.00mm",
                    "CP_Radial_D12.5mm_P5.00mm", "CP_Radial_D16.0mm_P7.50mm", "CP_Radial_D18.0mm_P7.50mm"};
        case ComponentKind::Inductor:
            return {"L_0603", "L_0805", "L_1206", "L_Axial_L5.3mm_D2.2mm_P10.16mm_Horizontal_Vishay_IM-1",
                    "L_Axial_L9.5mm_D4.0mm_P12.70mm_Horizontal_Fastron_SMCC", "L_Radial_D10.0mm_P5.00mm_Fastron_07P",
                    "L_Radial_D12.0mm_P5.00mm_Fastron_11P"};
        case ComponentKind::Diode:
            return {"D_SOD123", "D_SMA", "D_DO41_THT", "D_DO-35_SOD27_P7.62mm_Horizontal", "D_DO-15_P12.70mm_Horizontal",
                    "D_DO-201AD_P15.24mm_Horizontal"};
        case ComponentKind::LED: return {"LED_0603", "LED_0805", "LED_D3.0mm", "LED_D5.0mm"};
        default: return {};
    }
}

std::string Library::packageLabel(const std::string& footprint) {
    static const std::map<std::string, std::string> labels = {
        {"R_0402", "0402"}, {"R_0603", "0603"}, {"R_0805", "0805"}, {"R_1206", "1206"},
        {"R_Axial_THT", "Axial THT (1/4 W)"}, {"C_0402", "0402"}, {"C_0603", "0603"}, {"C_0805", "0805"},
        {"C_1206", "1206"}, {"C_Disc_THT", "Ceramic disc THT"}, {"CP_Tant_A", "Tantalum A (3216)"},
        {"CP_Tant_B", "Tantalum B (3528)"}, {"CP_Radial_THT", "Electrolytic radial THT"}, {"L_0603", "0603"},
        {"L_0805", "0805"}, {"L_1206", "1206"}, {"D_SOD123", "SOD-123"}, {"D_SMA", "SMA (DO-214AC)"},
        {"D_DO41_THT", "DO-41 THT"}, {"LED_0603", "0603"}, {"LED_0805", "0805"},
    };
    auto it = labels.find(footprint);
    if (it != labels.end()) return it->second;
    const FootprintDef* fp = instance().footprint(footprint);  // the through-hole sizes carry their own label
    return fp && !fp->label.empty() ? fp->label : footprint;
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
