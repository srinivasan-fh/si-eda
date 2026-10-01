// SiEDA core unit tests — dependency-free; run via `ctest` or directly.
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "sieda/Export.hpp"
#include "sieda/Json.hpp"
#include "sieda/Mesh.hpp"
#include "sieda/Project.hpp"
#include "sieda/Simulator.hpp"
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

TEST(pcb_drc_detects_short_and_unrouted) {
    Project p = ledProject();
    p.pcb.autoPlace(p.schematic, true);
    auto pads = p.pcb.pads(p.schematic);
    // Draw a track between two pads of different nets → short.
    Track t;
    t.layer = CopperLayer::Top;
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

// ======================================================================= persistence & exports

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
