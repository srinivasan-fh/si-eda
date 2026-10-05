// SiEDA Core — board stack-up and controlled impedance: laminate materials (FR-4, high-Tg FR-4, Isola 370HR,
// Rogers RO4350B, Panasonic Megtron 6, polyimide, aluminium IMS), IPC-2141 microstrip / stripline / differential
// impedance, and the track geometry that hits a target impedance on each layer of this board.
#pragma once

#include <string>
#include <vector>

#include "sieda/Json.hpp"
#include "sieda/Pcb.hpp"

namespace sieda {

struct LaminateMaterial {
    std::string id;       // "fr4", "rogers-4350b", …
    std::string name;     // "Rogers RO4350B"
    double er = 4.4;      // dielectric constant (design value, ~1–2 GHz)
    double lossTangent = 0.02;
    double tg = 140;      // glass transition, °C
    double thermalConductivity = 0.3;  // W/m·K through the dielectric
    std::string note;     // where it is used
};

const std::vector<LaminateMaterial>& laminateMaterials();
const LaminateMaterial* findLaminate(const std::string& id);
const LaminateMaterial& boardLaminate(const BoardSettings& s);  // the board's material (FR-4 if unknown)

/// IPC-2141 surface microstrip: width w, dielectric height h to the reference plane, copper thickness t (mm).
double microstripImpedance(double w, double h, double t, double er);
/// IPC-2141 symmetric stripline between two planes `b` apart.
double striplineImpedance(double w, double b, double t, double er);
/// Edge-coupled differential impedance from the single-ended Z0 and gap s (IPC-2141 approximations).
double differentialMicrostrip(double z0, double s, double h);
double differentialStripline(double z0, double s, double b);

/// Copper thickness (mm) of the board's layers.
double copperThickness(const BoardSettings& s);
/// Dielectric between neighbouring copper layers (mm), assuming an evenly spaced stack-up.
double layerDielectric(const BoardSettings& s);
/// True when the copper layer is buried between two planes (stripline) rather than on the surface (microstrip).
bool isStriplineLayer(const BoardSettings& s, int layer);
/// Dielectric height the impedance of `layer` is computed with: the height to the nearest plane for a microstrip, the
/// plane-to-plane spacing b for a stripline (mm).
double impedanceReferenceHeight(const BoardSettings& s, int layer);
/// Characteristic impedance of a track of width w on `layer`.
double trackImpedance(const BoardSettings& s, int layer, double w);
/// Dielectric between copper layer `layer` and `layer + 1` (mm): HDI boards have thin laser-drillable build-up
/// layers (0.075 mm) under the outer layers; otherwise the stack is evenly spaced (`layerDielectric`).
double dielectricBelow(const BoardSettings& s, int layer);
/// Depth of a copper layer's top surface below the top of the board (mm).
double layerDepth(const BoardSettings& s, int layer);
/// Depth (mm) of a via barrel through the layers it spans: the board thickness for a through via.
double viaBarrelDepth(const BoardSettings& s, const Via& v);
/// Track width for a target single-ended impedance on `layer` (bisection; 0 when out of range).
double widthForImpedance(const BoardSettings& s, int layer, double ohms);
/// Width and gap of an edge-coupled pair for a target differential impedance on `layer`: the gap is set to the
/// track width (tightly coupled) or the board clearance, whichever is larger.
std::pair<double, double> differentialPairGeometry(const BoardSettings& s, int layer, double ohms);

/// {"material":…, "er":…, "layers":[{name,type,thickness}], "singleEnded":[{layer, width}], "differential":[…]} for
/// the UI and the fab notes.
Json stackupJson(const BoardSettings& s);

}  // namespace sieda
