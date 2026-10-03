// SiEDA Core — embedded (buried) passives: resistors formed from a thin-film resistive foil (Ohmega-Ply / TICER
// NiCr / NiP, 25–250 Ω/sq) and capacitors formed from a buried-capacitance laminate between two inner layers.
// They are fabricated inside the board, so they free surface area, shorten loops and are not assembled.
#pragma once

#include "sieda/Geometry.hpp"
#include "sieda/Pcb.hpp"
#include "sieda/Schematic.hpp"

#include <optional>
#include <string>
#include <vector>

namespace sieda {

class Project;

/// Thin-film resistive foil sheet resistances on offer (Ω per square).
const std::vector<double>& embeddedResistorSheets();
/// Buried-capacitance laminate (≈ 8 µm ceramic-filled dielectric): capacitance per area, nF / mm².
constexpr double kBuriedCapacitanceDensity = 0.016;  // 1.6 nF/cm²
/// Power the thin-film element dissipates safely per mm² of foil (conservative, still air).
constexpr double kEmbeddedResistorWattsPerMm2 = 0.1;
/// Untrimmed tolerance of an embedded thin-film resistor (laser trimming reaches ≈ ±2 %).
constexpr double kEmbeddedResistorTolerance = 10.0;

/// Resistors and capacitors can be embedded.
bool canEmbed(const Component& c);

struct EmbeddedElement {
    int componentId = -1;
    bool resistor = true;
    int layer = 1;   // inner copper layer of the element (resistor) or the first plate (capacitor)
    int layer2 = 1;  // second plate of a capacitor (layer + 1); = layer for a resistor
    double value = 0;  // Ω or F
    double sheet = 0;  // Ω/sq (resistor) or nF/mm² (capacitor)
    double squares = 0;
    double width = 0, length = 0;  // resistor foil W × L; capacitor plate side × side (mm)
    double area = 0;               // mm²
    double powerRating = 0;        // W (resistor)
    bool inRange = true;           // value achievable with the materials and a sensible size
    std::string material;
    Vec2 centre;
    int rotation = 0;
    /// Foil / plate outline on the board.
    Rect bounds() const;
};

/// The element a component becomes when it is embedded on its inner layer (nullopt when it is not embedded or
/// the stack-up cannot hold it: fewer than 4 layers or the layer is not inner).
std::optional<EmbeddedElement> embeddedElement(const Component& c, const BoardSettings& s);

/// Pads (terminations / plates) of an embedded part, in place of its footprint pads.
std::vector<Pad> embeddedPads(const Component& c, const EmbeddedElement& e, const Schematic& sch);

/// Every embedded element on the board.
std::vector<EmbeddedElement> embeddedElements(const Schematic& sch, const BoardSettings& s);

/// Gerber of the resistive foil to keep on `layer` (Ohmega / TICER "resistor element" artwork), empty if none.
std::string exportEmbeddedResistorGerber(const Schematic& sch, const PcbLayout& pcb, int layer);

}  // namespace sieda
