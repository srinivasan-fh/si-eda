// SiEDA Core — basic circuit validation (beyond structural ERC).
//
// Checks standard component values, decoupling of IC power pins, and — from a DC operating point —
// part ratings: resistor dissipation, LED/diode/transistor current and power, reverse-biased LEDs,
// supply over-current (likely shorts), op-amp saturation and fuse overload. Exceeding a rating is a
// warning; reaching twice the rating is an error.
#pragma once

#include <string>
#include <vector>

#include "sieda/Schematic.hpp"

namespace sieda {

struct PartRatings {
    double resistorPower = 0.125;  // W, 0805 thick-film
    double ledCurrent = 0.030;     // A, typical 0805 indicator LED absolute maximum
    double diodeCurrent = 0.300;   // A, 1N4148 continuous forward current
    double npnCurrent = 0.100;     // A, BC847 collector current
    double npnPower = 0.250;       // W, SOT-23
    double nmosCurrent = 0.115;    // A, 2N7002 continuous drain current
    double nmosPower = 0.200;      // W, SOT-23
    double supplyCurrent = 3.0;    // A, above this a supply is assumed shorted (USB-C / 1S chargers draw 1–3 A)
    double opampRail = 14.0;       // V, |Vout| beyond this is treated as saturated (±15 V model)
    std::string derating;          // e.g. "Space derating: power 50 %, current 50 % of rating" (added to messages)
    double powerFactor = 1.0;      // derating applied to part-number ratings (DeviceModels)
    double currentFactor = 1.0;
    bool transientStress = true;   // also check steady-state RMS/peak stress when sources are SIN/PULSE
};

std::vector<RuleViolation> validateCircuit(const Schematic& sch, const PartRatings& ratings = {});

}  // namespace sieda
