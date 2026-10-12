// SiEDA Core — built-in standard component library (common ICs, regulators and connectors).
//
// Pinouts follow the manufacturers' datasheets (TI, Microchip, ST). The parts are ordinary custom-part specs,
// so they get generated symbols/footprints and are registered on demand like datasheet imports.
#pragma once

#include <string>
#include <vector>

#include "sieda/CustomParts.hpp"

namespace sieda {

struct StandardPart {
    std::string category;  // "Timers", "Regulators", "Op-Amps", "Microcontrollers · Microchip", "Logic", "Drivers", "Connectors"
    CustomPartSpec spec;
    std::string footprint;  // the KiCad footprint a catalog part's pads follow ("TQFP-32_7x7mm_P0.8mm"); empty: none
};

const std::vector<StandardPart>& standardParts();
/// Case-insensitive lookup by part name (e.g. "ne555"). nullptr if not in the standard library.
const StandardPart* findStandardPart(const std::string& name);

}  // namespace sieda
