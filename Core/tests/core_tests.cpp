// SiEDA core unit tests — dependency-free; run via `ctest` or directly.
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include <algorithm>

#include "sieda/CustomParts.hpp"
#include "sieda/Export.hpp"
#include "sieda/Json.hpp"
#include "sieda/Mesh.hpp"
#include "sieda/Project.hpp"
#include "sieda/Simulator.hpp"
#include "sieda/StandardParts.hpp"
#include "sieda/Validation.hpp"
#include "sieda/Verification.hpp"
#include "sieda/Units.hpp"

extern "C" int sieda_c_api_smoke_test(void);

using namespace sieda;

namespace {
int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failures;                                                              \
            std::printf("    FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                              \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                                          \
    do {                                                                                               \
        ++g_checks;                                                                                    \
        double _a = (a), _b = (b);                                                                     \
        if (!(std::fabs(_a - _b) <= (tol))) {                                                          \
            ++g_failures;                                                                              \
            std::printf("    FAILED %s:%d: %s = %g, expected %g ± %g\n", __FILE__, __LINE__, #a, _a, _b, \
                        (double)(tol));                                                                \
        }                                                                                              \
    } while (0)

struct TestCase {
    const char* name;
    std::function<void()> fn;
};
std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}
struct Register {
    Register(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};
#define TEST(name)                                     \
    static void name();                                \
    static Register reg_##name(#name, name);           \
    static void name()

// Helpers ---------------------------------------------------------------
int pin(const Schematic& s, int comp, const char* name) { return s.pinIndex(comp, name); }
void wire(Schematic& s, int a, const char* pa, int b, const char* pb) {
    int id = s.connect({a, pin(s, a, pa)}, {b, pin(s, b, pb)});
    CHECK(id >= 0);
}
double netV(const Schematic& s, const DcResult& r, int comp, const char* p) {
    int n = s.netOf({comp, pin(s, comp, p)});
    return n < 0 ? NAN : r.netVoltages[static_cast<size_t>(n)];
}
const DeviceReading* reading(const DcResult& r, int comp) {
    for (const auto& d : r.devices)
        if (d.componentId == comp) return &d;
    return nullptr;
}

/// 5 V → 330 Ω → red LED → GND, the classic indicator circuit.
Project ledProject() {
    Project p;
    p.name = "LED indicator";
    auto& s = p.schematic;
    int v = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    int r = s.addComponent(ComponentKind::Resistor, "330", {80, -40});
    int d = s.addComponent(ComponentKind::LED, "Red", {160, -40});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    wire(s, v, "+", r, "1");
    wire(s, r, "2", d, "A");
    wire(s, d, "K", g, "GND");
    wire(s, v, "-", g, "GND");
    p.schematicChanged();
    return p;
}
}  // namespace

// ======================================================================= units & json

TEST(units_parse) {
    CHECK_NEAR(*parseEngineeringValue("4.7k"), 4700, 1e-9);
    CHECK_NEAR(*parseEngineeringValue("4k7"), 4700, 1e-9);
    CHECK_NEAR(*parseEngineeringValue("2R2"), 2.2, 1e-12);
    CHECK_NEAR(*parseEngineeringValue("100nF"), 100e-9, 1e-18);
    CHECK_NEAR(*parseEngineeringValue("10uF"), 10e-6, 1e-15);
    CHECK_NEAR(*parseEngineeringValue("1meg"), 1e6, 1e-6);
    CHECK_NEAR(*parseEngineeringValue("1M"), 1e6, 1e-6);
    CHECK_NEAR(*parseEngineeringValue("1m"), 1e-3, 1e-12);
    CHECK_NEAR(*parseEngineeringValue("5V"), 5, 1e-12);
    CHECK_NEAR(*parseEngineeringValue("1e-6"), 1e-6, 1e-15);
    CHECK_NEAR(*parseEngineeringValue("-3.3"), -3.3, 1e-12);
    CHECK(!parseEngineeringValue("abc"));
    CHECK(!parseEngineeringValue(""));
    CHECK(formatEngineeringValue(4700, "Ω") == "4.7kΩ");
    CHECK(formatEngineeringValue(1e-7, "F") == "100nF");
}

TEST(source_specs) {
    auto dc = SourceSpec::parse("12V");
    CHECK(dc && dc->kind == SourceSpec::Kind::DC && dc->dc == 12);
    auto sine = SourceSpec::parse("SIN(0 1 1k)");
    CHECK(sine && sine->kind == SourceSpec::Kind::Sine);
    CHECK_NEAR(sine->valueAt(0.25e-3), 1.0, 1e-9);
    auto pulse = SourceSpec::parse("PULSE(0 5 1m)");
    CHECK(pulse && pulse->kind == SourceSpec::Kind::Pulse);
    CHECK_NEAR(pulse->valueAt(0), 0.0, 1e-12);
    CHECK_NEAR(pulse->valueAt(0.1e-3), 5.0, 1e-12);
    CHECK_NEAR(pulse->valueAt(0.7e-3), 0.0, 1e-12);
    CHECK(!SourceSpec::parse("SIN(0 1)"));
    CHECK(!SourceSpec::parse("banana"));
}

TEST(json_roundtrip) {
    Json j = Json::parse(R"({"a":[1,2.5,"x\né"],"b":{"c":true,"d":null},"e":-1e-3})");
    CHECK(j.get("a").size() == 3);
    CHECK(j.get("a")[2].asString() == "x\n\xc3\xa9");
    CHECK(j.get("b").get("c").asBool());
    CHECK(j.get("b").get("d").isNull());
    CHECK_NEAR(j.get("e").asNumber(), -1e-3, 1e-15);
    Json k = Json::parse(j.dump(true));
    CHECK(k.dump() == j.dump());
    bool threw = false;
    try {
        Json::parse("{\"a\":}");
    } catch (const JsonError&) {
        threw = true;
    }
    CHECK(threw);
    // Copy-on-write: mutating a copy must not affect the original.
    Json copy = j;
    copy["new"] = 1;
    CHECK(!j.has("new") && copy.has("new"));
}

// ======================================================================= schematic

TEST(schematic_connectivity) {
    Schematic s;
    int r1 = s.addComponent(ComponentKind::Resistor, "1k", {0, 0});
    int r2 = s.addComponent(ComponentKind::Resistor, "1k", {100, 0});
    int l1 = s.addComponent(ComponentKind::NetLabel, "VCC", {-50, 0});
    int l2 = s.addComponent(ComponentKind::NetLabel, "VCC", {150, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 50});
    CHECK(s.find(r1)->ref == "R1" && s.find(r2)->ref == "R2");
    wire(s, l1, "N", r1, "1");
    wire(s, l2, "N", r2, "2");
    wire(s, r1, "2", g, "GND");
    CHECK(s.netOf({r1, 0}) == s.netOf({r2, 1}));  // merged through identically named labels
    CHECK(s.nets()[static_cast<size_t>(s.netOf({r1, 0}))].name == "VCC");
    CHECK(s.netOf({r1, 1}) == s.groundNet());
    CHECK(s.connect({r1, 0}, {l1, 0}) == -1);  // duplicate wire rejected
    CHECK(s.connect({r1, 0}, {r1, 5}) == -1);  // invalid pin rejected

    // Rotation: pin "2" at (+30,0) rotated 90° clockwise on screen lands below the origin.
    s.rotateComponent(r1, 90);
    Vec2 p = s.pinPosition({r1, 1});
    CHECK_NEAR(p.x, 0, 1e-9);
    CHECK_NEAR(p.y, 30, 1e-9);

    CHECK(s.removeComponent(r1));
    CHECK(s.wires().size() == 1);
}

TEST(erc_rules) {
    Schematic s;
    int v = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    int r = s.addComponent(ComponentKind::Resistor, "abc", {100, 0});
    wire(s, v, "+", v, "-");
    auto has = [&](const std::string& code) {
        for (const auto& e : s.runERC())
            if (e.code == code) return true;
        return false;
    };
    CHECK(has("ERC_NO_GROUND"));
    CHECK(has("ERC_SHORTED_SOURCE"));
    CHECK(has("ERC_INVALID_VALUE"));
    CHECK(has("ERC_FLOATING_COMPONENT"));
    (void)r;

    Schematic t;
    int v2 = t.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    int d = t.addComponent(ComponentKind::LED, "Red", {100, 0});
    int g = t.addComponent(ComponentKind::Ground, "", {0, 80});
    wire(t, v2, "+", d, "A");
    wire(t, d, "K", g, "GND");
    wire(t, v2, "-", g, "GND");
    bool warned = false;
    for (const auto& e : t.runERC()) warned |= e.code == "ERC_NO_CURRENT_LIMIT";
    CHECK(warned);

    Project led = ledProject();
    int errors = 0;
    for (const auto& e : led.schematic.runERC()) errors += e.severity == Severity::Error;
    CHECK(errors == 0);
}

// ======================================================================= simulation

TEST(sim_voltage_divider) {
    Schematic s;
    int v = s.addComponent(ComponentKind::VoltageSource, "10", {0, 0});
    int r1 = s.addComponent(ComponentKind::Resistor, "10k", {100, 0});
    int r2 = s.addComponent(ComponentKind::Resistor, "10k", {200, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    wire(s, v, "+", r1, "1");
    wire(s, r1, "2", r2, "1");
    wire(s, r2, "2", g, "GND");
    wire(s, v, "-", g, "GND");
    Simulator sim(s);
    auto r = sim.dcOperatingPoint();
    CHECK(r.converged);
    CHECK_NEAR(netV(s, r, r1, "2"), 5.0, 1e-6);
    CHECK_NEAR(reading(r, v)->current, 0.5e-3, 1e-9);
    CHECK_NEAR(reading(r, v)->power, -5e-3, 1e-9);
    CHECK_NEAR(reading(r, r1)->power, 2.5e-3, 1e-9);
}

TEST(sim_led_forward_voltage) {
    Project p = ledProject();
    Simulator sim(p.schematic);
    auto r = sim.dcOperatingPoint();
    CHECK(r.converged);
    const Component* led = p.schematic.findByRef("D1");
    double vf = netV(p.schematic, r, led->id, "A");
    CHECK(vf > 1.7 && vf < 2.2);
    double i = reading(r, led->id)->current;
    CHECK(i > 8e-3 && i < 10.5e-3);
}

TEST(sim_rc_transient) {
    Schematic s;
    int v = s.addComponent(ComponentKind::VoltageSource, "PULSE(0 5 20m)", {0, 0});
    int r = s.addComponent(ComponentKind::Resistor, "1k", {100, 0});
    int c = s.addComponent(ComponentKind::Capacitor, "1u", {200, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    wire(s, v, "+", r, "1");
    wire(s, r, "2", c, "1");
    wire(s, c, "2", g, "GND");
    wire(s, v, "-", g, "GND");
    Simulator sim(s);
    auto res = sim.transient(5e-3, 5e-6);
    CHECK(res.ok);
    int net = s.netOf({c, 0});
    // After one time constant (1 ms) the capacitor should be at ~63 % of 5 V.
    size_t idx = 200;
    CHECK_NEAR(res.time[idx], 1e-3, 1e-9);
    CHECK_NEAR(res.netVoltages[static_cast<size_t>(net)][idx], 5 * (1 - std::exp(-1.0)), 0.03);
    CHECK_NEAR(res.netVoltages[static_cast<size_t>(net)].back(), 5 * (1 - std::exp(-5.0)), 0.03);
}

TEST(sim_rl_dc_and_transient) {
    Schematic s;
    int v = s.addComponent(ComponentKind::VoltageSource, "PULSE(0 1 1)", {0, 0});
    int r = s.addComponent(ComponentKind::Resistor, "100", {100, 0});
    int l = s.addComponent(ComponentKind::Inductor, "10m", {200, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    wire(s, v, "+", r, "1");
    wire(s, r, "2", l, "1");
    wire(s, l, "2", g, "GND");
    wire(s, v, "-", g, "GND");
    Simulator sim(s);
    auto res = sim.transient(1e-3, 1e-6);  // tau = L/R = 100 µs
    CHECK(res.ok);
    auto& il = res.currents[l];
    CHECK_NEAR(il[100], 0.01 * (1 - std::exp(-1.0)), 3e-4);
    CHECK_NEAR(il.back(), 0.01, 1e-4);
}

TEST(sim_npn_switch_saturates) {
    Schematic s;
    int v = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    int rb = s.addComponent(ComponentKind::Resistor, "10k", {100, 0});
    int rc = s.addComponent(ComponentKind::Resistor, "1k", {200, -80});
    int q = s.addComponent(ComponentKind::NPN, "BC847", {200, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    wire(s, v, "+", rb, "1");
    wire(s, rb, "2", q, "B");
    wire(s, v, "+", rc, "1");
    wire(s, rc, "2", q, "C");
    wire(s, q, "E", g, "GND");
    wire(s, v, "-", g, "GND");
    Simulator sim(s);
    auto r = sim.dcOperatingPoint();
    CHECK(r.converged);
    double vce = netV(s, r, q, "C");
    double vbe = netV(s, r, q, "B");
    CHECK(vce < 0.3);
    CHECK(vbe > 0.6 && vbe < 0.85);
    CHECK_NEAR(reading(r, q)->current, (5 - vce) / 1000, 1e-5);
}

TEST(sim_npn_active_region) {
    // Base current 10 µA, beta 200 → Ic ≈ 2 mA, collector load small enough to stay active.
    Schematic s;
    int vcc = s.addComponent(ComponentKind::VoltageSource, "10", {0, 0});
    int ib = s.addComponent(ComponentKind::CurrentSource, "10u", {100, 0});
    int rc = s.addComponent(ComponentKind::Resistor, "1k", {200, -80});
    int q = s.addComponent(ComponentKind::NPN, "BC847", {200, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    wire(s, ib, "+", q, "B");
    wire(s, ib, "-", g, "GND");
    wire(s, vcc, "+", rc, "1");
    wire(s, rc, "2", q, "C");
    wire(s, q, "E", g, "GND");
    wire(s, vcc, "-", g, "GND");
    Simulator sim(s);
    auto r = sim.dcOperatingPoint();
    CHECK(r.converged);
    CHECK_NEAR(reading(r, q)->current, 2e-3, 0.1e-3);
}

TEST(sim_opamp_noninverting) {
    Schematic s;
    int vin = s.addComponent(ComponentKind::VoltageSource, "1", {0, 0});
    int u = s.addComponent(ComponentKind::OpAmp, "LM358", {100, 0});
    int rf = s.addComponent(ComponentKind::Resistor, "10k", {150, -60});
    int rg = s.addComponent(ComponentKind::Resistor, "10k", {50, -60});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    wire(s, vin, "+", u, "IN+");
    wire(s, vin, "-", g, "GND");
    wire(s, u, "OUT", rf, "2");
    wire(s, rf, "1", u, "IN-");
    wire(s, rg, "2", u, "IN-");
    wire(s, rg, "1", g, "GND");
    Simulator sim(s);
    auto r = sim.dcOperatingPoint();
    CHECK(r.converged);
    CHECK_NEAR(netV(s, r, u, "OUT"), 2.0, 1e-3);
}

TEST(sim_nmos_low_side_switch) {
    Schematic s;
    int v = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    int rd = s.addComponent(ComponentKind::Resistor, "1k", {100, -80});
    int m = s.addComponent(ComponentKind::NMOS, "2N7002", {100, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    wire(s, v, "+", rd, "1");
    wire(s, rd, "2", m, "D");
    wire(s, v, "+", m, "G");
    wire(s, m, "S", g, "GND");
    wire(s, v, "-", g, "GND");
    Simulator sim(s);
    auto r = sim.dcOperatingPoint();
    CHECK(r.converged);
    CHECK(netV(s, r, m, "D") < 0.1);
}

TEST(sim_no_ground_reports_error) {
    Schematic s;
    int v = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    int r = s.addComponent(ComponentKind::Resistor, "1k", {100, 0});
    wire(s, v, "+", r, "1");
    wire(s, v, "-", r, "2");
    Simulator sim(s);
    auto res = sim.dcOperatingPoint();
    CHECK(!res.converged);
    CHECK(!res.error.empty());
}

// ======================================================================= PCB

namespace {
int drcErrors(const Project& p, std::string* first = nullptr) {
    int n = 0;
    for (const auto& v : p.pcb.runDRC(p.schematic))
        if (v.severity == Severity::Error) {
            if (first && n == 0) *first = v.code + ": " + v.message;
            ++n;
        }
    return n;
}

Project amplifierProject() {
    // Non-inverting amplifier + LED driver + decoupling — a denser board.
    Project p;
    auto& s = p.schematic;
    int j = s.addComponent(ComponentKind::Connector, "PWR", {0, 0});
    int vcc = s.addComponent(ComponentKind::NetLabel, "VCC", {-40, -10});
    int gnd = s.addComponent(ComponentKind::Ground, "", {-40, 30});
    wire(s, j, "1", vcc, "N");
    wire(s, j, "2", gnd, "GND");
    int u = s.addComponent(ComponentKind::OpAmp, "LM358", {200, 0});
    int rin = s.addComponent(ComponentKind::Resistor, "10k", {100, 10});
    int rb = s.addComponent(ComponentKind::Resistor, "10k", {100, 60});
    int rf = s.addComponent(ComponentKind::Resistor, "22k", {200, -60});
    int rg = s.addComponent(ComponentKind::Resistor, "10k", {120, -60});
    int c1 = s.addComponent(ComponentKind::Capacitor, "100n", {300, 60});
    int q = s.addComponent(ComponentKind::NPN, "BC847", {320, 0});
    int rl = s.addComponent(ComponentKind::Resistor, "330", {320, -80});
    int d = s.addComponent(ComponentKind::LED, "Green", {380, -80});
    int rbq = s.addComponent(ComponentKind::Resistor, "4k7", {270, 0});
    int l2 = s.addComponent(ComponentKind::NetLabel, "VCC", {80, 10});
    int l3 = s.addComponent(ComponentKind::NetLabel, "VCC", {300, -120});
    int g2 = s.addComponent(ComponentKind::Ground, "", {100, 100});
    int g3 = s.addComponent(ComponentKind::Ground, "", {320, 60});
    wire(s, l2, "N", rin, "1");
    wire(s, rin, "2", u, "IN+");
    wire(s, rb, "1", u, "IN+");
    wire(s, rb, "2", g2, "GND");
    wire(s, rg, "1", g2, "GND");
    wire(s, rg, "2", u, "IN-");
    wire(s, rf, "1", u, "IN-");
    wire(s, rf, "2", u, "OUT");
    wire(s, u, "OUT", rbq, "1");
    wire(s, rbq, "2", q, "B");
    wire(s, q, "E", g3, "GND");
    wire(s, l3, "N", rl, "1");
    wire(s, rl, "2", d, "A");
    wire(s, d, "K", q, "C");
    wire(s, c1, "1", l3, "N");
    wire(s, c1, "2", g3, "GND");
    p.schematicChanged();
    return p;
}
}  // namespace

TEST(pcb_place_and_route_led) {
    Project p = ledProject();
    p.pcb.settings.width = 30;
    p.pcb.settings.height = 20;
    p.pcb.autoPlace(p.schematic, true);
    for (const auto& c : p.schematic.components())
        if (c.hasFootprint()) CHECK(c.pcb.placed);
    CHECK(!p.pcb.ratsnest(p.schematic).empty());
    RouteStats st = p.pcb.autoRoute(p.schematic);
    CHECK(st.failed == 0);
    CHECK(st.connections == 3);
    CHECK(p.pcb.ratsnest(p.schematic).empty());
    std::string first;
    CHECK(drcErrors(p, &first) == 0);
    if (!first.empty()) std::printf("    first DRC error: %s\n", first.c_str());
}

TEST(pcb_route_amplifier_drc_clean) {
    Project p = amplifierProject();
    p.pcb.settings.width = 40;
    p.pcb.settings.height = 30;
    p.pcb.autoPlace(p.schematic, true);
    RouteStats st = p.pcb.autoRoute(p.schematic);
    std::printf("    %d/%d connections, %d vias, %.1f mm\n", st.routed, st.connections, st.vias, st.trackLength);
    CHECK(st.failed == 0);
    std::string first;
    CHECK(drcErrors(p, &first) == 0);
    if (!first.empty()) std::printf("    first DRC error: %s\n", first.c_str());
}

TEST(pcb_fit_board_keeps_design_valid) {
    Project p = amplifierProject();
    p.pcb.settings.width = 80;
    p.pcb.settings.height = 60;
    p.pcb.autoPlace(p.schematic, true);
    p.pcb.autoRoute(p.schematic);
    size_t tracks = p.pcb.tracks.size();
    CHECK(p.pcb.fitBoardToComponents(p.schematic, 2.0));
    CHECK(p.pcb.settings.width < 80);
    CHECK(p.pcb.settings.height < 60);
    CHECK(p.pcb.tracks.size() == tracks);
    std::string first;
    CHECK(drcErrors(p, &first) == 0);  // shifted copper must still connect and keep clearance
    if (!first.empty()) std::printf("    first DRC error: %s\n", first.c_str());
    for (const auto& c : p.schematic.components())
        if (c.hasFootprint()) {
            Rect cy = p.pcb.courtyard(c);
            CHECK(cy.x0 >= 0 && cy.y0 >= 0 && cy.x1 <= p.pcb.settings.width && cy.y1 <= p.pcb.settings.height);
        }
}

TEST(pcb_drc_detects_short_and_unrouted) {
    Project p = ledProject();
    p.pcb.autoPlace(p.schematic, true);
    auto pads = p.pcb.pads(p.schematic);
    // Draw a track between two pads of different nets → short.
    Track t;
    t.layer = kTopLayer;
    t.width = 0.25;
    for (const auto& a : pads)
        for (const auto& b : pads)
            if (a.net != b.net && !a.throughHole && !b.throughHole && a.componentId == b.componentId) {
                t.a = a.position;
                t.b = b.position;
                t.net = a.net;
            }
    p.pcb.addTrack(t);
    bool shortFound = false, unrouted = false;
    for (const auto& v : p.pcb.runDRC(p.schematic)) {
        shortFound |= v.code == "DRC_SHORT";
        unrouted |= v.code == "DRC_UNROUTED";
    }
    CHECK(shortFound);
    CHECK(unrouted);
}

TEST(pcb_routing_survives_schematic_edit) {
    Project p = ledProject();
    p.pcb.autoPlace(p.schematic, true);
    p.pcb.autoRoute(p.schematic);
    size_t before = p.pcb.tracks.size();
    CHECK(before > 0);
    // Adding an unrelated component shifts net indices; copper must be re-associated, not dropped.
    p.schematic.addComponent(ComponentKind::Resistor, "1k", {500, 500});
    p.schematicChanged();
    CHECK(p.pcb.tracks.size() == before);
    // Deleting every part leaves the copper without any pads: it is orphaned and must be pruned.
    std::vector<int> ids;
    for (const auto& c : p.schematic.components()) ids.push_back(c.id);
    for (int id : ids) p.schematic.removeComponent(id);
    p.schematicChanged();
    CHECK(p.pcb.tracks.empty());
    CHECK(p.pcb.vias.empty());
}

TEST(pcb_single_layer_routing) {
    Project p = ledProject();
    p.pcb.settings.layerCount = 1;
    p.pcb.settings.width = 30;
    p.pcb.settings.height = 20;
    p.pcb.autoPlace(p.schematic, true);
    RouteStats st = p.pcb.autoRoute(p.schematic);
    CHECK(st.failed == 0);
    CHECK(st.vias == 0);
    CHECK(p.pcb.vias.empty());
    for (const auto& t : p.pcb.tracks) CHECK(t.layer == 0);
    std::string first;
    CHECK(drcErrors(p, &first) == 0);
    if (!first.empty()) std::printf("    first DRC error: %s\n", first.c_str());
}

TEST(pcb_multi_layer_routing) {
    for (int layers : {4, 6}) {
        Project p = amplifierProject();
        p.pcb.settings.layerCount = layers;
        p.pcb.settings.width = 30;
        p.pcb.settings.height = 24;
        p.pcb.autoPlace(p.schematic, true);
        RouteStats st = p.pcb.autoRoute(p.schematic);
        std::printf("    %d layers: %d/%d routed, %d vias\n", layers, st.routed, st.connections, st.vias);
        CHECK(st.failed == 0);
        for (const auto& t : p.pcb.tracks) CHECK(t.layer >= 0 && t.layer < layers);
        std::string first;
        CHECK(drcErrors(p, &first) == 0);
        if (!first.empty()) std::printf("    first DRC error: %s\n", first.c_str());
        // Every layer exports as Gerber; inner layers are marked "Inr".
        std::string inner = exportCopperGerber(p.schematic, p.pcb, 1);
        CHECK(inner.find("Copper,L2,Inr") != std::string::npos);
        std::string bottom = exportGerber(p.schematic, p.pcb, GerberLayer::BottomCopper);
        CHECK(bottom.find("Copper,L" + std::to_string(layers) + ",Bot") != std::string::npos);
        Mesh layerMesh = buildCopperLayerMesh(p.schematic, p.pcb, 0);
        CHECK(layerMesh.vertexCount() > 0);
    }
}

TEST(pcb_dense_board_uses_inner_layers) {
    // A crowded grid of resistors with crossing connections: 4 layers should route at least as much as 2.
    auto build = [](int layers) {
        Project p;
        auto& s = p.schematic;
        std::vector<int> r;
        for (int i = 0; i < 12; ++i) r.push_back(s.addComponent(ComponentKind::Resistor, "1k", {i * 80.0, 0}));
        for (int i = 0; i < 12; ++i) s.connect({r[static_cast<size_t>(i)], 0}, {r[static_cast<size_t>((i * 5 + 3) % 12)], 1});
        p.pcb.settings.layerCount = layers;
        p.pcb.settings.width = 16;
        p.pcb.settings.height = 14;
        p.pcb.autoPlace(p.schematic, true);
        return p.pcb.autoRoute(p.schematic);
    };
    RouteStats two = build(2), four = build(4);
    std::printf("    2 layers %d/%d (%d vias), 4 layers %d/%d (%d vias)\n", two.routed, two.connections, two.vias,
                four.routed, four.connections, four.vias);
    CHECK(four.routed >= two.routed);
}

TEST(pcb_router_keeps_vias_out_of_smd_pads) {
    // Dense SMD board: every via the router drops must sit outside SMD pads (no DRC_VIA_IN_PAD).
    for (int layers : {2, 4, 6}) {
        Project p = amplifierProject();
        p.pcb.settings.layerCount = layers;
        p.pcb.settings.width = 22;
        p.pcb.settings.height = 16;
        p.pcb.autoPlace(p.schematic, true);
        RouteStats st = p.pcb.autoRoute(p.schematic);
        int viaInPad = 0;
        for (const auto& v : p.pcb.runDRC(p.schematic)) viaInPad += v.code == "DRC_VIA_IN_PAD";
        std::printf("    %d layers: %d/%d routed, %d vias, %d via-in-pad\n", layers, st.routed, st.connections, st.vias, viaInPad);
        CHECK(viaInPad == 0);
    }
}

namespace {
CustomPartSpec ne555Spec(const std::string& package) {
    CustomPartSpec s;
    s.name = "NE555";
    s.manufacturer = "Texas Instruments";
    s.description = "Precision timer";
    s.package.type = package;
    const char* names[8] = {"GND", "TRIG", "OUT", "RESET", "CONT", "THRES", "DISCH", "VCC"};
    const PinType types[8] = {PinType::PowerIn, PinType::Input, PinType::Output, PinType::Input,
                              PinType::Passive, PinType::Input, PinType::OpenCollector, PinType::PowerIn};
    for (int i = 0; i < 8; ++i) s.pins.push_back({std::to_string(i + 1), names[i], types[i], ""});
    return s;
}
}  // namespace

TEST(custom_part_generation) {
    auto& reg = CustomPartRegistry::instance();
    auto dip = reg.registerPart(ne555Spec("DIP"));
    CHECK(dip->footprint.pads.size() == 8);
    CHECK(dip->footprint.pads[0].throughHole);
    CHECK(dip->footprint.pads[0].pinIndex == 0 && dip->footprint.pads[7].pinIndex == 7);
    CHECK(dip->def.pins.size() == 8);
    CHECK(dip->def.pins[0].offset.x < 0 && dip->def.pins[7].offset.x > 0);  // DIP order: 1 left, 8 right
    CHECK(dip->def.pins[0].offset.y == dip->def.pins[7].offset.y);          // pins 1 and 8 face each other
    CHECK(reg.registerPart(ne555Spec("DIP"))->id == dip->id);               // idempotent
    CHECK(reg.registerPart(ne555Spec("SOIC"))->id != dip->id);

    // Every package generates a footprint whose pads do not overlap.
    struct Case { const char* pkg; int pins; size_t pads; };
    for (Case c : {Case{"SOIC", 14, 14}, Case{"TSSOP", 20, 20}, Case{"QFN", 16, 16}, Case{"LQFP", 32, 32},
                   Case{"SOT23", 5, 5}, Case{"SOT23", 3, 3}, Case{"HEADER", 6, 6}, Case{"TO220", 3, 3}, Case{"DIP", 28, 28}}) {
        CustomPartSpec s;
        s.name = std::string("T_") + c.pkg + std::to_string(c.pins);
        s.package.type = c.pkg;
        for (int i = 1; i <= c.pins; ++i) s.pins.push_back({std::to_string(i), "P" + std::to_string(i), PinType::Passive, ""});
        auto part = reg.registerPart(s);
        CHECK(part->footprint.pads.size() == c.pads);
        bool overlap = false;
        const auto& pads = part->footprint.pads;
        for (size_t i = 0; i < pads.size(); ++i)
            for (size_t j = i + 1; j < pads.size(); ++j)
                overlap |= rectRectDistance(Rect::centered(pads[i].offset, pads[i].size.x, pads[i].size.y),
                                            Rect::centered(pads[j].offset, pads[j].size.x, pads[j].size.y)) < 0.15;
        if (overlap) std::printf("    overlapping pads in %s\n", s.name.c_str());
        CHECK(!overlap);
    }

    // QFN exposed pad from an "EP" pin.
    CustomPartSpec q;
    q.name = "QFN_EP";
    q.package.type = "QFN";
    for (int i = 1; i <= 16; ++i) q.pins.push_back({std::to_string(i), "P" + std::to_string(i), PinType::Passive, ""});
    q.pins.push_back({"EP", "GND", PinType::PowerIn, ""});
    CHECK(reg.registerPart(q)->footprint.pads.size() == 17);

    // Validation.
    CustomPartSpec bad = ne555Spec("SOIC");
    bad.pins[1].number = "1";
    bool threw = false;
    try { reg.registerPart(bad); } catch (const JsonError&) { threw = true; }
    CHECK(threw);

    // Lenient JSON (as produced by language models).
    CustomPartSpec parsed = customPartSpecFromJson(Json::parse(
        R"({"name":"LM7805","package":{"type":"TO-220"},"pins":[{"number":1,"name":"IN","type":"power"},{"number":2,"name":"GND","type":"ground"},{"number":3,"name":"OUT","type":"power out"}]})"));
    CHECK(parsed.package.type == "TO220");
    CHECK(parsed.pins.size() == 3 && parsed.pins[0].type == PinType::PowerIn && parsed.pins[2].type == PinType::PowerOut);
    int pinsFromName = 0;
    (void)pinsFromName;
    CustomPartSpec soic = customPartSpecFromJson(Json::parse(R"({"name":"X","package":"SOIC-8","pins":[{"number":"1","name":"A"}]})"));
    CHECK(soic.package.type == "SOIC" && soic.package.pinCount == 8);
}

TEST(custom_part_in_design) {
    Project p;
    std::string id = p.addCustomPart(ne555Spec("SOIC"));
    auto& s = p.schematic;
    int u = s.addCustomComponent(id, "", {200, 0});
    CHECK(u >= 0);
    CHECK(s.find(u)->ref == "U1");
    CHECK(s.find(u)->value == "NE555");
    CHECK(s.pinIndex(u, "VCC") == 7);
    CHECK(s.pinIndex(u, "8") == 7);  // datasheet pin number
    int v = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    int r = s.addComponent(ComponentKind::Resistor, "1k", {100, -60});
    s.connect({v, 1}, {g, 0});
    s.connect({v, 0}, {r, 0});
    s.connect({r, 1}, {u, s.pinIndex(u, "VCC")});

    auto codes = [&]() {
        std::vector<std::string> out;
        for (const auto& e : s.runERC()) out.push_back(e.code);
        return out;
    };
    auto has = [](const std::vector<std::string>& v, const char* c) { return std::find(v.begin(), v.end(), c) != v.end(); };
    CHECK(has(codes(), "ERC_POWER_PIN_UNCONNECTED"));  // GND pin open
    s.connect({u, s.pinIndex(u, "GND")}, {g, 0});
    CHECK(!has(codes(), "ERC_POWER_PIN_UNCONNECTED"));
    CHECK(has(codes(), "ERC_NOT_SIMULATED"));

    // DC still solves around the un-modelled IC.
    Simulator sim(s);
    CHECK(sim.dcOperatingPoint().converged);

    // Place, route and persist.
    p.schematicChanged();
    p.pcb.autoPlace(p.schematic, true);
    RouteStats st = p.pcb.autoRoute(p.schematic);
    CHECK(st.failed == 0);
    std::string first;
    CHECK(drcErrors(p, &first) == 0);
    if (!first.empty()) std::printf("    first DRC error: %s\n", first.c_str());
    Json saved = p.toJson();
    Project q = Project::fromJson(Json::parse(saved.dump()));
    CHECK(q.customLibrary.size() == 1);
    CHECK(q.schematic.findByRef("U1")->def().name == "NE555");
    CHECK(q.toJson().dump() == saved.dump());
    CHECK(exportBomCsv(q.schematic).find("SOIC-8") != std::string::npos);
    CHECK(!p.removeCustomPart(id));  // still used

    // Editing the part (rename a pin, drop pin 5) keeps wires on matching pin numbers.
    CustomPartSpec edited = ne555Spec("SOIC");
    edited.pins[7].name = "VDD";
    edited.pins.erase(edited.pins.begin() + 4);
    std::string id2 = p.addCustomPart(edited);
    size_t wiresBefore = s.wires().size();
    CHECK(s.replaceCustomPart(id, id2) == 1);
    CHECK(s.wires().size() == wiresBefore);
    CHECK(s.pinIndex(u, "VDD") >= 0);
    CHECK(p.removeCustomPart(id));
}

TEST(standard_values_and_parts) {
    CHECK_NEAR(nearestStandardValue(4800, ESeries::E24), 4700, 1e-9);
    CHECK_NEAR(nearestStandardValue(9.6e-9, ESeries::E12), 10e-9, 1e-18);   // rolls into the next decade
    CHECK_NEAR(nearestStandardValue(330, ESeries::E12), 330, 1e-9);
    CHECK_NEAR(nearestStandardValue(4990, ESeries::E96), 4990, 1e-9);
    CHECK(isStandardValue(4700, ESeries::E24));
    CHECK(isStandardValue(1e5, ESeries::E12));
    CHECK(!isStandardValue(4800, ESeries::E24));
    CHECK(!isStandardValue(-1, ESeries::E24));

    CHECK(standardParts().size() >= 12);
    CHECK(findStandardPart("ne555") != nullptr);
    CHECK(findStandardPart("does-not-exist") == nullptr);
    for (const auto& sp : standardParts()) {
        auto part = CustomPartRegistry::instance().registerPart(sp.spec);
        CHECK(part->footprint.pads.size() >= sp.spec.pins.size());
        // Every package pad maps to a pin (no anonymous pads on the standard parts).
        for (const auto& pad : part->footprint.pads) CHECK(pad.pinIndex >= 0);
    }
    const StandardPart* mega = findStandardPart("ATmega328P");
    CHECK(mega && mega->spec.pins.size() == 28 && mega->spec.pins[6].name == "VCC");
}

namespace {
bool hasCode(const std::vector<RuleViolation>& list, const std::string& code) {
    return std::any_of(list.begin(), list.end(), [&](const RuleViolation& v) { return v.code == code; });
}

/// V1 (+) → R → device pin A, device pin B → GND.
Schematic seriesCircuit(const char* volts, const char* ohms, ComponentKind kind, const char* value, bool reversed = false) {
    Schematic s;
    int v = s.addComponent(ComponentKind::VoltageSource, volts, {0, 0});
    int r = s.addComponent(ComponentKind::Resistor, ohms, {100, 0});
    int d = s.addComponent(kind, value, {200, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    s.connect({v, 0}, {r, 0});
    s.connect({r, 1}, {d, reversed ? 1 : 0});
    s.connect({d, reversed ? 0 : 1}, {g, 0});
    s.connect({v, 1}, {g, 0});
    return s;
}
}  // namespace

TEST(circuit_validation_rules) {
    // Clean indicator: no warnings.
    Project ok = ledProject();
    for (const auto& v : validateCircuit(ok.schematic)) CHECK(v.severity == Severity::Info);

    CHECK(hasCode(validateCircuit(seriesCircuit("5", "4.8k", ComponentKind::LED, "Red")), "VAL_NONSTANDARD_VALUE"));
    CHECK(!hasCode(validateCircuit(seriesCircuit("5", "4.7k", ComponentKind::LED, "Red")), "VAL_NONSTANDARD_VALUE"));
    CHECK(!hasCode(validateCircuit(seriesCircuit("5", "4.99k", ComponentKind::LED, "Red")), "VAL_NONSTANDARD_VALUE"));  // E96
    CHECK(hasCode(validateCircuit(seriesCircuit("5", "47", ComponentKind::LED, "Red")), "VAL_LED_CURRENT"));
    CHECK(hasCode(validateCircuit(seriesCircuit("5", "330", ComponentKind::LED, "Red", true)), "VAL_REVERSE_BIAS"));
    auto heavy = validateCircuit(seriesCircuit("12", "100", ComponentKind::Resistor, "100"));
    CHECK(hasCode(heavy, "VAL_RESISTOR_POWER"));
    auto shorted = validateCircuit(seriesCircuit("5", "1", ComponentKind::Resistor, "1"));
    CHECK(hasCode(shorted, "VAL_SUPPLY_CURRENT"));
    CHECK(hasCode(validateCircuit(seriesCircuit("12", "10", ComponentKind::Fuse, "100m")), "VAL_FUSE_OVERLOAD"));
    CHECK(hasCode(validateCircuit(seriesCircuit("5", "1", ComponentKind::Diode, "1N4148")), "VAL_DIODE_CURRENT"));

    // Saturated op-amp: gain of 1000 on a 1 V input.
    Schematic s;
    int vin = s.addComponent(ComponentKind::VoltageSource, "1", {0, 0});
    int u = s.addComponent(ComponentKind::OpAmp, "LM358", {100, 0});
    int rf = s.addComponent(ComponentKind::Resistor, "1M", {150, -60});
    int rg = s.addComponent(ComponentKind::Resistor, "1k", {50, -60});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    s.connect({vin, 0}, {u, 0});
    s.connect({vin, 1}, {g, 0});
    s.connect({u, 2}, {rf, 1});
    s.connect({rf, 0}, {u, 1});
    s.connect({rg, 1}, {u, 1});
    s.connect({rg, 0}, {g, 0});
    CHECK(hasCode(validateCircuit(s), "VAL_OPAMP_SATURATED"));

    // Decoupling on a standard IC's supply rail.
    Project p;
    std::string id = p.addCustomPart(findStandardPart("NE555")->spec);
    auto& sch = p.schematic;
    int timer = sch.addCustomComponent(id, "", {200, 0});
    int v = sch.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    int gnd = sch.addComponent(ComponentKind::Ground, "", {0, 80});
    sch.connect({v, 0}, {timer, sch.pinIndex(timer, "8")});
    sch.connect({v, 1}, {gnd, 0});
    sch.connect({timer, sch.pinIndex(timer, "1")}, {gnd, 0});
    CHECK(hasCode(validateCircuit(sch), "VAL_NO_DECOUPLING"));
    int c = sch.addComponent(ComponentKind::Capacitor, "100n", {260, 0});
    sch.connect({c, 0}, {timer, sch.pinIndex(timer, "8")});
    sch.connect({c, 1}, {gnd, 0});
    CHECK(!hasCode(validateCircuit(sch), "VAL_NO_DECOUPLING"));
}

TEST(pcb_rule_presets_and_manufacturability_checks) {
    Project p = ledProject();
    CHECK(designRulePresets().size() == 5);
    CHECK(p.pcb.settings.applyPreset("Prototype (Conservative)"));
    CHECK_NEAR(p.pcb.settings.trackWidth, 0.30, 1e-9);
    CHECK(!p.pcb.settings.applyPreset("nope"));
    p.pcb.autoPlace(p.schematic, true);
    CHECK(p.pcb.autoRoute(p.schematic).failed == 0);
    std::string first;
    CHECK(drcErrors(p, &first) == 0);
    if (!first.empty()) std::printf("    first DRC error: %s\n", first.c_str());

    // Persisted with the project.
    Project q = Project::fromJson(Json::parse(p.toJson().dump()));
    CHECK(q.pcb.settings.rulePreset == "Prototype (Conservative)");
    CHECK_NEAR(q.pcb.settings.minHoleToHole, 0.50, 1e-9);

    auto codes = [](const Project& pr) {
        std::vector<std::string> v;
        for (const auto& e : pr.pcb.runDRC(pr.schematic)) v.push_back(e.code);
        return v;
    };
    auto has = [](const std::vector<std::string>& v, const char* c) { return std::find(v.begin(), v.end(), c) != v.end(); };

    // Thin track, tiny drill, thin annular ring, crowded holes, stub, acute join, via in pad.
    Project bad = p;
    int net = bad.pcb.tracks.front().net;
    Track thin = bad.pcb.tracks.front();
    thin.width = 0.08;
    bad.pcb.addTrack(thin);
    Via v1;
    v1.net = net;
    v1.position = {3, 3};
    v1.drill = 0.1;
    v1.diameter = 0.25;
    bad.pcb.addVia(v1);
    Via v2 = v1;
    v2.position = {3.3, 3};
    v2.drill = 0.3;
    v2.diameter = 0.6;
    bad.pcb.addVia(v2);
    Track stub;
    stub.net = net;
    stub.layer = 0;
    stub.width = 0.25;
    stub.a = {2, 8};
    stub.b = {6, 8};
    bad.pcb.addTrack(stub);
    Track acute = stub;
    acute.a = {6, 8};
    acute.b = {2, 9};
    bad.pcb.addTrack(acute);
    auto pads = bad.pcb.pads(bad.schematic);
    for (const auto& pad : pads)
        if (!pad.throughHole && pad.net >= 0) {
            Via inPad;
            inPad.net = pad.net;
            inPad.position = pad.position;
            bad.pcb.addVia(inPad);
            break;
        }
    auto c = codes(bad);
    CHECK(has(c, "DRC_TRACK_WIDTH"));
    CHECK(has(c, "DRC_DRILL_SIZE"));
    CHECK(has(c, "DRC_ANNULAR_RING"));
    CHECK(has(c, "DRC_HOLE_SPACING"));
    CHECK(has(c, "DRC_DANGLING_TRACK"));
    CHECK(has(c, "DRC_ACUTE_ANGLE"));
    CHECK(has(c, "DRC_VIA_IN_PAD"));

    // IPC-2221: ~1 A on 0.3 mm outer track at 10 °C rise needs ≈0.3 mm; 3 A needs far more.
    CHECK(ipc2221TrackWidth(1.0, 10, 1, false) > 0.25 && ipc2221TrackWidth(1.0, 10, 1, false) < 0.40);
    CHECK(ipc2221TrackWidth(1.0, 10, 1, true) > ipc2221TrackWidth(1.0, 10, 1, false));
    Project power = ledProject();
    power.schematic.setValue(power.schematic.findByRef("R1")->id, "1");  // ~3 A through a 0.3 mm track
    power.schematicChanged();
    power.pcb.settings.applyPreset("Prototype (Conservative)");
    power.pcb.autoPlace(power.schematic, true);
    power.pcb.autoRoute(power.schematic);
    CHECK(has(codes(power), "DRC_TRACK_CURRENT"));

    // Graded clearance: between the fab minimum and the design rule is a warning, not an error.
    Project graded = ledProject();
    graded.pcb.autoPlace(graded.schematic, true);
    graded.pcb.autoRoute(graded.schematic);
    graded.pcb.settings.clearance = 5.0;  // absurd design rule, fab minimum unchanged
    auto gv = graded.pcb.runDRC(graded.schematic);
    CHECK(std::any_of(gv.begin(), gv.end(), [](const RuleViolation& v) { return v.code == "DRC_CLEARANCE_RULE"; }));
    CHECK(std::none_of(gv.begin(), gv.end(), [](const RuleViolation& v) { return v.code == "DRC_CLEARANCE"; }));
}

// ======================================================================= persistence & exports

TEST(design_verification_pipeline) {
    auto stage = [](const VerificationReport& r, const char* id) -> const VerificationStage* {
        for (const auto& s : r.stages)
            if (s.id == id) return &s;
        return nullptr;
    };

    // Empty design fails at ERC; later stages are skipped.
    VerificationReport empty = verifyDesign(Project{});
    CHECK(empty.verdict == StageStatus::Fail);
    CHECK(empty.stages.size() == 7);
    CHECK(stage(empty, "routing")->status == StageStatus::Skipped);

    // Schematic only: no placement yet.
    Project p = ledProject();
    for (const auto& c : p.schematic.components()) p.schematic.find(c.id)->pcb.placed = false;
    p.pcb.clearRouting();
    VerificationReport unplaced = verifyDesign(p);
    CHECK(stage(unplaced, "erc")->status != StageStatus::Fail);
    CHECK(stage(unplaced, "simulation")->status == StageStatus::Pass);
    CHECK(stage(unplaced, "placement")->status == StageStatus::Fail);
    CHECK(!unplaced.passed());

    // Placed but not routed.
    p.pcb.autoPlace(p.schematic, true);
    VerificationReport unrouted = verifyDesign(p);
    CHECK(stage(unrouted, "placement")->status == StageStatus::Pass);
    CHECK(stage(unrouted, "routing")->status == StageStatus::Fail);
    CHECK(unrouted.verdict == StageStatus::Fail);

    // Fully routed: every stage passes and every manufacturing file is checked.
    CHECK(p.pcb.autoRoute(p.schematic).failed == 0);
    VerificationReport done = verifyDesign(p);
    for (const auto& s : done.stages)
        if (s.status == StageStatus::Fail) std::printf("    stage %s failed: %s\n", s.id.c_str(), s.summary.c_str());
    CHECK(done.passed());
    CHECK(done.errors == 0);
    const VerificationStage* mfg = stage(done, "manufacturing");
    CHECK(mfg && mfg->status == StageStatus::Pass);
    CHECK(mfg && mfg->details.size() == static_cast<size_t>(p.pcb.settings.layerCount + 4 + 4));
    std::string md = done.toMarkdown();
    CHECK(md.find("# Design Verification Report") != std::string::npos);
    CHECK(md.find("| Routing Completion | PASS |") != std::string::npos);
    Json j = Json::parse(done.toJson().dump());
    CHECK(j["verdict"].asString() == stageStatusName(done.verdict));
    CHECK(j["stages"].size() == 7);

    // Four layers: one copper Gerber per layer is verified.
    p.pcb.settings.layerCount = 4;
    CHECK(p.pcb.autoRoute(p.schematic).failed == 0);
    VerificationReport four = verifyDesign(p);
    CHECK(stage(four, "manufacturing")->details.size() == 12);
    CHECK(four.passed());

    // An overloaded resistor fails circuit validation and therefore the design.
    Project hot = p;
    for (const auto& c : p.schematic.components())
        if (c.kind == ComponentKind::Resistor) hot.schematic.setValue(c.id, "10");
    VerificationReport overloaded = verifyDesign(hot);
    CHECK(stage(overloaded, "validation")->status == StageStatus::Fail);
    CHECK(!overloaded.passed());
}

TEST(project_json_roundtrip) {
    Project p = amplifierProject();
    p.requirements = "Amplify a sensor signal and light an LED.";
    p.pcb.autoPlace(p.schematic, true);
    p.pcb.autoRoute(p.schematic);
    Json saved = p.toJson();
    Project q = Project::fromJson(Json::parse(saved.dump(true)));
    CHECK(q.name == p.name);
    CHECK(q.requirements == p.requirements);
    CHECK(q.schematic.components().size() == p.schematic.components().size());
    CHECK(q.schematic.wires().size() == p.schematic.wires().size());
    CHECK(q.pcb.tracks.size() == p.pcb.tracks.size());
    CHECK(q.pcb.vias.size() == p.pcb.vias.size());
    CHECK(q.toJson().dump() == saved.dump());
    // New components after reload must not collide with restored ids.
    int id = q.schematic.addComponent(ComponentKind::Resistor, "1k", {0, 0});
    for (const auto& c : p.schematic.components()) CHECK(c.id != id);
    bool threw = false;
    try {
        Project::fromJson(Json::parse("{\"format\":\"other\"}"));
    } catch (const JsonError&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(exports_are_well_formed) {
    Project p = amplifierProject();
    p.pcb.autoPlace(p.schematic, true);
    p.pcb.autoRoute(p.schematic);
    std::string spice = exportSpiceNetlist(p.schematic, "amp");
    CHECK(spice.find(".end") != std::string::npos);
    CHECK(spice.find("XU1") != std::string::npos);
    CHECK(spice.find(".subckt OPAMP_IDEAL") != std::string::npos);
    std::string bom = exportBomCsv(p.schematic);
    CHECK(bom.rfind("Item,Quantity", 0) == 0);
    CHECK(bom.find("R_0805") != std::string::npos);
    for (auto layer : {GerberLayer::TopCopper, GerberLayer::BottomCopper, GerberLayer::TopMask, GerberLayer::TopSilk,
                       GerberLayer::EdgeCuts}) {
        std::string g = exportGerber(p.schematic, p.pcb, layer);
        CHECK(g.find("%FSLAX46Y46*%") != std::string::npos);
        CHECK(g.size() > 6 && g.substr(g.size() - 5) == "M02*\n");
    }
    std::string top = exportGerber(p.schematic, p.pcb, GerberLayer::TopCopper);
    CHECK(top.find("D03*") != std::string::npos);
    CHECK(top.find("D01*") != std::string::npos);
    std::string drill = exportExcellonDrill(p.schematic, p.pcb);
    CHECK(drill.rfind("M48", 0) == 0);
    CHECK(drill.find("T1C1.000") != std::string::npos || drill.find("C1.000") != std::string::npos);
    std::string pnp = exportPickAndPlaceCsv(p.schematic);
    CHECK(pnp.find("U1") != std::string::npos);
}

TEST(mesh_is_valid) {
    Project p = amplifierProject();
    p.pcb.autoPlace(p.schematic, true);
    p.pcb.autoRoute(p.schematic);
    Mesh m = buildAssemblyMesh(p.schematic, p.pcb);
    CHECK(m.vertexCount() > 100);
    CHECK(m.indices.size() % 3 == 0);
    CHECK(m.normals.size() == m.positions.size());
    CHECK(m.colors.size() == m.vertexCount() * 4);
    bool finite = true, inRange = true;
    for (float f : m.positions) finite &= std::isfinite(f);
    for (uint32_t i : m.indices) inRange &= i < m.vertexCount();
    CHECK(finite);
    CHECK(inRange);
    // Top face of the board must face up.
    CHECK(m.normals[1] > 0.99f);
    std::string stl = exportStl(m, "board");
    CHECK(stl.rfind("solid board", 0) == 0);
    std::string obj = exportObj(m, "board");
    CHECK(obj.find("\nf ") != std::string::npos);
}

TEST(c_api_smoke) {
    int rc = sieda_c_api_smoke_test();
    if (rc != 0) std::printf("    c api step %d failed\n", rc);
    CHECK(rc == 0);
}

int main() {
    for (const auto& t : registry()) {
        int before = g_failures;
        std::printf("[ RUN  ] %s\n", t.name);
        t.fn();
        std::printf("[ %s ] %s\n", g_failures == before ? " OK " : "FAIL", t.name);
    }
    std::printf("\n%d checks, %d failures, %zu tests\n", g_checks, g_failures, registry().size());
    return g_failures == 0 ? 0 : 1;
}
