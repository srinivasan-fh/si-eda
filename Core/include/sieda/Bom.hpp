// SiEDA Core — bill of materials: parts grouped into order lines with descriptions, ratings, sourcing and cost.
#pragma once

#include <string>
#include <vector>

#include "sieda/Json.hpp"
#include "sieda/Schematic.hpp"

namespace sieda {

struct BomLine {
    int item = 0;
    std::vector<std::string> refs;   // naturally sorted: R1, R2, R10
    std::vector<int> componentIds;
    std::string type, value, footprint;
    std::string description;  // "Resistor 10kΩ ±1% 0805"
    std::string rating;       // "0.125 W", "≥ 16 V", "500 mA"
    Sourcing sourcing;        // shared by every part on the line
    std::string suggestedManufacturer, suggestedMpn;  // standard part when one is known (e.g. Yageo RC0805 resistors)
    std::vector<std::string> notes;  // what is still missing or worth checking
    bool embedded = false;           // formed inside the PCB (embedded passive): not bought or assembled
    int quantity() const { return static_cast<int>(refs.size()); }
    double lineCost() const { return sourcing.dnp || embedded ? 0 : sourcing.unitPrice * quantity(); }
};

struct BomSummary {
    int lines = 0, placements = 0, dnp = 0, missingMpn = 0, unpriced = 0, embedded = 0;
    double costPerBoard = 0;
};

/// Parts with a footprint grouped into BOM lines: same type, value, footprint and sourcing. Symbols (ground, net
/// labels, junctions) are never listed.
std::vector<BomLine> buildBom(const Schematic& sch);
BomSummary summarizeBom(const std::vector<BomLine>& lines);
/// {"lines": [...], "summary": {...}, "buildQuantity": n, "orderCost": …} for the UI.
Json bomJson(const Schematic& sch, int buildQuantity);

/// Orderable part number for a standard part, if one follows from the value and footprint (e.g. a 10k 0805
/// resistor → Yageo RC0805FR-0710KL); empty when it does not.
std::pair<std::string, std::string> suggestedPart(const Component& c);

}  // namespace sieda
