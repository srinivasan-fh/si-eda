// SiEDA Core — imported 3D models of parts: VRML 2.0 / 97 (.wrl, what KiCad libraries ship next to their STEP
// files), STL (ASCII and binary) and Wavefront OBJ, read into a triangle mesh with per-material colours.
//
// A part refers to its model by id (CustomPartSpec::model3d) with an alignment: file unit, scale, rotation and
// offset. The meshes live in a process-wide registry keyed by a content hash, are saved with the project
// ("models3d") and replace the generated body in the 3D assembly view and in the STL / OBJ exports.
//
// STEP (.step / .stp) is not read: it is a B-rep (NURBS surfaces and trimmed faces) that needs a geometry kernel
// to tessellate. KiCad ships a .wrl next to every .step; import that instead.
//
// Files are untrusted input: size, nesting, vertex and triangle counts and every number are limited, and a file
// that cannot be read throws Model3DError with the line where reading stopped.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <map>
#include <string>
#include <vector>

#include "sieda/CustomParts.hpp"
#include "sieda/Json.hpp"
#include "sieda/Mesh.hpp"

namespace sieda {

struct Model3DError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// A triangle mesh in the file's own units and axes (x right, y away from the viewer, z up, as KiCad models are).
struct Model3DMesh {
    std::string name;    // file name
    std::string format;  // "vrml", "stl", "obj"
    std::vector<float> positions;   // xyz
    std::vector<uint32_t> indices;  // triangles
    struct Group {
        float r = 0.6f, g = 0.6f, b = 0.62f, a = 1.0f;
        uint32_t first = 0, count = 0;  // range of `indices`
    };
    std::vector<Group> groups;  // one per material, covering every index
    std::vector<std::string> warnings;

    size_t vertexCount() const { return positions.size() / 3; }
    size_t triangleCount() const { return indices.size() / 3; }
    /// Axis-aligned bounds {min x, y, z, max x, y, z} (zeros when empty).
    std::array<double, 6> bounds() const;
};

struct Model3DLimits {
    static constexpr size_t maxFile = 32u << 20;    // bytes
    static constexpr size_t maxTriangles = 200000;  // per model (kept in the project file)
    static constexpr size_t maxVertices = 600000;
    static constexpr int maxDepth = 64;             // VRML node nesting
};

Model3DMesh parseVrml(const std::string& text, const std::string& name = {});
Model3DMesh parseStl(const std::string& bytes, const std::string& name = {});
Model3DMesh parseObj(const std::string& text, const std::string& name = {});
/// By extension (.wrl / .vrml, .stl, .obj); .step / .stp and anything else throw with the reason.
Model3DMesh parseModel3D(const std::string& bytes, const std::string& name);
/// True for the file types parseModel3D reads, plus .step / .stp (recognised so they can be refused clearly).
bool isModel3DFile(const std::string& name);

/// Millimetres per file unit a format is usually written in: KiCad VRML uses 0.1 inch (2.54 mm), STL / OBJ mm.
double defaultModelUnit(const std::string& format);

class Model3DRegistry {
public:
    static Model3DRegistry& instance();
    /// Registers a mesh and returns its id ("m" + 16 hex digits of a content hash); identical meshes share an id.
    std::string add(Model3DMesh mesh);
    std::shared_ptr<const Model3DMesh> get(const std::string& id) const;
    /// Makes `alias` refer to the mesh registered as `id` (a project file's id kept for its parts).
    void addAlias(const std::string& alias, const std::string& id);

private:
    Model3DRegistry() = default;
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<const Model3DMesh>> meshes_;
};

/// The 4 × 3 affine transform from file coordinates to the footprint's model frame in mm (x right, y up the
/// footprint i.e. towards −y in the PCB view, z up from the board top): unit × scale, then rotation about x, y and z
/// (degrees, counter-clockwise looking down each axis, applied in that order), then offset (mm).
std::array<double, 12> model3dTransform(const Model3DRef& ref);
/// Bounds of the aligned model in the footprint's model frame (mm) {min x, y, z, max x, y, z}; false if unknown.
bool model3dAlignedBounds(const Model3DRef& ref, std::array<double, 6>& out);
/// The offset that centres the aligned model on the footprint origin (x, y) and seats its lowest point on the board
/// (z = 0), keeping unit, scale and rotation. Unchanged when the model is unknown.
Model3DRef model3dSeated(const Model3DRef& ref);

/// Adds the aligned model of a placed part to `out` (flat-shaded, a surface per material colour); `boardTop` and
/// `boardBottom` are the heights of the outer copper surfaces. False when the model is not registered.
bool appendModel3D(Mesh& out, const Model3DRef& ref, const PcbPlacement& placement, double boardTop, double boardBottom);

/// {"id","name","format","positions":[…],"indices":[…],"groups":[[r,g,b,a,first,count],…]}
Json model3dToJson(const std::string& id, const Model3DMesh& mesh);
/// Reads and registers a mesh saved by model3dToJson (checked like a file); returns its id.
std::string model3dFromJson(const Json& j);
/// The meshes the given custom parts refer to, for the project file.
Json models3dForParts(const std::vector<std::string>& customPartIds);
/// Registers every mesh of a project's "models3d" array (bad entries are skipped).
void registerModels3d(const Json& models);

/// C API: {"name","content" | "contentBase64"} → {"ok","error","id","name","format","unit","vertices","triangles",
/// "bounds":[6],"warnings":[…]} (the mesh is registered).
Json model3dImportRequest(const Json& request);
/// C API: aligned bounds and the seated alignment of a part spec's model: {"ok","error","bounds":[6],"seated":{ref}}.
Json model3dFitRequest(const Json& specJson);

/// Base64 (standard alphabet, padding and white space tolerated); throws Model3DError on other characters.
std::string decodeBase64(const std::string& text);

}  // namespace sieda
