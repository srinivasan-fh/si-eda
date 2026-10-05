// SiEDA core unit tests — dependency-free; run via `ctest` or directly.
#include <map>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <thread>

#include <algorithm>

#include "sieda/Analysis.hpp"
#include "sieda/Avr.hpp"
#include "sieda/CustomParts.hpp"
#include "sieda/DeviceModels.hpp"
#include "sieda/Bom.hpp"
#include "sieda/Embedded.hpp"
#include "sieda/Export.hpp"
#include "sieda/Fabrication.hpp"
#include "sieda/Ibis.hpp"
#include "sieda/PowerIntegrity.hpp"
#include "sieda/SignalIntegrity.hpp"
#include "sieda/Channel.hpp"
#include "sieda/Eye.hpp"
#include "sieda/LossyLine.hpp"
#include "sieda/Touchstone.hpp"
#include "sieda/PdnPlanning.hpp"
#include "sieda/Firmware.hpp"
#include "sieda/Industry.hpp"
#include "sieda/InteractiveRouter.hpp"
#include "sieda/Json.hpp"
#include "sieda/Mesh.hpp"
#include "sieda/Project.hpp"
#include "sieda/LengthMatch.hpp"
#include "sieda/LibraryImport.hpp"
#include "sieda/Reliability.hpp"
#include "sieda/Aerospace.hpp"
#include "sieda/Isolation.hpp"
#include "sieda/Medical.hpp"
#include "sieda/Retail.hpp"
#include "sieda/Appliance.hpp"
#include "sieda/Memory.hpp"
#include "sieda/Naval.hpp"
#include "sieda/Automotive.hpp"
#include "sieda/Robotics.hpp"
#include "sieda/Stackup.hpp"
#include "sieda/Simulator.hpp"
#include "sieda/SpiceModels.hpp"
#include "sieda/Noise.hpp"
#include "sieda/Waveforms.hpp"
#include "sieda/StandardParts.hpp"
#include "sieda/Validation.hpp"
#include "sieda/Verification.hpp"
#include "sieda/Units.hpp"
#include "sieda/sieda_c.h"

#include "bench/BoardGenerator.hpp"

extern "C" int sieda_c_api_smoke_test(void);
extern "C" int sieda_c_api_library_import_test(void);
extern "C" int sieda_c_api_router_test(void);

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

TEST(sim_blue_white_leds_conduct) {
    // Wide-gap LEDs (blue/white, Is ≈ 1e-26) need junction exponents near 60; a fixed exp() knee at 40 left them open.
    for (const char* colour : {"Blue", "White", "Red", "Green"}) {
        Schematic s;
        int v = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
        int r = s.addComponent(ComponentKind::Resistor, "100", {80, 0});
        int d = s.addComponent(ComponentKind::LED, colour, {160, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        wire(s, v, "+", r, "1");
        wire(s, r, "2", d, "A");
        wire(s, d, "K", g, "GND");
        wire(s, v, "-", g, "GND");
        DcResult dc = Simulator(s).dcOperatingPoint();
        CHECK(dc.converged);
        double i = 0, vf = 0;
        for (const auto& dev : dc.devices)
            if (dev.componentId == d) {
                i = dev.current;
                vf = dev.voltage;
            }
        bool wide = std::string(colour) == "Blue" || std::string(colour) == "White";
        std::printf("    %-5s Vf %.3f V, I %.2f mA\n", colour, vf, i * 1000);
        CHECK(i > 0.010 && i < 0.035);
        CHECK(wide ? (vf > 2.6 && vf < 3.3) : (vf > 1.7 && vf < 2.2));
    }
}

TEST(device_models_and_switching_stress) {
    CHECK(findDeviceModel(ComponentKind::NMOS, "SI2302") != nullptr);
    CHECK(findDeviceModel(ComponentKind::NMOS, "si2302cds-t1") != nullptr);
    CHECK(findDeviceModel(ComponentKind::NMOS, "BC847") == nullptr);  // wrong kind
    CHECK(findDeviceModel(ComponentKind::Diode, "SS14")->maxCurrent == 1.0);
    CHECK_NEAR(primaryValue("120 2W") == "120" ? 1 : 0, 1, 0);
    CHECK_NEAR(*powerRating("120 2W"), 2.0, 1e-12);
    CHECK_NEAR(*powerRating("0R1 1/2W"), 0.5, 1e-12);
    CHECK_NEAR(*powerRating("4k7/250mW"), 0.25, 1e-12);
    CHECK(primaryValue("4k7/250mW") == "4k7");
    CHECK(!powerRating("4k7"));

    // Coreless motor channel: 3.7 V LiPo, 1.2 Ω + 50 µH winding, 20 kHz PWM, Schottky flyback.
    auto motor = [](const char* fet, const char* winding) {
        Schematic s;
        int b = s.addComponent(ComponentKind::VoltageSource, "3.7", {0, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        int r = s.addComponent(ComponentKind::Resistor, winding, {80, 0});
        int l = s.addComponent(ComponentKind::Inductor, "50u", {160, 0});
        int q = s.addComponent(ComponentKind::NMOS, fet, {240, 0});
        int d = s.addComponent(ComponentKind::Diode, "SS14", {240, -80});
        int vg = s.addComponent(ComponentKind::VoltageSource, "PULSE(0 3.3 50u 0.5)", {0, 160});
        int rg = s.addComponent(ComponentKind::Resistor, "100", {120, 160});
        wire(s, b, "+", r, "1");
        wire(s, r, "2", l, "1");
        wire(s, l, "2", q, "D");
        wire(s, q, "S", g, "GND");
        wire(s, b, "-", g, "GND");
        wire(s, d, "A", q, "D");
        wire(s, d, "K", b, "+");
        wire(s, vg, "+", rg, "1");
        wire(s, rg, "2", q, "G");
        wire(s, vg, "-", g, "GND");
        return s;
    };
    auto peakDrain = [&](const char* fet) {
        Schematic s = motor(fet, "1.2 5W");
        TransientResult tr = Simulator(s).transient(1e-3, 0.5e-6);
        double peak = 0;
        for (const auto& c : s.components())
            if (c.kind == ComponentKind::NMOS)
                for (double i : tr.currents[c.id]) peak = std::max(peak, std::fabs(i));
        return peak;
    };
    double si = peakDrain("SI2302"), generic = peakDrain("2N7002");
    std::printf("    peak drain current: SI2302 %.2f A, 2N7002 %.2f A\n", si, generic);
    CHECK(si > 1.0 && si < 3.5);  // a real logic-level FET drives the coreless motor
    CHECK(generic < 0.3);         // the small-signal part saturates

    auto codes = [](const std::vector<RuleViolation>& v, const char* code) {
        int n = 0;
        for (const auto& f : v) n += f.code == code;
        return n;
    };
    // 2N7002 in a motor drive: DC (t = 0, gate low) sees nothing, the switching-stress check catches it.
    Schematic weak = motor("2N7002", "1.2 5W");
    CHECK(codes(validateCircuit(weak), "VAL_TRANSIENT_STRESS") >= 1);
    // SI2302 with a power-rated winding model: within ratings.
    Schematic good = motor("SI2302", "1.2 5W");
    auto findings = validateCircuit(good);
    for (const auto& f : findings)
        if (f.severity != Severity::Info) std::printf("    unexpected: %s %s\n", f.code.c_str(), f.message.c_str());
    CHECK(codes(findings, "VAL_TRANSIENT_STRESS") == 0);
    // The same winding as a plain 0805 resistor is flagged (2.3 W average).
    CHECK(codes(validateCircuit(motor("SI2302", "1.2")), "VAL_TRANSIENT_STRESS") >= 1);
}

TEST(behavioural_chip_models) {
    auto partId = [](const char* name) {
        return CustomPartRegistry::instance().registerPart(findStandardPart(name)->spec)->id;
    };
    auto net = [](const Schematic& s, const DcResult& dc, int comp, const char* pin) {
        return dc.netVoltages[static_cast<size_t>(s.netOf({comp, s.pinIndex(comp, pin)}))];
    };
    auto codes = [](const std::vector<RuleViolation>& v, const char* code) {
        int n = 0;
        for (const auto& f : v) n += f.code == code;
        return n;
    };
    // LDO from a 1S LiPo: regulates, then drops out as the battery sags.
    auto ldo = [&](const char* vin, const char* load) {
        Schematic s;
        int v = s.addComponent(ComponentKind::VoltageSource, vin, {0, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        int u = s.addCustomComponent(partId("XC6206P332"), "", {100, 0});
        int r = s.addComponent(ComponentKind::Resistor, load, {200, 0});
        wire(s, v, "+", u, "VIN");
        wire(s, u, "VSS", g, "GND");
        wire(s, u, "VOUT", r, "1");
        wire(s, r, "2", g, "GND");
        wire(s, v, "-", g, "GND");
        return std::make_pair(s, u);
    };
    {
        auto [s, u] = ldo("3.7", "150");  // 22 mA
        DcResult dc = Simulator(s).dcOperatingPoint();
        CHECK(dc.converged);
        CHECK_NEAR(net(s, dc, u, "VOUT"), 3.3, 0.01);
        CHECK(codes(validateCircuit(s), "VAL_REGULATOR_DROPOUT") == 0);
    }
    {
        auto [s, u] = ldo("3.4", "150");
        DcResult dc = Simulator(s).dcOperatingPoint();
        CHECK(net(s, dc, u, "VOUT") < 3.2 && net(s, dc, u, "VOUT") > 3.05);
        CHECK(codes(validateCircuit(s), "VAL_REGULATOR_DROPOUT") == 1);
    }
    {
        auto [s, u] = ldo("3.7", "5");  // would need 660 mA from a 250 mA LDO
        DcResult dc = Simulator(s).dcOperatingPoint();
        CHECK(dc.converged);
        CHECK(net(s, dc, u, "VOUT") < 1.4);  // 250 mA × 5 Ω
        CHECK(codes(validateCircuit(s), "VAL_REGULATOR_OVERLOAD") == 1);
    }
    {
        // Output back-fed from a higher rail: the regulator must not sink.
        auto [s, u] = ldo("3.7", "1k");
        int v2 = s.addComponent(ComponentKind::VoltageSource, "5", {300, 0});
        int g2 = s.addComponent(ComponentKind::Ground, "", {300, 80});
        wire(s, v2, "+", u, "VOUT");
        wire(s, v2, "-", g2, "GND");
        DcResult dc = Simulator(s).dcOperatingPoint();
        CHECK(dc.converged);
        for (const auto& d : dc.devices)
            if (d.componentId == u && d.subIndex == 0) CHECK(std::fabs(d.current) < 1e-6);
    }
    // LM317 adjustable: 1.25 V × (1 + 715/240) + I_ADJ·R2 ≈ 5.01 V; a 7805 dissipating 7 W in TO-220 free air is an error.
    {
        Schematic s;
        int v = s.addComponent(ComponentKind::VoltageSource, "12", {0, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        int u = s.addCustomComponent(partId("LM317"), "", {100, 0});
        int r1 = s.addComponent(ComponentKind::Resistor, "240", {200, 0});
        int r2 = s.addComponent(ComponentKind::Resistor, "715", {200, 80});
        wire(s, v, "+", u, "IN");
        wire(s, u, "OUT", r1, "1");
        wire(s, r1, "2", u, "ADJ");
        wire(s, u, "ADJ", r2, "1");
        wire(s, r2, "2", g, "GND");
        wire(s, v, "-", g, "GND");
        DcResult dc = Simulator(s).dcOperatingPoint();
        CHECK_NEAR(net(s, dc, u, "OUT"), 1.25 * (1 + 715.0 / 240.0) + 50e-6 * 715, 0.01);  // includes I_ADJ · R2
    }
    {
        Schematic s;
        int v = s.addComponent(ComponentKind::VoltageSource, "12", {0, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        int u = s.addCustomComponent(partId("LM7805"), "", {100, 0});
        int r = s.addComponent(ComponentKind::Resistor, "5 10W", {200, 0});
        wire(s, v, "+", u, "IN");
        wire(s, u, "GND", g, "GND");
        wire(s, u, "OUT", r, "1");
        wire(s, r, "2", g, "GND");
        wire(s, v, "-", g, "GND");
        auto f = validateCircuit(s);
        CHECK(codes(f, "VAL_REGULATOR_POWER") == 1);
        bool error = false;
        for (const auto& x : f) error |= x.code == "VAL_REGULATOR_POWER" && x.severity == Severity::Error;
        CHECK(error);
    }
    // TP4056 charging a 3.7 V cell from USB: constant current 1 A, no overload finding (CC is normal for a charger).
    {
        Schematic s;
        int usb = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
        int bat = s.addComponent(ComponentKind::VoltageSource, "3.7", {300, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        int g2 = s.addComponent(ComponentKind::Ground, "", {300, 80});
        int u = s.addCustomComponent(partId("TP4056"), "", {150, 0});
        wire(s, usb, "+", u, "VCC");
        wire(s, u, "GND", g, "GND");
        wire(s, u, "BAT", bat, "+");
        wire(s, usb, "-", g, "GND");
        wire(s, bat, "-", g2, "GND");
        DcResult dc = Simulator(s).dcOperatingPoint();
        CHECK(dc.converged);
        for (const auto& d : dc.devices)
            if (d.componentId == u && d.subIndex == 0) {
                CHECK_NEAR(d.current, 1.0, 1e-3);
                CHECK(d.state == 1);
            }
        CHECK(codes(validateCircuit(s), "VAL_REGULATOR_OVERLOAD") == 0);
    }
    // IC supply current: an ATmega328P draws its operating current from the rail.
    {
        Schematic s;
        int v = s.addComponent(ComponentKind::VoltageSource, "3.3", {0, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        int u = s.addCustomComponent(partId("ATmega328P"), "", {150, 0});
        wire(s, v, "+", u, "7");
        wire(s, u, "8", g, "GND");
        wire(s, v, "-", g, "GND");
        DcResult dc = Simulator(s).dcOperatingPoint();
        double supply = 0;
        for (const auto& d : dc.devices)
            if (d.componentId == v) supply = d.current;
        CHECK_NEAR(supply, 0.004, 2e-4);
    }
    // 2×4 header footprint: two columns 2.54 mm apart, IDC numbering.
    {
        auto part = CustomPartRegistry::instance().get(partId("NRF24L01_Module"));
        CHECK(part->footprint.pads.size() == 8);
        CHECK_NEAR(part->footprint.pads[1].offset.x - part->footprint.pads[0].offset.x, 2.54, 1e-9);
        CHECK_NEAR(part->footprint.pads[2].offset.y - part->footprint.pads[0].offset.y, 2.54, 1e-9);
    }
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
        // With wider (automotive) rules the router still keeps exact via-to-pad clearance.
        Project wide = amplifierProject();
        wide.pcb.settings.layerCount = layers;
        wide.pcb.settings.applyPreset("Automotive (IPC-6012 Class 3/A)");
        wide.pcb.settings.width = 26;
        wide.pcb.settings.height = 20;
        wide.pcb.autoPlace(wide.schematic, true);
        wide.pcb.autoRoute(wide.schematic);
        int clearance = 0;
        for (const auto& v : wide.pcb.runDRC(wide.schematic))
            clearance += v.code == "DRC_CLEARANCE_RULE" || v.code == "DRC_CLEARANCE" || v.code == "DRC_SHORT";
        CHECK(clearance == 0);
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
    auto shorted = validateCircuit(seriesCircuit("5", "0.5", ComponentKind::Resistor, "0.5"));  // 5 A
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
    CHECK(designRulePresets().size() == 13);
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
    power.pcb.settings.autoSizeNets = false;  // route at the default width to provoke the finding
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
    CHECK(empty.stages.size() == 8);
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
    // Copper per layer, mask ×2, silk, outline, drill, BOM, PnP, netlist, then paste, designators, IPC-356, job, zip.
    CHECK(mfg && mfg->details.size() == static_cast<size_t>(p.pcb.settings.layerCount + 4 + 4 + 5));
    std::string md = done.toMarkdown();
    CHECK(md.find("# Design Verification Report") != std::string::npos);
    CHECK(md.find("| Routing Completion | PASS |") != std::string::npos);
    Json j = Json::parse(done.toJson().dump());
    CHECK(j["verdict"].asString() == stageStatusName(done.verdict));
    CHECK(j["stages"].size() == 8);

    // Four layers: one copper Gerber per layer is verified.
    p.pcb.settings.layerCount = 4;
    CHECK(p.pcb.autoRoute(p.schematic).failed == 0);
    VerificationReport four = verifyDesign(p);
    CHECK(stage(four, "manufacturing")->details.size() == 17);
    CHECK(four.passed());

    // An overloaded resistor fails circuit validation and therefore the design.
    Project hot = p;
    for (const auto& c : p.schematic.components())
        if (c.kind == ComponentKind::Resistor) hot.schematic.setValue(c.id, "10");
    VerificationReport overloaded = verifyDesign(hot);
    CHECK(stage(overloaded, "validation")->status == StageStatus::Fail);
    CHECK(!overloaded.passed());
}

TEST(industry_profiles_and_derating) {
    CHECK(industryProfiles().size() == 21);
    for (const char* id : {"general", "robotics", "uav", "power", "automotive", "rf", "space", "marine", "industrial",
                           "medical", "defence", "networking", "vlsi", "memory"}) {
        const IndustryProfile* p = findIndustry(id);
        CHECK(p != nullptr);
        if (!p) continue;
        BoardSettings b;
        CHECK(b.applyPreset(p->rulePreset));  // every profile names a real preset
        CHECK(!p->guidance.empty() && !p->standards.empty());
    }
    CHECK(findIndustry("SPACE") != nullptr);
    CHECK(findIndustry("nope") == nullptr);

    const IndustryProfile& space = *findIndustry("space");
    PartRatings r = deratedRatings(space);
    CHECK_NEAR(r.resistorPower, 0.0625, 1e-12);
    CHECK_NEAR(r.ledCurrent, 0.015, 1e-12);
    CHECK(r.derating.find("Space") != std::string::npos);
    CHECK(deratedRatings(*findIndustry("general")).derating.empty());

    // Project: the profile sets rules and altitude and survives a save/load.
    Project p = ledProject();
    CHECK(!p.applyIndustry("unknown"));
    CHECK(p.applyIndustry("space"));
    CHECK(p.pcb.settings.rulePreset == "Space (IPC-6012 Class 3/A, ECSS)");
    CHECK(p.pcb.settings.highAltitude);
    Project q = Project::fromJson(Json::parse(p.toJson().dump()));
    CHECK(q.industry == "space");
    CHECK(q.pcb.settings.highAltitude);

    // A 330 Ω LED at 5 V (~9 mA) is fine in general, but a 150 Ω one (~20 mA) exceeds the space-derated 15 mA.
    Project led = ledProject();
    for (const auto& c : led.schematic.components())
        if (c.kind == ComponentKind::Resistor) led.schematic.setValue(c.id, "150");
    auto hasLedFinding = [](const std::vector<RuleViolation>& v) {
        for (const auto& f : v)
            if (f.code == "VAL_LED_CURRENT") return f.message.find("derating") != std::string::npos;
        return false;
    };
    CHECK(!hasLedFinding(validateCircuit(led.schematic, led.partRatings())));
    led.applyIndustry("space");
    CHECK(hasLedFinding(validateCircuit(led.schematic, led.partRatings())));

    led.pcb.autoPlace(led.schematic, true);
    CHECK(led.pcb.autoRoute(led.schematic).failed == 0);
    VerificationReport report = verifyDesign(led);
    CHECK(report.industryName == "Space");
    CHECK(report.toMarkdown().find("Industry profile:** Space") != std::string::npos);
}

TEST(router_keeps_voltage_spacing) {
    // 48 V (PoE/telecom) board on fine High-Speed rules: the router must keep the IPC-2221 B2 0.6 mm spacing, not the
    // 0.15 mm design rule, between the 48 V and ground copper; the design rule itself is left unchanged.
    Project p = ledProject();
    for (const auto& c : p.schematic.components()) {
        if (c.kind == ComponentKind::VoltageSource) p.schematic.setValue(c.id, "48");
        if (c.kind == ComponentKind::Resistor) p.schematic.setValue(c.id, "4.7k 1W");
    }
    p.schematicChanged();
    CHECK(p.pcb.settings.applyPreset("High-Speed Digital (100 Ω diff)"));
    CHECK_NEAR(voltageRoutingClearance(p.schematic, false), 0.6, 1e-12);
    CHECK_NEAR(voltageRoutingClearance(ledProject().schematic, false), 0.1, 1e-12);  // 5 V: the design rule wins
    p.pcb.autoPlace(p.schematic, true);
    CHECK(p.pcb.autoRoute(p.schematic).failed == 0);
    CHECK_NEAR(p.pcb.settings.clearance, 0.15, 1e-12);
    for (const auto& v : p.pcb.runDRC(p.schematic)) CHECK(v.code != "DRC_HV_CLEARANCE");
}

// ======================================================================= microcontroller simulation

namespace {
const std::string& exampleHex(const char* id) {
    const FirmwareExample* e = findFirmwareExample(id);
    static const std::string none;
    return e ? e->hex : none;
}

/// 5 V supply, an ATmega328P (or ATtiny85) with VCC/GND (and AVCC) wired, firmware attached. Returns the MCU id.
int mcuBoard(Project& p, const char* part, const char* firmware, int& gnd) {
    auto& s = p.schematic;
    std::string id = p.addCustomPart(findStandardPart(part)->spec);
    int v = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    gnd = s.addComponent(ComponentKind::Ground, "", {0, 100});
    int u = s.addCustomComponent(id, part, {200, 0});
    wire(s, v, "-", gnd, "GND");
    wire(s, v, "+", u, "VCC");
    wire(s, u, "GND", gnd, "GND");
    if (s.pinIndex(u, "AVCC") >= 0) wire(s, u, "AVCC", v, "+");
    if (firmware) s.setFirmware(u, exampleHex(firmware), firmware, 0);
    p.schematicChanged();
    return u;
}

int countEdges(const std::vector<double>& v, double threshold) {
    int edges = 0;
    for (size_t i = 1; i < v.size(); ++i) edges += (v[i - 1] < threshold) != (v[i] < threshold);
    return edges;
}
}  // namespace

TEST(intel_hex_parser) {
    std::vector<uint8_t> bytes;
    for (int i = 0; i < 70; ++i) bytes.push_back(static_cast<uint8_t>(i * 7));
    std::string hex = toIntelHex(bytes);
    HexImage img = parseIntelHex(hex);
    CHECK(img.ok());
    CHECK(img.bytes == bytes);
    CHECK(!parseIntelHex(":10000000FFFF\n").ok());
    std::string bad = hex;
    bad[10] = bad[10] == '0' ? '1' : '0';  // corrupt a data digit: checksum fails
    CHECK(parseIntelHex(bad).error.find("checksum") != std::string::npos);
    CHECK(parseIntelHex("hello").error.find("Intel HEX") != std::string::npos);
    CHECK(!parseIntelHex(":00000001FF\n").ok());  // no data
    CHECK(mcuModelForPart("ATmega328P") == McuModel::ATmega328P);
    CHECK(mcuModelForPart("atmega328p-pu") == McuModel::ATmega328P);
    CHECK(mcuModelForPart("ATtiny85") == McuModel::ATtiny85);
    CHECK(!mcuModelForPart("NE555"));
    CHECK(firmwareExamples().size() >= 9);
    for (const auto& e : firmwareExamples()) CHECK(parseIntelHex(e.hex).ok());
}

TEST(avr_cpu_matches_reference_simulator) {
    // avr-gcc output of Core/tests/firmware/alu.c; the expected text is simavr's output for the same image.
    const std::string expected = "-23\n79\n-7700\n-14\n-2\n43\n88\n2\n2\n64\n25\n-25\n-12024\n-30585\n-38\n-147\n-1544\n26784\n8571\n3\n19456\n-987641976\n819560599\n-80004\n-4941\n-7716050\n4000065537\n376121344\n61034\n14742\n488281\n-3962745\n3600000000\n3554416254\n2802067423\n3596950572\n229283573\n3256818826\n610\n15129\n-456\n3141\n64159\n-1256636\n31006\n1\n-987654\n9\n0\nSiEDA-AVR\n-17\n-3\n0\n4\n4\n5\n9\n22\n1\nEND\n";
    HexImage img = parseIntelHex(exampleHex("cpu_selftest"));
    CHECK(img.ok());
    AvrMcu mcu(McuModel::ATmega328P);
    std::string err;
    CHECK(mcu.loadFirmware(img.bytes, err));
    mcu.setSupply(5.0);
    std::string out;
    for (int i = 0; i < 400 && out.find("END\n") == std::string::npos; ++i) {
        mcu.run(16000);
        out += mcu.takeSerialOutput();
    }
    CHECK(out == expected);
    CHECK(mcu.fault().empty());
}

TEST(avr_peripherals_standalone) {
    AvrMcu mcu(McuModel::ATmega328P);
    std::string err;
    CHECK(mcu.loadFirmware(parseIntelHex(exampleHex("arduino_analog_serial")).bytes, err));
    mcu.setSupply(5.0);
    mcu.setPinVoltage(mcu.pinIndex("PC0"), 2.5);
    std::string out;
    for (int i = 0; i < 160; ++i) {  // 160 ms
        mcu.run(16000);
        out += mcu.takeSerialOutput();
    }
    // millis() and Serial.println at 115200: "0 512", "50 512", "100 512", "150 512" (the Arduino core's drift included).
    CHECK(out.rfind("0 512\r\n50 512\r\n100 512\r\n150 512\r\n", 0) == 0);
    // Flash larger than the chip is refused.
    AvrMcu tiny(McuModel::ATtiny85);
    CHECK(!tiny.loadFirmware(std::vector<uint8_t>(9000, 0), err));
    CHECK(err.find("flash") != std::string::npos);
    CHECK(tiny.pinIndex("PB5/RST") == 5);
    CHECK(tiny.pinIndex("PD0") == -1);
}

TEST(mcu_blinks_an_led_in_the_circuit) {
    Project p;
    int gnd = -1;
    int u = mcuBoard(p, "ATmega328P", "arduino_blink", gnd);
    auto& s = p.schematic;
    int r = s.addComponent(ComponentKind::Resistor, "330", {350, 0});
    int d = s.addComponent(ComponentKind::LED, "Red", {450, 0});
    wire(s, u, "PB5", r, "1");
    wire(s, r, "2", d, "A");
    wire(s, d, "K", gnd, "GND");
    p.schematicChanged();
    Simulator sim(s);
    TransientResult tr = sim.transient(1.0, 1e-4);
    CHECK(tr.ok);
    CHECK(tr.mcus.size() == 1 && tr.mcus[0].running);
    CHECK(tr.mcus[0].status.find("16 MHz") != std::string::npos);
    int ledNet = s.netOf({r, 0});
    const auto& v = tr.netVoltages[static_cast<size_t>(ledNet)];
    // 200 ms on / 200 ms off: edges at 0, 0.2, 0.4, 0.6, 0.8 and 1.0 s (Arduino delay() is micros()-accurate).
    std::vector<double> edges;
    for (size_t i = 1; i < v.size(); ++i)
        if ((v[i - 1] < 2.5) != (v[i] < 2.5)) edges.push_back(tr.time[i]);
    CHECK(edges.size() == 6);
    for (size_t i = 0; i < edges.size(); ++i) CHECK_NEAR(edges[i], 0.2 * static_cast<double>(i), 0.0015);
    double peak = *std::max_element(v.begin(), v.end());
    CHECK(peak > 4.5 && peak < 5.0);  // 25 Ω output resistance drops a little under the LED current
    double iMax = *std::max_element(tr.currents[d].begin(), tr.currents[d].end());
    CHECK(iMax > 0.007 && iMax < 0.011);  // (5 V − ~2 V) / (330 + 25) Ω
    // Pin at 0.3 s is off, at 0.1 s on.
    CHECK(v[1000] > 4.5);
    CHECK(v[3000] < 0.5);
}

TEST(mcu_adc_pwm_and_serial_in_the_circuit) {
    Project p;
    int gnd = -1;
    int u = mcuBoard(p, "ATmega328P", "arduino_analog_serial", gnd);
    auto& s = p.schematic;
    int supply = s.addComponent(ComponentKind::VoltageSource, "5", {0, 200});  // separate source for the divider
    int r1 = s.addComponent(ComponentKind::Resistor, "10k", {100, 200});
    int r2 = s.addComponent(ComponentKind::Resistor, "30k", {100, 300});
    wire(s, supply, "-", gnd, "GND");
    wire(s, supply, "+", r1, "1");
    wire(s, r1, "2", r2, "1");
    wire(s, r2, "2", gnd, "GND");
    wire(s, r1, "2", u, "PC0");  // A0 = 3.75 V → 767
    // PWM on pin 9 (PB1) into an RC low-pass: the filtered voltage follows the duty (767 / 4 = 191 → 3.75 V).
    int rf = s.addComponent(ComponentKind::Resistor, "10k", {400, 0});
    int cf = s.addComponent(ComponentKind::Capacitor, "10u", {500, 50});
    wire(s, u, "PB1", rf, "1");
    wire(s, rf, "2", cf, "1");
    wire(s, cf, "2", gnd, "GND");
    p.schematicChanged();
    Simulator sim(s);
    TransientResult tr = sim.transient(0.6, 2e-5);
    CHECK(tr.ok);
    CHECK(tr.mcus.size() == 1);
    const std::string& serial = tr.mcus[0].serial;
    CHECK(serial.find("0 767\r\n") == 0);
    CHECK(serial.find(" 767\r\n", 10) != std::string::npos);
    const auto& vf = tr.netVoltages[static_cast<size_t>(s.netOf({rf, 1}))];
    double avg = 0;
    for (size_t i = vf.size() - 5000; i < vf.size(); ++i) avg += vf[i];
    avg /= 5000;
    CHECK_NEAR(avg, 5.0 * 191 / 255, 0.12);
    // JSON carries the serial monitor text and the status.
    Json j = p.transientToJson(tr);
    CHECK(j.get("mcus").items().size() == 1);
    CHECK(j.get("mcus").items()[0].get("serial").asString().find("767") != std::string::npos);
}

TEST(mcu_button_interrupt_with_live_switch) {
    Project p;
    int gnd = -1;
    int u = mcuBoard(p, "ATmega328P", "button_interrupt", gnd);
    auto& s = p.schematic;
    int sw = s.addComponent(ComponentKind::Switch, "open", {300, 100});
    int r = s.addComponent(ComponentKind::Resistor, "470", {350, 0});
    int d = s.addComponent(ComponentKind::LED, "Green", {450, 0});
    wire(s, u, "PD2", sw, "1");
    wire(s, sw, "2", gnd, "GND");
    wire(s, u, "PB5", r, "1");
    wire(s, r, "2", d, "A");
    wire(s, d, "K", gnd, "GND");
    p.schematicChanged();
    Simulator sim(s);
    std::string err;
    CHECK(sim.begin(err));
    auto ledVolts = [&]() { return sim.netVoltages()[static_cast<size_t>(s.netOf({r, 0}))]; };
    for (int i = 0; i < 200; ++i) CHECK(sim.advance(1e-4, err));  // 20 ms released
    CHECK(ledVolts() < 0.5);
    for (int press = 0; press < 3; ++press) {
        sim.setSwitch(sw, true);
        for (int i = 0; i < 100; ++i) sim.advance(1e-4, err);
        CHECK(ledVolts() > 4.0);  // LED on while pressed
        sim.setSwitch(sw, false);
        for (int i = 0; i < 100; ++i) sim.advance(1e-4, err);
        CHECK(ledVolts() < 0.5);
    }
    auto reports = sim.mcuReports();
    CHECK(reports.size() == 1);
    CHECK(reports[0].serial == "press 1\npress 2\npress 3\n");
    CHECK_NEAR(sim.time(), 0.08, 1e-9);
}

TEST(attiny85_and_mcu_status) {
    Project p;
    int gnd = -1;
    int u = mcuBoard(p, "ATtiny85", "tiny_blink", gnd);
    auto& s = p.schematic;
    int r = s.addComponent(ComponentKind::Resistor, "1k", {350, 0});
    wire(s, u, "PB0", r, "1");
    wire(s, r, "2", gnd, "GND");
    p.schematicChanged();
    TransientResult tr = Simulator(s).transient(0.5, 1e-4);
    CHECK(tr.ok);
    CHECK(tr.mcus.size() == 1 && tr.mcus[0].model == "ATtiny85");
    CHECK(tr.mcus[0].status.find("8 MHz") != std::string::npos);
    CHECK(countEdges(tr.netVoltages[static_cast<size_t>(s.netOf({r, 0}))], 2.5) == 5);  // toggles every 100 ms

    // No firmware: a clear status, pins stay high-impedance.
    s.setFirmware(u, "", "", 0);
    TransientResult none = Simulator(s).transient(0.01, 1e-4);
    CHECK(none.ok && !none.mcus[0].running);
    CHECK(none.mcus[0].status.find("No firmware") != std::string::npos);
    CHECK(std::fabs(none.netVoltages[static_cast<size_t>(s.netOf({r, 0}))].back()) < 1e-3);

    // A 1.5 V supply holds the chip in reset.
    s.setFirmware(u, exampleHex("tiny_blink"), "tiny_blink", 0);
    for (const auto& c : s.components())
        if (c.kind == ComponentKind::VoltageSource) s.setValue(c.id, "1.5");
    TransientResult low = Simulator(s).transient(0.01, 1e-4);
    CHECK(!low.mcus[0].running);
    CHECK(low.mcus[0].status.find("reset") != std::string::npos);
}

TEST(firmware_persists_with_the_project) {
    Project p;
    int gnd = -1;
    int u = mcuBoard(p, "ATmega328P", "blink", gnd);
    p.schematic.setFirmware(u, exampleHex("blink"), "blink.hex", 8e6);
    Project q = Project::fromJson(Json::parse(p.toJson().dump()));
    const Component* c = q.schematic.findByRef(p.schematic.find(u)->ref);
    CHECK(c != nullptr);
    if (!c) return;
    CHECK(c->firmware == exampleHex("blink"));
    CHECK(c->firmwareName == "blink.hex");
    CHECK_NEAR(c->clockHz, 8e6, 1e-6);
    Json snap = q.snapshot();
    bool found = false;
    for (const auto& j : snap.get("components").items())
        if (j.get("ref").asString() == c->ref) {
            found = true;
            CHECK(j.get("mcu").get("model").asString() == "ATmega328P");
            CHECK(j.get("mcu").get("firmwareName").asString() == "blink.hex");
            CHECK(j.get("mcu").get("firmwareBytes").asInt() > 100);
            CHECK_NEAR(j.get("mcu").get("clockHz").asNumber(), 8e6, 1e-6);
        }
    CHECK(found);
}

TEST(live_simulation_c_api) {
    SiedaProject* p = sieda_project_new("live");
    // ATmega328P from the standard library, through the C API like the app does.
    Json parts = Json::parse(std::string([] {
        char* j = sieda_standard_parts_json();
        std::string s = j;
        sieda_string_free(j);
        return s;
    }()));
    std::string specJson;
    for (const auto& item : parts.items())
        if (item.get("spec").get("name").asString() == "ATmega328P") specJson = item.get("spec").dump();
    CHECK(!specJson.empty());
    char* err = nullptr;
    char* partJson = sieda_custom_part_register(p, specJson.c_str(), &err);
    CHECK(partJson != nullptr);
    std::string partId = Json::parse(partJson).get("id").asString();
    sieda_string_free(partJson);
    int u = sieda_add_custom_component(p, partId.c_str(), "ATmega328P", 200, 0, 0, "U1");
    int v = sieda_add_component(p, static_cast<int32_t>(ComponentKind::VoltageSource), "5", 0, 0, 0, "V1");
    int g = sieda_add_component(p, static_cast<int32_t>(ComponentKind::Ground), "0", 0, 100, 0, "GND1");
    int sw = sieda_add_component(p, static_cast<int32_t>(ComponentKind::Switch), "push", 300, 100, 0, "SW1");
    int r = sieda_add_component(p, static_cast<int32_t>(ComponentKind::Resistor), "330", 350, 0, 0, "R1");
    int d = sieda_add_component(p, static_cast<int32_t>(ComponentKind::LED), "Red", 450, 0, 0, "D1");
    auto pin = [&](int c, const char* name) { return sieda_find_pin(p, c, name); };
    CHECK(sieda_connect(p, v, pin(v, "+"), u, pin(u, "VCC")) >= 0);
    CHECK(sieda_connect(p, v, pin(v, "-"), g, 0) >= 0);
    CHECK(sieda_connect(p, u, pin(u, "GND"), g, 0) >= 0);
    CHECK(sieda_connect(p, u, pin(u, "PD2"), sw, 0) >= 0);
    CHECK(sieda_connect(p, sw, 1, g, 0) >= 0);
    CHECK(sieda_connect(p, u, pin(u, "PB5"), r, 0) >= 0);
    CHECK(sieda_connect(p, r, 1, d, 0) >= 0);
    CHECK(sieda_connect(p, d, 1, g, 0) >= 0);
    char* hex = sieda_firmware_example_hex("button_interrupt");
    CHECK(hex != nullptr);
    CHECK(sieda_set_firmware(p, u, hex, "Button", 0, &err) == 1);
    CHECK(sieda_set_firmware(p, r, hex, "Button", 0, &err) == 0);  // a resistor is not a microcontroller
    CHECK(err != nullptr && std::string(err).find("not a microcontroller") != std::string::npos);
    sieda_string_free(err);
    err = nullptr;
    CHECK(sieda_set_firmware(p, u, ":nonsense", "x", 0, &err) == 0);
    sieda_string_free(err);
    err = nullptr;
    sieda_string_free(hex);

    SiedaLiveSim* live = sieda_live_start(p, &err);
    CHECK(live != nullptr);
    auto state = [&]() {
        char* j = sieda_live_state(live);
        Json out = Json::parse(j);
        sieda_string_free(j);
        return out;
    };
    auto led = [&](const Json& st) {
        for (const auto& l : st.get("leds").items())
            if (l.get("ref").asString() == "D1") return l.get("brightness").asNumber();
        return -1.0;
    };
    CHECK(sieda_live_run(live, 0.02, 1e-4, 50, &err) == 1);
    Json s0 = state();
    CHECK_NEAR(s0.get("time").asNumber(), 0.02, 1e-9);
    CHECK(led(s0) < 0.05);
    CHECK(s0.get("switches").items().size() == 1);
    CHECK(s0.get("switches").items()[0].get("momentary").asBool());
    CHECK(!s0.get("switches").items()[0].get("closed").asBool());
    CHECK(s0.get("trace").get("time").items().size() == 50);
    CHECK(!s0.get("trace").get("nets").items().empty());
    sieda_live_set_switch(live, sw, 1);  // press and hold
    CHECK(sieda_live_run(live, 0.01, 1e-4, 50, &err) == 1);
    Json s1 = state();
    CHECK(led(s1) > 0.4);  // ~8 mA of 15 mA full glow
    CHECK(s1.get("switches").items()[0].get("closed").asBool());
    CHECK(s1.get("mcus").items()[0].get("serial").asString() == "press 1\n");
    CHECK(s1.get("mcus").items()[0].get("running").asBool());
    sieda_live_set_switch(live, sw, 0);
    CHECK(sieda_live_run(live, 0.01, 1e-4, 50, &err) == 1);
    CHECK(led(state()) < 0.05);
    sieda_live_serial_input(live, u, "ignored");  // the button firmware does not read; must not disturb anything
    CHECK(sieda_live_run(live, 0.005, 1e-4, 10, &err) == 1);
    sieda_live_free(live);

    // A circuit without ground cannot start.
    SiedaProject* bad = sieda_project_new("bad");
    sieda_add_component(bad, static_cast<int32_t>(ComponentKind::Resistor), "1k", 0, 0, 0, "R1");
    CHECK(sieda_live_start(bad, &err) == nullptr);
    CHECK(err != nullptr);
    sieda_string_free(err);
    sieda_project_free(bad);
    sieda_project_free(p);
}

TEST(ipc2221_voltage_clearance) {
    CHECK_NEAR(ipc2221Clearance(12, false), 0.1, 1e-12);
    CHECK_NEAR(ipc2221Clearance(48, false), 0.6, 1e-12);
    CHECK_NEAR(ipc2221Clearance(230, false), 1.25, 1e-12);
    CHECK_NEAR(ipc2221Clearance(400, false), 2.5, 1e-12);
    CHECK_NEAR(ipc2221Clearance(1000, false), 5.0, 1e-12);
    CHECK_NEAR(ipc2221Clearance(230, true), 6.4, 1e-12);
    CHECK(ipc2221Clearance(-230, false) == ipc2221Clearance(230, false));

    // 230 V across an 0805 resistor: its pads are ~1 mm apart, below the 1.25 mm B2 spacing.
    auto build = [](const char* volts) {
        Project p;
        auto& s = p.schematic;
        int v = s.addComponent(ComponentKind::VoltageSource, volts, {0, 0});
        int r = s.addComponent(ComponentKind::Resistor, "1M", {100, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        wire(s, v, "+", r, "1");
        wire(s, r, "2", g, "GND");
        wire(s, v, "-", g, "GND");
        p.schematicChanged();
        p.pcb.autoPlace(p.schematic, true);
        p.pcb.autoRoute(p.schematic);
        int hv = 0;
        for (const auto& f : p.pcb.runDRC(p.schematic)) hv += f.code == "DRC_HV_CLEARANCE";
        return hv;
    };
    CHECK(build("5") == 0);
    CHECK(build("230") > 0);
    CHECK(build("SIN(0 325 50)") > 0);  // mains peak, not the 0 V DC value
}

TEST(no_connect_flags) {
    Project p;
    auto& s = p.schematic;
    std::string id = CustomPartRegistry::instance().registerPart(findStandardPart("NE555")->spec)->id;
    int v = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    int u = s.addCustomComponent(id, "", {150, 0});
    wire(s, v, "+", u, "8");
    wire(s, u, "1", g, "GND");
    wire(s, v, "-", g, "GND");
    auto count = [&](const char* code) {
        int n = 0;
        for (const auto& f : s.runERC()) n += f.code == code;
        return n;
    };
    CHECK(count("ERC_UNCONNECTED_PIN") == 6);  // pins 2–7 open
    for (int pin = 1; pin <= 6; ++pin) CHECK(s.setPinNoConnect(u, pin, true));
    CHECK(!s.setPinNoConnect(u, 99, true));
    CHECK(count("ERC_UNCONNECTED_PIN") == 0);
    // A flagged pin that gets wired is reported.
    wire(s, u, "4", v, "+");
    CHECK(count("ERC_NC_CONNECTED") == 1);
    // Persisted with the project and visible in the snapshot.
    Project q = Project::fromJson(Json::parse(p.toJson().dump()));
    CHECK(q.schematic.find(u)->noConnect.size() == 6);
    CHECK(p.snapshot().dump().find("\"noConnect\":true") != std::string::npos);
    CHECK(s.setPinNoConnect(u, 3, false));
    CHECK(s.find(u)->noConnect.size() == 5);
}

TEST(net_classes_and_fine_pitch) {
    // Net class: a 1 A LED-strip feed gets a wide track; DRC stays clean around it.
    Project p;
    auto& s = p.schematic;
    int v = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    int r = s.addComponent(ComponentKind::Resistor, "4.7 10W", {120, 0});
    int r2 = s.addComponent(ComponentKind::Resistor, "10k", {120, 80});
    int d = s.addComponent(ComponentKind::LED, "Red", {200, 80});
    wire(s, v, "+", r, "1");
    wire(s, r, "2", g, "GND");
    wire(s, v, "+", r2, "1");
    wire(s, r2, "2", d, "A");
    wire(s, d, "K", g, "GND");
    wire(s, v, "-", g, "GND");
    p.schematicChanged();
    auto widths = p.pcb.autoNetWidths(s);
    CHECK(!widths.empty());
    for (auto& [net, w] : widths) std::printf("    net class %s = %.2f mm\n", net.c_str(), w);
    p.pcb.settings.width = 30;
    p.pcb.settings.height = 22;
    p.pcb.autoPlace(s, true);
    CHECK(p.pcb.autoRoute(s).failed == 0);
    double widest = 0;
    for (const auto& t : p.pcb.tracks) widest = std::max(widest, t.width);
    CHECK(widest >= 0.45 - 1e-9);
    int bad = 0;
    for (const auto& f : p.pcb.runDRC(s))
        if (f.severity != Severity::Info && f.code != "DRC_TRACK_CURRENT") {
            ++bad;
            std::printf("    %s %s\n", f.code.c_str(), f.message.c_str());
        }
    CHECK(bad == 0);
    Project q = Project::fromJson(Json::parse(p.toJson().dump()));
    CHECK(q.pcb.settings.netWidths == p.pcb.settings.netWidths);

    // Fine pitch: an MPU-6050 (0.5 mm QFN) wired up on Class 3 rules has no design-rule clearance findings from
    // its own pad gaps, and tracks entering its pads are necked down to the pad width.
    Project f;
    auto& fs = f.schematic;
    std::string imu = CustomPartRegistry::instance().registerPart(findStandardPart("MPU-6050")->spec)->id;
    int u = fs.addCustomComponent(imu, "", {200, 0});
    int vv = fs.addComponent(ComponentKind::VoltageSource, "3.3", {0, 0});
    int gg = fs.addComponent(ComponentKind::Ground, "", {0, 80});
    int c1 = fs.addComponent(ComponentKind::Capacitor, "100n", {100, 80});
    int c2 = fs.addComponent(ComponentKind::Capacitor, "2.2n", {300, 80});
    int c3 = fs.addComponent(ComponentKind::Capacitor, "100n", {300, 160});
    wire(fs, vv, "+", u, "13");
    wire(fs, u, "8", u, "13");
    wire(fs, u, "18", gg, "GND");
    wire(fs, u, "9", gg, "GND");
    wire(fs, u, "1", gg, "GND");
    wire(fs, u, "11", gg, "GND");
    wire(fs, vv, "-", gg, "GND");
    wire(fs, c1, "1", u, "13");
    wire(fs, c1, "2", gg, "GND");
    wire(fs, c2, "1", u, "20");
    wire(fs, c2, "2", gg, "GND");
    wire(fs, c3, "1", u, "10");
    wire(fs, c3, "2", gg, "GND");
    f.schematicChanged();
    f.pcb.settings.applyPreset("IPC-2221 Class 3");
    f.pcb.settings.width = 24;
    f.pcb.settings.height = 20;
    f.pcb.autoPlace(fs, true);
    RouteStats st = f.pcb.autoRoute(fs);
    std::printf("    QFN board: %d/%d routed\n", st.routed, st.connections);
    CHECK(st.failed == 0);
    int clearance = 0;
    for (const auto& x : f.pcb.runDRC(fs))
        if (x.code == "DRC_CLEARANCE_RULE" || x.code == "DRC_CLEARANCE" || x.code == "DRC_SHORT") {
            ++clearance;
            std::printf("    %s %s\n", x.code.c_str(), x.message.c_str());
        }
    CHECK(clearance == 0);
    bool necked = false;
    for (const auto& t : f.pcb.tracks) necked |= t.width < f.pcb.settings.trackWidth - 1e-9;
    CHECK(necked);
}

TEST(board_outline_holes_and_pours) {
    // Outline presets.
    BoardSettings b;
    b.setOutline(boardOutlinePreset("quad-x", 100, 40, 12));
    CHECK_NEAR(b.width, 100, 1e-6);
    CHECK_NEAR(b.height, 100, 1e-6);
    CHECK(b.contains({50, 50}));
    CHECK(b.contains({88, 88}));    // on a diagonal arm
    CHECK(!b.contains({3, 50}));    // between two arms
    CHECK(!b.contains({50, 97}));
    CHECK(b.edgeDistance({50, 50}) > 15);
    CHECK(b.edgeDistance({3, 50}) < 0);
    CHECK(b.rectInside(Rect::centered({50, 50}, 10, 10), 1));
    CHECK(!b.rectInside(Rect::centered({50, 72}, 10, 10), 0));
    BoardSettings r;
    r.setOutline(boardOutlinePreset("rounded", 30, 20, 4));
    CHECK(r.contains({15, 10}) && !r.contains({0.3, 0.3}) && r.contains({0.3, 10}));
    CHECK(boardOutlinePreset("hexagon", 10, 10, 0).empty());

    // Amplifier on a quad-X frame with a 30.5 mm M3 mounting pattern and ground pours on both sides.
    Project p = amplifierProject();
    auto& s = p.schematic;
    p.pcb.settings.setOutline(boardOutlinePreset("quad-x", 80, 40, 12));
    for (double dx : {-15.25, 15.25})
        for (double dy : {-15.25, 15.25}) p.pcb.settings.holes.push_back({{40 + dx, 40 + dy}, 3.2, 6.4});
    p.pcb.zones.push_back({"GND", 1, false, 0});
    p.pcb.zones.push_back({"GND", 0, false, 0});
    p.pcb.autoPlace(s, true);
    for (const auto& c : s.components())
        if (c.hasFootprint()) CHECK(p.pcb.settings.rectInside(p.pcb.courtyard(c), 0));
    RouteStats st = p.pcb.autoRoute(s);
    std::printf("    quad-X board: %d/%d routed, %d vias\n", st.routed, st.connections, st.vias);
    CHECK(st.failed == 0);
    CHECK(p.pcb.ratsnest(s).empty());
    const auto& fills = p.pcb.zoneFills(s);
    CHECK(fills.size() == 2);
    for (const auto& f : fills) {
        CHECK(f.net >= 0 && f.islands >= 1 && f.area() > 200);
        for (const auto& rc : f.rects) {  // poured copper stays inside the outline and out of the hole keep-outs
            CHECK(p.pcb.settings.edgeDistance(rc.center()) >= p.pcb.settings.edgeClearance);
            CHECK(p.pcb.settings.holeDistance(rc.center()) > 0);
        }
    }
    std::string first;
    CHECK(drcErrors(p, &first) == 0);
    if (!first.empty()) std::printf("    first DRC error: %s\n", first.c_str());
    // Ground tracks are only short stubs to the pour: no ground track current warning.
    for (const auto& f : p.pcb.runDRC(s)) CHECK(f.code != "DRC_TRACK_CURRENT");

    std::string edge = exportGerber(s, p.pcb, GerberLayer::EdgeCuts);
    size_t lines = 0;
    for (size_t at = edge.find("D01*"); at != std::string::npos; at = edge.find("D01*", at + 1)) ++lines;
    CHECK(lines == p.pcb.settings.outline.size());
    CHECK(exportCopperGerber(s, p.pcb, 1).find("G36*") != std::string::npos);
    std::string npth = exportExcellonDrill(s, p.pcb, false);
    CHECK(npth.find("T1C3.200") != std::string::npos && npth.find("NPTH") != std::string::npos);
    Mesh m = buildAssemblyMesh(s, p.pcb);
    CHECK(m.triangleCount() > 100);

    Project q = Project::fromJson(Json::parse(p.toJson().dump()));
    CHECK(q.pcb.settings.outline.size() == p.pcb.settings.outline.size());
    CHECK(q.pcb.settings.holes.size() == 4);
    CHECK(q.pcb.zones.size() == 2 && q.pcb.zones[0].net == "GND" && q.pcb.zones[0].layer == 1);
    CHECK(q.pcb.ratsnest(q.schematic).empty());
    Json snap = q.snapshot();
    CHECK(snap.get("zoneFills").items().size() == 2);
    CHECK(snap.get("board").get("holes").items().size() == 4);

    // Off-board and hole keep-out findings.
    Project bad = ledProject();
    bad.pcb.settings.width = 30;
    bad.pcb.settings.height = 20;
    bad.pcb.autoPlace(bad.schematic, true);
    bad.pcb.settings.holes.push_back({bad.schematic.components()[1].pcb.position, 3.2, 6.4});
    bool keepout = false;
    for (const auto& e : bad.pcb.runDRC(bad.schematic)) keepout |= e.code == "DRC_HOLE_KEEPOUT";
    CHECK(keepout);

    // Four-layer board with a reserved ground plane: no other net routes on it, the ground pads reach it by vias.
    Project f = amplifierProject();
    f.pcb.settings.layerCount = 4;
    f.pcb.settings.width = 40;
    f.pcb.settings.height = 30;
    f.pcb.zones.push_back({"GND", 1, true, 0});
    f.pcb.autoPlace(f.schematic, true);
    RouteStats fs = f.pcb.autoRoute(f.schematic);
    CHECK(fs.failed == 0);
    int gnd = -1;
    for (const auto& n : f.schematic.nets())
        if (n.name == "GND") gnd = n.index;
    for (const auto& t : f.pcb.tracks) CHECK(t.layer != 1 || t.net == gnd);
    CHECK(f.pcb.zoneFills(f.schematic)[0].area() > 0.6 * 40 * 30);
    CHECK(drcErrors(f, &first) == 0);
    if (!first.empty()) std::printf("    first DRC error: %s\n", first.c_str());
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

TEST(assembly_mesh_is_realistic) {
    // Every vertex carries what it is made of, so the 3D view can light mask, metal, solder and plastic differently.
    Project p = amplifierProject();
    p.pcb.autoPlace(p.schematic, true);
    CHECK(p.pcb.autoRoute(p.schematic).failed == 0);
    Mesh m = buildAssemblyMesh(p.schematic, p.pcb);
    CHECK(m.surfaces.size() == m.vertexCount());
    for (uint8_t v : m.surfaces) CHECK(v < kSurfaceCount);
    for (Surface s : {Surface::Mask, Surface::Laminate, Surface::Finish, Surface::Gold, Surface::Tin, Surface::Solder,
                      Surface::Silk, Surface::Plastic, Surface::Ceramic, Surface::Glass, Surface::Hole, Surface::Marking})
        CHECK(m.surfaceVertices(s) > 0);
    // The board's top face is still the first quad, in the mask colour.
    CHECK(m.surfaces[0] == static_cast<uint8_t>(Surface::Mask) && m.normals[1] > 0.99f);

    // A bare board (no parts): pads show their finish, no solder joints, leads or bodies.
    Mesh bare = buildAssemblyMesh(p.schematic, p.pcb, {false, true, true});
    CHECK(bare.surfaceVertices(Surface::Solder) == 0 && bare.surfaceVertices(Surface::Plastic) == 0);
    CHECK(bare.surfaceVertices(Surface::Finish) > 0 && bare.surfaceVertices(Surface::Silk) > 0);
    // Silkscreen legends: reference designators make the silk far richer than the bare outlines.
    size_t parts = 0;
    for (const auto& c : p.schematic.components()) parts += c.hasFootprint() && c.pcb.placed ? 1 : 0;
    CHECK(bare.surfaceVertices(Surface::Silk) > parts * 4 * 24 * 2);

    // The X-ray copper layers are all finish.
    Mesh layer = buildCopperLayerMesh(p.schematic, p.pcb, 0);
    CHECK(layer.surfaces.size() == layer.vertexCount() && layer.surfaceVertices(Surface::Finish) == layer.vertexCount());
}

TEST(silkscreen_stroke_font) {
    // Every character a reference or part value uses draws something; unknown ones fall back to '?'.
    for (char ch : std::string("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcxyz-+./_?")) {
        Mesh t;
        t.addText(std::string(1, ch), {0, 0}, 1.0, 0.0, 0.01, Rgba{});
        CHECK(t.vertexCount() > 0);
    }
    Mesh space;
    space.addText(" ", {0, 0}, 1.0, 0.0, 0.01, Rgba{});
    CHECK(space.vertexCount() == 0);
    Mesh unknown;
    unknown.addText("~", {0, 0}, 1.0, 0.0, 0.01, Rgba{});
    CHECK(unknown.vertexCount() > 0);

    // Width grows with length and height; text is centred, and mirrored text (bottom legend) is its reflection.
    CHECK(Mesh::textWidth("R10", 1.0) > Mesh::textWidth("R1", 1.0));
    CHECK(std::fabs(Mesh::textWidth("U1", 2.0) - 2 * Mesh::textWidth("U1", 1.0)) < 1e-9);
    Mesh a, b;
    a.addText("R7", {10, 5}, 1.0, 0.0, 0.01, Rgba{});
    b.addText("R7", {10, 5}, 1.0, 0.0, 0.01, Rgba{}, true);
    double minA = 1e9, maxA = -1e9, sumA = 0, sumB = 0;
    for (size_t v = 0; v < a.vertexCount(); ++v) {
        minA = std::min(minA, double(a.positions[v * 3]));
        maxA = std::max(maxA, double(a.positions[v * 3]));
        sumA += a.positions[v * 3] - 10;
        sumB += b.positions[v * 3] - 10;
    }
    CHECK(std::fabs((minA + maxA) / 2 - 10) < 0.2);
    CHECK(a.vertexCount() == b.vertexCount());
    CHECK(std::fabs(sumA + sumB) < 1e-3);  // x mirrored about the centre
    // Text tops point to board −Y (the top of the PCB view): the '7' bar sits at the smallest Z.
    double minZ = 1e9, maxZ = -1e9;
    for (size_t v = 0; v < a.vertexCount(); ++v) {
        minZ = std::min(minZ, double(a.positions[v * 3 + 2]));
        maxZ = std::max(maxZ, double(a.positions[v * 3 + 2]));
    }
    CHECK(minZ < 5 - 0.4 && maxZ > 5 + 0.4);
}

namespace {
/// Positions (x, y, z) of the vertices tagged with `s`.
std::vector<Vec3> surfacePoints(const Mesh& m, Surface s) {
    std::vector<Vec3> pts;
    for (size_t v = 0; v < m.vertexCount(); ++v)
        if (m.surfaces[v] == static_cast<uint8_t>(s)) pts.push_back({m.positions[v * 3], m.positions[v * 3 + 1], m.positions[v * 3 + 2]});
    return pts;
}
bool anyIn(const std::vector<Vec3>& pts, Rect r, double y0, double y1) {
    for (const auto& p : pts)
        if (p.x >= r.x0 && p.x <= r.x1 && p.z >= r.y0 && p.z <= r.y1 && p.y >= y0 && p.y <= y1) return true;
    return false;
}
}  // namespace

TEST(realistic_assembly_geometry) {
    Project p = amplifierProject();
    p.pcb.autoPlace(p.schematic, true);
    CHECK(p.pcb.autoRoute(p.schematic).failed == 0);
    const auto& s = p.schematic;
    const double t = p.pcb.settings.thickness;
    Mesh m = buildAssemblyMesh(s, p.pcb);
    const auto solder = surfacePoints(m, Surface::Solder), tin = surfacePoints(m, Surface::Tin);
    const auto silk = surfacePoints(m, Surface::Silk), marking = surfacePoints(m, Surface::Marking);
    const auto glass = surfacePoints(m, Surface::Glass), ceramic = surfacePoints(m, Surface::Ceramic);
    const auto pads = p.pcb.pads(s);

    for (const auto& pad : pads) {
        const Component* c = s.find(pad.componentId);
        if (!c || c->pcb.bottom) continue;
        if (!pad.throughHole) {
            // A solder dome sits on every top SMD pad, inside the pad and just above the copper.
            CHECK(anyIn(solder, pad.bounds(), 0.0, 0.2));
        } else {
            // Through-hole joints are on the solder side, under the board.
            CHECK(anyIn(solder, pad.bounds().inflated(0.05), -t - 0.4, -t));
        }
    }
    // Every SOIC lead reaches its pad: tinned metal over each pad that sticks out of the package body.
    int socs = 0, leds = 0, chips = 0, smdPads = 0, thPads = 0;
    for (const auto& pad : pads) (pad.throughHole ? thPads : smdPads) += 1;
    CHECK(smdPads > 10 && thPads >= 2);
    for (const auto& c : s.components()) {
        if (c.def().footprint != "SOIC8_OPAMP") continue;
        ++socs;
        int reached = 0, total = 0;
        for (const auto& pad : pads) {
            if (pad.componentId != c.id) continue;
            ++total;
            reached += anyIn(tin, pad.bounds(), 0.0, 0.3) ? 1 : 0;
        }
        CHECK(total == 8 && reached == 8);
        // The part number is etched on the package top and a pin-1 dimple sits on it.
        const FootprintDef* fp = Library::instance().footprint(c.def().footprint);
        Rect body = Rect::centered(c.pcb.position, fp->body.width + 0.01, fp->body.depth + 0.01);
        if (((c.pcb.rotation / 90) % 2 + 2) % 2 == 1) body = Rect::centered(c.pcb.position, fp->body.depth + 0.01, fp->body.width + 0.01);
        CHECK(anyIn(marking, body, fp->body.height, fp->body.height + 0.1));
    }
    // Each placed part's reference is printed on the silkscreen above its outline.
    for (const auto& c : s.components()) {
        if (!c.hasFootprint() || !c.pcb.placed || c.ref.empty()) continue;
        Rect r = p.pcb.courtyard(c);
        Rect label{r.x0 - 3, r.y0 - 2.5, r.x1 + 3, r.y0};
        CHECK(anyIn(silk, label, 0.0, 0.1));
    }
    // Glass only on LEDs, ceramic only on chip passives.
    for (const auto& c : s.components()) {
        if (!c.hasFootprint() || !c.pcb.placed) continue;
        Rect r = p.pcb.courtyard(c);
        const std::string fp = c.def().footprint;
        const bool chip = fp.rfind("R_", 0) == 0 || fp.rfind("C_", 0) == 0;
        if (c.kind == ComponentKind::LED) {
            ++leds;
            CHECK(anyIn(glass, r, 0.0, 10.0));
        }
        if (chip) {
            ++chips;
            CHECK(anyIn(ceramic, r, 0.0, 10.0) && !anyIn(glass, r, 0.0, 10.0));
        }
    }
    CHECK(socs == 1 && leds == 1 && chips >= 6);
    for (const auto& g : glass) CHECK(g.y >= 0);  // no LED glass inside or under the board here

    // Vias: a dark bore of the drill's radius through the board and plated lands on both outer layers.
    p.pcb.vias.push_back({});
    Via& v = p.pcb.vias.back();
    v.position = {1.5, 1.5};
    v.drill = 0.3;
    v.diameter = 0.6;
    Mesh withVia = buildAssemblyMesh(s, p.pcb);
    double boreR = 0, boreTop = -1e9, boreBottom = 1e9;
    for (const auto& h : surfacePoints(withVia, Surface::Hole)) {
        double r = std::hypot(h.x - 1.5, h.z - 1.5);
        if (r > 0.4) continue;
        boreR = std::max(boreR, r);
        boreTop = std::max(boreTop, h.y);
        boreBottom = std::min(boreBottom, h.y);
    }
    CHECK(std::fabs(boreR - 0.15) < 1e-3);
    CHECK(boreTop > 0 && boreBottom < -t);
    const auto finish = surfacePoints(withVia, Surface::Finish);
    CHECK(anyIn(finish, Rect::centered({1.5, 1.5}, 0.62, 0.62), -0.001, 0.04));
    CHECK(anyIn(finish, Rect::centered({1.5, 1.5}, 0.62, 0.62), -t - 0.04, -t + 0.001));
}

TEST(realistic_assembly_bottom_side) {
    // A part flipped to the bottom: its body, solder and mirrored legend hang under the board.
    Project p = amplifierProject();
    p.pcb.autoPlace(p.schematic, true);
    int flipped = -1;
    for (auto& c : p.schematic.mutableComponents())
        if (flipped < 0 && c.kind == ComponentKind::Resistor && c.pcb.placed) {
            c.pcb.bottom = true;
            flipped = c.id;
        }
    CHECK(flipped >= 0);
    const auto& s = p.schematic;
    const double t = p.pcb.settings.thickness;
    Mesh m = buildAssemblyMesh(s, p.pcb);
    Rect r = p.pcb.courtyard(*s.find(flipped));
    CHECK(anyIn(surfacePoints(m, Surface::Ceramic), r, -t - 5, -t));
    CHECK(!anyIn(surfacePoints(m, Surface::Ceramic), r, 0.0, 5.0));
    CHECK(anyIn(surfacePoints(m, Surface::Solder), r.inflated(0.5), -t - 0.3, -t));
    Rect label{r.x0 - 3, r.y0 - 2.5, r.x1 + 3, r.y0};
    CHECK(anyIn(surfacePoints(m, Surface::Silk), label, -t - 0.1, -t));
}

TEST(realistic_assembly_mesh_stays_bounded) {
    // Big boards: the extra detail (legends, leads, joints) costs a bounded number of vertices per part and the mesh
    // builds quickly.
    Project p;
    auto& s = p.schematic;
    int prev = -1;
    for (int k = 0; k < 120; ++k) {
        int r = s.addComponent(ComponentKind::Resistor, "10k", {k * 30.0, 0});
        if (prev >= 0) wire(s, prev, "2", r, "1");
        prev = r;
    }
    p.pcb.settings.width = 160;
    p.pcb.settings.height = 120;
    p.pcb.autoPlace(s, true);
    size_t placed = 0;
    for (const auto& c : s.components()) placed += c.pcb.placed ? 1 : 0;
    CHECK(placed == 120);
    auto start = std::chrono::steady_clock::now();
    Mesh m = buildAssemblyMesh(s, p.pcb);
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(seconds < 5.0);
    CHECK(m.vertexCount() < placed * 3000);
    CHECK(m.surfaces.size() == m.vertexCount() && m.colors.size() == m.vertexCount() * 4);
    // Exports still carry the realistic colours.
    CHECK(exportObj(m, "big").find("\nv ") != std::string::npos);
}

TEST(complex_design_flow_is_fast) {
    // Productivity: a dense 40-channel signal-conditioning board (40 op-amp stages, each with gain resistors, a
    // filter capacitor and an output connector pin, all on shared VCC / GND) goes from schematic to a placed,
    // routed, verified and meshed board in seconds.
    Project p;
    auto& s = p.schematic;
    int j = s.addComponent(ComponentKind::Connector, "PWR", {0, 0});
    int vcc = s.addComponent(ComponentKind::NetLabel, "VCC", {-40, -10});
    int gnd = s.addComponent(ComponentKind::Ground, "", {-40, 30});
    wire(s, j, "1", vcc, "N");
    wire(s, j, "2", gnd, "GND");
    constexpr int kStages = 40;
    for (int k = 0; k < kStages; ++k) {
        const double x = 100.0 + k * 120.0;
        int u = s.addComponent(ComponentKind::OpAmp, "LM358", {x, 0});
        int rin = s.addComponent(ComponentKind::Resistor, "10k", {x - 60, 10});
        int rf = s.addComponent(ComponentKind::Resistor, "47k", {x, -60});
        int rg = s.addComponent(ComponentKind::Resistor, "10k", {x - 40, -60});
        int cf = s.addComponent(ComponentKind::Capacitor, "1n", {x + 40, -60});
        int lv = s.addComponent(ComponentKind::NetLabel, "VCC", {x - 90, 10});
        int g = s.addComponent(ComponentKind::Ground, "", {x - 40, 60});
        int out = s.addComponent(ComponentKind::NetLabel, "OUT" + std::to_string(k), {x + 60, 0});
        wire(s, lv, "N", rin, "1");
        wire(s, rin, "2", u, "IN+");
        wire(s, u, "IN-", rg, "2");
        wire(s, rg, "1", g, "GND");
        wire(s, u, "IN-", rf, "1");
        wire(s, rf, "2", u, "OUT");
        wire(s, cf, "1", u, "IN-");
        wire(s, cf, "2", u, "OUT");
        wire(s, u, "OUT", out, "N");
    }
    size_t parts = 0;
    for (const auto& c : s.components()) parts += c.hasFootprint() ? 1 : 0;
    CHECK(parts == 1 + kStages * 5);  // the connector, then an op-amp, three resistors and a capacitor per stage

    using clock = std::chrono::steady_clock;
    auto secs = [](clock::time_point a) { return std::chrono::duration<double>(clock::now() - a).count(); };
    auto t0 = clock::now();
    p.pcb.autoPlace(s, true);
    p.pcb.fitBoardToComponents(s, 2.5);
    const double place = secs(t0);
    t0 = clock::now();
    const auto stats = p.pcb.autoRoute(s);
    const double route = secs(t0);
    t0 = clock::now();
    const auto report = verifyDesign(p);
    const double verify = secs(t0);
    t0 = clock::now();
    Mesh m = buildAssemblyMesh(s, p.pcb);
    const double mesh = secs(t0);
    std::printf("    flow: %zu parts, %d connections — place %.2f s, route %.2f s, verify %.2f s, 3D %.2f s\n", parts,
                stats.connections, place, route, verify, mesh);
    CHECK(stats.failed == 0);
    for (const auto& c : s.components()) CHECK(!c.hasFootprint() || c.pcb.placed);
    CHECK(m.vertexCount() > 0);
    CHECK(report.stages.size() == 8);
    // Generous budgets (sanitiser and debug builds run here too): a regression to the old quadratic scans or an
    // unoptimised core shows up as minutes.
    CHECK(place < 20.0);
    CHECK(route < 60.0);
    CHECK(verify < 20.0);
    CHECK(mesh < 5.0);
}

TEST(production_parts_catalog) {
    // The robotics / automotive / industrial catalog: every orderable part number is in the library on its real
    // package, with datasheet pin names, and registers as a placeable part.
    const char* catalog[] = {
        "ATMEGA328P-AU", "ATMEGA328P-PU", "STM32F103C8T6", "STM32F405RGT6", "ESP32-WROOM-32E", "RP2040", "L293DD", "L293D",
        "L298HN", "ULN2003ADR", "ULN2003A", "PCA9685PW", "IR2101S", "IR2110S", "TMC2209-LA", "MPU-6050", "MPU-9250",
        "ADS1115IDGS", "LM358DR", "LM358N", "LM393DR", "LM393N", "MAX9814ETD+T", "MAX4466EXK+T", "74HC595D", "74HC595",
        "PCF8574TS", "PCF8574N", "CH340G", "CP2102N-A02-GQFN28", "MAX485ESA+T", "SP485EEN-L", "TJA1050", "MCP2551-I/SN",
        "LM7805", "LM7812", "AMS1117-3.3", "LM2596S-5.0", "XL4015E1", "TP4056", "LM74700QDBVRQ1", "PC817X3NSZ0F",
        "TLP281-4", "STM32H743IIT6", "STM32F765VIT6", "LPC1768", "XC7A35T-1CSG324I", "CSD18540Q5B",
        "TMC2160-TA", "TMC5160A-TA", "AS5047D-ATSM", "INA240A1EDRQ1", "TCAN1042VDRQ1", "ADM2587EBRWZ",
        "LTC4359IMS8#PBF", "BMI160", "ICS-43434", "ATSAMD51J20A-AU", "MT48LC16M16A2TG-6A", "W9812G6KH-6", "IS42S16400J-7TL",
        "MT41K256M16HA-125", "AS4C256M16D3-12BCN", "MT40A512M16LY-062E",
    };
    for (const char* name : catalog) {
        const StandardPart* sp = findStandardPart(name);
        CHECK(sp != nullptr);
        if (!sp) {
            std::printf("    missing %s\n", name);
            continue;
        }
        bool ok = true;
        try {
            auto part = CustomPartRegistry::instance().registerPart(sp->spec);
            size_t connected = 0;
            for (const auto& pad : part->footprint.pads) connected += pad.pinIndex >= 0 ? 1 : 0;
            ok = !part->def.pins.empty() && part->footprint.pads.size() >= sp->spec.pins.size() - 1 && connected > 0;
        } catch (const std::exception& e) {
            std::printf("    %s: %s\n", name, e.what());
            ok = false;
        }
        CHECK(ok);
    }
    // The catalog's flyback diodes are diode values with their own simulation models (Schottky / rectifier).
    CHECK(findDeviceModel(ComponentKind::Diode, "1N5819") != nullptr);
    CHECK(findDeviceModel(ComponentKind::Diode, "1N4007") != nullptr);
    auto reg = [](const char* name) { return CustomPartRegistry::instance().registerPart(findStandardPart(name)->spec); };
    auto pinName = [](const std::shared_ptr<const CustomPart>& part, const PadDef& pad) {
        return pad.pinIndex >= 0 ? part->def.pins[static_cast<size_t>(pad.pinIndex)].name : std::string("-");
    };

    // Datasheet pin names from the generated catalog.
    CHECK(reg("LM358DR")->def.pins[7].name == "V+" || reg("LM358DR")->def.pins[7].number == "8");
    CHECK(reg("MAX485ESA+T")->def.pins[5].name == "A" && reg("MAX485ESA+T")->def.pins[6].name == "B");
    CHECK(reg("L298HN")->def.pins[3].name == "Vs");
    CHECK(reg("TCAN1042VDRQ1")->def.pins[4].name == "VIO");

    // FPGA: 324 balls in an 18 × 18 grid at 0.8 mm, named A1…V18 (no I / O / Q / S rows), pins on their balls.
    auto fpga = reg("XC7A35T-1CSG324I");
    CHECK(fpga->footprint.pads.size() == 324);
    CHECK(fpga->footprint.pads.front().round);
    CHECK(std::fabs(fpga->footprint.pads[1].offset.x - fpga->footprint.pads[0].offset.x - 0.8) < 1e-9);
    size_t mapped = 0;
    for (const auto& pad : fpga->footprint.pads) mapped += pad.pinIndex >= 0 ? 1 : 0;
    CHECK(mapped == 324);
    bool hasV18 = false, hasI = false;
    for (const auto& pin : fpga->def.pins) {
        hasV18 |= pin.number == "V18";
        hasI |= pin.number[0] == 'I' || pin.number[0] == 'O';
    }
    CHECK(hasV18 && !hasI);

    // D²PAK buck: five leads plus the tab soldered to pin 3 (GND); SOT-223 LDO: tab on pin 2 (VO).
    auto buck = reg("LM2596S-5.0");
    CHECK(buck->footprint.pads.size() == 6);
    CHECK(pinName(buck, buck->footprint.pads.back()) == "GND");
    CHECK(buck->footprint.pads.back().size.x > 9);
    auto ldo = reg("AMS1117-3.3");
    CHECK(ldo->footprint.pads.size() == 4 && pinName(ldo, ldo->footprint.pads.back()) == "VO");

    // Multiwatt-15: through-hole, staggered rows.
    auto l298 = reg("L298HN");
    CHECK(l298->footprint.pads.size() == 15 && l298->footprint.pads[0].throughHole);
    CHECK(l298->footprint.pads[0].offset.y != l298->footprint.pads[1].offset.y);

    // Power MOSFET SON: the drain tab joins the drain pins; TQFP-48 drivers get their exposed pad.
    auto fet = reg("CSD18540Q5B");
    CHECK(fet->footprint.pads.size() == 9 && pinName(fet, fet->footprint.pads.back()) == "D");
    CHECK(reg("TMC2160-TA")->footprint.pads.size() == 49);
    CHECK(reg("TMC2209-LA")->footprint.pads.size() == 29);
    // Wide-body SOIC and SC70 / SOT-23-5 packages.
    CHECK(reg("ADM2587EBRWZ")->footprint.body.width > 7);
    CHECK(reg("MAX4466EXK+T")->footprint.pads.size() == 5);
    // Linear regulators simulate.
    CHECK(findStandardPart("LM7812")->spec.model.hasRegulator && findStandardPart("AMS1117-3.3")->spec.model.hasRegulator);
}

TEST(passive_package_variants) {
    // Resistors, capacitors, inductors, diodes and LEDs can be fitted in the package the BOM calls for: chip sizes,
    // through-hole, tantalum and electrolytic capacitors, SMA / DO-41 diodes.
    Project p = amplifierProject();
    auto& s = p.schematic;
    int r = -1, c = -1, d = -1;
    for (const auto& comp : s.components()) {
        if (r < 0 && comp.kind == ComponentKind::Resistor) r = comp.id;
        if (c < 0 && comp.kind == ComponentKind::Capacitor) c = comp.id;
    }
    d = s.addComponent(ComponentKind::Diode, "1N4007", {500, 0});
    CHECK(r >= 0 && c >= 0 && d >= 0);
    CHECK(Library::packageVariants(ComponentKind::Resistor).size() == 5);
    CHECK(Library::packageVariants(ComponentKind::OpAmp).empty());
    for (ComponentKind k : {ComponentKind::Resistor, ComponentKind::Capacitor, ComponentKind::Inductor, ComponentKind::Diode,
                            ComponentKind::LED})
        for (const auto& v : Library::packageVariants(k)) CHECK(Library::instance().footprint(v) != nullptr);
    CHECK(Library::packageLabel("CP_Tant_B") == "Tantalum B (3528)");

    // Default footprint until a variant is chosen; invalid variants are refused.
    CHECK(s.find(r)->footprintName() == "R_0805" && s.find(r)->package.empty());
    CHECK(!s.setPackage(r, "D_SMA") && !s.setPackage(r, "nonsense"));
    CHECK(s.setPackage(r, "R_0603") && s.find(r)->footprintName() == "R_0603");
    CHECK(s.setPackage(c, "CP_Tant_B") && s.setPackage(d, "D_DO41_THT"));
    p.pcb.autoPlace(s, true);
    const auto pads = p.pcb.pads(s);
    for (const auto& pad : pads) {
        if (pad.componentId == r) CHECK(std::fabs(pad.size.x - 0.9) < 1e-9 || std::fabs(pad.size.y - 0.9) < 1e-9);
        if (pad.componentId == d) CHECK(pad.throughHole && pad.drill > 0.9);
    }
    // BOM, export, save / load and the snapshot carry the package.
    Json snap = p.snapshot();
    bool sawOptions = false;
    for (const auto& j : snap.get("components").items()) {
        if (j.get("id").asInt() == c) {
            CHECK(j.get("footprint").asString() == "CP_Tant_B" && j.get("package").asString() == "CP_Tant_B");
            CHECK(j.get("packageOptions").items().size() == 8);
            sawOptions = true;
        }
    }
    CHECK(sawOptions);
    CHECK(exportBomCsv(s).find("Tantalum B") != std::string::npos);
    Project q = Project::fromJson(Json::parse(p.toJson().dump()));
    CHECK(q.schematic.find(r)->footprintName() == "R_0603" && q.schematic.find(d)->footprintName() == "D_DO41_THT");
    // Back to the default clears the variant.
    CHECK(s.setPackage(r, "R_0805") && s.find(r)->package.empty());
    // 3D: the tantalum is a moulded body, the axial diode a through-hole part with leads under the board.
    Mesh m = buildAssemblyMesh(s, p.pcb);
    Rect dr = p.pcb.courtyard(*s.find(d));
    CHECK(anyIn(surfacePoints(m, Surface::Hole), dr, -p.pcb.settings.thickness - 0.1, 0.1));

    // Crystals in the catalog's packages.
    auto reg = [](const char* name) { return CustomPartRegistry::instance().registerPart(findStandardPart(name)->spec); };
    CHECK(reg("Crystal_16MHz_3225")->footprint.pads.size() == 4);
    CHECK(reg("Crystal_32.768kHz_3215")->footprint.pads.size() == 2 && !reg("Crystal_32.768kHz_3215")->footprint.pads[0].throughHole);
    CHECK(reg("Crystal_32.768kHz_Cylinder")->footprint.pads[0].throughHole);
    CHECK(reg("Crystal_16MHz")->footprint.pads[0].throughHole);

    // C API.
    SiedaProject* api = sieda_project_new("pkg");
    int32_t rr = sieda_add_component(api, static_cast<int32_t>(ComponentKind::Resistor), "10k", 0, 0, 0, "R1");
    CHECK(sieda_set_component_package(api, rr, "R_Axial_THT") == 1);
    CHECK(sieda_set_component_package(api, rr, "C_0603") == 0);
    sieda_project_free(api);
}

TEST(c_api_smoke) {
    int rc = sieda_c_api_smoke_test();
    if (rc != 0) std::printf("    c api step %d failed\n", rc);
    CHECK(rc == 0);
}

int main(int argc, char** argv) {
    // Optional arguments: run only the named tests.
    const std::set<std::string> only(argv + 1, argv + argc);
    for (const auto& t : registry()) {
        if (!only.empty() && !only.count(t.name)) continue;
        int before = g_failures;
        std::printf("[ RUN  ] %s\n", t.name);
        t.fn();
        std::printf("[ %s ] %s\n", g_failures == before ? " OK " : "FAIL", t.name);
    }
    std::printf("\n%d checks, %d failures, %zu tests\n", g_checks, g_failures, registry().size());
    return g_failures == 0 ? 0 : 1;
}

TEST(ina333_instrumentation_amplifier) {
    // ECG front end: 3.3 V, VREF = 1.65 V, a floating 1 mV electrode signal through 10 kΩ protection resistors with
    // 1 MΩ bias returns to VREF, RG = 1 kΩ → G = 101.
    Project p;
    auto& s = p.schematic;
    std::string id = p.addCustomPart(findStandardPart("INA333")->spec);
    int vs = s.addComponent(ComponentKind::VoltageSource, "3.3", {0, 0});
    int gnd = s.addComponent(ComponentKind::Ground, "", {0, 100});
    int r3 = s.addComponent(ComponentKind::Resistor, "100k", {100, 0});
    int r4 = s.addComponent(ComponentKind::Resistor, "100k", {100, 100});
    int sig = s.addComponent(ComponentKind::VoltageSource, "0.001", {0, 300});
    int r1 = s.addComponent(ComponentKind::Resistor, "10k", {100, 250});
    int r2 = s.addComponent(ComponentKind::Resistor, "10k", {100, 350});
    int r5 = s.addComponent(ComponentKind::Resistor, "1M", {200, 250});
    int r6 = s.addComponent(ComponentKind::Resistor, "1M", {200, 350});
    int u = s.addCustomComponent(id, "INA333", {300, 300});
    int rg = s.addComponent(ComponentKind::Resistor, "1k", {300, 200});
    int load = s.addComponent(ComponentKind::Resistor, "10k", {400, 300});
    wire(s, vs, "-", gnd, "GND");
    wire(s, vs, "+", r3, "1");
    wire(s, r3, "2", r4, "1");
    wire(s, r4, "2", gnd, "GND");
    wire(s, sig, "+", r1, "1");
    wire(s, r1, "2", u, "+IN");
    wire(s, sig, "-", r2, "1");
    wire(s, r2, "2", u, "-IN");
    wire(s, u, "+IN", r5, "1");
    wire(s, r5, "2", r3, "2");
    wire(s, u, "-IN", r6, "1");
    wire(s, r6, "2", r3, "2");
    wire(s, u, "1", rg, "1");
    wire(s, rg, "2", u, "8");
    wire(s, u, "V+", vs, "+");
    wire(s, u, "V-", gnd, "GND");
    wire(s, u, "REF", r3, "2");
    wire(s, u, "VOUT", load, "1");
    wire(s, load, "2", gnd, "GND");
    p.schematicChanged();

    auto out = [&] {
        DcResult r = Simulator(s).dcOperatingPoint();
        CHECK(r.converged);
        return r.converged ? netV(s, r, u, "VOUT") - netV(s, r, u, "REF") : NAN;
    };
    double vdiff = 0.001 * 2e6 / 2.02e6;  // divided by the protection resistors
    double g101 = out();
    CHECK(std::fabs(g101 - 101 * vdiff) < 1e-3);

    // Two RG resistors in parallel (500 Ω) → G = 201.
    int rg2 = s.addComponent(ComponentKind::Resistor, "1k", {300, 150});
    wire(s, rg2, "1", u, "1");
    wire(s, rg2, "2", u, "8");
    p.schematicChanged();
    CHECK(std::fabs(out() - 201 * vdiff) < 2e-3);

    // RG open → unity gain.
    s.removeComponent(rg);
    s.removeComponent(rg2);
    p.schematicChanged();
    CHECK(std::fabs(out() - vdiff) < 1e-4);

    // A large input saturates just inside the rails instead of following REF + G·Vd.
    int rgBig = s.addComponent(ComponentKind::Resistor, "100", {300, 200});
    wire(s, rgBig, "1", u, "1");
    wire(s, rgBig, "2", u, "8");
    s.setValue(sig, "0.1");
    p.schematicChanged();
    DcResult sat = Simulator(s).dcOperatingPoint();
    CHECK(sat.converged);
    double vo = netV(s, sat, u, "VOUT");
    CHECK(vo > 3.2 && vo < 3.3);
    s.setValue(sig, "-0.1");
    p.schematicChanged();
    DcResult neg = Simulator(s).dcOperatingPoint();
    CHECK(neg.converged && netV(s, neg, u, "VOUT") < 0.1 && netV(s, neg, u, "VOUT") > 0);
}

TEST(project_reset_returns_to_a_blank_project) {
    auto save = [](SiedaProject* p) {
        char* j = sieda_project_save_json(p);
        std::string s = j ? j : "";
        sieda_string_free(j);
        return s;
    };
    SiedaProject* blank = sieda_project_new(nullptr);
    std::string fresh = save(blank);
    sieda_project_free(blank);

    SiedaProject* p = sieda_project_new("Old design");
    char* err = nullptr;
    char* all = sieda_standard_parts_json();
    Json parts = Json::parse(std::string(all));
    sieda_string_free(all);
    std::string specJson;
    for (const auto& item : parts.items())
        if (item.get("spec").get("name").asString() == "NE555") specJson = item.get("spec").dump();
    char* partJson = sieda_custom_part_register(p, specJson.c_str(), &err);
    CHECK(partJson != nullptr);
    sieda_string_free(partJson);
    sieda_add_component(p, static_cast<int32_t>(ComponentKind::Resistor), "1k", 0, 0, 0, "R1");
    sieda_project_set_requirements(p, "a PRD");
    CHECK(sieda_project_set_industry(p, "medical") == 1);
    sieda_pcb_set_board(p, 77, 55, 0.4, 0.3);
    CHECK(sieda_pcb_add_mounting_hole(p, 3, 3, 3.2, 1) >= 0);
    CHECK(save(p) != fresh);

    sieda_project_clear(p);  // keeps the library and board (used when a plan rebuilds the schematic)
    CHECK(save(p).find("NE555") != std::string::npos);
    sieda_project_reset(p);
    CHECK(save(p) == fresh);
    sieda_project_free(p);
}

TEST(microcontroller_library_by_vendor) {
    // Ten microcontrollers per vendor group, each on its real package: lead count, pitch, body and exposed pad.
    std::map<std::string, int> perGroup;
    for (const auto& p : standardParts())
        if (p.category.rfind("Microcontrollers · ", 0) == 0) ++perGroup[p.category];
    // + the catalog's LPC1769 (CNC / 3D-printer boards) and the general-purpose RP2350B.
    CHECK(perGroup["Microcontrollers · Arm"] == 12);
    // + the catalog's STM32F405 / H743 / F765, the robotics spares STM32F446 / G474 / H723 and 11 general-purpose
    // STM32s (F0, F1, F3, F4, G0, L0, L4, WB, C0).
    CHECK(perGroup["Microcontrollers · STMicroelectronics"] == 27);
    CHECK(perGroup["Microcontrollers · Texas Instruments"] == 10);
    // + ATmega328P, ATtiny85, the rad-tolerant ATmegaS128, the catalog's ATMEGA328P-AU / -PU and SAM D51, and 8
    // general-purpose AVR / SAM D parts.
    CHECK(perGroup["Microcontrollers · Microchip"] == 24);
    CHECK(perGroup["Microcontrollers · Espressif"] == 3);  // ESP32-C3, ESP32-S3, ESP32-PICO-D4
    for (const auto& p : standardParts()) {
        if (p.category.rfind("Microcontrollers · ", 0) != 0) continue;
        bool ok = true;
        try {
            auto part = CustomPartRegistry::instance().registerPart(p.spec);
            ok = part->footprint.pads.size() >= static_cast<size_t>(p.spec.package.pinCount) && !part->def.pins.empty();
        } catch (const std::exception&) {
            ok = false;
        }
        CHECK(ok);
    }

    struct Expect {
        const char* name;
        int leads;
        double pitch;   // centre-to-centre of pads 1 and 2
        bool exposedPad;
        const char* pin1;
    };
    const Expect expect[] = {
        {"RP2040", 56, 0.4, true, "IOVDD"},           {"nRF52832", 48, 0.4, true, "DEC1"},
        {"STM32F103C8T6", 48, 0.5, false, "VBAT"},    {"STM32F030F4P6", 20, 0.65, false, "BOOT0"},
        {"STM32F042K6T6", 32, 0.8, false, "VDD"},     {"STM32L432KCU6", 32, 0.5, true, "VDD"},
        {"ATmega32U4", 44, 0.8, false, "PE6"},        {"ATmega2560", 100, 0.5, false, "PG5"},
        {"ATtiny1614", 14, 1.27, false, "VCC"},       {"PIC16F877A", 40, 2.54, false, "nMCLR/Vpp"},
        {"MSP430G2553", 20, 2.54, false, "DVCC"},     {"MSP432E401Y", 128, 0.4, false, nullptr},
        {"EFM32HG308F64", 24, 0.65, true, nullptr},   {"LPC1768", 100, 0.5, false, nullptr},
    };
    for (const auto& e : expect) {
        const StandardPart* sp = findStandardPart(e.name);
        CHECK(sp != nullptr);
        if (!sp) continue;
        auto part = CustomPartRegistry::instance().registerPart(sp->spec);
        const auto& pads = part->footprint.pads;
        CHECK(static_cast<int>(pads.size()) == e.leads + (e.exposedPad ? 1 : 0));
        double pitch = std::hypot(pads[1].offset.x - pads[0].offset.x, pads[1].offset.y - pads[0].offset.y);
        CHECK(std::fabs(pitch - e.pitch) < 1e-6);
        // Every lead pad is wired to its pin; the exposed pad (if any) to the last listed pin.
        for (int i = 0; i < e.leads; ++i) CHECK(pads[static_cast<size_t>(i)].pinIndex == i);
        if (e.exposedPad) CHECK(pads.back().pinIndex == e.leads && pads.back().size.x > 1.5);
        if (e.pin1) CHECK(sp->spec.pins[0].name == e.pin1);
        // Neighbouring pads never overlap (fine-pitch packages included).
        CHECK(pads[0].size.y < e.pitch || pads[0].size.x < e.pitch);
    }
    // Real body sizes: LQFP-100 14 × 14 mm, QFN-56 7 × 7 mm.
    auto body = [](const char* n) { return CustomPartRegistry::instance().registerPart(findStandardPart(n)->spec)->footprint.body; };
    CHECK(std::fabs(body("STM32F407VGT6").width - 14) < 1e-9);
    CHECK(std::fabs(body("RP2040").width - 7) < 1e-9);
    // Pitch and body survive the JSON round trip, and parts without them keep their ids.
    CustomPartSpec spec = findStandardPart("RP2040")->spec;
    CustomPartSpec back = customPartSpecFromJson(customPartSpecToJson(spec));
    CHECK(back.package.pitch == 0.4 && back.package.bodySize == 7);
    CHECK(customPartSpecToJson(findStandardPart("NE555")->spec).get("package").get("pitch").isNull());
}

TEST(battery_and_ac_sources) {
    // A 9 V battery lighting a 1 kΩ load, and a 12 V rms / 50 Hz AC source into another.
    Project p;
    auto& s = p.schematic;
    int bt = s.addComponent(ComponentKind::Battery, "", {0, 0});
    int ac = s.addComponent(ComponentKind::ACSource, "", {0, 200});
    int r1 = s.addComponent(ComponentKind::Resistor, "1k", {100, 0});
    int r2 = s.addComponent(ComponentKind::Resistor, "1k", {100, 200});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 100});
    CHECK(s.find(bt)->value == "9" && s.find(bt)->ref.rfind("BT", 0) == 0);
    CHECK(s.find(ac)->value == "SIN(0 17 50)" && s.find(ac)->ref.rfind("VAC", 0) == 0);
    wire(s, bt, "+", r1, "1");
    wire(s, r1, "2", g, "GND");
    wire(s, bt, "-", g, "GND");
    wire(s, ac, "+", r2, "1");
    wire(s, r2, "2", g, "GND");
    wire(s, ac, "-", g, "GND");
    p.schematicChanged();

    DcResult dc = Simulator(s).dcOperatingPoint();
    CHECK(dc.converged);
    CHECK(std::fabs(netV(s, dc, r1, "1") - 9.0) < 1e-6);
    TransientResult tr = Simulator(s).transient(0.04, 1e-4);  // two 50 Hz cycles
    CHECK(tr.ok);
    const auto& wave = tr.netVoltages[static_cast<size_t>(s.netOf({r2, 0}))];
    double hi = *std::max_element(wave.begin(), wave.end()), lo = *std::min_element(wave.begin(), wave.end());
    CHECK(hi > 16.5 && lo < -16.5);  // ±17 V peak

    // Both export as SPICE voltage sources (named V…), and a shorted battery is caught by ERC.
    std::string spice = exportSpiceNetlist(s, "sources");
    CHECK(spice.find("V" + s.find(bt)->ref + " ") != std::string::npos);
    CHECK(spice.find(s.find(ac)->ref + " ") != std::string::npos && spice.find("SIN(0 17 50)") != std::string::npos);
    int shorted = s.addComponent(ComponentKind::Battery, "3", {300, 0});
    wire(s, shorted, "+", g, "GND");
    wire(s, shorted, "-", g, "GND");
    bool shortFound = false;
    for (const auto& v : s.runERC())
        if (v.code == "ERC_SHORTED_SOURCE") shortFound = true;
    CHECK(shortFound);
}

TEST(wire_junctions) {
    // The schematic a user draws: 9 V battery → 10 kΩ → red LED, then a capacitor added in parallel with the LED by
    // dropping its pins onto the existing wires (T-junctions), not onto other pins.
    Project p;
    auto& s = p.schematic;
    int bt = s.addComponent(ComponentKind::Battery, "9", {0, 0});
    int r1 = s.addComponent(ComponentKind::Resistor, "10k", {100, -60});
    int d1 = s.addComponent(ComponentKind::LED, "Red", {300, 0}, 90);
    int c1 = s.addComponent(ComponentKind::Capacitor, "100n", {200, 0}, 90);
    int g = s.addComponent(ComponentKind::Ground, "", {0, 100});
    wire(s, bt, "+", r1, "1");
    int top = s.connect({r1, 1}, {d1, 0});
    int bottom = s.connect({d1, 1}, {bt, 1});
    wire(s, bt, "-", g, "GND");
    CHECK(top >= 0 && bottom >= 0);

    int jt = s.splitWire(top, {200, -60});
    int jb = s.splitWire(bottom, {200, 60});
    CHECK(jt >= 0 && jb >= 0 && jt != jb);
    CHECK(s.find(jt)->kind == ComponentKind::Junction && s.find(jt)->ref[0] == '#');
    CHECK(s.connect({c1, 0}, {jt, 0}) >= 0);
    CHECK(s.connect({c1, 1}, {jb, 0}) >= 0);
    CHECK(s.wireCount(jt) == 3 && s.wireCount(jb) == 3);
    // Splitting at an existing junction end reuses it.
    CHECK(s.splitWire(s.wires().back().id, {200, 60}) == jb);

    // C1 is in parallel with D1; junctions join nets but are not net members.
    CHECK(s.netOf({c1, 0}) == s.netOf({d1, 0}) && s.netOf({c1, 0}) == s.netOf({r1, 1}));
    CHECK(s.netOf({c1, 1}) == s.netOf({d1, 1}) && s.netOf({c1, 1}) == s.groundNet());
    CHECK(s.netOf({jt, 0}) == s.netOf({c1, 0}));
    for (const auto& n : s.nets())
        for (const auto& pin : n.pins) CHECK(s.find(pin.component)->kind != ComponentKind::Junction);
    for (const auto& v : s.runERC()) {
        CHECK(v.code != "ERC_UNCONNECTED_PIN" && v.code != "ERC_DANGLING_WIRE" && v.code != "ERC_DUPLICATE_REF");
    }
    p.schematicChanged();
    DcResult dc = Simulator(s).dcOperatingPoint();
    CHECK(dc.converged);
    double vled = netV(s, dc, d1, "A") - netV(s, dc, d1, "K");
    CHECK(std::fabs(netV(s, dc, c1, "1") - netV(s, dc, c1, "2") - vled) < 1e-6 && vled > 1.5 && vled < 2.2);
    CHECK(exportSpiceNetlist(s, "j").find(s.find(jt)->ref) == std::string::npos);
    CHECK(exportBomCsv(s).find("Junction") == std::string::npos);

    // Round trip through the project file.
    Project q = Project::fromJson(Json::parse(p.toJson().dump()));
    CHECK(q.schematic.find(jt) && q.schematic.find(jt)->kind == ComponentKind::Junction);
    CHECK(q.schematic.netOf({c1, 0}) == q.schematic.netOf({d1, 0}));

    // A bend point: move it anywhere, delete it and the wire straightens (connection kept).
    int r1d1 = -1;
    for (const auto& w : s.wires())
        if ((w.a.component == jt && w.b.component == d1) || (w.b.component == jt && w.a.component == d1)) r1d1 = w.id;
    int bend = s.splitWire(r1d1, {250, -60});
    CHECK(s.moveComponent(bend, {250, -100}));
    CHECK(s.netOf({bend, 0}) == s.netOf({d1, 0}));
    CHECK(s.removeComponent(bend));
    CHECK(s.netOf({d1, 0}) == s.netOf({c1, 0}));

    // Deleting C1's wire leaves its junction as a bend point; deleting the last wires removes the junction.
    int cw = -1;
    for (const auto& w : s.wires())
        if (w.a.component == c1 && w.a.pin == 0) cw = w.id;
    CHECK(s.removeWire(cw));
    CHECK(s.find(jt) && s.wireCount(jt) == 2);

    // A wire abandoned in empty space is a dangling junction chain: reported, then cleaned up.
    int j1 = s.addComponent(ComponentKind::Junction, "", {200, -150});
    int j2 = s.addComponent(ComponentKind::Junction, "", {260, -150});
    s.connect({c1, 0}, {j1, 0});
    s.connect({j1, 0}, {j2, 0});
    bool dangling = false;
    for (const auto& v : s.runERC())
        if (v.code == "ERC_DANGLING_WIRE") dangling = true;
    CHECK(dangling);
    CHECK(s.netOf({c1, 0}) >= 0 && s.nets()[static_cast<size_t>(s.netOf({c1, 0}))].pins.size() == 1);  // still open
    CHECK(s.removeDanglingJunctions(j2) == 2);
    CHECK(!s.find(j1) && !s.find(j2) && s.wireCount(c1) == 1);
}

TEST(solder_mask_colours) {
    // Green is the default (like most boards); the other fab colours change the 3D board and are saved.
    Project p = amplifierProject();
    p.pcb.autoPlace(p.schematic, true);
    CHECK(p.pcb.settings.solderMask == "green");
    CHECK(solderMaskStyles().size() == 7 && std::string(solderMaskStyles().front().name) == "green");
    auto topColour = [&]() {
        Mesh m = buildAssemblyMesh(p.schematic, p.pcb, {false, false, false});
        // The first box is the board core; find a vertex on its top face (normal +Y).
        for (size_t v = 0; v < m.vertexCount(); ++v)
            if (m.normals[v * 3 + 1] > 0.99f) return Rgba{m.colors[v * 4], m.colors[v * 4 + 1], m.colors[v * 4 + 2], 1};
        return Rgba{};
    };
    Rgba green = topColour();
    CHECK(green.g > green.r * 3 && green.g > green.b);
    for (const char* name : {"black", "blue", "red", "yellow", "white", "purple"}) {
        CHECK(findSolderMask(name) != nullptr);
        p.pcb.settings.solderMask = name;
        Rgba c = topColour();
        CHECK(c.r == findSolderMask(name)->mask.r && c.g == findSolderMask(name)->mask.g);
    }
    CHECK(findSolderMask("yellow")->silk.r < 0.1f && findSolderMask("white")->silk.r < 0.1f);  // black legend
    CHECK(findSolderMask("red")->silk.r > 0.9f);
    CHECK(!findSolderMask("orange"));

    p.pcb.settings.solderMask = "purple";
    Project q = Project::fromJson(Json::parse(p.toJson().dump()));
    CHECK(q.pcb.settings.solderMask == "purple");
    Json old = p.toJson();
    old["board"]["solderMask"] = std::string("tartan");
    CHECK(Project::fromJson(old).pcb.settings.solderMask == "green");
}

TEST(assembly_mesh_follows_layer_count) {
    // The 3D board shows the stack-up picked in PCB Layout: a single-sided board has a bare FR-4 underside, two layers
    // have solder mask on both sides, and 4/6-layer boards show their inner copper as bands on the board edge.
    Project p = amplifierProject();
    p.pcb.autoPlace(p.schematic, true);
    const Rgba mask = findSolderMask("green")->mask;
    auto count = [](const Mesh& m, auto pred) {
        int n = 0;
        for (size_t v = 0; v < m.vertexCount(); ++v)
            if (pred(Rgba{m.colors[v * 4], m.colors[v * 4 + 1], m.colors[v * 4 + 2], 1}, m.normals[v * 3 + 1],
                     m.positions[v * 3 + 1]))
                ++n;
        return n;
    };
    auto same = [](Rgba a, Rgba b) { return std::fabs(a.r - b.r) < 1e-4 && std::fabs(a.g - b.g) < 1e-4 && std::fabs(a.b - b.b) < 1e-4; };
    const Rgba innerCopper{0.80f, 0.52f, 0.28f, 1};
    const double t = p.pcb.settings.thickness;
    for (int layers : {1, 2, 4, 6}) {
        p.pcb.settings.layerCount = layers;
        Mesh m = buildAssemblyMesh(p.schematic, p.pcb, {false, false, false});
        CHECK(m.normals[1] > 0.99f && same({m.colors[0], m.colors[1], m.colors[2], 1}, mask));  // green top
        int maskUnderside = count(m, [&](Rgba c, float ny, float py) { return ny < -0.99f && std::fabs(py + t) < 1e-3 && same(c, mask); });
        int bareUnderside = count(m, [&](Rgba c, float ny, float py) { return ny < -0.99f && std::fabs(py + t) < 1e-3 && !same(c, mask); });
        int bands = count(m, [&](Rgba c, float ny, float) { return ny > 0.99f && same(c, innerCopper); }) / 4;  // 4 vertices per top quad
        CHECK((layers == 1) == (maskUnderside == 0));
        CHECK((layers == 1) == (bareUnderside > 0));
        CHECK(bands == std::max(0, layers - 2));
    }
}

TEST(fabrication_package_is_complete) {
    Project p = amplifierProject();
    p.name = "Amp: rev A";
    p.pcb.autoPlace(p.schematic, true);
    p.pcb.settings.layerCount = 4;
    p.pcb.settings.solderMask = "black";
    p.pcb.settings.holes.push_back({{3, 3}, 3.2, 6.4});
    CHECK(p.pcb.autoRoute(p.schematic).failed == 0);
    // One SMD part on the bottom side.
    for (auto& c : p.schematic.mutableComponents())
        if (c.kind == ComponentKind::Resistor && c.hasFootprint()) {
            c.pcb.bottom = true;
            break;
        }
    auto files = fabricationPackage(p);
    std::map<std::string, std::string> byName;
    for (const auto& f : files) byName[f.path] = f.content;
    auto has = [&](const std::string& path) { return byName.count(path) == 1 && !byName[path].empty(); };
    const std::string g = "gerbers/Amp_rev_A";
    for (const char* layer : {"-F_Cu", "-In1_Cu", "-In2_Cu", "-B_Cu", "-F_Mask", "-B_Mask", "-F_Paste", "-B_Paste",
                              "-F_Silkscreen", "-B_Silkscreen", "-Edge_Cuts"})
        CHECK(has(g + layer + ".gbr"));
    CHECK(has(g + "-PTH.drl") && has(g + "-NPTH.drl") && has(g + "-ipc356.ipc") && has(g + "-job.gbrjob"));
    for (const char* a : {"-bom.csv", "-bom_assembly.csv", "-cpl.csv", "-pick_and_place.csv", "-assembly_top.svg",
                          "-assembly_bottom.svg"})
        CHECK(has("assembly/Amp_rev_A" + std::string(a)));
    CHECK(has("Amp_rev_A-gerbers.zip") && has("fab_notes.txt") && has("Amp_rev_A-netlist.cir") && has("3d/Amp_rev_A.stl"));

    // Gerber X2 attributes, paste and silkscreen content.
    CHECK(byName[g + "-F_Mask.gbr"].find("%TF.FilePolarity,Negative*%") != std::string::npos);
    CHECK(byName[g + "-B_Paste.gbr"].find("%TF.FileFunction,Paste,Bot*%") != std::string::npos);
    CHECK(byName[g + "-B_Silkscreen.gbr"].find("%TF.FileFunction,Legend,Bot*%") != std::string::npos);
    int topSmd = 0;
    for (const auto& pd : p.pcb.pads(p.schematic)) topSmd += !pd.throughHole && pd.smdLayer == kTopLayer;
    size_t flashes = 0;
    const std::string& paste = byName[g + "-F_Paste.gbr"];
    for (size_t at = paste.find("D03*"); at != std::string::npos; at = paste.find("D03*", at + 1)) ++flashes;
    CHECK(static_cast<int>(flashes) == topSmd);
    // Designators are drawn: the top silkscreen has many more strokes than outlines alone.
    auto strokes = silkscreenText("R12", {0, 0}, 1.0, false);
    CHECK(strokes.size() >= 3);
    auto mirrored = silkscreenText("R12", {0, 0}, 1.0, true);
    CHECK(mirrored[0][0].x <= 0 && strokes[0][0].x >= 0);

    // IPC-D-356A: one test point per pad and via, fixed columns, terminated.
    const std::string& ipc = byName[g + "-ipc356.ipc"];
    int records = 0;
    std::istringstream lines(ipc);
    for (std::string line; std::getline(lines, line);)
        if (line.rfind("317", 0) == 0 || line.rfind("327", 0) == 0) {
            ++records;
            CHECK(line[26] == '-' && line[41] == 'X' && line[49] == 'Y');
        }
    CHECK(records == static_cast<int>(p.pcb.pads(p.schematic).size() + p.pcb.vias.size()));
    CHECK(ipc.size() >= 4 && ipc.compare(ipc.size() - 4, 4, "999\n") == 0);

    // Job file: stack-up with 4 copper layers, black mask, white legend.
    Json job = Json::parse(byName[g + "-job.gbrjob"]);
    CHECK(job.get("GeneralSpecs").get("LayerNumber").asInt() == 4);
    int copper = 0;
    bool black = false, white = false;
    for (const auto& l : job.get("MaterialStackup").items()) {
        copper += l.get("Type").asString() == "Copper";
        black |= l.get("Type").asString() == "SolderMask" && l.get("Color").asString() == "Black";
        white |= l.get("Type").asString() == "Legend" && l.get("Color").asString() == "White";
    }
    CHECK(copper == 4 && black && white);
    CHECK(job.get("FilesAttributes").size() == 11);

    // The zip holds the gerbers/ files (flat), each stored intact.
    const std::string& zip = byName["Amp_rev_A-gerbers.zip"];
    CHECK(zip.compare(0, 4, "PK\x03\x04") == 0);
    size_t entries = 0;
    for (size_t at = zip.find("PK\x01\x02"); at != std::string::npos; at = zip.find("PK\x01\x02", at + 1)) ++entries;
    size_t gerberFiles = 0;
    for (const auto& f : files) gerberFiles += f.path.rfind("gerbers/", 0) == 0;
    CHECK(entries == gerberFiles);
    CHECK(zip.find(byName[g + "-F_Cu.gbr"]) != std::string::npos);

    // Order notes carry what the fab asks for.
    const std::string& notes = byName["fab_notes.txt"];
    for (const char* text : {"Layers               4", "Thickness            1.60 mm", "Solder mask          Black",
                             "Silkscreen           White, top and bottom", "HASL lead-free", "Non-plated           1",
                             "IPC-D-356A", "Amp_rev_A-gerbers.zip"})
        CHECK(notes.find(text) != std::string::npos);
    std::string cpl = byName["assembly/Amp_rev_A-cpl.csv"];
    CHECK(cpl.find("Bottom") != std::string::npos && cpl.find("mm,") != std::string::npos);

    // Single-sided: no bottom copper or mask.
    p.pcb.settings.layerCount = 1;
    for (auto& c : p.schematic.mutableComponents()) c.pcb.bottom = false;
    std::set<std::string> single;
    for (const auto& f : fabricationPackage(p, "ss")) single.insert(f.path);
    CHECK(single.count("gerbers/ss-F_Cu.gbr") && !single.count("gerbers/ss-B_Cu.gbr") && !single.count("gerbers/ss-B_Mask.gbr"));

    // Written to disk.
    std::string dir = (std::filesystem::temp_directory_path() / "sieda_fab_test").string();
    std::filesystem::remove_all(dir);
    std::vector<std::string> written;
    std::string error;
    CHECK(writeFabricationPackage(p, dir, &written, &error, "ss"));
    CHECK(error.empty() && written.size() == single.size());
    CHECK(std::filesystem::exists(dir + "/gerbers/ss-F_Cu.gbr") && std::filesystem::exists(dir + "/ss-gerbers.zip"));
    std::filesystem::remove_all(dir);
}

TEST(net_roles_for_copper_colours) {
    Project p;
    auto& s = p.schematic;
    int bt = s.addComponent(ComponentKind::Battery, "9", {0, 0});
    int r1 = s.addComponent(ComponentKind::Resistor, "1k", {100, 0});
    int r2 = s.addComponent(ComponentKind::Resistor, "1k", {200, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 100});
    int vneg = s.addComponent(ComponentKind::VoltageSource, "12", {300, 0});   // + at ground, − is a −12 V rail
    int sig = s.addComponent(ComponentKind::VoltageSource, "SIN(0 1 1k)", {400, 0});  // a test signal, not a rail
    int label = s.addComponent(ComponentKind::NetLabel, "3V3", {500, 0});
    int r3 = s.addComponent(ComponentKind::Resistor, "1k", {500, 50});
    wire(s, bt, "+", r1, "1");
    wire(s, r1, "2", r2, "1");
    wire(s, r2, "2", g, "GND");
    wire(s, bt, "-", g, "GND");
    wire(s, vneg, "+", g, "GND");
    int r4 = s.addComponent(ComponentKind::Resistor, "1k", {300, 100});
    wire(s, vneg, "-", r4, "1");
    int r5 = s.addComponent(ComponentKind::Resistor, "1k", {400, 100});
    wire(s, sig, "+", r5, "1");
    wire(s, sig, "-", g, "GND");
    s.connect({label, 0}, {r3, 0});
    auto role = [&](int comp, int pin) { return s.netRole(s.netOf({comp, pin})); };
    CHECK(role(r1, 0) == NetRole::Power);            // battery +
    CHECK(role(r1, 1) == NetRole::Signal);           // divider midpoint
    CHECK(role(r2, 1) == NetRole::Ground);
    CHECK(role(r4, 0) == NetRole::NegativeSupply);   // −12 V rail
    CHECK(role(r5, 0) == NetRole::Signal);           // sine test source
    CHECK(role(r3, 0) == NetRole::Power);            // named 3V3
    CHECK(std::string(netRoleName(NetRole::Ground)) == "ground");
    // A timer IC's VCC / GND power pins mark their nets even without names or symbols.
    std::string id = p.addCustomPart(findStandardPart("NE555")->spec);
    int u = s.addCustomComponent(id, "", {600, 0});
    int r6 = s.addComponent(ComponentKind::Resistor, "1k", {600, 100});
    int r7 = s.addComponent(ComponentKind::Resistor, "1k", {700, 100});
    s.connect({u, s.pinIndex(u, "VCC")}, {r6, 0});
    s.connect({u, s.pinIndex(u, "GND")}, {r7, 0});
    CHECK(role(r6, 0) == NetRole::Power);
    CHECK(role(r7, 0) == NetRole::Ground);
    // In the UI view model.
    Json vm = Json::parse(p.snapshot().dump());
    bool sawPower = false;
    for (const auto& n : vm.get("nets").items()) sawPower |= n.get("role").asString() == "power";
    CHECK(sawPower);
}

TEST(routing_cleanup_chamfers_and_merges) {
    Project p;
    PcbLayout& pcb = p.pcb;
    auto add = [&](Vec2 a, Vec2 b, int net) {
        Track t;
        t.a = a;
        t.b = b;
        t.net = net;
        t.width = 0.25;
        pcb.addTrack(t);
    };
    add({10, 10}, {20, 10}, 1);  // right-angle corner at (20,10) in open board
    add({20, 10}, {20, 20}, 1);
    add({5, 30}, {10, 30}, 2);   // collinear pieces
    add({10, 30}, {15, 30}, 2);
    // A corner hemmed in by another net's track running diagonally across it: no room for a chamfer.
    add({30, 5}, {40, 5}, 3);
    add({40, 5}, {40, 15}, 3);
    add({39.4, 5.2}, {39.8, 5.6}, 4);
    int changes = pcb.cleanupRouting(p.schematic);
    CHECK(changes == 2);
    int diagonals = 0, net2 = 0;
    for (const auto& t : pcb.tracks) {
        if (t.net == 1 && std::fabs(std::fabs(t.b.x - t.a.x) - std::fabs(t.b.y - t.a.y)) < 1e-9) ++diagonals;
        net2 += t.net == 2;
    }
    CHECK(diagonals == 1 && net2 == 1);
    // Still one connected piece of copper from (10,10) to (20,20).
    bool startsAt = false, endsAt = false;
    for (const auto& t : pcb.tracks)
        if (t.net == 1) {
            startsAt |= (t.a - Vec2{10, 10}).length() < 1e-9 || (t.b - Vec2{10, 10}).length() < 1e-9;
            endsAt |= (t.a - Vec2{20, 20}).length() < 1e-9 || (t.b - Vec2{20, 20}).length() < 1e-9;
        }
    CHECK(startsAt && endsAt);
    int net3 = 0;
    for (const auto& t : pcb.tracks) net3 += t.net == 3;
    CHECK(net3 == 2);  // hemmed-in corner untouched
}

TEST(bill_of_materials) {
    Project p;
    auto& s = p.schematic;
    int bt = s.addComponent(ComponentKind::Battery, "9", {0, 0});
    std::vector<int> r;
    for (const char* v : {"10k", "10k", "4k7", "330", "10k"}) r.push_back(s.addComponent(ComponentKind::Resistor, v, {100, 0}));
    for (int i = 0; i < 6; ++i) s.addComponent(ComponentKind::Resistor, "1M", {100, 0});  // R6…R11: natural order
    int c1 = s.addComponent(ComponentKind::Capacitor, "100n", {200, 0});
    int d1 = s.addComponent(ComponentKind::LED, "Red", {300, 0});
    int q1 = s.addComponent(ComponentKind::NPN, "BC847", {400, 0});
    s.addComponent(ComponentKind::Ground, "", {0, 100});
    s.addComponent(ComponentKind::NetLabel, "VCC", {0, 100});

    auto lines = buildBom(s);
    auto find = [&](const std::string& value) -> const BomLine* {
        for (const auto& l : lines)
            if (l.value == value) return &l;
        return nullptr;
    };
    const BomLine* tenK = find("10k");
    CHECK(tenK && tenK->quantity() == 3 && tenK->refs[0] == "R1" && tenK->refs[2] == "R5");
    CHECK(tenK->suggestedManufacturer == "Yageo" && tenK->suggestedMpn == "RC0805FR-0710KL");
    CHECK(tenK->description == "Resistor 10kΩ ±1% 0805" && tenK->rating == "0.125 W");
    CHECK(find("4k7")->suggestedMpn == "RC0805FR-074K7L");
    CHECK(find("330")->suggestedMpn == "RC0805FR-07330RL");
    const BomLine* meg = find("1M");
    CHECK(meg && meg->refs.size() == 6 && meg->refs[4] == "R10" && meg->refs[5] == "R11");
    CHECK(find("100n")->rating == "≥ 25 V");  // 9 V supply, rated at twice the working voltage
    CHECK(find("BC847")->suggestedMpn == "BC847" && find("Red")->suggestedMpn.empty());
    for (const auto& l : lines) CHECK(l.type != "Ground" && l.type != "Net Label");
    CHECK(lines.front().item == 1 && lines.size() == 8);  // BT1, R×4 values, C, D, Q

    // Sourcing: part numbers, price and DNP flow into the totals and the assembly files.
    for (int id : {r[0], r[1], r[4]}) {
        auto* c = s.find(id);
        c->sourcing.manufacturer = "Yageo";
        c->sourcing.mpn = "RC0805FR-0710KL";
        c->sourcing.supplierPart = "C17414";
        c->sourcing.unitPrice = 0.002;
    }
    s.find(c1)->sourcing.unitPrice = 0.01;
    s.find(d1)->sourcing.dnp = true;
    lines = buildBom(s);
    BomSummary sum = summarizeBom(lines);
    CHECK(sum.dnp == 1 && sum.placements == 14);
    CHECK(std::fabs(sum.costPerBoard - (3 * 0.002 + 0.01)) < 1e-9);
    CHECK(find("10k")->notes.empty());  // part number and price entered
    CHECK(!find("4k7")->notes.empty() && find("4k7")->notes[0].find("suggested: RC0805FR-074K7L") != std::string::npos);
    std::string assembly = exportAssemblyBomCsv(s);
    CHECK(assembly.find("C17414") != std::string::npos && assembly.find("RC0805FR-0710KL") != std::string::npos);
    CHECK(assembly.find(s.find(d1)->ref) == std::string::npos);  // DNP not ordered
    std::string bom = exportBomCsv(s);
    CHECK(bom.rfind("Item,Quantity,References", 0) == 0 && bom.find("DNP") != std::string::npos);
    CHECK(bom.find("Yageo") != std::string::npos && bom.find("0.0060") != std::string::npos);  // line total
    s.find(d1)->pcb.placed = true;
    s.find(q1)->pcb.placed = true;
    std::string cpl = exportCplCsv(s, p.pcb);
    CHECK(cpl.find(s.find(q1)->ref + ",") != std::string::npos && cpl.find(s.find(d1)->ref + ",") == std::string::npos);

    // Saved with the project, together with the build quantity.
    p.buildQuantity = 25;
    Project q = Project::fromJson(Json::parse(p.toJson().dump()));
    CHECK(q.buildQuantity == 25);
    CHECK(q.schematic.find(r[0])->sourcing.supplierPart == "C17414" && q.schematic.find(d1)->sourcing.dnp);
    CHECK(q.schematic.find(bt)->sourcing.empty());
    Json j = bomJson(q.schematic, q.buildQuantity);
    CHECK(std::fabs(j.get("orderCost").asNumber() - 25 * (3 * 0.002 + 0.01)) < 1e-9);
    CHECK(j.get("summary").get("dnp").asInt() == 1);

    // C API: set sourcing field by field.
    SiedaProject* api = sieda_project_new("bom");
    int rid = sieda_add_component(api, static_cast<int32_t>(ComponentKind::Resistor), "10k", 0, 0, 0, "");
    CHECK(sieda_set_component_sourcing(api, rid, "{\"mpn\":\"X1\",\"unitPrice\":0.5}") == 1);
    CHECK(sieda_set_component_sourcing(api, rid, "{\"dnp\":true}") == 1);
    sieda_set_build_quantity(api, 4);
    char* out = sieda_bom_json(api);
    Json api_j = Json::parse(out);
    sieda_string_free(out);
    CHECK(api_j.get("lines")[0].get("mpn").asString() == "X1" && api_j.get("lines")[0].get("dnp").asBool());
    CHECK(api_j.get("buildQuantity").asInt() == 4);
    sieda_project_free(api);
}

TEST(ipc2221b_spacing_table_and_coating) {
    CHECK_NEAR(ipc2221Spacing(230, Ipc2221Column::B2), 1.25, 1e-12);
    CHECK_NEAR(ipc2221Spacing(230, Ipc2221Column::A5), 0.4, 1e-12);
    CHECK_NEAR(ipc2221Spacing(48, Ipc2221Column::B4), 0.13, 1e-12);
    CHECK_NEAR(ipc2221Spacing(12, Ipc2221Column::B1), 0.05, 1e-12);
    CHECK_NEAR(ipc2221Spacing(80, Ipc2221Column::A6), 0.5, 1e-12);
    CHECK_NEAR(ipc2221Spacing(1000, Ipc2221Column::A5), 3.05, 1e-9);
    CHECK_NEAR(ipc2221Clearance(230, false, true), 0.4, 1e-12);   // coated: A5 at any altitude
    CHECK_NEAR(ipc2221Clearance(230, true, false), 6.4, 1e-12);   // B3
    CHECK(std::string(ipc2221ColumnName(externalSpacingColumn(false, true))) == "A5");
    Project p;
    p.pcb.settings.coating = "silicone";
    Project q = Project::fromJson(Json::parse(p.toJson().dump()));
    CHECK(q.pcb.settings.coating == "silicone" && q.pcb.settings.coated());
    CHECK(leakageSpacing(q.pcb.settings) == 0.25);
    SiedaProject* api = sieda_project_new("c");
    CHECK(sieda_pcb_set_coating(api, "parylene") == 1 && sieda_pcb_set_coating(api, "glitter") == 0);
    sieda_project_free(api);
}

TEST(reliability_circuit_rules) {
    auto codes = [](const Project& p) {
        std::set<std::string> c;
        for (const auto& v : reliabilityChecks(p)) c.insert(v.code);
        return c;
    };
    // A relay coil / motor winding switched by a MOSFET with no flyback diode.
    Project p;
    p.industry = "automotive";
    auto& s = p.schematic;
    int bt = s.addComponent(ComponentKind::Battery, "12", {0, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 100});
    int l1 = s.addComponent(ComponentKind::Inductor, "10m", {100, 0});
    int q1 = s.addComponent(ComponentKind::NMOS, "2N7002", {200, 0});
    int pwm = s.addComponent(ComponentKind::VoltageSource, "PULSE(0 5 1m)", {300, 0});
    wire(s, bt, "+", l1, "1");
    wire(s, bt, "-", g, "GND");
    wire(s, l1, "2", q1, "D");
    wire(s, q1, "S", g, "GND");
    wire(s, pwm, "+", q1, "G");
    wire(s, pwm, "-", g, "GND");
    auto c = codes(p);
    CHECK(c.count("REL_FLYBACK"));
    CHECK(c.count("REL_LOAD_DUMP"));        // automotive supply without TVS
    CHECK(c.count("REL_THERMAL_CYCLING"));  // automotive guidance
    int d1 = s.addComponent(ComponentKind::Diode, "1N4148", {100, 50});
    wire(s, d1, "A", l1, "2");
    wire(s, d1, "K", l1, "1");
    int tvs = s.addComponent(ComponentKind::Diode, "SMBJ24A", {0, 50});
    wire(s, tvs, "K", bt, "+");
    wire(s, tvs, "A", g, "GND");
    c = codes(p);
    CHECK(!c.count("REL_FLYBACK") && !c.count("REL_LOAD_DUMP"));
    // The PWM-driven gate net is a fast net.
    NetClassification cls = classifyNets(p);
    CHECK(cls.fast.count(s.netOf({q1, 0})));

    // A half-bridge of two N-MOSFETs with no gate driver: shoot-through risk.
    int q2 = s.addComponent(ComponentKind::NMOS, "IRF540N", {400, 0});
    wire(s, q2, "D", bt, "+");
    wire(s, q2, "S", q1, "D");
    p.industry = "robotics";
    c = codes(p);
    CHECK(c.count("REL_SHOOT_THROUGH") && c.count("REL_SNUBBER") && c.count("REL_FLEX"));

    // A high-impedance amplifier input (10 MΩ bias resistor) wants a guard ring driven by the other input.
    Project a;
    auto& t = a.schematic;
    int v = t.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    int gg = t.addComponent(ComponentKind::Ground, "", {0, 100});
    int u = t.addComponent(ComponentKind::OpAmp, "LM358", {200, 0});
    int rb = t.addComponent(ComponentKind::Resistor, "10M", {100, 0});
    int rf = t.addComponent(ComponentKind::Resistor, "10k", {300, 0});
    wire(t, v, "-", gg, "GND");
    wire(t, rb, "1", u, "IN+");
    wire(t, rb, "2", gg, "GND");
    wire(t, u, "IN-", u, "OUT");  // follower
    wire(t, u, "OUT", rf, "1");
    wire(t, rf, "2", gg, "GND");
    wire(t, v, "+", rf, "1");
    NetClassification hz = classifyNets(a);
    int inPlus = t.netOf({u, 0});
    CHECK(hz.highImpedance.count(inPlus) && hz.highImpedance.at(inPlus) == t.netOf({u, 1}));
    CHECK(codes(a).count("REL_HIGH_IMPEDANCE"));
    // The autorouter keeps other nets the leakage spacing away from the high-impedance input.
    a.pcb.autoPlace(t, true);
    CHECK(a.pcb.autoRoute(t).failed == 0);
    CHECK(!codes(a).count("REL_LEAKAGE"));

    // Space: tin whiskers, outgassing, radiation; tin-lead solder and polyimide in the fab requirements.
    a.industry = "space";
    c = codes(a);
    CHECK(c.count("REL_TIN_WHISKER") && c.count("REL_OUTGASSING") && c.count("REL_RADIATION"));
    FabricationRequirements req = fabricationRequirements(a);
    CHECK(req.solder.find("Sn63Pb37") != std::string::npos && req.material.find("Polyimide") != std::string::npos);
    CHECK(req.ipcClass == 3 && recommendedSurfaceFinish(a).find("SnPb") != std::string::npos);
}

TEST(reliability_layout_rules) {
    auto has = [](const std::vector<RuleViolation>& v, const char* code) {
        return std::any_of(v.begin(), v.end(), [&](const RuleViolation& x) { return x.code == code; });
    };
    Project p = amplifierProject();
    p.pcb.autoPlace(p.schematic, true);
    CHECK(p.pcb.autoRoute(p.schematic).failed == 0);
    auto clean = reliabilityChecks(p);
    CHECK(!has(clean, "REL_ACID_TRAP") && !has(clean, "REL_SOLDER_BRIDGE") && !has(clean, "REL_VIA_ASPECT"));

    // An acute bend on one net (acid trap) and a 0.1 mm via in a 1.6 mm board (16:1).
    int net = p.pcb.tracks.front().net;
    Track a;
    a.net = net;
    a.layer = 0;
    a.width = 0.25;
    a.a = {2, 2};
    a.b = {6, 2};
    Track b = a;
    b.a = {6, 2};
    b.b = {3, 4};
    p.pcb.addTrack(a);
    p.pcb.addTrack(b);
    Via v;
    v.net = net;
    v.position = {2, 8};
    v.drill = 0.1;
    v.diameter = 0.35;
    p.pcb.addVia(v);
    // Two resistors pushed together: their pads nearly touch (solder bridge).
    Component* r1 = nullptr;
    Component* r2 = nullptr;
    for (auto& c : p.schematic.mutableComponents())
        if (c.kind == ComponentKind::Resistor && c.hasFootprint()) (r1 ? r2 : r1) = r1 ? (r2 ? r2 : &c) : &c;
    CHECK(r1 && r2);
    r1->pcb.rotation = 0;
    r2->pcb.rotation = 180;  // stacked, pads swapped: R2.2 over R1.1 — different nets face each other
    double padH = 0;
    for (const auto& pd : p.pcb.pads(p.schematic))
        if (pd.componentId == r1->id) padH = pd.size.y;
    r2->pcb.position = r1->pcb.position + Vec2{0, padH + 0.1};  // 0.1 mm between the pad rows
    auto found = reliabilityChecks(p);
    CHECK(has(found, "REL_ACID_TRAP"));
    CHECK(has(found, "REL_VIA_ASPECT"));
    CHECK(has(found, "REL_SOLDER_BRIDGE"));

    // RF line width for 50 Ω (IPC-2141 microstrip on FR-4).
    BoardSettings s;
    s.layerCount = 2;
    s.thickness = 1.6;
    double w2 = microstripWidth(50, s);
    CHECK(w2 > 2.6 && w2 < 3.2);
    s.layerCount = 4;
    double w4 = microstripWidth(50, s);
    CHECK(w4 > 0.3 && w4 < 0.45);  // 0.2 mm prepreg under the top layer of a standard 4-layer build

    // Verification has a Design for Reliability stage.
    VerificationReport rep = verifyDesign(p);
    bool stage = false;
    for (const auto& st : rep.stages) stage |= st.id == "reliability";
    CHECK(stage);
}

TEST(stackup_controlled_impedance) {
    // IPC-2141 sanity: wider track → lower impedance; stripline below microstrip for the same geometry.
    CHECK(microstripImpedance(0.3, 0.2, 0.035, 4.4) > microstripImpedance(0.5, 0.2, 0.035, 4.4));
    CHECK(striplineImpedance(0.2, 0.4, 0.035, 4.4) < microstripImpedance(0.2, 0.2, 0.035, 4.4) + 30);
    CHECK(findLaminate("rogers-4350b") && findLaminate("megtron-6") && !findLaminate("unobtainium"));

    BoardSettings s;
    s.layerCount = 6;
    s.thickness = 1.6;
    const double fr4 = widthForImpedance(s, 0, 50);
    CHECK(fr4 > 0);
    CHECK_NEAR(trackImpedance(s, 0, fr4), 50, 1.0);
    CHECK(isStriplineLayer(s, 1) && !isStriplineLayer(s, 0) && !isStriplineLayer(s, 5));
    CHECK(widthForImpedance(s, 2, 50) < fr4);  // stripline: narrower for the same 50 Ω
    BoardSettings four = s;
    four.layerCount = 4;  // standard 4-layer build: 0.2 mm prepreg, thick core
    CHECK(std::fabs(dielectricBelow(four, 0) - 0.2) < 1e-9 && dielectricBelow(four, 1) > 1.0);
    s.material = "rogers-4350b";
    CHECK(widthForImpedance(s, 0, 50) > fr4);  // lower εr → wider line
    auto [dw, gap] = differentialPairGeometry(s, 0, 100);
    CHECK(dw > 0 && gap >= dw - 1e-9);
    CHECK_NEAR(differentialMicrostrip(trackImpedance(s, 0, dw), gap, dielectricBelow(s, 0)), 100, 5);

    // Persistence of the stack-up and the C API.
    Project p = amplifierProject();
    p.pcb.settings.layerCount = 4;
    p.pcb.settings.material = "megtron-6";
    p.pcb.settings.construction = "rigid-flex";
    p.pcb.settings.singleEndedImpedance = 45;
    p.pcb.settings.backdrill = true;
    Project q = Project::fromJson(p.toJson());
    CHECK(q.pcb.settings.material == "megtron-6" && q.pcb.settings.construction == "rigid-flex");
    CHECK_NEAR(q.pcb.settings.singleEndedImpedance, 45, 1e-9);
    CHECK(q.pcb.settings.backdrill);
    Json sj = stackupJson(q.pcb.settings);
    CHECK(sj["materialName"].asString().find("Megtron") != std::string::npos);
    CHECK(sj["layers"].size() == 7);  // 4 copper + 3 dielectrics

    // A 4-layer via on a clock net that only connects the top layer leaves a stub → backdrill file.
    Project c = amplifierProject();
    auto& sch = c.schematic;
    int clk = sch.addComponent(ComponentKind::NetLabel, "CLK", {0, 200});
    int r = sch.addComponent(ComponentKind::Resistor, "33", {60, 200});
    int g = sch.addComponent(ComponentKind::Ground, "", {120, 200});
    wire(sch, clk, "N", r, "1");
    wire(sch, r, "2", g, "GND");
    c.schematicChanged();
    c.pcb.settings.layerCount = 4;
    c.pcb.autoPlace(c.schematic, true);  // footprints on the board: layout rules apply
    int net = sch.netOf({r, 0});
    Via v;
    v.net = net;
    v.position = {5, 5};
    c.pcb.addVia(v);
    Track t;
    t.net = net;
    t.layer = 0;
    t.width = 0.2;
    t.a = {5, 5};
    t.b = {9, 5};
    c.pcb.addTrack(t);
    auto stubs = viaStubs(c);
    CHECK(stubs.size() == 1);
    CHECK(stubs.size() == 1 && stubs[0].bottomStub > 1.0 && stubs[0].topStub == 0);
    auto codes = [&] {
        std::set<std::string> out;
        for (const auto& x : reliabilityChecks(c)) out.insert(x.code);
        return out;
    };
    CHECK(codes().count("REL_VIA_STUB"));
    c.pcb.settings.backdrill = true;
    CHECK(codes().count("REL_BACKDRILL") && !codes().count("REL_VIA_STUB"));
    CHECK(exportBackdrill(c).find("M48") != std::string::npos);

    // Metal-core boards are single/double-sided.
    c.pcb.settings.construction = "metal-core";
    CHECK(codes().count("REL_METAL_CORE_LAYERS"));
}

TEST(length_matching_serpentines) {
    Project p;
    auto& s = p.schematic;
    auto signal = [&](const char* name, double y) {
        int l = s.addComponent(ComponentKind::NetLabel, name, {0, y});
        int r = s.addComponent(ComponentKind::Resistor, "33", {60, y});
        int g = s.addComponent(ComponentKind::Ground, "", {120, y});
        wire(s, l, "N", r, "1");
        wire(s, r, "2", g, "GND");
        return r;
    };
    int dq0 = signal("DQ0", 0), dq1 = signal("DQ1", 100);
    int dp = signal("USB_DP", 200), dn = signal("USB_DN", 300);
    signal("A0", 400);
    signal("A1", 500);  // MCU analog pins — not a bus
    signal("LED1", 600);
    signal("LED2", 700);
    p.schematicChanged();

    auto groups = lengthGroups(s, p.pcb.settings);
    CHECK(groups.size() == 2);
    bool pair = false, bus = false;
    for (const auto& g : groups) {
        pair |= g.kind == "pair" && g.name == "USB_D" && g.nets.size() == 2;
        bus |= g.kind == "bus" && g.name == "DQ" && g.nets.size() == 2;
    }
    CHECK(pair && bus);

    p.pcb.settings.width = 100;
    p.pcb.settings.height = 80;
    int n0 = s.netOf({dq0, 0}), n1 = s.netOf({dq1, 0});
    auto track = [&](int net, Vec2 a, Vec2 b) {
        Track t;
        t.net = net;
        t.width = 0.2;
        t.a = a;
        t.b = b;
        p.pcb.addTrack(t);
    };
    track(n0, {10, 60}, {40, 60});  // 30 mm
    track(n1, {10, 70}, {30, 70});  // 20 mm: 10 mm short
    track(s.netOf({dp, 0}), {10, 20}, {40, 20});
    track(s.netOf({dn, 0}), {10, 22}, {39, 22});  // 1 mm skew, a neighbour 2 mm away
    auto matched = [](const Json& r, size_t i) { return r.get("groups")[i].get("matched").asBool(false); };
    Json before = lengthReportJson(p.pcb, s);
    CHECK(!matched(before, 0) && !matched(before, 1));

    CHECK(tuneLengths(p.pcb, s) == 2);
    CHECK_NEAR(routedNetLength(p.pcb, n1), 30, 1e-6);
    CHECK_NEAR(routedNetLength(p.pcb, s.netOf({dn, 0})), 30, 1e-6);
    Json after = lengthReportJson(p.pcb, s);
    CHECK(after.get("groups").size() == 2 && matched(after, 0) && matched(after, 1));
    // The serpentine keeps the end points (connectivity) and clearance to the neighbouring pair member.
    for (const auto& t : p.pcb.tracks)
        if (t.net == s.netOf({dn, 0}))
            CHECK(segmentSegmentDistance(t.a, t.b, {10, 20}, {40, 20}) - t.width >= p.pcb.settings.clearance - 1e-6);
    CHECK(tuneLengths(p.pcb, s) == 0);  // already matched

    // Persistence.
    p.pcb.settings.pairSkewTolerance = 0.05;
    p.pcb.settings.lengthTuning = false;
    Project q = Project::fromJson(p.toJson());
    CHECK_NEAR(q.pcb.settings.pairSkewTolerance, 0.05, 1e-9);
    CHECK(!q.pcb.settings.lengthTuning);
}

TEST(hdi_blind_buried_microvias) {
    // Via kinds on a 6-layer stack.
    Via v;
    CHECK(std::string(viaKind(v, 6)) == "through" && v.isThrough());
    v.fromLayer = 0;
    v.toLayer = 1;
    v.drill = 0.1;
    CHECK(std::string(viaKind(v, 6)) == "microvia" && v.spans(1) && !v.spans(2));
    v.toLayer = 2;
    v.drill = 0.2;
    CHECK(std::string(viaKind(v, 6)) == "blind");
    v.fromLayer = 2;
    v.toLayer = 3;
    CHECK(std::string(viaKind(v, 6)) == "buried");

    // HDI stack-up: thin laser build-up dielectrics under the outer layers.
    BoardSettings bs;
    bs.layerCount = 6;
    bs.thickness = 1.6;
    const double plain = dielectricBelow(bs, 0);
    bs.hdi = true;
    CHECK(dielectricBelow(bs, 0) < 0.1 && dielectricBelow(bs, 4) < 0.1 && dielectricBelow(bs, 2) > plain);
    CHECK(widthForImpedance(bs, 0, 50) < 0.3);  // thin build-up → narrow 50 Ω microstrip
    Via mv;
    mv.toLayer = 1;
    mv.drill = 0.1;
    CHECK((viaBarrelDepth(bs, mv) - 2 * copperThickness(bs)) / mv.drill <= 1.0);  // laser-drillable dielectric

    // Auto Route on a 4-layer HDI board: vias are cut to the layers they join, connectivity and DRC stay intact.
    Project p = amplifierProject();
    p.pcb.settings.layerCount = 4;
    p.pcb.settings.hdi = true;
    p.pcb.autoPlace(p.schematic, true);
    RouteStats st = p.pcb.autoRoute(p.schematic);
    CHECK(st.failed == 0);
    CHECK(p.pcb.ratsnest(p.schematic).empty());
    int errors = 0;
    for (const auto& e : p.pcb.runDRC(p.schematic)) errors += e.severity == Severity::Error;
    CHECK(errors == 0);
    for (const auto& via : p.pcb.vias) CHECK(via.fromLayer >= 0 && via.lastLayer(4) > via.fromLayer);

    // A hand-placed microvia from the top to layer 2 joining two tracks: own drill file, only on its layers.
    Project q = amplifierProject();
    q.pcb.settings.layerCount = 4;
    q.pcb.settings.hdi = true;
    int net = q.schematic.netOf({q.schematic.components().front().id, 0});
    Track a;
    a.net = net;
    a.layer = 0;
    a.a = {5, 5};
    a.b = {10, 5};
    Track b = a;
    b.layer = 1;
    b.a = {10, 5};
    b.b = {10, 12};
    q.pcb.addTrack(a);
    q.pcb.addTrack(b);
    Via through;
    through.net = net;
    through.position = {10, 5};
    q.pcb.addVia(through);
    CHECK(q.pcb.applyHdiVias(q.schematic) == 1);
    const Via& cut = q.pcb.vias.front();
    CHECK(std::string(viaKind(cut, 4)) == "microvia");
    CHECK_NEAR(cut.drill, 0.1, 1e-9);
    auto spans = viaSpans(q.pcb);
    CHECK(spans.size() == 1 && spans[0].first == 0 && spans[0].second == 1);
    CHECK(exportExcellonSpan(q.pcb, 0, 1).find("Plated,1,2,Blind") != std::string::npos);
    CHECK(exportExcellonDrill(q.schematic, q.pcb, true).find("X10.000") == std::string::npos);  // not in the PTH file
    CHECK(exportCopperGerber(q.schematic, q.pcb, 0).find("X10000000Y") != std::string::npos ||
          exportCopperGerber(q.schematic, q.pcb, 0).find("D03") != std::string::npos);
    // The bottom layer has no copper of this via.
    const std::string bottom = exportCopperGerber(q.schematic, q.pcb, 3);
    CHECK(bottom.find("D03") == std::string::npos);
    // Persistence of the span (the hand-drawn copper is not on a pad, so check the saved document itself).
    const std::string saved = q.toJson().dump();
    CHECK(saved.find("\"toLayer\":1") != std::string::npos && saved.find("\"hdi\":true") != std::string::npos);
    CHECK(Project::fromJson(q.toJson()).pcb.settings.hdi);
    // Fabrication package lists the span drill file and HDI notes.
    bool spanFile = false, notes = false;
    for (const auto& f : fabricationPackage(q)) {
        spanFile |= f.path.find("-L1-L2.drl") != std::string::npos;
        notes |= f.content.find("HDI (IPC-2226)") != std::string::npos;
    }
    CHECK(spanFile && notes);
}

TEST(locked_footprints_survive_auto_place) {
    Project p = amplifierProject();
    p.pcb.autoPlace(p.schematic, true);
    Component* first = nullptr;
    for (auto& c : p.schematic.mutableComponents())
        if (c.hasFootprint()) { first = &c; break; }
    CHECK(first);
    first->pcb.position = {7.5, 9.0};
    first->pcb.locked = true;
    const int id = first->id;
    p.pcb.autoPlace(p.schematic, true);
    const Component* again = p.schematic.find(id);
    CHECK(again && again->pcb.placed && again->pcb.locked);
    CHECK_NEAR(again->pcb.position.x, 7.5, 1e-9);
    CHECK_NEAR(again->pcb.position.y, 9.0, 1e-9);
    Project q = Project::fromJson(p.toJson());
    CHECK(q.schematic.find(id)->pcb.locked);
}

TEST(embedded_passives) {
    Project p = amplifierProject();
    p.pcb.settings.layerCount = 4;
    Component* r330 = nullptr;
    Component* cap = nullptr;
    for (auto& c : p.schematic.mutableComponents()) {
        if (c.kind == ComponentKind::Resistor && c.value == "330") r330 = &c;
        if (c.kind == ComponentKind::Capacitor) cap = &c;
    }
    CHECK(r330 && cap);
    const int rid = r330->id, cid = cap->id;
    r330->pcb.embeddedLayer = 1;
    p.pcb.autoPlace(p.schematic, true);

    // 330 Ω on 100 Ω/sq foil: 3.3 squares, 0.5 mm wide.
    auto e = embeddedElement(*p.schematic.find(rid), p.pcb.settings);
    CHECK(e && e->resistor && e->layer == 1 && e->inRange);
    CHECK(e && e->sheet == 100 && std::fabs(e->squares - 3.3) < 1e-9 && std::fabs(e->length - 1.65) < 1e-9);
    int onInner = 0;
    for (const auto& pd : p.pcb.pads(p.schematic))
        if (pd.componentId == rid) onInner += pd.smdLayer == 1 && !pd.throughHole;
    CHECK(onInner == 2);

    // Routes and passes DRC with the part inside the board.
    CHECK(p.pcb.autoRoute(p.schematic).failed == 0);
    CHECK(p.pcb.ratsnest(p.schematic).empty());
    int errors = 0;
    for (const auto& v : p.pcb.runDRC(p.schematic)) errors += v.severity == Severity::Error;
    CHECK(errors == 0);

    // Not assembled: off the CPL and the assembly BOM; listed as embedded in the BOM.
    CHECK(exportCplCsv(p.schematic, p.pcb).find(r330->ref + ",") == std::string::npos);
    CHECK(exportAssemblyBomCsv(p.schematic).find(r330->ref) == std::string::npos);
    bool bomEmbedded = false;
    for (const auto& l : buildBom(p.schematic)) bomEmbedded |= l.embedded && l.refs.front() == r330->ref;
    CHECK(bomEmbedded);
    // Fabrication: resistor element artwork and notes.
    bool artwork = false, notes = false;
    for (const auto& f : fabricationPackage(p)) {
        artwork |= f.path.find("-In1_Resistor.gbr") != std::string::npos && f.content.find("G36") != std::string::npos;
        notes |= f.content.find("EMBEDDED PASSIVES") != std::string::npos;
    }
    CHECK(artwork && notes);
    auto codes = [&] {
        std::set<std::string> out;
        for (const auto& x : reliabilityChecks(p)) out.insert(x.code);
        return out;
    };
    CHECK(codes().count("REL_EMBEDDED_PASSIVES"));

    // 100 nF is far too large for a buried plate pair; a 2-layer board cannot hold embedded parts at all.
    p.schematic.find(cid)->pcb.embeddedLayer = 1;
    auto ce = embeddedElement(*p.schematic.find(cid), p.pcb.settings);
    CHECK(ce && !ce->resistor && ce->layer2 == 2 && !ce->inRange);
    CHECK(codes().count("REL_EMBEDDED_RANGE"));
    p.schematic.find(rid)->value = "330 1%";
    CHECK(codes().count("REL_EMBEDDED_TOLERANCE"));
    Project saved = Project::fromJson(p.toJson());
    CHECK(saved.schematic.find(rid)->pcb.embeddedLayer == 1);
    p.pcb.settings.layerCount = 2;
    CHECK(!embeddedElement(*p.schematic.find(rid), p.pcb.settings));
    CHECK(codes().count("REL_EMBEDDED_STACKUP"));
}

TEST(high_layer_count_and_computing_segments) {
    // Even stack-ups up to 24 layers (servers, mainframes, GPU baseboards).
    CHECK(BoardSettings::normalizeLayerCount(1) == 1 && BoardSettings::normalizeLayerCount(3) == 4);
    CHECK(BoardSettings::normalizeLayerCount(8) == 8 && BoardSettings::normalizeLayerCount(13) == 14);
    CHECK(BoardSettings::normalizeLayerCount(40) == BoardSettings::kMaxLayers);
    Project p = amplifierProject();
    p.pcb.settings.layerCount = 12;
    p.pcb.zones.push_back({"GND", 1, true, 0});
    p.pcb.autoPlace(p.schematic, true);
    CHECK(p.pcb.autoRoute(p.schematic).failed == 0);
    CHECK(p.pcb.ratsnest(p.schematic).empty());
    Json st = stackupJson(p.pcb.settings);
    CHECK(st["layers"].size() == 23);  // 12 copper + 11 dielectrics
    Project q = Project::fromJson(p.toJson());
    CHECK(q.pcb.settings.layerCount == 12);

    // Computing segments: profiles, materials and fabrication requirements.
    for (const char* id : {"motherboard", "server", "hpc", "arm", "addin"}) {
        const IndustryProfile* prof = findIndustry(id);
        CHECK(prof && !prof->guidance.empty() && !prof->standards.empty());
        CHECK(prof && std::any_of(designRulePresets().begin(), designRulePresets().end(),
                                  [&](const DesignRulePreset& r) { return r.name == prof->rulePreset; }));
        Project seg = amplifierProject();
        seg.industry = id;
        FabricationRequirements req = fabricationRequirements(seg);
        CHECK(!req.finish.empty());
        CHECK(std::any_of(req.notes.begin(), req.notes.end(),
                          [](const std::string& n) { return n.find("Controlled impedance") != std::string::npos; }));
    }
    Project server = amplifierProject();
    server.industry = "server";
    CHECK(fabricationRequirements(server).ipcClass == 3);
    Project card = amplifierProject();
    card.industry = "addin";
    CHECK(fabricationRequirements(card).finish.find("hard") != std::string::npos);
    CHECK(findLaminate("megtron-7") && findLaminate("tachyon-100g"));
    CHECK(findLaminate("tachyon-100g")->er < findLaminate("megtron-6")->er);
}

TEST(symbol_editor_auto_arrange_rules_geometry_and_board) {
    using P = PinType;
    auto spot = [](const SymbolSpec& sym, const std::string& number) -> const SymbolPin* {
        for (const auto& p : sym.pins)
            if (p.number == number) return &p;
        return nullptr;
    };
    auto side = [&](const SymbolSpec& sym, const std::string& number) { return spot(sym, number) ? spot(sym, number)->side : '?'; };
    // A part exercising every Auto Arrange rule.
    CustomPartSpec mcu;
    mcu.name = "ARRANGE-TEST";
    const std::vector<std::tuple<const char*, P>> pins = {
        {"VDD", P::PowerIn}, {"AVDD", P::PowerIn}, {"VDD", P::PowerIn}, {"GND", P::PowerIn}, {"AGND", P::Passive},
        {"PGND", P::PowerIn}, {"VSS", P::PowerIn}, {"VEE", P::PowerIn}, {"V-", P::PowerIn}, {"0V", P::Passive},
        {"NRST", P::Input}, {"XTAL1", P::Passive}, {"SWDIO", P::Bidirectional}, {"BOOT0", P::Input},
        {"IN_A", P::Input}, {"OUT_A", P::Output}, {"nFAULT", P::OpenCollector}, {"VOUT", P::PowerOut},
        {"PA10", P::Bidirectional}, {"PA9", P::Bidirectional}, {"PA0", P::Bidirectional}, {"P1.3", P::Bidirectional},
        {"P1_2", P::Bidirectional}, {"GPIO12", P::Bidirectional}, {"IO5", P::Bidirectional}, {"RB7", P::Bidirectional},
        {"PA5/ADC1_5", P::Bidirectional}, {"NC", P::NoConnect}, {"NC", P::NoConnect}, {"CAP", P::Passive}, {"FB", P::Passive}};
    for (size_t i = 0; i < pins.size(); ++i)
        mcu.pins.push_back({std::to_string(i + 1), std::get<0>(pins[i]), std::get<1>(pins[i]), ""});
    const SymbolSpec sym = autoArrangeSymbol(mcu);
    CHECK(sym.pins.size() == mcu.pins.size());
    // Supplies on top (repeated ones stacked), grounds and negative supplies at the bottom.
    CHECK(side(sym, "1") == 'T' && side(sym, "2") == 'T' && side(sym, "3") == 'T');
    CHECK(spot(sym, "1")->slot == spot(sym, "3")->slot && spot(sym, "1")->slot != spot(sym, "2")->slot);
    for (const char* g : {"4", "5", "6", "7", "8", "9", "10"}) CHECK(side(sym, g) == 'B');
    // Control pins first on the left, then inputs; outputs (open-drain, regulator outputs too) on the right.
    for (const char* c : {"11", "12", "13", "14"}) CHECK(side(sym, c) == 'L' && spot(sym, c)->slot < 4);
    CHECK(side(sym, "15") == 'L' && spot(sym, "15")->slot > spot(sym, "14")->slot + 1);  // a gap before inputs
    CHECK(side(sym, "16") == 'R' && side(sym, "17") == 'R' && side(sym, "18") == 'R');
    // Ports: PA in bit order (PA0, PA5, PA9, PA10 — not lexical), each port one group on one side.
    CHECK(side(sym, "21") == side(sym, "27") && side(sym, "27") == side(sym, "20") && side(sym, "20") == side(sym, "19"));
    CHECK(spot(sym, "21")->slot < spot(sym, "27")->slot && spot(sym, "27")->slot < spot(sym, "20")->slot &&
          spot(sym, "20")->slot < spot(sym, "19")->slot);
    CHECK(side(sym, "22") == side(sym, "23") && spot(sym, "23")->slot + 1 == spot(sym, "22")->slot);  // P1_2, P1.3
    for (const char* port : {"24", "25", "26"}) CHECK(side(sym, port) == 'L' || side(sym, port) == 'R');
    // No-connect pins last on the right; plain passives on the sides.
    int lastRight = -1;
    for (const auto& p : sym.pins)
        if (p.side == 'R') lastRight = std::max(lastRight, p.slot);
    CHECK(side(sym, "28") == 'R' && side(sym, "29") == 'R' && spot(sym, "29")->slot == lastRight);
    CHECK((side(sym, "30") == 'L' || side(sym, "30") == 'R') && (side(sym, "31") == 'L' || side(sym, "31") == 'R'));
    // Without stacking every pin has its own spot; the result always passes the checks.
    const SymbolSpec flat = autoArrangeSymbol(mcu, false);
    CHECK(spot(flat, "1")->slot != spot(flat, "3")->slot);
    CustomPartSpec checked = mcu;
    checked.symbol = sym;
    for (const auto& issue : checkSymbol(checked)) CHECK(issue.severity == "info");
    checked.symbol = flat;
    CHECK(checkSymbol(checked).empty());
    // A long port is cut into groups of 8 balanced over both sides.
    CustomPartSpec wide;
    wide.name = "PORT-TEST";
    for (int b = 0; b < 24; ++b) wide.pins.push_back({std::to_string(b + 1), "PB" + std::to_string(b), P::Bidirectional, ""});
    const SymbolSpec banks = autoArrangeSymbol(wide);
    int left = 0, right = 0;
    for (const auto& p : banks.pins) (p.side == 'L' ? left : right) += 1;
    CHECK(left == 16 && right == 8);
    CHECK(side(banks, "1") != side(banks, "9"));  // PB0–7 and PB8–15 on different sides

    // Geometry: the generated box equals its explicit layout; long top names make room above the side pins; a
    // requested width below the automatic one is ignored; the body is never narrower than 30.
    CustomPartSpec chip;
    chip.name = "GEO-TEST";
    chip.pins = {{"1", "A", P::Input}, {"2", "B", P::Input}, {"3", "C", P::Output}, {"4", "D", P::Output}};
    auto generated = CustomPartRegistry::instance().registerPart(chip);
    CustomPartSpec explicitBox = chip;
    explicitBox.symbol.pins = {{"1", 'L', 0}, {"2", 'L', 1}, {"3", 'R', 1}, {"4", 'R', 0}};
    auto same = CustomPartRegistry::instance().registerPart(explicitBox);
    CHECK(same->symbolHalfWidth == generated->symbolHalfWidth && same->symbolHalfHeight == generated->symbolHalfHeight);
    for (size_t i = 0; i < 4; ++i)
        CHECK(same->def.pins[i].offset.x == generated->def.pins[i].offset.x && same->def.pins[i].offset.y == generated->def.pins[i].offset.y);
    CHECK(generated->symbolHalfWidth >= 30);
    CustomPartSpec tall = explicitBox;
    tall.pins.push_back({"5", "VERY_LONG_SUPPLY", P::PowerIn, ""});
    tall.symbol.pins.push_back({"5", 'T', 0});
    auto tallPart = CustomPartRegistry::instance().registerPart(tall);
    CHECK(tallPart->symbolHalfHeight > generated->symbolHalfHeight + 40);
    CHECK(tallPart->def.pins[4].offset.x == 0 && tallPart->def.pins[4].offset.y == -(tallPart->symbolHalfHeight + 20));
    CustomPartSpec narrow = explicitBox;
    narrow.symbol.width = 20;
    CHECK(CustomPartRegistry::instance().registerPart(narrow)->symbolHalfWidth == generated->symbolHalfWidth);

    // Stacked pins on a rotated component stay joined and move together.
    CustomPartSpec stacked = chip;
    stacked.pins.push_back({"5", "GND", P::PowerIn, ""});
    stacked.pins.push_back({"6", "GND", P::PowerIn, ""});
    stacked.symbol.pins = {{"1", 'L', 0}, {"2", 'L', 1}, {"3", 'R', 0}, {"4", 'R', 1}, {"5", 'B', 0}, {"6", 'B', 0}};
    Project p;
    auto& sch = p.schematic;
    const std::string id = p.addCustomPart(stacked);
    int u = sch.addCustomComponent(id, "", {300, 0}, 90);
    const Vec2 a = sch.pinPosition({u, sch.pinIndex(u, "5")}), b = sch.pinPosition({u, sch.pinIndex(u, "6")});
    CHECK(a.x == b.x && a.y == b.y);
    CHECK(sch.netOf({u, sch.pinIndex(u, "5")}) == sch.netOf({u, sch.pinIndex(u, "6")}));
    // Wired up and laid out: both GND pads are on the ground net and the router joins them.
    int v = sch.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    int r = sch.addComponent(ComponentKind::Resistor, "1k", {120, -80});
    int g = sch.addComponent(ComponentKind::Ground, "", {0, 120});
    sch.connect({v, 1}, {g, 0});
    sch.connect({v, 0}, {r, 0});
    sch.connect({r, 1}, {u, sch.pinIndex(u, "1")});
    sch.connect({u, sch.pinIndex(u, "5")}, {g, 0});
    p.pcb.autoPlace(sch, true);
    CHECK(p.pcb.autoRoute(sch).failed == 0);
    int groundPads = 0;
    const int gnd = sch.netOf({g, 0});
    for (const auto& pad : p.pcb.pads(sch))
        if (pad.componentId == u && pad.net == gnd) ++groundPads;
    CHECK(groundPads == 2);
    // The layout is saved with the project and loads back to the same symbol and nets.
    Project back = Project::fromJson(Json::parse(p.toJson().dump()));
    const Component* restored = back.schematic.find(u);
    CHECK(restored && back.schematic.netOf({u, back.schematic.pinIndex(u, "6")}) == back.schematic.netOf({g, 0}));
    const CustomPart* restoredPart = CustomPartRegistry::instance().find(restored->customPart);
    CHECK(restoredPart && restoredPart->spec.symbol.pins.size() == 6);
    // Swapping in a re-arranged symbol keeps the wires on their pins.
    CustomPartSpec moved = stacked;
    moved.symbol.pins[4].side = 'T';
    moved.symbol.pins[5].side = 'T';
    const std::string movedId = p.addCustomPart(moved);
    CHECK(sch.replaceCustomPart(id, movedId) == 1);
    CHECK(sch.netOf({u, sch.pinIndex(u, "6")}) == sch.netOf({g, 0}));
    CHECK(sch.pinPosition({u, sch.pinIndex(u, "5")}).x != a.x || sch.pinPosition({u, sch.pinIndex(u, "5")}).y != a.y);
}

TEST(symbol_editor_layout_auto_arrange_and_checks) {
    auto reg = [](const CustomPartSpec& spec) { return CustomPartRegistry::instance().registerPart(spec); };
    auto pinAt = [](const std::shared_ptr<const CustomPart>& part, const std::string& number) -> const PinDef* {
        for (const auto& p : part->def.pins)
            if (p.number == number) return &p;
        return nullptr;
    };
    auto codes = [](const std::vector<SymbolIssue>& v) {
        std::multiset<std::string> out;
        for (const auto& i : v) out.insert(i.code);
        return out;
    };

    // Small parts keep the generated datasheet-order box: NE555 pins 1–4 down the left, 5–8 up the right.
    const CustomPartSpec ne555 = findStandardPart("NE555")->spec;
    CHECK(ne555.symbol.empty());
    auto timer = reg(ne555);
    CHECK(pinAt(timer, "1")->side == 'L' && pinAt(timer, "8")->side == 'R');
    CHECK(pinAt(timer, "1")->offset.y < pinAt(timer, "4")->offset.y);
    CHECK(pinAt(timer, "8")->offset.y == pinAt(timer, "1")->offset.y);

    // Large library parts are auto-arranged: supplies on top (repeated ones stacked), grounds below, control pins top left.
    for (const auto& sp : standardParts()) {
        if (sp.spec.pins.size() < 16) continue;
        CHECK(!sp.spec.symbol.empty());
        bool clean = true;
        for (const auto& issue : checkSymbol(sp.spec)) clean &= issue.severity == "info";
        if (!clean) std::printf("    %s: symbol issues\n", sp.spec.name.c_str());
        CHECK(clean);
        auto part = reg(sp.spec);
        bool onGrid = true;
        for (const auto& p : part->def.pins)
            onGrid &= std::fmod(std::fabs(p.offset.x), 10.0) < 1e-9 && std::fmod(std::fabs(p.offset.y), 10.0) < 1e-9;
        CHECK(onGrid);
    }
    auto stm = reg(findStandardPart("STM32F405RGT6")->spec);
    std::set<std::pair<double, double>> vdd;
    for (const auto& p : stm->def.pins)
        if (p.name == "VDD") vdd.insert({p.offset.x, p.offset.y});
    CHECK(vdd.size() == 1);  // four VDD pins, one stacked pin
    CHECK(pinAt(stm, "19")->name == "VDD" && pinAt(stm, "19")->side == 'T');
    CHECK(pinAt(stm, "19")->offset.y == -(stm->symbolHalfHeight + 20));
    const PinDef* vss = nullptr;
    for (const auto& p : stm->def.pins)
        if (p.name == "VSS") vss = &p;
    CHECK(vss && vss->side == 'B' && vss->offset.y == stm->symbolHalfHeight + 20);
    const PinDef* nrst = nullptr;
    for (const auto& p : stm->def.pins)
        if (p.name == "NRST") nrst = &p;
    double topLeft = 1e9;  // NRST is the first pin of the left side (reset / boot / debug group)
    for (const auto& p : stm->def.pins)
        if (p.side == 'L') topLeft = std::min(topLeft, p.offset.y);
    CHECK(nrst && nrst->side == 'L' && nrst->offset.y == topLeft);
    // Port pins in bit order, one slot apart, within a group of 8.
    auto y = [&](const char* name) {
        for (const auto& p : stm->def.pins)
            if (p.name == name) return p.offset.y;
        return 1e9;
    };
    CHECK(y("PA1") - y("PA0") == 20 && y("PA7") - y("PA0") == 140);

    // A hand-made layout on all four sides.
    CustomPartSpec chip;
    chip.name = "SYM-TEST";
    chip.pins = {{"1", "VCC", PinType::PowerIn, ""}, {"2", "IN", PinType::Input, ""}, {"3", "OUT", PinType::Output, ""},
                 {"4", "GND", PinType::PowerIn, ""}, {"5", "GND", PinType::PowerIn, ""}, {"6", "EN", PinType::Input, ""}};
    chip.symbol.pins = {{"1", 'T', 0}, {"2", 'L', 0}, {"6", 'L', 2}, {"3", 'R', 1}, {"4", 'B', 0}, {"5", 'B', 0}};
    CHECK(codes(checkSymbol(chip)) == std::multiset<std::string>({"SYM_STACK"}));
    auto c = reg(chip);
    CHECK(pinAt(c, "1")->offset.x == 0 && pinAt(c, "1")->offset.y == -(c->symbolHalfHeight + 20));
    CHECK(pinAt(c, "4")->offset.x == pinAt(c, "5")->offset.x && pinAt(c, "4")->offset.y == pinAt(c, "5")->offset.y);
    CHECK(pinAt(c, "6")->offset.y - pinAt(c, "2")->offset.y == 40);  // slot 2 leaves a gap
    CHECK(pinAt(c, "2")->offset.x == -(c->symbolHalfWidth + 20) && pinAt(c, "3")->offset.x == c->symbolHalfWidth + 20);
    // The body widens for a requested width and for a row of top pins.
    CustomPartSpec wide = chip;
    wide.symbol.width = 200;
    CHECK(reg(wide)->symbolHalfWidth == 100);
    CustomPartSpec row = chip;
    for (int k = 0; k < 12; ++k) {
        row.pins.push_back({std::to_string(7 + k), "D" + std::to_string(k), PinType::Bidirectional, ""});
        row.symbol.pins.push_back({std::to_string(7 + k), 'T', 1 + k});
    }
    auto rowPart = reg(row);
    CHECK(rowPart->symbolHalfWidth >= 13 * 10 + 10);
    double minX = 1e9, maxX = -1e9;
    for (const auto& p : rowPart->def.pins)
        if (p.side == 'T') {
            minX = std::min(minX, p.offset.x);
            maxX = std::max(maxX, p.offset.x);
        }
    CHECK(minX == -maxX && maxX < rowPart->symbolHalfWidth);

    // Checks: missing, unknown, duplicate, overlap, stacked signal pins; errors refuse registration.
    CustomPartSpec bad = chip;
    bad.symbol.pins.pop_back();  // pin 5 missing
    CHECK(codes(checkSymbol(bad)).count("SYM_MISSING") == 1);
    bad.symbol.pins.push_back({"9", 'R', 5});
    CHECK(codes(checkSymbol(bad)).count("SYM_UNKNOWN") == 1);
    bad = chip;
    bad.symbol.pins.push_back({"2", 'R', 6});
    CHECK(codes(checkSymbol(bad)).count("SYM_DUPLICATE") == 1);
    bad = chip;
    bad.symbol.pins[2] = {"6", 'L', 0};  // EN on top of IN
    CHECK(codes(checkSymbol(bad)).count("SYM_OVERLAP") == 1);
    bool threw = false;
    try {
        reg(bad);
    } catch (const JsonError&) {
        threw = true;
    }
    CHECK(threw);
    CustomPartSpec sig = chip;
    sig.pins.push_back({"7", "IN", PinType::Input, ""});
    sig.symbol.pins.push_back({"7", 'L', 0});
    CHECK(codes(checkSymbol(sig)).count("SYM_STACK_SIGNAL") == 1);

    // JSON: the layout round-trips, changes the part id, and is validated.
    Json j = customPartSpecToJson(chip);
    CHECK(j.get("symbolLayout").get("pins").size() == 6);
    CustomPartSpec back = customPartSpecFromJson(j);
    CHECK(back.symbol.pins.size() == 6 && back.symbol.pins[0].side == 'T' && back.symbol.pins[2].slot == 2);
    CHECK(reg(back)->id == c->id);
    CustomPartSpec plain = chip;
    plain.symbol = {};
    CHECK(reg(plain)->id != c->id);
    CHECK(customPartSpecToJson(plain).get("symbolLayout").isNull());  // generated symbols keep their old ids
    for (const char* badSym : {R"({"pins":[{"number":"1","side":"X","slot":0}]})", R"({"pins":[{"number":"1","side":"L","slot":-1}]})",
                               R"({"width":900,"pins":[]})", R"({"pins":[{"side":"L","slot":0}]})"}) {
        Json bj = customPartSpecToJson(chip);
        bj["symbolLayout"] = Json::parse(badSym);
        threw = false;
        try {
            customPartSpecFromJson(bj);
        } catch (const JsonError&) {
            threw = true;
        }
        CHECK(threw);
    }

    // Stacked pins are one node: wiring one GND pin connects both, and the board must join both pads.
    Project p;
    auto& sch = p.schematic;
    const std::string id = p.addCustomPart(chip);
    int u = sch.addCustomComponent(id, "", {200, 0});
    int v = sch.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    int g = sch.addComponent(ComponentKind::Ground, "", {0, 100});
    sch.connect({v, 0}, {u, sch.pinIndex(u, "1")});
    sch.connect({v, 1}, {g, 0});
    sch.connect({u, sch.pinIndex(u, "4")}, {g, 0});
    CHECK(sch.netOf({u, sch.pinIndex(u, "5")}) == sch.netOf({u, sch.pinIndex(u, "4")}));
    CHECK(sch.netOf({u, sch.pinIndex(u, "5")}) == sch.netOf({g, 0}));
    CHECK(sch.netOf({u, sch.pinIndex(u, "2")}) != sch.netOf({u, sch.pinIndex(u, "6")}));  // the gap joins nothing
    CHECK(sch.isPinConnected({u, sch.pinIndex(u, "5")}));
    // A stack with no wire is still open: ERC reports the floating ground pins.
    int u2 = sch.addCustomComponent(id, "", {400, 0});
    CHECK(sch.netOf({u2, sch.pinIndex(u2, "4")}) == sch.netOf({u2, sch.pinIndex(u2, "5")}));
    CHECK(!sch.isPinConnected({u2, sch.pinIndex(u2, "4")}) && !sch.isPinConnected({u2, sch.pinIndex(u2, "5")}));
    bool groundOpen = false;
    for (const auto& v : sch.runERC())
        groundOpen |= v.code == "ERC_FLOATING_COMPONENT" && !v.components.empty() && v.components[0] == u2;
    CHECK(groundOpen);

    // C API: auto-arrange and check.
    const std::string specJson = customPartSpecToJson(findStandardPart("CH340G")->spec).dump();
    char* error = nullptr;
    char* arranged = sieda_symbol_auto_arrange(specJson.c_str(), 1, &error);
    CHECK(arranged && !error);
    if (arranged) {
        Json aj = Json::parse(arranged);
        CHECK(aj.get("symbolLayout").get("pins").size() == 16);
        char* issues = sieda_check_symbol(arranged);
        CHECK(issues && Json::parse(issues).size() == 0);
        sieda_string_free(issues);
        sieda_string_free(arranged);
    }
    CHECK(sieda_symbol_auto_arrange(R"({"name":"X","pins":[]})", 1, &error) == nullptr && error);
    sieda_string_free(error);
    Json broken = customPartSpecToJson(chip);
    broken["symbolLayout"] = Json::parse(R"({"pins":[{"number":"1","side":"Q","slot":0}]})");
    char* invalid = sieda_check_symbol(broken.dump().c_str());
    CHECK(Json::parse(invalid).items().front().get("code").asString("") == "SYM_INVALID");
    sieda_string_free(invalid);
}

TEST(footprint_editor_checks_geometry_and_c_api) {
    // A two-pin part whose land pattern each case rewrites.
    CustomPartSpec two;
    two.name = "FP-EDGE";
    two.package.type = "CUSTOM";
    two.pins = {{"1", "A", PinType::Passive, ""}, {"2", "B", PinType::Passive, ""}};
    auto with = [&](std::vector<PackageSpec::Land> lands) {
        CustomPartSpec s = two;
        s.package.lands = std::move(lands);
        return s;
    };
    auto codes = [](const std::vector<LandIssue>& v) {
        std::multiset<std::string> out;
        for (const auto& i : v) out.insert(i.code);
        return out;
    };
    using L = PackageSpec::Land;
    // Circles: the gap is centre distance minus the radii (not the bounding boxes).
    CHECK(checkLandPattern(with({L(0, 0, 1, 1, 0, true), L(1.2, 0, 1, 1, 0, true)})).empty());
    CHECK(codes(checkLandPattern(with({L(0, 0, 1, 1, 0, true), L(1.05, 0, 1, 1, 0, true)}))).count("LAND_GAP") == 1);
    CHECK(codes(checkLandPattern(with({L(0, 0, 1, 1, 0, true), L(0.9, 0, 1, 1, 0, true)}))).count("LAND_OVERLAP") == 1);
    // Diagonal neighbours: corner-to-corner distance, 0.05 mm on each axis → 0.0707 mm.
    auto diag = checkLandPattern(with({L(0, 0, 1, 1), L(1.05, 1.05, 1, 1)}));
    CHECK(diag.size() == 1 && diag[0].code == "LAND_GAP" && diag[0].message.find("0.071") != std::string::npos);
    CHECK(diag[0].pads == std::vector<int>({1, 2}));
    // Touching edges are a zero gap, not an overlap; the minimum gap is configurable.
    CHECK(codes(checkLandPattern(with({L(0, 0, 1, 1), L(1, 0, 1, 1)}))).count("LAND_GAP") == 1);
    CHECK(checkLandPattern(with({L(0, 0, 1, 1), L(1.15, 0, 1, 1)}), 0.1).empty());
    CHECK(codes(checkLandPattern(with({L(0, 0, 1, 1), L(1.15, 0, 1, 1)}), 0.2)).count("LAND_GAP") == 1);
    // Two pads of one pin may overlap (a split exposed pad); two mechanical pads may not.
    CHECK(checkLandPattern(with({L(0, 0, 1, 1), L(3, 0, 1, 1), L(0.5, 0, 1, 1, 0, false, "1")})).empty());
    CHECK(codes(checkLandPattern(with({L(0, 0, 1, 1), L(3, 0, 1, 1), L(6, 0, 2, 2, 1, true, "-"),
                                        L(6.5, 0, 2, 2, 1, true, "-")}))).count("LAND_OVERLAP") == 1);
    // Pin references are case-insensitive ("ep" is pin "EP").
    CustomPartSpec ep = two;
    ep.pins.push_back({"EP", "GND", PinType::PowerIn, ""});
    ep.package.lands = {L(-2, 0, 1, 1), L(2, 0, 1, 1), L(0, 0, 2, 2, 0, false, "ep")};
    CHECK(checkLandPattern(ep).empty());
    auto epPart = CustomPartRegistry::instance().registerPart(ep);
    CHECK(epPart->def.pins[static_cast<size_t>(epPart->footprint.pads[2].pinIndex)].number == "EP");
    // The pin reference survives JSON ([x, y, w, h, drill, round, pin]); "custom" normalises to CUSTOM.
    Json j = customPartSpecToJson(ep);
    CHECK(j.get("package").get("lands")[2].size() == 7);
    j["package"]["type"] = "custom";
    CustomPartSpec back = customPartSpecFromJson(j);
    CHECK(back.package.type == "CUSTOM" && back.package.lands[2].pin == "ep");
    CHECK(CustomPartRegistry::instance().registerPart(back)->footprint.pads.size() == 3);
    // A BGA converts with its ball names as pad pins: every ball keeps its pin.
    const CustomPartSpec bga = landPatternFromFootprint(findStandardPart("XC7A35T-1CSG324I")->spec);
    CHECK(bga.package.lands.size() == 324 && bga.package.lands[0].pin == "A1");
    CHECK(checkLandPattern(bga).empty());

    // C API: convert, check, and the error paths.
    const std::string ne555 = customPartSpecToJson(findStandardPart("NE555")->spec).dump();
    char* error = nullptr;
    char* converted = sieda_custom_part_land_pattern(ne555.c_str(), &error);
    CHECK(converted != nullptr && error == nullptr);
    if (converted) {
        Json cj = Json::parse(converted);
        CHECK(cj.get("package").get("type").asString("") == "CUSTOM" && cj.get("package").get("lands").size() == 8);
        char* issues = sieda_check_land_pattern(converted, 0.1);
        CHECK(issues && std::string(issues) == "[]");
        sieda_string_free(issues);
        sieda_string_free(converted);
    }
    CHECK(sieda_custom_part_land_pattern("{not json", &error) == nullptr && error != nullptr);
    sieda_string_free(error);
    char* invalid = sieda_check_land_pattern(R"({"name":"X","package":{"type":"CUSTOM","lands":[[0,0,1,1,2,1]]},"pins":[]})", 0.1);
    Json ij = Json::parse(invalid);
    CHECK(ij.size() == 1 && ij.items().front().get("code").asString("") == "LAND_INVALID");
    sieda_string_free(invalid);
    char* overlap = sieda_check_land_pattern(customPartSpecToJson(with({L(0, 0, 1, 1), L(0.5, 0, 1, 1)})).dump().c_str(), 0);
    Json oj = Json::parse(overlap);
    CHECK(oj.size() == 1 && oj.items().front().get("severity").asString("") == "error" && oj.items().front().get("pads").size() == 2);
    sieda_string_free(overlap);
}

TEST(footprint_editor_land_patterns) {
    auto spec = [](const char* name) { return findStandardPart(name)->spec; };
    auto padOf = [](const std::shared_ptr<const CustomPart>& part, const std::string& number) -> const PadDef* {
        for (const auto& pad : part->footprint.pads)
            if (pad.pinIndex >= 0 && part->def.pins[static_cast<size_t>(pad.pinIndex)].number == number) return &pad;
        return nullptr;
    };
    // Any generated footprint converts to an editable land pattern that draws the same pads on the same pins.
    for (const char* name : {"NE555", "LM7805", "STM32F405RGT6", "DRV8833PWPR", "LM2596S-5.0", "XC7A35T-1CSG324I",
                             "BMI088", "TMC2209-LA", "LM358DR", "AMS1117-3.3", "BSS138", "ESP32-WROOM-32E",
                             "Crystal_16MHz_3225", "BSC028N06LS3G"}) {
        const CustomPartSpec original = spec(name);
        const CustomPartSpec editable = landPatternFromFootprint(original);
        CHECK(usesLandPattern(editable.package.type));
        auto a = CustomPartRegistry::instance().registerPart(original);
        auto b = CustomPartRegistry::instance().registerPart(editable);
        CHECK(a->footprint.pads.size() == b->footprint.pads.size());
        bool same = true;
        for (size_t i = 0; i < a->footprint.pads.size(); ++i) {
            const PadDef &pa = a->footprint.pads[i], &pb = b->footprint.pads[i];
            same &= std::fabs(pa.offset.x - pb.offset.x) < 1e-9 && std::fabs(pa.offset.y - pb.offset.y) < 1e-9 &&
                    std::fabs(pa.size.x - pb.size.x) < 1e-9 && pa.throughHole == pb.throughHole && pa.round == pb.round &&
                    pa.pinIndex == pb.pinIndex;
        }
        if (!same) std::printf("    %s: converted pads differ\n", name);
        CHECK(same);
        CHECK(std::fabs(a->footprint.body.width - b->footprint.body.width) < 1e-9);
        // The pins are untouched (the app compares them field by field).
        CHECK(customPartSpecToJson(editable).get("pins").dump() == customPartSpecToJson(original).get("pins").dump());
        // Survives JSON (saved in the project library) and checks clean.
        CHECK(customPartSpecToJson(customPartSpecFromJson(customPartSpecToJson(editable))).dump() ==
              customPartSpecToJson(editable).dump());
        for (const auto& issue : checkLandPattern(editable))
            if (issue.severity == "error") std::printf("    %s: %s\n", name, issue.message.c_str());
    }
    // DIP keeps its plated through-holes; the TO-263 tab stays on the middle lead's pin; the QFN's exposed pad on EP.
    const CustomPartSpec dip = landPatternFromFootprint(spec("NE555"));
    CHECK(dip.package.lands[0].drill > 0 && dip.package.lands[1].round);
    const CustomPartSpec d2pak = landPatternFromFootprint(spec("LM2596S-5.0"));
    CHECK(d2pak.package.lands.size() == 6 && !d2pak.package.lands[5].pin.empty());
    const CustomPartSpec qfn = landPatternFromFootprint(spec("TMC2209-LA"));
    bool epOk = false;
    for (const auto& l : qfn.package.lands) epOk |= l.pin == "EP" || l.pin == "29";
    CHECK(epOk);
    CHECK(landPatternFromFootprint(spec("BMI088")).package.type == "LGA");  // already a land pattern
    bool emptyThrew = false;  // nothing to convert: no pins, no package
    try {
        landPatternFromFootprint(CustomPartSpec{});
    } catch (const JsonError&) {
        emptyThrew = true;
    }
    CHECK(emptyThrew);

    // Edit: move pad 2 onto pad 1 → overlap; nudge it close → gap warning; a pad with no pin; a pin with no pad.
    CustomPartSpec e = landPatternFromFootprint(spec("NE555"));
    CHECK(checkLandPattern(e).empty());
    auto codes = [](const std::vector<LandIssue>& v) {
        std::set<std::string> out;
        for (const auto& i : v) out.insert(i.code);
        return out;
    };
    CustomPartSpec moved = e;
    moved.package.lands[1].x = moved.package.lands[0].x;
    moved.package.lands[1].y = moved.package.lands[0].y + 0.5;
    CHECK(codes(checkLandPattern(moved)).count("LAND_OVERLAP"));
    moved.package.lands[1].y = moved.package.lands[0].y + moved.package.lands[0].h + 0.05;
    auto gapIssues = checkLandPattern(moved);
    CHECK(codes(gapIssues).count("LAND_GAP") && !codes(gapIssues).count("LAND_OVERLAP"));
    CustomPartSpec extra = e;
    extra.package.lands.push_back(PackageSpec::Land(0, 0, 1.0, 1.0, 0.8, true));  // 0.1 mm ring is fine
    CHECK(codes(checkLandPattern(extra)).count("LAND_NO_PIN"));
    extra.package.lands.back().drill = 0.9;
    CHECK(codes(checkLandPattern(extra)).count("LAND_ANNULAR"));
    extra.package.lands.back().pin = "-";  // mechanical
    CHECK(!codes(checkLandPattern(extra)).count("LAND_NO_PIN"));
    auto mech = CustomPartRegistry::instance().registerPart(extra);
    CHECK(mech->footprint.pads.back().pinIndex == -1 && mech->footprint.pads.back().throughHole);
    CustomPartSpec missing = e;
    missing.package.lands.pop_back();
    CHECK(codes(checkLandPattern(missing)).count("LAND_NO_PAD"));
    bool threw = false;
    try {
        CustomPartRegistry::instance().registerPart(missing);
    } catch (const JsonError&) {
        threw = true;
    }
    CHECK(threw);
    // Two pads on one pin (a split ground) both connect to it and may touch.
    CustomPartSpec split = e;
    split.package.lands.push_back(PackageSpec::Land(0, 0, 1.0, 1.0, 0, false, "1"));
    auto sp = CustomPartRegistry::instance().registerPart(split);
    CHECK(sp->footprint.pads.back().pinIndex == padOf(sp, "1")->pinIndex);
    // Drill larger than the pad is refused on load.
    Json bad = customPartSpecToJson(e);
    bad["package"]["lands"] = Json::parse("[[0, 0, 1, 1, 1.2, 1]]");
    threw = false;
    try {
        customPartSpecFromJson(bad);
    } catch (const JsonError&) {
        threw = true;
    }
    CHECK(threw);
}

/// The robot segments JSON lists every platform with its parts kit (the app reads the kits from it).
static bool json_roundtrip_platform_kits() {
    Project p;
    Json j = Json::parse(robotSegmentsJson(p).dump());
    for (const auto& plat : j.get("platforms").items()) {
        const auto& kit = robotPartKit(plat.get("id").asString(""));
        if (plat.get("kit").size() != kit.size() || kit.empty()) return false;
        if (plat.get("kit")[0].get("parts").size() != kit[0].parts.size()) return false;
    }
    return j.get("platforms").size() == 7;
}

TEST(robotics_part_kits_place_on_a_board) {
    // Every robot platform (rover, drone, arm, quadruped, humanoid, 3D printer, CNC) has a production parts kit; every
    // part in it is in the standard library on its real package, registers, and places and routes on a board.
    const char* platforms[] = {"rover", "fpv", "arm", "quadruped", "humanoid", "printer3d", "cnc"};
    std::set<std::string> all;
    for (const char* id : platforms) {
        CHECK(findRobotPlatform(id) != nullptr);
        const auto& kit = robotPartKit(id);
        CHECK(kit.size() >= 7);
        size_t count = 0;
        for (const auto& group : kit) {
            CHECK(!group.subsystem.empty() && !group.parts.empty());
            for (const auto& name : group.parts) {
                ++count;
                all.insert(name);
                const StandardPart* sp = findStandardPart(name);
                if (!sp) std::printf("    %s kit: missing %s\n", id, name.c_str());
                CHECK(sp != nullptr);
            }
        }
        CHECK(count >= 25);
    }
    CHECK(robotPartKit("toaster").empty());
    CHECK(all.size() >= 110);
    for (const auto& name : all) {
        const StandardPart* sp = findStandardPart(name);
        if (!sp) continue;
        bool ok = true;
        try {
            auto part = CustomPartRegistry::instance().registerPart(sp->spec);
            size_t mapped = 0;
            for (const auto& pad : part->footprint.pads) mapped += pad.pinIndex >= 0 ? 1 : 0;
            // Every pin has a pad (pins that share a pad — stacked supplies — are fine) and the courtyard holds them.
            ok = !part->def.pins.empty() && mapped >= 2 && part->footprint.courtyardW > 0 && part->footprint.courtyardH > 0;
        } catch (const std::exception& e) {
            std::printf("    %s: %s\n", name.c_str(), e.what());
            ok = false;
        }
        CHECK(ok);
    }
    CHECK(json_roundtrip_platform_kits());

    // A drone kit's flight-controller core placed and routed: STM32F405 + BMI088 + MS5611 + W25Q128 on one board.
    Project p;
    auto& sch = p.schematic;
    int x = 0;
    std::map<std::string, int> u;
    for (const char* name : {"STM32F405RGT6", "BMI088", "MS5611-01BA03", "W25Q128JVSIQ", "BSC028N06LS3G"}) {
        u[name] = sch.addCustomComponent(p.addCustomPart(findStandardPart(name)->spec), "", {static_cast<double>(x), 0});
        x += 300;
    }
    auto pin = [&](const char* part, const char* pinName) { return sch.pinIndex(u[part], pinName); };
    CHECK(pin("BMI088", "SDO1") >= 0 && pin("MS5611-01BA03", "SDO") >= 0);
    sch.connect({u["STM32F405RGT6"], pin("STM32F405RGT6", "PA6")}, {u["BMI088"], pin("BMI088", "SDO1")});
    sch.connect({u["STM32F405RGT6"], pin("STM32F405RGT6", "PA5")}, {u["MS5611-01BA03"], pin("MS5611-01BA03", "SCLK")});
    sch.connect({u["STM32F405RGT6"], pin("STM32F405RGT6", "PB3")}, {u["W25Q128JVSIQ"], pin("W25Q128JVSIQ", "CLK")});
    sch.connect({u["STM32F405RGT6"], pin("STM32F405RGT6", "PB0")}, {u["BSC028N06LS3G"], pin("BSC028N06LS3G", "G")});
    p.pcb.settings.width = 70;
    p.pcb.settings.height = 50;
    p.pcb.autoPlace(sch, true);
    const auto route = p.pcb.autoRoute(sch);
    CHECK(route.failed == 0);
}

TEST(exact_land_patterns_for_irregular_packages) {
    auto reg = [](const char* name) { return CustomPartRegistry::instance().registerPart(findStandardPart(name)->spec); };
    auto padOf = [](const std::shared_ptr<const CustomPart>& part, const char* number) -> const PadDef* {
        for (const auto& pad : part->footprint.pads)
            if (pad.pinIndex >= 0 && part->def.pins[static_cast<size_t>(pad.pinIndex)].number == number) return &pad;
        return nullptr;
    };
    // BMP280: 2 × 2.5 mm, clockwise pad numbering — pads 1…4 across the top, 5…8 back along the bottom.
    auto bmp = reg("BMP280");
    CHECK(bmp->footprint.pads.size() == 8);
    CHECK(bmp->footprint.label == "LGA-8");
    CHECK_NEAR(padOf(bmp, "1")->offset.x, -0.975, 1e-9);
    CHECK_NEAR(padOf(bmp, "1")->offset.y, -0.8, 1e-9);
    CHECK_NEAR(padOf(bmp, "4")->offset.x, 0.975, 1e-9);
    CHECK_NEAR(padOf(bmp, "5")->offset.y, 0.8, 1e-9);
    CHECK_NEAR(bmp->footprint.body.width, 2.0, 1e-9);
    CHECK_NEAR(bmp->footprint.body.depth, 2.5, 1e-9);
    // BMI088: 4.5 × 3 mm, seven pads per long side and one on each short side (pad 8 right, 16 left).
    auto bmi = reg("BMI088");
    CHECK(padOf(bmi, "8")->offset.x > 1.9 && std::fabs(padOf(bmi, "8")->offset.y) < 1e-9);
    CHECK(padOf(bmi, "16")->offset.x < -1.9);
    CHECK(padOf(bmi, "8")->size.x > padOf(bmi, "8")->size.y);  // short-side pad turned 90°
    // SuperSO8 MOSFET: the drain is one large pad, not four small ones.
    auto fet = reg("BSC028N06LS3G");
    CHECK(fet->footprint.pads.size() == 5);
    const PadDef* drain = padOf(fet, "5");
    CHECK(drain && drain->size.x * drain->size.y > 15.0 && fet->def.pins[static_cast<size_t>(drain->pinIndex)].name == "D");
    // SC-70-6 load switch on its 0.65 mm pitch.
    auto sw = reg("TPS22919DCKR");
    CHECK_NEAR(padOf(sw, "2")->offset.y - padOf(sw, "1")->offset.y, 0.65, 1e-9);
    // The land pattern survives JSON (it is what the app saves into the project library) and is validated.
    CustomPartSpec back = customPartSpecFromJson(customPartSpecToJson(findStandardPart("BMI088")->spec));
    CHECK(back.package.type == "LGA" && back.package.lands.size() == 16);
    CHECK_NEAR(back.package.lands[7].x, findStandardPart("BMI088")->spec.package.lands[7].x, 1e-12);
    CHECK_NEAR(back.package.bodyDepth, 3.0, 1e-12);
    CHECK(CustomPartRegistry::instance().registerPart(back)->id == bmi->id);
    CustomPartSpec bare = back;
    bare.package.lands.clear();
    bool threw = false;
    try {
        CustomPartRegistry::instance().registerPart(bare);
    } catch (const JsonError&) {
        threw = true;
    }
    CHECK(threw);
    Json bad = customPartSpecToJson(back);
    bad["package"]["lands"] = Json::parse("[[0, 0, 0, 1]]");
    threw = false;
    try {
        customPartSpecFromJson(bad);
    } catch (const JsonError&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(robotics_segments_and_rules) {
    CHECK(robotPlatforms().size() == 7);  // + 3D printer and CNC machine
    CHECK(findRobotPlatform("quadruped") && findRobotPlatform("rover") && !findRobotPlatform("toaster"));
    auto codes = [](const Project& p) {
        std::set<std::string> out;
        for (const auto& v : roboticsChecks(p)) out.insert(v.code);
        return out;
    };
    // A 24 V motor drive: battery, low-side MOSFET switching an inductive load.
    Project p;
    auto& s = p.schematic;
    int v = s.addComponent(ComponentKind::VoltageSource, "24", {0, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 100});
    int bus = s.addComponent(ComponentKind::NetLabel, "VBUS", {60, -40});
    int load = s.addComponent(ComponentKind::Inductor, "1m", {120, 0});
    int q = s.addComponent(ComponentKind::NMOS, "AO3400", {200, 40});
    wire(s, v, "-", g, "GND");
    wire(s, bus, "N", load, "1");
    wire(s, load, "2", q, "D");
    wire(s, q, "S", g, "GND");
    int direct = s.connect({v, 0}, {bus, 0});
    p.schematicChanged();
    CHECK(roboticsChecks(p).empty());  // not a robot project yet
    p.robotPlatform = "arm";
    auto c1 = codes(p);
    CHECK(c1.count("REL_REVERSE_POLARITY") && c1.count("REL_NO_FUSE"));
    // Series Schottky + fuse on the input clears both.
    s.removeWire(direct);
    int d = s.addComponent(ComponentKind::Diode, "SS34", {20, -40});
    int f = s.addComponent(ComponentKind::Fuse, "5", {40, -40});
    wire(s, v, "+", d, "A");
    wire(s, d, "K", f, "1");
    wire(s, f, "2", bus, "N");
    p.schematicChanged();
    auto c2 = codes(p);
    CHECK(!c2.count("REL_REVERSE_POLARITY") && !c2.count("REL_NO_FUSE"));

    // CAN bus: termination required between CANH and CANL.
    int h = s.addComponent(ComponentKind::NetLabel, "CANH", {300, 0});
    int l = s.addComponent(ComponentKind::NetLabel, "CANL", {300, 60});
    int rt = s.addComponent(ComponentKind::Resistor, "1k", {340, 30});
    wire(s, h, "N", rt, "1");
    wire(s, l, "N", rt, "2");
    p.schematicChanged();
    CHECK(codes(p).count("REL_BUS_TERMINATION"));
    s.find(rt)->value = "120";
    CHECK(!codes(p).count("REL_BUS_TERMINATION"));
    bool canPair = false;
    for (auto [a, b] : differentialPairs(s)) canPair |= s.nets()[static_cast<size_t>(a)].name == "CANH";
    CHECK(canPair);

    // Analog ground: one star tie, not zero and not two.
    int agnd = s.addComponent(ComponentKind::NetLabel, "AGND", {400, 0});
    int r1 = s.addComponent(ComponentKind::Resistor, "10k", {440, 0});
    wire(s, agnd, "N", r1, "1");
    wire(s, r1, "2", bus, "N");
    p.schematicChanged();
    CHECK(codes(p).count("REL_STAR_GROUND"));  // floating
    int fb = s.addComponent(ComponentKind::Inductor, "1u", {440, 60});
    wire(s, agnd, "N", fb, "1");
    wire(s, fb, "2", g, "GND");
    p.schematicChanged();
    CHECK(!codes(p).count("REL_STAR_GROUND"));
    int fb2 = s.addComponent(ComponentKind::Inductor, "1u", {480, 60});
    wire(s, agnd, "N", fb2, "1");
    wire(s, fb2, "2", g, "GND");
    p.schematicChanged();
    CHECK(codes(p).count("REL_STAR_GROUND"));  // ground loop

    // Segment report: seven segments, power complete only once a TVS joins the fuse and reverse protection.
    auto segs = robotSegments(p);
    CHECK(segs.size() == 7);
    CHECK(segs[0].id == "power" && segs[0].status == "partial");
    CHECK(robotSegmentsJson(p)["segments"].size() == 7);
    Project saved = Project::fromJson(p.toJson());
    CHECK(saved.robotPlatform == "arm");

    // Thermal vias at an SMD power FET on a laid-out board.
    p.pcb.settings.layerCount = 4;
    p.pcb.settings.width = 60;
    p.pcb.settings.height = 40;
    p.pcb.autoPlace(s, true);
    CHECK(p.pcb.autoRoute(s).failed == 0);
    const size_t before = p.pcb.vias.size();
    const int added = addThermalVias(p, q, 6);
    CHECK(added > 0 && p.pcb.vias.size() == before + static_cast<size_t>(added));
    int drainNet = s.netOf({q, 1});
    for (size_t i = before; i < p.pcb.vias.size(); ++i) CHECK(p.pcb.vias[i].net == drainNet);
    CHECK(p.pcb.ratsnest(s).empty());
    int errors = 0;
    for (const auto& e : p.pcb.runDRC(s)) errors += e.severity == Severity::Error;
    CHECK(errors == 0);
}

TEST(automotive_ecu_segments_and_rules) {
    auto partId = [](const char* name) {
        return CustomPartRegistry::instance().registerPart(findStandardPart(name)->spec)->id;
    };
    CHECK(ecuTypes().size() == 6);
    CHECK(findEcuType("bcm") && findEcuType("gateway") && !findEcuType("toaster"));
    auto codes = [](const Project& p) {
        std::set<std::string> out;
        for (const auto& v : automotiveChecks(p))
            if (v.severity != Severity::Info) out.insert(v.code);
        return out;
    };
    // Battery straight into an LM7805 that powers an ATmega328P.
    Project p;
    auto& s = p.schematic;
    int v = s.addComponent(ComponentKind::VoltageSource, "13.5", {0, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 100});
    int reg = s.addCustomComponent(partId("LM7805"), "", {100, 0});
    int mcu = s.addCustomComponent(partId("ATmega328P"), "", {300, 0});
    wire(s, v, "-", g, "GND");
    wire(s, v, "+", reg, "1");
    wire(s, reg, "2", g, "GND");
    wire(s, reg, "3", mcu, "7");
    wire(s, mcu, "8", g, "GND");
    p.schematicChanged();
    CHECK(automotiveChecks(p).empty());  // not an ECU project yet
    p.ecuType = "bcm";
    auto c1 = codes(p);
    CHECK(c1.count("REL_REVERSE_POLARITY") && c1.count("REL_NO_FUSE") && c1.count("REL_EMI_FILTER"));
    CHECK(c1.count("REL_COLD_CRANK"));  // 7805: 5 V + 2 V dropout > 6 V crank
    CHECK(c1.count("REL_WATCHDOG"));

    // Watchdog supervisor: RESET to the MCU reset and WDI kicked by a GPIO clears the rule.
    int wd = s.addCustomComponent(partId("TPS3823-33-Q1"), "", {300, 200});
    wire(s, wd, "VDD", mcu, "7");
    wire(s, wd, "GND", g, "GND");
    wire(s, wd, "RESET", mcu, "1");
    p.schematicChanged();
    CHECK(codes(p).count("REL_WATCHDOG"));  // WDI not kicked yet
    wire(s, wd, "WDI", mcu, "5");
    p.schematicChanged();
    CHECK(!codes(p).count("REL_WATCHDOG"));

    // CAN bus lines: termination and bus ESD.
    int h = s.addComponent(ComponentKind::NetLabel, "CANH", {500, 0});
    int l = s.addComponent(ComponentKind::NetLabel, "CANL", {500, 60});
    int rt = s.addComponent(ComponentKind::Resistor, "120", {540, 30});
    wire(s, h, "N", rt, "1");
    wire(s, l, "N", rt, "2");
    p.schematicChanged();
    auto c2 = codes(p);
    CHECK(!c2.count("REL_BUS_TERMINATION") && c2.count("REL_BUS_ESD") && c2.count("REL_CLOCK"));
    int e1 = s.addComponent(ComponentKind::Diode, "PESD1CAN", {560, 0});
    int e2 = s.addComponent(ComponentKind::Diode, "PESD1CAN", {560, 60});
    wire(s, e1, "K", h, "N");
    wire(s, e1, "A", g, "GND");
    wire(s, e2, "K", l, "N");
    wire(s, e2, "A", g, "GND");
    int y = s.addCustomComponent(partId("Crystal_8MHz"), "", {300, -100});
    wire(s, y, "1", mcu, "9");
    wire(s, y, "2", mcu, "10");
    p.schematicChanged();
    auto c3 = codes(p);
    CHECK(!c3.count("REL_BUS_ESD") && !c3.count("REL_CLOCK"));

    // Six segments; a body ECU without a LIN transceiver is incomplete on the network segment.
    auto segs = ecuSegments(p);
    CHECK(segs.size() == 6);
    CHECK(segs[0].id == "shield" && segs[0].status == "partial");
    CHECK(segs[3].id == "network");
    bool linMissing = false;
    for (const auto& i : segs[3].items) linMissing |= i.label == "LIN node" && !i.ok;
    CHECK(linMissing);
    p.ecuType = "powertrain";
    const auto powertrain = ecuSegments(p);
    for (const auto& i : powertrain[3].items)
        if (i.label == "LIN node") CHECK(i.ok);
    Json j = ecuSegmentsJson(p);
    CHECK(j["segments"].size() == 6 && j["platforms"].size() == 6);
    Project saved = Project::fromJson(p.toJson());
    CHECK(saved.ecuType == "powertrain");
    // The automotive industry profile alone turns the checks on.
    p.ecuType.clear();
    CHECK(automotiveChecks(p).empty());
    p.industry = "automotive";
    CHECK(!automotiveChecks(p).empty());
}

TEST(aerospace_segments_rules_and_isolated_power) {
    auto partId = [](const char* name) {
        return CustomPartRegistry::instance().registerPart(findStandardPart(name)->spec)->id;
    };
    CHECK(aerospaceMissions().size() == 5);
    CHECK(findAerospaceMission("leo") && findAerospaceMission("commercial") && !findAerospaceMission("mars"));
    auto codes = [](const Project& p) {
        std::set<std::string> out;
        for (const auto& v : aerospaceChecks(p))
            if (v.severity != Severity::Info) out.insert(v.code);
        return out;
    };
    // A 28 V bus into an isolated DC-DC: the secondary regulates and the primary draws P_out / efficiency.
    Project p;
    auto& s = p.schematic;
    int v = s.addComponent(ComponentKind::VoltageSource, "28", {0, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 200});
    int rtn = s.addComponent(ComponentKind::NetLabel, "RTN", {0, 100});
    int dc = s.addCustomComponent(partId("ISO-DCDC-2805S"), "", {200, 0});
    int load = s.addComponent(ComponentKind::Resistor, "10", {400, 0});
    int bleed = s.addComponent(ComponentKind::Resistor, "10meg", {100, 150});
    wire(s, v, "+", dc, "+VIN");
    wire(s, v, "-", rtn, "N");
    wire(s, dc, "-VIN", rtn, "N");
    wire(s, dc, "+VOUT", load, "1");
    wire(s, load, "2", g, "GND");
    wire(s, dc, "-VOUT", g, "GND");
    wire(s, bleed, "1", rtn, "N");
    wire(s, bleed, "2", g, "GND");
    p.schematicChanged();
    {
        DcResult dcr = Simulator(s).dcOperatingPoint();
        CHECK(dcr.converged);
        CHECK_NEAR(netV(s, dcr, load, "1"), 5.0, 0.05);
        double supply = 0;
        for (const auto& d : dcr.devices)
            if (d.componentId == v) supply = d.current;
        // 2.5 W out at 82 % from 28 V ≈ 109 mA (+ 2 mA quiescent).
        CHECK_NEAR(supply, 2.5 / 0.82 / 28.0 + 0.002, 0.003);
        CHECK(std::fabs(netV(s, dcr, rtn, "N")) < 0.01);  // the bleeder carries no load current
    }
    CHECK(aerospaceChecks(p).empty());  // not an aerospace project yet
    p.industry = "space";
    CHECK(!aerospaceChecks(p).empty() && codes(p).empty());  // industry alone: advisory only
    p.aerospaceMission = "leo";
    auto c1 = codes(p);
    CHECK(!c1.count("REL_ISOLATED_POWER") && c1.count("REL_LIGHTNING_TVS") && c1.count("REL_SSPC"));

    // Three commercial MCUs straight on the rail: rad-hard, MRAM, latch-up and TMR voter findings.
    std::vector<int> lanes;
    for (int i = 0; i < 3; ++i) {
        int u = s.addCustomComponent(partId("ATmega328P"), "", {600.0 + 200 * i, 0});
        wire(s, u, "7", load, "1");
        wire(s, u, "8", g, "GND");
        lanes.push_back(u);
    }
    p.schematicChanged();
    auto c2 = codes(p);
    CHECK(c2.count("REL_RAD_HARD") && c2.count("REL_MRAM") && c2.count("REL_SEL_PROTECTION") && c2.count("REL_SEL_WATCHDOG"));
    CHECK(c2.count("REL_TMR_VOTER"));
    int voter = s.addCustomComponent(partId("74HC10"), "", {600, 300});
    wire(s, voter, "VCC", load, "1");
    wire(s, voter, "GND", g, "GND");
    p.schematicChanged();
    CHECK(!codes(p).count("REL_TMR_VOTER"));

    // LVDS receiver: 100 Ω across each used pair.
    int rx = s.addCustomComponent(partId("UT54LVDS032"), "", {600, 500});
    int dp = s.addComponent(ComponentKind::NetLabel, "SPW_DIN_P", {700, 500});
    int dn = s.addComponent(ComponentKind::NetLabel, "SPW_DIN_N", {700, 560});
    wire(s, rx, "RIN1+", dp, "N");
    wire(s, rx, "RIN1-", dn, "N");
    p.schematicChanged();
    CHECK(codes(p).count("REL_LVDS_TERMINATION"));
    int rt = s.addComponent(ComponentKind::Resistor, "100", {740, 530});
    wire(s, rt, "1", dp, "N");
    wire(s, rt, "2", dn, "N");
    p.schematicChanged();
    CHECK(!codes(p).count("REL_LVDS_TERMINATION"));

    // Five segments; MRAM missing, SpaceWire present for a LEO mission.
    auto segs = aerospaceSegments(p);
    CHECK(segs.size() == 5);
    CHECK(segs[0].id == "compute" && segs[0].status != "complete");
    CHECK(segs[3].id == "avionics");
    for (const auto& i : segs[3].items)
        if (i.label == "SpaceWire / LVDS") CHECK(i.ok);
    Json j = aerospaceSegmentsJson(p);
    CHECK(j["segments"].size() == 5 && j["platforms"].size() == 5);
    Project saved = Project::fromJson(p.toJson());
    CHECK(saved.aerospaceMission == "leo");
    // The isolated model survives a save / load of the part spec.
    CustomPartSpec spec = customPartSpecFromJson(customPartSpecToJson(findStandardPart("ISO-DCDC-2805S")->spec));
    CHECK(spec.model.regulator.isolated() && spec.model.regulator.inReturn == "2");
    CHECK_NEAR(spec.model.regulator.efficiency, 0.82, 1e-9);
}

TEST(naval_segments_and_rules) {
    auto partId = [](const char* name) {
        return CustomPartRegistry::instance().registerPart(findStandardPart(name)->spec)->id;
    };
    CHECK(navalPlatforms().size() == 5 && findNavalPlatform("submarine") && !findNavalPlatform("yacht"));
    auto codes = [](const Project& p) {
        std::set<std::string> out;
        for (const auto& v : navalChecks(p))
            if (v.severity != Severity::Info) out.insert(v.code);
        return out;
    };
    Project p;
    auto& s = p.schematic;
    int v = s.addComponent(ComponentKind::VoltageSource, "28", {0, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 100});
    int u = s.addCustomComponent(partId("ATmega328P"), "", {200, 0});
    wire(s, v, "-", g, "GND");
    wire(s, v, "+", u, "7");
    wire(s, u, "8", g, "GND");
    p.schematicChanged();
    CHECK(navalChecks(p).empty());
    p.industry = "marine";
    CHECK(!navalChecks(p).empty() && codes(p).empty());  // advisory only
    p.navalPlatform = "combatant";
    auto c1 = codes(p);
    for (const char* code : {"REL_SHIP_ISOLATION", "REL_SURGE_FRONT_END", "REL_SALT_FOG_COATING", "REL_HERMETIC",
                             "REL_SHOCK_UNDERFILL", "REL_SHOCK_MOUNTING", "REL_SHOCK_SUBSTRATE", "REL_FIBER_LINK"})
        CHECK(c1.count(code));
    // Board build: coating, underfill, thick polyimide, close mounting holes.
    p.pcb.settings.coating = "parylene";
    p.pcb.settings.underfill = true;
    p.pcb.settings.thickness = 2.4;
    p.pcb.settings.layerCount = 8;
    p.pcb.settings.material = "polyimide";
    p.pcb.settings.clearance = 0.25;
    for (double x : {10.0, 70.0})
        for (double y : {10.0, 70.0}) p.pcb.settings.holes.push_back({{x, y}, 3.2, 6.4});
    auto c2 = codes(p);
    for (const char* code : {"REL_SALT_FOG_COATING", "REL_SHOCK_UNDERFILL", "REL_SHOCK_MOUNTING", "REL_SHOCK_SUBSTRATE",
                             "REL_ECM_SPACING"})
        CHECK(!c2.count(code));
    // A radar LNA needs a PIN limiter on its input.
    int lna = s.addCustomComponent(partId("LNA-MMIC"), "", {400, 0});
    int in = s.addComponent(ComponentKind::NetLabel, "LNA_IN", {360, 0});
    wire(s, lna, "RFIN", in, "N");
    wire(s, lna, "GND", g, "GND");
    p.schematicChanged();
    CHECK(codes(p).count("REL_LNA_LIMITER"));
    int d = s.addComponent(ComponentKind::Diode, "BAP64-02", {380, 60});
    wire(s, d, "A", in, "N");
    wire(s, d, "K", g, "GND");
    p.schematicChanged();
    CHECK(!codes(p).count("REL_LNA_LIMITER"));
    auto segs = navalSegments(p);
    CHECK(segs.size() == 5 && segs[2].id == "mechanical" && segs[2].status == "complete");
    CHECK(navalSegmentsJson(p)["segments"].size() == 5);
    CHECK(Project::fromJson(p.toJson()).navalPlatform == "combatant");
    CHECK(Project::fromJson(p.toJson()).pcb.settings.underfill);
}

TEST(galvanic_domains_and_isolation_barrier) {
    auto partId = [](const char* name) {
        return CustomPartRegistry::instance().registerPart(findStandardPart(name)->spec)->id;
    };
    // System side: 5 V → isolated converter → patient side with a resistor load; an isolator crosses too.
    Project p;
    auto& s = p.schematic;
    int v = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 100});
    int dc = s.addCustomComponent(partId("ISO-DCDC-MED"), "", {200, 0});
    int pg = s.addComponent(ComponentKind::NetLabel, "PGND", {300, 100});
    int load = s.addComponent(ComponentKind::Resistor, "1k", {400, 0});
    int ecg = s.addComponent(ComponentKind::NetLabel, "ECG_RA", {500, 0});
    int rin = s.addComponent(ComponentKind::Resistor, "10k", {500, 60});
    wire(s, v, "-", g, "GND");
    wire(s, v, "+", dc, "+VIN");
    wire(s, dc, "-VIN", g, "GND");
    wire(s, dc, "+VOUT", load, "1");
    wire(s, dc, "-VOUT", pg, "N");
    wire(s, load, "2", pg, "N");
    wire(s, ecg, "N", rin, "1");
    wire(s, rin, "2", pg, "N");
    p.schematicChanged();
    const GalvanicDomains doms = galvanicDomains(s);
    const int sys = doms.domainOfNet(s.netOf({v, 0})), pat = doms.domainOfNet(s.netOf({load, 0}));
    CHECK(sys >= 0 && pat >= 0 && sys != pat);
    CHECK(doms.domainOfNet(s.netOf({rin, 0})) == pat);
    CHECK(doms.domainOfComponent(s, *s.find(dc)) == -1 && doms.domainOfComponent(s, *s.find(load)) == pat);
    {
        DcResult dcr = Simulator(s).dcOperatingPoint();  // 3.0–5.5 V in, isolated 5 V out
        CHECK(dcr.converged);
        CHECK_NEAR(netV(s, dcr, load, "1") - netV(s, dcr, load, "2"), 5.0, 0.05);
    }
    // Medical rules: patient barrier present; 2 × MOPP creepage asked of the board.
    p.medicalClass = "cf";
    std::set<std::string> c1;
    for (const auto& x : medicalChecks(p))
        if (x.severity != Severity::Info) c1.insert(x.code);
    CHECK(!c1.count("REL_PATIENT_ISOLATION") && c1.count("REL_MOPP_CREEPAGE") && c1.count("REL_DEFIB_PROTECTION"));
    // A Y-capacitor across the barrier merges the domains: the patient is no longer isolated.
    int ycap = s.addComponent(ComponentKind::Capacitor, "4.7n", {300, 200});
    wire(s, ycap, "1", pg, "N");
    wire(s, ycap, "2", g, "GND");
    p.schematicChanged();
    std::set<std::string> c2;
    for (const auto& x : medicalChecks(p))
        if (x.severity != Severity::Info) c2.insert(x.code);
    CHECK(c2.count("REL_PATIENT_ISOLATION"));
    s.removeComponent(ycap);
    p.schematicChanged();

    // Layout: with an 8 mm barrier the domains are placed, routed and poured 8 mm apart and DRC proves it.
    p.pcb.settings.isolationGap = 8;
    p.pcb.settings.layerCount = 2;
    p.pcb.settings.width = 60;
    p.pcb.settings.height = 40;
    p.pcb.zones.push_back({"GND", 1, false, 0});
    p.pcb.autoPlace(s, true);
    CHECK(p.pcb.autoRoute(s).failed == 0);
    int gapErrors = 0;
    for (const auto& x : p.pcb.runDRC(s)) gapErrors += x.code == "DRC_ISOLATION_GAP";
    CHECK(gapErrors == 0);
    // A patient-side part moved next to the system side is caught.
    const Vec2 next = s.find(v)->pcb.position + Vec2{3, 0};
    for (auto& c : s.mutableComponents())
        if (c.id == load) c.pcb.position = next;
    p.pcb.clearRouting();
    int after = 0;
    for (const auto& x : p.pcb.runDRC(s)) after += x.code == "DRC_ISOLATION_GAP";
    CHECK(after > 0);
    CHECK(Project::fromJson(p.toJson()).pcb.settings.isolationGap == 8);
    CHECK(Project::fromJson(p.toJson()).medicalClass == "cf");
}

TEST(tamper_mesh_over_secure_element) {
    auto partId = [](const char* name) {
        return CustomPartRegistry::instance().registerPart(findStandardPart(name)->spec)->id;
    };
    Project p;
    auto& s = p.schematic;
    int v = s.addComponent(ComponentKind::VoltageSource, "3.3", {0, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 100});
    int se = s.addCustomComponent(partId("SECURE-MCU"), "", {200, 0});
    int c1 = s.addComponent(ComponentKind::Capacitor, "100n", {300, 0});
    int r1 = s.addComponent(ComponentKind::Resistor, "10k", {300, 100});
    int r2 = s.addComponent(ComponentKind::Resistor, "10k", {300, 200});
    int ma = s.addComponent(ComponentKind::NetLabel, "TAMPER_MESH_A", {400, 0});
    int mb = s.addComponent(ComponentKind::NetLabel, "TAMPER_MESH_B", {400, 100});
    wire(s, v, "-", g, "GND");
    wire(s, v, "+", se, "VDD");
    wire(s, se, "GND", g, "GND");
    wire(s, c1, "1", se, "VDD");
    wire(s, c1, "2", g, "GND");
    wire(s, r1, "1", se, "NRST");
    wire(s, r1, "2", se, "VDD");
    wire(s, r2, "1", se, "TAMPER1");
    wire(s, r2, "2", g, "GND");
    wire(s, se, "MESH_A_DRV", ma, "N");
    wire(s, se, "MESH_A_SNS", ma, "N");
    wire(s, se, "MESH_B_DRV", mb, "N");
    wire(s, se, "MESH_B_SNS", mb, "N");
    p.schematicChanged();
    const std::string seRef = s.find(se)->ref;

    // Retail rules: a countertop terminal's secure element without a mesh, backup cell or tamper switches.
    p.retailDevice = "countertop";
    auto retailCodes = [&] {
        std::set<std::string> c;
        for (const auto& x : retailChecks(p))
            if (x.severity != Severity::Info) c.insert(x.code);
        return c;
    };
    {
        const auto c = retailCodes();
        CHECK(!c.count("REL_SECURE_ELEMENT") && c.count("REL_TAMPER_MESH") && c.count("REL_KEY_BATTERY"));
        CHECK(!c.count("REL_TAMPER_SWITCHES"));  // TAMPER1 is wired (pull-down R2)
        CHECK(c.count("REL_RETAIL_COATING") == 0);  // countertop: coating advice is Info
    }
    p.pcb.settings.layerCount = 4;
    p.pcb.settings.width = 50;
    p.pcb.settings.height = 45;
    p.pcb.tamperMeshes.push_back({seRef, "TAMPER_MESH_A", "TAMPER_MESH_B", 1, 2, 2.0});
    CHECK(!retailCodes().count("REL_TAMPER_MESH"));
    {
        const auto segs = retailSegments(p);
        CHECK(segs.size() == 4 && segs[0].id == "security");
        CHECK(segs[0].items[0].ok && segs[0].items[1].ok && !segs[0].items[2].ok);  // no backup cell yet
    }
    p.pcb.autoPlace(s, true);
    const RouteStats st = p.pcb.autoRoute(s);
    CHECK(st.failed == 0);
    const auto geo = p.pcb.tamperMeshGeometry(s, p.pcb.pads(s));
    CHECK(geo.size() == 1 && geo[0].error.empty());
    // The secure element sits under the mesh; both serpentines are laid on their inner layers.
    const Rect area = geo[0].region;
    CHECK(area.contains(s.find(se)->pcb.position));
    int stripesA = 0, stripesB = 0;
    for (const auto& t : p.pcb.tracks) {
        stripesA += t.net == geo[0].netA && t.layer == 1;
        stripesB += t.net == geo[0].netB && t.layer == 2;
        // Nothing else crosses the mesh layers inside the secure area.
        if ((t.layer == 1 || t.layer == 2) && t.net != geo[0].netA && t.net != geo[0].netB)
            CHECK(segmentRectDistance(t.a, t.b, area) > 0);
    }
    CHECK(stripesA > 10 && stripesB > 10);
    for (const auto& via : p.pcb.vias) CHECK(!area.contains(via.position) || via.net == geo[0].netA || via.net == geo[0].netB);
    std::set<std::string> codes;
    for (const auto& x : p.pcb.runDRC(s)) codes.insert(x.code);
    CHECK(!codes.count("DRC_TAMPER_MESH") && !codes.count("DRC_TAMPER_MESH_BREACH") && !codes.count("DRC_UNROUTED"));
    CHECK(!codes.count("DRC_CLEARANCE"));
    // A via drilled through the secure area is a breach.
    Via drill;
    drill.net = s.netOf({g, 0});
    drill.position = s.find(se)->pcb.position;
    p.pcb.vias.push_back(drill);
    std::set<std::string> breach;
    for (const auto& x : p.pcb.runDRC(s)) breach.insert(x.code);
    CHECK(breach.count("DRC_TAMPER_MESH_BREACH"));
    // Persisted with the project; a 2-layer board cannot carry a mesh.
    const Project q = Project::fromJson(p.toJson());
    CHECK(q.pcb.tamperMeshes.size() == 1 && q.pcb.tamperMeshes[0].netB == "TAMPER_MESH_B");
    p.pcb.settings.layerCount = 2;
    std::set<std::string> two;
    for (const auto& x : p.pcb.runDRC(s)) two.insert(x.code);
    CHECK(two.count("DRC_TAMPER_MESH"));
}

TEST(retail_printer_and_peripheral_rules) {
    auto partId = [](const char* name) {
        return CustomPartRegistry::instance().registerPart(findStandardPart(name)->spec)->id;
    };
    Project p;
    auto& s = p.schematic;
    p.industry = "retail";
    p.retailDevice = "printer";
    int v = s.addComponent(ComponentKind::VoltageSource, "24", {0, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 100});
    int head = s.addCustomComponent(partId("TPH-80MM"), "", {200, 0});
    int q = s.addComponent(ComponentKind::NMOS, "AO3400", {300, 100});
    int sol = s.addComponent(ComponentKind::NetLabel, "CUTTER_SOL", {400, 100});
    int j = s.addComponent(ComponentKind::Connector, "CUTTER", {500, 100});
    wire(s, v, "-", g, "GND");
    wire(s, v, "+", head, "1");
    wire(s, head, "3", g, "GND");
    wire(s, q, "D", sol, "N");
    wire(s, q, "S", g, "GND");
    wire(s, j, "2", sol, "N");
    wire(s, j, "1", v, "+");
    p.schematicChanged();
    auto codes = [&] {
        std::set<std::string> c;
        for (const auto& x : retailChecks(p))
            if (x.severity != Severity::Info) c.insert(x.code);
        return c;
    };
    {
        const auto c = codes();
        CHECK(c.count("REL_TPH_BULK") && c.count("REL_PRINTER_MOTOR") && c.count("REL_SOLENOID_FLYBACK"));
        CHECK(!c.count("REL_SECURE_ELEMENT"));  // a printer carries no payment keys
    }
    int bulk = s.addComponent(ComponentKind::Capacitor, "1000u", {200, 200});
    int fly = s.addComponent(ComponentKind::Diode, "1N4007", {400, 200});
    wire(s, bulk, "1", v, "+");
    wire(s, bulk, "2", g, "GND");
    wire(s, fly, "A", sol, "N");
    wire(s, fly, "K", v, "+");
    p.schematicChanged();
    {
        const auto c = codes();
        CHECK(!c.count("REL_TPH_BULK") && !c.count("REL_SOLENOID_FLYBACK") && c.count("REL_PRINTER_MOTOR"));
    }
    CHECK(Project::fromJson(p.toJson()).retailDevice == "printer");
    CHECK(findIndustry("retail") && findIndustry("appliance"));
    // A buck converter with a shared ground is simulated by efficiency and is not a galvanic barrier.
    Project b;
    auto& t = b.schematic;
    int vin = t.addComponent(ComponentKind::VoltageSource, "24", {0, 0});
    int gg = t.addComponent(ComponentKind::Ground, "", {0, 100});
    int buck = t.addCustomComponent(partId("LM2596-5.0"), "", {200, 0});
    int l = t.addComponent(ComponentKind::Inductor, "33u", {300, 0});
    int load = t.addComponent(ComponentKind::Resistor, "10", {400, 0});
    wire(t, vin, "-", gg, "GND");
    wire(t, vin, "+", buck, "VIN");
    wire(t, buck, "GND", gg, "GND");
    wire(t, buck, "ON_OFF", gg, "GND");
    wire(t, buck, "OUTPUT", l, "1");
    wire(t, l, "2", load, "1");
    wire(t, buck, "FEEDBACK", load, "1");
    wire(t, load, "2", gg, "GND");
    b.schematicChanged();
    DcResult dc = Simulator(t).dcOperatingPoint();
    CHECK(dc.converged);
    CHECK_NEAR(netV(t, dc, load, "1"), 5.0, 0.05);
    // 0.5 A out at 5 V and 85 %: ≈ 0.12 A from 24 V instead of 0.5 A through a linear pass element.
    CHECK(reading(dc, vin) && std::fabs(reading(dc, vin)->current) < 0.2);
    CHECK(galvanicDomains(t).domainOfNet(t.netOf({vin, 0})) == galvanicDomains(t).domainOfNet(t.netOf({load, 0})));
}

TEST(appliance_mains_rules_and_voltage_spacing) {
    auto partId = [](const char* name) {
        return CustomPartRegistry::instance().registerPart(findStandardPart(name)->spec)->id;
    };
    // 230 VAC through a fuse and a bridge into a bulk capacitor and a resistive load; a 3.3 V logic island beside it.
    Project p;
    auto& s = p.schematic;
    p.industry = "appliance";
    p.applianceType = "kitchen";
    int ac = s.addComponent(ComponentKind::ACSource, "SIN(0 325 50)", {0, 0});
    int f = s.addComponent(ComponentKind::Fuse, "2", {100, -100});
    int lbl = s.addComponent(ComponentKind::NetLabel, "AC_L", {50, -100});
    int d1 = s.addComponent(ComponentKind::Diode, "1N4007", {200, -100});
    int d2 = s.addComponent(ComponentKind::Diode, "1N4007", {200, 0});
    int d3 = s.addComponent(ComponentKind::Diode, "1N4007", {200, 100});
    int d4 = s.addComponent(ComponentKind::Diode, "1N4007", {200, 200});
    int g = s.addComponent(ComponentKind::Ground, "", {300, 300});
    int bulk = s.addComponent(ComponentKind::Capacitor, "10u", {300, 0});
    int load = s.addComponent(ComponentKind::Resistor, "100k", {400, 0});
    int v = s.addComponent(ComponentKind::VoltageSource, "3.3", {600, 0});
    int r1 = s.addComponent(ComponentKind::Resistor, "10k", {700, 0});
    int r2 = s.addComponent(ComponentKind::Resistor, "10k", {700, 100});
    wire(s, ac, "+", lbl, "N");
    wire(s, lbl, "N", f, "1");
    wire(s, f, "2", d1, "A");
    wire(s, f, "2", d3, "K");
    wire(s, ac, "-", d2, "A");
    wire(s, ac, "-", d4, "K");
    wire(s, d1, "K", bulk, "1");
    wire(s, d2, "K", bulk, "1");
    wire(s, d3, "A", g, "GND");
    wire(s, d4, "A", g, "GND");
    wire(s, bulk, "2", g, "GND");
    wire(s, load, "1", bulk, "1");
    wire(s, load, "2", g, "GND");
    wire(s, v, "-", g, "GND");
    wire(s, v, "+", r1, "1");
    wire(s, r1, "2", r2, "1");
    wire(s, r2, "2", g, "GND");
    p.schematicChanged();

    // The transient puts L, N and the bus at mains potential (not their DC point of 0 V); logic stays low.
    const auto ranges = netVoltageRanges(s);
    const int bus = s.netOf({bulk, 0}), logic = s.netOf({r1, 1});
    CHECK(ranges.at(bus).second > 250 && ranges.at(s.netOf({f, 1})).second > 250);
    CHECK(std::fabs(ranges.at(logic).second) < 5);
    {
        std::set<std::string> c;
        for (const auto& x : applianceChecks(p))
            if (x.severity != Severity::Info) c.insert(x.code);
        CHECK(!c.count("REL_MAINS_FUSE") && c.count("REL_MAINS_MOV") && c.count("REL_X_CAP") && c.count("REL_OFFLINE_SUPPLY"));
    }
    int mov = s.addCustomComponent(partId("S10K275"), "", {150, -50});
    int x2 = s.addCustomComponent(partId("X2-100N-275VAC"), "", {150, 50});
    wire(s, mov, "1", f, "2");
    wire(s, mov, "2", ac, "-");
    wire(s, x2, "1", f, "2");
    wire(s, x2, "2", ac, "-");
    p.schematicChanged();
    {
        std::set<std::string> c;
        for (const auto& x : applianceChecks(p))
            if (x.severity != Severity::Info) c.insert(x.code);
        CHECK(!c.count("REL_MAINS_MOV") && !c.count("REL_X_CAP"));
        const auto segs = applianceSegments(p);
        CHECK(segs.size() == 4 && segs[0].id == "mains" && segs[0].items[0].ok && segs[0].items[1].ok == false);
    }

    // Mains spacing: coated board → 0.8 mm for 325 V, applied by fencing the mains nets, not the whole board.
    p.pcb.settings.layerCount = 2;
    p.pcb.settings.width = 70;
    p.pcb.settings.height = 50;
    p.pcb.settings.coating = "acrylic";
    const SpacingDomains sd = spacingDomains(s, p.pcb.settings);
    CHECK_NEAR(sd.hvGap, 0.8, 1e-9);
    CHECK(sd.highVoltage[static_cast<size_t>(bus)] && !sd.highVoltage[static_cast<size_t>(logic)]);
    CHECK(sd.domains.domainOfNet(bus) != sd.domains.domainOfNet(logic));
    p.pcb.autoPlace(s, true);
    CHECK(p.pcb.autoRoute(s).failed == 0);
    CHECK(p.pcb.settings.clearance < 0.5);  // the logic keeps its design-rule clearance
    int hv = 0;
    for (const auto& x : p.pcb.runDRC(s)) hv += x.code == "DRC_HV_CLEARANCE" || x.code == "DRC_SHORT";
    CHECK(hv == 0);
    CHECK(Project::fromJson(p.toJson()).applianceType == "kitchen");
}

TEST(memory_design_segments_and_checks) {
    auto partId = [](const char* name) {
        return CustomPartRegistry::instance().registerPart(findStandardPart(name)->spec)->id;
    };
    auto codes = [](const Project& p, bool warningsOnly = true) {
        std::set<std::string> c;
        for (const auto& x : memoryChecks(p))
            if (!warningsOnly || x.severity != Severity::Info) c.insert(x.code);
        return c;
    };
    CHECK(memoryDesignTypes().size() == 5);
    for (const char* id : {"sdram", "ddr", "lpddr", "dimm", "rdimm"}) CHECK(findMemoryDesignType(id) != nullptr);
    CHECK(findMemoryDesignType("ddr6") == nullptr);
    CHECK(findIndustry("memory") && findIndustry("memory")->rulePreset == "HDI / Fine-Pitch BGA (IPC-2226)");

    // The DRAM parts: SDR SDRAM in TSOP-II, DDR3L / DDR4 in a depopulated 9 × 16 FBGA laid out by ball name.
    const auto sdr = CustomPartRegistry::instance().registerPart(findStandardPart("MT48LC16M16A2TG-6A")->spec);
    CHECK(sdr->footprint.pads.size() == 54 && sdr->footprint.label == "TSSOP-54 0.8mm pitch");
    for (const char* name : {"MT41K256M16HA-125", "AS4C256M16D3-12BCN", "MT40A512M16LY-062E"}) {
        const auto ddr = CustomPartRegistry::instance().registerPart(findStandardPart(name)->spec);
        CHECK(ddr->footprint.pads.size() == 96);
        for (const auto& pad : ddr->footprint.pads) CHECK(pad.pinIndex >= 0);
        const PadDef* a1 = nullptr;
        const PadDef* t9 = nullptr;
        for (const auto& pad : ddr->footprint.pads) {
            const std::string& n = ddr->def.pins[static_cast<size_t>(pad.pinIndex)].number;
            if (n == "A1") a1 = &pad;
            if (n == "T9") t9 = &pad;
        }
        // A1 top-left and T9 bottom-right of a 9-column, 16-row grid at 0.8 mm.
        CHECK(a1 && t9);
        if (a1 && t9) {
            CHECK_NEAR(a1->offset.x, -3.2, 1e-9);
            CHECK_NEAR(a1->offset.y, -6.0, 1e-9);
            CHECK_NEAR(t9->offset.x, 3.2, 1e-9);
            CHECK_NEAR(t9->offset.y, 6.0, 1e-9);
        }
        CHECK(ddr->footprint.body.depth > ddr->footprint.body.width);
    }
    // A full square grid keeps its row-major layout (the FPGA is unchanged).
    const auto fpga = CustomPartRegistry::instance().registerPart(findStandardPart("XC7A35T-1CSG324I")->spec);
    CHECK(fpga->footprint.pads.size() == 324 && fpga->footprint.body.width == fpga->footprint.body.depth);

    // SDR SDRAM beside a controller, wired data-only at first.
    Project p;
    auto& s = p.schematic;
    CHECK(memoryChecks(p).empty() && !isMemoryProject(p));
    p.industry = "memory";
    int mcu = s.addCustomComponent(partId("STM32H743IIT6"), "", {0, 0});
    int ram = s.addCustomComponent(partId("MT48LC16M16A2TG-6A"), "", {600, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {300, 400});
    const char* data[][2] = {{"DQ0", "PD14"}, {"DQ1", "PD15"}, {"DQ2", "PD0"}, {"DQ3", "PD1"}, {"DQ4", "PE7"}, {"DQ5", "PE8"},
                             {"DQ6", "PE9"}, {"DQ7", "PE10"}};
    for (auto& d : data) wire(s, ram, d[0], mcu, d[1]);
    wire(s, ram, "CLK", mcu, "PG8");
    wire(s, ram, "VSS", g, "GND");
    p.schematicChanged();
    for (const auto& v : memoryChecks(p)) CHECK(v.severity == Severity::Info);  // industry only: advice
    p.memoryDesign = "sdram";
    {
        const auto c = codes(p);
        CHECK(c.count("REL_DRAM_DECOUPLING") && c.count("REL_DRAM_LAYERS"));
        CHECK(!c.count("REL_DRAM_CONTROLLER"));  // the STM32 is found on the data lines
        CHECK(!c.count("REL_DDR_ZQ") && !c.count("REL_DDR_VTT"));  // no DDR rules on SDR
        const auto info = codes(p, false);
        CHECK(info.count("REL_DRAM_CLOCK_SERIES") && info.count("REL_DRAM_LENGTH_MATCH"));
    }
    // Decoupling, a named data bus and a 4-layer board with a ground plane clear them.
    int vdd = s.addComponent(ComponentKind::NetLabel, "+3V3", {300, -300});
    wire(s, ram, "VDD", vdd, "N");
    wire(s, ram, "VDDQ", vdd, "N");
    for (int i = 0; i < 4; ++i) {
        int c = s.addComponent(ComponentKind::Capacitor, i == 0 ? "10u" : "100n", {400.0 + 40 * i, -200});
        wire(s, c, "1", vdd, "N");
        wire(s, c, "2", g, "GND");
    }
    for (int i = 0; i < 8; ++i) {
        int l = s.addComponent(ComponentKind::NetLabel, ("SD_DQ" + std::to_string(i)).c_str(), {500, 50.0 * i});
        wire(s, l, "N", ram, data[i][0]);
    }
    p.pcb.settings.layerCount = 4;
    p.pcb.zones.push_back({"GND", 1, true, 0});
    p.schematicChanged();
    {
        const auto c = codes(p, false);
        CHECK(!c.count("REL_DRAM_DECOUPLING") && !c.count("REL_DRAM_BULK") && !c.count("REL_DRAM_LAYERS"));
        CHECK(!c.count("REL_DRAM_LENGTH_MATCH") && !c.count("REL_DRAM_REFERENCE_PLANE"));
        const auto segs = memorySegments(p);
        CHECK(segs.size() == 5 && segs[0].id == "power" && segs[1].id == "clock" && segs[2].id == "data" &&
              segs[3].id == "config" && segs[4].id == "layout");
        CHECK(segs[0].status == "complete");
        CHECK(segs[2].items[0].ok);  // data lanes matched
        CHECK(!segs[1].items[1].ok);  // address lines still open
    }
    CHECK(Project::fromJson(p.toJson()).memoryDesign == "sdram");
    Json bad = p.toJson();
    bad["memoryDesign"] = "ddr9";
    CHECK(Project::fromJson(bad).memoryDesign.empty());

    // DDR3L memory-down: ZQ, VREF, RESET_n, VTT and the CK pair are checked.
    Project q;
    auto& d = q.schematic;
    q.memoryDesign = "ddr";
    int dram = d.addCustomComponent(partId("MT41K256M16HA-125"), "", {0, 0});
    int gq = d.addComponent(ComponentKind::Ground, "", {0, 400});
    wire(d, dram, "V_{SS}", gq, "GND");
    q.schematicChanged();
    {
        const auto c = codes(q);
        CHECK(c.count("REL_DDR_ZQ") && c.count("REL_DDR_VREF") && c.count("REL_DDR_RESET") && c.count("REL_DDR_VTT"));
        CHECK(c.count("REL_DRAM_CONTROLLER"));
    }
    int zq = d.addComponent(ComponentKind::Resistor, "100", {100, 100});
    wire(d, dram, "ZQ", zq, "1");
    wire(d, zq, "2", gq, "GND");
    q.schematicChanged();
    bool wrongValue = false;
    for (const auto& v : memoryChecks(q)) wrongValue |= v.code == "REL_DDR_ZQ" && v.message.find("240") != std::string::npos;
    CHECK(wrongValue);
    d.find(zq)->value = "240 1%";
    int reset = d.addComponent(ComponentKind::Resistor, "10k", {100, 200});
    wire(d, dram, "nRESET", reset, "1");
    wire(d, reset, "2", gq, "GND");
    int vtt = d.addComponent(ComponentKind::NetLabel, "VTT", {200, 0});
    int rt = d.addComponent(ComponentKind::Resistor, "39", {200, 100});
    wire(d, dram, "A0", rt, "1");
    wire(d, rt, "2", vtt, "N");
    int ck = d.addComponent(ComponentKind::Resistor, "100", {300, 100});
    wire(d, dram, "CK", ck, "1");
    wire(d, dram, "nCK", ck, "2");
    int vref = d.addComponent(ComponentKind::NetLabel, "VREF", {300, 200});
    int ra = d.addComponent(ComponentKind::Resistor, "1k", {300, 250});
    int rb = d.addComponent(ComponentKind::Resistor, "1k", {300, 300});
    int cv = d.addComponent(ComponentKind::Capacitor, "100n", {350, 250});
    wire(d, dram, "V_{REFCA}", vref, "N");
    wire(d, dram, "V_{REFDQ}", vref, "N");
    wire(d, ra, "1", vref, "N");
    wire(d, rb, "1", vref, "N");
    wire(d, rb, "2", gq, "GND");
    wire(d, cv, "1", vref, "N");
    wire(d, cv, "2", gq, "GND");
    q.schematicChanged();
    {
        const auto c = codes(q);
        CHECK(!c.count("REL_DDR_ZQ") && !c.count("REL_DDR_RESET") && !c.count("REL_DDR_VTT") && !c.count("REL_DDR_CK_TERM"));
        CHECK(!c.count("REL_DDR_VREF"));
        const auto segs = memorySegments(q);
        CHECK(segs[2].items[2].ok);  // ZQ calibration resistor
    }

    // A DDR5 module: SPD EEPROM with pull-ups and a 1.2–1.27 mm board.
    Project m;
    m.memoryDesign = "dimm";
    m.schematic.addCustomComponent(partId("MT40A512M16LY-062E"), "", {0, 0});
    m.schematicChanged();
    {
        const auto c = codes(m);
        CHECK(c.count("REL_SPD") && c.count("REL_DIMM_THICKNESS"));
    }
    m.pcb.settings.thickness = 1.27;
    m.schematic.addCustomComponent(partId("AT24CS02-SSHM-T"), "", {300, 0});
    m.schematicChanged();
    {
        const auto c = codes(m);
        CHECK(!c.count("REL_SPD") && c.count("REL_SPD_PULLUPS") && !c.count("REL_DIMM_THICKNESS"));
    }

    // The C API: set / reject a type and read the segment report.
    SiedaProject* api = sieda_project_new("mem");
    CHECK(sieda_set_memory_design(api, "rdimm") == 1);
    CHECK(sieda_set_memory_design(api, "nope") == 0);
    char* json = sieda_memory_segments_json(api);
    const Json report = Json::parse(json);
    CHECK(report.get("platform").asString("") == "rdimm" && report.get("applies").asBool());
    CHECK(report.get("platforms").size() == 5 && report.get("segments").size() == 5);
    sieda_string_free(json);
    CHECK(sieda_set_memory_design(api, "") == 1);
    sieda_project_free(api);
}

namespace {
/// DDR3L / FPGA / generic routing test boards: HDI rules, the given planes, auto-placed and fitted.
int routeErrors(Project& p, int layers, std::initializer_list<std::pair<const char*, int>> planes, RouteStats& st) {
    p.pcb.settings.applyPreset("HDI / Fine-Pitch BGA (IPC-2226)");
    p.pcb.settings.width = 70;
    p.pcb.settings.height = 50;
    p.pcb.settings.layerCount = layers;
    for (const auto& [net, layer] : planes) p.pcb.zones.push_back({net, layer, true, 0});
    p.pcb.autoPlace(p.schematic, true);
    p.pcb.fitBoardToComponents(p.schematic, 2.5);
    st = p.pcb.autoRoute(p.schematic);
    int errors = 0;
    for (const auto& v : p.pcb.runDRC(p.schematic)) errors += v.severity == Severity::Error;
    return errors;
}
}  // namespace

TEST(bga_fanout_fpga_and_ddr3_memory_down) {
    // An Artix-7 (324-ball, 0.8 mm) talking to a DDR3L x16 (96-ball) over 50 lines, on 8 layers with GND / 1.0 V /
    // 1.35 V planes: every ball fans out (dogbone) and the bus routes completely with no DRC error.
    auto norm = [](std::string s) {
        std::string o;
        for (char c : s)
            if (c != '{' && c != '}') o += c;
        if (o.size() > 2 && o[0] == 'V' && o[1] == '_') o.erase(1, 1);
        return o;
    };
    Project p;
    auto& sch = p.schematic;
    int f = sch.addCustomComponent(p.addCustomPart(findStandardPart("XC7A35T-1CSG324I")->spec), "", {0, 0});
    int m = sch.addCustomComponent(p.addCustomPart(findStandardPart("MT41K256M16HA-125")->spec), "", {600, 0});
    int g = sch.addComponent(ComponentKind::Ground, "", {0, 400});
    int v33 = sch.addComponent(ComponentKind::NetLabel, "+3V3", {0, -400});
    int v1 = sch.addComponent(ComponentKind::NetLabel, "+1V0", {100, -400});
    int v135 = sch.addComponent(ComponentKind::NetLabel, "+1V35", {200, -400});
    const auto& fpins = sch.find(f)->def().pins;
    const auto& mpins = sch.find(m)->def().pins;
    std::vector<int> io;
    for (int i = 0; i < static_cast<int>(fpins.size()); ++i) {
        const std::string& n = fpins[static_cast<size_t>(i)].name;
        if (n.rfind("IO_", 0) == 0) io.push_back(i);
        else if (n == "GND" || n == "GNDADC_0") sch.connect({f, i}, {g, 0});
        else if (n == "VCCO_34" || n == "VCCO_35") sch.connect({f, i}, {v135, 0});
        else if (n.rfind("VCCO", 0) == 0 || n == "VCCADC_0" || n == "VCCBATT_0" || n == "VCCAUX") sch.connect({f, i}, {v33, 0});
        else if (n == "VCCINT" || n == "VCCBRAM") sch.connect({f, i}, {v1, 0});
    }
    int k = 0;
    for (int i = 0; i < static_cast<int>(mpins.size()); ++i) {
        const std::string n = norm(mpins[static_cast<size_t>(i)].name);
        if (n == "VSS" || n == "VSSQ") sch.connect({m, i}, {g, 0});
        else if (n == "VDD" || n == "VDDQ") sch.connect({m, i}, {v135, 0});
        else if (n != "NC" && n.rfind("VREF", 0) != 0) sch.connect({m, i}, {f, io[static_cast<size_t>(k++)]});
    }
    CHECK(k == 50);
    RouteStats st;
    CHECK(routeErrors(p, 8, {{"GND", 1}, {"+1V0", 5}, {"+1V35", 6}}, st) == 0);
    CHECK(st.failed == 0 && st.routed == st.connections && st.connections > 150);
    // Every signal ball has its fan-out via in the gap between four balls (not in the pad: VIPPO is off).
    int fanned = 0;
    for (const auto& pad : p.pcb.pads(sch)) {
        if (pad.componentId != m || pad.net < 0) continue;
        for (const auto& v : p.pcb.vias)
            if (v.net == pad.net && std::fabs((v.position - pad.position).length() - 0.8 / std::sqrt(2.0)) < 0.01) {
                ++fanned;
                break;
            }
    }
    CHECK(fanned > 60);
    CHECK(p.pcb.settings.routingGrid == 0.25);  // the finer BGA grid is only used while routing
}

TEST(every_package_type_autoroutes) {
    // One library part of every package type (DIP, SOIC, TSSOP, QFN, LQFP, SON, SOT-23, SOT-223, TO-220, TO-263,
    // Multiwatt, LGA, BGA, modules, headers, crystals, discs): its pins wired to resistors, auto-routed on 4 layers
    // with a GND plane — every connection routes and DRC is clean.
    std::map<std::string, const StandardPart*> byType;
    for (const auto& sp : standardParts()) byType.emplace(sp.spec.package.type, &sp);
    CHECK(byType.size() >= 20);
    for (const auto& [type, sp] : byType) {
        Project p;
        auto& sch = p.schematic;
        int u = sch.addCustomComponent(p.addCustomPart(sp->spec), "", {0, 0});
        int g = sch.addComponent(ComponentKind::Ground, "", {0, 400});
        const auto& pins = sch.find(u)->def().pins;
        int sig = 0;
        for (int i = 0; i < static_cast<int>(pins.size()) && sig < 16; ++i) {
            if (pins[static_cast<size_t>(i)].type == static_cast<int>(PinType::NoConnect)) continue;
            int r = sch.addComponent(ComponentKind::Resistor, "1k", {300.0 + 60 * (sig % 8), 60.0 * (sig / 8)});
            sch.connect({u, i}, {r, 0});
            sch.connect({r, 1}, {g, 0});
            ++sig;
        }
        RouteStats st;
        const int errors = routeErrors(p, 4, {{"GND", 1}}, st);
        if (errors != 0 || st.failed != 0)
            std::printf("    %s (%s): %d/%d routed, %d DRC errors\n", type.c_str(), sp->spec.name.c_str(), st.routed,
                        st.connections, errors);
        CHECK(errors == 0 && st.failed == 0);
    }
}

TEST(library_footprints_keep_pads_apart) {
    // No footprint puts copper of two different pins closer than 0.1 mm (an exposed pad or a corner pad touching a
    // pin pad would short them on the board).
    for (const auto& sp : standardParts()) {
        auto part = CustomPartRegistry::instance().registerPart(sp.spec);
        const auto& pads = part->footprint.pads;
        double worst = 1e9;
        for (size_t i = 0; i < pads.size(); ++i)
            for (size_t j = i + 1; j < pads.size(); ++j) {
                if (pads[i].pinIndex >= 0 && pads[i].pinIndex == pads[j].pinIndex) continue;
                const Rect a = Rect::centered(pads[i].offset, pads[i].size.x, pads[i].size.y);
                const Rect b = Rect::centered(pads[j].offset, pads[j].size.x, pads[j].size.y);
                double gap = std::hypot(std::max({0.0, b.x0 - a.x1, a.x0 - b.x1}), std::max({0.0, b.y0 - a.y1, a.y0 - b.y1}));
                if (pads[i].round && pads[j].round)
                    gap = std::max(0.0, (pads[i].offset - pads[j].offset).length() - pads[i].size.x / 2 - pads[j].size.x / 2);
                worst = std::min(worst, gap);
            }
        if (worst < 0.099) std::printf("    %s: pads %.3f mm apart\n", sp.spec.name.c_str(), worst);
        CHECK(worst >= 0.099);
    }
}

TEST(general_purpose_catalog) {
    // The general-purpose catalog (tools/fetch_catalog_parts.py from the KiCad library): MCUs, op-amps, regulators,
    // interfaces, logic, drivers, memories, sensors, transistors and protection parts, each on its orderable package
    // and registering with every pad on a pin.
    const char* names[] = {
        "STM32F030C8T6", "STM32F103RBT6", "STM32F303CCT6", "STM32F411RET6", "STM32F429ZIT6", "STM32G031K8T6",
        "STM32L072CZT6", "STM32L476RGT6", "STM32WB55CGU6", "STM32C011F6P6", "STM32G071KBT6N", "ESP32-C3", "ESP32-S3",
        "ESP32-PICO-D4", "ATTINY202-SSN", "ATTINY404-SSN", "ATTINY3216-SN", "ATMEGA328PB-AU", "ATMEGA1284P-AU",
        "ATSAMD21J18A-AU", "ATSAMD11C14A-SSUT", "ATTINY84A-SSU", "RP2350B", "LM324DR", "LM324N", "TL072CDR",
        "TL074CDR", "NE5532DR", "MCP6004T-I/SL", "OP07CDR", "TLV9062IDR", "LM339DR", "LM311DR", "AD620ARZ",
        "INA128UA", "MCP6002T-I/SN", "LMV321IDBVR", "LMV358IDR", "OPA2340UA", "OPA2134UA", "LM386MX-1",
        "PCM5102APWR", "PAM8403DR", "LM7905", "LM1117S-3.3", "MCP1700T-3302E/TT", "LP5907MFX-3.3", "MIC5219-3.3YM5",
        "AP7361C-33E-13", "TLV75533PDBVR", "TPS5430DDAR", "MC34063ADR", "TPS563200DDCR", "MT3608", "LM2675M-5.0",
        "AP63203WU-7", "LM2596S-3.3", "TL431AIDBZR", "LM4040AIM3-2.5", "REF3033AIDBZR", "LM1117MPX-3.3",
        "AMS1117-5.0", "L78L05ACD13TR", "TPS54302DDCR", "TPS54360DDAR", "MCP73831T-2ACI/OT", "TPS3839G33DBZR",
        "MCP130T-315I/TT", "BQ21040DBVR", "USBLC6-4SC6", "FT232RL", "FT231XS", "CH340C", "MAX232DR", "MCP2562-E/SN",
        "SN65HVD231DR", "TCA9555PWR", "ENC28J60-I/SO", "THVD1400DR", "MCP23017-E/SO", "MCP23S17-E/SO",
        "MCP23008-E/SO", "CH9102F", "CP2104-F03-GMR", "STUSB4500QTR", "MCP2021A-500E/SN", "W5100S-Q", "LAN8742A-CZ",
        "DP83848IVV", "ISO7720DR", "ADUM1200ARZ", "74HC14D", "74HC04D", "74HC165D", "74HC245DW", "74HC4051D",
        "74HC138D", "CD4051BM96", "74HC164D", "74HC02D", "74AHCT125D", "TLC5940PWP", "TC4427AEOA", "ULN2803ADWR",
        "MAX7219CWG+", "MAX7219CNG+", "DRV8870DDAR", "A4950ELJTR-T", "W25Q32JVSSIQ", "AT24C02C-SSHM-T",
        "25LC256-I/SN", "MCP9808-E/MS", "TMP36GSZ", "LIS3DHTR", "BME680", "MCP3008-I/SL", "ADS1015IDGSR", "HX711",
        "MCP4921-E/SN", "LM75BD", "DS18B20Z+", "ADXL343BCCZ", "LSM6DS3TR-C", "LSM6DSLTR", "MPU-6000", "LPS22HHTR",
        "LPS25HBTR", "SHTC3", "DRV5033FAQDBZR", "ACS712ELCTR-05B-T", "ADS1013IDGSR", "AO3401A", "2N7002", "BSS84",
        "IRLML6402TRPBF", "IRLB8721PBF", "IRF9540NPBF", "MMBT3904", "MMBT3906", "BC817-40", "BC807-40", "TIP120",
        "IRLML6244TRPBF", "IRLML0030TRPBF", "DMG3402L-7", "DMG2301L-7", "SI2319CDS-T1-GE3", "DS3231MZ+", "DS1307Z+",
        "PCF8563T", "LMC555CMX", "TPD2E2U06DCKR",
    };
    CHECK(sizeof names / sizeof names[0] == 152);
    std::set<std::string> unique;
    for (const auto& sp : standardParts()) CHECK(unique.insert(sp.spec.name).second);  // no name twice
    CHECK(standardParts().size() >= 420);
    for (const char* name : names) {
        const StandardPart* sp = findStandardPart(name);
        CHECK(sp != nullptr);
        if (!sp) {
            std::printf("    missing %s\n", name);
            continue;
        }
        bool ok = true;
        try {
            auto part = CustomPartRegistry::instance().registerPart(sp->spec);
            ok = part->footprint.pads.size() >= sp->spec.pins.size() && !sp->spec.manufacturer.empty() &&
                 !sp->spec.description.empty();
            for (const auto& pad : part->footprint.pads) ok = ok && pad.pinIndex >= 0;
        } catch (const std::exception& e) {
            std::printf("    %s: %s\n", name, e.what());
            ok = false;
        }
        CHECK(ok);
    }
    // Datasheet pin names and packages.
    auto pin = [](const char* part, size_t i) { return findStandardPart(part)->spec.pins[i].name; };
    CHECK(pin("LM324DR", 3) == "V+" && pin("LM324DR", 10) == "V-");
    CHECK(pin("2N7002", 0) == "G" && pin("2N7002", 1) == "S" && pin("2N7002", 2) == "D");
    CHECK(pin("FT232RL", 0) == "TXD" && findStandardPart("FT232RL")->spec.package.pinCount == 28);
    CHECK(pin("TL431AIDBZR", 1) == "REF");
    CHECK(pin("AMS1117-5.0", 1) == "VO" && findStandardPart("AMS1117-5.0")->spec.package.type == "SOT223");
    CHECK(findStandardPart("ESP32-C3")->spec.pins.size() == 33 && findStandardPart("ESP32-C3")->spec.pins.back().number == "EP");
    CHECK(findStandardPart("STM32F429ZIT6")->spec.package.pinCount == 144);
    CHECK(findStandardPart("LIS3DHTR")->spec.package.type == "LGA");                 // exact KiCad land pattern
    auto hsop = CustomPartRegistry::instance().registerPart(findStandardPart("TPS5430DDAR")->spec);
    CHECK(hsop->footprint.pads.size() == 9 && hsop->def.pins[hsop->footprint.pads.back().pinIndex].name == "GNDPAD");
}

TEST(collinear_segments_are_not_a_crossing) {
    // Two dogbone stubs on one 45° diagonal, 0.57 mm apart: with fused multiply-add the orientation products come out
    // as tiny values of either sign, which used to read as a crossing (a false DRC short on Apple silicon).
    const Vec2 a{16.75, 16.45}, b{17.15, 16.05}, c{15.95, 17.25}, d{16.35, 16.85};
    CHECK(!segmentsIntersect(a, b, c, d));
    CHECK(std::fabs(segmentSegmentDistance(a, b, c, d) - (d - a).length()) < 1e-9);
    CHECK(segmentSegmentDistance(a, b, {16.95, 16.25}, {17.35, 15.85}) < 1e-9);  // overlapping collinear: touching
    CHECK(segmentsIntersect({0, 0}, {1, 1}, {0, 1}, {1, 0}));                     // a real crossing still is one
}

// ======================================================================= multi-sheet schematics, variants, buses

extern "C" int sieda_c_api_sheets_test(void);

namespace {
const Net* netNamed(const Schematic& s, const std::string& name) {
    for (const auto& n : s.nets())
        if (n.name == name) return &n;
    return nullptr;
}

/// Two sheets: "Power" holds a 5 V source driving global label VIN; "Load" holds R1 (1 kΩ) from VIN to ground.
Project twoSheetProject() {
    Project p;
    p.name = "Two sheets";
    auto& s = p.schematic;
    s.renameSheet(1, "Power");
    int load = s.addSheet("Load");
    int v = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    int g1 = s.addComponent(ComponentKind::Ground, "", {0, 80});
    int l1 = s.addComponent(ComponentKind::NetLabel, "VIN", {60, -40});
    wire(s, v, "+", l1, "N");
    wire(s, v, "-", g1, "GND");
    s.setActiveSheet(load);
    int r = s.addComponent(ComponentKind::Resistor, "1k", {80, 0});
    int l2 = s.addComponent(ComponentKind::NetLabel, "VIN", {0, 0});
    int g2 = s.addComponent(ComponentKind::Ground, "", {160, 80});
    wire(s, l2, "N", r, "1");
    wire(s, r, "2", g2, "GND");
    p.schematicChanged();
    return p;
}
}  // namespace

TEST(sheets_add_rename_parent_remove) {
    Schematic s;
    CHECK(s.sheets().size() == 1);
    CHECK(s.sheets()[0].name == "Main");
    int a = s.addSheet("  Analog  ");
    CHECK(a > 1);
    CHECK(s.findSheet(a)->name == "Analog");  // trimmed
    CHECK(s.addSheet("Analog") == -1);         // names are unique
    CHECK(s.addSheet("") == -1);
    CHECK(s.addSheet("Orphan", 999) == -1);    // unknown parent
    int child = s.addSheet("Filter", a);
    CHECK(child > a);
    CHECK(s.sheetDepth(child) == 1);
    CHECK(!s.setSheetParent(a, child));  // cycle
    CHECK(!s.renameSheet(child, "Main"));
    CHECK(s.renameSheet(child, "Anti-alias"));
    CHECK(s.reorderSheet(child, 0));
    CHECK(s.sheets()[0].id == child);

    // Placement follows the active sheet; wires never cross sheets.
    CHECK(s.setActiveSheet(a));
    CHECK(!s.setActiveSheet(12345));
    int r1 = s.addComponent(ComponentKind::Resistor, "1k", {0, 0});
    CHECK(s.find(r1)->sheet == a);
    s.setActiveSheet(1);
    int r2 = s.addComponent(ComponentKind::Resistor, "1k", {0, 0});
    CHECK(s.connect({r1, 0}, {r2, 0}) == -1);

    // A sheet with parts is removed only with its contents; its children move up.
    CHECK(!s.removeSheet(a, false));
    CHECK(s.removeSheet(a, true));
    CHECK(s.find(r1) == nullptr);
    CHECK(s.findSheet(child)->parent == 0);
    CHECK(s.removeSheet(child, false));  // empty
    CHECK(!s.removeSheet(1, true));      // the last sheet stays
    CHECK(s.activeSheet() == 1);
    s.clear();
    CHECK(s.sheets().size() == 1 && s.activeSheet() == 1);
}

TEST(sheets_global_labels_flatten_for_netlist_simulation_and_pcb) {
    Project p = twoSheetProject();
    const auto& s = p.schematic;
    const Component* r = s.findByRef("R1");
    CHECK(r && r->sheet != 1);
    // One VIN net spanning both sheets, one ground.
    const Net* vin = netNamed(s, "VIN");
    CHECK(vin != nullptr);
    if (vin) CHECK(vin->pins.size() == 4);  // source +, both labels, resistor pin 1
    CHECK(s.netOf({r->id, 0}) == s.netOf({s.findByRef("V1")->id, 0}));
    CHECK(s.netOf({r->id, 1}) == s.groundNet());
    Simulator sim(s);
    DcResult dc = sim.dcOperatingPoint();
    CHECK(dc.converged);
    CHECK_NEAR(netV(s, dc, r->id, "1"), 5.0, 1e-6);
    std::string spice = exportSpiceNetlist(s, "two sheets");
    CHECK(spice.find("R1") != std::string::npos);
    CHECK(spice.find("VIN") != std::string::npos);
    auto erc = s.runERC();
    CHECK(!hasCode(erc, "ERC_CROSS_SHEET_WIRE"));
    CHECK(!hasCode(erc, "ERC_DANGLING_LABEL"));
    // The board sees the flattened design: placed and routed with no unrouted connection.
    p.pcb.autoPlace(p.schematic, true);
    RouteStats st = p.pcb.autoRoute(p.schematic);
    CHECK(st.failed == 0);
    CHECK(st.connections > 0);
}

TEST(sheets_local_labels_stay_on_their_sheet) {
    Schematic s;
    int b = s.addSheet("Sensor");
    int r1 = s.addComponent(ComponentKind::Resistor, "1k", {0, 0});
    int la = s.addComponent(ComponentKind::NetLabel, "EN", {-60, 0});
    CHECK(s.setLabelScope(la, LabelScope::Local));
    wire(s, la, "N", r1, "1");
    s.setActiveSheet(b);
    int r2 = s.addComponent(ComponentKind::Resistor, "1k", {0, 0});
    int lb = s.addComponent(ComponentKind::NetLabel, "EN", {-60, 0});
    CHECK(s.setLabelScope(lb, LabelScope::Local));
    wire(s, lb, "N", r2, "1");
    int lb2 = s.addComponent(ComponentKind::NetLabel, "EN", {-60, 60});
    CHECK(s.setLabelScope(lb2, LabelScope::Local));
    int r3 = s.addComponent(ComponentKind::Resistor, "1k", {0, 60});
    wire(s, lb2, "N", r3, "1");
    // Same name on two sheets: two nets, qualified by sheet name; labels on one sheet join.
    CHECK(s.netOf({r1, 0}) != s.netOf({r2, 0}));
    CHECK(s.netOf({r2, 0}) == s.netOf({r3, 0}));
    CHECK(netNamed(s, "Main/EN") != nullptr);
    CHECK(netNamed(s, "Sensor/EN") != nullptr);
    CHECK(hasCode(s.runERC(), "ERC_LOCAL_LABEL_SPLIT"));
    // A local name used once keeps its plain name.
    s.setValue(la, "RESET");
    CHECK(netNamed(s, "RESET") != nullptr);
    CHECK(netNamed(s, "EN") != nullptr);
    CHECK(!hasCode(s.runERC(), "ERC_LOCAL_LABEL_SPLIT"));
    // A global label of the same name is a different net and keeps its name; the local one is qualified.
    int g = s.addComponent(ComponentKind::NetLabel, "EN", {-60, 120});
    int r4 = s.addComponent(ComponentKind::Resistor, "1k", {0, 120});
    wire(s, g, "N", r4, "1");
    CHECK(s.netOf({r4, 0}) != s.netOf({r2, 0}));
    CHECK(s.nets()[static_cast<size_t>(s.netOf({r4, 0}))].name == "EN");
    CHECK(s.nets()[static_cast<size_t>(s.netOf({r2, 0}))].name == "Sensor/EN");
    // Scope rules: only labels; an entry needs a real other sheet.
    CHECK(!s.setLabelScope(r1, LabelScope::Local));
    CHECK(!s.setLabelScope(lb, LabelScope::SheetEntry, b));
    CHECK(!s.setLabelScope(lb, LabelScope::SheetEntry, 77));
}

TEST(sheets_hierarchical_ports_and_sheet_entries) {
    Schematic s;
    int child = s.addSheet("Divider", 1);
    // Child: port IN → R1 → port OUT; R2 from OUT to ground.
    s.setActiveSheet(child);
    int r1 = s.addComponent(ComponentKind::Resistor, "10k", {80, 0});
    int r2 = s.addComponent(ComponentKind::Resistor, "10k", {160, 60}, 90);
    int pin = s.addComponent(ComponentKind::NetLabel, "IN", {0, 0});
    int pout = s.addComponent(ComponentKind::NetLabel, "OUT", {240, 0});
    int gc = s.addComponent(ComponentKind::Ground, "", {160, 140});
    CHECK(s.setLabelScope(pin, LabelScope::Port));
    CHECK(s.setLabelScope(pout, LabelScope::Port));
    wire(s, pin, "N", r1, "1");
    wire(s, r1, "2", pout, "N");
    wire(s, pout, "N", r2, "1");
    wire(s, r2, "2", gc, "GND");
    CHECK((s.sheetPorts(child) == std::vector<std::string>{"IN", "OUT"}));
    auto erc = s.runERC();
    CHECK(hasCode(erc, "ERC_PORT_UNUSED"));  // no sheet symbol yet

    // Parent: the sheet symbol's entries, a 10 V source on IN and a load on OUT.
    CHECK(s.placeSheetEntries(child, {100, 0}) == 2);
    CHECK(s.placeSheetEntries(child, {100, 0}) == 0);  // already there
    CHECK(s.placeSheetEntries(1, {0, 0}) == -1);       // top-level sheet has no symbol
    int entryIn = -1, entryOut = -1;
    for (const auto& c : s.components())
        if (c.kind == ComponentKind::NetLabel && c.scope == LabelScope::SheetEntry) {
            CHECK(c.sheet == 1);
            CHECK(c.targetSheet == child);
            (c.value == "IN" ? entryIn : entryOut) = c.id;
        }
    CHECK(entryIn > 0 && entryOut > 0);
    CHECK(s.activeSheet() == child);  // placing entries does not switch sheets
    s.setActiveSheet(1);
    int v = s.addComponent(ComponentKind::VoltageSource, "10", {0, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    int rl = s.addComponent(ComponentKind::Resistor, "1meg", {200, 20});
    wire(s, v, "+", entryIn, "N");
    wire(s, v, "-", g, "GND");
    wire(s, entryOut, "N", rl, "1");
    wire(s, rl, "2", g, "GND");
    CHECK(s.netOf({v, 0}) == s.netOf({r1, 0}));
    CHECK(s.netOf({rl, 0}) == s.netOf({r2, 0}));
    CHECK(netNamed(s, "IN") != nullptr);
    erc = s.runERC();
    CHECK(!hasCode(erc, "ERC_PORT_UNUSED"));
    CHECK(!hasCode(erc, "ERC_SHEET_ENTRY_NO_PORT"));
    Simulator sim(s);
    DcResult dc = sim.dcOperatingPoint();
    CHECK(dc.converged);
    CHECK_NEAR(netV(s, dc, rl, "1"), 4.975, 0.01);  // 10 V halved, loaded by 1 MΩ
    // ERC locations carry their sheet.
    for (const auto& violation : erc)
        if (!violation.components.empty()) CHECK(violation.sheet == s.find(violation.components.front())->sheet);

    // An entry without its port is an error; a port on a top-level sheet is flagged.
    s.setValue(pout, "VOUT");
    erc = s.runERC();
    CHECK(hasCode(erc, "ERC_SHEET_ENTRY_NO_PORT"));
    CHECK(hasCode(erc, "ERC_PORT_UNUSED"));
    CHECK(s.netOf({rl, 0}) != s.netOf({r2, 0}));
    CHECK(s.setSheetParent(child, 0));
    CHECK(hasCode(s.runERC(), "ERC_PORT_NO_PARENT"));
    // Removing the child sheet removes the entries that pointed into it.
    CHECK(s.removeSheet(child, true));
    CHECK(s.find(entryIn) == nullptr && s.find(entryOut) == nullptr);
    CHECK(s.find(v) != nullptr);
}

TEST(sheets_move_components_between_sheets) {
    Schematic s;
    int other = s.addSheet("Other");
    int r1 = s.addComponent(ComponentKind::Resistor, "1k", {0, 0});
    int r2 = s.addComponent(ComponentKind::Resistor, "1k", {100, 0});
    int r3 = s.addComponent(ComponentKind::Resistor, "1k", {200, 0});
    int w = s.connect({r1, 1}, {r2, 0});
    CHECK(w >= 0);
    int j = s.splitWire(w, {50, 0});  // a bend joining only r1 and r2
    CHECK(j > 0);
    CHECK(s.find(j)->sheet == 1);
    wire(s, r2, "2", r3, "1");
    CHECK(s.moveToSheet({r1, r2}, other) == 3);  // r1, r2 and their junction
    CHECK(s.find(j)->sheet == other);
    CHECK(s.netOf({r1, 1}) == s.netOf({r2, 0}));  // still joined on the new sheet
    CHECK(s.netOf({r2, 1}) != s.netOf({r3, 0}));  // the wire to r3 would have crossed sheets
    for (const auto& wr : s.wires()) CHECK(s.find(wr.a.component)->sheet == s.find(wr.b.component)->sheet);
    CHECK(s.moveToSheet({r1}, 999) == 0);
}

TEST(sheets_persist_round_trip_and_old_files_load_as_one_sheet) {
    Project p = twoSheetProject();
    int child = p.schematic.addSheet("Child", 1);
    p.schematic.setActiveSheet(child);
    int port = p.schematic.addComponent(ComponentKind::NetLabel, "SDA", {0, 0});
    p.schematic.setLabelScope(port, LabelScope::Port);
    p.schematic.placeSheetEntries(child, {300, 0});
    int local = p.schematic.addComponent(ComponentKind::NetLabel, "X", {0, 40});
    p.schematic.setLabelScope(local, LabelScope::Local);
    Json saved = p.toJson();
    Project q = Project::fromJson(Json::parse(saved.dump()));
    CHECK(q.schematic.sheets().size() == 3);
    CHECK(q.schematic.activeSheet() == child);
    for (size_t i = 0; i < p.schematic.sheets().size(); ++i) {
        CHECK(q.schematic.sheets()[i].id == p.schematic.sheets()[i].id);
        CHECK(q.schematic.sheets()[i].name == p.schematic.sheets()[i].name);
        CHECK(q.schematic.sheets()[i].parent == p.schematic.sheets()[i].parent);
    }
    for (const auto& c : p.schematic.components()) {
        const Component* r = q.schematic.find(c.id);
        CHECK(r != nullptr);
        if (!r) continue;
        CHECK(r->sheet == c.sheet);
        CHECK(r->scope == c.scope);
        CHECK(r->targetSheet == c.targetSheet);
    }
    CHECK(q.schematic.nets().size() == p.schematic.nets().size());
    for (size_t i = 0; i < p.schematic.nets().size(); ++i) CHECK(q.schematic.nets()[i].name == p.schematic.nets()[i].name);
    CHECK(q.toJson().dump() == saved.dump());
    // A new sheet after reload gets a fresh id.
    CHECK(q.schematic.addSheet("Later") > child);

    // A file written before sheets existed: no "sheets", no "sheet" on components.
    const char* old = R"({"format":"sieda-project","version":1,"name":"Old","board":{},
        "components":[{"id":1,"kind":5,"ref":"V1","value":"5","x":0,"y":0,"rotation":0,"pcb":{}},
                      {"id":2,"kind":0,"ref":"R1","value":"1k","x":80,"y":0,"rotation":0,"pcb":{}},
                      {"id":3,"kind":15,"ref":"#NL1","value":"VIN","x":40,"y":0,"rotation":0,"pcb":{}},
                      {"id":4,"kind":7,"ref":"#GND1","value":"","x":0,"y":80,"rotation":0,"pcb":{}}],
        "wires":[{"id":1,"a":{"component":1,"pin":0},"b":{"component":3,"pin":0}},
                 {"id":2,"a":{"component":3,"pin":0},"b":{"component":2,"pin":0}},
                 {"id":3,"a":{"component":2,"pin":1},"b":{"component":4,"pin":0}},
                 {"id":4,"a":{"component":1,"pin":1},"b":{"component":4,"pin":0}}]})";
    Project o = Project::fromJson(Json::parse(old));
    CHECK(o.schematic.sheets().size() == 1);
    CHECK(o.schematic.activeSheet() == o.schematic.sheets()[0].id);
    for (const auto& c : o.schematic.components()) {
        CHECK(c.sheet == o.schematic.sheets()[0].id);
        CHECK(c.scope == LabelScope::Global);
    }
    CHECK(o.schematic.wires().size() == 4);
    CHECK(netNamed(o.schematic, "VIN") != nullptr);
    CHECK(o.variants.empty() && o.activeVariant.empty());
    // A single-sheet design saves no per-component sheet field: its components look as before.
    std::string again = o.toJson().dump();
    CHECK(again.find("\"sheet\":") == std::string::npos);
    CHECK(again.find("\"scope\"") == std::string::npos);

    // Hand-edited files: components on unknown sheets go to the first sheet, entries into nothing become local.
    Json bad = Json::parse(old);
    bad["sheets"] = Json::parse(R"([{"id":5,"name":"A","parent":6},{"id":6,"name":"B","parent":5},{"id":0,"name":"Z"}])");
    Project b = Project::fromJson(bad);
    CHECK(b.schematic.sheets().size() == 2);
    CHECK(b.schematic.sheetDepth(5) + b.schematic.sheetDepth(6) <= 1);  // the cycle is broken
    for (const auto& c : b.schematic.components()) CHECK(c.sheet == 5);
}

TEST(sheets_cross_sheet_erc) {
    Project p = twoSheetProject();
    auto& s = p.schematic;
    // A signal global label only on one sheet is reported (info); supply-like names are not.
    int r = s.addComponent(ComponentKind::Resistor, "1k", {300, 0});
    int a = s.addComponent(ComponentKind::NetLabel, "SENSE", {240, 0});
    int b = s.addComponent(ComponentKind::NetLabel, "SENSE", {380, 0});
    int r2 = s.addComponent(ComponentKind::Resistor, "1k", {440, 0});
    wire(s, a, "N", r, "1");
    wire(s, b, "N", r2, "1");
    auto erc = s.runERC();
    CHECK(hasCode(erc, "ERC_GLOBAL_LABEL_ONE_SHEET"));
    for (const auto& v : erc)
        if (v.code == "ERC_GLOBAL_LABEL_ONE_SHEET") CHECK(v.message.find("SENSE") != std::string::npos);
    // A label in bus notation joins one net only: warned.
    s.setValue(a, "D[0..7]");
    CHECK(hasCode(s.runERC(), "ERC_BUS_LABEL"));
    // Single-sheet designs never see the multi-sheet rules.
    Project led = ledProject();
    for (const auto& v : led.schematic.runERC()) {
        CHECK(v.code != "ERC_GLOBAL_LABEL_ONE_SHEET");
        CHECK(v.sheet == 1 || v.components.empty());
    }
}

TEST(sheets_output_conflict_names_both_sheets) {
    Project p;
    std::string id = p.addCustomPart(customPartSpecFromJson(Json::parse(
        R"({"name":"BUF","package":{"type":"SOT23","pinCount":3},"pins":[{"number":"1","name":"VDD","type":"power_in"},
            {"number":"2","name":"GND","type":"power_in"},{"number":"3","name":"Y","type":"output"}]})")));
    auto& s = p.schematic;
    int second = s.addSheet("Second");
    int u1 = s.addCustomComponent(id, "", {0, 0});
    int l1 = s.addComponent(ComponentKind::NetLabel, "BUS", {80, 0});
    s.connect({u1, 2}, {l1, 0});
    s.setActiveSheet(second);
    int u2 = s.addCustomComponent(id, "", {0, 0});
    int l2 = s.addComponent(ComponentKind::NetLabel, "BUS", {80, 0});
    s.connect({u2, 2}, {l2, 0});
    bool found = false;
    for (const auto& v : s.runERC())
        if (v.code == "ERC_OUTPUT_CONFLICT") {
            found = true;
            CHECK(v.message.find("across sheets Main, Second") != std::string::npos);
        }
    CHECK(found);
}

TEST(bus_notation_expands_and_bus_labels_wire_pins) {
    CHECK((expandBus("D[0..3]") == std::vector<std::string>{"D0", "D1", "D2", "D3"}));
    CHECK((expandBus("A[3..1]") == std::vector<std::string>{"A3", "A2", "A1"}));
    CHECK((expandBus("D[0..1]_N, WR") == std::vector<std::string>{"D0_N", "D1_N", "WR"}));
    CHECK((expandBus("SDA,SCL") == std::vector<std::string>{"SDA", "SCL"}));
    CHECK(expandBus("VCC").empty());
    CHECK(expandBus("D[0..]").empty());
    CHECK(expandBus("[0..3]").empty());
    CHECK(expandBus("D[0..3").empty());
    CHECK(expandBus("D[0..5000]").empty());
    CHECK(expandBus("A,,B").empty());

    Schematic s;
    int j1 = s.addComponent(ComponentKind::Connector, "", {0, 0});
    const int pins = static_cast<int>(s.find(j1)->def().pins.size());
    CHECK(pins >= 2);
    std::vector<int> list;
    for (int i = 0; i < pins; ++i) list.push_back(i);
    std::string bus = "D[0.." + std::to_string(pins - 1) + "]";
    CHECK(s.addBusLabels(j1, list, "D[0..99]") == -1);  // count mismatch
    CHECK(s.addBusLabels(j1, {0, 99}, "D[0..1]") == -1);
    CHECK(s.addBusLabels(j1, list, bus, LabelScope::Local) == pins);
    for (int i = 0; i < pins; ++i) {
        const int net = s.netOf({j1, i});
        CHECK(net >= 0);
        if (net >= 0) CHECK(s.nets()[static_cast<size_t>(net)].name == "D" + std::to_string(i));
        CHECK(s.isPinConnected({j1, i}));
    }
    int labels = 0;
    for (const auto& c : s.components())
        if (c.kind == ComponentKind::NetLabel) {
            ++labels;
            CHECK(c.scope == LabelScope::Local);
            CHECK(c.sheet == s.find(j1)->sheet);
        }
    CHECK(labels == pins);
}

TEST(annotation_renumbers_by_sheet_and_position) {
    Schematic s;
    int two = s.addSheet("Two");
    int ra = s.addComponent(ComponentKind::Resistor, "1k", {200, 0}, 0, "R7");
    int rb = s.addComponent(ComponentKind::Resistor, "1k", {0, 0}, 0, "R7");    // duplicate
    int rc = s.addComponent(ComponentKind::Resistor, "1k", {0, 100}, 0, "R?");  // unnumbered
    int c1 = s.addComponent(ComponentKind::Capacitor, "1u", {100, 0}, 0, "C9");
    int g = s.addComponent(ComponentKind::Ground, "", {0, 200});
    s.setActiveSheet(two);
    int rd = s.addComponent(ComponentKind::Resistor, "1k", {0, 0}, 0, "R1");
    const std::string groundRef = s.find(g)->ref;

    AnnotateOptions keep;
    keep.keepExisting = true;
    auto changes = s.annotate(keep);
    // Rows: R7 at x=0 is met first and kept; the second R7 and R? get free numbers.
    CHECK(s.find(rb)->ref == "R7");
    CHECK(s.find(ra)->ref != "R7");
    CHECK(s.find(rc)->ref.back() != '?');
    CHECK(s.find(c1)->ref == "C9");
    CHECK(s.find(rd)->ref == "R1");
    CHECK(changes.size() == 2);
    CHECK(!hasCode(s.runERC(), "ERC_DUPLICATE_REF"));

    auto all = s.annotate(AnnotateOptions{});
    CHECK(s.find(rb)->ref == "R1");  // sheet 1, row y=0, x=0
    CHECK(s.find(ra)->ref == "R2");  // row y=0, x=200
    CHECK(s.find(rc)->ref == "R3");  // row y=100
    CHECK(s.find(rd)->ref == "R4");  // second sheet
    CHECK(s.find(c1)->ref == "C1");
    CHECK(s.find(g)->ref == groundRef);  // net symbols keep theirs
    CHECK(!all.empty());

    AnnotateOptions columns;
    columns.byColumns = true;
    s.annotate(columns);
    CHECK(s.find(rb)->ref == "R1");  // column x=0: y=0 then y=100
    CHECK(s.find(rc)->ref == "R2");
    CHECK(s.find(ra)->ref == "R3");

    AnnotateOptions perSheet;
    perSheet.sheetNumbering = true;
    s.annotate(perSheet);
    CHECK(s.find(rb)->ref == "R101");
    CHECK(s.find(rd)->ref == "R201");
    CHECK(s.find(c1)->ref == "C101");

    // The project wrapper keeps tamper meshes on their part.
    Project p;
    int u = p.schematic.addComponent(ComponentKind::Resistor, "1k", {0, 0}, 0, "R5");
    p.pcb.tamperMeshes.push_back(TamperMesh{"R5", "MESH_A", "MESH_B", 1, 2, 2.0});
    p.annotate(AnnotateOptions{});
    CHECK(p.schematic.find(u)->ref == "R1");
    CHECK(p.pcb.tamperMeshes[0].componentRef == "R1");
}

TEST(design_variants_bom_cpl_and_persistence) {
    Project p = ledProject();
    auto& s = p.schematic;
    const int r = s.findByRef("R1")->id;
    const int d = s.findByRef("D1")->id;
    const int g = s.findByRef("#GND1") ? s.findByRef("#GND1")->id : -1;
    p.pcb.autoPlace(s, true);

    CHECK(p.addVariant("Lite"));
    CHECK(!p.addVariant("Lite"));
    CHECK(!p.addVariant("  "));
    CHECK(p.setVariantPart("Lite", d, 0, nullptr));            // LED not fitted
    const std::string v470 = "470";
    CHECK(p.setVariantPart("Lite", r, -1, &v470));             // resistor value override
    CHECK(!p.setVariantPart("Nope", r, 0, nullptr));
    if (g > 0) CHECK(!p.setVariantPart("Lite", g, 0, nullptr));  // net symbols are not assembled
    CHECK(p.addVariant("Pro", "Lite"));
    CHECK(p.setVariantPart("Pro", d, 1, nullptr));
    CHECK(p.renameVariant("Pro", "Full"));
    CHECK(!p.renameVariant("Full", "Lite"));

    // The base design is untouched.
    const std::string baseBom = exportBomCsv(s);
    CHECK(baseBom.find(",DNP\n", baseBom.find('\n') + 1) == std::string::npos);  // past the header
    Schematic lite = p.variantSchematic("Lite");
    CHECK(lite.find(d)->sourcing.dnp);
    CHECK(lite.find(r)->value == "470");
    CHECK(s.find(r)->value == "330");
    CHECK(lite.nets().size() == s.nets().size());  // connectivity unchanged
    {
        const std::string liteBom = exportBomCsv(lite);
        CHECK(liteBom.find(",DNP\n", liteBom.find('\n') + 1) != std::string::npos);
    }
    CHECK(exportCplCsv(lite, p.pcb).find("D1,") == std::string::npos);
    CHECK(exportCplCsv(s, p.pcb).find("D1,") != std::string::npos);
    CHECK(exportAssemblyBomCsv(lite).find("470") != std::string::npos);
    Schematic full = p.variantSchematic("Full");
    CHECK(!full.find(d)->sourcing.dnp);
    CHECK(full.find(r)->value == "470");  // copied from Lite

    // The active variant drives the fabrication package's assembly files.
    CHECK(!p.setActiveVariant("Missing"));
    CHECK(p.setActiveVariant("Lite"));
    bool sawCpl = false;
    for (const auto& f : fabricationPackage(p, "led")) {
        if (f.path == "assembly/led-cpl.csv") {
            sawCpl = true;
            CHECK(f.content.find("D1,") == std::string::npos);
            CHECK(f.description.find("Lite") != std::string::npos);
        }
        if (f.path == "fab_notes.txt") CHECK(f.content.find("Variant              Lite") != std::string::npos);
    }
    CHECK(sawCpl);

    // Round trip: variants, their parts and the active one.
    Project q = Project::fromJson(Json::parse(p.toJson().dump()));
    CHECK(q.variants.size() == 2);
    CHECK(q.activeVariant == "Lite");
    const DesignVariant* ql = q.findVariant("Lite");
    CHECK(ql && ql->parts.size() == 2);
    if (ql) {
        CHECK(ql->parts.at(d).fitted == 0);
        CHECK(ql->parts.at(r).value == "470");
    }
    CHECK(q.toJson().dump() == p.toJson().dump());
    // Snapshot marks parts not fitted in the active variant and their overridden values.
    Json snap = q.snapshot();
    bool sawLed = false;
    for (const auto& c : snap.get("components").items()) {
        if (c.get("id").asInt(-1) == d) {
            sawLed = true;
            CHECK(!c.get("fitted").asBool(true));
        }
        if (c.get("id").asInt(-1) == r) CHECK(c.get("variantValue").asString("") == "470");
    }
    CHECK(sawLed);
    CHECK(snap.get("activeVariant").asString("") == "Lite");

    // Clearing an override and removing a variant; removing the active one selects the base design.
    const std::string same = "330";
    CHECK(p.setVariantPart("Lite", r, -1, &same));  // equal to the base value: no override left
    CHECK(p.findVariant("Lite")->parts.count(r) == 0);
    CHECK(p.removeVariant("Lite"));
    CHECK(p.activeVariant.empty());
    // Parts deleted from the design drop out of variants on save.
    s.removeComponent(d);
    Json saved = p.toJson();
    CHECK(saved.dump().find("\"component\":" + std::to_string(d)) == std::string::npos);
}

TEST(sheets_and_variants_c_api) {
    int rc = sieda_c_api_sheets_test();
    if (rc != 0) std::printf("    sheets c api step %d failed\n", rc);
    CHECK(rc == 0);
}

// ======================================================================= AC, sweeps, tolerances, spectrum

namespace {
// V1 (value `source`) → R1 (r) → OUT → C1 (c) → GND. Returns the OUT net index.
int rcLowPass(Schematic& s, const char* source, const char* r, const char* c) {
    int v = s.addComponent(ComponentKind::VoltageSource, source, {0, 0});
    int rr = s.addComponent(ComponentKind::Resistor, r, {100, 0});
    int cc = s.addComponent(ComponentKind::Capacitor, c, {200, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    wire(s, v, "+", rr, "1");
    wire(s, rr, "2", cc, "1");
    wire(s, cc, "2", g, "GND");
    wire(s, v, "-", g, "GND");
    return s.netOf({rr, 1});
}
}  // namespace

TEST(source_spec_ac_stimulus) {
    auto a = SourceSpec::parse("0 AC 1");
    CHECK(a && a->kind == SourceSpec::Kind::DC && a->dc == 0 && a->hasAc && a->acMagnitude == 1 && a->acPhaseDeg == 0);
    auto b = SourceSpec::parse("SIN(2.5 1 1k) ac 2m 90");
    CHECK(b && b->kind == SourceSpec::Kind::Sine && b->hasAc);
    if (b) {
        CHECK_NEAR(b->offset, 2.5, 1e-12);
        CHECK_NEAR(b->acMagnitude, 2e-3, 1e-15);
        CHECK_NEAR(b->acPhaseDeg, 90, 1e-12);
    }
    auto c = SourceSpec::parse("AC 1");
    CHECK(c && c->dc == 0 && c->hasAc);
    auto d = SourceSpec::parse("DC 5 AC 1");
    CHECK(d && d->dc == 5 && d->hasAc);
    CHECK(!SourceSpec::parse("5 AC"));
    CHECK(!SourceSpec::parse("5 AC 1 2 3"));
    CHECK(!SourceSpec::parse("5 AC x"));
    auto plain = SourceSpec::parse("12V");  // existing values are unchanged
    CHECK(plain && plain->dc == 12 && !plain->hasAc);
    // The SPICE netlist carries the stimulus.
    Schematic s;
    rcLowPass(s, "SIN(0 1 1k) AC 1 -90", "1k", "100n");
    CHECK(exportSpiceNetlist(s, "ac").find("SIN(0 1 1000) AC 1 -90") != std::string::npos);
}

TEST(ac_rc_lowpass_corner) {
    Schematic s;
    int out = rcLowPass(s, "0 AC 1", "1k", "100n");
    const double fc = 1.0 / (2 * kPi * 1e3 * 100e-9);  // 1591.55 Hz
    Simulator sim(s);
    AcOptions o;
    o.fStart = 10;
    o.fStop = 1e6;
    o.pointsPerDecade = 20;
    AcResult r = sim.ac(o);
    CHECK(r.ok);
    if (!r.ok) return;
    CHECK(r.frequency.size() == 101);
    CHECK_NEAR(r.frequency.front(), 10, 1e-9);
    CHECK_NEAR(r.frequency.back(), 1e6, 1e-6);
    // Every point matches H = 1 / (1 + jωRC).
    double worst = 0;
    for (size_t i = 0; i < r.frequency.size(); ++i) {
        std::complex<double> h = 1.0 / std::complex<double>(1.0, r.frequency[i] / fc);
        worst = std::max(worst, std::abs(r.netPhasors[static_cast<size_t>(out)][i] - h));
    }
    CHECK(worst < 1e-8);
    const AcMetrics& m = r.metrics[out];
    // Half-power point relative to the gain at the sweep start (10 Hz): |H(f)|² = |H(10)|² / 2 → f = fc·√(1 + 2(10/fc)²).
    CHECK_NEAR(m.f3dbHz / (fc * std::sqrt(1 + 2 * (10 / fc) * (10 / fc))), 1.0, 1e-7);  // refined on the circuit
    CHECK(std::fabs(m.f3dbHz / fc - 1.0) < 0.005);                                     // the corner, within 0.5 %
    CHECK_NEAR(m.lowFreqDb, 0.0, 1e-3);
    CHECK(std::isnan(m.unityHz));
    // Interpolated readouts (no re-solve) stay within 0.5 % even at 20 points per decade.
    AcMetrics coarse = acMetrics(r.frequency, r.netPhasors[static_cast<size_t>(out)]);
    CHECK(std::fabs(coarse.f3dbHz / fc - 1.0) < 0.005);
    // Phase: −45° at the corner, approaching −90°.
    auto phase = unwrappedPhaseDeg(r.netPhasors[static_cast<size_t>(out)]);
    CHECK(phase.back() < -89 && phase.back() > -90.0001);
    CHECK(phase.front() < 0 && phase.front() > -1);
}

TEST(ac_rlc_resonance) {
    // Series RLC driven by V1: the resistor voltage is a band-pass peaking at f0 = 1/(2π√LC) with bandwidth R/(2πL);
    // the capacitor voltage peaks at f0·√(1 − 1/(2Q²)) with Q/√(1 − 1/(4Q²)).
    Schematic s;
    int v = s.addComponent(ComponentKind::VoltageSource, "AC 1", {0, 0});
    int l = s.addComponent(ComponentKind::Inductor, "10m", {100, 0});
    int c = s.addComponent(ComponentKind::Capacitor, "100n", {200, 0});
    int r = s.addComponent(ComponentKind::Resistor, "100", {300, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    wire(s, v, "+", l, "1");
    wire(s, l, "2", c, "1");
    wire(s, c, "2", r, "1");
    wire(s, r, "2", g, "GND");
    wire(s, v, "-", g, "GND");
    const double L = 10e-3, C = 100e-9, R = 100;
    const double f0 = 1.0 / (2 * kPi * std::sqrt(L * C)), bw = R / (2 * kPi * L), q = std::sqrt(L / C) / R;
    Simulator sim(s);
    AcOptions o;
    o.fStart = 100;
    o.fStop = 100e3;
    o.pointsPerDecade = 50;
    AcResult res = sim.ac(o);
    CHECK(res.ok);
    if (!res.ok) return;
    const AcMetrics& mr = res.metrics[s.netOf({r, 0})];
    CHECK_NEAR(mr.peakHz / f0, 1.0, 1e-5);
    CHECK_NEAR(mr.peakDb, 0.0, 1e-6);
    CHECK_NEAR((mr.bwHighHz - mr.bwLowHz) / bw, 1.0, 1e-4);
    CHECK_NEAR(std::sqrt(mr.bwHighHz * mr.bwLowHz) / f0, 1.0, 1e-4);  // geometric centre
    // Reordered as R–L–C to ground, the capacitor node is the classic second-order low-pass.
    Schematic s2;
    int v2 = s2.addComponent(ComponentKind::VoltageSource, "AC 1", {0, 0});
    int r2 = s2.addComponent(ComponentKind::Resistor, "100", {100, 0});
    int l2 = s2.addComponent(ComponentKind::Inductor, "10m", {200, 0});
    int c2 = s2.addComponent(ComponentKind::Capacitor, "100n", {300, 0});
    int g2 = s2.addComponent(ComponentKind::Ground, "", {0, 80});
    wire(s2, v2, "+", r2, "1");
    wire(s2, r2, "2", l2, "1");
    wire(s2, l2, "2", c2, "1");
    wire(s2, c2, "2", g2, "GND");
    wire(s2, v2, "-", g2, "GND");
    Simulator sim2(s2);
    AcResult res2 = sim2.ac(o);
    CHECK(res2.ok);
    if (!res2.ok) return;
    const AcMetrics& mc = res2.metrics[s2.netOf({c2, 0})];
    CHECK_NEAR(mc.peakHz / (f0 * std::sqrt(1 - 1 / (2 * q * q))), 1.0, 1e-5);
    CHECK_NEAR(mc.peakDb, 20 * std::log10(q / std::sqrt(1 - 1 / (4 * q * q))), 1e-5);
    CHECK_NEAR(mc.lowFreqDb, 0.0, 0.01);
}

TEST(ac_opamp_gain_bandwidth) {
    // Non-inverting gain 2 with an LM358 (GBW 1 MHz): 6.02 dB, closed-loop pole at GBW·β (≈ 500 kHz).
    Schematic s;
    int vin = s.addComponent(ComponentKind::VoltageSource, "1 AC 1", {0, 0});
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
    AcOptions o;
    o.fStart = 10;
    o.fStop = 10e6;
    AcResult r = sim.ac(o);
    CHECK(r.ok);
    if (!r.ok) return;
    int out = s.netOf({u, 2});
    const AcMetrics& m = r.metrics[out];
    CHECK_NEAR(m.lowFreqDb, 20 * std::log10(2.0), 1e-4);
    // A(s) = A0 / (1 + s/ωp), ωp = 2π·GBW/A0 → closed-loop pole at fp·(1 + A0·β).
    const double a0 = 1e6, fp = 1e6 / a0, expected = fp * (1 + a0 * 0.5);
    CHECK_NEAR(m.f3dbHz / expected, 1.0, 1e-4);
    CHECK_NEAR(r.dcNetVoltages[static_cast<size_t>(out)], 2.0, 1e-3);

    // Open loop (TL072, GBW 3 MHz): unity gain at ≈ GBW with 90° phase margin (single pole).
    Schematic ol;
    int vs = ol.addComponent(ComponentKind::VoltageSource, "0 AC 1", {0, 0});
    int a = ol.addComponent(ComponentKind::OpAmp, "TL072", {100, 0});
    int gg = ol.addComponent(ComponentKind::Ground, "", {0, 80});
    int rl = ol.addComponent(ComponentKind::Resistor, "10k", {200, 0});
    wire(ol, vs, "+", a, "IN+");
    wire(ol, vs, "-", gg, "GND");
    wire(ol, a, "IN-", gg, "GND");
    wire(ol, a, "OUT", rl, "1");
    wire(ol, rl, "2", gg, "GND");
    Simulator olSim(ol);
    AcOptions oo;
    oo.fStart = 0.01;
    oo.fStop = 100e6;
    AcResult olr = olSim.ac(oo);
    CHECK(olr.ok);
    if (!olr.ok) return;
    const AcMetrics& om = olr.metrics[ol.netOf({a, 2})];
    CHECK_NEAR(om.lowFreqDb, 120.0, 0.01);
    CHECK_NEAR(om.unityHz / 3e6, 1.0, 1e-4);
    CHECK_NEAR(om.phaseMarginDeg, 90.0, 0.01);
    CHECK_NEAR(om.f3dbHz, 3.0, 1e-3);  // dominant pole GBW / A0
    // An explicit "GBW=" in the value overrides the part table.
    ol.setValue(a, "generic GBW=10MEG");
    Simulator gbwSim(ol);
    AcResult gr = gbwSim.ac(oo);
    CHECK(gr.ok);
    if (gr.ok) CHECK_NEAR(gr.metrics[ol.netOf({a, 2})].unityHz / 10e6, 1.0, 1e-4);
}

TEST(ac_semiconductor_small_signal) {
    // Diode at 1 mA: r_d = n·V_T / (I + I_s).
    {
        Schematic s;
        int i = s.addComponent(ComponentKind::CurrentSource, "1m AC 1u", {0, 0});
        int d = s.addComponent(ComponentKind::Diode, "1N4148", {100, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        wire(s, i, "+", d, "A");
        wire(s, d, "K", g, "GND");
        wire(s, i, "-", g, "GND");
        Simulator sim(s);
        AcOptions o;
        o.fStart = 100;
        o.fStop = 1000;
        o.pointsPerDecade = 2;
        AcResult r = sim.ac(o);
        CHECK(r.ok);
        if (!r.ok) return;
        const double rd = 1.752 * 0.025852 / (1e-3 + 2.52e-9);
        CHECK_NEAR(std::abs(r.netPhasors[static_cast<size_t>(s.netOf({d, 0}))][0]) / (1e-6 * rd), 1.0, 1e-4);
    }
    // Common-emitter stage driven by a base current: v_c = −β·i_b·R_C (β = 200 for the BC847 model).
    {
        Schematic s;
        int vcc = s.addComponent(ComponentKind::VoltageSource, "10", {0, 0});
        int ib = s.addComponent(ComponentKind::CurrentSource, "10u AC 1u", {100, 0});
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
        AcOptions o;
        o.fStart = 100;
        o.fStop = 1000;
        o.pointsPerDecade = 2;
        AcResult r = sim.ac(o);
        CHECK(r.ok);
        if (!r.ok) return;
        std::complex<double> vc = r.netPhasors[static_cast<size_t>(s.netOf({q, 1}))][0];
        CHECK_NEAR(vc.real(), -200 * 1e-6 * 1e3, 1e-4);
        CHECK_NEAR(vc.imag(), 0.0, 1e-9);
        CHECK(r.stimulus.size() == 1 && r.stimulus[0] == ib);  // the DC supply is not a stimulus
    }
}

TEST(ac_stimulus_selection_and_errors) {
    {
        Schematic s;  // no AC magnitude and no sine source
        rcLowPass(s, "5", "1k", "100n");
        Simulator sim(s);
        AcResult r = sim.ac({});
        CHECK(!r.ok && r.error.find("No AC stimulus") != std::string::npos);
        AcOptions o;
        o.sourceId = s.findByRef("V1")->id;  // an explicit input drives with 1 V
        AcResult r2 = sim.ac(o);
        CHECK(r2.ok);
        o.sourceId = s.findByRef("R1")->id;
        CHECK(!sim.ac(o).ok);
    }
    {
        Schematic s;  // a SIN source is the stimulus by default
        int out = rcLowPass(s, "SIN(0 1 1k)", "1k", "100n");
        Simulator sim(s);
        AcOptions o;
        o.measureNets = {out};
        AcResult r = sim.ac(o);
        CHECK(r.ok && r.metrics.size() == 1);
        AcOptions bad;
        bad.fStart = 1e3;
        bad.fStop = 10;
        CHECK(!sim.ac(bad).ok);
    }
}

TEST(dc_sweep_source) {
    Schematic s;
    int v = s.addComponent(ComponentKind::VoltageSource, "10", {0, 0});
    int r1 = s.addComponent(ComponentKind::Resistor, "10k", {100, 0});
    int r2 = s.addComponent(ComponentKind::Resistor, "30k", {200, 0});
    int d = s.addComponent(ComponentKind::Diode, "1N4148", {300, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    wire(s, v, "+", r1, "1");
    wire(s, r1, "2", r2, "1");
    wire(s, r2, "2", g, "GND");
    wire(s, r1, "2", d, "A");
    wire(s, d, "K", g, "GND");
    wire(s, v, "-", g, "GND");
    Simulator sim(s);
    DcSweepResult r = sim.dcSweep(v, -5, 5, 0.25);
    CHECK(r.ok);
    if (!r.ok) return;
    CHECK(r.values.size() == 41);
    const auto& mid = r.netVoltages[static_cast<size_t>(s.netOf({r1, 1}))];
    // Reverse biased: a plain 10k / 30k divider. Forward: clamped below ~0.75 V and rising monotonically.
    CHECK_NEAR(mid[0], -5 * 0.75, 1e-4);  // only the diode's leakage loads the divider
    CHECK(mid.back() > 0.5 && mid.back() < 0.75);
    for (size_t i = 1; i < mid.size(); ++i) CHECK(mid[i] > mid[i - 1]);
    // Same answer as a separate DC operating point.
    s.setValue(v, "5");
    Simulator op(s);
    DcResult dc = op.dcOperatingPoint();
    CHECK_NEAR(mid.back(), netV(s, dc, r1, "2"), 1e-6);
    CHECK(r.currents.count(d) == 1 && r.currents.at(d).size() == 41);
    CHECK(!sim.dcSweep(r1, 0, 1, 0.1).ok);  // not a source
    CHECK(!sim.dcSweep(v, 0, 1, 0).ok);
}

TEST(parameter_sweep_dc_ac_transient) {
    Schematic s;
    int out = rcLowPass(s, "1 AC 1", "1k", "100n");
    int r = s.findByRef("R1")->id;
    ParamSweepOptions o;
    o.componentId = r;
    o.values = {"1k", "2k", "4k7"};
    o.analysis = SweepAnalysis::Ac;
    o.ac.fStart = 10;
    o.ac.fStop = 1e6;
    o.ac.measureNets = {out};
    std::string err;
    auto runs = parameterSweep(s, o, err);
    CHECK(err.empty() && runs.size() == 3);
    const double rs[] = {1e3, 2e3, 4.7e3};
    for (size_t i = 0; i < runs.size(); ++i) {
        CHECK(runs[i].ok());
        const double fc = 1 / (2 * kPi * rs[i] * 100e-9);
        CHECK_NEAR(runs[i].ac.metrics[out].f3dbHz / (fc * std::sqrt(1 + 2 * (10 / fc) * (10 / fc))), 1.0, 1e-7);
    }
    o.analysis = SweepAnalysis::Transient;
    o.tStop = 1e-3;
    o.tStep = 10e-6;
    runs = parameterSweep(s, o, err);
    CHECK(runs.size() == 3 && runs[0].ok() && runs[2].ok());
    o.analysis = SweepAnalysis::Dc;
    o.values = {"1k", "bogus"};
    runs = parameterSweep(s, o, err);
    CHECK(runs.size() == 2 && runs[0].ok() && !runs[1].ok() && runs[1].error().find("R1") != std::string::npos);
    CHECK(s.findByRef("R1")->value == "1k");  // the design itself is untouched

    // JSON form: the DC sweep of a divider's lower resistor.
    Schematic d;
    int v = d.addComponent(ComponentKind::VoltageSource, "10", {0, 0});
    int a = d.addComponent(ComponentKind::Resistor, "10k", {100, 0});
    int b = d.addComponent(ComponentKind::Resistor, "10k", {200, 0});
    int g = d.addComponent(ComponentKind::Ground, "", {0, 80});
    wire(d, v, "+", a, "1");
    wire(d, a, "2", b, "1");
    wire(d, b, "2", g, "GND");
    wire(d, v, "-", g, "GND");
    int mid = d.netOf({a, 1});
    Json opts = Json::parse("{\"component\":\"R2\",\"values\":[\"10k\",\"30k\",90000],\"analysis\":\"dc\",\"net\":" +
                            std::to_string(mid) + "}");
    Json j = simulateParamSweepJson(d, opts);
    CHECK(j.get("ok").asBool());
    CHECK(j.get("runs").size() == 3);
    if (j.get("runs").size() == 3) {
        CHECK_NEAR(j.get("runs")[0].get("nets")[0].get("voltage").asNumber(), 5.0, 1e-6);
        CHECK_NEAR(j.get("runs")[1].get("nets")[0].get("voltage").asNumber(), 7.5, 1e-6);
        CHECK_NEAR(j.get("runs")[2].get("nets")[0].get("voltage").asNumber(), 9.0, 1e-6);
    }
}

TEST(monte_carlo_and_worst_case_divider) {
    Schematic s;
    int v = s.addComponent(ComponentKind::VoltageSource, "10", {0, 0});
    int r1 = s.addComponent(ComponentKind::Resistor, "10k", {100, 0});
    int r2 = s.addComponent(ComponentKind::Resistor, "10k", {200, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    wire(s, v, "+", r1, "1");
    wire(s, r1, "2", r2, "1");
    wire(s, r2, "2", g, "GND");
    wire(s, v, "-", g, "GND");
    Measurement m;
    m.net = s.netOf({r1, 1});
    ToleranceOptions o;
    o.runs = 2000;
    o.seed = 42;
    ToleranceResult r = toleranceAnalysis(s, m, o);
    CHECK(r.ok);
    CHECK(r.parts.size() == 2 && r.failedRuns == 0 && r.samples.size() == 2000);
    CHECK_NEAR(r.nominal, 5.0, 1e-6);
    // Worst case with 1 % parts: 10·1.01 / (0.99 + 1.01) = 5.05 and 4.95.
    CHECK(r.hasWorstCase);
    CHECK_NEAR(r.worstMax, 5.05, 1e-6);
    CHECK_NEAR(r.worstMin, 4.95, 1e-6);
    CHECK_NEAR(r.worstMaxScale[r2], 1.01, 1e-12);
    CHECK_NEAR(r.worstMaxScale[r1], 0.99, 1e-12);
    CHECK(r.min >= r.worstMin - 1e-9 && r.max <= r.worstMax + 1e-9);
    // Uniform ±1 %: V ≈ 5·(1 + (δ2 − δ1)/2) → σ = 5·√(2/3)·0.01 / 2 ≈ 20.4 mV.
    CHECK_NEAR(r.mean, 5.0, 0.002);
    CHECK_NEAR(r.sigma / (5 * std::sqrt(2.0 / 3.0) * 0.01 / 2), 1.0, 0.06);
    // Seeded: the same seed repeats exactly, another seed differs.
    ToleranceOptions small = o;
    small.runs = 20;
    small.worstCase = false;
    auto a = toleranceAnalysis(s, m, small), b = toleranceAnalysis(s, m, small);
    small.seed = 7;
    auto c = toleranceAnalysis(s, m, small);
    CHECK(a.samples == b.samples);
    CHECK(a.samples != c.samples);
    // A tolerance in the value overrides the default; Gaussian runs stay inside the bounds.
    s.setValue(r1, "10k 5%");
    small.gaussian = true;
    small.runs = 200;
    small.worstCase = true;
    auto gsn = toleranceAnalysis(s, m, small);
    CHECK(gsn.ok && gsn.parts.size() == 2 && gsn.parts[0].fromValue && !gsn.parts[1].fromValue);
    CHECK_NEAR(gsn.parts[0].tolerance, 0.05, 1e-12);
    CHECK_NEAR(gsn.worstMax, 10 * 1.01 / (0.95 + 1.01), 1e-6);
    CHECK(gsn.min >= gsn.worstMin - 1e-9 && gsn.max <= gsn.worstMax + 1e-9);
    // Portable RNG: SplitMix64 reference values for seed 0.
    SeededRandom rng(0);
    CHECK(rng.next() == 0xE220A8397B1DCDAFULL);
    CHECK(rng.next() == 0x6E789E6AA1B965F4ULL);
}

TEST(monte_carlo_filter_corner) {
    // RC low-pass with default tolerances (R 1 %, C 10 %): f_c = 1/(2πRC) spans fc/(1.01·1.1) … fc/(0.99·0.9).
    Schematic s;
    int out = rcLowPass(s, "0 AC 1", "1k", "100n");
    Measurement m;
    m.kind = Measurement::Kind::AcF3db;
    m.net = out;
    m.ac.fStart = 10;
    m.ac.fStop = 1e6;
    m.ac.pointsPerDecade = 20;
    ToleranceOptions o;
    o.runs = 50;
    ToleranceResult r = toleranceAnalysis(s, m, o);
    CHECK(r.ok);
    const double fc = 1.0 / (2 * kPi * 1e3 * 100e-9);
    // (Half-power points are referred to the 10 Hz gain: within 2e-4 of 1/(2πRC) here.)
    CHECK_NEAR(r.nominal / fc, 1.0, 2e-4);
    CHECK_NEAR(r.worstMax / (fc / (0.99 * 0.9)), 1.0, 2e-4);
    CHECK_NEAR(r.worstMin / (fc / (1.01 * 1.1)), 1.0, 2e-4);
    CHECK(r.min >= r.worstMin * (1 - 1e-9) && r.max <= r.worstMax * (1 + 1e-9));
    Json j = simulateToleranceJson(
        s, Json::parse("{\"net\":" + std::to_string(out) + ",\"measure\":\"f3db\",\"runs\":10,\"start\":10,\"stop\":\"1MEG\"}"));
    CHECK(j.get("ok").asBool() && j.get("unit").asString("") == "Hz" && j.get("samples").size() == 10);
    CHECK(j.get("histogram").get("counts").size() == 20);
}

TEST(spectrum_thd) {
    // Synthetic: fundamental 1 kHz + 10 % third harmonic + DC.
    std::vector<double> t, v;
    for (int i = 0; i <= 5000; ++i) {
        double ti = i * 2e-6;
        t.push_back(ti);
        v.push_back(0.5 + std::sin(2 * kPi * 1e3 * ti) + 0.1 * std::sin(2 * kPi * 3e3 * ti));
    }
    SpectrumResult r = spectrum(t, v, 1e3);
    CHECK(r.ok && r.cycles == 10);
    CHECK_NEAR(r.thd, 0.1, 1e-4);
    CHECK_NEAR(r.dc, 0.5, 1e-4);
    CHECK(r.harmonics.size() >= 3);
    if (r.harmonics.size() >= 3) {
        CHECK_NEAR(r.harmonics[0].amplitude, 1.0, 1e-4);
        CHECK_NEAR(r.harmonics[2].dbc, -20.0, 0.01);
    }
    SpectrumResult detected = spectrum(t, v, 0);  // finds the 1 kHz fundamental itself
    CHECK(detected.ok);
    CHECK_NEAR(detected.fundamentalHz, 1e3, 5);
    CHECK_NEAR(detected.thd, 0.1, 0.002);
    CHECK(!spectrum(t, v, 10).ok);  // shorter than one period

    // Circuit: a ±1 V square wave (PULSE) has odd harmonics 1/n → THD through the 10th = √(Σ 1/n², n = 3,5,7,9).
    Schematic s;
    int src = s.addComponent(ComponentKind::VoltageSource, "PULSE(-1 1 1m)", {0, 0});
    int rl = s.addComponent(ComponentKind::Resistor, "1k", {100, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    wire(s, src, "+", rl, "1");
    wire(s, rl, "2", g, "GND");
    wire(s, src, "-", g, "GND");
    Json j = simulateFftJson(s, Json::parse("{\"net\":" + std::to_string(s.netOf({rl, 0})) +
                                            ",\"stop\":\"10m\",\"step\":\"1u\",\"fundamental\":1000}"));
    CHECK(j.get("ok").asBool());
    const double square = 100 * std::sqrt(1 / 9.0 + 1 / 25.0 + 1 / 49.0 + 1 / 81.0);
    CHECK_NEAR(j.get("thdPercent").asNumber(), square, 0.05);

    // A clean sine through an RC filter: negligible distortion (the fundamental defaults to the SIN source).
    Schematic f;
    int out = rcLowPass(f, "SIN(0 1 1k)", "1k", "100n");
    Json k = simulateFftJson(f, Json::parse("{\"net\":" + std::to_string(out) + ",\"stop\":\"20m\",\"step\":\"2u\"}"));
    CHECK(k.get("ok").asBool());
    CHECK_NEAR(k.get("fundamentalHz").asNumber(), 1000, 1e-9);
    CHECK(k.get("thdPercent").asNumber() < 0.001);
}

TEST(analysis_json_api) {
    Schematic s;
    rcLowPass(s, "0 AC 1", "1k", "100n");
    // Through text, as the app reads it.
    Json ac = Json::parse(
        simulateAcJson(s, Json::parse("{\"start\":\"10\",\"stop\":\"1MEG\",\"pointsPerDecade\":10}")).dump());
    CHECK(ac.get("ok").asBool());
    CHECK(ac.get("frequency").size() == 51);
    CHECK(ac.get("stimulus").size() == 1 && ac.get("stimulus")[0].asString("") == "V1");
    bool found = false;
    for (size_t i = 0; i < ac.get("nets").size(); ++i) {
        const Json& n = ac.get("nets")[i];
        if (n.get("metrics").get("f3dbHz").isNumber()) {
            found = true;
            CHECK_NEAR(n.get("metrics").get("f3dbHz").asNumber(), 1591.612, 0.01);  // referred to the 10 Hz gain
            CHECK(n.get("metrics").get("unityHz").isNull());  // NaN → null
            CHECK(n.get("magnitudeDb").size() == 51 && n.get("phaseDeg").size() == 51);
        }
    }
    CHECK(found);
    CHECK(!simulateAcJson(s, Json::parse("{\"source\":\"V9\"}")).get("ok").asBool());
    Json sweep = simulateDcSweepJson(s, Json::parse("{\"source\":\"V1\",\"start\":0,\"stop\":1,\"step\":\"250m\"}"));
    CHECK(sweep.get("ok").asBool() && sweep.get("values").size() == 5 && sweep.get("unit").asString("") == "V");
    CHECK(!simulateDcSweepJson(s, Json::object()).get("ok").asBool());
}

// ------------------------------------------------------------------ library import (KiCad / Eagle)

namespace {
std::string readFixture(const std::string& name) {
    std::ifstream f(std::string(SIEDA_FIXTURE_DIR) + "/library/" + name, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

const ImportedPart* importedPart(const LibraryImport& r, const std::string& name) {
    for (const auto& p : r.parts)
        if (p.spec.name == name) return &p;
    return nullptr;
}

bool anyContains(const std::vector<std::string>& lines, const std::string& text) {
    for (const auto& l : lines)
        if (l.find(text) != std::string::npos) return true;
    return false;
}

SymbolPin layoutPin(const CustomPartSpec& spec, const std::string& number) {
    for (const auto& p : spec.symbol.pins)
        if (p.number == number) return p;
    return SymbolPin{number, '?', -1};
}

const char* const kSoic = "SOIC-8_3.9x4.9mm_P1.27mm.kicad_mod";
const char* const kQfn = "QFN-16-1EP_3x3mm_P0.5mm_EP1.7x1.7mm.kicad_mod";
const char* const kHeader = "PinHeader_1x04_P2.54mm_Vertical.kicad_mod";
const char* const kSymbols = "test_parts.kicad_sym";
const char* const kEagle = "test_parts.lbr";
}  // namespace

TEST(library_import_kicad_footprints) {
    // KiCad 8 SOIC-8: roundrect pads become rectangles, the fab outline is the body, the courtyard the centre.
    const ImportedFootprint soic = parseKicadFootprint(readFixture(kSoic), kSoic);
    CHECK(soic.name == "SOIC-8_3.9x4.9mm_P1.27mm");
    CHECK(soic.description.find("MS-012AA") != std::string::npos);
    CHECK(soic.package.type == "CUSTOM" && soic.package.lands.size() == 8 && soic.padNumbers.size() == 8);
    CHECK(std::fabs(soic.package.lands[0].x + 2.475) < 1e-9 && std::fabs(soic.package.lands[0].y + 1.905) < 1e-9);
    CHECK(std::fabs(soic.package.lands[0].w - 1.95) < 1e-9 && std::fabs(soic.package.lands[0].h - 0.6) < 1e-9);
    CHECK(std::fabs(soic.package.lands[4].x - 2.475) < 1e-9 && std::fabs(soic.package.lands[4].y - 1.905) < 1e-9);
    CHECK(std::fabs(soic.package.bodySize - 3.9) < 1e-9 && std::fabs(soic.package.bodyDepth - 4.9) < 1e-9);
    CHECK(soic.warnings.empty());

    // QFN-16: pads rotated by 90° / 270° swap their sides; the exposed pad and its thermal vias are pin 17; paste-only
    // apertures without copper are skipped.
    const ImportedFootprint qfn = parseKicadFootprint(readFixture(kQfn), kQfn);
    CHECK(qfn.package.lands.size() == 21);
    CHECK(std::fabs(qfn.package.lands[4].w - 0.25) < 1e-9 && std::fabs(qfn.package.lands[4].h - 0.85) < 1e-9);
    CHECK(std::fabs(qfn.package.lands[8].w - 0.85) < 1e-9);
    CHECK(qfn.padNumbers[16] == "17" && qfn.padNumbers[20] == "17");
    CHECK(qfn.package.lands[17].drill > 0.19 && qfn.package.lands[17].round);
    CHECK(anyContains(qfn.warnings, "paste-only"));
    CHECK(std::fabs(qfn.package.bodySize - 3) < 1e-9);

    // KiCad 5 (module …) syntax, unquoted pad numbers, pin 1 at the origin: recentred on the courtyard; the
    // non-plated hole is reported, not imported.
    const ImportedFootprint header = parseKicadFootprint(readFixture(kHeader), kHeader);
    CHECK(header.package.lands.size() == 4);
    CHECK(std::fabs(header.package.lands[0].y + 3.8) < 1e-9 && std::fabs(header.package.lands[3].y - 3.82) < 1e-9);
    CHECK(!header.package.lands[0].round && header.package.lands[1].round);
    CHECK(std::fabs(header.package.lands[0].drill - 1.0) < 1e-9);
    CHECK(std::fabs(header.package.bodySize - 2.54) < 1e-9 && std::fabs(header.package.bodyDepth - 10.16) < 1e-9);
    CHECK(anyContains(header.warnings, "Origin moved") && anyContains(header.warnings, "non-plated"));

    // A footprint on its own is a part: one passive pin per pad number, connectors get the J prefix.
    const ImportedPart part = makeImportedPart(nullptr, &header);
    CHECK(part.ok && part.spec.pins.size() == 4 && part.spec.refPrefix == "J");
    CHECK(part.spec.pins[0].type == PinType::Passive && part.spec.pins[3].number == "4");
    auto reg = CustomPartRegistry::instance().registerPart(part.spec);
    CHECK(reg->footprint.pads.size() == 4 && reg->footprint.pads[0].throughHole && reg->footprint.pads[2].pinIndex == 2);
    const ImportedPart qfnPart = makeImportedPart(nullptr, &qfn);
    CHECK(qfnPart.ok && qfnPart.spec.pins.size() == 17 && qfnPart.spec.refPrefix == "U");
    CHECK(checkLandPattern(qfnPart.spec).empty());

    // Errors name the problem and the line.
    auto fails = [](const std::string& text, const std::string& expect) {
        try {
            parseKicadFootprint(text);
        } catch (const ImportError& e) {
            if (std::string(e.what()).find(expect) != std::string::npos) return true;
            std::printf("    unexpected message: %s\n", e.what());
            return false;
        }
        return false;
    };
    CHECK(fails("", "expected '('"));
    CHECK(fails("(footprint \"X\"\n  (pad \"1\" smd rect (at 0 0) (size 1 1) (layers \"F.Cu\"))\n", "line 3: missing ')'"));
    CHECK(fails("(kicad_symbol_lib)", "not a KiCad footprint"));
    CHECK(fails("(footprint \"X\" (pad \"1\" smd rect (at 0 0) (size 1 1) (layers \"F.Cu\"))"
                " (pad \"2\" smd rect (at 130 0) (size 1 1) (layers \"F.Cu\")))",
                "60 mm"));
    CHECK(fails("(footprint \"X\" (pad \"1\" smd rect (at 0 0) (size 40 1) (layers \"F.Cu\")))", "larger than 30 mm"));
    CHECK(fails("(footprint \"X\" (pad \"1\" smd rect (at 0 0) (layers \"F.Cu\")))", "no position or size"));
    CHECK(fails("(footprint \"X\" (fp_line (start 0 0) (end 1 1) (layer \"F.SilkS\")))", "no copper pads"));
    CHECK(fails("(footprint \"X\" (pad \"1\" smd rect (at 0 0) (size 1 1) (layers \"F.Cu\")) \"open", "unterminated string"));
    std::string many = "(footprint \"BIG\"";
    for (int i = 0; i < 600; ++i)
        many += " (pad \"" + std::to_string(i + 1) + "\" smd rect (at " + std::to_string((i % 30) * 1.5 - 22) + " " +
                std::to_string((i / 30) * 1.5 - 15) + ") (size 1 1) (layers \"F.Cu\"))";
    CHECK(fails(many + ")", "up to 512"));
    // Slots, oblique pads, drills as large as the pad, custom shapes and bottom pads import with a note.
    const ImportedFootprint odd = parseKicadFootprint(
        "(footprint \"ODD\" (pad \"1\" thru_hole oval (at -3 0) (size 1.2 2.4) (drill oval 0.8 1.6) (layers \"*.Cu\"))"
        " (pad \"2\" smd rect (at 0 0 45) (size 1 1) (layers \"F.Cu\"))"
        " (pad \"3\" thru_hole circle (at 3 0) (size 1 1) (drill 1) (layers \"*.Cu\"))"
        " (pad \"4\" smd custom (at 0 4) (size 0.5 0.5) (layers \"F.Cu\") (primitives (gr_poly (pts (xy -1 -0.5) (xy 1 -0.5)"
        " (xy 1 0.5) (xy -1 0.5)))))"
        " (pad \"5\" smd rect (at 0 -4) (size 1 1) (layers \"B.Cu\")))");
    CHECK(odd.package.lands.size() == 5);
    CHECK(std::fabs(odd.package.lands[0].drill - 0.8) < 1e-9);
    CHECK(std::fabs(odd.package.lands[1].w - std::sqrt(2.0)) < 1e-3);
    CHECK(odd.package.lands[2].w > 1.15 && odd.package.lands[2].drill < odd.package.lands[2].w);
    CHECK(std::fabs(odd.package.lands[3].w - 2) < 1e-9 && std::fabs(odd.package.lands[3].h - 1) < 1e-9);
    for (const char* note : {"slotted", "at an angle", "enlarged", "custom-shaped", "bottom-side"})
        CHECK(anyContains(odd.warnings, note));
}

TEST(library_import_kicad_symbols_and_pairing) {
    const auto symbols = parseKicadSymbols(readFixture(kSymbols), kSymbols);
    CHECK(symbols.size() == 5);
    const ImportedSymbol& lm358 = symbols[0];
    CHECK(lm358.name == "LM358" && lm358.units == 3 && lm358.pins.size() == 8 && lm358.refPrefix == "U");
    CHECK(lm358.footprint == "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm" && lm358.datasheet.find("lm2904") != std::string::npos);
    // Derived symbol: the parent's pins and footprint, its own value and description.
    CHECK(symbols[1].name == "LM2904" && symbols[1].pins.size() == 8 && symbols[1].footprint == lm358.footprint);
    CHECK(symbols[1].description.find("automotive") != std::string::npos);

    std::vector<ImportFile> files;
    for (const char* f : {kSymbols, kSoic, kQfn, kHeader}) files.push_back({f, readFixture(f)});
    const LibraryImport r = importLibraryFiles(files);
    CHECK(r.files.size() == 4 && r.symbols.size() == 5 && r.footprints.size() == 3);
    for (const auto& f : r.files) CHECK(f.error.empty());
    CHECK(r.parts.size() == 6);  // 5 symbols + the header no symbol uses

    // LM358 → its SOIC-8 by the footprint name. Units side by side: A (+ − out), a gap, B; supplies top and bottom.
    const ImportedPart* op = importedPart(r, "LM358");
    CHECK(op && op->ok && op->footprintName == "SOIC-8_3.9x4.9mm_P1.27mm");
    if (op) {
        CHECK(op->spec.pins.size() == 8 && op->spec.pins[0].number == "1" && op->spec.pins[7].number == "8");
        CHECK(op->spec.pins[1].name == "-" && op->spec.pins[1].type == PinType::Input);
        CHECK(layoutPin(op->spec, "3").side == 'L' && layoutPin(op->spec, "3").slot == 0);
        CHECK(layoutPin(op->spec, "2").slot == 2 && layoutPin(op->spec, "5").slot == 4);
        CHECK(layoutPin(op->spec, "1").side == 'R' && layoutPin(op->spec, "7").side == 'R');
        CHECK(layoutPin(op->spec, "8").side == 'T' && layoutPin(op->spec, "4").side == 'B');
        CHECK(checkSymbol(op->spec).empty() && checkLandPattern(op->spec).empty());
        CHECK(anyContains(op->warnings, "3 units"));
        auto part = CustomPartRegistry::instance().registerPart(op->spec);
        CHECK(part->footprint.pads.size() == 8 && part->def.pins.size() == 8);
        for (size_t i = 0; i < 8; ++i) CHECK(part->footprint.pads[i].pinIndex == static_cast<int>(i));
    }
    CHECK(importedPart(r, "LM2904") && importedPart(r, "LM2904")->ok);

    // MCU → QFN-16: exposed pad and thermal vias on pin 17, layout kept (ports, reset, debug), stacked VDD.
    const ImportedPart* mcu = importedPart(r, "TESTMCU-QFN16");
    CHECK(mcu && mcu->ok && mcu->spec.manufacturer == "Example Semi");
    if (mcu) {
        CHECK(mcu->spec.pins.size() == 17 && mcu->spec.pins[4].name == "nRESET");
        CHECK(mcu->spec.pins[15].type == PinType::PowerIn);  // the second VDD, "passive" in the file
        CHECK(layoutPin(mcu->spec, "15").side == 'T' && layoutPin(mcu->spec, "16").slot == layoutPin(mcu->spec, "15").slot);
        CHECK(layoutPin(mcu->spec, "17").side == 'B');                                // hidden ground pin
        CHECK(layoutPin(mcu->spec, "5").slot == layoutPin(mcu->spec, "4").slot + 2);  // the gap in the file is kept
        auto part = CustomPartRegistry::instance().registerPart(mcu->spec);
        CHECK(part->footprint.pads.size() == 21);
        for (size_t i = 16; i < part->footprint.pads.size(); ++i) CHECK(part->footprint.pads[i].pinIndex == 16);
        CHECK(!anyContains(mcu->warnings, "auto-arranged"));
    }
    // No footprint file: a package SiEDA generates from the name; an unknown one is an error that says what to add.
    const ImportedPart* ldo = importedPart(r, "AMS1117-3.3");
    CHECK(ldo && ldo->ok && ldo->spec.package.type == "SOT223" && anyContains(ldo->warnings, "generated from its package name"));
    const ImportedPart* odd = importedPart(r, "ODD-SENSOR");
    CHECK(odd && !odd->ok && odd->error.find("Odd_Sensor_Module") != std::string::npos);
    const ImportedPart* header = importedPart(r, "PinHeader_1x04_P2.54mm_Vertical");
    CHECK(header && header->ok && header->symbolName.empty());

    // Explicit pairs: unused pads become mechanical; a pin without a pad is refused with the check's message.
    const LibraryImport paired = importLibraryFiles(files, {{"ODD-SENSOR", "SOIC-8_3.9x4.9mm_P1.27mm"},
                                                           {"TESTMCU-QFN16", "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm"},
                                                           {"AMS1117-3.3", "Nope"}});
    const ImportedPart* sensor = importedPart(paired, "ODD-SENSOR");
    CHECK(sensor && sensor->ok && anyContains(sensor->warnings, "mechanical"));
    if (sensor) CHECK(sensor->spec.package.lands[3].pin == "-" && sensor->spec.package.lands[0].pin.empty());
    const ImportedPart* wrong = importedPart(paired, "TESTMCU-QFN16");
    CHECK(wrong && !wrong->ok && wrong->error.find("has no pad") != std::string::npos);
    const ImportedPart* missing = importedPart(paired, "AMS1117-3.3");
    CHECK(missing && !missing->ok && missing->error.find("Nope") != std::string::npos);

    // A single symbol + footprint download pairs even when the symbol names another footprint.
    const LibraryImport single = importLibraryFiles(
        {{"odd.kicad_sym", "(kicad_symbol_lib (symbol \"X1\" (property \"Reference\" \"Q?\") (property \"Footprint\" \"Vendor:X1\")"
                           " (symbol \"X1_1_1\" (pin input line (at -5.08 0 0) (length 2.54) (name \"A\") (number \"1\"))"
                           " (pin output line (at 5.08 0 180) (length 2.54) (name \"B\") (number \"2\")))))"},
         {kSoic, readFixture(kSoic)}});
    CHECK(single.parts.size() == 1 && single.parts[0].ok && single.parts[0].footprintName == "SOIC-8_3.9x4.9mm_P1.27mm");
    CHECK(!single.parts.empty() && single.parts[0].spec.refPrefix == "Q");

    // Footprint filters (ki_fp_filters) pick the matching footprint among several; a base symbol from another file
    // (one file per symbol in a .kicad_symdir) gives a derived symbol its pins.
    const LibraryImport filtered = importLibraryFiles(
        {{"base.kicad_sym", "(kicad_symbol_lib (symbol \"OPA\" (property \"ki_fp_filters\" \"DIP*W7.62mm* SOIC*3.9x4.9mm*P1.27mm*\")"
                            " (symbol \"OPA_1_1\" (pin input line (at -5.08 0 0) (length 2.54) (name \"IN\") (number \"3\"))"
                            " (pin output line (at 5.08 0 180) (length 2.54) (name \"OUT\") (number \"6\")))))"},
         {"derived.kicad_sym", "(kicad_symbol_lib (symbol \"OPA2\" (extends \"OPA\") (property \"Value\" \"OPA2\")))"},
         {"orphan.kicad_sym", "(kicad_symbol_lib (symbol \"OPA3\" (extends \"NOWHERE\")))"},
         {kQfn, readFixture(kQfn)},
         {kSoic, readFixture(kSoic)}});
    const ImportedPart* opa = importedPart(filtered, "OPA");
    CHECK(opa && opa->ok && opa->footprintName == "SOIC-8_3.9x4.9mm_P1.27mm");
    const ImportedPart* opa2 = importedPart(filtered, "OPA2");
    CHECK(opa2 && opa2->ok && opa2->spec.pins.size() == 2 && opa2->footprintName == "SOIC-8_3.9x4.9mm_P1.27mm");
    const ImportedPart* opa3 = importedPart(filtered, "OPA3");
    CHECK(opa3 && !opa3->ok && opa3->error.find("NOWHERE.kicad_sym") != std::string::npos);

    // Pads of two pins that share copper (USB-C: A1 and B12, both GND) join one pin; pins of different names on
    // overlapping pads stay an error.
    const std::string usbFootprint =
        "(footprint \"USB_TEST\" (pad \"A1\" smd rect (at -1 0) (size 0.6 1.4) (layers \"F.Cu\"))"
        " (pad \"B12\" smd rect (at -1 0) (size 0.6 1.4) (layers \"F.Cu\"))"
        " (pad \"A6\" smd rect (at 0 0) (size 0.3 1.4) (layers \"F.Cu\"))"
        " (pad \"SH\" thru_hole oval (at 3 0) (size 1 2) (drill 0.6) (layers \"*.Cu\")))";
    auto usbSymbol = [](const char* b12Name) {
        return std::string("(kicad_symbol_lib (symbol \"USB\" (property \"Reference\" \"J\") (property \"Footprint\" \"X:USB_TEST\")"
                           " (symbol \"USB_1_1\" (pin power_in line (at 0 -7.62 90) (length 2.54) (name \"GND\") (number \"A1\"))"
                           " (pin power_in line (at 0 -7.62 90) (length 2.54) (name \"") +
               b12Name +
               "\") (number \"B12\"))"
               " (pin bidirectional line (at 7.62 0 180) (length 2.54) (name \"D+\") (number \"A6\"))"
               " (pin passive line (at -7.62 0 0) (length 2.54) (name \"SHIELD\") (number \"SH\")))))";
    };
    const LibraryImport usb = importLibraryFiles({{"usb.kicad_sym", usbSymbol("GND")}, {"usb.kicad_mod", usbFootprint}});
    const ImportedPart* usbPart = importedPart(usb, "USB");
    CHECK(usbPart && usbPart->ok && usbPart->spec.pins.size() == 3 && anyContains(usbPart->warnings, "B12 into A1"));
    if (usbPart) CHECK(usbPart->spec.package.lands[1].pin == "A1" && layoutPin(usbPart->spec, "B12").slot < 0);
    const LibraryImport clashUsb = importLibraryFiles({{"usb.kicad_sym", usbSymbol("VBUS")}, {"usb.kicad_mod", usbFootprint}});
    CHECK(importedPart(clashUsb, "USB") && !importedPart(clashUsb, "USB")->ok &&
          importedPart(clashUsb, "USB")->error.find("overlap") != std::string::npos);
    const LibraryImport usbAlone = importLibraryFiles({{"usb.kicad_mod", usbFootprint}});
    CHECK(usbAlone.parts.size() == 1 && usbAlone.parts[0].ok && usbAlone.parts[0].spec.pins.size() == 3);
    CHECK(!usbAlone.parts.empty() && usbAlone.parts[0].spec.refPrefix == "J");

    // A layout with two different pins on one spot is auto-arranged with a note; a library without symbols is an error.
    const auto clash = parseKicadSymbols(
        "(kicad_symbol_lib (symbol \"C\" (symbol \"C_1_1\" (pin input line (at -5.08 0 0) (length 2.54) (name \"A\") (number \"1\"))"
        " (pin input line (at -5.08 0 0) (length 2.54) (name \"B\") (number \"2\")))) )");
    CHECK(clash.size() == 1 && clash[0].pins.size() == 2);
    const ImportedFootprint soic = parseKicadFootprint(readFixture(kSoic));
    const ImportedPart clashPart = makeImportedPart(&clash[0], &soic);
    CHECK(clashPart.ok && checkSymbol(clashPart.spec).empty());
    bool threw = false;
    try {
        parseKicadSymbols("(kicad_symbol_lib (version 1))");
    } catch (const ImportError& e) {
        threw = std::string(e.what()).find("no symbols") != std::string::npos;
    }
    CHECK(threw);

    // JSON round trip of the result, as the app reads it.
    const Json j = libraryImportToJson(r);
    CHECK(j.get("parts").size() == 6 && j.get("symbols").asInt() == 5 && j.get("footprints").asInt() == 3);
    const CustomPartSpec back = customPartSpecFromJson(j.get("parts")[0].get("spec"));
    CHECK(back.name == r.parts[0].spec.name && back.package.lands.size() == r.parts[0].spec.package.lands.size());
}

TEST(library_import_eagle_libraries) {
    LibraryImport r = importLibraryFiles({{kEagle, readFixture(kEagle)}});
    CHECK(r.files.size() == 1 && r.files[0].format == "eagle_lbr" && r.files[0].error.empty());
    CHECK(r.footprints.size() == 2 && r.parts.size() == 4);  // the GND supply symbol has no package: no part

    const ImportedPart* op = importedPart(r, "LM358D");
    CHECK(op && op->ok && op->spec.refPrefix == "IC" && op->footprintName == "SO08");
    if (op) {
        CHECK(op->spec.description == "Dual op amp Low power, single supply.");
        CHECK(op->spec.pins.size() == 8 && op->spec.pins[0].number == "1" && op->spec.pins[0].name == "OUT");
        CHECK(op->spec.pins[7].name == "V+" && op->spec.pins[7].type == PinType::PowerIn);
        // Eagle's y points up: pad 1 (y = −2.6) is at the bottom, y = +2.6 in SiEDA.
        CHECK(std::fabs(op->spec.package.lands[0].y - 2.6) < 1e-9 && std::fabs(op->spec.package.lands[4].y + 2.6) < 1e-9);
        CHECK(std::fabs(op->spec.package.bodySize - 4.8) < 1e-9 && std::fabs(op->spec.package.bodyDepth - 3.8) < 1e-9);
        CHECK(layoutPin(op->spec, "3").side == 'L' && layoutPin(op->spec, "1").side == 'R');
        CHECK(layoutPin(op->spec, "8").side == 'T' && layoutPin(op->spec, "4").side == 'B');
        CHECK(layoutPin(op->spec, "5").slot > layoutPin(op->spec, "2").slot);  // gate B below gate A
        CHECK(checkSymbol(op->spec).empty());
    }
    // '*' takes the technology: one part per technology. Long round pads rotated by 90°, the hole reported.
    const ImportedPart* bc = importedPart(r, "BC547B");
    CHECK(bc && bc->ok && importedPart(r, "BC548C") && bc->spec.refPrefix == "Q");
    if (bc) {
        const auto& l = bc->spec.package.lands[0];
        CHECK(std::fabs(l.w - 1.0) < 1e-9 && std::fabs(l.h - 2.0) < 1e-9 && std::fabs(l.drill - 0.6) < 1e-9 && l.round);
        CHECK(anyContains(bc->warnings, "non-plated"));
        CHECK(bc->spec.pins[1].name == "B" && layoutPin(bc->spec, "2").side == 'L');
    }
    // A pin on several pads: one pin per pad, stacked on the symbol and joined on the board.
    const ImportedPart* fet = importedPart(r, "SI4410DY");
    CHECK(fet && fet->ok && fet->spec.pins.size() == 8 && anyContains(fet->warnings, "stacked"));
    if (fet) {
        CHECK(fet->spec.pins[0].name == "S" && fet->spec.pins[2].name == "S" && fet->spec.pins[7].name == "D");
        CHECK(layoutPin(fet->spec, "1").slot == layoutPin(fet->spec, "3").slot && layoutPin(fet->spec, "1").side == 'B');
        auto part = CustomPartRegistry::instance().registerPart(fet->spec);
        CHECK(part->footprint.pads.size() == 8);
    }

    // Malformed XML and unsupported content are reported with a line number; the import goes on.
    auto error = [](const std::string& text) {
        LibraryImport out = importLibraryFiles({{"x.lbr", text}});
        return out.files.empty() ? std::string() : out.files[0].error;
    };
    CHECK(error("<eagle><drawing><library></drawing></eagle>").find("</drawing> closes <library>") != std::string::npos);
    CHECK(error("<eagle>\n<drawing>").find("line 2") != std::string::npos);
    CHECK(error("<schematic/>").find("not an Eagle library") != std::string::npos);
    CHECK(error("<eagle><drawing/></eagle>").find("no <library>") != std::string::npos);
    CHECK(error("<eagle a=b></eagle>").find("not quoted") != std::string::npos);
    LibraryImport broken = importLibraryFiles(
        {{"y.lbr", "<eagle><drawing><library><packages/><symbols/><devicesets><deviceset name=\"X\"><gates/><devices>"
                   "<device name=\"\" package=\"GONE\"><connects/></device></devices></deviceset></devicesets></library>"
                   "</drawing></eagle>"}});
    CHECK(broken.parts.size() == 1 && !broken.parts[0].ok && broken.parts[0].error.find("GONE") != std::string::npos);
}

TEST(library_import_rejects_unsupported_formats_and_survives_fuzzing) {
    auto fileError = [](const std::string& name, const std::string& content) {
        LibraryImport out = importLibraryFiles({{name, content}});
        return out.files.size() == 1 ? out.files[0].error : std::string("?");
    };
    CHECK(fileError("Parts.SchLib", std::string("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8)).find("Altium") != std::string::npos);
    CHECK(fileError("Parts.PcbLib", "x").find("not supported") != std::string::npos);
    CHECK(fileError("old.lib", "EESchema-LIBRARY Version 2.4\n").find("KiCad 5") != std::string::npos);
    CHECK(fileError("notes.txt", "hello").find("unknown file type") != std::string::npos);
    CHECK(fileError("deep.kicad_mod", std::string(100000, '(')).find("nesting too deep") != std::string::npos);
    CHECK(fileError("lt.lbr", "<eagle>" + std::string(5000, '<')).find("line 1") != std::string::npos);
    std::string nested = "<eagle>";
    for (int i = 0; i < 1000; ++i) nested += "<a>";
    CHECK(fileError("nested.lbr", nested).find("nesting too deep") != std::string::npos);
    CHECK(importLibraryFiles({}).parts.empty());
    // Content sniffing without an extension.
    LibraryImport sniffed = importLibraryFiles({{"download", readFixture(kSoic)}});
    CHECK(sniffed.files[0].format == "kicad_mod" && sniffed.parts.size() == 1 && sniffed.parts[0].ok);

    // Deterministic fuzzing: truncations, byte flips, deleted, duplicated and inserted ranges of every fixture never
    // crash or throw, and every part reported as importable registers.
    uint32_t seed = 12345;
    auto rnd = [&seed](size_t n) {
        seed = seed * 1664525u + 1013904223u;
        return n ? static_cast<size_t>((seed >> 8) % n) : size_t(0);
    };
    const char* noise[] = {"(", ")", "\"", "<", ">", "/>", "</a>", "&#x110000;", "&", "1e999", "-", "nan", "\xFF\xFE",
                           " (pad \"9\" smd rect (at 70 0) (size 1 1) (layers \"F.Cu\"))"};
    int runs = 0, okParts = 0;
    bool allOk = true;
    for (const char* name : {kSoic, kQfn, kHeader, kSymbols, kEagle}) {
        const std::string original = readFixture(name);
        for (int k = 0; k < 160; ++k) {
            std::string text = original;
            const int op = k % 5;
            const size_t at = rnd(text.size());
            if (op == 0) text.resize(at);
            else if (op == 1) text[at] = static_cast<char>(rnd(256));
            else if (op == 2) text.erase(at, rnd(200));
            else if (op == 3) text.insert(at, text.substr(rnd(text.size()), rnd(300)));
            else text.insert(at, noise[rnd(sizeof noise / sizeof noise[0])]);
            try {
                LibraryImport out = importLibraryFiles({{name, text}, {kSoic, readFixture(kSoic)}});
                for (const auto& p : out.parts) {
                    if (!p.ok) continue;
                    ++okParts;
                    CustomPartRegistry::instance().registerPart(customPartSpecFromJson(customPartSpecToJson(p.spec)));
                }
                (void)libraryImportToJson(out).dump();
            } catch (const std::exception& e) {
                std::printf("    %s mutation %d: %s\n", name, k, e.what());
                allOk = false;
            }
            ++runs;
        }
    }
    CHECK(allOk && runs == 800 && okParts > 400);
}

TEST(library_import_c_api) {
    CHECK(sieda_c_api_library_import_test() == 0);
}

// ======================================================================= interactive router

namespace {
int placeR(Project& p, Vec2 at, int rotation = 0) {
    const int id = p.schematic.addComponent(ComponentKind::Resistor, "1k", {at.x * 10, at.y * 10});
    Component* c = p.schematic.find(id);
    c->pcb.position = at;
    c->pcb.rotation = rotation;
    c->pcb.placed = true;
    return id;
}

Vec2 padAt(const Project& p, int comp, int pin) {
    for (const auto& pd : p.pcb.pads(p.schematic))
        if (pd.componentId == comp && pd.pinIndex == pin) return pd.position;
    return {-1, -1};
}

void addPath(PcbLayout& pcb, int net, int layer, double w, const std::vector<Vec2>& pts, bool locked = false) {
    for (size_t k = 0; k + 1 < pts.size(); ++k) {
        Track t;
        t.net = net;
        t.layer = layer;
        t.width = w;
        t.a = pts[k];
        t.b = pts[k + 1];
        t.locked = locked;
        pcb.addTrack(t);
    }
}

/// DRC errors (unrouted connections aside) and clearance warnings: what interactive routing must never cause.
int routingProblems(const Project& p) {
    int n = 0;
    for (const auto& v : p.pcb.runDRC(p.schematic)) {
        if ((v.severity == Severity::Error && v.code != "DRC_UNROUTED") || v.code == "DRC_CLEARANCE_RULE") {
            ++n;
            std::printf("    DRC %s: %s\n", v.code.c_str(), v.message.c_str());
        }
    }
    return n;
}

/// No ratsnest line ends on a pad of `net`.
bool netRouted(const Project& p, int net) {
    const auto pads = p.pcb.pads(p.schematic);
    for (const auto& l : p.pcb.ratsnest(p.schematic))
        for (const auto& pd : pads)
            if (pd.net == net && ((pd.position - l.first).length() < 1e-9 || (pd.position - l.second).length() < 1e-9))
                return false;
    return true;
}

double crossOf(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }

/// Three lanes (nets of resistor pairs) at 0.5 mm pitch along y = 22, 22.5 and 23, fanned out to pads at both
/// ends, and a fourth net with pads below them.
struct LaneBoard {
    Project p;
    int lane[3] = {-1, -1, -1};
    int net = -1;
    Vec2 from, to;
};

LaneBoard laneBoard() {
    LaneBoard b;
    auto& s = b.p.schematic;
    b.p.pcb.settings.width = 70;
    b.p.pcb.settings.height = 40;
    int left[3], right[3];
    for (int i = 0; i < 3; ++i) {
        left[i] = placeR(b.p, {5, 6.0 + 3 * i});
        right[i] = placeR(b.p, {65, 6.0 + 3 * i});
        wire(s, left[i], "2", right[i], "1");
    }
    const int c1 = placeR(b.p, {14, 32}), c2 = placeR(b.p, {56, 32});
    wire(s, c1, "2", c2, "1");
    b.p.schematicChanged();
    for (int i = 0; i < 3; ++i) {
        b.lane[i] = s.netOf({left[i], 1});
        const double y = 6.0 + 3 * i, Y = 22 + 0.5 * i, d = Y - y;
        addPath(b.p.pcb, b.lane[i], 0, 0.25,
                {padAt(b.p, left[i], 1), {8, y}, {8 + d, Y}, {62 - d, Y}, {62, y}, padAt(b.p, right[i], 0)});
    }
    b.net = s.netOf({c1, 1});
    b.from = padAt(b.p, c1, 1);
    b.to = padAt(b.p, c2, 0);
    return b;
}

/// Routes the lane board's fourth net along the lanes, close enough to shove all three.
RouteChanges routeAlongLanes(LaneBoard& b, std::string* previewDump = nullptr) {
    InteractiveRouter r(b.p.pcb, b.p.schematic);
    CHECK(r.beginRoute(b.from, 0));
    r.moveTo({20, 23.3});
    CHECK(r.fixHead());
    const RoutePreview& pv = r.moveTo({45, 23.3});
    CHECK(!pv.blocked);
    CHECK(!pv.shovedTracks.empty());
    if (previewDump) *previewDump = routePreviewJson(pv).dump();
    CHECK(r.fixHead());
    const RoutePreview& end = r.moveTo(b.to);
    CHECK(end.reachedTarget);
    return r.commit();
}
}  // namespace

TEST(router_shoves_a_chain_of_three_tracks) {
    LaneBoard b = laneBoard();
    CHECK(routingProblems(b.p) == 0);
    const RouteChanges ch = routeAlongLanes(b);
    CHECK(ch.ok);
    CHECK(!ch.addedTracks.empty());
    // Every lane was pushed (some of its tracks replaced), and stays connected pad to pad.
    for (int lane : b.lane) {
        int removed = 0;
        for (const auto& t : ch.removedTracks) removed += t.net == lane ? 1 : 0;
        CHECK(removed > 0);
        CHECK(netRouted(b.p, lane));
    }
    CHECK(netRouted(b.p, b.net));
    CHECK(routingProblems(b.p) == 0);
    // Every pair of nets keeps the clearance.
    for (const auto& t : b.p.pcb.tracks)
        for (const auto& u : b.p.pcb.tracks)
            if (t.net != u.net && t.layer == u.layer)
                CHECK(segmentSegmentDistance(t.a, t.b, u.a, u.b) - (t.width + u.width) / 2 >= 0.2 - 1e-6);
}

TEST(router_is_deterministic) {
    std::string d1, d2;
    LaneBoard a = laneBoard(), b = laneBoard();
    routeAlongLanes(a, &d1);
    routeAlongLanes(b, &d2);
    CHECK(!d1.empty() && d1 == d2);
    bool same = a.p.pcb.tracks.size() == b.p.pcb.tracks.size();
    for (size_t i = 0; same && i < a.p.pcb.tracks.size(); ++i)
        same = a.p.pcb.tracks[i].a == b.p.pcb.tracks[i].a && a.p.pcb.tracks[i].b == b.p.pcb.tracks[i].b &&
               a.p.pcb.tracks[i].net == b.p.pcb.tracks[i].net;
    CHECK(same);
}

TEST(router_walkaround_finds_a_path) {
    Project p;
    auto& s = p.schematic;
    const int r1 = placeR(p, {8, 20}), r2 = placeR(p, {42, 20});
    const int r3 = placeR(p, {25, 8}, 90), r4 = placeR(p, {25, 32}, 90);
    wire(s, r1, "2", r2, "1");
    wire(s, r3, "2", r4, "1");
    p.schematicChanged();
    const int netA = s.netOf({r1, 1}), netB = s.netOf({r3, 1});
    addPath(p.pcb, netB, 0, 0.25, {padAt(p, r3, 1), padAt(p, r4, 0)});  // a wall across the direct path
    const Track wall = p.pcb.tracks.front();
    InteractiveRouter r(p.pcb, s);
    RouterOptions o;
    o.mode = RouterMode::Walkaround;
    r.setOptions(o);
    CHECK(r.beginRoute(padAt(p, r1, 1), 0));
    const RoutePreview& pv = r.moveTo(padAt(p, r2, 0));
    CHECK(pv.reachedTarget);
    CHECK(!pv.blocked);
    CHECK(pv.shovedTracks.empty() && pv.hiddenTracks.empty());
    CHECK(pv.length > (padAt(p, r2, 0) - padAt(p, r1, 1)).length() + 5);  // it went round the wall
    const RouteChanges ch = r.commit();
    CHECK(ch.ok && ch.removedTracks.empty());
    CHECK(netRouted(p, netA));
    CHECK(routingProblems(p) == 0);
    bool wallKept = false;
    for (const auto& t : p.pcb.tracks) wallKept = wallKept || (t.id == wall.id && t.a == wall.a && t.b == wall.b);
    CHECK(wallKept);
    // 45° posture: every segment is horizontal, vertical or diagonal.
    for (const auto& t : p.pcb.tracks) {
        const double dx = std::fabs(t.b.x - t.a.x), dy = std::fabs(t.b.y - t.a.y);
        CHECK(dx < 1e-9 || dy < 1e-9 || std::fabs(dx - dy) < 1e-6);
    }
}

TEST(router_refuses_to_shove_pads_and_locked_tracks) {
    Project p;
    auto& s = p.schematic;
    const int r1 = placeR(p, {10, 20}), r2 = placeR(p, {30, 20}), r3 = placeR(p, {20, 20});
    wire(s, r1, "2", r2, "1");
    p.schematicChanged();
    // Pads are fixed: aiming into another net's pad stops the head short of it, clear of the pad.
    {
        InteractiveRouter r(p.pcb, s);
        CHECK(r.beginRoute(padAt(p, r1, 1), 0));
        const RoutePreview& pv = r.moveTo(padAt(p, r3, 0));
        CHECK(pv.blocked);
        CHECK(!pv.head.empty());
        CHECK(pv.status.find("pad") != std::string::npos);
        CHECK(pv.end.x < padAt(p, r3, 0).x - 0.5 - 0.2 - 0.125 + 1e-3);
        CHECK(r.commit().ok);
        CHECK(routingProblems(p) == 0);
    }
    // A locked track is never shoved: the head walks around it. Unlocked, the same track is pushed aside.
    for (bool locked : {true, false}) {
        Project q;
        auto& qs = q.schematic;
        const int a = placeR(q, {10, 20}), b = placeR(q, {40, 30}), c = placeR(q, {40, 5}), d = placeR(q, {40, 36});
        wire(qs, a, "2", b, "1");
        wire(qs, c, "2", d, "1");
        q.schematicChanged();
        addPath(q.pcb, qs.netOf({c, 1}), 0, 0.25, {{20, 15}, {20, 25}}, locked);
        InteractiveRouter r(q.pcb, qs);
        CHECK(r.beginRoute(padAt(q, a, 1), 0));
        const RoutePreview& pv = r.moveTo({25, 20});
        CHECK(!pv.blocked);
        CHECK(pv.hiddenTracks.empty() == locked);
        CHECK(r.commit().ok);
        CHECK(routingProblems(q) == 0);
        bool kept = false;
        for (const auto& t : q.pcb.tracks) kept = kept || (t.a == Vec2{20, 15} && t.b == Vec2{20, 25});
        CHECK(kept == locked);
    }
}

TEST(router_changes_layer_with_vias) {
    Project p;
    auto& s = p.schematic;
    const int r1 = placeR(p, {10, 20}), r2 = placeR(p, {30, 20}), r3 = placeR(p, {45, 5}), r4 = placeR(p, {45, 35});
    wire(s, r1, "2", r2, "1");
    wire(s, r3, "2", r4, "1");
    p.schematicChanged();
    const int netA = s.netOf({r1, 1});
    addPath(p.pcb, s.netOf({r3, 1}), 0, 0.25, {{20, 1}, {20, 39}}, true);  // a locked wall across the top layer
    InteractiveRouter r(p.pcb, s);
    CHECK(r.beginRoute(padAt(p, r1, 1), 0));
    r.moveTo({17, 20});
    CHECK(r.addVia());
    CHECK(r.preview().layer == 1 && r.preview().vias.size() == 1);
    r.moveTo({23, 20});
    CHECK(r.addVia(0));
    CHECK(r.preview().layer == 0 && r.preview().vias.size() == 2);
    const RoutePreview& pv = r.moveTo(padAt(p, r2, 0));
    CHECK(pv.reachedTarget);
    const RouteChanges ch = r.commit();
    CHECK(ch.ok && ch.addedVias.size() == 2);
    int vias = 0, bottom = 0;
    for (const auto& v : p.pcb.vias) vias += v.net == netA ? 1 : 0;
    for (const auto& t : p.pcb.tracks) bottom += t.net == netA && t.layer == 1 ? 1 : 0;
    CHECK(vias == 2 && bottom >= 1);
    CHECK(netRouted(p, netA));
    CHECK(routingProblems(p) == 0);
    // A single-sided board has no vias.
    Project single;
    const int a = placeR(single, {10, 20}), b = placeR(single, {30, 20});
    wire(single.schematic, a, "2", b, "1");
    single.pcb.settings.layerCount = 1;
    single.schematicChanged();
    InteractiveRouter rs(single.pcb, single.schematic);
    CHECK(rs.beginRoute(padAt(single, a, 1), 0));
    rs.moveTo({15, 20});
    CHECK(!rs.addVia());
    CHECK(!rs.error().empty());
}

TEST(router_routes_differential_pairs_at_the_pair_gap) {
    Project p;
    auto& s = p.schematic;
    const int p1 = placeR(p, {10, 14}), p2 = placeR(p, {40, 14}), n1 = placeR(p, {10, 16}), n2 = placeR(p, {40, 16});
    const int lp = s.addComponent(ComponentKind::NetLabel, "USB_P", {0, 0});
    const int ln = s.addComponent(ComponentKind::NetLabel, "USB_N", {0, 50});
    wire(s, p1, "2", p2, "1");
    wire(s, n1, "2", n2, "1");
    wire(s, lp, "N", p1, "2");
    wire(s, ln, "N", n1, "2");
    p.schematicChanged();
    const int netP = s.netOf({p1, 1}), netN = s.netOf({n1, 1});
    InteractiveRouter r(p.pcb, s);
    RouterOptions o;
    o.pairGap = 0.2;
    r.setOptions(o);
    CHECK(!r.beginPair({25, 25}, 0));  // not on a pad
    CHECK(r.beginPair(padAt(p, p1, 1), 0));
    CHECK(r.preview().kind == "pair" && r.preview().nets.size() == 2);
    r.moveTo({25, 15});
    CHECK(r.fixHead());
    const RoutePreview& pv = r.moveTo(padAt(p, p2, 0));
    CHECK(pv.reachedTarget);
    CHECK_NEAR(pv.gap, 0.2, 1e-9);
    CHECK(r.commit().ok);
    CHECK(netRouted(p, netP) && netRouted(p, netN));
    CHECK(routingProblems(p) == 0);
    // The coupled run keeps exactly the pair gap; nowhere closer.
    double minGap = 1e9, coupled = 0;
    for (const auto& a : p.pcb.tracks)
        for (const auto& b : p.pcb.tracks) {
            if (a.net != netP || b.net != netN) continue;
            const double g = segmentSegmentDistance(a.a, a.b, b.a, b.b) - (a.width + b.width) / 2;
            minGap = std::min(minGap, g);
            if (std::fabs(g - 0.2) < 1e-6 && std::fabs(crossOf(a.b - a.a, b.b - b.a)) < 1e-9)
                coupled += std::min((a.b - a.a).length(), (b.b - b.a).length());
        }
    CHECK(minGap >= 0.2 - 1e-6);
    CHECK(coupled > 20);
    // Without an explicit gap the pair uses the stack-up's coupled gap, never below the clearance.
    InteractiveRouter r2(p.pcb, s);
    CHECK(r2.beginPair(padAt(p, n1, 1), 0));
    CHECK(r2.preview().gap >= p.pcb.settings.clearance - 1e-9);
    r2.cancel();
    CHECK(!r2.active());
    // V on a pair places two vias side by side, far enough apart for their clearance, and both change layer.
    Project q;
    auto& qs = q.schematic;
    const int a1 = placeR(q, {10, 14}), a2 = placeR(q, {40, 14}), b1 = placeR(q, {10, 16}), b2 = placeR(q, {40, 16});
    wire(qs, a1, "2", a2, "1");
    wire(qs, b1, "2", b2, "1");
    wire(qs, qs.addComponent(ComponentKind::NetLabel, "CLK+", {0, 0}), "N", a1, "2");
    wire(qs, qs.addComponent(ComponentKind::NetLabel, "CLK-", {0, 50}), "N", b1, "2");
    q.schematicChanged();
    InteractiveRouter r3(q.pcb, qs);
    r3.setOptions(o);
    CHECK(r3.beginPair(padAt(q, a1, 1), 0));
    r3.moveTo({20, 15});
    CHECK(r3.addVia());
    const RoutePreview& pv3 = r3.moveTo({30, 15});
    CHECK(pv3.layer == 1 && pv3.vias.size() == 2);
    if (pv3.vias.size() == 2) {
        const double d = (pv3.vias[0].position - pv3.vias[1].position).length();
        CHECK(d >= q.pcb.settings.viaDiameter + q.pcb.settings.clearance - 1e-6);
    }
    CHECK(!pv3.head.empty());
    for (const auto& t : pv3.head) CHECK(t.layer == 1);
    CHECK(r3.commit().ok);
    CHECK(routingProblems(q) == 0);
}

TEST(router_drags_a_segment_and_shoves) {
    LaneBoard b = laneBoard();
    int id = -1;
    for (const auto& t : b.p.pcb.tracks)
        if (t.net == b.lane[2] && std::fabs(t.a.y - 23) < 1e-9 && std::fabs(t.b.y - 23) < 1e-9) id = t.id;
    CHECK(id >= 0);
    InteractiveRouter r(b.p.pcb, b.p.schematic);
    CHECK(r.beginDrag(id, {35, 23}));
    CHECK(r.preview().active && r.preview().kind == "drag" && r.preview().hiddenTracks.size() >= 1);
    const RoutePreview& pv = r.moveTo({35, 22.7});
    CHECK(!pv.blocked);
    CHECK(!pv.shovedTracks.empty());
    const RouteChanges ch = r.commit();
    CHECK(ch.ok);
    for (int lane : b.lane) CHECK(netRouted(b.p, lane));
    CHECK(routingProblems(b.p) == 0);
    bool moved = false;
    for (const auto& t : b.p.pcb.tracks)
        moved = moved || (t.net == b.lane[2] && std::fabs(t.a.y - 22.7) < 1e-9 && std::fabs(t.b.y - 22.7) < 1e-9);
    CHECK(moved);
    // Cancel leaves the board untouched; a locked track cannot be dragged.
    LaneBoard c = laneBoard();
    const auto tracksBefore = c.p.pcb.tracks.size();
    InteractiveRouter rc(c.p.pcb, c.p.schematic);
    CHECK(rc.beginDrag(c.p.pcb.tracks[2].id, {30, 22}));
    rc.moveTo({30, 25});
    rc.cancel();
    CHECK(c.p.pcb.tracks.size() == tracksBefore);
    c.p.pcb.tracks[2].locked = true;
    CHECK(!rc.beginDrag(c.p.pcb.tracks[2].id, {30, 22}));
}

TEST(router_commit_refuses_a_stale_board) {
    LaneBoard b = laneBoard();
    InteractiveRouter r(b.p.pcb, b.p.schematic);
    CHECK(r.beginRoute(b.from, 0));
    r.moveTo({20, 26});
    b.p.pcb.tracks.pop_back();  // the board changes behind the router's back
    const auto n = b.p.pcb.tracks.size();
    const RouteChanges ch = r.commit();
    CHECK(!ch.ok && !ch.error.empty());
    CHECK(b.p.pcb.tracks.size() == n);
}

TEST(router_tunes_length_with_meanders) {
    Project p;
    auto& s = p.schematic;
    const int r1 = placeR(p, {8, 20}), r2 = placeR(p, {42, 20});
    wire(s, r1, "2", r2, "1");
    p.schematicChanged();
    const int net = s.netOf({r1, 1});
    InteractiveRouter r(p.pcb, s);
    CHECK(r.beginRoute(padAt(p, r1, 1), 0));
    r.moveTo(padAt(p, r2, 0));
    CHECK(r.commit().ok);
    const double before = routedNetLength(p.pcb, net);
    const LengthTuneResult t = tuneTrackLength(p.pcb, s, p.pcb.tracks.front().id, before + 3);
    CHECK(t.ok);
    CHECK_NEAR(t.after, before + 3, 0.05);
    CHECK_NEAR(routedNetLength(p.pcb, net), before + 3, 0.05);
    CHECK(netRouted(p, net));
    CHECK(routingProblems(p) == 0);
    // Without a target, a net outside any matched group has nothing to match.
    CHECK(!tuneTrackLength(p.pcb, s, p.pcb.tracks.front().id, 0).ok);
}

TEST(c_api_router) {
    const int rc = sieda_c_api_router_test();
    if (rc != 0) std::printf("    c api router step %d failed\n", rc);
    CHECK(rc == 0);
}

TEST(router_keeps_an_autorouted_board_drc_clean) {
    // Routes and drags with shove at pseudo-random spots of an autorouted board; every commit must leave DRC as
    // clean as it was (no new error, no clearance warning).
    unsigned seed = 2024;
    auto rnd = [&seed] {
        seed = seed * 1103515245u + 12345u;
        return ((seed >> 8) & 0xFFFFFF) / double(0xFFFFFF);
    };
    Project p;
    auto& s = p.schematic;
    std::vector<int> ids;
    for (int i = 0; i < 24; ++i) ids.push_back(s.addComponent(ComponentKind::Resistor, "1k", {(i % 6) * 100.0, (i / 6) * 100.0}));
    for (size_t k = 0; k < ids.size(); ++k) {
        const size_t other = static_cast<size_t>(rnd() * ids.size()) % ids.size();
        if (other != k) s.connect({ids[k], 1}, {ids[other], 0});
    }
    p.pcb.settings.width = 42;
    p.pcb.settings.height = 30;
    p.schematicChanged();
    p.pcb.autoPlace(s, true);
    p.pcb.autoRoute(s);
    CHECK(routingProblems(p) == 0);
    int commits = 0, shoved = 0;
    for (int it = 0; it < 30; ++it) {
        InteractiveRouter r(p.pcb, s);
        const bool drag = rnd() < 0.35 && !p.pcb.tracks.empty();
        bool ok;
        if (drag) {
            const Track& t = p.pcb.tracks[static_cast<size_t>(rnd() * p.pcb.tracks.size()) % p.pcb.tracks.size()];
            ok = r.beginDrag(t.id, (t.a + t.b) * 0.5);
        } else {
            const auto pads = p.pcb.pads(s);
            const Pad& pd = pads[static_cast<size_t>(rnd() * pads.size()) % pads.size()];
            ok = r.beginRoute(pd.position, 0);
        }
        if (!ok) continue;
        const Vec2 at = r.preview().end;
        for (int m = 0; m < 3; ++m) {
            const Vec2 c = drag ? at + Vec2{(rnd() - 0.5) * 2, (rnd() - 0.5) * 2}
                                : Vec2{rnd() * p.pcb.settings.width, rnd() * p.pcb.settings.height};
            shoved += r.moveTo(c).shovedTracks.empty() ? 0 : 1;
            if (!drag && rnd() < 0.5) r.fixHead();
        }
        if (!r.commit().ok) continue;
        ++commits;
        const int problems = routingProblems(p);
        CHECK(problems == 0);
        if (problems) break;
    }
    CHECK(commits >= 20);
    CHECK(shoved > 0);
}

// ======================================================================= signal and power integrity

namespace {
/// A logic part: 1 OUT (output), 2 IN (input), 3 VDD, 4 GND, 5–8 not connected; drawing `loadAmps` from VDD.
CustomPartSpec siLogicPart(const char* name, double loadAmps) {
    CustomPartSpec spec;
    spec.name = name;
    spec.package.type = "SOIC";
    spec.package.pinCount = 8;
    auto add = [&](const char* number, const char* pinName, PinType type) {
        CustomPin p;
        p.number = number;
        p.name = pinName;
        p.type = type;
        spec.pins.push_back(p);
    };
    add("1", "OUT", PinType::Output);
    add("2", "IN", PinType::Input);
    add("3", "VDD", PinType::PowerIn);
    add("4", "GND", PinType::PowerIn);
    add("5", "NC5", PinType::NoConnect);
    add("6", "NC6", PinType::NoConnect);
    add("7", "NC7", PinType::NoConnect);
    add("8", "NC8", PinType::NoConnect);
    if (loadAmps > 0) spec.model.loads.push_back({"VDD", "GND", loadAmps});
    return spec;
}

void siPlace(Project& p, int id, Vec2 at) {
    Component* c = p.schematic.find(id);
    c->pcb.placed = true;
    c->pcb.position = at;
}

Vec2 siPad(const Project& p, int comp, const char* pinName) {
    const int pin = p.schematic.pinIndex(comp, pinName);
    for (const auto& pad : p.pcb.pads(p.schematic))
        if (pad.componentId == comp && pad.pinIndex == pin) return pad.position;
    CHECK(false);
    return {};
}

void siTrack(Project& p, int net, Vec2 a, Vec2 b, int layer = 0, double width = 0.25) {
    Track t;
    t.net = net;
    t.layer = layer;
    t.width = width;
    t.a = a;
    t.b = b;
    p.pcb.addTrack(t);
}

/// U1 (driver) → CLK → U2 (receiver), 120 × 40 mm, 4 layers, supplied from J1 (+3V3 / GND). U1 and U2 draw 0.25 A.
struct SiBoard {
    Project p;
    int u1 = -1, u2 = -1, j1 = -1, clk = -1, vcc = -1;
};
SiBoard siBoard() {
    SiBoard b;
    Project& p = b.p;
    auto& s = p.schematic;
    const std::string part = p.addCustomPart(siLogicPart("SI-LOGIC", 0.25));
    b.u1 = s.addCustomComponent(part, "", {0, 0});
    b.u2 = s.addCustomComponent(part, "", {400, 0});
    b.j1 = s.addComponent(ComponentKind::Connector, "PWR", {-200, 0});
    int vcc = s.addComponent(ComponentKind::NetLabel, "+3V3", {-100, -100});
    int gnd = s.addComponent(ComponentKind::Ground, "", {-100, 100});
    wire(s, b.u1, "OUT", b.u2, "IN");
    for (int u : {b.u1, b.u2}) {
        wire(s, u, "VDD", vcc, "N");
        wire(s, u, "GND", gnd, "GND");
    }
    wire(s, b.j1, "1", vcc, "N");
    wire(s, b.j1, "2", gnd, "GND");
    p.schematicChanged();
    p.pcb.settings.width = 120;
    p.pcb.settings.height = 40;
    p.pcb.settings.layerCount = 4;
    siPlace(p, b.u1, {10, 20});
    siPlace(p, b.u2, {100, 20});
    siPlace(p, b.j1, {5, 5});
    b.clk = s.netOf({b.u1, pin(s, b.u1, "OUT")});
    b.vcc = s.netOf({b.u1, pin(s, b.u1, "VDD")});
    return b;
}

/// Routes CLK on the top layer: down from U1 OUT to y = 30, along it, and up into U2 IN.
void siRouteClock(SiBoard& b) {
    const Vec2 a = siPad(b.p, b.u1, "OUT"), c = siPad(b.p, b.u2, "IN");
    siTrack(b.p, b.clk, a, {a.x, 30});
    siTrack(b.p, b.clk, {a.x, 30}, {c.x, 30});
    siTrack(b.p, b.clk, {c.x, 30}, c);
}

const char* kSampleIbis = R"(|  Sample IBIS 4.2 file for SiEDA tests
[IBIS Ver]      4.2
[File Name]     sample.ibs
[Comment Char]  |_char
[Component]     SAMPLE-MCU
[Manufacturer]  Example Semiconductor
[Package]
| variable  typ     min     max
R_pkg       0.25    0.2     0.3
L_pkg       3.0nH   2.5nH   3.5nH
C_pkg       0.5pF   0.4pF   0.6pF
[Pin]  signal_name  model_name  R_pin  L_pin  C_pin
1      PA0          GPIO_FAST   0.2    2.0nH  0.4pF
2      PA1          IN_ONLY     NA     NA     NA
3      VDD          POWER
4      VSS          GND
[Model Selector] GPIO
GPIO_FAST   High drive
GPIO_SLOW   Low drive
[Model]  GPIO_FAST
Model_type   I/O
Polarity     Non-Inverting
Vinl = 0.99V
Vinh = 2.31V
C_comp   4.0pF   3.0pF   5.0pF
[Voltage Range]   3.3V   3.0V   3.6V
[Pulldown]
| V        I(typ)     I(min)     I(max)
-3.3      -0.10      -0.08      -0.12
 0.0       0.0        0.0        0.0
 0.33      13.2mA     11mA       16.5mA
 0.66      26.4mA     22mA       33mA
 0.99      39.6mA     33mA       49.5mA
 3.3       60mA       50mA       75mA
[Pullup]
-3.3       0.10       0.08       0.12
 0.0       0.0        0.0        0.0
 0.33     -11mA      -9.4mA     -13.2mA
 0.66     -22mA      -18.9mA    -26.4mA
 0.99     -33mA      -28.3mA    -39.6mA
 3.3      -50mA      -42mA      -60mA
[GND Clamp]
-1.0      -50mA      NA         NA
 0.0       0          NA         NA
[Ramp]
| variable     typ          min          max
dV/dt_r        1.2/0.6n     1.0/0.9n     1.4/0.4n
dV/dt_f        1.2/0.75n    1.0/1.0n     1.4/0.5n
R_load = 50
[Model]  IN_ONLY
Model_type   Input
Vinl = 0.8
Vinh = 2.0
C_comp   2.5pF   NA   NA
[Model]  GPIO_SLOW
Model_type   Output
C_comp   3pF
[Voltage Range]   3.3   3.0   3.6
[Rising Waveform]
R_fixture = 50
V_fixture = 0
0.0ns    0.0     0.0    0.0
1.0ns    0.4     0.3    0.5
2.0ns    1.6     1.2    1.8
3.0ns    2.0     1.6    2.2
[End]
)";
}  // namespace

TEST(si_ibis_import) {
    IbisFile f = parseIbis(kSampleIbis);
    CHECK(f.version == "4.2" && f.fileName == "sample.ibs");
    CHECK(f.components.size() == 1 && f.components[0].name == "SAMPLE-MCU");
    CHECK(f.components[0].manufacturer == "Example Semiconductor");
    CHECK_NEAR(f.components[0].lPkg.typ, 3e-9, 1e-15);
    CHECK(f.components[0].pins.size() == 4);
    CHECK(f.components[0].pins[0].model == "GPIO_FAST");
    CHECK_NEAR(f.components[0].pins[0].lPin, 2e-9, 1e-15);
    CHECK(!std::isfinite(f.components[0].pins[1].rPin));  // "NA"
    CHECK(f.models.size() == 3);
    const IbisModel* m = f.findModel("gpio_fast");
    CHECK(m && m->type == "I/O" && m->canDrive());
    if (!m) return;
    CHECK(f.findModel("GPIO") == m);  // a [Model Selector] resolves to its first model
    CHECK(f.findModel("IN_ONLY") && !f.findModel("IN_ONLY")->canDrive());
    CHECK_NEAR(m->cComp.max, 5e-12, 1e-18);
    CHECK(m->pulldown.size() == 6 && m->pullup.size() == 6 && m->gndClamp.size() == 2);
    CHECK_NEAR(m->rampRiseDt.typ, 0.6e-9, 1e-18);
    CHECK_NEAR(m->rLoad, 50, 1e-12);
    CHECK(f.warnings.empty());

    // Typ corner: the pulldown is a 25 Ω line near 0 V and the pullup 30 Ω → 27.5 Ω; the 20–80 % ramp of 0.6 ns is a
    // 0.8 ns 10–90 % edge; thresholds, C_comp and the pin's package parasitics come along.
    const IbisPin* pin1 = &f.components[0].pins[0];
    DriverModel d = driverFromIbis(*m, "typ", &f.components[0], pin1);
    CHECK(d.source == "ibis" && d.id == "ibis:GPIO_FAST" && d.type == "io");
    CHECK_NEAR(d.rOut, 27.5, 0.01);
    CHECK_NEAR(d.riseTime, 0.8e-9, 1e-15);
    CHECK_NEAR(d.fallTime, 1.0e-9, 1e-15);
    CHECK_NEAR(d.cComp, 4e-12, 1e-18);
    CHECK_NEAR(d.vih, 2.31, 1e-12);
    CHECK_NEAR(d.vil, 0.99, 1e-12);
    CHECK_NEAR(d.vHigh, 3.3, 1e-12);
    CHECK_NEAR(d.lPkg, 2e-9, 1e-15);  // R_pin / L_pin / C_pin override [Package]
    CHECK_NEAR(d.rPkg, 0.2, 1e-12);
    // Max (strong / fast) corner: stiffer and faster; [Package] typ values when the pin has none.
    DriverModel fast = driverFromIbis(*m, "max", &f.components[0]);
    CHECK(fast.rOut < d.rOut && fast.riseTime < d.riseTime);
    CHECK_NEAR(fast.lPkg, 3e-9, 1e-15);
    // A model with a rising waveform but no [Ramp]: 20 % (0.4 V) at 1.0 ns, 80 % (1.6 V) at 2.0 ns.
    DriverModel slow = driverFromIbis(*f.findModel("GPIO_SLOW"));
    CHECK_NEAR(slow.riseTime, 1.0e-9 * 4 / 3, 1e-14);
    // Errors and JSON.
    bool threw = false;
    try {
        parseIbis("[IBIS Ver] 5.0\n[Component] X\n");
    } catch (const IbisError&) {
        threw = true;
    }
    CHECK(threw);
    Json j = ibisFileJson(f);
    CHECK(j.get("models").size() == 3 && j.get("components")[0].get("pins").size() == 4);
    CHECK(driverModelFromJson(driverModelToJson(d)).rOut == d.rOut);
    CHECK(findLogicFamily("lvcmos33") && findLogicFamily("sstl15")->rTerm > 0 && !findLogicFamily("nope"));
}

TEST(si_physics_reference_values) {
    BoardSettings s;
    s.layerCount = 4;
    // FR-4 stripline propagates at √4.4 / c ≈ 7.0 ps/mm (≈ 178 ps/inch, the textbook figure).
    CHECK_NEAR(propagationDelayPerMm(s, 1, 0.15) * 25.4e12, 178, 1.5);
    // Microstrip εeff lies between (εr + 1)/2 and εr; Hammerstad–Jensen at w/h = 2 is 3.34 for εr 4.4.
    const double h = impedanceReferenceHeight(s, 0);
    CHECK_NEAR(effectivePermittivity(s, 0, 2 * h), 2.7 + 1.7 / std::sqrt(7.0), 1e-9);
    CHECK(effectivePermittivity(s, 0, 0.05) > 2.7 && effectivePermittivity(s, 0, 50) < 4.4);
    // Johnson & Graham §7.3 worked example: 0.063 in barrel, 0.028 in pad, 0.050 in anti-pad → 0.50 pF; about 1 nH for
    // a 0.063 in via of 0.028 in diameter.
    CHECK_NEAR(viaCapacitance(0.063 * 25.4, 0.028 * 25.4, 0.050 * 25.4, 4.4), 0.50e-12, 0.01e-12);
    CHECK_NEAR(viaInductance(0.063 * 25.4, 0.028 * 25.4), 1.02e-9, 0.02e-9);
    // Critical length: 1 ns edge on FR-4 stripline → about 24 mm ("1 inch per ns").
    CHECK_NEAR(criticalLength(1e-9, propagationDelayPerMm(s, 1, 0.15)), 23.8, 0.3);
    // Elliptic integral: K(0) = π/2, K(1/√2) = 1.854075.
    CHECK_NEAR(ellipticK(0), kPi / 2, 1e-12);
    CHECK_NEAR(ellipticK(std::sqrt(0.5)), 1.8540746773, 1e-9);
    // Cohn's stripline in air at w/b = 1 against Pozar's closed form 30π·b/(W + 0.441b) = 65.4 Ω; far apart the even
    // and odd modes meet the single line; close together they split.
    auto zs = coupledStriplineImpedance(1.0, 1e3, 1.0, 1.0);
    CHECK_NEAR(zs.first, zs.second, 1e-9);
    CHECK_NEAR(zs.first, 30 * kPi / 1.441, 0.02 * 65.4);
    auto zc = coupledStriplineImpedance(0.15, 0.15, 0.4, 4.4);
    CHECK(zc.first > zc.second && zc.second > 0);
}

TEST(si_crosstalk_coupling) {
    BoardSettings s;
    s.layerCount = 4;
    // Near-end crosstalk falls with spacing, saturates once 2·TD ≥ RT; the stripline has no far-end term.
    auto m1 = crosstalkCoupling(s, 0, 0.3, 0.3, 0.3, 50, 0.5e-9);
    auto m2 = crosstalkCoupling(s, 0, 0.3, 0.3, 0.9, 50, 0.5e-9);
    CHECK(m1.next > m2.next && m2.next > 0);
    CHECK(m1.next > 0.01 && m1.next < 0.15);  // a few percent for spacing = width over FR-4
    CHECK(m1.fext < 0 && m1.kc < m1.kl);       // microstrip: inductive coupling dominates → negative FEXT
    CHECK_NEAR(crosstalkCoupling(s, 0, 0.3, 0.3, 0.3, 500, 0.5e-9).next, m1.kb, 1e-12);  // saturated
    CHECK(crosstalkCoupling(s, 0, 0.3, 0.3, 0.3, 2, 0.5e-9).next < m1.kb);
    auto st = crosstalkCoupling(s, 1, 0.15, 0.15, 0.15, 50, 0.5e-9);
    CHECK_NEAR(st.fext, 0, 1e-12);
    CHECK_NEAR(st.kl, st.kc, 1e-12);
    CHECK(st.kb > 0.005 && st.kb < 0.2);
}

TEST(si_transmission_line_lattice) {
    // 25 Ω source, 50 Ω / 1 ns line, open end, a 1 V step with a 20 ps edge. Lattice diagram: the far end sees
    // 2 × 2/3 = 1.333 V at TD, then 1.333 − 0.444 = 0.889 V at 3 TD (source reflection −1/3), 1.037 V at 5 TD.
    TlNetwork n(2);
    n.addLine(0, 1, 50, 1e-9);
    n.driverNode = 0;
    n.rSource = 25;
    n.vHigh = 1;
    n.riseTime = n.fallTime = 20e-12;
    n.capacitance[1] = 1e-16;
    const double dt = 1e-12;
    TlRun run = simulateTl(n, {0, 1}, dt, 0.1e-9, 20e-9, 40e-9);
    auto at = [&](size_t probe, double t) { return run.probes[probe][static_cast<size_t>(std::lround(t / dt))]; };
    CHECK_NEAR(at(0, 0.6e-9), 2.0 / 3, 0.005);  // launched step
    CHECK_NEAR(at(1, 0.9e-9), 0.0, 0.005);      // nothing has arrived yet
    CHECK_NEAR(at(1, 1.6e-9), 4.0 / 3, 0.005);
    CHECK_NEAR(at(1, 3.6e-9), 8.0 / 9, 0.005);
    CHECK_NEAR(at(1, 5.6e-9), 28.0 / 27, 0.005);
    CHECK_NEAR(at(1, 19e-9), 1.0, 0.01);  // settles to the source level
    EdgeMetrics e = measureEdges(run, 1, 0.7, 0.3);
    CHECK_NEAR(e.overshoot, 1.0 / 3, 0.01);
    CHECK_NEAR(e.flightTime, 1e-9, 0.02e-9);
    CHECK(e.reachesHigh && e.reachesLow && e.ringbackHigh > 0);
    // Matched source (series termination): the far end steps cleanly to 1 V with no overshoot.
    n.rSource = 50;
    EdgeMetrics m = measureEdges(simulateTl(n, {0, 1}, dt, 0.1e-9, 20e-9, 40e-9), 1, 0.7, 0.3);
    CHECK(m.overshoot < 0.01 && m.undershoot < 0.01);
    // Parallel termination at the end to a 0.5 V rail: DC start at the divider, final level 0.5 + (1 − 0.5)·50/100.
    TlNetwork t = n;
    t.conductance[1] = 1 / 50.0;
    t.railCurrent[1] = 0.5 / 50.0;
    TlRun tr = simulateTl(t, {1}, dt, 0.1e-9, 20e-9, 40e-9);
    CHECK_NEAR(tr.probes[0][10], 0.25, 1e-6);
    CHECK_NEAR(tr.probes[0][static_cast<size_t>(15e-9 / dt)], 0.75, 0.005);
    // RC: 1 kΩ into 1 nF (no line) reaches 63 % at τ = 1 µs.
    TlNetwork rc(1);
    rc.rSource = 1000;
    rc.vHigh = 1;
    rc.riseTime = rc.fallTime = 1e-12;
    rc.capacitance[0] = 1e-9;
    TlRun r2 = simulateTl(rc, {0}, 1e-9, 0, 10e-6, 20e-6);
    CHECK_NEAR(r2.probes[0][1000], 1 - std::exp(-1.0), 0.003);
}

TEST(si_net_reflections_and_termination) {
    SiBoard b = siBoard();
    siRouteClock(b);
    SiNetResult r = analyzeNet(b.p, b.clk);
    CHECK(r.error.empty());
    CHECK(r.routed && !r.estimated);
    CHECK(r.driverComponent == b.u1 && !r.driverAssumed);
    CHECK(r.driver.id == "lvcmos33");  // default family from the 3.3 V supply
    CHECK(r.length > 90 && r.length < 120);
    CHECK(r.critical && r.criticalLength < r.length);
    CHECK_NEAR(r.z0Trunk, trackImpedance(b.p.pcb.settings, 0, 0.25), 1e-9);
    CHECK(r.receivers.size() == 1 && r.receivers[0].connected);
    if (r.receivers.empty()) return;
    // An unterminated 30 Ω driver into a ~59 Ω, 100 mm line rings well past 15 %.
    const EdgeMetrics e = r.receivers[0].metrics;
    CHECK(!r.ok && e.overshoot / (e.vHigh - e.vLow) > 0.15);
    CHECK_NEAR(e.vHigh, 3.3, 0.05);
    CHECK(e.flightTime > 0.5e-9 && e.flightTime < 1.0e-9);
    // Series termination advice: Z0 − R_out, E24; the what-if run with it overshoots far less.
    CHECK_NEAR(r.recommendedSeriesR, nearestStandardValue(r.z0Trunk - 30, ESeries::E24), 1e-9);
    CHECK(r.terminatedMetrics.overshoot < 0.5 * e.overshoot);
    Json j = siNetJson(r, 300);
    CHECK(j.get("waveform").get("time").size() <= 300);
    CHECK(j.get("waveform").get("receiver").size() == j.get("waveform").get("time").size());
    CHECK(j.get("terminatedWaveform").get("time").size() > 10);
    CHECK(j.get("receivers")[0].get("metrics").get("overshootPercent").asNumber() > 15);
    // The what-if parameter adds a series resistor.
    SiNetResult w = analyzeNet(b.p, b.clk, r.recommendedSeriesR);
    CHECK(!w.receivers.empty() && w.receivers[0].metrics.overshoot < 0.5 * e.overshoot);

    // A 22 Ω series resistor in the schematic is found and used: the driver sits behind it.
    SiBoard t = siBoard();
    auto& s = t.p.schematic;
    s.removeWire(s.wires().front().id);
    int r1 = s.addComponent(ComponentKind::Resistor, "22", {200, 0});
    wire(s, t.u1, "OUT", r1, "1");
    wire(s, r1, "2", t.u2, "IN");
    t.p.schematicChanged();
    siPlace(t.p, r1, {16, 30});
    const int line = s.netOf({r1, 1});
    const Vec2 a = siPad(t.p, r1, "2"), c = siPad(t.p, t.u2, "IN");
    siTrack(t.p, line, a, {c.x, a.y});
    siTrack(t.p, line, {c.x, a.y}, c);
    SiNetResult sr = analyzeNet(t.p, line);
    CHECK(sr.error.empty());
    CHECK(sr.seriesRef == s.find(r1)->ref && sr.seriesR == 22 && sr.driverComponent == t.u1);
    CHECK(sr.receivers.size() == 1 && !sr.receivers.empty() && sr.receivers[0].metrics.overshoot < e.overshoot);

    // Unrouted: estimated on straight-line lengths.
    SiBoard u = siBoard();
    SiNetResult ur = analyzeNet(u.p, u.clk);
    CHECK(ur.error.empty() && ur.estimated && !ur.routed && ur.length > 80);

    // Power nets are not signal nets.
    CHECK(!analyzeNet(b.p, b.vcc).error.empty());
    // The net list for the panel puts the critical clock first.
    const Json nets = siNetsJson(b.p);
    CHECK(nets.size() >= 1 && nets[0].get("net").asInt() == b.clk);
    CHECK(nets.size() >= 1 && nets[0].get("critical").asBool());
}

TEST(si_ibis_model_drives_the_net) {
    SiBoard b = siBoard();
    siRouteClock(b);
    IbisFile f = parseIbis(kSampleIbis);
    DriverModel slow = driverFromIbis(*f.findModel("GPIO_FAST"), "min", &f.components[0]);
    b.p.si.models.push_back(slow);
    b.p.si.componentModels[b.p.schematic.find(b.u1)->ref] = slow.id;
    SiNetResult r = analyzeNet(b.p, b.clk);
    CHECK(r.driver.source == "ibis" && r.driver.id == "ibis:GPIO_FAST");
    CHECK_NEAR(r.driver.riseTime, 0.9e-9 * 4 / 3, 1e-14);
    // The IBIS model on U2's input pin sets the receiver.
    b.p.si.pinModels[b.p.schematic.find(b.u2)->ref + ".2"] = slow.id;
    SiNetResult r2 = analyzeNet(b.p, b.clk);
    CHECK(!r2.receivers.empty() && r2.receivers[0].model == slow.name);
    // Settings persist with the project.
    b.p.si.signOff = true;
    b.p.si.rails.push_back({"+3V3", 3, 0.4, 0.6});
    Project q = Project::fromJson(b.p.toJson());
    CHECK(q.si.signOff && q.si.models.size() == 1 && q.si.models[0].id == slow.id);
    CHECK(!q.si.models.empty() && std::fabs(q.si.models[0].riseTime - slow.riseTime) < 1e-18);
    CHECK(q.si.componentModels.size() == 1 && q.si.pinModels.size() == 1);
    CHECK(q.si.rail("+3V3") && q.si.rail("+3V3")->transientCurrent == 0.4);
    // A default project writes no SI settings.
    CHECK(!Project().toJson().has("signalIntegrity"));
}

TEST(si_crosstalk_and_return_path) {
    SiBoard b = siBoard();
    siRouteClock(b);
    auto& s = b.p.schematic;
    // A victim net running 0.2 mm (edge to edge) beside the clock for 60 mm.
    int v1 = s.addComponent(ComponentKind::Resistor, "1k", {200, 200});
    int v2 = s.addComponent(ComponentKind::Resistor, "1k", {300, 200});
    int lab = s.addComponent(ComponentKind::NetLabel, "SENSE", {250, 200});
    wire(s, v1, "1", lab, "N");
    wire(s, v2, "1", lab, "N");
    b.p.schematicChanged();
    const int victim = s.netOf({v1, 0});
    siTrack(b.p, victim, {25, 30.45}, {85, 30.45});
    auto pairs = crosstalkPairs(b.p);
    const CrosstalkPair* hit = nullptr;
    for (const auto& p : pairs)
        if (p.aggressor == b.clk && p.victim == victim) hit = &p;
    CHECK(hit != nullptr);
    if (hit) {
        CHECK_NEAR(hit->coupledLength, 60, 0.01);
        CHECK_NEAR(hit->spacing, 0.2, 1e-6);
        CHECK(hit->next > 0.01 && hit->fext < 0 && hit->noise > 0);
    }
    b.p.si.crosstalkLimit = 0.005;
    bool flagged = false;
    for (const auto& v : signalPowerIntegrityChecks(b.p)) flagged |= v.code == "SI_CROSSTALK";
    CHECK(flagged);

    // Return path: a GND plane on Inner 1 cut by a 2 mm wide +3V3 track running under the clock. The clock is not a
    // "fast" net by name, so it is checked once a model is assigned to it.
    SiBoard g = siBoard();
    siRouteClock(g);
    g.p.pcb.zones.push_back({"GND", 1, true, 0});
    siTrack(g.p, g.vcc, {50, 2}, {50, 38}, 1, 2.0);
    Via stitch;  // keeps the plane right of the cut connected (J1's ground pin anchors the left part)
    stitch.net = g.p.schematic.groundNet();
    stitch.position = {110, 10};
    g.p.pcb.addVia(stitch);
    CHECK(returnPathIssues(g.p).empty());
    g.p.si.netModels[g.p.schematic.nets()[static_cast<size_t>(g.clk)].name] = "lvcmos33";
    bool gapFound = false;
    for (const auto& i : returnPathIssues(g.p)) gapFound |= i.code == "SI_PLANE_GAP" && i.net == g.clk && std::fabs(i.at.x - 50) < 3;
    CHECK(gapFound);

    // Reference change: the clock dives through a via from Top (over GND on Inner 1) to Bottom (over +3V3 on Inner 2).
    SiBoard v = siBoard();
    v.p.pcb.zones.push_back({"GND", 1, true, 0});
    v.p.pcb.zones.push_back({"+3V3", 2, true, 0});
    v.p.si.netModels[v.p.schematic.nets()[static_cast<size_t>(v.clk)].name] = "lvcmos33";
    const Vec2 a = siPad(v.p, v.u1, "OUT"), c = siPad(v.p, v.u2, "IN");
    Via via;
    via.net = v.clk;
    via.position = {60, 30};
    v.p.pcb.addVia(via);
    Via up;
    up.net = v.clk;
    up.position = {c.x, 30};
    v.p.pcb.addVia(up);
    siTrack(v.p, v.clk, a, {a.x, 30});
    siTrack(v.p, v.clk, {a.x, 30}, {60, 30});
    siTrack(v.p, v.clk, {60, 30}, {c.x, 30}, 3);
    siTrack(v.p, v.clk, {c.x, 30}, c);
    auto changes = [&] {
        int n = 0;
        for (const auto& i : returnPathIssues(v.p)) n += i.code == "SI_REFERENCE_CHANGE" && (i.at - Vec2(60, 30)).length() < 1e-6;
        return n;
    };
    CHECK(changes() == 1);
    // A stitching capacitor between the planes next to the via fixes it.
    auto& vs = v.p.schematic;
    int cs = vs.addComponent(ComponentKind::Capacitor, "100n", {100, 300});
    int vl = vs.addComponent(ComponentKind::NetLabel, "+3V3", {80, 300});
    int gl = vs.addComponent(ComponentKind::Ground, "", {120, 300});
    wire(vs, cs, "1", vl, "N");
    wire(vs, cs, "2", gl, "GND");
    v.p.schematicChanged();
    siPlace(v.p, cs, {62, 33});
    CHECK(changes() == 0);
    // The net's analysis follows both layers through the vias.
    SiNetResult vr = analyzeNet(v.p, v.clk);
    CHECK(vr.error.empty() && !vr.estimated && vr.vias >= 2 && vr.sections.size() == 2);
}

TEST(pi_pdn_impedance_and_ir_drop) {
    // Formula checks.
    CHECK_NEAR(targetImpedance(3.3, 5, 1.0), 0.165, 1e-12);
    CHECK_NEAR(std::abs(capacitorImpedance(selfResonance(100e-9, 1e-9), 100e-9, 0.02, 1e-9)), 0.02, 1e-9);
    CHECK_NEAR(planeCapacitance(10000, 0.1, 4.4), 3.896e-9, 0.005e-9);
    CHECK_NEAR(sheetResistance(0.035), 0.491e-3, 0.002e-3);  // 1 oz copper ≈ 0.49 mΩ/□
    CHECK(capacitorParasitics("C_0402", 100e-9).esl < capacitorParasitics("C_0805", 100e-9).esl);
    CHECK(capacitorParasitics("CP_Radial_THT", 100e-6).esr > capacitorParasitics("C_0603", 1e-6).esr);

    // The SI board's +3V3 rail: J1 feeds U1 and U2 (0.25 A each) through 0.5 mm tracks.
    SiBoard b = siBoard();
    const Vec2 j = siPad(b.p, b.j1, "1"), v1 = siPad(b.p, b.u1, "VDD"), v2 = siPad(b.p, b.u2, "VDD");
    siTrack(b.p, b.vcc, j, {j.x, 2}, 0, 0.5);
    siTrack(b.p, b.vcc, {j.x, 2}, {v1.x, 2}, 0, 0.5);
    siTrack(b.p, b.vcc, {v1.x, 2}, v1, 0, 0.5);
    siTrack(b.p, b.vcc, {v1.x, 2}, {v2.x, 2}, 0, 0.5);
    siTrack(b.p, b.vcc, {v2.x, 2}, v2, 0, 0.5);
    auto rail = [](const Project& p) {
        for (const auto& x : analyzePdn(p))
            if (x.name == "+3V3") return x;
        return PdnRailResult{};
    };
    const PdnRailResult r = rail(b.p);
    CHECK(r.name == "+3V3");
    CHECK_NEAR(r.voltage, 3.3, 1e-12);
    CHECK(!r.voltageEstimated && !r.currentEstimated);
    CHECK_NEAR(r.dcCurrent, 0.5, 1e-12);
    CHECK_NEAR(r.transientCurrent, 0.25, 1e-12);
    CHECK_NEAR(r.target, 3.3 * 0.05 / 0.25, 1e-12);
    CHECK(r.vrmKind == "connector" && r.decaps.empty());
    CHECK(r.freq.size() == 241 && r.z.size() == 241);
    // IR drop: R = ρ·L / (w·t). The common run from J1 to U1's corner carries 0.5 A, each branch 0.25 A.
    const double rpm = kCopperResistivity * 1e-3 / (0.5e-3 * 0.035e-3);  // Ω per mm
    const double common = (j.y - 2) + (v1.x - j.x), branch1 = v1.y - 2, run2 = (v2.x - v1.x) + (v2.y - 2);
    CHECK(r.irAnalyzed);
    double d1 = 0, d2 = 0;
    for (const auto& l : r.loads) (l.componentId == b.u1 ? d1 : d2) = l.drop;
    CHECK_NEAR(d1, rpm * (0.5 * common + 0.25 * branch1), 2e-4);
    CHECK_NEAR(d2, rpm * (0.5 * common + 0.25 * run2), 2e-4);
    CHECK_NEAR(r.irWorst, d2, 1e-12);
    // No decoupling → a PI finding.
    bool noDecap = false;
    for (const auto& v : signalPowerIntegrityChecks(b.p)) noDecap |= v.code == "PI_NO_DECOUPLING";
    CHECK(noDecap);

    // Decoupling: two 100 nF 0402s at the loads, then a 10 µF bulk capacitor lowers the worst impedance.
    auto& s = b.p.schematic;
    auto addCap = [&](const char* value, const char* package, Vec2 at) {
        int c = s.addComponent(ComponentKind::Capacitor, value, {600, 0});
        s.setPackage(c, package);
        int vl = s.addComponent(ComponentKind::NetLabel, "+3V3", {580, 0});
        int gl = s.addComponent(ComponentKind::Ground, "", {620, 0});
        wire(s, c, "1", vl, "N");
        wire(s, c, "2", gl, "GND");
        b.p.schematicChanged();
        siPlace(b.p, c, at);
        return c;
    };
    addCap("100n", "C_0402", {v1.x + 3, v1.y});
    addCap("100n", "C_0402", {v2.x + 3, v2.y});
    PdnRailResult before = rail(b.p);
    CHECK(before.decaps.size() == 2);
    if (before.decaps.size() == 2) {
        CHECK_NEAR(before.decaps[0].esl, 0.4e-9, 1e-15);
        CHECK(before.decaps[0].mounting > 0.1e-9);
        CHECK(before.decaps[0].srf > 5e6 && before.decaps[0].srf < 30e6);
    }
    CHECK(!before.peaks.empty());  // the connector's inductance resonates with the 200 nF
    addCap("10u", "C_1206", {20, 10});
    PdnRailResult after = rail(b.p);
    CHECK(after.decaps.size() == 3 && !after.decaps.empty() && std::fabs(after.decaps[0].c - 10e-6) < 1e-12);
    CHECK(after.worstZ < before.worstZ);
    if (!before.compliant) CHECK(!before.recommendations.empty());
    Json pj = pdnJson(analyzePdn(b.p));
    CHECK(pj.get("rails").size() >= 1 && pj.get("rails")[0].get("curve").get("freq").size() == 241);

    // A rail poured as a plane over a GND plane adds plane capacitance ε0·εr·A/d.
    SiBoard pl = siBoard();
    pl.p.pcb.zones.push_back({"GND", 1, true, 0});
    pl.p.pcb.zones.push_back({"+3V3", 2, true, 0});
    const PdnRailResult x = rail(pl.p);
    const double gapMm = dielectricBelow(pl.p.pcb.settings, 1) ;
    CHECK(x.planeC > 0 && x.planeArea > 0.5 * 120 * 40);
    CHECK_NEAR(x.planeC, planeCapacitance(x.planeArea, gapMm, 4.4), 0.05 * x.planeC);
    CHECK(x.cavityResonance > 0.5e9 && x.cavityResonance < 1e9);  // c / (2 · 120 mm · √4.4)
    CHECK(x.irAnalyzed);  // through the pour
}

TEST(si_verification_sign_off) {
    SiBoard b = siBoard();
    siRouteClock(b);
    VerificationOptions opt;
    opt.includeManufacturing = false;
    auto stage = [](const VerificationReport& r) -> const VerificationStage* {
        for (const auto& st : r.stages)
            if (st.id == "si") return &st;
        return nullptr;
    };
    VerificationReport off = verifyDesign(b.p, opt);
    CHECK(stage(off) == nullptr);  // opt-in: designs without SI sign-off are unchanged
    b.p.si.signOff = true;
    VerificationReport on = verifyDesign(b.p, opt);
    const VerificationStage* st = stage(on);
    CHECK(st != nullptr);
    if (st) {
        CHECK(st->status == StageStatus::Warning);
        std::set<std::string> codes;
        for (const auto& v : st->findings) codes.insert(v.code);
        CHECK(codes.count("SI_OVERSHOOT") || codes.count("SI_RINGBACK"));
        CHECK(codes.count("PI_NO_DECOUPLING"));
        for (const auto& v : st->findings) CHECK(v.severity != Severity::Error);
    }

    // The C API.
    SiedaProject* api = sieda_project_new("si");
    CHECK(api != nullptr);
    char* err = nullptr;
    CHECK(sieda_si_import_ibis(api, kSampleIbis, "typ", "", &err) == 3 && err == nullptr);
    CHECK(sieda_si_import_ibis(api, "garbage", "typ", "", &err) == 0 && err != nullptr);
    sieda_string_free(err);
    err = nullptr;
    char* settings = sieda_si_settings_json(api);
    Json sj = Json::parse(settings);
    sieda_string_free(settings);
    CHECK(sj.get("models").size() == 3 && sj.get("families").size() == logicFamilies().size());
    CHECK(sieda_si_assign_model(api, "net", "CLK", "ibis:GPIO_FAST") == 1);
    CHECK(sieda_si_assign_model(api, "net", "CLK", "no-such-model") == 0);
    CHECK(sieda_si_assign_model(api, "bogus", "CLK", "lvcmos33") == 0);
    CHECK(sieda_si_assign_model(api, "net", "CLK", "") == 1);  // clears
    CHECK(sieda_si_set_options(api, 1, 0.2, 0.08) == 1);
    CHECK(sieda_pi_set_rail(api, "+3V3", 3, 0.5, 1.0) == 1);
    char* preview = sieda_ibis_parse(kSampleIbis, "max", &err);
    CHECK(preview && Json::parse(preview).get("models").size() == 3);
    sieda_string_free(preview);
    for (char* json : {sieda_si_net_list_json(api), sieda_si_crosstalk_json(api), sieda_pi_json(api), sieda_si_checks_json(api)}) {
        CHECK(json != nullptr);
        if (json) CHECK(!Json::parse(json).isNull());
        sieda_string_free(json);
    }
    char* missing = sieda_si_net_json(api, "NOPE", -1);
    CHECK(missing && Json::parse(missing).get("error").asString("").size() > 0);
    sieda_string_free(missing);
    sieda_project_free(api);
}

// ---- Scale: spatial-index DRC equivalence and the large-board benchmark -------------------------------------------

namespace {
/// A board crowded with deliberately bad copper (overlaps, near misses, acute joins, stubs, vias in pads, tight
/// holes) so that every DRC check fires, for comparing the spatial-index DRC against the pairwise scan.
Project messyBoard(uint32_t seed) {
    bench::detail::Lcg rng{seed * 2654435761u + 7u};
    Project p;
    auto& s = p.schematic;
    std::vector<int> parts;
    const int gnd = s.addComponent(ComponentKind::Ground, "", {0, 0});
    // A 48 V supply (IPC-2221 voltage spacing above the design clearance) through an isolated converter to a
    // patient-side load (two galvanic domains for the isolation-barrier check).
    const int src = s.addComponent(ComponentKind::VoltageSource, "48", {0, -100});
    const int src5 = s.addComponent(ComponentKind::VoltageSource, "5", {0, -200});
    const int hv = s.addComponent(ComponentKind::Resistor, "10k", {100, -100});
    const int dc = s.addCustomComponent(p.addCustomPart(findStandardPart("ISO-DCDC-MED")->spec), "", {200, -100});
    const int pgnd = s.addComponent(ComponentKind::NetLabel, "PGND", {300, -50});
    const int load = s.addComponent(ComponentKind::Resistor, "1k", {300, -100});
    wire(s, src, "-", gnd, "GND");
    wire(s, src, "+", hv, "1");
    wire(s, hv, "2", gnd, "GND");
    wire(s, src5, "-", gnd, "GND");
    wire(s, src5, "+", dc, "+VIN");
    wire(s, dc, "-VIN", gnd, "GND");
    wire(s, dc, "+VOUT", load, "1");
    wire(s, dc, "-VOUT", pgnd, "N");
    wire(s, load, "2", pgnd, "N");
    for (int k = 0; k < 14; ++k) {
        const int r = s.addComponent(k % 3 == 0 ? ComponentKind::Capacitor : ComponentKind::Resistor, "1k", {100.0 * k, 0});
        if (k % 2) s.setPackage(r, k % 3 == 0 ? "C_0603" : "R_0603");
        parts.push_back(r);
    }
    // Fine-pitch pads (their unconnected pins are checked against each other too); the passives get random nets.
    s.addComponent(ComponentKind::OpAmp, "LM358", {0, 200});
    s.addCustomComponent(p.addCustomPart(findStandardPart("STM32G431CBU6")->spec), "", {400, 200});
    for (int k = 0; k < 40; ++k) {
        const int a = parts[rng.next() % parts.size()], b = parts[rng.next() % parts.size()];
        const int pa = static_cast<int>(rng.next() % s.find(a)->def().pins.size());
        const int pb = static_cast<int>(rng.next() % s.find(b)->def().pins.size());
        if (a != b) s.connect({a, pa}, {b, pb});
    }
    for (int k = 0; k < 4; ++k) s.connect({parts[rng.next() % 14], 1}, {gnd, 0});
    p.pcb.settings.width = 45;
    p.pcb.settings.height = 35;
    p.pcb.settings.layerCount = seed % 2 ? 2 : 4;
    if (seed % 3 == 0) p.pcb.settings.isolationGap = 2.0;
    if (seed % 2 == 0) p.pcb.zones.push_back({"GND", 0, false, 0});
    p.pcb.settings.holes.push_back({{6, 6}, 3.2, 6.4});
    p.pcb.autoPlace(s, true);
    const auto pads = p.pcb.pads(s);
    auto anyPad = [&]() -> const Pad& { return pads[rng.next() % pads.size()]; };
    auto jitter = [&](Vec2 at, double r) { return at + Vec2{(rng.unit() - 0.5) * 2 * r, (rng.unit() - 0.5) * 2 * r}; };
    // Copper of the 48 V and the patient-side nets reaching toward the others.
    for (const Pad& a : pads)
        if (a.componentId == hv || a.componentId == load)
            for (int k = 0; k < 4; ++k) {
                Track t;
                t.net = a.net;
                t.layer = a.smdLayer;
                t.width = 0.2;
                t.a = a.position;
                t.b = jitter(anyPad().position, 1.0);
                p.pcb.addTrack(t);
            }
    for (int k = 0; k < 160; ++k) {
        const Pad& a = anyPad();
        const Pad& b = anyPad();
        Track t;
        t.net = a.net >= 0 ? a.net : b.net;
        t.layer = a.throughHole ? static_cast<int>(rng.next() % 2) * (p.pcb.settings.layerCount - 1) : a.smdLayer;
        t.width = 0.12 + 0.05 * (rng.next() % 6);
        t.a = a.position;
        t.b = k % 4 == 0 ? jitter(b.position, 0.4) : jitter(a.position, 3.0);
        p.pcb.addTrack(t);
        if (k % 5 == 0) {  // a second piece from the end: acute or right-angle joins, collinear runs
            Track u = t;
            u.a = t.b;
            u.b = jitter(t.a, 1.5);
            p.pcb.addTrack(u);
        }
    }
    for (int k = 0; k < 40; ++k) {
        Via v;
        const Pad& a = anyPad();
        v.net = a.net;
        v.position = k % 3 == 0 ? a.position : jitter(a.position, 2.0);
        v.drill = k % 7 == 0 ? 0.15 : 0.3;
        v.diameter = k % 5 == 0 ? 0.45 : 0.6;
        p.pcb.addVia(v);
    }
    return p;
}

bool sameViolations(const std::vector<RuleViolation>& a, const std::vector<RuleViolation>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].code != b[i].code || a[i].message != b[i].message || a[i].severity != b[i].severity ||
            a[i].components != b[i].components || a[i].location.x != b[i].location.x || a[i].location.y != b[i].location.y)
            return false;
    return true;
}
}  // namespace

TEST(drc_spatial_index_matches_brute_force) {
    // The spatial-index DRC (and the copper connectivity, ratsnest and routing clean-up built on the same index) must
    // report exactly what the pairwise scan reports, in the same order, on boards full of violations.
    std::set<std::string> codes;
    for (uint32_t seed = 1; seed <= 6; ++seed) {
        Project p = messyBoard(seed);
        const auto& s = p.schematic;
        const auto fast = p.pcb.runDRC(s);
        const auto fastRats = p.pcb.ratsnest(s);
        PcbLayout fastClean = p.pcb;
        fastClean.cleanupRouting(s);
        setDrcBruteForce(true);
        const auto slow = p.pcb.runDRC(s);
        const auto slowRats = p.pcb.ratsnest(s);
        PcbLayout slowClean = p.pcb;
        slowClean.cleanupRouting(s);
        setDrcBruteForce(false);
        CHECK(fast.size() > 20);
        CHECK(!netVoltageRanges(s).empty());  // the DC operating point converged: voltage spacing is checked
        CHECK(sameViolations(fast, slow));
        CHECK(fastRats == slowRats);
        bool sameTracks = fastClean.tracks.size() == slowClean.tracks.size();
        for (size_t i = 0; sameTracks && i < fastClean.tracks.size(); ++i) {
            const Track &x = fastClean.tracks[i], &y = slowClean.tracks[i];
            sameTracks = x.net == y.net && x.layer == y.layer && x.width == y.width && x.a.x == y.a.x && x.a.y == y.a.y &&
                         x.b.x == y.b.x && x.b.y == y.b.y;
        }
        CHECK(sameTracks);
        for (const auto& v : fast) codes.insert(v.code);
    }
    // The boards exercise every pairwise check.
    for (const char* code : {"DRC_SHORT", "DRC_CLEARANCE", "DRC_CLEARANCE_RULE", "DRC_HV_CLEARANCE", "DRC_FINE_PITCH_PADS",
                             "DRC_DANGLING_TRACK", "DRC_ACUTE_ANGLE", "DRC_HOLE_SPACING", "DRC_VIA_IN_PAD", "DRC_ISOLATION_GAP",
                             "DRC_DRILL_SIZE", "DRC_UNROUTED"}) {
        if (!codes.count(code)) std::printf("    not exercised: %s\n", code);
        CHECK(codes.count(code) == 1);
    }
    // A routed board too.
    Project r = bench::makeBenchBoard({5, 2, 6, true, false, 0.45}).project;
    r.pcb.autoPlace(r.schematic, true);
    r.pcb.autoRoute(r.schematic);
    const auto fast = r.pcb.runDRC(r.schematic);
    setDrcBruteForce(true);
    const auto slow = r.pcb.runDRC(r.schematic);
    setDrcBruteForce(false);
    CHECK(sameViolations(fast, slow));
}

TEST(scale_benchmark_medium_board) {
    // The CI case of the scale benchmark (Core/tests/bench, `sieda_route_bench` for the large boards): 166 parts —
    // two 96-ball BGAs, QFN / LQFP / TSSOP ICs and their passives — on 6 layers with GND and +3V3 planes. It places,
    // routes completely and passes DRC in seconds; SIEDA_BENCH_LARGE=1 also runs the 900-part, 8-layer FPGA board.
    using clock = std::chrono::steady_clock;
    auto secs = [](clock::time_point a) { return std::chrono::duration<double>(clock::now() - a).count(); };
    auto run = [&](const bench::BenchSpec& spec, bool requireComplete) {
        bench::BenchBoard b = bench::makeBenchBoard(spec);
        Project& p = b.project;
        auto t0 = clock::now();
        p.pcb.autoPlace(p.schematic, true);
        const double place = secs(t0);
        t0 = clock::now();
        const RouteStats st = p.pcb.autoRoute(p.schematic);
        const double route = secs(t0);
        t0 = clock::now();
        int errors = 0;
        for (const auto& v : p.pcb.runDRC(p.schematic)) errors += v.severity == Severity::Error;
        const double drc = secs(t0);
        std::printf("    %d parts, %d nets, %d layers: place %.2f s, route %.2f s (%d / %d), DRC %.2f s, %d errors\n",
                    b.components, b.nets, spec.layers, place, route, st.routed, st.connections, drc, errors);
        for (const auto& c : p.schematic.components()) CHECK(!c.hasFootprint() || c.pcb.placed);
        if (requireComplete) {
            CHECK(st.failed == 0 && errors == 0);
        }
        return place + route + drc;
    };
    const double total = run({1, 6, 6, true, false, 0.45}, true);
    CHECK(total < 60.0);  // ~5 s in a release build; generous for debug and sanitiser builds
    if (std::getenv("SIEDA_BENCH_LARGE")) run({2, 32, 8, true, true, 0.45}, false);
}

// ======================================================================= repeated (multi-instance) sheets

namespace {
/// A divider block on its own sheet: port IN → R1 → port OUT, R2 from OUT to ground.
struct DividerBlock {
    int sheet = 0, in = 0, r1 = 0, r2 = 0, out = 0, gnd = 0;
};
DividerBlock dividerBlock(Schematic& s) {
    DividerBlock b;
    b.sheet = s.addSheet("Channel", 1);
    s.setActiveSheet(b.sheet);
    b.in = s.addComponent(ComponentKind::NetLabel, "IN", {0, 0});
    b.r1 = s.addComponent(ComponentKind::Resistor, "10k", {80, 0});
    b.r2 = s.addComponent(ComponentKind::Resistor, "10k", {160, 60}, 90);
    b.out = s.addComponent(ComponentKind::NetLabel, "OUT", {240, 0});
    b.gnd = s.addComponent(ComponentKind::Ground, "", {160, 140});
    s.setLabelScope(b.in, LabelScope::Port);
    s.setLabelScope(b.out, LabelScope::Port);
    wire(s, b.in, "N", b.r1, "1");
    wire(s, b.r1, "2", b.out, "N");
    wire(s, b.out, "N", b.r2, "1");
    wire(s, b.r2, "2", b.gnd, "GND");
    return b;
}

int countOn(const Schematic& s, int sheet) {
    int n = 0;
    for (const auto& c : s.components()) n += c.sheet == sheet;
    return n;
}

/// Every wire stays on one sheet and every copy matches its original (the invariants of a repeated design).
bool instancesConsistent(const Schematic& s) {
    for (const auto& w : s.wires()) {
        const Component* a = s.find(w.a.component);
        const Component* b = s.find(w.b.component);
        if (!a || !b) return false;
        // Only a hand-edited file can join two sheets with a wire (ERC reports it); never on a channel.
        if (a->sheet != b->sheet && (s.findSheet(a->sheet)->instanceOf != 0 || s.findSheet(b->sheet)->instanceOf != 0)) return false;
    }
    for (const auto& c : s.components()) {
        const Sheet* sh = s.findSheet(c.sheet);
        if (!sh) return false;
        if (sh->instanceOf != 0) {
            const Component* m = s.find(c.instanceOf);
            if (!m || m->sheet != sh->instanceOf || m->kind != c.kind || m->value != c.value) return false;
        } else if (c.instanceOf != 0) {
            return false;
        }
    }
    return true;
}
}  // namespace

TEST(repeated_sheets_channels_nets_designators_and_flattening) {
    Project p;
    Schematic& s = p.schematic;
    DividerBlock b = dividerBlock(s);
    CHECK(s.repeatSheet(b.sheet, 3) == 3);
    const auto group = s.sheetInstances(b.sheet);
    CHECK(group.size() == 3 && group[0] == b.sheet);
    if (group.size() != 3) return;
    CHECK(s.findSheet(group[1])->name == "Channel [B]" && s.findSheet(group[2])->channel == "C");
    CHECK(s.findSheet(b.sheet)->channel == "A");
    CHECK(s.definitionSheet(group[2]) == b.sheet && s.isRepeated(group[1]) && !s.isRepeated(1));
    CHECK(countOn(s, group[1]) == countOn(s, b.sheet) && countOn(s, group[2]) == countOn(s, b.sheet));
    CHECK(instancesConsistent(s));
    // Designators per channel: R1 inside the block, R201 / R301 / R401 by sheet number.
    CHECK(s.find(b.r1)->logicalRef == "R1" && s.find(b.r1)->ref == "R201");
    const int r1b = s.copyOn(b.r1, group[1]), r1c = s.copyOn(b.r1, group[2]);
    CHECK(r1b > 0 && r1c > 0 && r1b != r1c);
    CHECK(s.find(r1b)->ref == "R301" && s.find(r1c)->ref == "R401");
    CHECK(s.masterOf(r1c) == b.r1 && s.masterOf(b.r1) == b.r1);
    // Each channel has its own nets: ports and local labels are per sheet.
    CHECK(s.netOf({b.r1, 1}) != s.netOf({r1b, 1}));
    CHECK(s.netOf({r1b, 1}) != s.netOf({r1c, 1}));

    // The parent sheet uses each channel through its own sheet symbol.
    s.setActiveSheet(1);
    for (size_t k = 0; k < group.size(); ++k) CHECK(s.placeSheetEntries(group[k], {300, 100.0 * static_cast<double>(k)}) == 2);
    const int g = s.addComponent(ComponentKind::Ground, "", {0, 400});
    const double volts[] = {10, 5, 2};
    std::vector<int> loads;
    for (size_t k = 0; k < group.size(); ++k) {
        int in = -1, out = -1;
        for (const auto& c : s.components())
            if (c.scope == LabelScope::SheetEntry && c.targetSheet == group[k]) (c.value == "IN" ? in : out) = c.id;
        CHECK(in > 0 && out > 0);
        const int v = s.addComponent(ComponentKind::VoltageSource, std::to_string(static_cast<int>(volts[k])),
                                     {0, 100.0 * static_cast<double>(k)});
        const int rl = s.addComponent(ComponentKind::Resistor, "1meg", {500, 100.0 * static_cast<double>(k)});
        wire(s, v, "+", in, "N");
        wire(s, v, "-", g, "GND");
        wire(s, out, "N", rl, "1");
        wire(s, rl, "2", g, "GND");
        loads.push_back(rl);
    }
    p.schematicChanged();
    DcResult dc = Simulator(s).dcOperatingPoint();
    CHECK(dc.converged);
    for (size_t k = 0; k < group.size(); ++k) CHECK_NEAR(netV(s, dc, loads[k], "1"), volts[k] * 0.4975, 0.01);
    auto erc = s.runERC();
    CHECK(!hasCode(erc, "ERC_LOCAL_LABEL_SPLIT"));
    CHECK(!hasCode(erc, "ERC_DUPLICATE_REF"));
    CHECK(!hasCode(erc, "ERC_PORT_UNUSED"));
    // Net names stay unique: the channels' OUT nets are qualified by their sheet.
    std::set<std::string> names;
    for (const auto& n : s.nets()) CHECK(names.insert(n.name).second);

    // Flattened outputs: three of every block part in the BOM, netlist and on the board.
    p.pcb.autoPlace(s, true);
    int placed = 0;
    for (const auto& c : s.components()) placed += c.hasFootprint() && c.pcb.placed;
    CHECK(placed == 2 * 3 + 3 + 3);  // the block resistors of three channels, the sources and the loads
    const std::string bom = exportBomCsv(s);
    for (const char* ref : {"R201", "R301", "R401", "R202", "R302", "R402"}) CHECK(bom.find(ref) != std::string::npos);

    // Editing a copy edits the block: value, position, new parts and wires reach every channel.
    CHECK(s.setValue(s.copyOn(b.r2, group[2]), "20k"));
    CHECK(s.find(b.r2)->value == "20k" && s.find(s.copyOn(b.r2, group[1]))->value == "20k");
    CHECK(s.moveComponent(r1b, {90, 0}));
    CHECK(s.find(b.r1)->position.x == 90 && s.find(r1c)->position.x == 90);
    CHECK(s.find(r1b)->pcb.placed);  // footprints keep their own placement through block edits
    s.setActiveSheet(group[1]);
    const int cap = s.addComponent(ComponentKind::Capacitor, "100n", {300, 60});
    CHECK(cap > 0 && s.find(cap) && s.find(cap)->sheet == group[1]);
    const int capMaster = s.masterOf(cap);
    CHECK(capMaster != cap && s.find(capMaster)->sheet == b.sheet);
    CHECK(s.copyOn(capMaster, group[2]) > 0);
    const int w = s.connect({cap, 0}, {s.copyOn(b.out, group[1]), 0});
    CHECK(w > 0);
    CHECK(s.netOf({s.copyOn(capMaster, group[2]), 0}) == s.netOf({s.copyOn(b.r1, group[2]), 1}));
    CHECK(s.netOf({capMaster, 0}) == s.netOf({b.r1, 1}));
    CHECK(instancesConsistent(s));
    // A wire split on an instance bends the block's wire everywhere.
    const int j = s.splitWire(w, {300, 0});
    CHECK(j > 0 && s.find(j) && s.find(j)->sheet == group[1]);
    CHECK(s.netOf({s.copyOn(capMaster, group[2]), 0}) == s.netOf({s.copyOn(b.r1, group[2]), 1}));
    CHECK(instancesConsistent(s));
    // Designators: the block designator is edited from any channel.
    CHECK(s.setRef(r1c, "R9"));
    CHECK(s.find(b.r1)->logicalRef == "R9" && s.find(b.r1)->ref == "R209" && s.find(r1b)->ref == "R309");
    CHECK(!s.setRef(r1b, "R2"));  // taken inside the block
    // Removing a copy removes the part from the block.
    CHECK(s.removeComponent(s.copyOn(capMaster, group[2])));
    CHECK(!s.find(cap) && !s.find(capMaster));
    CHECK(instancesConsistent(s));

    // Variants are per channel: one channel's R2 not fitted.
    CHECK(p.addVariant("Two channels"));
    CHECK(p.setVariantPart("Two channels", s.copyOn(b.r2, group[2]), 0, nullptr));
    Schematic two = p.variantSchematic("Two channels");
    CHECK(two.find(s.copyOn(b.r2, group[2]))->sourcing.dnp && !two.find(b.r2)->sourcing.dnp);

    // Persistence: the block, its channels and designators survive a round trip unchanged.
    p.schematicChanged();
    const std::string saved = p.toJson().dump();
    Project q = Project::fromJson(Json::parse(saved));
    CHECK(q.toJson().dump() == saved);
    CHECK(q.schematic.sheetInstances(b.sheet).size() == 3);
    CHECK(q.schematic.find(r1c) && q.schematic.find(r1c)->ref == "R409");
    CHECK(q.schematic.nets().size() == s.nets().size());
    CHECK(instancesConsistent(q.schematic));
    {
        Json snap = q.snapshot();
        bool sawInstance = false;
        for (const auto& sh : snap.get("sheets").items())
            if (sh.get("id").asInt() == group[1]) {
                sawInstance = true;
                CHECK(sh.get("instanceOf").asInt() == b.sheet && sh.get("channel").asString() == "B");
                CHECK(sh.get("instances").asInt() == 3);
            }
        CHECK(sawInstance);
        for (const auto& c : snap.get("components").items())
            if (c.get("id").asInt() == r1c) CHECK(c.get("logicalRef").asString() == "R9" && c.get("instanceOf").asInt() == b.r1);
    }

    // Suffix designators: R9_A, R9_B, R9_C.
    CHECK(s.setInstanceRefs(group[1], InstanceRefs::Suffix));
    CHECK(s.find(b.r1)->ref == "R9_A" && s.find(r1b)->ref == "R9_B" && s.find(r1c)->ref == "R9_C");
    CHECK(s.setSheetChannel(group[2], "CH3"));
    CHECK(s.find(r1c)->ref == "R9_CH3");
    CHECK(!s.setSheetChannel(group[2], "B"));    // used by another channel
    CHECK(!s.setSheetChannel(group[2], "a b"));  // not a label
    // Fewer channels: the last goes with its sheet symbol; one channel ends the repetition.
    CHECK(s.repeatSheet(b.sheet, 2) == 2);
    CHECK(!s.findSheet(group[2]) && !s.find(r1c));
    for (const auto& c : s.components()) CHECK(!(c.scope == LabelScope::SheetEntry && c.targetSheet == group[2]));
    CHECK(s.repeatSheet(b.sheet, 1) == 1);
    CHECK(!s.isRepeated(b.sheet) && !s.findSheet(group[1]));
    CHECK(s.find(b.r1)->ref == "R9" && s.find(b.r1)->logicalRef.empty());
    CHECK(instancesConsistent(s));
}

TEST(repeated_sheets_rules_annotation_and_erc) {
    Schematic s;
    DividerBlock b = dividerBlock(s);
    // Only a leaf sheet can be repeated, and not an instance; counts are 1 … 64.
    CHECK(s.repeatSheet(b.sheet, 0) == -1 && s.repeatSheet(b.sheet, 65) == -1 && s.repeatSheet(999, 2) == -1);
    const int child = s.addSheet("Sub", b.sheet);
    CHECK(s.repeatSheet(b.sheet, 2) == -1);
    CHECK(s.removeSheet(child, true));
    CHECK(s.repeatSheet(b.sheet, 2) == 2);
    const int inst = s.sheetInstances(b.sheet)[1];
    CHECK(s.repeatSheet(inst, 3) == -1);
    CHECK(s.addSheet("Nested", b.sheet) == -1 && s.addSheet("Nested", inst) == -1);
    const int other = s.addSheet("Other", 1);
    CHECK(!s.setSheetParent(other, inst));
    // Moving a part onto an instance moves it into the block; copies cannot be moved out of it.
    s.setActiveSheet(1);
    const int loose = s.addComponent(ComponentKind::Resistor, "1k", {0, 0});
    CHECK(s.moveToSheet({loose}, inst) == 1);
    CHECK(s.find(loose)->sheet == b.sheet && s.copyOn(loose, inst) > 0);
    CHECK(s.moveToSheet({s.copyOn(loose, inst)}, 1) == 0);
    CHECK(s.moveToSheet({loose}, 1) == 1);  // out of the block again: its copy goes
    CHECK(s.copyOn(loose, inst) == -1);

    // Annotation numbers the block inside itself and the rest of the design around the channels' designators.
    s.setActiveSheet(1);
    for (int k = 0; k < 3; ++k) s.addComponent(ComponentKind::Resistor, "1k", {100.0 * k, 300}, 0, "R?");
    auto changes = s.annotate({});
    std::set<std::string> refs;
    for (const auto& c : s.components())
        if (!isNetSymbolKind(c.kind)) CHECK(refs.insert(c.ref).second);
    CHECK(s.find(b.r1)->logicalRef == "R1" && s.find(b.r2)->logicalRef == "R2");
    CHECK(s.find(loose)->ref == "R1");  // top sheet: numbered from 1, clear of the channels' designators
    CHECK(!changes.empty());
    // ERC: a signal's global label inside the block joins every channel.
    s.setActiveSheet(b.sheet);
    const int glabel = s.addComponent(ComponentKind::NetLabel, "SYNC", {0, 100});
    wire(s, glabel, "N", b.r1, "1");
    CHECK(hasCode(s.runERC(), "ERC_GLOBAL_LABEL_IN_REPEAT"));
    CHECK(s.setLabelScope(glabel, LabelScope::Local));
    CHECK(!hasCode(s.runERC(), "ERC_GLOBAL_LABEL_IN_REPEAT"));
    // Deleting the definition deletes its instances.
    CHECK(!s.removeSheet(b.sheet, false));
    CHECK(s.removeSheet(b.sheet, true));
    CHECK(!s.findSheet(inst) && !s.find(b.r1));
    CHECK(instancesConsistent(s));
}

TEST(repeated_sheets_hostile_files_load_safely) {
    Project p;
    DividerBlock b = dividerBlock(p.schematic);
    p.schematic.repeatSheet(b.sheet, 3);
    const std::string good = p.toJson().dump();
    // Mutations of the saved links: the file still loads, and the design is consistent afterwards.
    std::vector<std::pair<std::string, std::string>> edits = {
        {"\"instanceOf\":" + std::to_string(b.sheet), "\"instanceOf\":999"},
        {"\"instanceOf\":" + std::to_string(b.sheet), "\"instanceOf\":-4"},
        {"\"instanceOf\":" + std::to_string(b.sheet), "\"instanceOf\":\"x\""},
        {"\"instanceOf\":" + std::to_string(b.r1), "\"instanceOf\":" + std::to_string(b.r2)},
        {"\"instanceOf\":" + std::to_string(b.r1), "\"instanceOf\":123456"},
        {"\"logicalRef\":\"R1\"", "\"logicalRef\":\"R2\""},
        {"\"logicalRef\":\"R1\"", "\"logicalRef\":\"" + std::string(200, 'Q') + "\""},
        {"\"channel\":\"B\"", "\"channel\":\"A\""},
        {"\"channel\":\"B\"", "\"channel\":\"  \""},
        {"\"parent\":1", "\"parent\":" + std::to_string(b.sheet)},
    };
    int loaded = 0;
    for (const auto& [from, to] : edits) {
        std::string text = good;
        for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
            text.replace(at, from.size(), to);
        try {
            Project q = Project::fromJson(Json::parse(text));
            ++loaded;
            CHECK(instancesConsistent(q.schematic));
            // A second sync changes nothing.
            const std::string once = q.toJson().dump();
            q.schematic.syncInstances();
            CHECK(q.toJson().dump() == once);
            std::set<std::string> refs;
            for (const auto& c : q.schematic.components())
                if (!isNetSymbolKind(c.kind) && q.schematic.isRepeated(c.sheet)) CHECK(refs.insert(c.ref).second);
        } catch (const JsonError&) {
        }
    }
    CHECK(loaded == static_cast<int>(edits.size()));
    // Random byte damage never crashes the loader.
    unsigned seed = 12345;
    for (int round = 0; round < 200; ++round) {
        std::string text = good;
        for (int k = 0; k < 4; ++k) {
            seed = seed * 1103515245u + 12345u;
            const size_t at = (seed >> 8) % text.size();
            text[at] = "0123456789-\"{}[],:x"[(seed >> 20) % 19];
        }
        try {
            Project q = Project::fromJson(Json::parse(text));
            CHECK(instancesConsistent(q.schematic));
        } catch (const std::exception&) {
        }
    }
}

extern "C" int sieda_c_api_capture_test(void);

TEST(schematic_capture_c_api) {
    const int rc = sieda_c_api_capture_test();
    if (rc != 0) std::printf("    capture c api step %d failed\n", rc);
    CHECK(rc == 0);
}

// ======================================================================= graphical buses

namespace {
/// A registered test part with data pins D0 … D3 and supply pins.
std::string busPart(const std::string& name) {
    CustomPartSpec spec;
    spec.name = name;
    spec.package.type = "SOIC";
    const char* pins[] = {"D0", "D1", "D2", "D3", "VCC", "GND"};
    for (int i = 0; i < 6; ++i) {
        CustomPin p;
        p.number = std::to_string(i + 1);
        p.name = pins[i];
        p.type = i < 4 ? PinType::Bidirectional : PinType::PowerIn;
        spec.pins.push_back(p);
    }
    return CustomPartRegistry::instance().registerPart(spec)->id;
}
}  // namespace

TEST(graphical_buses_entries_connectivity_erc_and_persistence) {
    Project p;
    Schematic& s = p.schematic;
    const int mcu = s.addCustomComponent(busPart("BUSMCU"), "", {0, 0});
    const int mem = s.addCustomComponent(busPart("BUSMEM"), "", {400, 0});
    CHECK(s.addBus("not a bus", {{100, -60}, {100, 60}}) == -1);
    CHECK(s.addBus("D[0..3]", {{100, -60}}) == -1);
    const int bus = s.addBus("D[0..3]", {{200, -80}, {200, 80}});
    CHECK(bus > 0 && s.findBus(bus) && s.busMembers(bus).size() == 4);
    CHECK(hasCode(s.runERC(), "ERC_BUS_NO_ENTRIES"));
    CHECK(s.connectBusToPart(bus, mcu) == 4);
    CHECK(s.connectBusToPart(bus, mem) == 4);
    CHECK(s.connectBusToPart(bus, 9999) == -1);
    for (int i = 0; i < 4; ++i) CHECK(s.netOf({mcu, i}) == s.netOf({mem, i}) && s.netOf({mcu, i}) >= 0);
    CHECK(s.netOf({mcu, 0}) != s.netOf({mcu, 1}));
    int entries = 0;
    for (const auto& c : s.components())
        if (c.bus == bus) {
            ++entries;
            CHECK(c.kind == ComponentKind::NetLabel && c.scope == LabelScope::Local);
            const Vec2 q = s.nearestBusPoint(bus, c.position);
            CHECK(std::hypot(q.x - c.position.x, q.y - c.position.y) < 15);  // drawn on the bus
        }
    CHECK(entries == 8);
    auto erc = s.runERC();
    CHECK(!hasCode(erc, "ERC_BUS_NO_ENTRIES") && !hasCode(erc, "ERC_BUS_MEMBER_UNCONNECTED") &&
          !hasCode(erc, "ERC_BUS_ENTRY_NOT_MEMBER") && !hasCode(erc, "ERC_BUS_LABEL"));
    // A member ripped out at one place only leads nowhere; an entry that is not a member is an error.
    int memD3 = -1;
    for (const auto& c : s.components())
        if (c.bus == bus && c.value == "D3" && s.netOf({c.id, 0}) == s.netOf({mem, 3})) {
            for (const auto& w : s.wires())
                if ((w.a.component == c.id && w.b.component == mem) || (w.b.component == c.id && w.a.component == mem)) memD3 = c.id;
        }
    CHECK(memD3 > 0);
    CHECK(s.removeComponent(memD3));
    CHECK(hasCode(s.runERC(), "ERC_BUS_MEMBER_UNCONNECTED"));
    CHECK(s.connectBusToPart(bus, mem) == 1);  // only D3 was open: reconnected
    CHECK(!hasCode(s.runERC(), "ERC_BUS_MEMBER_UNCONNECTED"));
    // Ripping entries: every member once; unknown members refused.
    const int bus2 = s.addBus("A[7..0]", {{0, 200}, {300, 200}, {300, 300}});
    CHECK(s.ripBusEntries(bus2, {"X9"}) == -1);
    CHECK(s.ripBusEntries(bus2, {"A1", "A0"}) == 2);
    CHECK(s.ripBusEntries(bus2, {}) == 6);
    CHECK(s.ripBusEntries(bus2, {}) == 0);
    int labelOnBus2 = -1;
    for (const auto& c : s.components())
        if (c.bus == bus2) labelOnBus2 = c.id;
    const Vec2 before = s.find(labelOnBus2)->position;
    CHECK(s.moveBus(bus2, {20, 10}));
    CHECK(s.find(labelOnBus2)->position.x == before.x + 20 && s.findBus(bus2)->points[0].y == 210);
    CHECK(s.setValue(labelOnBus2, "Q1"));
    CHECK(hasCode(s.runERC(), "ERC_BUS_ENTRY_NOT_MEMBER"));
    CHECK(s.renameBus(bus2, "A[7..0],Q1") && !s.renameBus(bus2, "nope"));
    CHECK(!hasCode(s.runERC(), "ERC_BUS_ENTRY_NOT_MEMBER"));

    // Persistence round trip, snapshot.
    p.schematicChanged();
    const std::string saved = p.toJson().dump();
    Project q = Project::fromJson(Json::parse(saved));
    CHECK(q.toJson().dump() == saved);
    CHECK(q.schematic.buses().size() == 2);
    CHECK(q.schematic.netOf({mcu, 2}) == q.schematic.netOf({mem, 2}));
    Json snap = q.snapshot();
    CHECK(snap.get("buses").size() == 2 && snap.get("buses")[0].get("members").size() == 4);
    // Removing a bus removes its entries.
    CHECK(s.removeBus(bus2) && !s.findBus(bus2) && !s.find(labelOnBus2));
    // A file without buses still loads as before.
    CHECK(Project::fromJson(Json::parse(ledProject().toJson().dump())).schematic.buses().empty());

    // Buses on a repeated sheet are copied into every channel, with their entries.
    Project r;
    Schematic& t = r.schematic;
    const int sheet = t.addSheet("Chan", 1);
    t.setActiveSheet(sheet);
    const int a = t.addCustomComponent(busPart("BUSMCU"), "", {0, 0});
    const int b2 = t.addCustomComponent(busPart("BUSMEM"), "", {400, 0});
    const int tb = t.addBus("D[0..3]", {{200, -80}, {200, 80}});
    CHECK(t.connectBusToPart(tb, a) == 4 && t.connectBusToPart(tb, b2) == 4);
    CHECK(t.repeatSheet(sheet, 2) == 2);
    const int inst = t.sheetInstances(sheet)[1];
    int copyBus = -1;
    for (const auto& bb : t.buses())
        if (bb.sheet == inst) copyBus = bb.id;
    CHECK(copyBus > 0 && t.findBus(copyBus)->instanceOf == tb);
    int copyEntries = 0;
    for (const auto& c : t.components()) copyEntries += c.sheet == inst && c.bus == copyBus;
    CHECK(copyEntries == 8);
    CHECK(t.netOf({t.copyOn(a, inst), 1}) == t.netOf({t.copyOn(b2, inst), 1}));
    CHECK(t.netOf({t.copyOn(a, inst), 1}) != t.netOf({a, 1}));  // local entries: per channel
    CHECK(t.moveBus(copyBus, {10, 0}) && t.findBus(tb)->points[0].x == 210 && t.findBus(copyBus)->points[0].x == 210);
    CHECK(instancesConsistent(t));
    const std::string repeated = r.toJson().dump();
    CHECK(Project::fromJson(Json::parse(repeated)).toJson().dump() == repeated);

    // Hostile bus data: bad points, duplicate ids, unknown sheets and entries of missing buses load safely.
    std::string text = saved;
    const std::string from = "\"buses\":[";
    const size_t at = text.find(from);
    CHECK(at != std::string::npos);
    text.insert(at + from.size(),
                "{\"id\":1,\"sheet\":1,\"name\":\"D[0..1]\",\"points\":[{\"x\":0,\"y\":0},{\"x\":1,\"y\":1}]},"
                "{\"id\":77,\"sheet\":42,\"name\":\"D[0..1]\",\"points\":[{\"x\":0,\"y\":0},{\"x\":1,\"y\":1}]},"
                "{\"id\":78,\"sheet\":1,\"name\":\"D[0..1]\",\"points\":[{\"x\":0}]},"
                "{\"id\":79,\"sheet\":1,\"name\":\"\",\"points\":\"x\"},");
    for (size_t k = text.find("\"bus\":"); k != std::string::npos; k = text.find("\"bus\":", k + 1)) {
        text.replace(k, 6, "\"bus\":9");  // every entry now names bus 9…
        break;
    }
    try {
        Project h = Project::fromJson(Json::parse(text));
        CHECK(h.schematic.buses().size() == 2);  // the duplicate id, missing sheet and bad shapes are dropped
        for (const auto& c : h.schematic.components())
            if (c.bus != 0) CHECK(h.schematic.findBus(c.bus) && h.schematic.findBus(c.bus)->sheet == c.sheet);
        h.schematic.runERC();
    } catch (const JsonError&) {
        CHECK(false);
    }
}

extern "C" int sieda_c_api_bus_test(void);

TEST(graphical_buses_c_api) {
    const int rc = sieda_c_api_bus_test();
    if (rc != 0) std::printf("    bus c api step %d failed\n", rc);
    CHECK(rc == 0);
}

// ======================================================================= multi-unit parts

namespace {
/// A quad op-amp (LM324 pinout): units A–D, supplies on pins 4 and 11 (the power unit).
CustomPartSpec quadOpAmpSpec() {
    CustomPartSpec spec;
    spec.name = "LM324-TEST";
    spec.package.type = "SOIC";
    const char* names[] = {"OUT1", "IN1-", "IN1+", "V+", "IN2+", "IN2-", "OUT2", "OUT3", "IN3-", "IN3+", "V-", "IN4+", "IN4-", "OUT4"};
    for (int i = 0; i < 14; ++i) {
        CustomPin p;
        p.number = std::to_string(i + 1);
        p.name = names[i];
        const std::string n = names[i];
        p.type = n.rfind("OUT", 0) == 0 ? PinType::Output : (n == "V+" || n == "V-") ? PinType::PowerIn : PinType::Input;
        spec.pins.push_back(p);
    }
    spec.units = {{"A", {"1", "2", "3"}}, {"B", {"7", "6", "5"}}, {"C", {"8", "9", "10"}}, {"D", {"14", "13", "12"}}};
    return spec;
}
}  // namespace

TEST(multi_unit_parts_units_package_netlist_erc_and_persistence) {
    const auto part = CustomPartRegistry::instance().registerPart(quadOpAmpSpec());
    CHECK(part->units.size() == 5);
    if (part->units.size() != 5) return;
    CHECK(part->units[4].name == "P" && part->units[4].power && !part->units[0].power);
    CHECK(part->units[0].def.pins.size() == 3 && part->units[4].def.pins.size() == 2);
    CHECK(part->units[1].pins[0] == 6);  // unit B's first pin is pin 7 (index 6)
    CHECK(part->def.pins.size() == 14 && !part->footprint.pads.empty());
    {
        Json j = customPartToJson(*part);
        CHECK(j.get("unitSymbols").size() == 5 && j.get("units").size() == 4);
        CustomPartSpec back = customPartSpecFromJson(customPartSpecToJson(part->spec));
        CHECK(back.units.size() == 4 && back.units[1].pins[0] == "7");
        CHECK(CustomPartRegistry::instance().registerPart(back)->id == part->id);
        CustomPartSpec bad = quadOpAmpSpec();
        bad.units[0].pins.push_back("99");
        bool threw = false;
        try {
            CustomPartRegistry::instance().registerPart(bad);
        } catch (const JsonError&) {
            threw = true;
        }
        CHECK(threw);
        bool dup = false;
        try {
            customPartSpecFromJson(Json::parse("{\"name\":\"X\",\"pins\":[{\"number\":\"1\",\"name\":\"A\"}],"
                                               "\"units\":[{\"name\":\"A\",\"pins\":[\"1\"]},{\"name\":\"A\",\"pins\":[\"1\"]}]}"));
        } catch (const JsonError&) {
            dup = true;
        }
        CHECK(dup);
    }

    Project p;
    Schematic& s = p.schematic;
    p.addCustomPart(part->spec);
    const int a = s.addCustomUnits(part->id, "", {0, 0});
    CHECK(a > 0 && s.find(a)->kind == ComponentKind::PartUnit && s.unitName(*s.find(a)) == "A");
    const int pkg = s.unitPackage(a);
    CHECK(pkg > 0 && s.find(pkg)->packageOnly && s.find(pkg)->kind == ComponentKind::Custom);
    CHECK(s.displayRef(*s.find(a)) == "U1A" && s.find(a)->ref == "U1");
    CHECK(s.addPartUnit(a, 1, {0, 0}) == -1);  // already placed
    CHECK(s.addPartUnit(a, 9, {0, 0}) == -1);
    const int b = s.placeNextUnit(a, {0, 200});
    const int c = s.placeNextUnit(pkg, {0, 400});
    const int d = s.placeNextUnit(a, {0, 600});
    CHECK(b > 0 && c > 0 && d > 0 && s.unitName(*s.find(d)) == "D");
    // Not all units placed: the power pins are open (error), and so on.
    {
        auto erc = s.runERC();
        bool powerMessage = false;
        for (const auto& v : erc)
            if (v.code == "ERC_POWER_PIN_UNCONNECTED" && v.message.find("place unit P of U1") != std::string::npos) powerMessage = true;
        CHECK(powerMessage);
        CHECK(hasCode(erc, "ERC_FLOATING_COMPONENT"));  // the units are not wired yet
    }
    const int pwr = s.placeNextUnit(a, {200, 0});
    CHECK(pwr > 0 && s.unitName(*s.find(pwr)) == "P" && s.placeNextUnit(a, {0, 0}) == -1);
    CHECK((s.placedUnits(pkg) == std::vector<int>{a, b, c, d, pwr}));

    // Wire unit A as a follower from a 5 V divider, supplies on P; units B–D tied off.
    const int v = s.addComponent(ComponentKind::VoltageSource, "5", {-300, 0});
    const int g = s.addComponent(ComponentKind::Ground, "", {-300, 200});
    const int r1 = s.addComponent(ComponentKind::Resistor, "10k", {-200, 0});
    const int r2 = s.addComponent(ComponentKind::Resistor, "10k", {-200, 100});
    wire(s, v, "+", r1, "1");
    wire(s, r1, "2", r2, "1");
    wire(s, r2, "2", g, "GND");
    wire(s, v, "-", g, "GND");
    CHECK(s.connect({r1, 1}, {a, 2}) >= 0);    // IN1+
    CHECK(s.connect({a, 0}, {a, 1}) >= 0);     // OUT1 → IN1-
    CHECK(s.connect({pwr, 0}, {v, 0}) >= 0);   // V+
    CHECK(s.connect({pwr, 1}, {g, 0}) >= 0);   // V-
    for (int u : {b, c, d}) {
        CHECK(s.connect({u, 0}, {u, 1}) >= 0);
        CHECK(s.connect({u, 2}, {g, 0}) >= 0);
    }
    // A unit's pins are its package's pins; only the package's pins are net members.
    CHECK(s.netOf({a, 2}) == s.netOf({pkg, 2}) && s.netOf({a, 2}) == s.netOf({r1, 1}));
    CHECK(s.netOf({b, 0}) == s.netOf({pkg, 6}));
    CHECK(s.netOf({pwr, 0}) == s.netOf({pkg, 3}));
    for (const auto& n : s.nets())
        for (const auto& pin : n.pins) CHECK(s.find(pin.component)->kind != ComponentKind::PartUnit);
    CHECK(s.isPinConnected({a, 2}) && s.isPinConnected({pkg, 2}));
    {
        auto erc = s.runERC();
        CHECK(!hasCode(erc, "ERC_POWER_PIN_UNCONNECTED") && !hasCode(erc, "ERC_UNIT_NOT_PLACED") &&
              !hasCode(erc, "ERC_UNCONNECTED_PIN") && !hasCode(erc, "ERC_FLOATING_COMPONENT") &&
              !hasCode(erc, "ERC_DUPLICATE_REF"));
    }
    // An open unit pin is reported with the unit's designator.
    const int stray = s.addCustomUnits(part->id, "", {600, 0});
    {
        bool named = false;
        for (const auto& vio : s.runERC())
            if (vio.message.find("U2A") != std::string::npos) named = true;
        CHECK(named && hasCode(s.runERC(), "ERC_UNIT_NOT_PLACED"));
    }
    CHECK(s.removeComponent(stray));
    CHECK(!s.find(stray) && s.findByRef("U2") == nullptr);  // the last unit takes its package along

    // One part on the board and in the BOM, with every pad.
    p.schematicChanged();
    p.pcb.autoPlace(s, true);
    CHECK(s.find(pkg)->pcb.placed);
    int pads = 0;
    for (const auto& pad : p.pcb.pads(s)) pads += pad.componentId == pkg;
    CHECK(pads == 14);
    for (const auto& pad : p.pcb.pads(s)) CHECK(s.find(pad.componentId)->kind != ComponentKind::PartUnit);
    const std::string bom = exportBomCsv(s);
    CHECK(bom.find("U1") != std::string::npos && bom.find("U1A") == std::string::npos);
    CHECK(Simulator(s).dcOperatingPoint().converged);

    // The designator and value of any unit are the package's.
    CHECK(s.setRef(c, "U7"));
    CHECK(s.find(pkg)->ref == "U7" && s.find(a)->ref == "U7" && s.displayRef(*s.find(pwr)) == "U7P");
    CHECK(s.setValue(b, "LM2902"));
    CHECK(s.find(pkg)->value == "LM2902" && s.find(d)->value == "LM2902");
    // Variants fit the package.
    CHECK(p.addVariant("NoAmp") && p.setVariantPart("NoAmp", a, 0, nullptr));
    CHECK(p.findVariant("NoAmp")->parts.count(pkg) == 1);
    {
        Json snap = p.snapshot();
        int units = 0, hidden = 0;
        for (const auto& comp : snap.get("components").items()) {
            if (comp.has("unitOf")) {
                ++units;
                CHECK(comp.get("kind").asInt() == static_cast<int>(ComponentKind::Custom) && comp.get("unitOf").asInt() == pkg);
            }
            if (comp.get("unitPackage").asBool(false)) ++hidden;
        }
        CHECK(units == 5 && hidden == 1);
    }

    // Persistence: the same design comes back.
    const std::string saved = p.toJson().dump();
    Project q = Project::fromJson(Json::parse(saved));
    CHECK(q.toJson().dump() == saved);
    CHECK(q.schematic.netOf({a, 2}) == q.schematic.netOf({r1, 1}));
    CHECK(q.schematic.placedUnits(pkg).size() == 5);
    // Hostile: units of missing packages and out-of-range units are dropped on load; a package left without units goes.
    std::string text = saved;
    const std::string key = "\"unitOf\":" + std::to_string(pkg);
    for (size_t k = text.find(key); k != std::string::npos; k = text.find(key, k + 1)) text.replace(k, key.size(), "\"unitOf\":424242");
    Project h = Project::fromJson(Json::parse(text));
    CHECK(h.schematic.find(a) == nullptr && h.schematic.find(pkg) == nullptr);
    CHECK(h.schematic.find(r1) != nullptr);
    std::string ranged = saved;
    const std::string unitKey = "\"unit\":2";
    const size_t at = ranged.find(unitKey);
    CHECK(at != std::string::npos);
    ranged.replace(at, unitKey.size(), "\"unit\":99");
    Project h2 = Project::fromJson(Json::parse(ranged));
    CHECK(h2.schematic.find(b) == nullptr && h2.schematic.find(a) != nullptr);
    h2.schematic.runERC();

    // Removing the package removes its units.
    CHECK(s.removeComponent(pkg));
    CHECK(!s.find(a) && !s.find(pwr));
}

TEST(multi_unit_parts_annotation_packs_units_and_repeated_sheets) {
    const auto part = CustomPartRegistry::instance().registerPart(quadOpAmpSpec());
    Schematic s;
    // Two gates placed as two separate parts: packing puts them into one package as A and B.
    const int g1 = s.addCustomUnits(part->id, "", {0, 0});
    const int g2 = s.addCustomUnits(part->id, "", {200, 0});
    const int p2 = s.placeNextUnit(g2, {200, 300});  // unit B of the second package
    CHECK(s.unitPackage(g1) != s.unitPackage(g2));
    AnnotateOptions o;
    o.packUnits = true;
    s.annotate(o);
    CHECK(s.unitPackage(g1) == s.unitPackage(g2));
    CHECK(s.unitName(*s.find(g1)) == "A" && s.unitName(*s.find(g2)) == "B");
    CHECK(s.find(g1)->ref == "U1" && s.find(g2)->ref == "U1");
    // The third gate (unit B of the old second package) fills slot C of the same quad package; the empty one goes.
    CHECK(s.unitPackage(p2) == s.unitPackage(g1) && s.unitName(*s.find(p2)) == "C");
    int packages = 0;
    for (const auto& c : s.components()) packages += c.packageOnly;
    CHECK(packages == 1);
    // Five gates need two packages.
    for (int k = 0; k < 2; ++k) s.addCustomUnits(part->id, "", {400.0 + 100 * k, 600});
    s.annotate(o);
    packages = 0;
    for (const auto& c : s.components()) packages += c.packageOnly;
    CHECK(packages == 2);
    CHECK(s.findByRef("U1") && s.findByRef("U2") && !s.findByRef("U3"));

    // Units on a repeated sheet: each channel's units belong to that channel's package.
    Schematic t;
    const int sheet = t.addSheet("Amp", 1);
    t.setActiveSheet(sheet);
    const int ua = t.addCustomUnits(part->id, "", {0, 0});
    const int ub = t.placeNextUnit(ua, {0, 200});
    CHECK(t.repeatSheet(sheet, 2) == 2);
    const int inst = t.sheetInstances(sheet)[1];
    const int ca = t.copyOn(ua, inst), cb = t.copyOn(ub, inst);
    CHECK(ca > 0 && cb > 0 && t.unitPackage(ca) == t.unitPackage(cb));
    CHECK(t.unitPackage(ca) != t.unitPackage(ua) && t.unitPackage(ca) == t.copyOn(t.unitPackage(ua), inst));
    CHECK(t.find(ca)->ref == t.find(t.unitPackage(ca))->ref && t.find(ca)->ref != t.find(ua)->ref);
    CHECK(instancesConsistent(t));
    // A unit placed on a channel goes into the block.
    t.setActiveSheet(inst);
    const int uc = t.placeNextUnit(ca, {0, 400});
    CHECK(uc > 0 && t.find(uc)->sheet == inst && t.find(t.masterOf(uc))->sheet == sheet);
    CHECK(t.placedUnits(t.unitPackage(ua)).size() == 3);
}

extern "C" int sieda_c_api_units_test(void);

TEST(multi_unit_parts_c_api) {
    const int rc = sieda_c_api_units_test();
    if (rc != 0) std::printf("    units c api step %d failed\n", rc);
    CHECK(rc == 0);
}

TEST(multi_unit_standard_part_lm324) {
    const StandardPart* lm324 = findStandardPart("lm324");
    CHECK(lm324 != nullptr);
    if (!lm324) return;
    const auto part = CustomPartRegistry::instance().registerPart(lm324->spec);
    CHECK(part->units.size() == 5 && part->units.back().name == "P" && part->units.back().power);
    // Placed whole (as plans and older designs do) it is one 14-pin symbol, exactly as before units existed.
    Schematic s;
    const int whole = s.addCustomComponent(part->id, "", {0, 0});
    CHECK(whole > 0 && s.find(whole)->def().pins.size() == 14 && !s.find(whole)->packageOnly);
    const char* json = sieda_standard_parts_json();
    CHECK(json && std::string(json).find("\"units\":[{\"name\":\"A\"") != std::string::npos);
    sieda_string_free(const_cast<char*>(json));
}

// ======================================================================= variants drive simulation

TEST(variants_drive_simulation_values_and_unfitted_parts) {
    Project p = ledProject();
    const int r = p.schematic.findByRef("R1")->id;
    const int d = p.schematic.findByRef("D1")->id;
    // No variant, nothing DNP: the simulated circuit is the design, bit for bit.
    {
        const Schematic circuit = p.simulationSchematic();
        CHECK(!circuit.omitsFromSimulation(*circuit.find(d)));
        DcResult a = Simulator(p.schematic).dcOperatingPoint(), b = Simulator(circuit).dcOperatingPoint();
        CHECK(a.converged && b.converged && a.netVoltages == b.netVoltages);
        CHECK(p.unfittedRefs().empty());
    }
    const double base = reading(Simulator(p.schematic).dcOperatingPoint(), r)->current;
    CHECK(base > 0.008 && base < 0.012);
    // A variant with R1 = 1k: a third of the current.
    const std::string k1 = "1k";
    CHECK(p.addVariant("Dim") && p.setVariantPart("Dim", r, -1, &k1) && p.setActiveVariant("Dim"));
    {
        const Schematic circuit = p.simulationSchematic();
        DcResult dc = Simulator(circuit).dcOperatingPoint();
        CHECK(dc.converged);
        CHECK_NEAR(reading(dc, r)->current, base * 330.0 / 1000.0, base * 0.15);
        CHECK(circuit.nets().size() == p.schematic.nets().size());
    }
    // A variant without the LED: no current flows, the resistor's far end rises to the supply.
    CHECK(p.addVariant("NoLed") && p.setVariantPart("NoLed", d, 0, nullptr) && p.setActiveVariant("NoLed"));
    {
        const Schematic circuit = p.simulationSchematic();
        CHECK(circuit.omitsFromSimulation(*circuit.find(d)));
        DcResult dc = Simulator(circuit).dcOperatingPoint();
        CHECK(dc.converged);
        CHECK(std::fabs(reading(dc, r)->current) < 1e-6);
        CHECK_NEAR(netV(circuit, dc, r, "2"), 5.0, 0.01);
        CHECK(reading(dc, d) == nullptr);
        CHECK((p.unfittedRefs() == std::vector<std::string>{"D1"}));
        CHECK(exportSpiceNetlist(circuit, "x").find("* D1 not fitted (DNP)") != std::string::npos);
        // Transient too.
        TransientResult tr = Simulator(circuit).transient(1e-3, 1e-4);
        CHECK(tr.ok);
    }
    // The design itself is untouched, and a base-design DNP flag is honoured as well.
    CHECK(!p.schematic.find(d)->sourcing.dnp);
    CHECK(p.setActiveVariant(""));
    p.schematic.find(d)->sourcing.dnp = true;
    CHECK(p.simulationSchematic().omitsFromSimulation(*p.simulationSchematic().find(d)));
    p.schematic.find(d)->sourcing.dnp = false;

    // Through the C API: results name the variant and the parts left out.
    SiedaProject* api = sieda_project_load_json(p.toJson().dump().c_str(), nullptr);
    CHECK(api != nullptr);
    if (!api) return;
    CHECK(sieda_set_active_variant(api, "NoLed") == 1);
    char* dc = sieda_simulate_dc(api);
    CHECK(dc && std::string(dc).find("\"variant\":\"NoLed\"") != std::string::npos &&
          std::string(dc).find("\"omitted\":[\"D1\"]") != std::string::npos);
    sieda_string_free(dc);
    char* ac = sieda_simulate_ac(api, "{}");
    CHECK(ac && std::string(ac).find("\"omitted\":[\"D1\"]") != std::string::npos);
    sieda_string_free(ac);
    char* spice = sieda_spice_netlist(api);
    CHECK(spice && std::string(spice).find("not fitted") != std::string::npos);
    sieda_string_free(spice);
    CHECK(sieda_set_active_variant(api, "") == 1);
    char* plain = sieda_simulate_dc(api);
    CHECK(plain && std::string(plain).find("\"omitted\"") == std::string::npos);
    sieda_string_free(plain);
    sieda_project_free(api);
}

#include "sieda/SchematicSearch.hpp"

// ======================================================================= find / replace, net navigator, title block

TEST(schematic_find_replace_net_navigator_and_title_block) {
    Project p = twoSheetProject();
    Schematic& s = p.schematic;
    const int r = s.addComponent(ComponentKind::Resistor, "4k7", {300, 300});
    SearchOptions all;
    // Designators, values and labels across sheets, in sheet order.
    auto hits = findInSchematic(s, "4k7", all);
    CHECK(hits.size() == 1 && hits[0].component == r && hits[0].field == "value");
    hits = findInSchematic(s, "r", all);
    CHECK(!hits.empty());
    for (size_t i = 1; i < hits.size(); ++i)
        if (hits[i].component >= 0 && hits[i - 1].component >= 0)
            CHECK(s.sheetIndex(hits[i - 1].sheet) <= s.sheetIndex(hits[i].sheet) || hits[i].field == "net");
    SearchOptions exact;
    exact.matchCase = true;
    exact.wholeWord = true;
    CHECK(findInSchematic(s, "4K7", exact).empty());
    CHECK(findInSchematic(s, "", all).empty());
    // Every label of a net, on every sheet, and the net itself.
    int labelHits = 0, netHits = 0;
    for (const auto& h : findInSchematic(s, "VIN", all)) {
        labelHits += h.field == "label";
        netHits += h.field == "net";
    }
    CHECK(labelHits >= 2 && netHits == 1);
    // Replace in values and label names; designators are left to annotation.
    SearchOptions values;
    values.refs = values.nets = values.pins = false;
    CHECK(replaceInSchematic(s, "4k7", "10k", values) == 1);
    CHECK(s.find(r)->value == "10k");
    const int before = static_cast<int>(s.nets().size());
    CHECK(replaceInSchematic(s, "VIN", "VSUP", values) >= 2);
    CHECK(findInSchematic(s, "VIN", all).empty());
    CHECK(static_cast<int>(s.nets().size()) == before);  // the renamed labels still join
    CHECK(replaceInSchematic(s, "VSUP", "", values) == 0);  // a label never loses its name

    // Net navigator: every place, sheet by sheet.
    const Component* label = nullptr;
    for (const auto& c : s.components())
        if (c.kind == ComponentKind::NetLabel && c.value == "VSUP") label = &c;
    CHECK(label != nullptr);
    if (!label) return;
    const int net = s.netOf({label->id, 0});
    const auto places = netPlaces(s, net);
    std::set<int> sheets;
    int pins = 0, globals = 0;
    for (const auto& pl : places) {
        sheets.insert(pl.sheet);
        pins += pl.kind == "pin";
        globals += pl.kind == "global";
    }
    CHECK(sheets.size() == 2 && pins >= 2 && globals >= 2);
    CHECK(netPlaces(s, -1).empty() && netPlaces(s, 99999).empty());

    // Repeated blocks and multi-unit parts: one edit each.
    Project q;
    DividerBlock b = dividerBlock(q.schematic);
    q.schematic.repeatSheet(b.sheet, 3);
    CHECK(findInSchematic(q.schematic, "10k", all).size() == 6);  // found where drawn: 2 per channel
    CHECK(replaceInSchematic(q.schematic, "10k", "22k", values) == 2);
    CHECK(q.schematic.find(q.schematic.copyOn(b.r1, q.schematic.sheetInstances(b.sheet)[2]))->value == "22k");

    // Title block: saved when set, defaults to the project name in the snapshot.
    CHECK(p.snapshot().get("titleBlock").get("title").asString() == p.name);
    p.titleBlock.company = "Acme";
    p.titleBlock.revision = "B";
    const std::string saved = p.toJson().dump();
    Project back = Project::fromJson(Json::parse(saved));
    CHECK(back.titleBlock.company == "Acme" && back.titleBlock.revision == "B");
    CHECK(Project::fromJson(Json::parse(ledProject().toJson().dump())).titleBlock.empty());
    CHECK(ledProject().toJson().dump().find("titleBlock") == std::string::npos);

    // C API.
    SiedaProject* api = sieda_project_load_json(saved.c_str(), nullptr);
    CHECK(api != nullptr);
    if (!api) return;
    char* found = sieda_schematic_find(api, "{\"text\":\"VSUP\",\"fields\":[\"label\"]}");
    CHECK(found && std::string(found).find("\"field\":\"label\"") != std::string::npos);
    sieda_string_free(found);
    char* bad = sieda_schematic_find(api, "{");
    CHECK(bad && std::string(bad).find("\"error\"") != std::string::npos);
    sieda_string_free(bad);
    CHECK(sieda_schematic_replace(api, "{\"text\":\"VSUP\",\"replacement\":\"VCC_IN\"}") >= 2);
    CHECK(sieda_schematic_replace(api, "not json") == 0);
    char* nav = sieda_net_places(api, net);
    CHECK(nav && std::string(nav).find("\"name\":\"VCC_IN\"") != std::string::npos &&
          std::string(nav).find("\"kind\":\"global\"") != std::string::npos);
    sieda_string_free(nav);
    CHECK(sieda_set_title_block(api, "{\"title\":\"Board\",\"drawnBy\":\"SN\"}") == 1);
    CHECK(sieda_set_title_block(api, "[1]") == 0 && sieda_set_title_block(api, "x") == 0);
    char* snap = sieda_project_snapshot(api);
    CHECK(snap && std::string(snap).find("\"drawnBy\":\"SN\"") != std::string::npos &&
          std::string(snap).find("\"company\":\"Acme\"") != std::string::npos);
    sieda_string_free(snap);
    sieda_project_free(api);
}

TEST(schematic_capture_hostile_inputs) {
    // Unit lists in part specs: malformed entries are refused with JsonError, never crash.
    for (const char* units : {"[{\"name\":\"\",\"pins\":[\"1\"]}]", "[{\"name\":\"A\",\"pins\":[]}]", "[{\"name\":\"TOOLONGNAME\",\"pins\":[\"1\"]}]",
                              "[{\"name\":\"A\",\"pins\":[\"9\"]}]", "[1,2,3]", "{\"x\":1}", "[{\"name\":\"A\",\"pins\":[1]}]"}) {
        const std::string spec = std::string("{\"name\":\"FUZZ\",\"package\":{\"type\":\"SOIC\"},\"pins\":[{\"number\":\"1\",\"name\":\"A\"},"
                                             "{\"number\":\"2\",\"name\":\"B\"}],\"units\":") + units + "}";
        try {
            CustomPartRegistry::instance().registerPart(customPartSpecFromJson(Json::parse(spec)));
        } catch (const JsonError&) {
        }
    }
    // A project with repeated sheets, buses and placed units survives random damage to its file.
    Project p;
    DividerBlock b = dividerBlock(p.schematic);
    const auto part = CustomPartRegistry::instance().registerPart(quadOpAmpSpec());
    p.addCustomPart(part->spec);
    const int a = p.schematic.addCustomUnits(part->id, "", {300, 300});
    p.schematic.placeNextUnit(a, {400, 300});
    const int bus = p.schematic.addBus("X[0..1]", {{0, 400}, {200, 400}});
    p.schematic.ripBusEntries(bus, {});
    p.schematic.repeatSheet(b.sheet, 2);
    p.titleBlock.title = "Fuzz";
    const std::string good = p.toJson().dump();
    unsigned seed = 777;
    int loaded = 0;
    for (int round = 0; round < 300; ++round) {
        std::string text = good;
        for (int k = 0; k < 3; ++k) {
            seed = seed * 1664525u + 1013904223u;
            text[(seed >> 7) % text.size()] = "0123456789-\"{}[],:tfx"[(seed >> 22) % 21];
        }
        try {
            Project q = Project::fromJson(Json::parse(text));
            ++loaded;
            CHECK(instancesConsistent(q.schematic));
            for (const auto& c : q.schematic.components()) {
                if (c.kind == ComponentKind::PartUnit) CHECK(q.schematic.unitPackage(c.id) > 0);
                if (c.bus != 0) CHECK(q.schematic.findBus(c.bus) != nullptr);
            }
            q.schematic.runERC();
            (void)q.snapshot();
        } catch (const std::exception&) {
        }
    }
    CHECK(loaded > 0);
}

// ======================================================================= lossy lines, channels, eye (SI package)

namespace {
/// Transfer of a 2-port network from a Thévenin source (rs) to a port-2 load admittance `yl(f)`.
TransferFn twoPortTransfer(const ChannelNetwork& net, double rs, std::function<cplx(double)> yl) {
    return [&net, rs, yl](const std::vector<double>& f) {
        const auto s = net.sParameters(f, 50);
        std::vector<cplx> h(f.size());
        for (size_t k = 0; k < f.size(); ++k)
            h[k] = terminatedVoltages(s[k], 50, {PortTermination::source(1.0, rs, 50), PortTermination::load(yl(f[k]), 50)})[1];
        return h;
    };
}
}  // namespace

TEST(si_lossy_line_physics) {
    // Djordjevic–Sarkar: exactly εr and tan δ at the 1 GHz reference, εr falling with frequency, tan δ nearly flat.
    const DielectricModel ds = DielectricModel::djordjevicSarkar(4.4, 0.02);
    CHECK_NEAR(ds.er(1e9), 4.4, 1e-9);
    CHECK_NEAR(ds.tanD(1e9), 0.02, 1e-9);
    CHECK(ds.er(1e6) > ds.er(1e9) && ds.er(1e9) > ds.er(1e10));
    CHECK_NEAR(ds.tanD(1e8), 0.02, 0.003);
    CHECK_NEAR(ds.tanD(1e10), 0.02, 0.003);
    CHECK(ds.epsInf > 1 && ds.epsInf < 4.4);
    CHECK_NEAR(DielectricModel::djordjevicSarkar(3.5, 0).er(2e10), 3.5, 1e-12);

    // Skin depth of copper at 1 GHz ≈ 2.09 µm; Rs ≈ 8.2 mΩ/□.
    CHECK_NEAR(skinDepth(1e9), 2.087e-6, 0.01e-6);
    CHECK_NEAR(surfaceResistance(1e9), 8.24e-3, 0.05e-3);
    // Hammerstad–Jensen: an air microstrip of w = h is 126.5 Ω.
    CHECK_NEAR(hammerstadJensenZ0Air(1.0), 126.5, 0.5);
    // Wheeler's rule on a wide microstrip tends to the parallel-plate 2/w (Pozar eq. 3.199).
    const double wide = microstripConductorFactor(100, 0.001, 0.1) * 100e-3 / 2;  // w/h = 1000
    CHECK_NEAR(wide, 1.0, 0.02);
    // Pozar Example 3.5: 50 Ω copper stripline, b = 3.2 mm, εr 2.2, W = 2.66 mm, t = 0.01 mm, 10 GHz → α_c = 0.122 Np/m.
    const double alphaC = striplineConductorFactor(2.66, 0.01, 3.2, 50, 2.2) * surfaceResistance(10e9) / (2 * 50);
    CHECK_NEAR(alphaC, 0.122, 0.004);
    // … and with tan δ 0.001 the dielectric loss α_d = π f √εr tan δ / c = 0.155 Np/m (homogeneous TEM).
    LossOptions dielOnly;
    dielOnly.noConductorLoss = true;
    const LineModel pz = lineModelFrom(true, 50, 2.2, 2.2, 0.001, 2.66, 0.01, 3.2, copperFoils()[0], dielOnly);
    const double f10 = 10e9;
    const double exact = kPi * f10 * std::sqrt(pz.dielectric.er(f10)) * pz.dielectric.tanD(f10) / kSpeedOfLight;
    CHECK_NEAR(pz.gamma(f10).real(), exact, 0.002 * exact);
    CHECK_NEAR(pz.gamma(f10).real(), 0.155, 0.006);
    // Bogatin's rule of thumb: α_d ≈ 2.3 · f[GHz] · tan δ · √εr dB/inch.
    const LineModel fr4 = lineModelFrom(true, 50, 4.4, 4.4, 0.02, 0.15, 0.035, 0.5, copperFoils()[0], dielOnly);
    CHECK_NEAR(fr4.attenuationDb(1e9) * 0.0254, 2.3 * 0.02 * std::sqrt(4.4), 0.03);

    // Conductor: R → R_dc at low frequency, ∝ √f once the skin depth is small, = Rs·(R_ac/Rs) at high frequency.
    LossOptions condOnly;
    condOnly.noDielectricLoss = true;
    condOnly.roughness = RoughnessModel::None;
    const LineModel c = lineModelFrom(true, 50, 4.4, 4.4, 0.02, 0.15, 0.035, 0.5, copperFoils()[0], condOnly);
    CHECK_NEAR(c.rdc, kCopperResistivity / (0.15e-3 * 0.035e-3), 1e-6);
    CHECK_NEAR(c.seriesZ(1e3).real(), c.rdc, 0.01 * c.rdc);
    CHECK_NEAR(c.seriesZ(4e9).real() / c.seriesZ(1e9).real(), 2.0, 0.06);
    CHECK_NEAR(c.seriesZ(10e9).real(), surfaceResistance(10e9) * c.acFactor, 0.03 * c.seriesZ(10e9).real());
    CHECK(c.internalZ(1e9).imag() > 0);  // internal inductance of the skin effect

    // Roughness: Hammerstad → 2, Huray → 1 + SR at high frequency, → 1 at low frequency.
    CHECK_NEAR(hammerstadRoughness(1e5, 1.5e-6), 1.0, 0.01);
    CHECK_NEAR(hammerstadRoughness(1e11, 1.5e-6), 2.0, 0.05);
    CHECK_NEAR(hurayRoughness(1e5, 1e-6, 2.2), 1.0, 0.01);
    CHECK_NEAR(hurayRoughness(1e13, 1e-6, 2.2), 3.2, 0.1);
    CHECK_NEAR(hurayRoughnessCausal(1e5, 1e-6, 2.2).real(), 1.0, 0.01);
    CHECK_NEAR(hurayRoughnessCausal(1e13, 1e-6, 2.2).real(), 3.2, 0.1);
    LossOptions rough;
    rough.noDielectricLoss = true;
    const CopperFoil stdFoil = copperFoils().back();
    const LineModel r = lineModelFrom(true, 50, 4.4, 4.4, 0.02, 0.15, 0.035, 0.5, stdFoil, rough);
    CHECK(r.seriesZ(10e9).real() > 1.5 * c.seriesZ(10e9).real());
    CHECK_NEAR(r.seriesZ(1e5).real(), c.seriesZ(1e5).real(), 0.01 * c.rdc);

    // On the stack-up: the lossless model is the reflection analysis' line (Z0 and delay).
    BoardSettings s;
    s.layerCount = 4;
    LossOptions ll;
    ll.lossless = true;
    for (int layer : {0, 1}) {
        const LineModel m = lineModel(s, layer, 0.2, ll);
        CHECK_NEAR(std::abs(m.zc(1e9)), trackImpedance(s, layer, 0.2), 0.001 * trackImpedance(s, layer, 0.2));
        CHECK_NEAR(m.gamma(1e9).imag() / (2 * kPi * 1e9), propagationDelayPerMm(s, layer, 0.2) * 1e3, 1e-14);
        const LineModel lm = lineModel(s, layer, 0.2);
        CHECK(lm.attenuationDb(10e9) > 3 * lm.attenuationDb(1e9));
        CHECK(lm.attenuationDb(1e9) > 0);
    }
    const Json loss = lineLossJson(s, LossOptions{});
    CHECK(loss.get("layers").size() == 4 && loss.get("freq").size() == 41);
    CHECK(loss.get("foil").asString() == "std");  // FR-4: standard ED foil

    // Time domain through the frequency-domain engine: the lossless lattice (25 Ω source, 50 Ω / 1 ns line, open end)
    // gives 4/3, 8/9 and 28/27 V at 1, 3 and 5 TD, as Bergeron's method does.
    ChannelNetwork net;
    net.addNode();
    net.addNode();
    net.lines.push_back({0, 1, LineModel::ideal(50, 1e-9 / 0.15), 0.15});
    net.ports = {0, 1};
    const double dt = 2e-12;
    const auto step = stepResponse(twoPortTransfer(net, 25, [](double f) { return cplx(0, 2 * kPi * f * 1e-16); }), 20e-12, dt, 30e-9);
    auto at = [&](double t) { return step[static_cast<size_t>(std::lround(t / dt))]; };
    CHECK(step.size() > 10000);
    if (step.size() <= 10000) return;
    CHECK_NEAR(at(0.8e-9), 0.0, 0.01);
    CHECK_NEAR(at(1.6e-9), 4.0 / 3, 0.01);
    CHECK_NEAR(at(3.6e-9), 8.0 / 9, 0.01);
    CHECK_NEAR(at(5.6e-9), 28.0 / 27, 0.01);
    CHECK_NEAR(at(25e-9), 1.0, 0.01);
    // A lossy FR-4 stripline of 200 mm: causal (nothing before the time of flight), slower edge, DC divider level.
    ChannelNetwork lossy;
    lossy.addNode();
    lossy.addNode();
    const LineModel fl = lineModel(s, 1, 0.15);
    lossy.lines.push_back({0, 1, fl, 0.2});
    lossy.ports = {0, 1};
    const double tof = 0.2 * fl.gamma(1e9).imag() / (2 * kPi * 1e9);
    const auto ls = stepResponse(twoPortTransfer(lossy, 50, [](double) { return cplx(1.0 / 50, 0); }), 30e-12, dt, 20e-9);
    double pre = 0;
    for (size_t k = 0; k < ls.size() && k * dt < 0.9 * tof; ++k) pre = std::max(pre, std::fabs(ls[k]));
    CHECK(pre < 2e-3);
    CHECK(!ls.empty() && std::fabs(ls.back() - 50 / (100 + fl.rdc * 0.2)) < 0.01);
    auto crossing = [&](const std::vector<double>& v, double level) {
        for (size_t k = 0; k < v.size(); ++k)
            if (v[k] >= level) return k * dt;
        return 1.0;
    };
    const double rise = crossing(ls, 0.9 * 0.5) - crossing(ls, 0.1 * 0.5);
    CHECK(rise > 30e-12);  // loss and dispersion slow the edge
}

namespace {
/// TX (1 TXP, 2 TXN outputs) → RX (3 RXP, 4 RXN inputs) differential link, 5 VDD, 6 GND; nets D_P / D_N.
struct DiffBoard {
    Project p;
    int u1 = -1, u2 = -1, np = -1, nn = -1;
};
DiffBoard diffBoard(double gap, double width = 0.15, int layer = 0) {
    DiffBoard b;
    Project& p = b.p;
    auto& s = p.schematic;
    CustomPartSpec spec;
    spec.name = "SI-SERDES";
    spec.package.type = "SOIC";
    spec.package.pinCount = 8;
    auto add = [&](const char* number, const char* name, PinType type) {
        CustomPin pin;
        pin.number = number;
        pin.name = name;
        pin.type = type;
        spec.pins.push_back(pin);
    };
    add("1", "TXP", PinType::Output);
    add("2", "TXN", PinType::Output);
    add("3", "RXP", PinType::Input);
    add("4", "RXN", PinType::Input);
    add("5", "VDD", PinType::PowerIn);
    add("6", "GND", PinType::PowerIn);
    add("7", "NC7", PinType::NoConnect);
    add("8", "NC8", PinType::NoConnect);
    const std::string part = p.addCustomPart(spec);
    b.u1 = s.addCustomComponent(part, "", {0, 0});
    b.u2 = s.addCustomComponent(part, "", {400, 0});
    int lp = s.addComponent(ComponentKind::NetLabel, "D_P", {200, -50});
    int ln = s.addComponent(ComponentKind::NetLabel, "D_N", {200, 50});
    int vcc = s.addComponent(ComponentKind::NetLabel, "+3V3", {-100, -100});
    int gnd = s.addComponent(ComponentKind::Ground, "", {-100, 100});
    wire(s, b.u1, "TXP", b.u2, "RXP");
    wire(s, b.u1, "TXN", b.u2, "RXN");
    wire(s, b.u1, "TXP", lp, "N");
    wire(s, b.u1, "TXN", ln, "N");
    for (int u : {b.u1, b.u2}) {
        wire(s, u, "VDD", vcc, "N");
        wire(s, u, "GND", gnd, "GND");
    }
    p.schematicChanged();
    p.pcb.settings.width = 140;
    p.pcb.settings.height = 40;
    p.pcb.settings.layerCount = 4;
    siPlace(p, b.u1, {10, 20});
    siPlace(p, b.u2, {120, 20});
    b.np = s.netOf({b.u1, pin(s, b.u1, "TXP")});
    b.nn = s.netOf({b.u1, pin(s, b.u1, "TXN")});
    // Route: each pin straight down to its lane, the lanes run in parallel `gap` apart (edge to edge), then up.
    const Vec2 tp = siPad(p, b.u1, "TXP"), tn = siPad(p, b.u1, "TXN"), rp = siPad(p, b.u2, "RXP"), rn = siPad(p, b.u2, "RXN");
    const double yP = 32, yN = yP + gap + width;
    siTrack(p, b.np, tp, {tp.x, yP}, layer, width);
    siTrack(p, b.np, {tp.x, yP}, {rp.x, yP}, layer, width);
    siTrack(p, b.np, {rp.x, yP}, rp, layer, width);
    siTrack(p, b.nn, tn, {tn.x, yN}, layer, width);
    siTrack(p, b.nn, {tn.x, yN}, {rn.x, yN}, layer, width);
    siTrack(p, b.nn, {rn.x, yN}, rn, layer, width);
    return b;
}

std::vector<double> linGrid(double fMax, int n) {
    std::vector<double> f;
    for (int k = 0; k < n; ++k) f.push_back(fMax * k / (n - 1));
    return f;
}
}  // namespace

TEST(si_channel_sparameters) {
    // A single 70 Ω, 0.5 ns ideal line against the closed form (reference 50 Ω):
    // S11 = Γ(1 − e^{−2jθ}) / (1 − Γ² e^{−2jθ}), S21 = (1 − Γ²) e^{−jθ} / (1 − Γ² e^{−2jθ}).
    ChannelNetwork one;
    one.addNode();
    one.addNode();
    one.lines.push_back({0, 1, LineModel::ideal(70, 0.5e-9 / 0.1), 0.1});
    one.ports = {0, 1};
    const auto freq = linGrid(10e9, 41);
    const auto s1 = one.sParameters(freq, 50);
    const double G = 20.0 / 120;
    double worst = 0;
    for (size_t k = 1; k < freq.size(); ++k) {
        const cplx e = std::exp(cplx(0, -2 * kPi * freq[k] * 0.5e-9));
        const cplx s11 = G * (1.0 - e * e) / (1.0 - G * G * e * e), s21 = (1 - G * G) * e / (1.0 - G * G * e * e);
        worst = std::max({worst, std::abs(s1[k](0, 0) - s11), std::abs(s1[k](1, 0) - s21)});
    }
    CHECK(worst < 1e-5);
    // Cascading two halves (Redheffer star product) equals the whole line.
    ChannelNetwork half = one;
    half.lines[0].length = 0.05;
    const auto sh = half.sParameters(freq, 50);
    double cw = 0;
    for (size_t k = 0; k < freq.size(); ++k) {
        const CMat c = cascadeStar(sh[k], sh[k]);
        for (size_t q = 0; q < c.a.size(); ++q) cw = std::max(cw, std::abs(c.a[q] - s1[k].a[q]));
    }
    CHECK(cw < 1e-6);

    // The routed clock of the SI board: reciprocal, passive, ≈ 0 dB at DC, a lossless twin that conserves power.
    SiBoard b = siBoard();
    siRouteClock(b);
    ChannelOptions opt;
    opt.net = b.p.schematic.nets()[static_cast<size_t>(b.clk)].name;
    opt.partner = "none";
    const ChannelModel m = extractChannel(b.p, opt);
    CHECK(m.error.empty() && !m.differential && m.network.ports.size() == 2);
    CHECK(m.lengthP > 90 && m.lengthP < 120);
    const auto grid = linGrid(10e9, 101);
    const SParams sp = channelSParams(m, grid, 50);
    bool passive = true, reciprocal = true;
    for (const auto& s : sp.s) {
        passive = passive && std::norm(s(0, 0)) + std::norm(s(1, 0)) <= 1 + 1e-9;
        reciprocal = reciprocal && std::abs(s(0, 1) - s(1, 0)) < 1e-9;
    }
    CHECK(passive && reciprocal);
    CHECK(std::abs(sp.s[0](1, 0)) > 0.99);
    CHECK(std::abs(sp.s.back()(1, 0)) < std::abs(sp.s[10](1, 0)));  // more loss at 10 GHz than at 1 GHz
    ChannelOptions lo = opt;
    lo.loss.lossless = true;
    const SParams sl = channelSParams(extractChannel(b.p, lo), grid, 50);
    double energy = 1;
    for (const auto& s : sl.s) energy = std::min(energy, std::norm(s(0, 0)) + std::norm(s(1, 0)));
    CHECK(energy > 0.999);
    // The routed analysis (with its driver and receiver) and the Touchstone export.
    EyeOptions eo;
    eo.bitRate = 1e9;
    const Json cj = channelJson(b.p, opt, ChannelDrive{}, &eo, nullptr);
    CHECK(cj.get("error").isNull());
    CHECK(cj.get("curves").size() == 3 && cj.get("curves")[0].get("name").asString() == "S21");
    CHECK(cj.get("step").get("lossy").size() > 50 && cj.get("eye").get("eyeHeight").asNumber() > 1.0);
    std::string err;
    const std::string ts = channelTouchstone(b.p, opt, &err);
    CHECK(err.empty() && ts.find("# Hz S RI R 50") != std::string::npos);
    const TouchstoneData back = parseTouchstone(ts, 2);
    const SParams ref = channelSParams(m, linGrid(opt.fMax, opt.points), 50);
    CHECK(back.sp.ports == 2 && back.sp.freq.size() == ref.freq.size());
    double rt = 0;
    for (size_t k = 0; k < std::min(back.sp.s.size(), ref.s.size()); ++k)
        for (size_t q = 0; q < 4; ++q) rt = std::max(rt, std::abs(back.sp.s[k].a[q] - ref.s[k].a[q]));
    CHECK(rt < 1e-8);
    // Unknown net.
    ChannelOptions bad;
    bad.net = "NOPE";
    CHECK(!extractChannel(b.p, bad).error.empty());
}

TEST(si_touchstone_parser) {
    // Version 1, 2-port, MA in GHz: S11 S21 S12 S22 order.
    const TouchstoneData a = parseTouchstone("! test\n# GHz S MA R 50\n1 0.1 0 0.9 -90 0.9 -90 0.1 0\n2 0.2 0 0.8 -180 0.8 -180 0.2 0\n", 2);
    CHECK(a.sp.ports == 2 && a.sp.freq.size() == 2 && a.sp.freq[1] == 2e9);
    CHECK_NEAR(a.sp.s[0](1, 0).imag(), -0.9, 1e-12);
    CHECK_NEAR(a.sp.s[1](0, 1).real(), -0.8, 1e-12);
    CHECK(!a.comments.empty() && a.comments[0] == "test");
    // DB and RI, MHz; the port count inferred from the first data line.
    const TouchstoneData d = parseTouchstone("# MHz S DB R 50\n100 -20 0 -1 45 -1 45 -20 0\n", 0);
    CHECK(d.sp.ports == 2 && d.sp.freq[0] == 100e6);
    CHECK_NEAR(std::abs(d.sp.s[0](0, 0)), 0.1, 1e-12);
    CHECK_NEAR(std::arg(d.sp.s[0](1, 0)), kPi / 4, 1e-12);
    // Z-parameters, version 1 normalised to R: a 1-port of 2 × 50 Ω → S11 = (100 − 50)/(100 + 50).
    const TouchstoneData z = parseTouchstone("# Hz Z RI R 50\n1e6 2 0\n", 1);
    CHECK_NEAR(z.sp.s[0](0, 0).real(), 1.0 / 3, 1e-12);
    const TouchstoneData y = parseTouchstone("# Hz Y RI R 50\n1e6 0.5 0\n", 1);  // Y·R = 0.5 → 100 Ω
    CHECK_NEAR(y.sp.s[0](0, 0).real(), 1.0 / 3, 1e-12);
    // Noise parameters after the 2-port data are skipped.
    const TouchstoneData n = parseTouchstone("# GHz S RI R 50\n1 0 0 1 0 1 0 0 0\n2 0 0 1 0 1 0 0 0\n1 1.2 0.5 30 0.3\n", 2);
    CHECK(n.sp.freq.size() == 2);
    // Version 2: keywords, 12_21 order, a 4-port written as rows.
    const std::string v2 = "[Version] 2.0\n# Hz S RI R 50\n[Number of Ports] 2\n[Two-Port Data Order] 12_21\n"
                           "[Number of Frequencies] 1\n[Network Data]\n1e9 0 0 0.5 0 0.7 0 0 0\n[End]\n";
    const TouchstoneData t2 = parseTouchstone(v2, 0);
    CHECK(t2.version == "2.0" && t2.sp.ports == 2);
    CHECK_NEAR(t2.sp.s[0](0, 1).real(), 0.5, 1e-12);
    CHECK_NEAR(t2.sp.s[0](1, 0).real(), 0.7, 1e-12);
    CHECK(touchstonePortsFromName("chan.S4P") == 4 && touchstonePortsFromName("x.txt") == 0 && touchstonePortsFromName("a.s2") == 0);
    // Round trip of a 4-port through the writer.
    SParams four;
    four.ports = 4;
    four.freq = {1e8, 2e8};
    for (int k = 0; k < 2; ++k) {
        CMat m(4);
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) m(i, j) = cplx(0.01 * (i + 1) + k, -0.02 * (j + 1));
        four.s.push_back(m);
    }
    const TouchstoneData f4 = parseTouchstone(writeTouchstone(four, "four"), 4);
    CHECK(f4.sp.ports == 4 && f4.sp.s.size() == 2);
    if (f4.sp.s.size() == 2) CHECK_NEAR(f4.sp.s[1](2, 3).imag(), -0.08, 1e-9);
    // Malformed input is rejected with a message.
    int rejected = 0;
    for (const char* badText : {"", "hello", "[Version] 2.0\n# GHz S MA R 50\n[Number of Ports] 2\n[Network Data]\n2 0 0 1 0 1 0 0 0\n1 0 0 1 0 1 0 0 0\n",
                                "# GHz S MA R 50\n1 0 0 1 0 1 0 0\n", "# GHz S XX R 50\n1 0 0\n", "# GHz S MA R -5\n1 0 0\n",
                                "1 0 0 1 0 1 0 0 0\n", "# GHz S MA R 50\n1 nan 0 1 0 1 0 0 0\n", "# GHz G MA R 50\n1 0 0\n",
                                "[Version] 2.0\n# Hz S RI R 50\n[Number of Ports] 99\n"}) {
        try {
            parseTouchstone(badText, 2);
        } catch (const std::runtime_error&) {
            ++rejected;
        }
    }
    CHECK(rejected == 10);
}

TEST(si_touchstone_fuzz) {
    // Random mutations of valid files and random token soup: the parser either succeeds or throws runtime_error.
    const std::string seeds[] = {
        "! c\n# GHz S MA R 50\n1 0.1 0 0.9 -90 0.9 -90 0.1 0\n2 0.2 0 0.8 -180 0.8 -180 0.2 0\n",
        "[Version] 2.0\n# Hz S RI R 50\n[Number of Ports] 4\n[Number of Frequencies] 1\n[Network Data]\n1e9 "
        "1 0 0 0 0 0 0 0\n0 0 1 0 0 0 0 0\n0 0 0 0 1 0 0 0\n0 0 0 0 0 0 1 0\n[End]\n",
        "# MHz Z DB R 75\n10 1 2\n20 3 4\n"};
    uint32_t rng = 12345;
    auto next = [&]() {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return rng;
    };
    const char alphabet[] = "0123456789.-+eE #![]!\n\t,SYZMADBRIHzkGgPortsNumber";
    int ok = 0, rejected = 0, other = 0;
    for (int it = 0; it < 3000; ++it) {
        std::string t = seeds[it % 3];
        const int edits = 1 + static_cast<int>(next() % 8);
        for (int e = 0; e < edits && !t.empty(); ++e) {
            const size_t at = next() % t.size();
            switch (next() % 4) {
                case 0: t[at] = alphabet[next() % (sizeof alphabet - 1)]; break;
                case 1: t.erase(at, 1 + next() % 5); break;
                case 2: t.insert(at, 1, alphabet[next() % (sizeof alphabet - 1)]); break;
                default: t = t.substr(0, at); break;
            }
        }
        if (it % 10 == 0) {
            t.clear();
            for (int k = 0; k < 200; ++k) t += alphabet[next() % (sizeof alphabet - 1)];
        }
        try {
            const TouchstoneData d = parseTouchstone(t, static_cast<int>(next() % 5));
            bool finite = d.sp.ports > 0 && d.sp.s.size() == d.sp.freq.size();
            for (const auto& m : d.sp.s)
                for (const auto& v : m.a) finite = finite && std::isfinite(v.real()) && std::isfinite(v.imag());
            CHECK(finite);
            ++ok;
        } catch (const std::runtime_error&) {
            ++rejected;
        } catch (...) {
            ++other;
        }
    }
    CHECK(other == 0);
    CHECK(ok > 0 && rejected > 0);
}

TEST(si_differential_pair_coupled_lines) {
    // Mixed mode of two identical, uncoupled 50 Ω lines: SDD21 = S21, no mode conversion, matched (SDD11 = 0).
    ChannelNetwork two;
    for (int k = 0; k < 4; ++k) two.addNode();
    two.lines.push_back({0, 2, LineModel::ideal(50, 6e-9), 0.1});
    two.lines.push_back({1, 3, LineModel::ideal(50, 6e-9), 0.1});
    two.ports = {0, 1, 2, 3};
    const auto f = linGrid(5e9, 11);
    const auto s4 = two.sParameters(f, 50);
    for (size_t k = 1; k < f.size(); ++k) {
        const CMat mm = mixedMode(s4[k]);
        CHECK_NEAR(std::abs(mm(1, 0) - s4[k](2, 0)), 0.0, 1e-9);
        CHECK(std::abs(mm(3, 0)) < 1e-9 && std::abs(mm(0, 0)) < 1e-6);
    }
    // Coupled pair referenced to its odd-mode impedance: matched differentially (SDD11 ≈ 0), not in common mode.
    BoardSettings bs;
    bs.layerCount = 4;
    LossOptions ll;
    ll.lossless = true;
    const LineModel base = lineModel(bs, 1, 0.15, ll);
    const CouplingEstimate ce = crosstalkCoupling(bs, 1, 0.15, 0.15, 0.15, 50, 1e-10);
    CHECK(ce.kl > 0.01 && std::fabs(ce.kl - ce.kc) < 1e-12);  // stripline: homogeneous
    ChannelNetwork cp;
    for (int k = 0; k < 4; ++k) cp.addNode();
    ChannelNetwork::Coupled cl;
    cl.a1 = 0, cl.a2 = 1, cl.b1 = 2, cl.b2 = 3;
    // Even / odd lines from the coupling coefficients (as the extractor builds them).
    auto mode = [&](int sign) {
        LineModel m = base;
        const double lf = 1 + sign * ce.kl, cf = (1 - sign * ce.kc) / (1 - ce.kl * ce.kc);
        m.z0 = base.z0 * std::sqrt(lf / cf);
        m.epsEff = base.epsEff * lf * cf;
        m.lExt = m.z0 * std::sqrt(m.epsEff) / kSpeedOfLight;
        m.cAir = 1 / (m.z0 * std::sqrt(m.epsEff) * kSpeedOfLight);
        return m;
    };
    cl.even = mode(+1);
    cl.odd = mode(-1);
    CHECK_NEAR(cl.even.epsEff, base.epsEff, 1e-9);  // homogeneous: both modes at c/√εr
    CHECK(cl.odd.z0 < base.z0 && cl.even.z0 > base.z0);
    cl.length = 0.05;
    cp.coupled.push_back(cl);
    cp.ports = {0, 1, 2, 3};
    const auto sc = cp.sParameters(f, cl.odd.z0);
    for (size_t k = 1; k < f.size(); ++k) {
        const CMat mm = mixedMode(sc[k]);
        CHECK(std::abs(mm(0, 0)) < 1e-4);         // SDD11
        CHECK(std::abs(mm(1, 0)) > 0.9999);       // SDD21
        CHECK(std::abs(mm(3, 0)) < 1e-9);         // SCD21: symmetric pair, no conversion
    }
    CHECK(std::abs(mixedMode(sc[5])(2, 2)) > 0.05);  // SCC11: common mode sees the even-mode impedance

    // A routed pair: the coupled run is found, odd < single < even, equal legs have no skew, SDD21 ≈ 0 dB at DC.
    DiffBoard d = diffBoard(0.15, 0.15, 0);
    ChannelOptions opt;
    opt.net = "D_P";
    const ChannelModel m = extractChannel(d.p, opt);
    CHECK(m.error.empty());
    CHECK(m.differential && m.netN == "D_N" && m.network.ports.size() == 4);
    CHECK(!m.coupled.empty() && m.coupledLength > 90);
    if (!m.coupled.empty()) {
        CHECK(m.coupled[0].zOdd < trackImpedance(d.p.pcb.settings, 0, 0.15) && m.coupled[0].zEven > trackImpedance(d.p.pcb.settings, 0, 0.15));
        CHECK(m.coupled[0].epsOdd < m.coupled[0].epsEven);  // microstrip: the odd mode runs more in air
        CHECK_NEAR(m.coupled[0].zDiff, 2 * m.coupled[0].zOdd, 1e-9);
    }
    CHECK(std::fabs(m.delayP - m.delayN) < 20e-12);
    const SParams sp = channelSParams(m, linGrid(10e9, 51), 50);
    CHECK(std::abs(mixedMode(sp.s[0])(1, 0)) > 0.98);
    CHECK(std::abs(mixedMode(sp.s[25])(3, 0)) < 0.3);  // near-symmetric routing: little mode conversion
    // Uncoupling the pair (far apart) changes the differential impedance seen.
    DiffBoard far = diffBoard(0.6, 0.15, 0);
    const ChannelModel mf = extractChannel(far.p, opt);
    CHECK(mf.error.empty() && mf.coupled.empty() == false);
    if (!m.coupled.empty() && !mf.coupled.empty()) CHECK(mf.coupled[0].zDiff > m.coupled[0].zDiff);
    // The N net also finds its partner; the eye and the 4-port Touchstone.
    ChannelOptions on = opt;
    on.net = "D_N";
    CHECK(extractChannel(d.p, on).netP == "D_P");
    EyeOptions eo;
    eo.bitRate = 5e9;
    ChannelDrive ideal;
    ideal.idealDriver = true;
    const Json j = channelJson(d.p, opt, ideal, &eo, nullptr);
    CHECK(j.get("differential").asBool() && j.get("curves")[0].get("name").asString() == "SDD21");
    CHECK(j.get("eye").get("open").asBool());
    std::string err;
    const TouchstoneData t4 = parseTouchstone(channelTouchstone(d.p, opt, &err), 4);
    CHECK(err.empty() && t4.sp.ports == 4);
    // Cascading the imported block doubles the loss at the far end.
    const Json jc = channelJson(d.p, opt, ideal, nullptr, &t4);
    const Json j1 = channelJson(d.p, opt, ideal, nullptr, nullptr);
    const double il1 = j1.get("curves")[0].get("db")[200].asNumber(), il2 = jc.get("curves")[0].get("db")[200].asNumber();
    CHECK(il2 < il1 - 0.1 && std::fabs(il2 - 2 * il1) < 0.5 + 0.2 * std::fabs(il1));
}

TEST(si_eye_diagram) {
    // PRBS: maximal-length sequences (2^n − 1 period, 2^(n−1) ones).
    const auto p7 = prbsSequence(7, 254);
    int ones = 0;
    for (size_t k = 0; k < 127; ++k) ones += p7[k];
    CHECK(ones == 64);
    CHECK(std::equal(p7.begin(), p7.begin() + 127, p7.begin() + 127));
    const auto p15 = prbsSequence(15, 32767);
    CHECK(std::count(p15.begin(), p15.end(), 1) == 16384);

    // Ideal channel (pure delay): the eye is fully open — height 2A, width one UI, no data-dependent jitter.
    EyeOptions o;
    o.bitRate = 10e9;
    o.riseTime = 10e-12;
    const TransferFn delay = [](const std::vector<double>& f) {
        std::vector<cplx> h;
        for (double x : f) h.push_back(std::exp(cplx(0, -2 * kPi * x * 0.3e-9)));
        return h;
    };
    const EyeResult ideal = simulateEye(delay, 0.5, 0.25, 0.3e-9, o);
    CHECK(ideal.error.empty() && ideal.open);
    CHECK_NEAR(ideal.eyeHeight, 1.0, 0.01);
    CHECK_NEAR(ideal.pdaHeight, 1.0, 0.01);
    CHECK_NEAR(ideal.eyeWidth, 100e-12, 2e-12);
    CHECK(ideal.djPeakToPeak < 2e-12);
    CHECK(ideal.bits == 127);
    CHECK(ideal.density.size() == static_cast<size_t>(ideal.rows * ideal.cols) && ideal.cols == 64);
    // Random jitter widens the total jitter by 2·Q(BER)·RJ (Q(1e-12) = 7.034).
    EyeOptions rj = o;
    rj.rjRms = 1e-12;
    CHECK_NEAR(simulateEye(delay, 0.5, 0.25, 0.3e-9, rj).totalJitter - ideal.djPeakToPeak, 2 * 7.034e-12, 0.05e-12);
    // A mask inside the eye passes; one taller than the eye fails.
    EyeOptions mk = o;
    mk.maskWidthUi = 0.4;
    mk.maskHeight = 0.5;
    CHECK(simulateEye(delay, 0.5, 0.25, 0.3e-9, mk).maskPass);
    mk.maskHeight = 1.2;
    CHECK(!simulateEye(delay, 0.5, 0.25, 0.3e-9, mk).maskPass);

    // RC channel, τ = T/2: the pulse response has cursors (1 − a)·a^k with a = e^{−T/τ}, so the worst-case (peak
    // distortion) eye is 2A·(1 − 2a); PRBS7 contains the worst run (six equal bits) and closes to the same value.
    const double T = 1 / o.bitRate, tau = T / 2;
    const TransferFn rc = [tau](const std::vector<double>& f) {
        std::vector<cplx> h;
        for (double x : f) h.push_back(1.0 / cplx(1, 2 * kPi * x * tau));
        return h;
    };
    EyeOptions sharp = o;
    sharp.riseTime = 1e-13;
    sharp.samplesPerUi = 64;
    const EyeResult er = simulateEye(rc, 0.5, 0, 0, sharp);
    const double a = std::exp(-T / tau);
    CHECK_NEAR(er.pdaHeight, 1.0 * (1 - 2 * a), 0.02);
    CHECK_NEAR(er.eyeHeight, er.pdaHeight, 0.02);
    CHECK(er.eyeHeight < ideal.eyeHeight && er.djPeakToPeak > 1e-12);

    // A lossy channel at a high rate: CTLE and FFE open the eye; automatic FFE taps sum to 1 in magnitude.
    const TransferFn lossy = [](const std::vector<double>& f) {
        std::vector<cplx> h;
        for (double x : f) {
            const double db = -1.2 * std::sqrt(x / 1e9) - 0.9 * (x / 1e9);  // skin + dielectric, −14 dB at 5 GHz
            h.push_back(std::pow(10.0, db / 20) * std::exp(cplx(0, -2 * kPi * x * 1e-9)));
        }
        return h;
    };
    const EyeResult raw = simulateEye(lossy, 0.5, 0, 1e-9, o);
    EyeOptions ctle = o;
    ctle.ctle = ctle.ctleAuto = true;
    const EyeResult eq = simulateEye(lossy, 0.5, 0, 1e-9, ctle);
    CHECK(eq.ctleDcGainDb < 0 && eq.pdaHeight > raw.pdaHeight);
    EyeOptions ffe = o;
    ffe.ffe = ffe.ffeAuto = true;
    const EyeResult ef = simulateEye(lossy, 0.5, 0, 1e-9, ffe);
    double sum = 0;
    for (double t : ef.ffeTaps) sum += std::fabs(t);
    CHECK(ef.ffeTaps.size() == 4);
    CHECK_NEAR(sum, 1.0, 1e-9);
    CHECK(ef.pdaHeight > raw.pdaHeight);
    // PRBS15 runs a full period.
    EyeOptions l15 = o;
    l15.prbs = 15;
    CHECK(simulateEye(lossy, 0.5, 0, 1e-9, l15).bits == 32767);
    // Options from JSON are clamped.
    const EyeOptions parsed = eyeOptionsFromJson(Json::parse(R"({"bitRate":1e15,"prbs":9,"ffeTaps":[0.1,"x",0.8]})"));
    CHECK(parsed.bitRate == 200e9 && parsed.prbs == 9 && parsed.ffeTaps.size() == 2);
}

extern "C" int sieda_c_api_channel_test(const char* project_json);

TEST(si_channel_sign_off_and_c_api) {
    DiffBoard d = diffBoard(0.15, 0.15, 0);
    // Settings persist; a default project still writes none.
    d.p.si.copperFoil = "rtf";
    d.p.si.channels.push_back({"D_P", 5e9, 0.1, 0.3});
    d.p.si.rails.push_back({"+3V3", 3, 0.4, 0.6, 0.004, 80e3});
    Project q = Project::fromJson(d.p.toJson());
    CHECK(q.si.copperFoil == "rtf" && q.si.channels.size() == 1 && q.si.channel("D_P"));
    CHECK(q.si.channel("D_P") && q.si.channel("D_P")->maskWidthUi == 0.3);
    CHECK(q.si.rail("+3V3") && q.si.rail("+3V3")->vrmR == 0.004 && q.si.rail("+3V3")->vrmBandwidth == 80e3);
    CHECK(!Project().toJson().has("signalIntegrity"));
    // A short coupled pair at 5 Gb/s passes a modest mask; at 40 Gb/s with a large mask it fails (SI_EYE_MASK).
    const ChannelCheck good = checkChannel(d.p, d.p.si.channels[0]);
    CHECK(good.ok && good.eyeHeight > 0.2);
    SiSettings::ChannelSpec hard{"D_P", 40e9, 0.9, 0.6};
    const ChannelCheck bad = checkChannel(d.p, hard);
    CHECK(!bad.ok && bad.message.find("D_P / D_N") != std::string::npos);
    d.p.si.channels = {hard};
    bool found = false;
    for (const auto& v : signalPowerIntegrityChecks(d.p)) found |= v.code == "SI_EYE_MASK";
    CHECK(found);
    // Without channel specs the checks are unchanged (no SI_EYE_MASK).
    d.p.si.channels.clear();
    found = false;
    for (const auto& v : signalPowerIntegrityChecks(d.p)) found |= v.code == "SI_EYE_MASK";
    CHECK(!found);
    CHECK(sieda_c_api_channel_test(d.p.toJson().dump().c_str()) == 0);
}

TEST(si_broadside_crosstalk) {
    BoardSettings s;
    s.layerCount = 6;
    // Directly above each other the coupling is strongest; it falls with lateral offset; buried pairs have no FEXT.
    const CouplingEstimate on = broadsideCoupling(s, 1, 2, 0.15, 0.15, 0, 50, 0.5e-9);
    const CouplingEstimate off = broadsideCoupling(s, 1, 2, 0.15, 0.15, 1.0, 50, 0.5e-9);
    CHECK(on.kl > off.kl && off.kl > 0 && on.kl < 0.9);
    CHECK_NEAR(on.kc, on.kl, 1e-12);
    CHECK_NEAR(on.fext, 0, 1e-12);
    CHECK(on.next > 0.02);
    // Top over Inner 1: microstrip side in air → FEXT negative.
    CHECK(broadsideCoupling(s, 0, 1, 0.15, 0.15, 0, 50, 0.5e-9).fext < 0);
    // Equal heights reduce to the edge-coupled image formula's form (symmetric in the two layers).
    CHECK_NEAR(broadsideCoupling(s, 2, 3, 0.15, 0.15, 0.3, 50, 1e-9).kl, broadsideCoupling(s, 3, 2, 0.15, 0.15, 0.3, 50, 1e-9).kl,
               1e-12);

    // On the SI board: a victim on Inner 1 right under the clock is found as a broadside pair.
    SiBoard b = siBoard();
    siRouteClock(b);
    auto& sch = b.p.schematic;
    int v1 = sch.addComponent(ComponentKind::Resistor, "1k", {200, 200});
    int v2 = sch.addComponent(ComponentKind::Resistor, "1k", {300, 200});
    int lab = sch.addComponent(ComponentKind::NetLabel, "SENSE", {250, 200});
    wire(sch, v1, "1", lab, "N");
    wire(sch, v2, "1", lab, "N");
    b.p.schematicChanged();
    const int victim = sch.netOf({v1, 0});
    siTrack(b.p, victim, {25, 30}, {85, 30}, 1, 0.25);
    const CrosstalkPair* hit = nullptr;
    const auto pairs = crosstalkPairs(b.p);
    for (const auto& p : pairs)
        if (p.aggressor == b.clk && p.victim == victim) hit = &p;
    CHECK(hit != nullptr);
    if (hit) {
        CHECK(hit->broadside && hit->layer == "Top / Inner 1");
        CHECK_NEAR(hit->coupledLength, 60, 0.01);
        CHECK(hit->next > 0.02 && hit->fext < 0);
    }
    const Json j = crosstalkJson(b.p);
    bool flagged = false;
    for (size_t k = 0; k < j.get("pairs").size(); ++k) flagged |= j.get("pairs")[k].get("broadside").asBool();
    CHECK(flagged);
}

extern "C" int sieda_c_api_pi_test(const char* project_json);

TEST(pi_cavity_decap_plan_and_ir_map) {
    // Cavity: 100 × 60 mm plane pair, 0.2 mm FR-4. Low frequency: the plate capacitance; first resonance f10 = c/(2a√εr).
    const double a = 100, bw = 60, d = 0.2, er = 4.4;
    CHECK_NEAR(cavityModeFrequency(a, bw, er, 1, 0), kSpeedOfLight / (2 * 0.1 * std::sqrt(er)), 1);
    const double cPlane = planeCapacitance(a * bw, d, er);
    const std::vector<CavityPort> corner{{1, 1, 0.5}};
    const double f1 = 1e6;
    CHECK_NEAR(std::abs(cavityImpedance(a, bw, d, er, 0.02, corner, f1)(0, 0)), 1 / (2 * kPi * f1 * cPlane), 0.02 / (2 * kPi * f1 * cPlane));
    const double f10 = cavityModeFrequency(a, bw, er, 1, 0);
    double peakF = 0, peakZ = 0;
    for (double f = 0.85 * f10; f <= 1.15 * f10; f += 1e6) {
        const double z = std::abs(cavityImpedance(a, bw, d, er, 0.02, corner, f)(0, 0));
        if (z > peakZ) {
            peakZ = z;
            peakF = f;
        }
    }
    CHECK_NEAR(peakF, f10, 0.02 * f10);
    // Reciprocity of the port matrix; in the plate's centre the (1,0) mode is not excited (cos(π/2) = 0).
    const std::vector<CavityPort> two{{10, 10, 0.5}, {70, 40, 0.5}};
    const CMat z2 = cavityImpedance(a, bw, d, er, 0.02, two, 300e6);
    CHECK(std::abs(z2(0, 1) - z2(1, 0)) < 1e-12);
    const std::vector<CavityPort> centre{{50, 30, 0.5}};
    CHECK(std::abs(cavityImpedance(a, bw, d, er, 0.02, centre, f10)(0, 0)) < 0.2 * peakZ);

    // A rail with planes and decoupling: the cavity curve, the lumped one and the regulator override.
    SiBoard b = siBoard();
    b.p.pcb.zones.push_back({"GND", 1, true, 0});
    b.p.pcb.zones.push_back({"+3V3", 2, true, 0});
    auto& s = b.p.schematic;
    auto addCap = [&](const char* value, const char* package, Vec2 at) {
        int c = s.addComponent(ComponentKind::Capacitor, value, {600, 0});
        s.setPackage(c, package);
        int vl = s.addComponent(ComponentKind::NetLabel, "+3V3", {580, 0});
        int gl = s.addComponent(ComponentKind::Ground, "", {620, 0});
        wire(s, c, "1", vl, "N");
        wire(s, c, "2", gl, "GND");
        b.p.schematicChanged();
        siPlace(b.p, c, at);
    };
    addCap("100n", "C_0402", {14, 24});
    addCap("100n", "C_0402", {104, 24});
    auto rail = [](const Project& p) {
        for (const auto& x : analyzePdn(p))
            if (x.name == "+3V3") return x;
        return PdnRailResult{};
    };
    const PdnRailResult r = rail(b.p);
    CHECK(r.planeC > 0 && r.planeX1 > r.planeX0 && r.decaps.size() == 2);
    const PdnCavityResult cav = pdnCavity(b.p, r);
    CHECK(cav.available && cav.freq.size() == 121 && cav.zCavity.size() == 121 && cav.ports == 4);
    CHECK(!cav.modes.empty() && cav.modes[0].m == 1 && cav.modes[0].n == 0);
    if (!cav.zCavity.empty()) CHECK_NEAR(cav.zCavity[0] / cav.zLumped[0], 1.0, 0.25);  // both VRM / bulk dominated at 1 MHz
    const Json cj = pdnCavityJson(b.p, "+3V3");
    CHECK(cj.get("available").asBool() && cj.get("zCavity").size() == 121);
    CHECK(pdnCavityJson(b.p, "NOPE").has("error"));
    b.p.si.rails.push_back({"+3V3", 0, 0, 0, 0.002, 200e3});
    const PdnRailResult ov = rail(b.p);
    CHECK_NEAR(ov.vrmR, 0.002, 1e-15);
    CHECK_NEAR(ov.vrmL, 0.002 / (2 * kPi * 200e3), 1e-18);
    CHECK_NEAR(ov.vrmBandwidth, 200e3, 1e-6);

    // Decoupling plan for an undecoupled rail: greedy additions until |Z| meets the target.
    SiBoard nd = siBoard();
    const PdnRailResult bare = rail(nd.p);
    const PdnDecapPlan plan = pdnDecapPlan(bare);
    CHECK(plan.needed && plan.compliant && !plan.additions.empty());
    CHECK(plan.worstAfter <= 1.0 && plan.worstBefore > 1.0);
    CHECK(plan.zAfter.size() == plan.freq.size());
    int added = 0;
    for (const auto& x : plan.additions) added += x.count;
    CHECK(added >= 1 && added <= 40);

    // IR-drop map on tracks: the 0.5 A common run carries 0.5 A / (0.5 mm × 35 µm) = 28.6 A/mm².
    const Vec2 j = siPad(nd.p, nd.j1, "1"), v1 = siPad(nd.p, nd.u1, "VDD"), v2 = siPad(nd.p, nd.u2, "VDD");
    siTrack(nd.p, nd.vcc, j, {j.x, 2}, 0, 0.5);
    siTrack(nd.p, nd.vcc, {j.x, 2}, {v1.x, 2}, 0, 0.5);
    siTrack(nd.p, nd.vcc, {v1.x, 2}, v1, 0, 0.5);
    siTrack(nd.p, nd.vcc, {v1.x, 2}, {v2.x, 2}, 0, 0.5);
    siTrack(nd.p, nd.vcc, {v2.x, 2}, v2, 0, 0.5);
    const PdnRailResult ir = rail(nd.p);
    CHECK(ir.irAnalyzed && !ir.irSegments.empty());
    CHECK_NEAR(ir.irMaxDensity, 0.5 / (0.5 * 0.035), 0.5);
    const Json map = pdnIrMapJson(nd.p, "+3V3");
    CHECK(map.get("segments").size() >= 5 && map.get("hotspots").size() >= 1 && map.get("loads").size() == 2);
    CHECK(map.get("board").get("outline").size() >= 4);
    // Through a pour: cells with drops and densities.
    const PdnRailResult pour = rail(b.p);
    CHECK(pour.irAnalyzed && !pour.irCells.empty());
    b.p.si.rails.clear();
    CHECK(sieda_c_api_pi_test(b.p.toJson().dump().c_str()) == 0);
}

TEST(si_channel_ends_at_connector) {
    // U2 OUT → J2 (a connector, no logic input): the channel ends at the connector pad.
    SiBoard b = siBoard();
    auto& s = b.p.schematic;
    int j2 = s.addComponent(ComponentKind::Connector, "OUT", {600, 0});
    wire(s, b.u2, "OUT", j2, "1");
    b.p.schematicChanged();
    siPlace(b.p, j2, {110, 8});
    const int net = s.netOf({b.u2, pin(s, b.u2, "OUT")});
    const Vec2 a = siPad(b.p, b.u2, "OUT"), c = siPad(b.p, j2, "1");
    siTrack(b.p, net, a, {a.x, c.y});
    siTrack(b.p, net, {a.x, c.y}, c);
    ChannelOptions o;
    o.net = s.nets()[static_cast<size_t>(net)].name;
    o.partner = "none";
    const ChannelModel m = extractChannel(b.p, o);
    CHECK(m.error.empty());
    CHECK(m.receiverRef == s.find(j2)->ref && m.lengthP > 5);
    CHECK(!m.notes.empty() && m.notes.back().find("no logic receiver") != std::string::npos);
    EyeOptions e;
    e.bitRate = 1e9;
    ChannelDrive ideal;
    ideal.idealDriver = true;
    const Json j = channelJson(b.p, o, ideal, &e, nullptr);
    CHECK(j.get("error").isNull() && j.get("eye").get("open").asBool());
}

// ======================================================================= interactive router: drag, via drag, tuning

namespace {
/// Net A runs r1 → top → via V1 (18, 20) → bottom → via V2 (26, 20) → top → r2; net B is a vertical top-layer track
/// at x = 21 between two pads.
struct ViaBoard {
    Project p;
    int netA = -1, netB = -1;
    int via1 = -1;
};

ViaBoard viaBoard() {
    ViaBoard b;
    auto& s = b.p.schematic;
    const int r1 = placeR(b.p, {10, 20}), r2 = placeR(b.p, {34, 20});
    const int r3 = placeR(b.p, {21, 8}, 90), r4 = placeR(b.p, {21, 32}, 90);
    wire(s, r1, "2", r2, "1");
    wire(s, r3, "2", r4, "1");
    b.p.schematicChanged();
    b.netA = s.netOf({r1, 1});
    b.netB = s.netOf({r3, 1});
    addPath(b.p.pcb, b.netA, 0, 0.25, {padAt(b.p, r1, 1), {18, 20}});
    addPath(b.p.pcb, b.netA, 1, 0.25, {{18, 20}, {26, 20}});
    addPath(b.p.pcb, b.netA, 0, 0.25, {{26, 20}, padAt(b.p, r2, 0)});
    addPath(b.p.pcb, b.netB, 0, 0.25, {padAt(b.p, r3, 1), padAt(b.p, r4, 0)});
    Via v;
    v.net = b.netA;
    v.position = {18, 20};
    b.via1 = b.p.pcb.addVia(v);
    v.position = {26, 20};
    b.p.pcb.addVia(v);
    return b;
}

/// Acute-angle (acid trap) warnings of the DRC.
int acuteWarnings(const Project& p) {
    int n = 0;
    for (const auto& v : p.pcb.runDRC(p.schematic)) n += v.code == "DRC_ACUTE_ANGLE" ? 1 : 0;
    return n;
}
}  // namespace

TEST(router_drags_a_via_and_shoves) {
    ViaBoard b = viaBoard();
    CHECK(routingProblems(b.p) == 0);
    CHECK(netRouted(b.p, b.netA) && netRouted(b.p, b.netB));
    {
        InteractiveRouter r(b.p.pcb, b.p.schematic);
        CHECK(r.beginViaDrag(b.via1, {18, 20}));
        CHECK(r.preview().kind == "via");
        CHECK(r.preview().hiddenVias.size() == 1);
        // Pushed into net B's track: the track is shoved aside, the via lands where it was dropped.
        const RoutePreview& pv = r.moveTo({20.5, 20.3});
        CHECK(!pv.blocked);
        CHECK(!pv.shovedTracks.empty());
        CHECK(pv.vias.size() == 1);
        CHECK(pv.head.size() >= 2);  // the top and bottom tracks follow the via
        CHECK(std::fabs(pv.end.x - 20.5) < 1e-9 && std::fabs(pv.end.y - 20.3) < 1e-9);
        const RouteChanges ch = r.commit();
        CHECK(ch.ok && ch.addedVias.size() == 1 && ch.removedVias.size() == 1);
    }
    bool moved = false;
    for (const auto& v : b.p.pcb.vias) moved = moved || (std::fabs(v.position.x - 20.5) < 1e-9 && v.net == b.netA);
    CHECK(moved);
    CHECK(netRouted(b.p, b.netA) && netRouted(b.p, b.netB));
    CHECK(routingProblems(b.p) == 0);
    CHECK(acuteWarnings(b.p) == 0);
    // Walk around: nothing moves, the via stops short of net B's track.
    ViaBoard c = viaBoard();
    {
        InteractiveRouter r(c.p.pcb, c.p.schematic);
        RouterOptions o;
        o.mode = RouterMode::Walkaround;
        r.setOptions(o);
        CHECK(r.beginViaDrag(c.via1, {18, 20}));
        const RoutePreview& pv = r.moveTo({21, 20});
        CHECK(pv.blocked);
        CHECK(pv.shovedTracks.empty());
        CHECK(pv.end.x < 21 - 0.3 - 0.2 - 0.125 + 1e-3);
        CHECK(r.commit().ok);
        CHECK(routingProblems(c.p) == 0);
        CHECK(netRouted(c.p, c.netA));
    }
    // Cancel leaves the board alone; a via in a pad of its net and an unknown via cannot be dragged.
    ViaBoard d = viaBoard();
    const auto vias = d.p.pcb.vias;
    InteractiveRouter rd(d.p.pcb, d.p.schematic);
    CHECK(rd.beginViaDrag(d.via1, {18, 20}));
    rd.moveTo({16, 24});
    rd.cancel();
    CHECK(d.p.pcb.vias.size() == vias.size() && d.p.pcb.vias[0].position == vias[0].position);
    CHECK(!rd.beginViaDrag(-7, {0, 0}));
    Via inPad;
    inPad.net = d.netA;
    inPad.position = d.p.pcb.tracks.front().a;  // r1's pad centre
    const int padVia = d.p.pcb.addVia(inPad);
    CHECK(!rd.beginViaDrag(padVia, inPad.position));
    CHECK(!rd.error().empty());
}

TEST(router_previews_length_tuning) {
    Project p;
    auto& s = p.schematic;
    const int r1 = placeR(p, {8, 20}), r2 = placeR(p, {42, 20});
    wire(s, r1, "2", r2, "1");
    p.schematicChanged();
    const int net = s.netOf({r1, 1});
    addPath(p.pcb, net, 0, 0.25, {padAt(p, r1, 1), padAt(p, r2, 0)});
    const int id = p.pcb.tracks.front().id;
    const double before = routedNetLength(p.pcb, net);
    LengthTuneOptions o;
    o.target = before + 4;
    o.spacing = 0.6;
    o.hasNear = true;
    o.near = {15, 20};
    o.apply = false;
    const LengthTuneResult pv = tuneTrackLength(p.pcb, s, id, o);
    CHECK(pv.ok && !pv.applied);
    CHECK(pv.group.empty());
    CHECK_NEAR(pv.after, before + 4, 0.05);
    CHECK(pv.removedTracks.size() == 1 && pv.removedTracks[0] == id);
    CHECK(pv.addedTracks.size() > 4);
    CHECK(p.pcb.tracks.size() == 1 && p.pcb.tracks.front().id == id);  // a preview leaves the board alone
    // The meander sits near the requested point, with legs 0.6 mm apart edge to edge.
    std::vector<double> legs;
    for (const auto& t : pv.addedTracks)
        if (std::fabs(t.a.x - t.b.x) < 1e-9) legs.push_back(t.a.x);
    CHECK(legs.size() >= 2);
    double mid = 0;
    for (double x : legs) mid += x / static_cast<double>(legs.size());
    CHECK(std::fabs(mid - 15) < 2);
    for (size_t k = 1; k < legs.size(); ++k) CHECK_NEAR(std::fabs(legs[k] - legs[k - 1]), 0.6 + 0.25, 1e-6);
    // Applying the same options writes exactly the previewed copper.
    o.apply = true;
    const LengthTuneResult ap = tuneTrackLength(p.pcb, s, id, o);
    CHECK(ap.ok && ap.applied);
    CHECK_NEAR(ap.after, pv.after, 1e-9);
    CHECK(p.pcb.tracks.size() == pv.addedTracks.size());
    CHECK(netRouted(p, net));
    CHECK(routingProblems(p) == 0);
    CHECK(acuteWarnings(p) == 0);
    // JSON round trip of the options and the result.
    const LengthTuneOptions jo =
        lengthTuneOptionsFromJson(Json::parse("{\"target\":12.5,\"spacing\":0.4,\"x\":3,\"y\":4,\"apply\":false}"));
    CHECK(jo.target == 12.5 && jo.spacing == 0.4 && jo.hasNear && jo.near == Vec2(3, 4) && !jo.apply);
    CHECK(lengthTuneJson(pv).dump().find("\"addedTracks\":[{") != std::string::npos);
}

TEST(router_tunes_pair_skew_and_reports_the_target) {
    // USB_P runs straight; USB_N takes a detour, so it is longer: tuning P with no target matches it to N.
    Project p;
    auto& s = p.schematic;
    const int p1 = placeR(p, {10, 14}), p2 = placeR(p, {40, 14}), n1 = placeR(p, {10, 18}), n2 = placeR(p, {40, 18});
    wire(s, p1, "2", p2, "1");
    wire(s, n1, "2", n2, "1");
    wire(s, s.addComponent(ComponentKind::NetLabel, "USB_P", {0, 0}), "N", p1, "2");
    wire(s, s.addComponent(ComponentKind::NetLabel, "USB_N", {0, 50}), "N", n1, "2");
    p.schematicChanged();
    const int netP = s.netOf({p1, 1}), netN = s.netOf({n1, 1});
    addPath(p.pcb, netP, 0, 0.25, {padAt(p, p1, 1), padAt(p, p2, 0)});
    const Vec2 a = padAt(p, n1, 1), b = padAt(p, n2, 0);
    addPath(p.pcb, netN, 0, 0.25, {a, {a.x + 4, a.y}, {a.x + 6, a.y + 2}, {b.x - 6, b.y + 2}, {b.x - 4, b.y}, b});
    const double lenN = routedNetLength(p.pcb, netN);
    CHECK(lenN > routedNetLength(p.pcb, netP) + 1);
    // The route preview reports the net's length and the length its pair partner has.
    {
        InteractiveRouter r(p.pcb, s);
        CHECK(r.beginRoute(padAt(p, p1, 1), 0));
        const RoutePreview& pv = r.moveTo({20, 12});
        CHECK_NEAR(pv.targetLength, lenN, 1e-9);
        CHECK_NEAR(pv.netLength, routedNetLength(p.pcb, netP) + pv.length, 1e-9);
        r.cancel();
    }
    LengthTuneOptions o;
    o.apply = false;
    const LengthTuneResult pv = tuneTrackLength(p.pcb, s, p.pcb.tracks.front().id, o);
    CHECK(pv.ok);
    CHECK(pv.groupKind == "pair");
    CHECK_NEAR(pv.target, lenN, 1e-9);
    CHECK(std::fabs(pv.after - lenN) <= pv.tolerance + 1e-6);
    o.apply = true;
    const LengthTuneResult ap = tuneTrackLength(p.pcb, s, p.pcb.tracks.front().id, o);
    CHECK(ap.ok && ap.applied);
    CHECK(std::fabs(routedNetLength(p.pcb, netP) - lenN) <= p.pcb.settings.pairSkewTolerance / 2 + 1e-6);
    CHECK(routingProblems(p) == 0);
    CHECK(netRouted(p, netP) && netRouted(p, netN));
}

extern "C" int sieda_c_api_router_drag_tune_test(void);

TEST(c_api_router_drag_and_tune) {
    const int rc = sieda_c_api_router_drag_tune_test();
    if (rc != 0) std::printf("    c api drag / tune step %d failed\n", rc);
    CHECK(rc == 0);
}

TEST(router_shoved_copper_adds_no_drc_warning) {
    // Routes, drags and via drags with shove at pseudo-random spots of autorouted boards (several seeds, 2 and 4
    // layers): no commit may add a DRC error, a clearance warning or an acute-angle warning.
    int commits = 0, shoved = 0, newAcute = 0;
    for (unsigned seed : {7u, 11u, 2024u, 99u}) {
        auto rnd = [&seed] {
            seed = seed * 1103515245u + 12345u;
            return ((seed >> 8) & 0xFFFFFF) / double(0xFFFFFF);
        };
        Project p;
        auto& s = p.schematic;
        std::vector<int> ids;
        for (int i = 0; i < 24; ++i) ids.push_back(s.addComponent(ComponentKind::Resistor, "1k", {(i % 6) * 100.0, (i / 6) * 100.0}));
        for (size_t k = 0; k < ids.size(); ++k) {
            const size_t other = static_cast<size_t>(rnd() * ids.size()) % ids.size();
            if (other != k) s.connect({ids[k], 1}, {ids[other], 0});
        }
        p.pcb.settings.width = 40;
        p.pcb.settings.height = 30;
        p.pcb.settings.layerCount = seed % 2 ? 2 : 4;
        p.schematicChanged();
        p.pcb.autoPlace(s, true);
        p.pcb.autoRoute(s);
        int acute = acuteWarnings(p);
        CHECK(routingProblems(p) == 0);
        for (int it = 0; it < 40; ++it) {
            InteractiveRouter r(p.pcb, s);
            const double pick = rnd();
            bool ok = false;
            if (pick < 0.3 && !p.pcb.tracks.empty()) {
                const Track& t = p.pcb.tracks[static_cast<size_t>(rnd() * p.pcb.tracks.size()) % p.pcb.tracks.size()];
                ok = r.beginDrag(t.id, (t.a + t.b) * 0.5);
            } else if (pick < 0.45 && !p.pcb.vias.empty()) {
                const Via& v = p.pcb.vias[static_cast<size_t>(rnd() * p.pcb.vias.size()) % p.pcb.vias.size()];
                ok = r.beginViaDrag(v.id, v.position);
            } else {
                const auto pads = p.pcb.pads(s);
                const Pad& pd = pads[static_cast<size_t>(rnd() * pads.size()) % pads.size()];
                ok = r.beginRoute(pd.position, 0);
            }
            if (!ok) continue;
            const Vec2 at = r.preview().end;
            const bool local = r.preview().kind != "route";
            for (int m = 0; m < 3; ++m) {
                const Vec2 c = local ? at + Vec2{(rnd() - 0.5) * 2, (rnd() - 0.5) * 2}
                                     : Vec2{rnd() * p.pcb.settings.width, rnd() * p.pcb.settings.height};
                shoved += r.moveTo(c).shovedTracks.empty() ? 0 : 1;
                if (!local && rnd() < 0.5) r.fixHead();
            }
            if (!r.commit().ok) continue;
            ++commits;
            const int problems = routingProblems(p);
            CHECK(problems == 0);
            const int now = acuteWarnings(p);
            if (now > acute) {
                ++newAcute;
                std::printf("    seed %u step %d: %d new acute-angle warning(s)\n", seed, it, now - acute);
            }
            acute = now;
            if (problems) break;
        }
    }
    std::printf("    %d commits, %d shoving moves, %d commits added acute angles\n", commits, shoved, newAcute);
    CHECK(commits >= 80);
    CHECK(shoved > 20);
    CHECK(newAcute == 0);
}

TEST(router_shoves_lines_around_hole_keepouts) {
    // Net B runs straight down x = 21; a mounting hole sits just right of it. Routing net A up to x = 21.6 pushes B
    // right, into the keep-out: B walks round the keep-out instead of refusing (which left the head blocked).
    Project p;
    auto& s = p.schematic;
    const int r1 = placeR(p, {10, 20});
    const int r3 = placeR(p, {21, 8}, 90), r4 = placeR(p, {21, 32}, 90);
    const int r5 = placeR(p, {40, 20});
    wire(s, r1, "2", r5, "1");
    wire(s, r3, "2", r4, "1");
    p.schematicChanged();
    p.pcb.settings.holes.push_back({{23, 20}, 1.0, 2.0});
    const int netB = s.netOf({r3, 1});
    addPath(p.pcb, netB, 0, 0.25, {padAt(p, r3, 1), padAt(p, r4, 0)});
    CHECK(routingProblems(p) == 0);
    InteractiveRouter r(p.pcb, s);
    CHECK(r.beginRoute(padAt(p, r1, 1), 0));
    const RoutePreview& pv = r.moveTo({21.6, 20});
    CHECK(!pv.blocked);
    CHECK(!pv.shovedTracks.empty());
    bool beyondHole = false;
    for (const auto& t : pv.shovedTracks) beyondHole = beyondHole || std::max(t.a.x, t.b.x) > 24;
    CHECK(beyondHole);
    CHECK(r.commit().ok);
    CHECK(routingProblems(p) == 0);
    CHECK(acuteWarnings(p) == 0);
    CHECK(netRouted(p, netB));
}

TEST(router_highlights_collisions) {
    // Highlight mode: the head goes straight through the lanes; nothing moves and each lane it crosses is listed.
    LaneBoard b = laneBoard();
    InteractiveRouter r(b.p.pcb, b.p.schematic);
    RouterOptions o;
    o.mode = RouterMode::Highlight;
    r.setOptions(o);
    CHECK(r.beginRoute(b.from, 0));
    const RoutePreview& pv = r.moveTo({b.from.x, 4});
    CHECK(!pv.blocked);
    CHECK(pv.shovedTracks.empty() && pv.hiddenTracks.empty());
    std::set<int> lanes;
    for (const auto& c : pv.collisions)
        if (c.kind == "track")
            for (const auto& t : b.p.pcb.tracks)
                if (t.id == c.id) lanes.insert(t.net);
    CHECK(lanes.size() == 3);
    CHECK(pv.status.find("collision") != std::string::npos);
    CHECK(routePreviewJson(pv).dump().find("\"kind\":\"track\"") != std::string::npos);
    CHECK(routerOptionsFromJson(Json::parse("{\"mode\":\"highlight\"}")).mode == RouterMode::Highlight);
    // Placed as asked: the DRC reports the crossings.
    CHECK(r.commit().ok);
    CHECK(routingProblems(b.p) > 0);
    // The other modes never report collisions.
    LaneBoard c = laneBoard();
    InteractiveRouter rs(c.p.pcb, c.p.schematic);
    CHECK(rs.beginRoute(c.from, 0));
    CHECK(rs.moveTo({c.from.x, 4}).collisions.empty());
}

TEST(router_places_blind_buried_and_micro_vias) {
    // 4-layer HDI board: top → microvia to Inner 1 → microvia to Inner 2 (buried) → blind via to the bottom → through
    // via back to the top pad. Every span is what was asked for, and the board stays DRC clean.
    Project p;
    auto& s = p.schematic;
    const int r1 = placeR(p, {8, 20}), r2 = placeR(p, {44, 20});
    wire(s, r1, "2", r2, "1");
    p.schematicChanged();
    p.pcb.settings.layerCount = 4;
    p.pcb.settings.hdi = true;
    const int net = s.netOf({r1, 1});
    InteractiveRouter r(p.pcb, s);
    RouterOptions o;
    o.viaType = RouterViaType::Micro;
    r.setOptions(o);
    CHECK(r.beginRoute(padAt(p, r1, 1), 0));
    r.moveTo({14, 20});
    CHECK(!r.addVia(2));  // a microvia joins neighbouring layers only
    CHECK(r.error().find("neighbouring") != std::string::npos);
    CHECK(r.addVia());  // the next layer down
    CHECK(r.preview().layer == 1);
    r.moveTo({20, 20});
    CHECK(r.addVia());
    CHECK(r.preview().layer == 2);
    o.viaType = RouterViaType::Blind;
    r.setOptions(o);
    r.moveTo({26, 20});
    CHECK(r.addVia(3));
    CHECK(r.preview().layer == 3);
    o.viaType = RouterViaType::Through;
    r.setOptions(o);
    r.moveTo({32, 20});
    CHECK(r.addVia(0));
    const RoutePreview& pv = r.moveTo(padAt(p, r2, 0));
    CHECK(pv.reachedTarget);
    CHECK(pv.vias.size() == 4);
    CHECK(routePreviewJson(pv).dump().find("\"kind\":\"microvia\"") != std::string::npos);
    CHECK(r.commit().ok);
    std::vector<std::string> kinds;
    for (const auto& v : p.pcb.vias) kinds.push_back(viaKind(v, 4));
    CHECK(kinds.size() == 4);
    if (kinds.size() == 4) {
        CHECK(kinds[0] == "microvia" && p.pcb.vias[0].fromLayer == 0 && p.pcb.vias[0].toLayer == 1);
        CHECK(kinds[1] == "microvia" && p.pcb.vias[1].fromLayer == 1 && p.pcb.vias[1].toLayer == 2);
        CHECK(kinds[2] == "blind" && p.pcb.vias[2].fromLayer == 2 && p.pcb.vias[2].toLayer == -1);
        CHECK(kinds[3] == "through");
        CHECK_NEAR(p.pcb.vias[0].drill, p.pcb.settings.microviaDrill, 1e-12);
    }
    CHECK(netRouted(p, net));
    CHECK(routingProblems(p) == 0);
    CHECK(acuteWarnings(p) == 0);
    // Auto picks a microvia for the next layer on an HDI board, a through via without HDI; blind / micro need HDI.
    {
        Project q = p;
        q.pcb.clearRouting();
        InteractiveRouter ra(q.pcb, q.schematic);
        RouterOptions oa;
        oa.viaType = RouterViaType::Auto;
        ra.setOptions(oa);
        CHECK(ra.beginRoute(padAt(q, r1, 1), 0));
        ra.moveTo({14, 20});
        CHECK(ra.addVia());
        CHECK(ra.preview().layer == 1 && ra.preview().vias.size() == 1);
        if (ra.preview().vias.size() == 1) CHECK(std::string(viaKind(ra.preview().vias[0], 4)) == "microvia");
        ra.cancel();
        q.pcb.settings.hdi = false;
        CHECK(ra.beginRoute(padAt(q, r1, 1), 0));
        ra.moveTo({14, 20});
        CHECK(ra.addVia());
        CHECK(ra.preview().layer == 3);  // no HDI: a through via to the other side
        ra.cancel();
        oa.viaType = RouterViaType::Blind;
        ra.setOptions(oa);
        CHECK(ra.beginRoute(padAt(q, r1, 1), 0));
        ra.moveTo({14, 20});
        CHECK(!ra.addVia());
        CHECK(ra.error().find("HDI") != std::string::npos);
    }
    CHECK(routerOptionsFromJson(Json::parse("{\"viaType\":\"micro\"}")).viaType == RouterViaType::Micro);
    CHECK(routerOptionsFromJson(Json::parse("{\"viaType\":\"auto\"}")).viaType == RouterViaType::Auto);
}

TEST(router_rounds_corners) {
    // Rounded corners: a route with a 45° and a 90° corner is written with arcs of short chords (each join turns at
    // most 15°), exactly as previewed, connected and DRC clean. A via of another net inside a corner keeps it sharp.
    for (bool obstacle : {false, true}) {
        Project p;
        auto& s = p.schematic;
        const int r1 = placeR(p, {8, 20}), r2 = placeR(p, {40, 32}), r3 = placeR(p, {45, 5}), r4 = placeR(p, {45, 10});
        wire(s, r1, "2", r2, "1");
        wire(s, r3, "2", r4, "1");
        p.schematicChanged();
        const int net = s.netOf({r1, 1});
        if (obstacle) {
            Via v;
            v.net = s.netOf({r3, 1});
            v.position = {19.732, 20.647};  // on the bisector, 0.7 mm inside the first corner at (20, 20)
            p.pcb.addVia(v);
        }
        InteractiveRouter r(p.pcb, s);
        RouterOptions o;
        o.cornerRadius = 2;
        r.setOptions(o);
        CHECK(r.beginRoute(padAt(p, r1, 1), 0));
        r.moveTo({20, 20});
        CHECK(r.fixHead());
        r.moveTo({28, 28});  // 45° corner at (20, 20)
        CHECK(r.fixHead());
        const RoutePreview& pv = r.moveTo(padAt(p, r2, 0));  // a 45° corner at (28, 28) onto the pad's row
        CHECK(pv.reachedTarget);
        double previewLength = 0;
        for (const auto& t : pv.placed) previewLength += (t.b - t.a).length();
        for (const auto& t : pv.head) previewLength += (t.b - t.a).length();
        CHECK(r.commit().ok);
        CHECK(netRouted(p, net));
        CHECK(routingProblems(p) == 0);
        CHECK(acuteWarnings(p) == 0);
        CHECK_NEAR(routedNetLength(p.pcb, net), previewLength, 1e-6);
        // Joins between the route's own segments.
        double sharpest = 180;
        int chords = 0;
        for (const auto& a : p.pcb.tracks)
            for (const auto& b : p.pcb.tracks) {
                if (a.id >= b.id || a.net != net || b.net != net) continue;
                for (Vec2 pa : {a.a, a.b})
                    for (Vec2 pb : {b.a, b.b}) {
                        if ((pa - pb).length() > 1e-6) continue;
                        const Vec2 da = (pa == a.a ? a.b : a.a) - pa, db = (pb == b.a ? b.b : b.a) - pb;
                        const double ang = std::acos(std::clamp(da.dot(db) / (da.length() * db.length()), -1.0, 1.0)) * 180 / kPi;
                        sharpest = std::min(sharpest, ang);
                        chords += ang < 179.9 ? 1 : 0;
                    }
            }
        if (obstacle) {
            CHECK(sharpest < 136);  // the corner next to the via stays a plain 45° corner
        } else {
            CHECK(sharpest > 164.9);  // every corner became an arc
            CHECK(chords >= 6);
        }
    }
    CHECK(routerOptionsFromJson(Json::parse("{\"cornerRadius\":-1}")).cornerRadius < 0);
}

TEST(router_head_update_can_be_cancelled) {
    // A cancelled head update leaves the router exactly as before it; the next update is unaffected and gives the
    // same result as on a router that was never cancelled. A request made while nothing runs changes nothing.
    LaneBoard a = laneBoard(), b = laneBoard();
    InteractiveRouter ra(a.p.pcb, a.p.schematic), rb(b.p.pcb, b.p.schematic);
    std::atomic<unsigned> counter{0};
    ra.setAbortSource(&counter);
    CHECK(ra.beginRoute(a.from, 0) && rb.beginRoute(b.from, 0));
    ra.moveTo({20, 23.3});
    rb.moveTo({20, 23.3});
    const std::string before = routePreviewJson(ra.preview()).dump();
    ra.requestAbort();  // nothing runs: no effect on what follows
    counter.fetch_add(1);
    CHECK(!ra.moveTo({20, 23.3}).aborted);
    CHECK(routePreviewJson(ra.preview()).dump() == before);
    // Cancel while a slow update runs (aimed into another net's pad: shove, walk-around search and a binary search
    // for the furthest fit take milliseconds): a helper thread keeps bumping the counter.
    std::atomic<bool> stop{false};
    std::thread bumper([&] {
        while (!stop.load()) counter.fetch_add(1);
    });
    const unsigned started = counter.load();
    while (counter.load() - started < 1000) std::this_thread::yield();  // the helper is running
    int cancelledMoves = 0;
    bool unchanged = true;
    for (int attempt = 0; attempt < 20 && cancelledMoves == 0; ++attempt) {
        RoutePreview cancelled = ra.moveTo(padAt(a.p, a.p.schematic.components()[1].id, 0));
        if (!cancelled.aborted) {
            ra.moveTo({20, 23.3});  // completed instead: back to the state before, try again
            continue;
        }
        ++cancelledMoves;
        cancelled.aborted = false;
        unchanged = unchanged && routePreviewJson(cancelled).dump() == before;
    }
    stop = true;
    bumper.join();
    CHECK(cancelledMoves > 0);
    CHECK(unchanged);
    // The next update runs normally and matches the router that was never cancelled.
    const std::string next = routePreviewJson(ra.moveTo({45, 23.3})).dump();
    CHECK(!ra.preview().aborted);
    CHECK(next == routePreviewJson(rb.moveTo({45, 23.3})).dump());
    CHECK(ra.commit().ok && rb.commit().ok);
    CHECK(routingProblems(a.p) == 0);
    // C API: abort is a no-op without a router, and safe to call at any time.
    SiedaProject* p = sieda_project_new("abort");
    sieda_router_abort(p);
    sieda_router_abort(nullptr);
    CHECK(sieda_router_active(p) == 0);
    sieda_project_free(p);
}

namespace {
/// Two SOIC-8 parts 30 mm apart; U1's right-hand pins 5…8 are wired to U2's left-hand pins 4…1 (same rows).
struct BusBoard {
    Project p;
    int u1 = -1, u2 = -1;
    std::vector<int> nets;  // U1 pin 5, 6, 7, 8
};

BusBoard busBoard() {
    BusBoard b;
    auto& s = b.p.schematic;
    CustomPartSpec spec;
    spec.name = "BUS-SOIC8";
    spec.package.type = "SOIC";
    spec.package.pinCount = 8;
    for (int i = 1; i <= 8; ++i) {
        CustomPin pin;
        pin.number = std::to_string(i);
        pin.name = "P" + std::to_string(i);
        pin.type = PinType::Passive;
        spec.pins.push_back(pin);
    }
    const std::string part = b.p.addCustomPart(spec);
    b.u1 = s.addCustomComponent(part, "", {0, 0});
    b.u2 = s.addCustomComponent(part, "", {400, 0});
    for (int k = 0; k < 4; ++k) wire(s, b.u1, ("P" + std::to_string(5 + k)).c_str(), b.u2, ("P" + std::to_string(4 - k)).c_str());
    b.p.schematicChanged();
    for (auto [id, x] : {std::pair<int, double>{b.u1, 12}, {b.u2, 42}}) {
        Component* c = s.find(id);
        c->pcb.position = {x, 20};
        c->pcb.placed = true;
    }
    for (int k = 0; k < 4; ++k) b.nets.push_back(s.netOf({b.u1, pin(s, b.u1, ("P" + std::to_string(5 + k)).c_str())}));
    return b;
}
}  // namespace

TEST(router_routes_a_bus_together) {
    BusBoard b = busBoard();
    auto& s = b.p.schematic;
    const Vec2 p8 = padAt(b.p, b.u1, pin(s, b.u1, "P8"));
    InteractiveRouter r(b.p.pcb, s);
    CHECK(!r.beginBus({1, 1}, 0, 4));  // not on a pad
    CHECK(r.beginBus(p8, 0, 4));
    CHECK(r.preview().kind == "bus" && r.preview().nets.size() == 4);
    const RoutePreview& pv = r.moveTo({30, 20});
    CHECK(!pv.blocked);
    CHECK(!r.addVia());  // vias track by track, after the bus
    CHECK(r.commit().ok);
    CHECK(routingProblems(b.p) == 0);
    CHECK(acuteWarnings(b.p) == 0);
    // Each member ends in the bundle at x = 30: four parallel ends at track pitch (width + clearance), in pin order.
    const double pitch = b.p.pcb.settings.trackWidth + b.p.pcb.settings.clearance;
    std::vector<std::pair<double, int>> ends;
    for (const auto& t : b.p.pcb.tracks)
        for (Vec2 e : {t.a, t.b})
            if (std::fabs(e.x - 30) < 1e-6) ends.push_back({e.y, t.net});
    std::sort(ends.begin(), ends.end());
    CHECK(ends.size() == 4);
    for (size_t k = 1; k < ends.size(); ++k) CHECK_NEAR(ends[k].first - ends[k - 1].first, pitch, 1e-6);
    // Each track is then finished on its own, onto its pad of U2: everything routed, DRC clean.
    for (size_t k = 0; k < ends.size(); ++k) {
        InteractiveRouter rk(b.p.pcb, s);
        CHECK(rk.beginRoute({30, ends[k].first}, 0));
        Vec2 target{-1, -1};
        for (const auto& pd : b.p.pcb.pads(s))
            if (pd.net == ends[k].second && pd.componentId == b.u2) target = pd.position;
        const RoutePreview& fk = rk.moveTo(target);
        CHECK(fk.reachedTarget);
        CHECK(rk.commit().ok);
    }
    for (int n : b.nets) CHECK(netRouted(b.p, n));
    CHECK(routingProblems(b.p) == 0);
    CHECK(acuteWarnings(b.p) == 0);
}

TEST(router_bus_turns_corners_at_pitch) {
    // The bundle turns a 90° corner (45° posture: two 45° bends) and stays at pitch: no member comes closer to
    // another than the clearance anywhere, and DRC stays clean.
    BusBoard b = busBoard();
    auto& s = b.p.schematic;
    InteractiveRouter r(b.p.pcb, s);
    CHECK(r.beginBus(padAt(b.p, b.u1, pin(s, b.u1, "P5")), 0, 4));
    r.moveTo({24, 20});
    CHECK(r.fixHead());
    const RoutePreview& pv = r.moveTo({30, 34});
    CHECK(!pv.blocked);
    CHECK(r.commit().ok);
    CHECK(routingProblems(b.p) == 0);
    CHECK(acuteWarnings(b.p) == 0);
    double closest = 1e9;
    for (const auto& t : b.p.pcb.tracks)
        for (const auto& u : b.p.pcb.tracks)
            if (t.net != u.net) closest = std::min(closest, segmentSegmentDistance(t.a, t.b, u.a, u.b) - (t.width + u.width) / 2);
    CHECK(closest >= b.p.pcb.settings.clearance - 1e-6);
    CHECK(closest <= b.p.pcb.settings.clearance + 1e-6);  // packed at pitch somewhere
}

TEST(router_fans_out_a_part) {
    // U1's four wired pins get an escape and a via each, straight out of the pad row; its four unconnected pins are
    // skipped. A second fanout finds nothing left to do. The board stays DRC clean.
    BusBoard b = busBoard();
    const FanoutResult f = fanoutComponent(b.p.pcb, b.p.schematic, b.u1);
    CHECK(f.ok);
    CHECK(f.fanned == 4 && f.skipped == 4 && f.failedPads.empty());
    CHECK(b.p.pcb.vias.size() == 4);
    for (const auto& v : b.p.pcb.vias) CHECK(v.position.x > 14.7 + 0.775);  // outside the right-hand pad column
    CHECK(routingProblems(b.p) == 0);
    CHECK(acuteWarnings(b.p) == 0);
    const FanoutResult again = fanoutComponent(b.p.pcb, b.p.schematic, b.u1);
    CHECK(!again.ok && again.fanned == 0 && again.skipped == 8);
    CHECK(b.p.pcb.vias.size() == 4);
    CHECK(!fanoutComponent(b.p.pcb, b.p.schematic, -1).ok);
    const FanoutOptions o = fanoutOptionsFromJson(Json::parse("{\"viaType\":\"micro\",\"shove\":true,\"distance\":1.5}"));
    CHECK(o.viaType == RouterViaType::Micro && o.shove && o.distance == 1.5);
    CHECK(fanoutJson(f).dump().find("\"fanned\":4") != std::string::npos);
}

TEST(router_fans_out_two_pin_parts_and_starts_their_bus) {
    // The app test's board: R1 at (20, 20) wired to R2 and R3 on both pins. Fanout gives both pads a via; the same
    // two pads also start a bus of two.
    Project p;
    auto& s = p.schematic;
    const int r1 = placeR(p, {20, 20}), r2 = placeR(p, {40, 10}), r3 = placeR(p, {40, 30});
    wire(s, r1, "2", r2, "1");
    wire(s, r1, "1", r3, "2");
    p.schematicChanged();
    {
        Project q = p;
        const FanoutResult f = fanoutComponent(q.pcb, q.schematic, r1);
        CHECK(f.fanned == 2 && q.pcb.vias.size() == 2);
        CHECK(routingProblems(q) == 0);
        CHECK(acuteWarnings(q) == 0);
    }
    InteractiveRouter r(p.pcb, s);
    CHECK(r.beginBus(padAt(p, r1, 1), 0, 2));
    const RoutePreview& pv = r.moveTo({20, 5});
    std::set<int> nets;
    for (const auto& t : pv.head) nets.insert(t.net);
    CHECK(nets.size() == 2);
    CHECK(!pv.blocked);
}

// ======================================================================= SPICE model import

extern "C" int sieda_c_api_spice_test(void);

namespace {
/// IC8 part driven at pin 1 by `vin` (DC), pin 2 loaded with 1 MΩ; the model `name` of `text` on the IC with `pins`.
/// Returns V(pin 2), NaN when the circuit does not simulate.
double spiceTwoPort(const std::string& text, const std::string& name, double vin, const std::string& pins = "1 2",
                    std::string* error = nullptr) {
    Schematic s;
    int v = s.addComponent(ComponentKind::VoltageSource, formatEngineeringValue(vin, "", 9), {0, 0});
    int u = s.addComponent(ComponentKind::IC8, "IC", {100, 0});
    int r = s.addComponent(ComponentKind::Resistor, "1MEG", {200, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    wire(s, v, "+", u, "1");
    wire(s, v, "-", g, "GND");
    wire(s, u, "2", r, "1");
    wire(s, r, "2", g, "GND");
    s.setSpiceModel(u, SpiceModelRef{text, name, pins});
    DcResult dc = Simulator(s).dcOperatingPoint();
    if (error) *error = dc.error;
    if (!dc.converged) return NAN;
    return netV(s, dc, u, "2");
}
double spicedevVt() { return 1.380649e-23 * 300.15 / 1.602176634e-19; }
bool hasDiag(const std::vector<SpiceDiagnostic>& d, SpiceDiagnostic::Level level, const std::string& text) {
    for (const auto& x : d)
        if (x.level == level && x.message.find(text) != std::string::npos) return true;
    return false;
}
}  // namespace

TEST(spice_numbers_and_expressions) {
    double v = 0;
    CHECK(parseSpiceNumber("1.5k", v) && std::fabs(v - 1500) < 1e-9);
    CHECK(parseSpiceNumber("10MEG", v) && std::fabs(v - 1e7) < 1e-3);
    CHECK(parseSpiceNumber("10m", v) && std::fabs(v - 0.01) < 1e-15);
    CHECK(parseSpiceNumber("100pF", v) && std::fabs(v - 1e-10) < 1e-22);
    CHECK(parseSpiceNumber("2.2u", v) && std::fabs(v - 2.2e-6) < 1e-18);
    CHECK(parseSpiceNumber("-1e-3", v) && std::fabs(v + 1e-3) < 1e-15);
    CHECK(parseSpiceNumber("1E3", v) && std::fabs(v - 1000) < 1e-9);
    CHECK(parseSpiceNumber("5mil", v) && std::fabs(v - 127e-6) < 1e-15);
    CHECK(parseSpiceNumber(".5", v) && std::fabs(v - 0.5) < 1e-15);
    CHECK(parseSpiceNumber("1kohm", v) && std::fabs(v - 1000) < 1e-9);
    for (const char* bad : {"", "abc", "0x10", "1..2", "-", "1k5", "inf", "nan", "1e999", "+.", "1%"}) CHECK(!parseSpiceNumber(bad, v));

    auto eval = [](const std::string& text) {
        std::string err;
        SpiceExprPtr e = parseSpiceExpression(text, err);
        return e ? evalSpiceExpression(*e, nullptr, 0) : NAN;
    };
    CHECK_NEAR(eval("2*3+4"), 10, 1e-12);
    CHECK_NEAR(eval("(1+2)**2"), 9, 1e-12);
    CHECK_NEAR(eval("2^3^2"), 512, 1e-9);  // right associative
    CHECK_NEAR(eval("-2^2"), -4, 1e-12);
    CHECK_NEAR(eval("max(1, 5, 3) - min(4, 2)"), 3, 1e-12);
    CHECK_NEAR(eval("if(1 > 0, 5, 6) + (0 ? 1 : 2)"), 7, 1e-12);
    CHECK_NEAR(eval("table(1.5, 1, 10, 2, 20)"), 15, 1e-12);
    CHECK_NEAR(eval("limit(5, 0, 1) + u(-1) + uramp(2) + abs(-3)"), 6, 1e-12);
    CHECK_NEAR(eval("2k * 1m"), 2, 1e-12);
    CHECK_NEAR(eval("1 < 2 && 3 >= 3 || 0"), 1, 1e-12);
    CHECK_NEAR(eval("pwrs(-4, 0.5)"), -2, 1e-12);
    CHECK_NEAR(eval("sqrt(-1) + ln(0) * 0"), 0, 1e-12);  // guarded: no NaN
    for (const char* bad : {"2*(3", "foo(", "1 +", "V(", "V(a,b,c)", "I(a,b)", ")", "1 ? 2", "@"}) {
        std::string err;
        CHECK(!parseSpiceExpression(bad, err) && !err.empty());
    }
    // Nesting is bounded (no stack overflow on hostile input).
    std::string deep(5000, '(');
    std::string err;
    CHECK(!parseSpiceExpression(deep + "1" + std::string(5000, ')'), err) && err.find("deeply") != std::string::npos);
    // POLY coefficient order: 1, x1, x2, x1², x1·x2, x2², x1³, …
    auto terms = spicePolyTerms(2, 7);
    CHECK(terms.size() == 7);
    CHECK(terms[0].empty() && terms[1] == std::vector<int>{0} && terms[2] == std::vector<int>{1});
    CHECK(terms[3] == (std::vector<int>{0, 0}) && terms[4] == (std::vector<int>{0, 1}) && terms[5] == (std::vector<int>{1, 1}));
    CHECK(terms[6] == (std::vector<int>{0, 0, 0}));
}

TEST(spice_library_parsing) {
    const std::string text = R"(* A vendor library
.model DA D(IS=1e-14 N=1.05
+ RS=0.5 CJO=2p ; inline comment
+ XTI=3 FOO=7)
.MODEL DB AKO:DA D(N=2)
.param GAIN=10 HALF={GAIN/2}
.func twice(x) {2*x}
.subckt AMP in out params: G=2
E1 out 0 in 0 {G*twice(1)}
R1 out 0 1k $ hspice comment
.ends AMP
.include other.lib
.tran 1u 1m
.subckt OPEN a b
R1 a b 1
)";
    SpiceLibrary lib = parseSpiceLibrary(text);
    auto entries = spiceLibraryEntries(lib);
    std::set<std::string> names;
    for (const auto& e : entries) names.insert(e.name);
    CHECK(names.count("DA") && names.count("DB") && names.count("AMP") && names.count("OPEN"));
    CHECK(hasDiag(lib.diagnostics, SpiceDiagnostic::Level::Error, "OPEN has no .ends"));
    CHECK(hasDiag(lib.diagnostics, SpiceDiagnostic::Level::Warning, "other.lib"));
    CHECK(hasDiag(lib.diagnostics, SpiceDiagnostic::Level::Info, ".TRAN"));
    SpiceFlatCircuit db = flattenSpiceModel(lib, "db");  // case-insensitive
    CHECK(db.ok && db.prims.size() == 1 && db.type == "D");
    if (db.ok) {
        CHECK_NEAR(db.prims[0].model.at("N"), 2, 1e-12);
        CHECK_NEAR(db.prims[0].model.at("IS"), 1e-14, 1e-26);  // from the AKO base
        CHECK_NEAR(db.prims[0].model.at("CJO"), 2e-12, 1e-24);
    }
    SpiceFlatCircuit da = flattenSpiceModel(lib, "DA");
    CHECK(hasDiag(da.diagnostics, SpiceDiagnostic::Level::Warning, "FOO not modelled"));
    CHECK(hasDiag(da.diagnostics, SpiceDiagnostic::Level::Info, "XTI"));
    SpiceFlatCircuit amp = flattenSpiceModel(lib, "AMP");
    CHECK(amp.ok && amp.ports == (std::vector<std::string>{"IN", "OUT"}));
    if (amp.ok) {
        CHECK(amp.prims[0].type == 'E' && amp.prims[0].coeffs.size() == 2);
        CHECK_NEAR(amp.prims[0].coeffs[1], 4, 1e-12);
    }
    CHECK(!flattenSpiceModel(lib, "OPEN").ok);
    CHECK(!flattenSpiceModel(lib, "NOTHING").ok);
    // Extraction keeps what AMP needs (params, func) and leaves the rest out.
    std::string amptext = extractSpiceModel(lib, "AMP");
    CHECK(amptext.find(".subckt AMP") != std::string::npos && amptext.find(".func") != std::string::npos);
    CHECK(amptext.find("OPEN") == std::string::npos && amptext.find("DA") == std::string::npos);
    CHECK(flattenSpiceModel(parseSpiceLibrary(amptext), "AMP").ok);
    std::string dbtext = extractSpiceModel(lib, "DB");
    CHECK(dbtext.find(".model DA") != std::string::npos);  // the AKO base comes along
    CHECK(flattenSpiceModel(parseSpiceLibrary(dbtext), "DB").ok);
    // Unsupported constructs are errors with line numbers, not silent omissions.
    SpiceLibrary bad = parseSpiceLibrary(".subckt T a b\nT1 a 0 b 0 Z0=50 TD=1n\nE1 b 0 LAPLACE {V(a)} = {1/(1+s)}\n.ends\n");
    SpiceFlatCircuit badFlat = flattenSpiceModel(bad, "T");
    CHECK(!badFlat.ok);
    CHECK(hasDiag(badFlat.diagnostics, SpiceDiagnostic::Level::Error, "transmission line"));
    CHECK(hasDiag(badFlat.diagnostics, SpiceDiagnostic::Level::Error, "LAPLACE"));
    bool lineKnown = false;
    for (const auto& d : badFlat.diagnostics) lineKnown |= d.line == 2;
    CHECK(lineKnown);
    // BSIM MOSFETs and recursive subckts are refused.
    CHECK(!flattenSpiceModel(parseSpiceLibrary(".model M1 NMOS(LEVEL=54)\n"), "M1").ok);
    SpiceFlatCircuit rec = flattenSpiceModel(parseSpiceLibrary(".subckt R a b\nX1 a b R\n.ends\n"), "R");
    CHECK(!rec.ok && hasDiag(rec.diagnostics, SpiceDiagnostic::Level::Error, "recursive"));
}

TEST(spice_controlled_sources_dc) {
    const double load = 1e6;
    auto par = [&](double r) { return r * load / (r + load); };
    auto sub = [](const std::string& body) { return ".subckt T in out\n" + body + "\n.ends\n"; };
    CHECK_NEAR(spiceTwoPort(sub("E1 out 0 in 0 3"), "T", 2), 6, 1e-6);
    CHECK_NEAR(spiceTwoPort(sub("G1 out 0 in 0 1m\nR1 out 0 1k"), "T", 2), -2 * par(1e3) * 1e-3, 1e-6);
    CHECK_NEAR(spiceTwoPort(sub("V1 in a 0\nR1 a 0 1k\nF1 0 out V1 2\nR2 out 0 1k"), "T", 2), 4e-3 * par(1e3), 1e-6);
    CHECK_NEAR(spiceTwoPort(sub("V1 in a 0\nR1 a 0 1k\nH1 out 0 V1 500"), "T", 2), 1.0, 1e-6);
    CHECK_NEAR(spiceTwoPort(sub("E1 out 0 POLY(2) in 0 in 0 1 2 3"), "T", 2), 11, 1e-6);
    CHECK_NEAR(spiceTwoPort(sub("E1 out 0 POLY(2) (in,0) (in,0) 0 0 0 1"), "T", 2), 4, 1e-6);  // x1²
    CHECK_NEAR(spiceTwoPort(sub("E1 out 0 POLY(1) in 0 2.5"), "T", 2), 5, 1e-6);  // one coefficient = gain
    CHECK_NEAR(spiceTwoPort(sub("B1 out 0 V=V(in)*V(in) + limit(V(in), 0, 1)"), "T", 2), 5, 1e-6);
    CHECK_NEAR(spiceTwoPort(sub("E1 out 0 TABLE {V(in)} = (0,0) (1,10) (3,20)"), "T", 2), 15, 1e-6);
    CHECK_NEAR(spiceTwoPort(sub("G1 0 out VALUE = {V(in)*1m}\nR1 out 0 1k"), "T", 2), 2e-3 * par(1e3), 1e-6);
    CHECK_NEAR(spiceTwoPort(sub("I1 0 out 1m\nR1 out 0 1k"), "T", 2), 1e-3 * par(1e3), 1e-6);  // n+ → source → n−
    CHECK_NEAR(spiceTwoPort(sub("B1 0 out I=V(in)*1m\nR1 out 0 1k"), "T", 2), 2e-3 * par(1e3), 1e-6);
    // A voltage-controlled switch, on and off.
    const std::string sw = sub(".model SWM SW(VT=1 VH=0 RON=1 ROFF=1MEG)\nS1 out x in 0 SWM\nV2 x 0 5\nR1 out 0 1k");
    CHECK_NEAR(spiceTwoPort(sw, "T", 2), 5 * par(1e3) / (par(1e3) + 1), 1e-3);
    CHECK(spiceTwoPort(sw, "T", 0) < 0.01);
    // Nested subcircuits with parameters (default, instance override, expression of a global .param).
    const std::string nested = ".param K0=1.5\n.subckt GAIN in out PARAMS: K={2*K0}\nE1 out 0 in 0 {K}\n.ends\n"
                               ".subckt TOP in out\nX1 in mid GAIN K=2\nX2 mid out GAIN\n.ends\n";
    CHECK_NEAR(spiceTwoPort(nested, "TOP", 1), 6, 1e-6);
    // .func, I() of a source in a behavioural expression, TIME is 0 at DC.
    CHECK_NEAR(spiceTwoPort(".func sq(x) {x*x}\n" + sub("V1 in a 0\nR1 a 0 1k\nB1 out 0 V={sq(V(in))} + I(V1)*1k + time"), "T", 3),
               12, 1e-6);
    // The pin map: ports swapped, a rail, a net by name, unknown pins.
    CHECK_NEAR(spiceTwoPort(sub("E1 out 0 in 0 1"), "T", 2, "1 2"), 2, 1e-6);
    CHECK_NEAR(spiceTwoPort(sub("E1 out 0 in 0 1"), "T", 2, "dc:7 2"), 7, 1e-6);
    std::string error;
    CHECK(std::isnan(spiceTwoPort(sub("E1 out 0 in 0 1"), "T", 2, "1 9", &error)) && error.find("no pin '9'") != std::string::npos);
    CHECK(std::isnan(spiceTwoPort(sub("E1 out 0 in 0 1"), "T", 2, "1", &error)) && error.find("2 ports") != std::string::npos);
    CHECK(std::isnan(spiceTwoPort(sub("E1 out 0 in 0 1"), "T", 2, "net:NOPE 2", &error)) && error.find("NOPE") != std::string::npos);
    CHECK(std::isnan(spiceTwoPort(sub("E1 out 0 in 0 1"), "MISSING", 2, "1 2", &error)) && error.find("MISSING") != std::string::npos);
    // A part without a model is simulated as before.
    Schematic s;
    int v = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    int d = s.addComponent(ComponentKind::Diode, "1N4148", {100, 0});
    int r = s.addComponent(ComponentKind::Resistor, "1k", {50, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    wire(s, v, "+", r, "1");
    wire(s, r, "2", d, "A");
    wire(s, d, "K", g, "GND");
    wire(s, v, "-", g, "GND");
    const double builtin = netV(s, Simulator(s).dcOperatingPoint(), d, "A");
    s.setSpiceModel(d, SpiceModelRef{".model DX D(IS=2.52n N=1.752)", "DX", ""});
    DcResult withModel = Simulator(s).dcOperatingPoint();
    CHECK(withModel.converged);
    // Same Shockley parameters; only the thermal voltage differs (300 K built-in, 27 °C SPICE).
    CHECK_NEAR(netV(s, withModel, d, "A"), builtin, 2e-3);
    CHECK_NEAR(reading(withModel, d)->current, (5 - netV(s, withModel, d, "A")) / 1e3, 1e-9);
    s.setSpiceModel(d, SpiceModelRef{});
    CHECK(netV(s, Simulator(s).dcOperatingPoint(), d, "A") == builtin);
}

TEST(spice_semiconductor_models_dc) {
    // Diode series resistance: 10 mA through RS = 10 Ω adds exactly 0.1 V.
    auto diodeV = [](const std::string& model) {
        Schematic s;
        int i = s.addComponent(ComponentKind::CurrentSource, "10m", {0, 0});
        int d = s.addComponent(ComponentKind::Diode, "", {100, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        wire(s, i, "+", d, "A");
        wire(s, d, "K", g, "GND");
        wire(s, i, "-", g, "GND");
        s.setSpiceModel(d, SpiceModelRef{model, "DX", ""});
        DcResult dc = Simulator(s).dcOperatingPoint();
        return dc.converged ? netV(s, dc, d, "A") : NAN;
    };
    const double v0 = diodeV(".model DX D(IS=1e-14 N=1)");
    CHECK_NEAR(v0, spicedevVt() * std::log(1e-2 / 1e-14 + 1), 1e-6);
    CHECK_NEAR(diodeV(".model DX D(IS=1e-14 N=1 RS=10)") - v0, 0.1, 1e-6);
    // Zener: reverse biased from 10 V through 1 kΩ it holds ≈ BV.
    {
        Schematic s;
        int v = s.addComponent(ComponentKind::VoltageSource, "10", {0, 0});
        int r = s.addComponent(ComponentKind::Resistor, "1k", {50, 0});
        int d = s.addComponent(ComponentKind::Diode, "", {100, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        wire(s, v, "+", r, "1");
        wire(s, r, "2", d, "K");
        wire(s, d, "A", g, "GND");
        wire(s, v, "-", g, "GND");
        s.setSpiceModel(d, SpiceModelRef{".model DZ D(IS=1f BV=5.1 IBV=5m NBV=1)", "DZ", ""});
        DcResult dc = Simulator(s).dcOperatingPoint();
        CHECK(dc.converged);
        const double vz = netV(s, dc, d, "K");
        CHECK_NEAR(vz, 5.1 + spicedevVt() * std::log((10 - vz) / 1e3 / 5e-3), 1e-3);
    }
    // NPN: forced base current, Ic = BF·Ib (no Early effect, no high injection).
    {
        Schematic s;
        int vcc = s.addComponent(ComponentKind::VoltageSource, "10", {0, 0});
        int ib = s.addComponent(ComponentKind::CurrentSource, "10u", {100, 0});
        int rc = s.addComponent(ComponentKind::Resistor, "1k", {200, -80});
        int q = s.addComponent(ComponentKind::NPN, "", {200, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        wire(s, ib, "+", q, "B");
        wire(s, ib, "-", g, "GND");
        wire(s, vcc, "+", rc, "1");
        wire(s, rc, "2", q, "C");
        wire(s, q, "E", g, "GND");
        wire(s, vcc, "-", g, "GND");
        s.setSpiceModel(q, SpiceModelRef{".model QN NPN(IS=1e-15 BF=100 BR=1)", "QN", ""});
        DcResult dc = Simulator(s).dcOperatingPoint();
        CHECK(dc.converged);
        // Ic = BF·Ib minus the tiny reverse term; the reading is the collector current and V_CE.
        CHECK_NEAR(reading(dc, q)->current, 1e-3, 1e-7);
        CHECK_NEAR(reading(dc, q)->voltage, 9.0, 1e-4);
        // Early effect: VAF = 50 V raises Ic by (1 + V_CB / VAF).
        s.setSpiceModel(q, SpiceModelRef{".model QN NPN(IS=1e-15 BF=100 VAF=50)", "QN", ""});
        DcResult early = Simulator(s).dcOperatingPoint();
        CHECK(early.converged && reading(early, q)->current > 1.05e-3);
        // Series resistances and the 2N3904 library model converge too.
        s.setSpiceModel(q, SpiceModelRef{builtinSpiceModels()[4].text, "2N3904", ""});
        DcResult lib = Simulator(s).dcOperatingPoint();
        CHECK(lib.converged && reading(lib, q)->current > 1e-3 && reading(lib, q)->current < 4.2e-3);
    }
    // PNP on a catalog part (pins 1 B, 2 E, 3 C): mapped by pin name.
    {
        auto id = CustomPartRegistry::instance().registerPart(findStandardPart("MMBT3906")->spec)->id;
        Schematic s;
        int v = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
        int q = s.addCustomComponent(id, "", {100, 0});
        int rb = s.addComponent(ComponentKind::Resistor, "100k", {150, 0});
        int rc = s.addComponent(ComponentKind::Resistor, "1k", {200, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        wire(s, v, "+", q, "E");
        wire(s, q, "B", rb, "1");
        wire(s, rb, "2", g, "GND");
        wire(s, q, "C", rc, "1");
        wire(s, rc, "2", g, "GND");
        wire(s, v, "-", g, "GND");
        SpiceFlatCircuit flat = flattenSpiceModel(parseSpiceLibrary(builtinSpiceModels()[5].text), "2N3906");
        std::vector<std::pair<std::string, std::string>> pins = {{"1", "B"}, {"2", "E"}, {"3", "C"}};
        CHECK(defaultSpicePinMap(flat, pins, false) == "3 1 2");
        s.setSpiceModel(q, SpiceModelRef{builtinSpiceModels()[5].text, "2N3906", ""});
        DcResult dc = Simulator(s).dcOperatingPoint();
        CHECK(dc.converged);
        const double vc = netV(s, dc, q, "C");
        CHECK(vc > 3.0 && vc < 5.0);  // saturated: β·I_B ≈ 7 mA would need more than 5 V across 1 kΩ
        CHECK(netV(s, dc, q, "B") > 4.0 && netV(s, dc, q, "B") < 4.5);
        CHECK_NEAR(reading(dc, q)->current, -vc / 1e3, 1e-6);  // current into C is negative for a PNP
    }
    // MOSFET level 1 in saturation: Id = KP/2·(W/L)·(Vgs − Vto)²·(1 + λ·Vds); level 3 divides by 1 + θ·(Vgs − Vto).
    auto mosId = [](const std::string& model, double vgs, double vds) {
        Schematic s;
        int vg = s.addComponent(ComponentKind::VoltageSource, formatEngineeringValue(vgs, "", 9), {0, 0});
        int vd = s.addComponent(ComponentKind::VoltageSource, formatEngineeringValue(vds, "", 9), {0, 100});
        int m = s.addComponent(ComponentKind::NMOS, "", {100, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        wire(s, vg, "+", m, "G");
        wire(s, vd, "+", m, "D");
        wire(s, m, "S", g, "GND");
        wire(s, vg, "-", g, "GND");
        wire(s, vd, "-", g, "GND");
        s.setSpiceModel(m, SpiceModelRef{model, "MN", ""});
        DcResult dc = Simulator(s).dcOperatingPoint();
        return dc.converged ? reading(dc, m)->current : NAN;
    };
    CHECK_NEAR(mosId(".model MN NMOS(VTO=1 KP=2m)", 3, 5), 1e-3 * 4, 1e-8);
    CHECK_NEAR(mosId(".model MN NMOS(VTO=1 KP=2m LAMBDA=0.02)", 3, 5), 4e-3 * 1.1, 1e-8);
    CHECK_NEAR(mosId(".model MN NMOS(VTO=1 KP=2m)", 3, 0.5), 2e-3 * (2 - 0.25) * 0.5, 1e-8);  // triode
    CHECK_NEAR(mosId(".model MN NMOS(LEVEL=3 VTO=1 KP=2m THETA=0.5)", 3, 5), 4e-3 / 2, 1e-8);
    CHECK_NEAR(mosId(".model MN NMOS(VTO=1 KP=2m)", 0.5, 5), 0, 1e-9);
    // P-MOSFET subcircuit (body diode) on a catalog part with pins G S D: conducts with the gate low.
    {
        auto id = CustomPartRegistry::instance().registerPart(findStandardPart("BSS84")->spec)->id;
        Schematic s;
        int v = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
        int m = s.addCustomComponent(id, "", {100, 0});
        int rd = s.addComponent(ComponentKind::Resistor, "1k", {200, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        wire(s, v, "+", m, "S");
        wire(s, m, "G", g, "GND");
        wire(s, m, "D", rd, "1");
        wire(s, rd, "2", g, "GND");
        wire(s, v, "-", g, "GND");
        s.setSpiceModel(m, SpiceModelRef{builtinSpiceModels()[8].text, "BSS84", ""});
        DcResult dc = Simulator(s).dcOperatingPoint();
        CHECK(dc.converged && netV(s, dc, m, "D") > 4.0);
    }
}

TEST(spice_opamp_macromodel) {
    // The µA741 Boyle macromodel (E/F/G/H with POLY, BJT input pair, clamp diodes) on the op-amp symbol: rails ±15 V.
    const std::string ua741 = builtinSpiceModels()[9].text;
    auto amp = [&](const std::string& vin, const char* rfv, const char* rgv) {
        Schematic s;
        int v = s.addComponent(ComponentKind::VoltageSource, vin, {0, 0});
        int u = s.addComponent(ComponentKind::OpAmp, "UA741", {100, 0});
        int rf = s.addComponent(ComponentKind::Resistor, rfv, {150, -60});
        int rg = s.addComponent(ComponentKind::Resistor, rgv, {50, -60});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        wire(s, v, "+", u, "IN+");
        wire(s, v, "-", g, "GND");
        wire(s, u, "OUT", rf, "2");
        wire(s, rf, "1", u, "IN-");
        wire(s, rg, "2", u, "IN-");
        wire(s, rg, "1", g, "GND");
        s.setSpiceModel(u, SpiceModelRef{ua741, "UA741", ""});
        return std::make_pair(s, u);
    };
    {
        auto [s, u] = amp("1", "10k", "10k");
        DcResult dc = Simulator(s).dcOperatingPoint();
        CHECK(dc.converged);
        CHECK_NEAR(netV(s, dc, u, "OUT"), 2.0, 2e-3);
        CHECK(reading(dc, u) && std::fabs(reading(dc, u)->voltage - 2.0) < 2e-3);
    }
    {
        // Gain 100: the closed-loop pole sits at GBW / 100 (≈ 10 kHz for 1 MHz).
        auto [s, u] = amp("0 AC 1", "99k", "1k");
        AcOptions o;
        o.fStart = 10;
        o.fStop = 10e6;
        AcResult r = Simulator(s).ac(o);
        CHECK(r.ok);
        if (r.ok) {
            const AcMetrics& m = r.metrics[s.netOf({u, 2})];
            CHECK_NEAR(m.lowFreqDb, 40.0, 0.05);
            CHECK(m.f3dbHz > 6e3 && m.f3dbHz < 14e3);
        }
    }
    {
        // Follower slewing a 5 V step: ≈ 0.5 V/µs.
        auto [s, u] = amp("PULSE(0 5 100u)", "1", "1G");
        TransientResult tr = Simulator(s).transient(30e-6, 20e-9);
        CHECK(tr.ok);
        if (tr.ok) {
            const auto& out = tr.netVoltages[static_cast<size_t>(s.netOf({u, 2}))];
            double t1 = NAN, t9 = NAN;
            for (size_t i = 0; i < out.size(); ++i) {
                if (std::isnan(t1) && out[i] > 1.0) t1 = tr.time[i];
                if (std::isnan(t9) && out[i] > 4.0) t9 = tr.time[i];
            }
            const double slew = 3.0 / (t9 - t1);
            CHECK(slew > 0.3e6 && slew < 0.8e6);
            CHECK_NEAR(out.back(), 5.0, 0.01);
        }
    }
}

TEST(spice_models_hostile_input) {
    // Random bytes, mutated real models and pathological structures: never a crash or a hang, always diagnostics.
    std::vector<std::string> seeds;
    for (const auto& m : builtinSpiceModels()) seeds.push_back(m.text);
    seeds.push_back(".subckt T in out\nB1 out 0 V=V(in)*2 + if(V(in)>1, 1, 0)\nE2 x 0 TABLE {V(in)} = (0,0) (1,1)\n.ends\n");
    uint64_t state = 12345;
    auto rnd = [&]() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<uint32_t>(state >> 33);
    };
    int flattened = 0, simulated = 0;
    for (int round = 0; round < 600; ++round) {
        std::string t = seeds[rnd() % seeds.size()];
        const int edits = 1 + static_cast<int>(rnd() % 6);
        for (int k = 0; k < edits && !t.empty(); ++k) {
            size_t at = rnd() % t.size();
            switch (rnd() % 6) {
                case 0: t.erase(at, 1 + rnd() % 8); break;
                case 1: t.insert(at, 1, "(){}=,;+*'\"\n\t.$-0"[rnd() % 18]); break;
                case 2: t[at] = static_cast<char>(rnd() % 256); break;
                case 3: t.insert(at, t.substr(rnd() % t.size(), rnd() % 40)); break;
                case 4: t.insert(at, "\n+ "); break;
                default: t.insert(at, "{" + std::string(rnd() % 50, '(')); break;
            }
        }
        if (round % 50 == 0) {  // pure noise
            t.clear();
            for (int k = 0; k < 400; ++k) t += static_cast<char>(rnd() % 256);
        }
        SpiceLibrary lib = parseSpiceLibrary(t);
        for (const auto& e : spiceLibraryEntries(lib)) {
            SpiceFlatCircuit f = flattenSpiceModel(lib, e.name);
            if (!f.ok) {
                CHECK(!f.diagnostics.empty());
                continue;
            }
            ++flattened;
            // Simulate it on an op-amp / IC8 with a default or positional map: must return, converged or not.
            Schematic s;
            int v = s.addComponent(ComponentKind::VoltageSource, "1", {0, 0});
            int u = s.addComponent(ComponentKind::IC8, "IC", {100, 0});
            int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
            wire(s, v, "+", u, "1");
            wire(s, v, "-", g, "GND");
            std::string pins;
            for (size_t p = 0; p < f.ports.size(); ++p) pins += std::to_string(p % 8 + 1) + " ";
            s.setSpiceModel(u, SpiceModelRef{extractSpiceModel(lib, e.name), e.name, pins});
            DcResult dc = Simulator(s).dcOperatingPoint();
            simulated += dc.converged;
            if (dc.converged)
                for (double x : dc.netVoltages) CHECK(std::isfinite(x));
        }
    }
    CHECK(flattened > 50);
    CHECK(simulated > 20);
    // Structural extremes.
    CHECK(!parseSpiceLibrary(std::string(9u << 20, '*')).diagnostics.empty());  // over 8 MB: refused
    std::string longLine = ".model D1 D(IS=1n";
    for (int i = 0; i < 20000; ++i) longLine += "\n+ N=1";
    CHECK(flattenSpiceModel(parseSpiceLibrary(longLine + ")\n"), "D1").ok);
    std::string chain;
    for (int i = 0; i < 60; ++i)
        chain += ".subckt S" + std::to_string(i) + " a b\nX1 a b S" + std::to_string(i + 1) + "\n.ends\n";
    chain += ".subckt S60 a b\nR1 a b 1k\n.ends\n";
    SpiceFlatCircuit deep = flattenSpiceModel(parseSpiceLibrary(chain), "S0");
    CHECK(!deep.ok && hasDiag(deep.diagnostics, SpiceDiagnostic::Level::Error, "nested more than 40"));
    std::string wide = ".subckt W a b\n";
    for (int i = 0; i < 12; ++i) wide += "X" + std::to_string(i) + " a b W" + std::to_string(i) + "\n";
    wide += ".ends\n";
    for (int i = 0; i < 12; ++i) {  // 10^6 resistors if fully expanded: refused at 100 000
        wide += ".subckt W" + std::to_string(i) + " a b\n";
        for (int k = 0; k < 10; ++k) wide += "X" + std::to_string(k) + " a b W" + std::to_string(i + 1) + "\n";
        wide += ".ends\n";
    }
    wide += ".subckt W12 a b\nR1 a b 1\n.ends\n";
    SpiceFlatCircuit huge = flattenSpiceModel(parseSpiceLibrary(wide), "W");
    CHECK(!huge.ok && hasDiag(huge.diagnostics, SpiceDiagnostic::Level::Error, "100 000"));
    const int rc = sieda_c_api_spice_test();
    if (rc) std::printf("    SPICE C API test failed at step %d\n", rc);
    CHECK(rc == 0);
}

TEST(spice_device_capacitances_ac) {
    const double vt = spicedevVt();
    // Reverse-biased junction behind 1 kΩ: corner at 1 / (2π·R·Cj(V)), Cj = CJO / (1 + V/VJ)^M.
    {
        Schematic s;
        int v = s.addComponent(ComponentKind::VoltageSource, "5 AC 1", {0, 0});
        int r = s.addComponent(ComponentKind::Resistor, "1k", {50, 0});
        int d = s.addComponent(ComponentKind::Diode, "", {100, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        wire(s, v, "+", r, "1");
        wire(s, r, "2", d, "K");
        wire(s, d, "A", g, "GND");
        wire(s, v, "-", g, "GND");
        s.setSpiceModel(d, SpiceModelRef{".model DC D(IS=1e-15 CJO=100p VJ=0.7 M=0.5)", "DC", ""});
        AcOptions o;
        o.fStart = 1e4;
        o.fStop = 1e9;
        AcResult r1 = Simulator(s).ac(o);
        CHECK(r1.ok);
        if (r1.ok) {
            const double cj = 100e-12 / std::sqrt(1 + 5 / 0.7);
            CHECK_NEAR(r1.metrics[s.netOf({d, 1})].f3dbHz * 2 * kPi * 1e3 * cj, 1.0, 2e-3);
        }
        // Without capacitances the built-in diode is flat.
        s.setSpiceModel(d, SpiceModelRef{});
        AcResult r0 = Simulator(s).ac(o);
        CHECK(r0.ok && std::isnan(r0.metrics[s.netOf({d, 1})].f3dbHz));
    }
    // Forward diode fed by a current: r_d ∥ C_d with C_d = TT·g_d, so the corner is 1 / (2π·TT).
    {
        Schematic s;
        int i = s.addComponent(ComponentKind::CurrentSource, "1m AC 1u", {0, 0});
        int d = s.addComponent(ComponentKind::Diode, "", {100, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        wire(s, i, "+", d, "A");
        wire(s, d, "K", g, "GND");
        wire(s, i, "-", g, "GND");
        s.setSpiceModel(d, SpiceModelRef{".model DT D(IS=1e-14 TT=1n)", "DT", ""});
        AcOptions o;
        o.fStart = 1e5;
        o.fStop = 1e10;
        AcResult r = Simulator(s).ac(o);
        CHECK(r.ok);
        if (r.ok) {
            const AcMetrics& m = r.metrics[s.netOf({d, 0})];
            CHECK_NEAR(m.f3dbHz * 2 * kPi * 1e-9, 1.0, 1e-3);
            CHECK_NEAR(std::pow(10.0, m.lowFreqDb / 20) / 1e-6, vt / (1e-3 + 1e-14), 1e-3 * vt / 1e-3);
        }
    }
    // BJT transit time: with a 1 Ω collector load the current gain falls to unity at f_T ≈ 1 / (2π·TF).
    {
        Schematic s;
        int vcc = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
        int ib = s.addComponent(ComponentKind::CurrentSource, "10u AC 1", {100, 0});
        int rc = s.addComponent(ComponentKind::Resistor, "1", {200, -80});
        int q = s.addComponent(ComponentKind::NPN, "", {200, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        wire(s, ib, "+", q, "B");
        wire(s, ib, "-", g, "GND");
        wire(s, vcc, "+", rc, "1");
        wire(s, rc, "2", q, "C");
        wire(s, q, "E", g, "GND");
        wire(s, vcc, "-", g, "GND");
        s.setSpiceModel(q, SpiceModelRef{".model QT NPN(IS=1e-15 BF=200 TF=1n)", "QT", ""});
        AcOptions o;
        o.fStart = 1e3;
        o.fStop = 1e10;
        AcResult r = Simulator(s).ac(o);
        CHECK(r.ok);
        if (r.ok) {
            const AcMetrics& m = r.metrics[s.netOf({q, 1})];
            CHECK_NEAR(m.lowFreqDb, 20 * std::log10(200.0), 0.05);
            CHECK_NEAR(m.unityHz * 2 * kPi * 1e-9, 1.0, 0.01);  // β / (1 + jωβ·TF): unity at ≈ 1/(2π·TF)
        }
    }
    // MOSFET overlap capacitances: gate through 10 kΩ, drain and source at AC ground: pole at 1/(2π·R·(Cgs + Cgd)).
    {
        Schematic s;
        int v = s.addComponent(ComponentKind::VoltageSource, "3 AC 1", {0, 0});
        int vd = s.addComponent(ComponentKind::VoltageSource, "5", {0, 100});
        int r = s.addComponent(ComponentKind::Resistor, "10k", {50, 0});
        int m = s.addComponent(ComponentKind::NMOS, "", {100, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        wire(s, v, "+", r, "1");
        wire(s, r, "2", m, "G");
        wire(s, vd, "+", m, "D");
        wire(s, m, "S", g, "GND");
        wire(s, v, "-", g, "GND");
        wire(s, vd, "-", g, "GND");
        s.setSpiceModel(m, SpiceModelRef{".model MC NMOS(VTO=1 KP=1m CGSO=200p CGDO=50p W=1 L=1)", "MC", ""});
        AcOptions o;
        o.fStart = 1e3;
        o.fStop = 1e8;
        AcResult res = Simulator(s).ac(o);
        CHECK(res.ok);
        if (res.ok) CHECK_NEAR(res.metrics[s.netOf({m, 0})].f3dbHz * 2 * kPi * 1e4 * 250e-12, 1.0, 1e-3);
    }
}

TEST(spice_device_capacitances_transient) {
    // A junction with M = 0 is a linear capacitor CJO: its RC step response matches a real capacitor sample by sample,
    // with backward Euler and with the trapezoidal rule alike.
    auto run = [](bool diode) {
        Schematic s;
        int v = s.addComponent(ComponentKind::VoltageSource, "PULSE(0 -5 1)", {0, 0});
        int r = s.addComponent(ComponentKind::Resistor, "1k", {50, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        int part;
        if (diode) {
            part = s.addComponent(ComponentKind::Diode, "", {100, 0});
            s.setSpiceModel(part, SpiceModelRef{".model DL D(IS=1e-20 CJO=1n M=0)", "DL", ""});
            wire(s, r, "2", part, "A");
            wire(s, part, "K", g, "GND");
        } else {
            part = s.addComponent(ComponentKind::Capacitor, "1n", {100, 0});
            wire(s, r, "2", part, "1");
            wire(s, part, "2", g, "GND");
        }
        wire(s, v, "+", r, "1");
        wire(s, v, "-", g, "GND");
        TransientResult tr = Simulator(s).transient(5e-6, 10e-9);
        return std::make_pair(tr, s.netOf({part, 0}));
    };
    auto [cap, capNet] = run(false);
    auto [dio, dioNet] = run(true);
    CHECK(cap.ok && dio.ok);
    if (cap.ok && dio.ok) {
        double worst = 0;
        for (size_t i = 0; i < cap.time.size(); ++i)
            worst = std::max(worst, std::fabs(cap.netVoltages[static_cast<size_t>(capNet)][i] - dio.netVoltages[static_cast<size_t>(dioNet)][i]));
        CHECK(worst < 1e-6);
        CHECK_NEAR(dio.netVoltages[static_cast<size_t>(dioNet)][100], -5 * (1 - std::exp(-1.0)), 0.02);  // τ = 1 µs
    }
    // Reverse recovery: a diode with TT conducts backwards for a while after the drive reverses; without TT it does not.
    auto recovery = [](const char* model) {
        Schematic s;
        int v = s.addComponent(ComponentKind::VoltageSource, "PULSE(5 -5 2u 0.5)", {0, 0});
        int r = s.addComponent(ComponentKind::Resistor, "100", {50, 0});
        int d = s.addComponent(ComponentKind::Diode, "", {100, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        wire(s, v, "+", r, "1");
        wire(s, r, "2", d, "A");
        wire(s, d, "K", g, "GND");
        wire(s, v, "-", g, "GND");
        s.setSpiceModel(d, SpiceModelRef{model, "DR", ""});
        TransientResult tr = Simulator(s).transient(2e-6, 2e-9);
        double reverse = 0;  // charge carried backwards (A·s)
        if (tr.ok)
            for (size_t i = 1; i < tr.time.size(); ++i)
                if (tr.currents[d][i] < 0) reverse += -tr.currents[d][i] * (tr.time[i] - tr.time[i - 1]);
        return tr.ok ? reverse : NAN;
    };
    const double withTT = recovery(".model DR D(IS=1e-14 TT=100n)");
    const double without = recovery(".model DR D(IS=1e-14)");
    // Stored charge ≈ TT·I_F = 100 ns · 43 mA ≈ 4.3 nC comes back out.
    CHECK(withTT > 2e-9 && withTT < 6e-9);
    CHECK(without < 1e-11);
}

TEST(opamp_multipole_macromodel) {
    // Open loop with GBW 1 MHz and a second pole at 2 MHz: |A(fu)| = 1 where fu²·(1 + fu²/P2²) = GBW², phase margin
    // 90° − atan(fu / P2).
    auto openLoop = [](const std::string& value) {
        Schematic s;
        int vs = s.addComponent(ComponentKind::VoltageSource, "0 AC 1", {0, 0});
        int a = s.addComponent(ComponentKind::OpAmp, value, {100, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        int rl = s.addComponent(ComponentKind::Resistor, "10k", {200, 0});
        wire(s, vs, "+", a, "IN+");
        wire(s, vs, "-", g, "GND");
        wire(s, a, "IN-", g, "GND");
        wire(s, a, "OUT", rl, "1");
        wire(s, rl, "2", g, "GND");
        AcOptions o;
        o.fStart = 0.01;
        o.fStop = 100e6;
        AcResult r = Simulator(s).ac(o);
        return std::make_pair(r, r.ok ? r.metrics[s.netOf({a, 2})] : AcMetrics{});
    };
    {
        auto [r, m] = openLoop("generic GBW=1MEG P2=2MEG");
        CHECK(r.ok);
        const double gbw = 1e6, p2 = 2e6;
        const double fu = std::sqrt((-1 + std::sqrt(1 + 4 * gbw * gbw / (p2 * p2))) / 2) * p2;
        CHECK_NEAR(m.lowFreqDb, 120.0, 0.01);
        CHECK_NEAR(m.unityHz / fu, 1.0, 1e-3);
        CHECK_NEAR(m.phaseMarginDeg, 90.0 - std::atan(fu / p2) * 180 / kPi, 0.1);
    }
    {
        // GBW alone keeps the established single-pole model; AOL changes the DC gain of the macromodel.
        auto [r, m] = openLoop("generic GBW=10MEG AOL=100dB P2=1G");
        CHECK(r.ok);
        CHECK_NEAR(m.lowFreqDb, 100.0, 0.01);
        CHECK_NEAR(m.unityHz / 10e6, 1.0, 0.01);
    }
    // Slew rate and output limits in a follower stepping 0 → 5 V.
    auto follower = [](const std::string& value, double stop) {
        Schematic s;
        int v = s.addComponent(ComponentKind::VoltageSource, "PULSE(0 5 1)", {0, 0});
        int a = s.addComponent(ComponentKind::OpAmp, value, {100, 0});
        int rl = s.addComponent(ComponentKind::Resistor, "10k", {200, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        wire(s, v, "+", a, "IN+");
        wire(s, v, "-", g, "GND");
        wire(s, a, "OUT", a, "IN-");
        wire(s, a, "OUT", rl, "1");
        wire(s, rl, "2", g, "GND");
        TransientResult tr = Simulator(s).transient(stop, stop / 2000);
        return std::make_pair(tr, s.netOf({a, 2}));
    };
    {
        auto [tr, out] = follower("generic GBW=10MEG SR=1V/us", 10e-6);
        CHECK(tr.ok);
        if (tr.ok) {
            const auto& w = tr.netVoltages[static_cast<size_t>(out)];
            double t1 = NAN, t4 = NAN;
            for (size_t i = 0; i < w.size(); ++i) {
                if (std::isnan(t1) && w[i] > 1.0) t1 = tr.time[i];
                if (std::isnan(t4) && w[i] > 4.0) t4 = tr.time[i];
            }
            CHECK_NEAR(3.0 / (t4 - t1) / 1e6, 1.0, 0.03);  // 1 V/µs
            CHECK_NEAR(w.back(), 5.0, 1e-3);
        }
    }
    {
        auto [tr, out] = follower("generic SR=1V/us VOH=3.3 VOL=0", 20e-6);
        CHECK(tr.ok);
        if (tr.ok) CHECK_NEAR(tr.netVoltages[static_cast<size_t>(out)].back(), 3.3, 0.02);  // clipped at VOH
    }
    {
        // Saturated at VOH = 4 V behind ROUT = 100 Ω into 100 Ω: 2 V; the reading is the delivered current.
        Schematic s;
        int v = s.addComponent(ComponentKind::VoltageSource, "1", {0, 0});
        int a = s.addComponent(ComponentKind::OpAmp, "generic VOH=4 VOL=-4 ROUT=100", {100, 0});
        int rl = s.addComponent(ComponentKind::Resistor, "100", {200, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        wire(s, v, "+", a, "IN+");
        wire(s, v, "-", g, "GND");
        wire(s, a, "IN-", g, "GND");
        wire(s, a, "OUT", rl, "1");
        wire(s, rl, "2", g, "GND");
        DcResult dc = Simulator(s).dcOperatingPoint();
        CHECK(dc.converged);
        CHECK_NEAR(netV(s, dc, a, "OUT"), 2.0, 0.01);
        CHECK(reading(dc, a) && std::fabs(reading(dc, a)->current - 0.02) < 1e-4);
        CHECK(reading(dc, a) && std::fabs(reading(dc, a)->voltage - 2.0) < 0.01);
        s.setValue(a, "generic VOH=1 VOL=2");
        std::string error = Simulator(s).dcOperatingPoint().error;
        CHECK(error.find("VOH must be above VOL") != std::string::npos);
    }
}

extern "C" int sieda_c_api_noise_test(void);

TEST(noise_analysis_closed_form) {
    const double kT = 1.380649e-23 * 300.15, q = 1.602176634e-19;
    // Divider of two 1 kΩ resistors: 4kT·(R1 ∥ R2) at the tap, ×2 referred to the input.
    {
        Schematic s;
        int v = s.addComponent(ComponentKind::VoltageSource, "0 AC 1", {0, 0});
        int r1 = s.addComponent(ComponentKind::Resistor, "1k", {50, 0});
        int r2 = s.addComponent(ComponentKind::Resistor, "1k", {100, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        wire(s, v, "+", r1, "1");
        wire(s, r1, "2", r2, "1");
        wire(s, r2, "2", g, "GND");
        wire(s, v, "-", g, "GND");
        NoiseOptions o;
        o.outputNet = s.netOf({r1, 1});
        o.fStart = 10;
        o.fStop = 1e4;
        NoiseResult r = noiseAnalysis(s, o);
        CHECK(r.ok);
        if (r.ok) {
            CHECK_NEAR(r.outputDensity[5] / std::sqrt(4 * kT * 500), 1.0, 1e-9);
            CHECK(r.inputSource == v && !r.inputIsCurrent);
            CHECK_NEAR(r.gain[5], 0.5, 1e-8);
            CHECK_NEAR(r.inputDensity[5] / (2 * std::sqrt(4 * kT * 500)), 1.0, 1e-9);
            CHECK_NEAR(r.outputRms / std::sqrt(4 * kT * 500 * (1e4 - 10)), 1.0, 1e-9);  // white: exact
            CHECK(r.contributions.size() == 2 && r.contributions[0].kind == "thermal");
            CHECK_NEAR(r.contributions[0].rms, std::sqrt(4 * kT * 250 * (1e4 - 10)), 1e-15);
        }
        o.temperature = 2 * 300.15;  // noise power scales with T
        NoiseResult hot = noiseAnalysis(s, o);
        CHECK(hot.ok && std::fabs(hot.outputDensity[0] / r.outputDensity[0] - std::sqrt(2.0)) < 1e-9);
    }
    // RC low-pass: integrated output noise → kT/C over the band (here the arctangent of the sweep limits).
    {
        Schematic s;
        rcLowPass(s, "0 AC 1", "1k", "1n");
        int c = -1;
        for (const auto& comp : s.components())
            if (comp.kind == ComponentKind::Capacitor) c = comp.id;
        NoiseOptions o;
        o.outputNet = s.netOf({c, 0});
        o.fStart = 1;
        o.fStop = 1e10;
        o.pointsPerDecade = 200;
        NoiseResult r = noiseAnalysis(s, o);
        CHECK(r.ok);
        const double fc = 1 / (2 * kPi * 1e3 * 1e-9);
        const double expected = std::sqrt(kT / 1e-9 * 2 / kPi * (std::atan(1e10 / fc) - std::atan(1 / fc)));
        if (r.ok) CHECK_NEAR(r.outputRms / expected, 1.0, 2e-3);
    }
    // Shot noise of a forward diode fed by a current: √(2qI)·r_d.
    {
        Schematic s;
        int i = s.addComponent(ComponentKind::CurrentSource, "1m AC 1", {0, 0});
        int d = s.addComponent(ComponentKind::Diode, "1N4148", {100, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        wire(s, i, "+", d, "A");
        wire(s, d, "K", g, "GND");
        wire(s, i, "-", g, "GND");
        NoiseOptions o;
        o.outputNet = s.netOf({d, 0});
        NoiseResult r = noiseAnalysis(s, o);
        CHECK(r.ok);
        const double rd = 1.752 * 0.025852 / (1e-3 + 2.52e-9);
        if (r.ok) {
            CHECK_NEAR(r.outputDensity[0] / (std::sqrt(2 * q * 1e-3) * rd), 1.0, 1e-3);
            CHECK(r.inputIsCurrent);
            CHECK_NEAR(r.inputDensity[0] / std::sqrt(2 * q * 1e-3), 1.0, 1e-3);  // referred to the input current
        }
        // Flicker noise of an imported model: S = (2qI + KF·I^AF / f)·r_d².
        s.setSpiceModel(d, SpiceModelRef{".model DF D(IS=1e-14 KF=1e-14 AF=1)", "DF", ""});
        o.fStart = 1;
        o.fStop = 1e4;
        o.pointsPerDecade = 1;
        NoiseResult f = noiseAnalysis(s, o);
        CHECK(f.ok);
        if (f.ok) {
            const double rd2 = spicedevVt() / 1e-3;
            for (size_t k = 0; k < f.frequency.size(); ++k) {
                const double expected = std::sqrt((2 * q * 1e-3 + 1e-14 * 1e-3 / f.frequency[k])) * rd2;
                CHECK_NEAR(f.outputDensity[k] / expected, 1.0, 2e-3);
            }
            bool flicker = false;
            for (const auto& cc : f.contributions) flicker |= cc.kind == "flicker" && cc.componentId == d;
            CHECK(flicker);
        }
    }
    // Op-amp noise densities: a follower passes EN; through 10 kΩ the current noise adds IN·10 kΩ and the resistor
    // its thermal noise. Both the single-pole model and the macromodel.
    for (const char* value : {"generic EN=10n IN=1p", "generic EN=10n IN=1p SR=1V/us"}) {
        Schematic s;
        int v = s.addComponent(ComponentKind::VoltageSource, "0 AC 1", {0, 0});
        int rs = s.addComponent(ComponentKind::Resistor, "10k", {50, 0});
        int a = s.addComponent(ComponentKind::OpAmp, value, {100, 0});
        int rl = s.addComponent(ComponentKind::Resistor, "10k", {200, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        wire(s, v, "+", rs, "1");
        wire(s, rs, "2", a, "IN+");
        wire(s, a, "OUT", a, "IN-");
        wire(s, a, "OUT", rl, "1");
        wire(s, rl, "2", g, "GND");
        wire(s, v, "-", g, "GND");
        NoiseOptions o;
        o.outputNet = s.netOf({a, 2});
        o.fStart = 10;
        o.fStop = 100;
        NoiseResult r = noiseAnalysis(s, o);
        CHECK(r.ok);
        const double expected = std::sqrt(1e-16 + 1e-24 * 1e8 + 4 * kT * 1e4);
        if (r.ok) CHECK_NEAR(r.outputDensity[0] / expected, 1.0, 1e-4);
    }
    // A BJT stage: collector shot noise and the resistors all contribute; the largest is listed first.
    {
        Schematic s;
        int vcc = s.addComponent(ComponentKind::VoltageSource, "10", {0, 0});
        int rb = s.addComponent(ComponentKind::Resistor, "470k", {100, 0});
        int rc = s.addComponent(ComponentKind::Resistor, "4.7k", {200, -80});
        int q1 = s.addComponent(ComponentKind::NPN, "BC847", {200, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        wire(s, vcc, "+", rb, "1");
        wire(s, rb, "2", q1, "B");
        wire(s, vcc, "+", rc, "1");
        wire(s, rc, "2", q1, "C");
        wire(s, q1, "E", g, "GND");
        wire(s, vcc, "-", g, "GND");
        NoiseOptions o;
        o.outputNet = s.netOf({q1, 1});
        o.sourceId = vcc;
        NoiseResult r = noiseAnalysis(s, o);
        CHECK(r.ok && r.contributions.size() >= 3);
        if (r.ok)
            for (size_t k = 1; k < r.contributions.size(); ++k) CHECK(r.contributions[k - 1].rms >= r.contributions[k].rms);
    }
    // Errors.
    {
        Schematic s;
        rcLowPass(s, "0 AC 1", "1k", "1n");
        NoiseOptions o;
        CHECK(noiseAnalysis(s, o).error.find("output") != std::string::npos);
        o.outputNet = 0;
        o.fStop = 0;
        CHECK(!noiseAnalysis(s, o).ok);
    }
    CHECK(sieda_c_api_noise_test() == 0);
}

TEST(waveform_measurements) {
    // A 1 kHz sine of 2 V amplitude on 0.5 V: RMS √(0.25 + 2), AC RMS √2, frequency, duty 50 %, no step.
    {
        std::vector<double> t, v;
        for (int i = 0; i <= 20000; ++i) {
            t.push_back(i * 1e-7);
            v.push_back(0.5 + 2 * std::sin(2 * kPi * 1e3 * t.back()));
        }
        WaveformMeasurements m = measureWaveform(t, v);
        CHECK(m.ok && !m.stepLike);
        CHECK_NEAR(m.max, 2.5, 1e-6);
        CHECK_NEAR(m.min, -1.5, 1e-6);
        CHECK_NEAR(m.peakToPeak, 4, 1e-6);
        CHECK_NEAR(m.average, 0.5, 1e-6);
        CHECK_NEAR(m.rms, std::sqrt(0.25 + 2.0), 1e-6);
        CHECK_NEAR(m.acRms, std::sqrt(2.0), 1e-6);
        CHECK_NEAR(m.frequency, 1e3, 1e-3);
        CHECK(m.cycles == 1);
        CHECK_NEAR(m.dutyCycle, 0.5, 1e-4);
        // Rise 10 % → 90 % of a sine: (asin(0.8) − asin(−0.8)) / ω.
        CHECK_NEAR(m.riseTime, 2 * std::asin(0.8) / (2 * kPi * 1e3), 1e-8);
        CHECK_NEAR(m.fallTime, m.riseTime, 1e-8);
        // A window: the first half period only.
        WaveformMeasurements half = measureWaveform(t, v, 0, 0.5e-3);
        CHECK(half.ok && std::isnan(half.frequency));
        CHECK_NEAR(half.max, 2.5, 1e-6);
    }
    // RC step: rise 10 → 90 % = τ·ln 9, no overshoot, settled to 2 % after τ·ln 50.
    {
        const double tau = 1e-3;
        std::vector<double> t, v;
        for (int i = 0; i <= 100000; ++i) {
            t.push_back(i * 1e-7);
            v.push_back(5 * (1 - std::exp(-t.back() / tau)));
        }
        WaveformMeasurements m = measureWaveform(t, v);
        CHECK(m.ok && m.stepLike);
        // The window ends at 10τ, 0.005 % short of the final value.
        CHECK_NEAR(m.riseTime / (tau * std::log(9.0)), 1.0, 1e-3);
        CHECK_NEAR(m.overshootPercent, 0, 1e-9);
        CHECK_NEAR(m.settlingTime / (tau * std::log(50.0)), 1.0, 2e-3);
        CHECK(std::isnan(m.fallTime) && std::isnan(m.frequency));
    }
    // Underdamped second-order step: overshoot exp(−πζ/√(1−ζ²)).
    {
        const double zeta = 0.3, wn = 2 * kPi * 1e3, wd = wn * std::sqrt(1 - zeta * zeta);
        std::vector<double> t, v;
        for (int i = 0; i <= 200000; ++i) {
            t.push_back(i * 1e-7);
            const double x = t.back();
            v.push_back(1 - std::exp(-zeta * wn * x) * (std::cos(wd * x) + zeta / std::sqrt(1 - zeta * zeta) * std::sin(wd * x)));
        }
        WaveformMeasurements m = measureWaveform(t, v);
        CHECK(m.ok && m.stepLike);
        CHECK_NEAR(m.overshootPercent, 100 * std::exp(-kPi * zeta / std::sqrt(1 - zeta * zeta)), 0.01);
    }
    // PWM at 25 % duty, 10 kHz: period, duty; flat lines and bad input.
    {
        std::vector<double> t, v;
        for (int i = 0; i <= 10000; ++i) {
            t.push_back(i * 1e-7);
            v.push_back(std::fmod(t.back(), 1e-4) < 0.25e-4 ? 3.3 : 0.0);
        }
        WaveformMeasurements m = measureWaveform(t, v);
        CHECK(m.ok);
        CHECK_NEAR(m.period, 1e-4, 1e-9);
        CHECK_NEAR(m.dutyCycle, 0.25, 0.002);
        CHECK(m.cycles == 8);  // starts high: rising edges at 0.1 … 0.9 ms
        WaveformMeasurements flat = measureWaveform({0, 1, 2}, {1, 1, 1});
        CHECK(flat.ok && flat.peakToPeak == 0 && std::isnan(flat.riseTime));
        CHECK(!measureWaveform({0}, {1}).ok);
        CHECK(!measureWaveform({0, 1}, {1, NAN}).ok);
        CHECK(!measureWaveform({1, 0}, {1, 2}).ok);
        CHECK(!measureWaveform({0, 1, 2}, {0, 1, 2}, 5, 6).ok);
    }
    // C API.
    char* r = sieda_measure_waveform("{\"time\":[0,1,2,3,4],\"values\":[0,0,1,1,1]}");
    CHECK(r && std::string(r).find("\"ok\":true") != std::string::npos && std::string(r).find("\"riseTime\":0.8") != std::string::npos);
    sieda_string_free(r);
    char* bad = sieda_measure_waveform("{nope");
    CHECK(bad && std::string(bad).find("\"ok\":false") != std::string::npos);
    sieda_string_free(bad);
    char* empty = sieda_measure_waveform(nullptr);
    CHECK(empty && std::string(empty).find("\"ok\":false") != std::string::npos);
    sieda_string_free(empty);
}

TEST(transient_trapezoidal_and_adaptive_lc_oscillator) {
    // A series LC loop charged to 1 V at t = 0 rings at 1/(2π√LC). Backward Euler damps it numerically; the trapezoidal
    // rule keeps its amplitude; the adaptive run keeps it within tolerance with its own steps.
    Schematic s;
    int v = s.addComponent(ComponentKind::VoltageSource, "PULSE(1 0 1 0.99)", {0, 0});  // 1 V at t = 0, then 0
    int r = s.addComponent(ComponentKind::Resistor, "1m", {50, 0});
    int l = s.addComponent(ComponentKind::Inductor, "1m", {100, 0});
    int c = s.addComponent(ComponentKind::Capacitor, "1u", {150, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    wire(s, v, "+", r, "1");
    wire(s, r, "2", l, "1");
    wire(s, l, "2", c, "1");
    wire(s, c, "2", g, "GND");
    wire(s, v, "-", g, "GND");
    const double f0 = 1 / (2 * kPi * std::sqrt(1e-3 * 1e-6)), period = 1 / f0;
    const int net = s.netOf({c, 0});
    auto amplitudeLate = [&](const TransientResult& tr) {
        double a = 0;
        for (size_t i = 0; i < tr.time.size(); ++i)
            if (tr.time[i] > 18 * period) a = std::max(a, std::fabs(tr.netVoltages[static_cast<size_t>(net)][i]));
        return a;
    };
    TransientOptions o;
    o.tStop = 20 * period;
    o.tStep = period / 200;
    TransientResult be = Simulator(s).transient(o);  // = the established fixed-step analysis
    CHECK(be.ok);
    o.trapezoidal = true;
    TransientResult trap = Simulator(s).transient(o);
    CHECK(trap.ok);
    o.adaptive = true;
    o.maxStep = period / 20;
    TransientResult ad = Simulator(s).transient(o);
    CHECK(ad.ok);
    if (be.ok && trap.ok && ad.ok) {
        CHECK(amplitudeLate(be) < 0.6);   // ≈ exp(−π·20·2π/200·…): heavily damped
        CHECK(amplitudeLate(trap) > 0.98);
        CHECK(amplitudeLate(ad) > 0.95);
        WaveformMeasurements m = measureWaveform(trap.time, trap.netVoltages[static_cast<size_t>(net)], 2 * period, 19 * period);
        CHECK(m.ok && std::fabs(m.frequency / f0 - 1) < 2e-3);
        WaveformMeasurements ma = measureWaveform(ad.time, ad.netVoltages[static_cast<size_t>(net)], 2 * period, 19 * period);
        CHECK(ma.ok && std::fabs(ma.frequency / f0 - 1) < 1e-2);  // trapezoidal phase error at ~20 steps per period
        CHECK(ad.time.size() < trap.time.size());  // coarser steps where the waveform allows them
        for (size_t i = 1; i < ad.time.size(); ++i) CHECK(ad.time[i] > ad.time[i - 1]);
    }
    // The default options reproduce the fixed-step analysis exactly.
    TransientResult classic = Simulator(s).transient(o.tStop, period / 200);
    CHECK(classic.time == be.time && classic.netVoltages == be.netVoltages);
}

TEST(transient_stiff_boost_converter) {
    // A 5 V → ~9.5 V boost converter: 100 kHz MOSFET switch, Schottky diode, 100 µH, 100 µF, 47 Ω — switching edges
    // between long smooth intervals. The adaptive run lands on every gate edge and agrees with a fine fixed step.
    Schematic s;
    int vin = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    int vg = s.addComponent(ComponentKind::VoltageSource, "PULSE(0 10 10u 0.5)", {0, 100});
    int l = s.addComponent(ComponentKind::Inductor, "100u", {50, 0});
    int m = s.addComponent(ComponentKind::NMOS, "IRLZ44N", {100, 50});
    int d = s.addComponent(ComponentKind::Diode, "1N5819", {150, 0});
    int c = s.addComponent(ComponentKind::Capacitor, "100u", {200, 50});
    int rl = s.addComponent(ComponentKind::Resistor, "47", {250, 50});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 150});
    wire(s, vin, "+", l, "1");
    wire(s, l, "2", m, "D");
    wire(s, m, "S", g, "GND");
    wire(s, vg, "+", m, "G");
    wire(s, vg, "-", g, "GND");
    wire(s, l, "2", d, "A");
    wire(s, d, "K", c, "1");
    wire(s, c, "2", g, "GND");
    wire(s, d, "K", rl, "1");
    wire(s, rl, "2", g, "GND");
    wire(s, vin, "-", g, "GND");
    const int out = s.netOf({c, 0});
    TransientOptions o;
    o.tStop = 3e-3;
    o.tStep = 50e-9;
    TransientResult fixed = Simulator(s).transient(o);
    CHECK(fixed.ok);
    o.adaptive = true;
    o.trapezoidal = true;
    o.maxStep = 1e-6;
    TransientResult ad = Simulator(s).transient(o);
    CHECK(ad.ok);
    if (fixed.ok && ad.ok) {
        const double vf = fixed.netVoltages[static_cast<size_t>(out)].back(), va = ad.netVoltages[static_cast<size_t>(out)].back();
        CHECK(vf > 7.5 && vf < 10.0);
        CHECK(std::fabs(va - vf) / vf < 0.02);
        CHECK(ad.time.size() * 3 < fixed.time.size());  // far fewer steps
        // Every gate edge is a time point.
        std::set<long> points;
        for (double t : ad.time) points.insert(std::lround(t * 1e10));
        for (int k = 1; k < 300; ++k) {
            CHECK(points.count(std::lround(k * 10e-6 * 1e10)) == 1);
            CHECK(points.count(std::lround((k * 10e-6 + 5e-6) * 1e10)) == 1);
        }
    }
}

TEST(transient_astable_multivibrator_oscillates) {
    // Two cross-coupled NPNs: f ≈ 1 / (ln 2·(R1·C1 + R2·C2)). The supply steps on at t = 0 and slightly unequal
    // capacitors start it. The transistors carry the 2N3904 model: its junction capacitances regularise the
    // regenerative switching (the capacitance-free built-in model has an impasse there).
    Schematic s;
    int vcc = s.addComponent(ComponentKind::VoltageSource, "PULSE(0 9 1 0.99)", {0, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 200});
    int q1 = s.addComponent(ComponentKind::NPN, "BC847", {100, 100});
    int q2 = s.addComponent(ComponentKind::NPN, "BC847", {300, 100});
    int rc1 = s.addComponent(ComponentKind::Resistor, "1k", {100, 0});
    int rc2 = s.addComponent(ComponentKind::Resistor, "1k", {300, 0});
    int rb1 = s.addComponent(ComponentKind::Resistor, "47k", {150, 0});
    int rb2 = s.addComponent(ComponentKind::Resistor, "47k", {250, 0});
    int c1 = s.addComponent(ComponentKind::Capacitor, "10n", {150, 50});
    int c2 = s.addComponent(ComponentKind::Capacitor, "11n", {250, 50});
    wire(s, vcc, "-", g, "GND");
    for (int r : {rc1, rc2, rb1, rb2}) wire(s, vcc, "+", r, "1");
    wire(s, rc1, "2", q1, "C");
    wire(s, rc2, "2", q2, "C");
    wire(s, q1, "E", g, "GND");
    wire(s, q2, "E", g, "GND");
    wire(s, rb1, "2", q1, "B");
    wire(s, rb2, "2", q2, "B");
    wire(s, q1, "C", c1, "1");   // C1: collector of Q1 → base of Q2
    wire(s, c1, "2", q2, "B");
    wire(s, q2, "C", c2, "1");   // C2: collector of Q2 → base of Q1
    wire(s, c2, "2", q1, "B");
    for (int q : {q1, q2}) s.setSpiceModel(q, SpiceModelRef{builtinSpiceModels()[4].text, "2N3904", ""});
    const double expected = 1 / (std::log(2.0) * 47e3 * (10e-9 + 11e-9));
    for (bool adaptive : {false, true}) {
        TransientOptions o;
        o.tStop = 10e-3;
        o.tStep = 1e-6;
        o.adaptive = adaptive;
        o.maxStep = 5e-6;
        TransientResult tr = Simulator(s).transient(o);
        CHECK(tr.ok);
        if (!tr.ok) continue;
        WaveformMeasurements m = measureWaveform(tr.time, tr.netVoltages[static_cast<size_t>(s.netOf({q1, 1}))], 3e-3, 10e-3);
        CHECK(m.ok && m.peakToPeak > 7);
        CHECK(m.ok && std::fabs(m.frequency / expected - 1) < 0.15);
    }
}

extern "C" int sieda_c_api_transient_ex_test(void);

TEST(dc_convergence_fallbacks) {
    // A stack of 30 series diodes from 100 V through 10 Ω: plain Newton from zero needs the damped steps and the
    // homotopies; it must converge to ~0.8 V per diode and keep KCL.
    Schematic s;
    int v = s.addComponent(ComponentKind::VoltageSource, "100", {0, 0});
    int r = s.addComponent(ComponentKind::Resistor, "10", {50, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    wire(s, v, "+", r, "1");
    wire(s, v, "-", g, "GND");
    int prev = r, prevPin = 1;
    std::vector<int> diodes;
    for (int i = 0; i < 30; ++i) {
        int d = s.addComponent(ComponentKind::Diode, "1N4007", {100.0 + i * 20, 0});
        s.connect({prev, prevPin}, {d, 0});
        prev = d;
        prevPin = 1;
        diodes.push_back(d);
    }
    s.connect({prev, 1}, {g, 0});
    DcResult dc = Simulator(s).dcOperatingPoint();
    CHECK(dc.converged);
    if (dc.converged) {
        const double i = reading(dc, r)->current;
        for (int d : diodes) CHECK_NEAR(reading(dc, d)->current, i, 1e-6 * i);
        const double perDiode = (100 - 10 * i) / 30;
        CHECK(perDiode > 0.8 && perDiode < 1.6);
    }
    CHECK(sieda_c_api_transient_ex_test() == 0);
    // Options are validated.
    TransientOptions bad;
    bad.adaptive = true;
    bad.reltol = 0;
    CHECK(!Simulator(s).transient(bad).ok);
    bad.reltol = 1e-3;
    bad.tStop = -1;
    CHECK(!Simulator(s).transient(bad).ok);
}

TEST(spice_netlist_export_with_models) {
    Schematic s;
    int v = s.addComponent(ComponentKind::VoltageSource, "1", {0, 0});
    int u = s.addComponent(ComponentKind::OpAmp, "UA741", {100, 0});
    int rf = s.addComponent(ComponentKind::Resistor, "10k", {150, -60});
    int rg = s.addComponent(ComponentKind::Resistor, "10k", {50, -60});
    int d = s.addComponent(ComponentKind::Diode, "", {200, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    wire(s, v, "+", u, "IN+");
    wire(s, v, "-", g, "GND");
    wire(s, u, "OUT", rf, "2");
    wire(s, rf, "1", u, "IN-");
    wire(s, rg, "2", u, "IN-");
    wire(s, rg, "1", g, "GND");
    wire(s, u, "OUT", d, "A");
    wire(s, d, "K", g, "GND");
    s.setSpiceModel(u, SpiceModelRef{builtinSpiceModels()[9].text, "UA741", ""});
    s.setSpiceModel(d, SpiceModelRef{".model DX D(IS=1n)\n", "DX", ""});
    const std::string net = exportSpiceNetlist(s, "models");
    CHECK(net.find("XU1 ") != std::string::npos && net.find(" UA741\n") != std::string::npos);
    CHECK(net.find("VU1_rail1 U1_rail1 0 DC 15") != std::string::npos);
    CHECK(net.find("VU1_rail2 U1_rail2 0 DC -15") != std::string::npos);
    CHECK(net.find(".subckt UA741") != std::string::npos && net.find(".model DX D(IS=1n)") != std::string::npos);
    CHECK(net.find("\nD1 ") != std::string::npos && net.find(" DX\n") != std::string::npos);
    CHECK(net.find("OPAMP_IDEAL") == std::string::npos && net.find("DSIEDA") == std::string::npos);
    // The netlist's definitions are importable again.
    SpiceLibrary lib = parseSpiceLibrary(net);
    CHECK(flattenSpiceModel(lib, "UA741").ok && flattenSpiceModel(lib, "DX").ok);
}

TEST(spice_standard_waveforms) {
    // SPICE PULSE(v1 v2 td tr tf pw per): delay, ramps, width, period.
    auto pulse = SourceSpec::parse("PULSE(0 5 1u 1u 2u 3u 10u)");
    CHECK(pulse && pulse->shape == SourceSpec::Shape::Spice);
    if (pulse) {
        CHECK_NEAR(pulse->valueAt(0.5e-6), 0, 1e-12);
        CHECK_NEAR(pulse->valueAt(1.5e-6), 2.5, 1e-9);
        CHECK_NEAR(pulse->valueAt(3e-6), 5, 1e-12);
        CHECK_NEAR(pulse->valueAt(6e-6), 2.5, 1e-9);
        CHECK_NEAR(pulse->valueAt(8e-6), 0, 1e-12);
        CHECK_NEAR(pulse->valueAt(13e-6), 5, 1e-9);  // next period
        CHECK(pulse->v1 == 0 && pulse->v2 == 5);      // the range other checks read
    }
    // The established square-wave form is untouched.
    auto square = SourceSpec::parse("PULSE(0 5 1m)");
    CHECK(square && square->shape == SourceSpec::Shape::Square && square->spiceText.empty());
    auto pwl = SourceSpec::parse("PWL(0 0 1m 1 2m 1 3m 0)");
    CHECK(pwl && pwl->shape == SourceSpec::Shape::Pwl);
    if (pwl) {
        CHECK_NEAR(pwl->valueAt(0.5e-3), 0.5, 1e-12);
        CHECK_NEAR(pwl->valueAt(1.5e-3), 1, 1e-12);
        CHECK_NEAR(pwl->valueAt(2.5e-3), 0.5, 1e-12);
        CHECK_NEAR(pwl->valueAt(9e-3), 0, 1e-12);
    }
    auto ex = SourceSpec::parse("EXP(0 1 1m 1m 5m 1m)");
    CHECK(ex && ex->shape == SourceSpec::Shape::Exp);
    if (ex) {
        CHECK_NEAR(ex->valueAt(2e-3), 1 - std::exp(-1.0), 1e-12);
        CHECK_NEAR(ex->valueAt(6e-3), (1 - std::exp(-5.0)) - (1 - std::exp(-1.0)), 1e-12);
    }
    auto sine = SourceSpec::parse("SIN(0 1 1k 1m 100 90)");
    CHECK(sine && sine->sineExtended);
    if (sine) {
        CHECK_NEAR(sine->valueAt(0.5e-3), 1, 1e-12);  // vo + va·sin(phase) before the delay
        CHECK_NEAR(sine->valueAt(1e-3 + 0.5e-3), std::exp(-100 * 0.5e-3) * std::sin(kPi + kPi / 2), 1e-12);
    }
    CHECK(SourceSpec::parse("SIN(0 1 1k)") && !SourceSpec::parse("SIN(0 1 1k)")->sineExtended);
    for (const char* bad : {"PWL(0 0 1m)", "PWL(1m 0 0 1)", "EXP(0 1 1m 0)", "PULSE(0 5 0 1u 1u 5u 2u)", "SIN(0 1 1k -1)"})
        CHECK(!SourceSpec::parse(bad));
    // In a circuit: an RC driven by a PWL ramp; the adaptive transient lands on its corners and the export keeps it.
    Schematic s;
    const int outNet = rcLowPass(s, "PWL(0 0 1m 5 2m 5 2.5m 0)", "1k", "100n");
    int v = -1;
    for (const auto& comp : s.components())
        if (comp.kind == ComponentKind::VoltageSource) v = comp.id;
    TransientOptions o;
    o.tStop = 4e-3;
    o.tStep = 50e-6;
    o.adaptive = true;
    o.trapezoidal = true;
    TransientResult tr = Simulator(s).transient(o);
    CHECK(tr.ok);
    if (tr.ok) {
        for (double corner : {1e-3, 2e-3, 2.5e-3}) {
            bool hit = false;
            for (double t : tr.time) hit |= std::fabs(t - corner) < 1e-12;
            CHECK(hit);
        }
        // Ramp of 5 V/ms through τ = 0.1 ms: the output lags the input by τ at the end of the ramp.
        const int net = outNet;
        double at1 = NAN;
        for (size_t i = 0; i < tr.time.size(); ++i)
            if (std::fabs(tr.time[i] - 1e-3) < 1e-12) at1 = tr.netVoltages[static_cast<size_t>(net)][i];
        CHECK_NEAR(at1, 5 * (1e-3 - 1e-4 * (1 - std::exp(-10.0))) / 1e-3, 0.01);
    }
    CHECK(exportSpiceNetlist(s, "pwl").find("PWL(0 0 1m 5 2m 5 2.5m 0)") != std::string::npos);
    (void)v;
    // Inside a model: a PULSE source drives the output through a buffer.
    const std::string sub = ".subckt GEN in out\nV1 a 0 PULSE(0 2 1u 0 0 2u)\nE1 out 0 a 0 1\nR1 in 0 1k\n.ends\n";
    SpiceFlatCircuit flat = flattenSpiceModel(parseSpiceLibrary(sub), "GEN");
    CHECK(flat.ok && !hasDiag(flat.diagnostics, SpiceDiagnostic::Level::Warning, "held at its DC value"));
    Schematic m;
    int src = m.addComponent(ComponentKind::VoltageSource, "0", {0, 0});
    int u = m.addComponent(ComponentKind::IC8, "IC", {100, 0});
    int rl = m.addComponent(ComponentKind::Resistor, "1k", {200, 0});
    int g = m.addComponent(ComponentKind::Ground, "", {0, 80});
    wire(m, src, "+", u, "1");
    wire(m, src, "-", g, "GND");
    wire(m, u, "2", rl, "1");
    wire(m, rl, "2", g, "GND");
    m.setSpiceModel(u, SpiceModelRef{sub, "GEN", "1 2"});
    TransientResult mt = Simulator(m).transient(5e-6, 0.1e-6);
    CHECK(mt.ok);
    if (mt.ok) {
        const auto& w = mt.netVoltages[static_cast<size_t>(m.netOf({u, 1}))];
        CHECK_NEAR(w[5], 0, 1e-9);    // 0.5 µs: before the delay
        CHECK_NEAR(w[20], 2, 1e-6);   // 2 µs: high
        CHECK_NEAR(w[45], 0, 1e-6);   // 4.5 µs: after the 2 µs width
    }
}

// ------------------------------------------------------------------ supplier part data (Nexar, DigiKey, Mouser)

#include "sieda/Suppliers.hpp"

extern "C" int sieda_c_api_supplier_test(void);

namespace {
std::string readSupplierFixture(const std::string& name) {
    std::ifstream f(std::string(SIEDA_FIXTURE_DIR) + "/suppliers/" + name, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

const SupplierPart* supplierPart(const std::vector<SupplierPart>& parts, const std::string& mpn) {
    for (const auto& p : parts)
        if (p.mpn == mpn) return &p;
    return nullptr;
}
}  // namespace

TEST(supplier_lifecycle_prices_and_quotes) {
    CHECK(partLifecycleFromText("Active") == PartLifecycle::Active);
    CHECK(partLifecycleFromText("Production (Last Updated: 2 years ago)") == PartLifecycle::Active);
    CHECK(partLifecycleFromText("Not Recommended for New Designs") == PartLifecycle::Nrnd);
    CHECK(partLifecycleFromText("NRND") == PartLifecycle::Nrnd);
    CHECK(partLifecycleFromText("Not For New Designs") == PartLifecycle::Nrnd);
    CHECK(partLifecycleFromText("End of Life") == PartLifecycle::Eol);
    CHECK(partLifecycleFromText("Last Time Buy") == PartLifecycle::Eol);
    CHECK(partLifecycleFromText("EOL") == PartLifecycle::Eol);
    CHECK(partLifecycleFromText("Obsolete") == PartLifecycle::Obsolete);
    CHECK(partLifecycleFromText("Discontinued at Digi-Key") == PartLifecycle::Obsolete);
    CHECK(partLifecycleFromText("New Product") == PartLifecycle::New);
    CHECK(partLifecycleFromText("Preliminary") == PartLifecycle::New);
    CHECK(partLifecycleFromText("") == PartLifecycle::Unknown);
    CHECK(partLifecycleFromText("Develop") == PartLifecycle::Unknown);  // "eol" only as a word
    CHECK(lifecycleNeedsAttention(PartLifecycle::Nrnd) && !lifecycleNeedsAttention(PartLifecycle::Active));
    CHECK(partLifecycleFromName(partLifecycleName(PartLifecycle::Eol)) == PartLifecycle::Eol);

    CHECK_NEAR(parsePriceText("$0.452"), 0.452, 1e-12);
    CHECK_NEAR(parsePriceText("0,45 \xE2\x82\xAC"), 0.45, 1e-12);
    CHECK_NEAR(parsePriceText("1.234,56 \xE2\x82\xAC"), 1234.56, 1e-9);
    CHECK_NEAR(parsePriceText("1,234.50"), 1234.5, 1e-9);
    CHECK_NEAR(parsePriceText("1,234"), 1234, 1e-9);
    CHECK_NEAR(parsePriceText("0,452"), 0.452, 1e-12);
    CHECK_NEAR(parsePriceText("12,5"), 12.5, 1e-12);
    CHECK_NEAR(parsePriceText("\xC2\xA5" "63"), 63, 1e-12);
    CHECK_NEAR(parsePriceText("1 234,56"), 1234.56, 1e-9);
    CHECK(std::isnan(parsePriceText("Quote")));
    CHECK(std::isnan(parsePriceText("")));
    CHECK(normalizeMpn("ltc4359ims8#pbf") == "LTC4359IMS8PBF");

    SupplierOffer o;
    o.currency = "USD";
    o.stock = 1000;
    o.prices = {{1, 0.50}, {10, 0.40}, {100, 0.20}};
    PriceQuote q = quoteOffer(o, 5);
    CHECK(q.ok && q.orderQuantity == 5 && std::fabs(q.unitPrice - 0.5) < 1e-12 && q.inStock);
    q = quoteOffer(o, 90);  // 90 x 0.40 = 36 > 100 x 0.20 = 20: buy 100
    CHECK(q.orderQuantity == 100 && std::fabs(q.extended - 20) < 1e-9);
    o.moq = 25;
    o.multiple = 25;
    q = quoteOffer(o, 30);  // MOQ and multiple: 50 at 0.40
    CHECK(q.orderQuantity == 50 && std::fabs(q.unitPrice - 0.40) < 1e-12);
    o.stock = 40;
    CHECK(!quoteOffer(o, 30).inStock);
    o.stock = -1;
    CHECK(!quoteOffer(o, 1).inStock);  // stock not reported: not confirmed
    SupplierOffer reel;
    reel.prices = {{2500, 0.1}};
    q = quoteOffer(reel, 3);
    CHECK(q.orderQuantity == 2500 && std::fabs(q.extended - 250) < 1e-9);
    CHECK(!quoteOffer(SupplierOffer{}, 3).ok);
    q = quoteOffer(o, 2000000000);  // no overflow on silly quantities
    CHECK(q.ok && q.orderQuantity > 0 && std::isfinite(q.extended));

    SupplierPart part;
    part.mpn = "X";
    SupplierOffer cheapNoStock = o, dearInStock = o;
    cheapNoStock.stock = 0;
    cheapNoStock.moq = cheapNoStock.multiple = 1;
    dearInStock.stock = 500;
    dearInStock.moq = dearInStock.multiple = 1;
    dearInStock.prices = {{1, 0.9}};
    SupplierOffer euro = dearInStock;
    euro.currency = "EUR";
    euro.prices = {{1, 0.01}};
    part.offers = {cheapNoStock, dearInStock, euro};
    OfferChoice c = bestOffer(part, 10, "USD");
    CHECK(c.offerIndex == 1);  // in stock beats cheaper but out of stock; EUR ignored while USD offers exist
    c = bestOffer(part, 10, "");
    CHECK(c.offerIndex == 2);
    c = bestOffer(part, 10, "GBP");  // nothing in GBP: any currency
    CHECK(c.offerIndex == 2);
}

TEST(supplier_parsers_read_recorded_replies) {
    // Mouser v2 keyword search.
    const SupplierSearchResult m = parseMouserResponse(readSupplierFixture("mouser_v2_keyword_lm358dr.json"));
    CHECK(m.error.empty() && m.source == "mouser" && m.total == 3 && m.parts.size() == 3);
    const SupplierPart* dr = supplierPart(m.parts, "LM358DR");
    CHECK(dr != nullptr);
    if (dr) {
        CHECK(dr->manufacturer == "Texas Instruments" && dr->lifecycle == PartLifecycle::Unknown);
        CHECK(dr->datasheet == "https://www.ti.com/lit/ds/symlink/lm358.pdf");
        CHECK(dr->offers.size() == 1 && dr->offers[0].sku == "595-LM358DR" && dr->offers[0].stock == 23764);
        CHECK(dr->offers[0].prices.size() == 5 && std::fabs(dr->offers[0].prices[2].price - 0.184) < 1e-12);
        CHECK(dr->offers[0].currency == "USD" && dr->offers[0].leadTimeDays == 42 && dr->offers[0].packaging == "Reel");
    }
    const SupplierPart* g4 = supplierPart(m.parts, "LM358DRG4");
    CHECK(g4 && g4->lifecycle == PartLifecycle::Nrnd && g4->offers.size() == 1 && g4->offers[0].stock == 0 &&
          g4->offers[0].moq == 2500 && g4->offers[0].multiple == 2500 && g4->offers[0].leadTimeDays == 42);
    const SupplierPart* on = supplierPart(m.parts, "LM358DR2G");
    CHECK(on && on->lifecycle == PartLifecycle::Eol && on->datasheet.empty());  // javascript: link dropped
    CHECK(on && on->productUrl == "https://www.mouser.de/ProductDetail/onsemi/LM358DR2G");
    CHECK(on && on->offers[0].currency == "EUR" && std::fabs(on->offers[0].prices[0].price - 0.41) < 1e-12 &&
          std::fabs(on->offers[0].prices[2].price - 1234.5) < 1e-9);
    const SupplierSearchResult me = parseMouserResponse(readSupplierFixture("mouser_v2_error_invalid_key.json"));
    CHECK(me.parts.empty() && me.errorKind == "auth" && me.error.find("Invalid unique identifier") != std::string::npos);

    // DigiKey v4 keyword search: exact matches first, no duplicates.
    const SupplierSearchResult d = parseDigikeyResponse(readSupplierFixture("digikey_v4_keyword_lm358dr.json"));
    CHECK(d.error.empty() && d.total == 41 && d.parts.size() == 2);
    if (d.parts.size() == 2) {
        CHECK(d.parts[0].mpn == "LM358DR" && d.parts[0].offers.size() == 1);
        const SupplierPart& e4 = d.parts[1];
        CHECK(e4.mpn == "LM358DRE4" && e4.lifecycle == PartLifecycle::Obsolete);
        CHECK(e4.datasheet == "https://www.ti.com/lit/ds/symlink/lm358.pdf");  // "//host" made https
        CHECK(e4.offers.size() == 1 && e4.offers[0].stock == 0 && e4.offers[0].prices.empty());
    }
    // The full product as listed in "Products": three packagings, the reel in whole reels.
    {
        Json root = Json::parse(readSupplierFixture("digikey_v4_keyword_lm358dr.json"));
        Json only = Json::object();
        Json products = Json::array();
        products.push(root.get("Products")[0]);
        only["Products"] = products;
        const SupplierSearchResult full = parseDigikeyResponse(only.dump());
        CHECK(full.parts.size() == 1);
        if (full.parts.size() == 1) {
            const SupplierPart& p = full.parts[0];
            CHECK(p.offers.size() == 3 && p.package == "8-SOIC" && p.rohs == "ROHS3 Compliant");
            CHECK(p.lifecycle == PartLifecycle::Active && p.category == "Instrumentation, Op Amps, Buffer Amps");
            CHECK(p.offers.size() == 3 && p.offers[0].sku == "296-1014-1-ND" && p.offers[0].packaging == "Cut Tape (CT)" &&
                  p.offers[0].stock == 18342);
            if (p.offers.size() == 3) {
                CHECK(p.offers[1].multiple == 2500 && p.offers[1].moq == 2500);
                CHECK(p.offers[2].multiple == 1);  // Digi-Reel: any quantity
                CHECK(p.offers[0].leadTimeDays == 42 && p.offers[0].currency == "USD");
            }
            CHECK(p.totalStock() == 18342 + 17500 + 18342);
            CHECK(bestOffer(p, 3000, "USD").offerIndex >= 0);
        }
    }
    const SupplierSearchResult de = parseDigikeyResponse(readSupplierFixture("digikey_v4_error_401.json"));
    CHECK(de.parts.empty() && de.errorKind == "auth" && de.error.find("expired") != std::string::npos);

    // Nexar supSearchMpn: specs carry lifecycle and package; converted prices; bad links dropped.
    const SupplierSearchResult n = parseNexarResponse(readSupplierFixture("nexar_supsearchmpn_mcp2551.json"));
    CHECK(n.error.empty() && n.total == 2 && n.parts.size() == 2);
    const SupplierPart* mcp = supplierPart(n.parts, "MCP2551-I/SN");
    CHECK(mcp && mcp->lifecycle == PartLifecycle::Nrnd && mcp->package == "SOIC" && mcp->offers.size() == 3);
    if (mcp && mcp->offers.size() == 3) {
        CHECK(mcp->offers[1].supplier == "Farnell" && mcp->offers[1].currency == "USD" &&
              std::fabs(mcp->offers[1].prices[0].price - 1.33) < 1e-12);
        CHECK(mcp->offers[2].url.empty() && mcp->offers[2].stock == -1 && mcp->offers[2].moq == 100);
        CHECK(mcp->offers[2].prices.size() == 2);  // the EUR ladder entry is not mixed in
        CHECK(mcp->offers[0].leadTimeDays == 49);
    }
    const SupplierPart* t = supplierPart(n.parts, "MCP2551T-I/SN");
    CHECK(t && t->lifecycle == PartLifecycle::Active && t->offers.empty());
    const SupplierSearchResult ne = parseNexarResponse(readSupplierFixture("nexar_error_unauthenticated.json"));
    CHECK(ne.parts.empty() && ne.errorKind == "auth");

    CHECK(parseSupplierResponse("octopart", readSupplierFixture("nexar_error_unauthenticated.json")).errorKind == "auth");
    CHECK(parseSupplierResponse("lcsc", "{}").errorKind == "source");

    // Normalised JSON round trip (the app's offline cache) keeps every field.
    const Json j = supplierSearchToJson(n, "USD");
    CHECK(j.get("schema").asString() == "sieda.supplier/1" && j.get("parts").size() == 2);
    const SupplierPart back = supplierPartFromJson(Json::parse(j.dump()).get("parts")[0]);
    CHECK(back.mpn == "MCP2551-I/SN" && back.lifecycle == PartLifecycle::Nrnd && back.offers.size() == 3 &&
          back.sources == std::vector<std::string>{"nexar"});
    CHECK(!back.offers.empty() && back.offers[0].prices.size() == 3 && back.offers[0].stock == 5210);
    const Json& pricing = j.get("parts")[0].get("pricing");
    CHECK(pricing.size() == 4);
    if (pricing.size() == 4) {
        CHECK(std::fabs(pricing[0].get("unitPrice").asNumber() - 1.18) < 1e-12);
        CHECK(std::fabs(pricing[2].get("unitPrice").asNumber() - 0.9) < 1e-12);
    }
}

TEST(supplier_merge_rollup_and_catalog_match) {
    const SupplierSearchResult m = parseMouserResponse(readSupplierFixture("mouser_v2_keyword_lm358dr.json"));
    const SupplierSearchResult d = parseDigikeyResponse(readSupplierFixture("digikey_v4_keyword_lm358dr.json"));
    const auto merged = mergeSupplierParts({m, d});
    const SupplierPart* dr = supplierPart(merged, "LM358DR");
    CHECK(dr && dr->offers.size() == 2 && dr->sources.size() == 2);  // Mouser + Digi-Key cut tape
    CHECK(merged.size() == 4);  // LM358DR, LM358DRG4, LM358DR2G, LM358DRE4
    // The worst lifecycle wins when distributors disagree.
    SupplierSearchResult a, b;
    SupplierPart pa, pb;
    pa.mpn = pb.mpn = "ABC123";
    pa.lifecycle = PartLifecycle::Active;
    pb.lifecycle = PartLifecycle::Eol;
    pb.lifecycleText = "Last Time Buy";
    a.parts = {pa};
    b.parts = {pb};
    const auto worst = mergeSupplierParts({a, b});
    CHECK(worst.size() == 1 && worst[0].lifecycle == PartLifecycle::Eol && worst[0].lifecycleText == "Last Time Buy");

    // Roll-up over the merged parts.
    Json req = Json::object();
    req["currency"] = "USD";
    Json qs = Json::array();
    for (int q : {1, 100}) qs.push(q);
    req["quantities"] = qs;
    req["buildQuantity"] = 10;
    Json lines = Json::array();
    auto line = [&](int item, std::vector<std::string> refs, int qty, const std::string& mpn, bool dnp = false) {
        Json l = Json::object();
        l["item"] = item;
        Json r = Json::array();
        for (const auto& s : refs) r.push(s);
        l["refs"] = r;
        l["quantity"] = qty;
        l["mpn"] = mpn;
        l["manufacturer"] = "";
        l["dnp"] = dnp;
        lines.push(l);
    };
    line(1, {"U1", "U2"}, 2, "lm358-dr");  // matched on the normalised MPN
    line(2, {"U3"}, 1, "LM358DRG4");       // NRND, reel only, none in stock
    line(3, {"R1"}, 1, "");
    line(4, {"U4"}, 1, "NOPE123");
    line(5, {"U5"}, 1, "LM358DR", true);
    req["lines"] = lines;
    Json parts = Json::array();
    for (const auto& p : merged) parts.push(supplierPartToJson(p));
    req["parts"] = parts;
    const Json r = supplierBomRollup(Json::parse(req.dump()));
    CHECK(r.get("lines").size() == 5 && r.get("totals").size() == 3);  // 1, 10 (build) and 100 boards
    if (r.get("lines").size() == 5) {
        const Json& l1 = r.get("lines")[0];
        CHECK(l1.get("status").asString() == "priced" && l1.get("offers").size() == 3);
        if (l1.get("offers").size() == 3) {
            CHECK(std::fabs(l1.get("offers")[0].get("extended").asNumber() - 0.90) < 1e-9);  // 2 at 0.45
            CHECK(l1.get("offers")[2].get("needed").asNumber() == 200);
        }
        const Json& l2 = r.get("lines")[1];
        CHECK(l2.get("lifecycle").asString() == "nrnd" &&
              l2.get("warning").asString().find("not recommended") != std::string::npos);
        CHECK(l2.get("offers").size() == 3 && l2.get("offers")[0].get("orderQuantity").asInt() == 2500 &&
              !l2.get("offers")[0].get("inStock").asBool());
        CHECK(r.get("lines")[2].get("status").asString() == "no_mpn");
        CHECK(r.get("lines")[3].get("status").asString() == "not_found");
        CHECK(r.get("lines")[4].get("status").asString() == "dnp" && r.get("lines")[4].get("offers").size() == 0);
    }
    if (r.get("totals").size() == 3) {
        const Json& t1 = r.get("totals")[0];
        CHECK(t1.get("boards").asInt() == 1 && t1.get("priced").asInt() == 2 && t1.get("unpriced").asInt() == 2 &&
              t1.get("shortages").asInt() == 1 && !t1.get("complete").asBool());
        CHECK(std::fabs(t1.get("cost").asNumber() - (0.90 + 255.0)) < 1e-6);  // 2 x 0.45 + a reel of 2500 x 0.102
        const Json& t100 = r.get("totals")[2];
        CHECK(std::fabs(t100.get("perBoard").asNumber() - t100.get("cost").asNumber() / 100) < 1e-12);
    }
    CHECK(r.get("warnings").size() >= 1 && !r.get("mixedCurrency").asBool());
    // Garbage requests give a roll-up, never an exception.
    CHECK(supplierBomRollup(Json::parse("{\"lines\":[1,\"x\",{}],\"parts\":[null,{\"mpn\":7}]}")).get("lines").size() == 3);
    CHECK(supplierBomRollup(Json()).get("totals").size() == 4);

    // Catalog matching.
    Json cm = catalogMatchForMpn("LM358DR");
    CHECK(cm.get("match").asString() == "LM358DR" && cm.get("exact").asBool());
    cm = catalogMatchForMpn("lm358dr");
    CHECK(cm.get("match").asString() == "LM358DR" && cm.get("exact").asBool());
    cm = catalogMatchForMpn("MAX485ESA+");  // catalog: MAX485ESA+T (reel)
    CHECK(cm.get("match").asString() == "MAX485ESA+T" && !cm.get("exact").asBool() && !cm.get("note").asString().empty());
    cm = catalogMatchForMpn("LTC4359IMS8#TRPBF");  // catalog: LTC4359IMS8#PBF
    CHECK(cm.get("match").asString() == "LTC4359IMS8#PBF");
    cm = catalogMatchForMpn("LM358DGKR");  // a different package: only a candidate
    CHECK(cm.get("match").asString().empty() && cm.get("candidates").size() >= 1);
    CHECK(catalogMatchForMpn("").get("match").asString().empty());
    CHECK(catalogMatchForMpn("ZZZZ99999").get("candidates").size() == 0);
}

TEST(supplier_parsers_survive_hostile_replies) {
    // Deep nesting, huge and empty bodies, non-JSON (an HTML error page), wrong types everywhere.
    CHECK(parseMouserResponse(std::string(100000, '[')).errorKind == "parse");
    CHECK(parseNexarResponse("{\"data\":" + std::string(5000, '{')).errorKind == "parse");
    CHECK(parseDigikeyResponse("").errorKind == "parse");
    CHECK(parseDigikeyResponse("<html><body>502 Bad Gateway</body></html>").errorKind == "service");
    CHECK(parseMouserResponse(std::string(SupplierLimits::maxBody + 1, ' ')).errorKind == "parse");
    CHECK(parseMouserResponse("[1,2,3]").errorKind == "parse");
    CHECK(parseMouserResponse("{\"SearchResults\":{\"Parts\":7}}").parts.empty());
    CHECK(parseDigikeyResponse("{\"Products\":[{\"ManufacturerProductNumber\":{},\"ProductVariations\":\"x\"}]}").parts.empty());
    const SupplierSearchResult weird = parseMouserResponse(
        "{\"SearchResults\":{\"Parts\":[{\"ManufacturerPartNumber\":\"A\\u0000B\\u0007\\ud800C\",\"Min\":-5,\"Mult\":1e300,"
        "\"PriceBreaks\":[{\"Quantity\":-1,\"Price\":\"$1\"},{\"Quantity\":1e30,\"Price\":\"$1e9\"},{\"Quantity\":5,\"Price\":1e400},"
        "{\"Quantity\":2,\"Price\":\"NaN\"},{\"Quantity\":3,\"Price\":\"$0.5\"}],\"AvailabilityInStock\":\"99999999999999999999\"}]}}");
    CHECK(weird.parts.size() == 1);
    if (weird.parts.size() == 1 && weird.parts[0].offers.size() == 1) {
        const SupplierOffer& o = weird.parts[0].offers[0];
        CHECK(o.moq == 1 && o.multiple >= 1);
        CHECK(o.stock == 1000000000000LL);
        for (const auto& b : o.prices) CHECK(b.quantity >= 1 && b.price > 0 && b.price <= 1e7);
        CHECK(weird.parts[0].mpn.find('\0') == std::string::npos);
    }
    // Too many parts: capped with a note.
    std::string many = "{\"SearchResults\":{\"Parts\":[";
    for (int i = 0; i < 260; ++i) many += std::string(i ? "," : "") + "{\"ManufacturerPartNumber\":\"P" + std::to_string(i) + "\"}";
    many += "]}}";
    const SupplierSearchResult capped = parseMouserResponse(many);
    CHECK(capped.parts.size() == SupplierLimits::maxParts && !capped.notes.empty());
    // A 100 kB description is cut.
    const SupplierSearchResult longText = parseMouserResponse(
        "{\"SearchResults\":{\"Parts\":[{\"ManufacturerPartNumber\":\"X\",\"Description\":\"" + std::string(100000, 'a') + "\"}]}}");
    CHECK(longText.parts.size() == 1 && longText.parts[0].description.size() <= SupplierLimits::maxString);

    // Deterministic mutations of every fixture: the parsers never throw and their output stays within the limits.
    const char* fixtures[][2] = {{"mouser", "mouser_v2_keyword_lm358dr.json"},
                                 {"digikey", "digikey_v4_keyword_lm358dr.json"},
                                 {"nexar", "nexar_supsearchmpn_mcp2551.json"},
                                 {"mouser", "mouser_v2_error_invalid_key.json"},
                                 {"digikey", "digikey_v4_error_401.json"},
                                 {"nexar", "nexar_error_unauthenticated.json"}};
    uint32_t seed = 0x5EDA5EDAu;
    auto rnd = [&]() {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return seed;
    };
    int parsed = 0;
    for (const auto& fx : fixtures) {
        const std::string original = readSupplierFixture(fx[1]);
        CHECK(!original.empty());
        for (int k = 0; k < 250; ++k) {
            std::string s = original;
            const size_t a = rnd() % (s.size() + 1), len = rnd() % 64;
            switch (k % 6) {
                case 0: s.resize(a); break;
                case 1:
                    for (int f = 0; f < 8 && !s.empty(); ++f) s[rnd() % s.size()] = static_cast<char>(rnd());
                    break;
                case 2: s.erase(a, len); break;
                case 3: s.insert(a, s.substr(a, len)); break;
                case 4: s.insert(a, std::string(len, "[{\"\\,:"[rnd() % 6])); break;
                default: s.insert(a, "\"Price\":\"$-1e999\",\"Quantity\":1e999,"); break;
            }
            try {
                const SupplierSearchResult r = parseSupplierResponse(fx[0], s);
                parsed += r.error.empty();
                CHECK(r.parts.size() <= SupplierLimits::maxParts);
                for (const auto& p : r.parts) {
                    CHECK(!p.mpn.empty() && p.offers.size() <= SupplierLimits::maxOffers);
                    for (const auto& o : p.offers) {
                        CHECK(o.moq >= 1 && o.multiple >= 1);
                        for (const auto& b : o.prices) CHECK(b.quantity >= 1 && b.price > 0 && std::isfinite(b.price));
                        CHECK(quoteOffer(o, 7).extended >= 0);
                    }
                    const Json j = supplierPartToJson(p, "USD");
                    CHECK(!Json::parse(j.dump()).get("mpn").asString().empty());
                }
            } catch (const std::exception& e) {
                std::printf("    %s mutation %d threw: %s\n", fx[1], k, e.what());
                CHECK(false);
            }
        }
    }
    CHECK(parsed > 100);  // most single mutations still read
}

TEST(supplier_c_api) { CHECK(sieda_c_api_supplier_test() == 0); }

// ------------------------------------------------------------------ imported 3D models (VRML, STL, OBJ)

#include <cstring>

#include "sieda/Model3D.hpp"

#define M3_THROWS(expr)                   \
    do {                                  \
        bool threw_ = false;              \
        try {                             \
            (void)(expr);                 \
        } catch (const std::exception&) { \
            threw_ = true;                \
        }                                 \
        CHECK(threw_);                    \
    } while (0)
#define M3_NOTHROW(expr)                  \
    do {                                  \
        bool threw_ = false;              \
        try {                             \
            (void)(expr);                 \
        } catch (const std::exception&) { \
            threw_ = true;                \
        }                                 \
        CHECK(!threw_);                   \
    } while (0)

extern "C" int sieda_c_api_model3d_test(void);

namespace {
std::string readModelFixture(const std::string& name) {
    std::ifstream f(std::string(SIEDA_FIXTURE_DIR) + "/models/" + name, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

const char* const kSoicModel = "SOIC-8_3.9x4.9mm_P1.27mm.wrl";

/// A unit cube [0, 1]³ as an ASCII STL.
std::string cubeStlAscii() {
    const float v[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    const int f[12][3] = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
                          {1, 2, 6}, {1, 6, 5}, {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7}};
    std::string s = "solid cube\n";
    for (const auto& t : f) {
        s += "  facet normal 0 0 0\n    outer loop\n";
        for (int k : t) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "      vertex %g %g %g\n", v[k][0], v[k][1], v[k][2]);
            s += buf;
        }
        s += "    endloop\n  endfacet\n";
    }
    return s + "endsolid cube\n";
}

/// The same cube as a binary STL (the header starts with "solid", as some exporters write it).
std::string cubeStlBinary() {
    const Model3DMesh ascii = parseStl(cubeStlAscii());
    std::string s(80, ' ');
    s.replace(0, 5, "solid");
    const uint32_t n = static_cast<uint32_t>(ascii.triangleCount());
    for (int k = 0; k < 4; ++k) s += static_cast<char>((n >> (8 * k)) & 0xFF);
    for (size_t t = 0; t < n; ++t) {
        s += std::string(12, '\0');
        for (int k = 0; k < 3; ++k) {
            const uint32_t vi = ascii.indices[3 * t + static_cast<size_t>(k)];
            for (int c = 0; c < 3; ++c) {
                float x = ascii.positions[3 * vi + static_cast<size_t>(c)];
                uint32_t bits;
                std::memcpy(&bits, &x, 4);
                for (int b = 0; b < 4; ++b) s += static_cast<char>((bits >> (8 * b)) & 0xFF);
            }
        }
        s += std::string(2, '\0');
    }
    return s;
}

std::string base64(const std::string& bytes) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const uint32_t v = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8) |
                           static_cast<unsigned char>(bytes[i + 2]);
        for (int k = 3; k >= 0; --k) out += t[(v >> (6 * k)) & 63];
    }
    if (i < bytes.size()) {
        uint32_t v = static_cast<unsigned char>(bytes[i]) << 16;
        if (i + 1 < bytes.size()) v |= static_cast<unsigned char>(bytes[i + 1]) << 8;
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        out += i + 1 < bytes.size() ? t[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

/// Highest point (mesh Y) of the vertices tagged with surfaces other than the board's own.
double partTop(const Mesh& m) {
    double top = -1e9;
    for (size_t i = 0; i < m.vertexCount(); ++i) {
        const Surface s = static_cast<Surface>(m.surfaces[i]);
        if (s == Surface::Plastic || s == Surface::Tin || s == Surface::Gold || s == Surface::Glass)
            top = std::max(top, static_cast<double>(m.positions[3 * i + 1]));
    }
    return top;
}
}  // namespace

TEST(model3d_readers_vrml_stl_obj) {
    // KiCad-style VRML 2.0: DEF / USE of materials and shapes, transforms, comments and commas.
    const Model3DMesh soic = parseVrml(readModelFixture(kSoicModel), kSoicModel);
    CHECK(soic.format == "vrml" && soic.triangleCount() == 12 * 10);  // body + DEF'd pin + 8 instances
    CHECK(soic.groups.size() == 2 && soic.groups[0].count == 36);
    CHECK(soic.groups[0].r < 0.2f && soic.groups[1].r > 0.8f);
    const auto b = soic.bounds();
    CHECK_NEAR(b[0], -1.1811, 1e-3);
    CHECK_NEAR(b[3], 1.1811, 1e-3);
    CHECK_NEAR(b[5], 0.689, 1e-4);
    CHECK(soic.warnings.empty());

    M3_THROWS(parseVrml("#VRML V1.0 ascii\nSeparator { }"));
    M3_THROWS(parseVrml("#VRML V2.0 utf8\nShape { geometry IndexedFaceSet { coordIndex [ 0 1 2 -1 ]"));
    try {
        parseVrml("#VRML V2.0 utf8\nShape {\n appearance Appearance {\n material Material { diffuseColor 1 1 } }\n"
                  " geometry IndexedFaceSet { coord Coordinate { point [ 0 0 0 } } }");
        CHECK(false);
    } catch (const Model3DError& e) {
        CHECK(std::string(e.what()).find("line 5") != std::string::npos);
    }
    std::string deep = "#VRML V2.0 utf8\n";
    for (int i = 0; i < 400; ++i) deep += "Group { children [ ";
    M3_THROWS(parseVrml(deep));
    // Boxes, PROTOs, ROUTEs, unknown USEs and Inline references are handled or reported.
    const Model3DMesh boxes = parseVrml(
        "#VRML V2.0 utf8\nPROTO Foo [ field SFFloat x 1 ] { Group { } }\n"
        "Transform { translation 1 0 0 children [ Shape { geometry Box { size 2 2 2 } } ] }\n"
        "Shape { geometry Sphere { radius 1 } }\nShape { geometry USE NOWHERE }\nInline { url \"other.wrl\" }\n"
        "DEF T TimeSensor { }\nROUTE T.fraction_changed TO X.set_fraction\n");
    CHECK(boxes.triangleCount() == 12 && boxes.warnings.size() == 4);
    CHECK_NEAR(boxes.bounds()[3], 2.0, 1e-6);
    // Quads and polygons are fanned; bad indices skip the face.
    const Model3DMesh faces = parseVrml(
        "#VRML V2.0 utf8\nShape { geometry IndexedFaceSet { coord Coordinate { point [0 0 0, 1 0 0, 1 1 0, 0 1 0, 0.5 1.5 0] }"
        " coordIndex [0 1 2 4 3 -1, 0 1 9 -1, 0 1 2] } }");
    CHECK(faces.triangleCount() == 4 && faces.warnings.size() == 1);

    // STL: ASCII and binary read to the same welded cube.
    const Model3DMesh a = parseStl(cubeStlAscii(), "cube.stl");
    CHECK(a.triangleCount() == 12 && a.vertexCount() == 8 && a.format == "stl");
    const Model3DMesh bin = parseStl(cubeStlBinary(), "cube.stl");
    CHECK(bin.triangleCount() == 12 && bin.vertexCount() == 8);
    CHECK(bin.positions == a.positions);
    M3_THROWS(parseStl("garbage that is not an stl file"));
    M3_THROWS(parseStl("solid x\nfacet normal 0 0 1\nouter loop\nvertex 0 0\n"));
    {
        std::string huge(84, '\0');
        const uint32_t n = 200001;
        for (int k = 0; k < 4; ++k) huge[80 + static_cast<size_t>(k)] = static_cast<char>((n >> (8 * k)) & 0xFF);
        huge.resize(84 + 50ull * n, '\0');
        M3_THROWS(parseStl(huge));  // over the triangle budget
    }

    // OBJ: quads, slashed and negative indices, materials by name.
    const Model3DMesh obj = parseObj(
        "# cube\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 0 0 1\nv 1 0 1\nv 1 1 1\nv 0 1 1\n"
        "usemtl Body_Black\nf 1 4 3 2\nf 5 6 7 8\nf 1/1/1 2/2/2 6/6/6 5/5/5\nusemtl Pin_Gold\nf -7 -6 -2 -3\nf 3 4 8 7\nf 4 1 5 8\n"
        "f 1 2 99\n");
    CHECK(obj.triangleCount() == 12 && obj.groups.size() == 2);
    CHECK(obj.groups.size() == 2 && obj.groups[0].r < 0.2f && obj.groups[1].r > 0.8f && obj.groups[1].b < 0.4f);
    CHECK(obj.warnings.size() == 2);  // bad face, guessed materials
    M3_THROWS(parseObj("v 1 2\nf 1 2 3\n"));

    // STEP is refused with the reason; unknown types too.
    try {
        parseModel3D("ISO-10303-21;", "part.step");
        CHECK(false);
    } catch (const Model3DError& e) {
        CHECK(std::string(e.what()).find(".wrl") != std::string::npos);
    }
    M3_THROWS(parseModel3D("x", "part.3ds"));
    CHECK(isModel3DFile("A.WRL") && isModel3DFile("b.stp") && !isModel3DFile("c.kicad_mod"));
    CHECK(defaultModelUnit("vrml") == 2.54 && defaultModelUnit("stl") == 1.0);
    CHECK(decodeBase64(base64("hello 3D")) == "hello 3D");
    M3_THROWS(decodeBase64("not*base64"));
}

TEST(model3d_alignment_assembly_and_project_file) {
    Model3DMesh soic = parseVrml(readModelFixture(kSoicModel), kSoicModel);
    const std::string id = Model3DRegistry::instance().add(soic);
    CHECK(id.size() == 17 && id[0] == 'm' && Model3DRegistry::instance().add(soic) == id);  // content-addressed
    Model3DRef ref;
    ref.id = id;
    ref.name = kSoicModel;
    ref.unit = 2.54;
    std::array<double, 6> b{};
    CHECK(model3dAlignedBounds(ref, b));
    CHECK_NEAR(b[3], 3.0, 1e-3);   // pin tips at 3.0 mm
    CHECK_NEAR(b[4], 2.45, 1e-3);  // body length 4.9 mm
    CHECK_NEAR(b[5], 1.75, 1e-3);  // 1.75 mm tall
    ref.rotate = {0, 0, 90};
    CHECK(model3dAlignedBounds(ref, b));
    CHECK_NEAR(b[3], 2.45, 1e-3);  // a quarter turn swaps x and y
    CHECK_NEAR(b[4], 3.0, 1e-3);
    ref.rotate = {0, 0, 0};
    ref.offset = {1, -2, 0.5};
    const Model3DRef seated = model3dSeated(ref);
    CHECK(model3dAlignedBounds(seated, b));
    CHECK_NEAR(b[0] + b[3], 0, 1e-3);
    CHECK_NEAR(b[1] + b[4], 0, 1e-3);
    CHECK_NEAR(b[2], 0, 1e-3);
    Model3DRef unknown;
    unknown.id = "m0000000000000000";
    CHECK(!model3dAlignedBounds(unknown, b));

    // A part with the model: its JSON keeps the alignment, invalid alignments are refused.
    const StandardPart* lm358 = findStandardPart("LM358DR");
    CHECK(lm358 != nullptr);
    if (!lm358) return;
    CustomPartSpec spec = lm358->spec;
    spec.model3d = model3dSeated(ref);
    const CustomPartSpec back = customPartSpecFromJson(Json::parse(customPartSpecToJson(spec).dump()));
    CHECK(back.model3d.id == id && back.model3d.unit == 2.54 && back.model3d.name == kSoicModel);
    CHECK(customPartSpecToJson(lm358->spec).get("model3d").isNull());  // parts without a model keep their ids
    {
        Json bad = customPartSpecToJson(spec);
        bad["model3d"]["scale"] = Json::parse("[1, 0, 1]");
        M3_THROWS(customPartSpecFromJson(bad));
        bad = customPartSpecToJson(spec);
        bad["model3d"]["offset"] = Json::parse("[1, 2]");
        M3_THROWS(customPartSpecFromJson(bad));
        bad["model3d"]["offset"] = Json::parse("[1e9, 0, 0]");
        M3_THROWS(customPartSpecFromJson(bad));
    }

    // In the assembly: the model replaces the generated body, on top and mirrored under the board.
    auto build = [&](const CustomPartSpec& s, bool bottom) {
        Project p;
        const std::string pid = p.addCustomPart(s);
        const int cid = p.schematic.addCustomComponent(pid, "", {0, 0}, 0, "U1");
        Component* c = p.schematic.find(cid);
        c->pcb.placed = true;
        c->pcb.position = {20, 15};
        c->pcb.bottom = bottom;
        c->pcb.rotation = 90;
        return p;
    };
    Project withModel = build(spec, false);
    Project generated = build(lm358->spec, false);
    const Mesh mm = buildAssemblyMesh(withModel.schematic, withModel.pcb);
    const Mesh gm = buildAssemblyMesh(generated.schematic, generated.pcb);
    CHECK(mm.vertexCount() != gm.vertexCount());
    CHECK_NEAR(partTop(mm), 0.035 + 1.75, 1e-3);
    // Rotated a quarter turn: the 4.9 mm body length now runs along board x.
    double xMin = 1e9, xMax = -1e9;
    for (size_t i = 0; i < mm.vertexCount(); ++i)
        if (static_cast<Surface>(mm.surfaces[i]) == Surface::Plastic) {
            xMin = std::min(xMin, static_cast<double>(mm.positions[3 * i]));
            xMax = std::max(xMax, static_cast<double>(mm.positions[3 * i]));
        }
    CHECK_NEAR(xMax - xMin, 4.9, 1e-3);
    CHECK_NEAR((xMax + xMin) / 2, 20, 1e-3);
    const Project under = build(spec, true);
    const Mesh um = buildAssemblyMesh(under.schematic, under.pcb);
    double lowest = 1e9;
    for (size_t i = 0; i < um.vertexCount(); ++i)
        if (static_cast<Surface>(um.surfaces[i]) == Surface::Plastic)
            lowest = std::min(lowest, static_cast<double>(um.positions[3 * i + 1]));
    CHECK_NEAR(lowest, -1.6 - 0.035 - 1.75, 1e-3);
    CHECK(exportStl(mm, "t").find("facet normal") != std::string::npos);

    // Saved with the project and read back; a damaged entry falls back to the generated body.
    const Json saved = withModel.toJson();
    CHECK(saved.get("models3d").size() == 1 && saved.get("models3d")[0].get("id").asString() == id);
    CHECK(generated.toJson().get("models3d").isNull());
    const Project reloaded = Project::fromJson(Json::parse(saved.dump()));
    CHECK(buildAssemblyMesh(reloaded.schematic, reloaded.pcb).vertexCount() == mm.vertexCount());
    Json entry = saved.get("models3d")[0];
    entry["id"] = "m00000000000000ab";  // an id from another build: kept as an alias
    CHECK(model3dFromJson(entry) == "m00000000000000ab" && Model3DRegistry::instance().get("m00000000000000ab"));
    Json broken = entry;
    broken["indices"] = Json::parse("[0, 1, 99999999]");
    M3_THROWS(model3dFromJson(broken));
    Json damaged = saved;
    Json models = Json::array();
    models.push(broken);
    damaged["models3d"] = models;
    M3_NOTHROW(Project::fromJson(damaged));
    CustomPartSpec missing = spec;
    missing.model3d.id = "m0123456789abcdef";  // never registered: the generated body
    Project fallback = build(missing, false);
    CHECK(buildAssemblyMesh(fallback.schematic, fallback.pcb).vertexCount() == gm.vertexCount());

    // The C API entry points.
    CHECK(sieda_c_api_model3d_test() == 0);
}

TEST(library_import_attaches_kicad_3d_models) {
    const std::string soicFp = readFixture(kSoic), header = readFixture(kHeader);
    const ImportedFootprint fp = parseKicadFootprint(soicFp, kSoic);
    CHECK(fp.modelPath.find("SOIC-8_3.9x4.9mm_P1.27mm.wrl") != std::string::npos);
    const ImportedFootprint hd = parseKicadFootprint(header, kHeader);
    CHECK(hd.modelPath.find("PinHeader_1x04") != std::string::npos);
    CHECK(hd.centreY > 3 && hd.centreY < 4.5);  // pin 1 at the origin: centred on the courtyard

    // Footprint + its .wrl: the part gets the model, in KiCad's 0.1 inch units.
    LibraryImport r = importLibraryFiles({{kSoic, soicFp}, {kSoicModel, readModelFixture(kSoicModel)}});
    const ImportedPart* soic = nullptr;
    for (const auto& p : r.parts)
        if (p.footprintName == "SOIC-8_3.9x4.9mm_P1.27mm") soic = &p;
    CHECK(soic != nullptr);
    if (soic) {
        CHECK(!soic->spec.model3d.empty() && soic->spec.model3d.unit == 2.54);
        CHECK(std::any_of(soic->warnings.begin(), soic->warnings.end(),
                          [](const std::string& w) { return w.find("3D model") != std::string::npos; }));
        CHECK(CustomPartRegistry::instance().registerPart(soic->spec) != nullptr);
    }
    CHECK(r.files.size() == 2 && r.files[1].format == "model3d" && r.files[1].error.empty());

    // A footprint whose model was not imported says how to attach it; the pin header's model moves with its pads.
    r = importLibraryFiles({{kHeader, header}});
    CHECK(r.parts.size() == 1 && r.parts[0].spec.model3d.empty());
    CHECK(std::any_of(r.parts[0].warnings.begin(), r.parts[0].warnings.end(),
                      [](const std::string& w) { return w.find(".3dshapes") != std::string::npos; }));
    r = importLibraryFiles({{kHeader, header}, {"PinHeader_1x04_P2.54mm_Vertical.stl", cubeStlAscii()}});
    CHECK(r.parts.size() == 1 && !r.parts[0].spec.model3d.empty());
    if (r.parts.size() == 1) {
        CHECK(r.parts[0].spec.model3d.unit == 1.0);
        CHECK_NEAR(r.parts[0].spec.model3d.offset[1], hd.centreY, 1e-6);  // the model moves with the pads
    }

    // Binary files travel as base64 through the C API's request; STEP files are reported.
    Json req = Json::object();
    Json files = Json::array();
    Json f1 = Json::object();
    f1["name"] = kSoic;
    f1["content"] = soicFp;
    files.push(f1);
    Json f2 = Json::object();
    f2["name"] = "SOIC-8_3.9x4.9mm_P1.27mm.stl";
    f2["contentBase64"] = base64(cubeStlBinary());
    files.push(f2);
    Json f3 = Json::object();
    f3["name"] = "SOIC-8_3.9x4.9mm_P1.27mm.step";
    f3["content"] = "ISO-10303-21;";
    files.push(f3);
    req["files"] = files;
    const Json out = importLibraryRequest(req);
    bool attached = false;
    for (const auto& p : out.get("parts").items())
        attached = attached || p.get("spec").get("model3d").get("unit").asNumber(0) == 1.0;
    CHECK(attached);
    CHECK(out.get("files")[2].get("error").asString().find("STEP") != std::string::npos);
}

TEST(model3d_readers_survive_fuzzing) {
    const std::vector<std::pair<std::string, std::string>> inputs = {
        {kSoicModel, readModelFixture(kSoicModel)},
        {"cube.stl", cubeStlAscii()},
        {"cube.stl", cubeStlBinary()},
        {"cube.obj", "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nusemtl metal\nf 1 2 3 4\nf -1 -2 -3\n"}};
    uint32_t seed = 0x3D3D3D3Du;
    auto rnd = [&]() {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return seed;
    };
    int ok = 0, refused = 0;
    for (const auto& [name, original] : inputs) {
        for (int k = 0; k < 300; ++k) {
            std::string s = original;
            const size_t a = rnd() % (s.size() + 1), len = rnd() % 48;
            switch (k % 6) {
                case 0: s.resize(a); break;
                case 1:
                    for (int f = 0; f < 6 && !s.empty(); ++f) s[rnd() % s.size()] = static_cast<char>(rnd());
                    break;
                case 2: s.erase(a, len); break;
                case 3: s.insert(a, s.substr(a, len)); break;
                case 4: s.insert(a, std::string(len, "{[ -1e30"[rnd() % 8])); break;
                default: s.insert(a, " 1e308 -1e308 nan 4294967295 "); break;
            }
            try {
                const Model3DMesh m = parseModel3D(s, name);
                ++ok;
                CHECK(!m.indices.empty() && m.triangleCount() <= Model3DLimits::maxTriangles);
                for (uint32_t i : m.indices) CHECK(i < m.vertexCount());
                for (float v : m.positions) CHECK(std::isfinite(v));
                uint64_t covered = 0;
                for (const auto& g : m.groups) covered += g.count;
                CHECK(covered == m.indices.size());
                CHECK(!Model3DRegistry::instance().add(m).empty());
            } catch (const Model3DError&) {
                ++refused;
            } catch (const std::exception& e) {
                std::printf("    %s mutation %d: unexpected %s\n", name.c_str(), k, e.what());
                CHECK(false);
            }
        }
    }
    CHECK(ok > 300 && refused > 50);
}

// ------------------------------------------------------------------ Altium libraries (OLE compound files)

#include "sieda/AltiumLibrary.hpp"

namespace {
std::string readAltiumFixture(const std::string& name) {
    std::ifstream f(std::string(SIEDA_FIXTURE_DIR) + "/altium/" + name, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

const ImportedPart* altiumPart(const LibraryImport& r, const std::string& name) {
    for (const auto& p : r.parts)
        if (p.spec.name == name) return &p;
    return nullptr;
}

char sideOf(const CustomPartSpec& s, const std::string& number) {
    for (const auto& p : s.symbol.pins)
        if (p.number == number) return p.side;
    return '?';
}
}  // namespace

TEST(altium_compound_file_reader) {
    const std::string sch = readAltiumFixture("Test.SchLib");
    CHECK(sch.size() > 4096 && CompoundFile::looksLikeCompoundFile(sch));
    const CompoundFile cfb(sch);
    std::set<std::string> names;
    for (int c : cfb.entries()[0].children) names.insert(cfb.entries()[static_cast<size_t>(c)].name);
    CHECK(names == std::set<std::string>({"FileHeader", "Storage", "LM358", "NE555", "Padding"}));
    const int lm = cfb.child(CompoundFile::root(), "lm358");  // case-insensitive
    CHECK(lm > 0 && cfb.child(lm, "DATA") > 0);
    CHECK(cfb.read(cfb.child(cfb.child(CompoundFile::root(), "Padding"), "Blob")).size() == 5000);  // regular sectors
    CHECK(cfb.read(cfb.child(lm, "Data")).size() > 200);                                             // mini stream
    CHECK(altiumLibraryKind(cfb, "x") == "schlib");
    const std::string pcb = readAltiumFixture("Test.PcbLib");
    CHECK(altiumLibraryKind(CompoundFile(pcb), "x") == "pcblib");
    const std::string intlib = readAltiumFixture("Test.IntLib");
    CHECK(altiumLibraryKind(CompoundFile(intlib), "Parts.IntLib") == "intlib");
    CHECK(altiumProperties("|RECORD=2|NAME=A|%UTF8%NAME=\xC3\xA9|TEXT=\xB5" "F").at("NAME") == "\xC3\xA9");
    CHECK(altiumProperties("|TEXT=\xB5" "F").at("TEXT") == "\xC2\xB5" "F");

    // Damaged containers are refused, never read out of bounds or looped over.
    auto refused = [](const std::string& bytes) {
        try {
            CompoundFile c(bytes);
            for (size_t i = 0; i < c.entries().size(); ++i)
                if (c.entries()[i].type == CompoundFile::EntryType::Stream) c.read(static_cast<int>(i));
            return false;
        } catch (const CfbError&) {
            return true;
        }
    };
    CHECK(refused(sch.substr(0, 511)));
    CHECK(refused(std::string("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8) + std::string(600, '\0')));
    std::string truncated = sch.substr(0, sch.size() - 1536);
    CHECK(refused(truncated));
    std::string loop = sch;  // first directory sector's FAT entry points to itself: a chain without end is cut
    const uint32_t firstFat = static_cast<unsigned char>(loop[0x4C]) | (static_cast<unsigned char>(loop[0x4D]) << 8);
    const size_t fatAt = (firstFat + 1) * 512;
    for (size_t i = 0; i < 128; ++i) {
        const uint32_t self = static_cast<uint32_t>(i);
        for (int k = 0; k < 4; ++k) loop[fatAt + 4 * i + static_cast<size_t>(k)] = static_cast<char>((self >> (8 * k)) & 0xFF);
    }
    try {
        CompoundFile c(loop);
        for (size_t i = 0; i < c.entries().size(); ++i)
            if (c.entries()[i].type == CompoundFile::EntryType::Stream) c.read(static_cast<int>(i));
    } catch (const CfbError&) {
    }
    CHECK(true);  // reaching here: no hang, no crash
}

TEST(library_import_reads_altium_libraries) {
    const LibraryImport r = importLibraryFiles(
        {{"Test.SchLib", readAltiumFixture("Test.SchLib")}, {"Test.PcbLib", readAltiumFixture("Test.PcbLib")}});
    CHECK(r.files.size() == 2 && r.files[0].format == "altium" && r.files[0].symbols == 2 && r.files[0].error.empty());
    CHECK(r.files[1].footprints == 3 && r.files[1].error.empty());
    // A multi-part op-amp paired with its current footprint model.
    const ImportedPart* lm = altiumPart(r, "LM358");
    CHECK(lm && lm->ok);
    if (lm && lm->ok) {
        CHECK(lm->footprintName == "SOIC8_TI");
        CHECK(lm->spec.pins.size() == 8 && lm->spec.refPrefix == "U");
        CHECK(lm->spec.manufacturer == "Texas Instruments");
        CHECK(lm->spec.datasheet == "https://www.ti.com/lit/ds/symlink/lm358.pdf");
        CHECK(lm->spec.description == "Dual op-amp, \xC2\xB1" "16 V");  // Windows-1252 ± read as UTF-8
        const int out = lm->spec.pinIndex("1"), vplus = lm->spec.pinIndex("8"), inb = lm->spec.pinIndex("5");
        CHECK(out >= 0 && lm->spec.pins[static_cast<size_t>(out)].type == PinType::Output);
        CHECK(vplus >= 0 && lm->spec.pins[static_cast<size_t>(vplus)].type == PinType::PowerIn);
        CHECK(inb >= 0 && lm->spec.pins[static_cast<size_t>(inb)].name == "+INB");
        CHECK(sideOf(lm->spec, "1") == 'R' && sideOf(lm->spec, "2") == 'L' && sideOf(lm->spec, "8") == 'T' &&
              sideOf(lm->spec, "4") == 'B');
        CHECK(std::any_of(lm->warnings.begin(), lm->warnings.end(),
                          [](const std::string& w) { return w.find("units") != std::string::npos; }));
        CHECK(lm->spec.package.lands.size() == 8);
        // Pad 1 top left (Altium's y up flipped), pad 6 turned 90° back to a horizontal pad, body from the overlay.
        CHECK(lm->spec.package.lands[0].x < 0 && lm->spec.package.lands[0].y < 0);
        CHECK_NEAR(lm->spec.package.lands[5].w, 1.55, 1e-6);
        CHECK_NEAR(lm->spec.package.lands[5].h, 0.6, 1e-6);
        CHECK_NEAR(lm->spec.package.bodySize, 3.9, 1e-6);
        CHECK_NEAR(lm->spec.package.bodyDepth, 4.9, 1e-6);
        CHECK(CustomPartRegistry::instance().registerPart(lm->spec) != nullptr);
    }
    // A single-part symbol: overbar, UTF-8 description, a footprint model not in the import (generated DIP-8).
    const ImportedPart* ne = altiumPart(r, "NE555");
    CHECK(ne && ne->ok);
    if (ne && ne->ok) {
        const int reset = ne->spec.pinIndex("4");
        CHECK(reset >= 0 && ne->spec.pins[static_cast<size_t>(reset)].name == "nRESET");
        CHECK(ne->spec.description == "Precision timer, 4.5\xE2\x80\x93" "16 V");
        CHECK(ne->spec.package.type == "DIP" && ne->spec.pins.size() == 8);
    }
    // Footprints of their own: a through-hole header (mounting hole skipped), and one with an unknown primitive.
    const ImportedPart* hdr = altiumPart(r, "HDR1X4");
    CHECK(hdr && hdr->ok);
    if (hdr && hdr->ok) {
        CHECK(hdr->spec.package.lands.size() == 4 && hdr->spec.package.lands[0].drill > 0.9);
        CHECK(!hdr->spec.package.lands[0].round && hdr->spec.package.lands[1].round);
        CHECK(std::any_of(hdr->warnings.begin(), hdr->warnings.end(),
                          [](const std::string& w) { return w.find("non-plated") != std::string::npos; }));
    }
    const ImportedPart* bad = altiumPart(r, "BAD");
    CHECK(bad && bad->spec.package.lands.size() == 1);
    if (bad)
        CHECK(std::any_of(bad->warnings.begin(), bad->warnings.end(),
                          [](const std::string& w) { return w.find("unknown primitive") != std::string::npos; }));
    // Explicit pairs work with Altium footprints too.
    const LibraryImport paired = importLibraryFiles(
        {{"Test.SchLib", readAltiumFixture("Test.SchLib")}, {"Test.PcbLib", readAltiumFixture("Test.PcbLib")}},
        {{"NE555", "SOIC8_TI"}});
    const ImportedPart* ne2 = altiumPart(paired, "NE555");
    CHECK(ne2 && ne2->ok && ne2->footprintName == "SOIC8_TI");
    // Integrated libraries are refused with what to do instead.
    const LibraryImport il = importLibraryFiles({{"Parts.IntLib", readAltiumFixture("Test.IntLib")}});
    CHECK(il.files.size() == 1 && il.files[0].error.find("Extract") != std::string::npos);

    // Through the C API's request, as base64 (binary files).
    Json req = Json::object();
    Json files = Json::array();
    Json f = Json::object();
    f["name"] = "Test.PcbLib";
    f["contentBase64"] = base64(readAltiumFixture("Test.PcbLib"));
    files.push(f);
    req["files"] = files;
    const Json out = importLibraryRequest(req);
    CHECK(out.get("footprints").asInt() == 3 && out.get("parts").size() == 3);
}

TEST(library_import_altium_survives_fuzzing) {
    const std::vector<std::pair<std::string, std::string>> inputs = {{"Test.SchLib", readAltiumFixture("Test.SchLib")},
                                                                     {"Test.PcbLib", readAltiumFixture("Test.PcbLib")}};
    uint32_t seed = 0xA171u;
    auto rnd = [&]() {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return seed;
    };
    int imported = 0, refused = 0;
    for (const auto& [name, original] : inputs) {
        for (int k = 0; k < 400; ++k) {
            std::string s = original;
            const size_t a = 512 + rnd() % (s.size() - 512), len = 1 + rnd() % 64;
            switch (k % 5) {
                case 0: s.resize(a); break;
                case 1:
                    for (int i = 0; i < 12; ++i) s[rnd() % s.size()] = static_cast<char>(rnd());
                    break;
                case 2:
                    for (int i = 0; i < 4; ++i) s[0x4C + rnd() % 400] = static_cast<char>(rnd());  // header and DIFAT
                    break;
                case 3: s.replace(a, std::min(len, s.size() - a), std::string(std::min(len, s.size() - a), '\xFF')); break;
                default:
                    for (size_t i = a; i < std::min(s.size(), a + len); ++i) s[i] = static_cast<char>(s[i] ^ 0x5A);
                    break;
            }
            try {
                const LibraryImport r = importLibraryFiles({{name, s}});
                for (const auto& p : r.parts) {
                    if (!p.ok) continue;
                    ++imported;
                    CHECK(CustomPartRegistry::instance().registerPart(p.spec) != nullptr);
                }
                refused += !r.files[0].error.empty();
            } catch (const std::exception& e) {
                std::printf("    %s mutation %d threw: %s\n", name.c_str(), k, e.what());
                CHECK(false);
            }
        }
    }
    CHECK(imported > 100 && refused > 20);
}

// ------------------------------------------------------------------ import sheet: choosing a symbol's footprint

TEST(library_import_footprint_candidates_for_the_sheet) {
    std::vector<ImportFile> files;
    for (const char* f : {kSymbols, kSoic, kQfn, kHeader}) files.push_back({f, readFixture(f)});
    const LibraryImport r = importLibraryFiles(files);
    const Json j = libraryImportToJson(r);
    CHECK(j.get("footprintList").size() == 3);
    const Json* lm = nullptr;
    for (const auto& p : j.get("parts").items())
        if (p.get("symbol").asString() == "LM358") lm = &p;
    CHECK(lm != nullptr);
    if (lm) {
        CHECK(lm->get("pairable").asBool());
        const Json& c = lm->get("candidates");
        CHECK(c.size() == 2);  // the SOIC it names first, then the QFN (pads 1…16 cover pins 1…8); not the 4-pin header
        if (c.size() == 2) {
            CHECK(c[0].asString() == "SOIC-8_3.9x4.9mm_P1.27mm");
            CHECK(c[1].asString() == "QFN-16-1EP_3x3mm_P0.5mm_EP1.7x1.7mm");
        }
    }
    // A footprint-only part offers no choice.
    for (const auto& p : j.get("parts").items())
        if (p.get("symbol").asString().empty()) CHECK(p.get("pairable").isNull());
    // Choosing a candidate re-pairs the symbol; a choice that does not fit is reported on the part.
    const LibraryImport repaired = importLibraryFiles(files, {{"AMS1117-3.3", "PinHeader_1x04_P2.54mm_Vertical"},
                                                              {"LM358", "QFN-16-1EP_3x3mm_P0.5mm_EP1.7x1.7mm"}});
    for (const auto& p : repaired.parts) {
        if (p.symbolName == "AMS1117-3.3") CHECK(p.ok && p.footprintName == "PinHeader_1x04_P2.54mm_Vertical");
        if (p.symbolName == "LM358") CHECK(!p.ok && p.error.find("overlap") != std::string::npos);
    }
    // Eagle device sets are paired by their connects: no choice there.
    const Json eagle = libraryImportToJson(importLibraryFiles({{kEagle, readFixture(kEagle)}}));
    for (const auto& p : eagle.get("parts").items()) CHECK(p.get("pairable").isNull());
    CHECK(eagle.get("footprintList").size() == 0);
}

// ------------------------------------------------------------------ catalog growth (StandardCatalogExtra.inc)

TEST(standard_catalog_has_over_a_thousand_registrable_parts) {
    const auto& parts = standardParts();
    CHECK(parts.size() >= 1000);
    std::set<std::string> names;
    std::map<std::string, int> categories;
    int failures = 0;
    for (const auto& sp : parts) {
        CHECK(names.insert(sp.spec.name).second);
        ++categories[sp.category];
        bool ok = !sp.spec.manufacturer.empty() && !sp.spec.description.empty() && !sp.spec.pins.empty();
        try {
            auto part = CustomPartRegistry::instance().registerPart(sp.spec);
            std::set<int> padded;
            for (const auto& pad : part->footprint.pads)
                if (pad.pinIndex >= 0) padded.insert(pad.pinIndex);
            ok = ok && padded.size() == sp.spec.pins.size();  // every pin on a pad
            for (const auto& issue : checkSymbol(sp.spec)) ok = ok && issue.severity != "error";  // arranged symbols
        } catch (const std::exception& e) {
            std::printf("    %s: %s\n", sp.spec.name.c_str(), e.what());
            ok = false;
        }
        if (!ok && ++failures < 10) std::printf("    not usable: %s\n", sp.spec.name.c_str());
    }
    CHECK(failures == 0);
    // Connectors, regulators, MCUs and interface ICs are all represented.
    for (const char* c : {"Connectors", "Regulators", "Interface", "Logic", "Memory", "Data Converters", "Op-Amps"})
        CHECK(categories[c] >= 10);
    const StandardPart* hdr = findStandardPart("PinHeader_2x05_P2.54mm");
    CHECK(hdr && hdr->spec.package.type == "HEADER2" && hdr->spec.pins.size() == 10 && hdr->spec.refPrefix == "J");
    std::printf("    %zu standard parts in %zu categories\n", parts.size(), categories.size());
}
