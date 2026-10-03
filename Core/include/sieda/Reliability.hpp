// SiEDA Core — design-for-reliability rules: leakage (IPC-2221B spacing, conformal coating, guard rings, moisture),
// thermal (hot spots vs FR-4 Tg, via aspect ratio / CTE), signal integrity and EMI (crosstalk, reference planes, RF
// impedance), assembly (tombstoning, solder bridges, acid traps), and the industry-specific risks of motor control,
// robotics, automotive, aerospace and RF boards. Findings explain the failure mechanism and the fix.
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "sieda/Project.hpp"

namespace sieda {

/// Net classes that drive routing and checks.
struct NetClassification {
    std::map<int, int> highImpedance;  // high-impedance net → guard net (the other amplifier input, the follower's
                                       // output, or ground): leakage-sensitive, needs spacing and a guard ring
    std::set<int> fast;                // fast edges (clocks, PWM, data, switching nodes): crosstalk / EMI
    std::set<int> rf;                  // RF nets: controlled 50 Ω impedance
};
NetClassification classifyNets(const Project& project);
/// The same from the schematic alone (`industry` "" = no RF board rule): what the autorouter uses.
NetClassification classifyNets(const Schematic& sch, const std::string& industry);

/// Spacing (mm) leakage-sensitive copper keeps from other nets: 0.5 mm uncoated, 0.25 mm under conformal coating.
double leakageSpacing(const BoardSettings& s);
/// Microstrip width (mm) for `ohms` on an outer layer over the nearest reference plane (IPC-2141, FR-4 εr 4.4).
double microstripWidth(double ohms, const BoardSettings& s);
/// Dielectric height (mm) from an outer layer to its reference plane.
double referencePlaneHeight(const BoardSettings& s);

/// All reliability findings for the project (PCB layout, circuit and industry profile). Errors are real defects;
/// warnings are likely field failures; info is guidance the profile's standards ask for.
std::vector<RuleViolation> reliabilityChecks(const Project& project);

/// Board material, finish, solder alloy, coating and IPC class the industry profile calls for (fab notes / job file).
struct FabricationRequirements {
    std::string material;     // "FR-4, Tg ≥ 170 °C (IPC-4101/126)", "Polyimide (IPC-4101/40)", …
    std::string finish;       // "ENIG", "HASL lead-free", "HASL tin-lead (SnPb)", "Immersion silver"
    std::string solder;       // "SAC305 (lead-free)" or "Sn63Pb37 (tin-lead)"
    int ipcClass = 2;
    std::vector<std::string> notes;  // coating, baking, staking, outgassing, impedance …
};
FabricationRequirements fabricationRequirements(const Project& project);

}  // namespace sieda
