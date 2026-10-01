// SiEDA Core — 3D assembly mesh generation (board, copper, vias, silkscreen and component bodies).
// Coordinate system: X = board X (mm), Y = up (mm), Z = board Y (mm). Board top surface at Y = 0.
#pragma once

#include <cstdint>
#include <vector>

#include "sieda/Geometry.hpp"
#include "sieda/Pcb.hpp"
#include "sieda/Schematic.hpp"

namespace sieda {

struct Rgba {
    float r = 1, g = 1, b = 1, a = 1;
};

struct Mesh {
    std::vector<float> positions;  // xyz
    std::vector<float> normals;    // xyz
    std::vector<float> colors;     // rgba
    std::vector<uint32_t> indices; // triangles

    size_t vertexCount() const { return positions.size() / 3; }
    size_t triangleCount() const { return indices.size() / 3; }

    void addBox(Vec3 min, Vec3 max, Rgba color);
    /// Box extruded along a 2D segment in the XZ plane (used for tracks and silkscreen lines).
    void addSegmentBox(Vec2 a, Vec2 b, double width, double y0, double y1, Rgba color);
    /// Vertical (Y axis) cylinder.
    void addCylinder(Vec3 baseCentre, double radius, double height, Rgba color, int segments = 20);
    /// Polygon (board XY) extruded between heights y0 and y1.
    void addPrism(const std::vector<Vec2>& polygon, double y0, double y1, Rgba color);

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
