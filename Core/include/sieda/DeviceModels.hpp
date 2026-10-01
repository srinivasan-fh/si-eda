// SiEDA Core — part-number device models: simulation parameters and absolute ratings for common discretes.
//
// A component's value selects its model ("SI2302", "1N5819", "2N2222" …, case-insensitive, prefix match), so a
// logic-level power MOSFET simulates and is rated as one instead of as a generic small-signal part.
#pragma once

#include <string>
#include <vector>

#include "sieda/Library.hpp"

namespace sieda {

struct DeviceModel {
    std::string part;         // "SI2302"
    ComponentKind kind;       // Diode, NPN or NMOS
    std::string description;  // "20 V N-MOSFET, logic level, SOT-23"
    // Simulation parameters (diode/BJT: Shockley is, emission n, betaF; MOSFET: square-law vth, kp, lambda).
    double is = 1e-14, emission = 1.0, betaF = 100, vth = 1.5, kp = 0.1, lambda = 0.01;
    // Continuous ratings at 25 °C in free air.
    double maxCurrent = 0.1;  // A (I_F, I_C or I_D)
    double maxPower = 0.25;   // W
    double maxVoltage = 30;   // V (V_R, V_CE or V_DS)
};

const std::vector<DeviceModel>& deviceModels();
/// Model for a component of `kind` whose value names a known part; nullptr for unknown values.
const DeviceModel* findDeviceModel(ComponentKind kind, const std::string& value);

}  // namespace sieda
