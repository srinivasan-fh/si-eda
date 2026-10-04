// SiEDA Core — 3D assembly mesh generation (board, copper, vias, silkscreen and component bodies).
// Coordinate system: X = board X (mm), Y = up (mm), Z = board Y (mm). Board top surface at Y = 0.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sieda/Geometry.hpp"
#include "sieda/Pcb.hpp"
#include "sieda/Schematic.hpp"

namespace sieda {

struct Rgba {
    float r = 1, g = 1, b = 1, a = 1;
};

/// Look of a solder mask colour in the 3D assembly: the mask over bare laminate, the mask over copper (tracks and
/// pours show through slightly lighter) and the silkscreen ink printed on it (black on yellow and white boards).
struct SolderMaskStyle {
    const char* name;
    Rgba mask;
    Rgba maskOverCopper;
    Rgba silk;
};

/// Mask colours fabs offer, in menu order (green first: the usual choice).
const std::vector<SolderMaskStyle>& solderMaskStyles();
/// nullptr for an unknown name.
const SolderMaskStyle* findSolderMask(const std::string& name);

/// What a triangle is made of, so a renderer can give each its own physical material (gloss, metalness…). The
/// vertex colour stays the base colour for every surface except `Finish`, whose look (ENIG gold, HASL tin, bare
/// copper) is the renderer's choice.
enum class Surface : uint8_t {
    Mask = 0,      // solder mask (glossy lacquer over laminate or copper)
    Laminate = 1,  // FR-4 / core on the board edge, bare underside, mounting-hole ring
    Finish = 2,    // exposed copper with its surface finish: pads, via lands, plated rings
    Gold = 3,      // hard-gold plated pins (headers, edge fingers)
    Tin = 4,       // tinned leads, chip terminations, can tops
    Solder = 5,    // solder joints and fillets
    Silk = 6,      // silkscreen ink
    Plastic = 7,   // moulded epoxy / plastic bodies
    Ceramic = 8,   // chip resistor / capacitor bodies
    Glass = 9,     // LED lenses and other clear bodies
    Hole = 10,     // drilled bores
    Marking = 11,  // laser marking and pin-1 dots on package tops
};
constexpr int kSurfaceCount = 12;

struct Mesh {
    std::vector<float> positions;  // xyz
    std::vector<float> normals;    // xyz
    std::vector<float> colors;     // rgba
    std::vector<uint32_t> indices; // triangles
    std::vector<uint8_t> surfaces; // one `Surface` per vertex
    /// Surface tagged on everything added next.
    Surface surface = Surface::Mask;

    size_t vertexCount() const { return positions.size() / 3; }
    size_t triangleCount() const { return indices.size() / 3; }

    void addBox(Vec3 min, Vec3 max, Rgba color);
    /// Box extruded along a 2D segment in the XZ plane (used for tracks and silkscreen lines).
    void addSegmentBox(Vec2 a, Vec2 b, double width, double y0, double y1, Rgba color);
    /// Vertical (Y axis) cylinder.
    void addCylinder(Vec3 baseCentre, double radius, double height, Rgba color, int segments = 20);
    /// Polygon (board XY) extruded between heights y0 and y1.
    void addPrism(const std::vector<Vec2>& polygon, double y0, double y1, Rgba color);
    /// Stroke-font text lying flat at heights y0…y1, centred on `centre` (board XY), reading along +X with its top
    /// towards board −Y (the top of the PCB view). `mirrored` reads correctly from below (bottom-side legend).
    /// Returns the text's width.
    double addText(const std::string& text, Vec2 centre, double height, double y0, double y1, Rgba color,
                   bool mirrored = false);
    /// Width `addText` would give `text` at `height`.
    static double textWidth(const std::string& text, double height);
    /// Number of vertices tagged with `s`.
    size_t surfaceVertices(Surface s) const;

private:
    void addQuad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, Rgba color);
};

struct MeshOptions {
    bool components = true;
    bool copper = true;
    bool silkscreen = true;
};

Mesh buildAssemblyMesh(const Schematic& sch, const PcbLayout& pcb, const MeshOptions& options = {});

/// Copper of a single layer (tracks, pads, via lands) laid flat at Y = 0 — used by the X-ray stack view.
Mesh buildCopperLayerMesh(const Schematic& sch, const PcbLayout& pcb, int layer);

/// Height (Y) of the underside of copper layer `layer` in the assembled board.
double copperLayerBase(int layer, int layerCount, double thickness, double copper);

}  // namespace sieda
