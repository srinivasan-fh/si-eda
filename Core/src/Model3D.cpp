// SiEDA Core — 3D models of parts: VRML 2.0 / STL / OBJ readers, the mesh registry, alignment and the meshes in
// the project file. See Model3D.hpp.
#include "sieda/Model3D.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
#include <unordered_map>

#include "sieda/Mesh.hpp"

namespace sieda {

namespace {
constexpr double kModelPi = 3.14159265358979323846;
constexpr double kMaxCoordinate = 1e6;  // file units

std::string lowerAscii(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string extensionOf(const std::string& name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || name.find('/', dot) != std::string::npos) return {};
    return lowerAscii(name.substr(dot + 1));
}

/// Groups cover every index; consecutive groups of the same colour merge; empty ones go.
void finishGroups(Model3DMesh& m) {
    std::vector<Model3DMesh::Group> out;
    for (const auto& g : m.groups) {
        if (g.count == 0) continue;
        if (!out.empty() && out.back().r == g.r && out.back().g == g.g && out.back().b == g.b && out.back().a == g.a &&
            out.back().first + out.back().count == g.first) {
            out.back().count += g.count;
            continue;
        }
        out.push_back(g);
    }
    m.groups = out;
    if (m.indices.empty()) throw Model3DError("The model has no triangles.");
}

void checkTriangleBudget(size_t triangles) {
    if (triangles > Model3DLimits::maxTriangles)
        throw Model3DError("The model has more than " + std::to_string(Model3DLimits::maxTriangles) +
                           " triangles: export a simplified model (fewer segments on round shapes).");
}

// ------------------------------------------------------------------ affine 3 × 4 transforms (row-major)

using Affine = std::array<double, 12>;

Affine identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}; }

Affine multiply(const Affine& a, const Affine& b) {  // a ∘ b (b applied first)
    Affine r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j)
            r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j];
        r[i * 4 + 3] = a[i * 4 + 0] * b[3] + a[i * 4 + 1] * b[7] + a[i * 4 + 2] * b[11] + a[i * 4 + 3];
    }
    return r;
}

Affine translation(double x, double y, double z) { return {1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z}; }
Affine scaling(double x, double y, double z) { return {x, 0, 0, 0, 0, y, 0, 0, 0, 0, z, 0}; }

/// Rotation by `angle` radians about the axis (x, y, z) (right-hand rule).
Affine axisRotation(double x, double y, double z, double angle) {
    const double len = std::sqrt(x * x + y * y + z * z);
    if (!(len > 1e-12) || !std::isfinite(angle)) return identity();
    x /= len;
    y /= len;
    z /= len;
    const double c = std::cos(angle), s = std::sin(angle), t = 1 - c;
    return {t * x * x + c,     t * x * y - s * z, t * x * z + s * y, 0,
            t * x * y + s * z, t * y * y + c,     t * y * z - s * x, 0,
            t * x * z - s * y, t * y * z + s * x, t * z * z + c,     0};
}

void apply(const Affine& m, double x, double y, double z, double out[3]) {
    out[0] = m[0] * x + m[1] * y + m[2] * z + m[3];
    out[1] = m[4] * x + m[5] * y + m[6] * z + m[7];
    out[2] = m[8] * x + m[9] * y + m[10] * z + m[11];
}

// ------------------------------------------------------------------ VRML 2.0 reader

struct VNode;
using VNodePtr = std::shared_ptr<VNode>;
struct VField {
    std::vector<double> nums;
    std::vector<VNodePtr> nodes;
    std::vector<std::string> strings;
};
struct VNode {
    std::string type;
    std::map<std::string, VField> fields;
    const VField* field(const char* name) const {
        auto it = fields.find(name);
        return it == fields.end() ? nullptr : &it->second;
    }
    VNodePtr node(const char* name) const {
        const VField* f = field(name);
        return f && !f->nodes.empty() ? f->nodes.front() : nullptr;
    }
    double num(const char* name, size_t i, double def) const {
        const VField* f = field(name);
        return f && f->nums.size() > i && std::isfinite(f->nums[i]) ? f->nums[i] : def;
    }
};

class VrmlReader {
public:
    explicit VrmlReader(const std::string& s) : s_(s) {}

    std::vector<VNodePtr> readScene() {
        std::vector<VNodePtr> roots;
        while (true) {
            skipSpace();
            if (pos_ >= s_.size()) break;
            const std::string word = identifier();
            if (word.empty()) fail("expected a node");
            if (word == "ROUTE") {  // ROUTE a.b TO c.d
                for (int k = 0; k < 3; ++k) identifierOrFail();
                continue;
            }
            if (word == "PROTO" || word == "EXTERNPROTO") {
                skipProto(word == "PROTO");
                ++skippedProtos_;
                continue;
            }
            if (VNodePtr n = nodeAfter(word, 0)) roots.push_back(n);
        }
        return roots;
    }

    int skippedProtos() const { return skippedProtos_; }
    int missingUses() const { return missingUses_; }
    size_t numbersRead() const { return numbers_; }

private:
    [[noreturn]] void fail(const std::string& message) const {
        int line = 1;
        for (size_t i = 0; i < pos_ && i < s_.size(); ++i) line += s_[i] == '\n';
        throw Model3DError("line " + std::to_string(line) + ": " + message);
    }

    void skipSpace() {
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == '#') {
                while (pos_ < s_.size() && s_[pos_] != '\n' && s_[pos_] != '\r') ++pos_;
            } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' || c == '\f' || c == '\v') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    static bool identStart(char c) {
        const unsigned char u = static_cast<unsigned char>(c);
        return u > 0x20 && u != 0x7F && !std::isdigit(u) && std::strchr("\"'#,.[]\\{}+-", c) == nullptr;
    }
    static bool identPart(char c) {
        const unsigned char u = static_cast<unsigned char>(c);
        return u > 0x20 && u != 0x7F && std::strchr("\"'#,[]\\{}", c) == nullptr;
    }

    std::string identifier() {
        skipSpace();
        if (pos_ >= s_.size() || !identStart(s_[pos_])) return {};
        const size_t start = pos_;
        while (pos_ < s_.size() && identPart(s_[pos_])) ++pos_;
        if (pos_ - start > 256) fail("name too long");
        return s_.substr(start, pos_ - start);
    }

    std::string identifierOrFail() {
        std::string w = identifier();
        if (w.empty()) fail("expected a name");
        return w;
    }

    bool atNumber() {
        skipSpace();
        if (pos_ >= s_.size()) return false;
        const char c = s_[pos_];
        if (std::isdigit(static_cast<unsigned char>(c))) return true;
        if ((c == '-' || c == '+' || c == '.') && pos_ + 1 < s_.size()) {
            const char d = s_[pos_ + 1];
            return std::isdigit(static_cast<unsigned char>(d)) || (d == '.' && c != '.');
        }
        return false;
    }

    double number() {
        skipSpace();
        const char* begin = s_.c_str() + pos_;
        char* end = nullptr;
        double v = 0;
        if (pos_ + 1 < s_.size() && s_[pos_] == '0' && (s_[pos_ + 1] == 'x' || s_[pos_ + 1] == 'X')) {
            v = static_cast<double>(std::strtoll(begin, &end, 16));
        } else {
            v = std::strtod(begin, &end);
        }
        if (end == begin) fail("expected a number");
        pos_ += static_cast<size_t>(end - begin);
        if (!std::isfinite(v) || std::fabs(v) > 1e9) fail("number out of range");
        if (++numbers_ > 40000000) fail("too many numbers");
        return v;
    }

    std::string quoted() {
        ++pos_;  // opening quote
        std::string out;
        while (pos_ < s_.size() && s_[pos_] != '"') {
            if (s_[pos_] == '\\' && pos_ + 1 < s_.size()) ++pos_;
            if (out.size() < 4096) out += s_[pos_];
            ++pos_;
        }
        if (pos_ >= s_.size()) fail("string never closed");
        ++pos_;
        return out;
    }

    void expect(char c) {
        skipSpace();
        if (pos_ >= s_.size() || s_[pos_] != c) fail(std::string("expected '") + c + "'");
        ++pos_;
    }

    bool peekChar(char c) {
        skipSpace();
        return pos_ < s_.size() && s_[pos_] == c;
    }

    /// Skips a balanced [ … ] or { … } group (PROTO interfaces and bodies).
    void skipGroup(char open, char close) {
        expect(open);
        int depth = 1;
        while (pos_ < s_.size() && depth > 0) {
            const char c = s_[pos_];
            if (c == '"') {
                quoted();
                continue;
            }
            if (c == '#') {
                skipSpace();
                continue;
            }
            if (c == open) ++depth;
            else if (c == close) --depth;
            ++pos_;
        }
        if (depth != 0) fail(std::string("'") + open + "' never closed");
    }

    void skipProto(bool proto) {
        identifierOrFail();  // name
        skipGroup('[', ']');
        if (proto) skipGroup('{', '}');
        else {
            skipSpace();
            if (peekChar('[')) skipGroup('[', ']');
            else if (peekChar('"')) quoted();
        }
    }

    /// The node after a word already read: DEF name Type { … }, USE name, or Type { … }.
    VNodePtr nodeAfter(const std::string& word, int depth) {
        if (depth > Model3DLimits::maxDepth) fail("nested too deeply");
        if (word == "USE") {
            const std::string name = identifierOrFail();
            auto it = defs_.find(name);
            if (it == defs_.end()) {
                ++missingUses_;
                return nullptr;
            }
            return it->second;
        }
        if (word == "DEF") {
            const std::string name = identifierOrFail();
            VNodePtr n = nodeAfter(identifierOrFail(), depth);
            if (n && defs_.size() < 100000) defs_[name] = n;
            return n;
        }
        if (word == "NULL") return nullptr;
        auto node = std::make_shared<VNode>();
        node->type = word;
        expect('{');
        while (true) {
            skipSpace();
            if (pos_ >= s_.size()) fail("'{' of " + word + " never closed");
            if (s_[pos_] == '}') {
                ++pos_;
                break;
            }
            const std::string field = identifier();
            if (field.empty()) fail("expected a field of " + word);
            if (field == "ROUTE") {
                for (int k = 0; k < 3; ++k) identifierOrFail();
                continue;
            }
            if (field == "PROTO" || field == "EXTERNPROTO") {
                skipProto(field == "PROTO");
                ++skippedProtos_;
                continue;
            }
            // "eventIn SFTime set_x" style interface declarations inside Script nodes: skip the declaration.
            if (field == "eventIn" || field == "eventOut" || field == "field" || field == "exposedField") {
                identifierOrFail();
                identifierOrFail();
                if (field == "field" || field == "exposedField") readValue(node->fields["_"], depth);
                continue;
            }
            readValue(node->fields[field], depth);
        }
        return node;
    }

    void readValue(VField& f, int depth) {
        skipSpace();
        if (pos_ >= s_.size()) fail("unexpected end of file");
        const char c = s_[pos_];
        if (c == '[') {
            ++pos_;
            while (true) {
                skipSpace();
                if (pos_ >= s_.size()) fail("'[' never closed");
                if (s_[pos_] == ']') {
                    ++pos_;
                    break;
                }
                if (atNumber()) f.nums.push_back(number());
                else if (s_[pos_] == '"') f.strings.push_back(quoted());
                else {
                    const std::string w = identifier();
                    if (w.empty()) fail(std::string("unexpected '") + s_[pos_] + "'");
                    if (w == "TRUE" || w == "FALSE") f.nums.push_back(w == "TRUE" ? 1 : 0);
                    else if (VNodePtr n = nodeAfter(w, depth + 1)) f.nodes.push_back(n);
                }
            }
            return;
        }
        if (atNumber()) {
            do f.nums.push_back(number());
            while (atNumber());
            return;
        }
        if (c == '"') {
            f.strings.push_back(quoted());
            return;
        }
        const std::string w = identifier();
        if (w.empty()) fail(std::string("unexpected '") + c + "'");
        if (w == "TRUE" || w == "FALSE") {
            f.nums.push_back(w == "TRUE" ? 1 : 0);
            return;
        }
        if (VNodePtr n = nodeAfter(w, depth + 1)) f.nodes.push_back(n);
    }

    const std::string& s_;
    size_t pos_ = 0;
    std::map<std::string, VNodePtr> defs_;
    int skippedProtos_ = 0, missingUses_ = 0;
    size_t numbers_ = 0;
};

struct VrmlFlattener {
    Model3DMesh& mesh;
    int otherGeometry = 0, badFaces = 0, inlines = 0;

    void walk(const VNodePtr& n, const Affine& m, int depth) {
        if (!n || depth > Model3DLimits::maxDepth) return;
        const std::string& t = n->type;
        if (t == "Transform") {
            const double tx = n->num("translation", 0, 0), ty = n->num("translation", 1, 0), tz = n->num("translation", 2, 0);
            const double cx = n->num("center", 0, 0), cy = n->num("center", 1, 0), cz = n->num("center", 2, 0);
            const Affine r = axisRotation(n->num("rotation", 0, 0), n->num("rotation", 1, 0), n->num("rotation", 2, 1),
                                          n->num("rotation", 3, 0));
            const Affine so = axisRotation(n->num("scaleOrientation", 0, 0), n->num("scaleOrientation", 1, 0),
                                           n->num("scaleOrientation", 2, 1), n->num("scaleOrientation", 3, 0));
            const Affine soInv = axisRotation(n->num("scaleOrientation", 0, 0), n->num("scaleOrientation", 1, 0),
                                              n->num("scaleOrientation", 2, 1), -n->num("scaleOrientation", 3, 0));
            const Affine s = scaling(n->num("scale", 0, 1), n->num("scale", 1, 1), n->num("scale", 2, 1));
            // P' = T × C × R × SR × S × −SR × −C × P
            Affine local = multiply(translation(tx, ty, tz), translation(cx, cy, cz));
            local = multiply(local, r);
            local = multiply(local, so);
            local = multiply(local, s);
            local = multiply(local, soInv);
            local = multiply(local, translation(-cx, -cy, -cz));
            children(n, multiply(m, local), depth);
        } else if (t == "Group" || t == "Anchor" || t == "Billboard" || t == "Collision" || t == "Separator") {
            children(n, m, depth);
        } else if (t == "Switch") {
            const VField* choice = n->field("choice");
            const int which = static_cast<int>(n->num("whichChoice", 0, -1));
            if (choice && which >= 0 && static_cast<size_t>(which) < choice->nodes.size()) walk(choice->nodes[which], m, depth + 1);
        } else if (t == "LOD") {
            const VField* level = n->field("level");
            if (level && !level->nodes.empty()) walk(level->nodes.front(), m, depth + 1);  // the most detailed
        } else if (t == "Shape") {
            shape(*n, m);
        } else if (t == "Inline") {
            ++inlines;
        }
    }

    void children(const VNodePtr& n, const Affine& m, int depth) {
        if (const VField* c = n->field("children"))
            for (const auto& child : c->nodes) walk(child, m, depth + 1);
    }

    void shape(const VNode& n, const Affine& m) {
        Model3DMesh::Group g;
        if (VNodePtr app = n.node("appearance"))
            if (VNodePtr mat = app->node("material")) {
                g.r = static_cast<float>(std::clamp(mat->num("diffuseColor", 0, 0.8), 0.0, 1.0));
                g.g = static_cast<float>(std::clamp(mat->num("diffuseColor", 1, 0.8), 0.0, 1.0));
                g.b = static_cast<float>(std::clamp(mat->num("diffuseColor", 2, 0.8), 0.0, 1.0));
                g.a = static_cast<float>(1.0 - std::clamp(mat->num("transparency", 0, 0), 0.0, 1.0));
            }
        VNodePtr geo = n.node("geometry");
        if (!geo) return;
        g.first = static_cast<uint32_t>(mesh.indices.size());
        if (geo->type == "IndexedFaceSet") faceSet(*geo, m);
        else if (geo->type == "Box") box(*geo, m);
        else ++otherGeometry;
        g.count = static_cast<uint32_t>(mesh.indices.size()) - g.first;
        mesh.groups.push_back(g);
    }

    uint32_t addVertex(const Affine& m, double x, double y, double z) {
        double p[3];
        apply(m, x, y, z, p);
        for (double v : p)
            if (!std::isfinite(v) || std::fabs(v) > kMaxCoordinate) throw Model3DError("A vertex lies outside ±10⁶ units.");
        if (mesh.vertexCount() >= Model3DLimits::maxVertices)
            throw Model3DError("The model has more than " + std::to_string(Model3DLimits::maxVertices) + " vertices.");
        for (double v : p) mesh.positions.push_back(static_cast<float>(v));
        return static_cast<uint32_t>(mesh.vertexCount() - 1);
    }

    void faceSet(const VNode& f, const Affine& m) {
        VNodePtr coord = f.node("coord");
        const VField* pts = coord ? coord->field("point") : nullptr;
        const VField* idx = f.field("coordIndex");
        if (!pts || !idx) return;
        const size_t count = pts->nums.size() / 3;
        const uint32_t base = static_cast<uint32_t>(mesh.vertexCount());
        for (size_t i = 0; i < count; ++i) addVertex(m, pts->nums[3 * i], pts->nums[3 * i + 1], pts->nums[3 * i + 2]);
        const bool ccw = f.num("ccw", 0, 1) != 0;
        // A mirroring transform turns the faces inside out: keep them facing out.
        const double det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) +
                           m[2] * (m[4] * m[9] - m[5] * m[8]);
        const bool flip = (det < 0) != !ccw;
        std::vector<uint32_t> face;
        auto emit = [&]() {
            if (face.size() >= 3) {
                for (size_t k = 1; k + 1 < face.size(); ++k) {
                    if (face[0] == face[k] || face[k] == face[k + 1] || face[0] == face[k + 1]) continue;
                    mesh.indices.push_back(face[0]);
                    mesh.indices.push_back(flip ? face[k + 1] : face[k]);
                    mesh.indices.push_back(flip ? face[k] : face[k + 1]);
                }
                checkTriangleBudget(mesh.triangleCount());
            }
            face.clear();
        };
        bool bad = false;
        for (double v : idx->nums) {
            if (v < 0) {
                if (!bad) emit();
                else {
                    ++badFaces;
                    face.clear();
                }
                bad = false;
                continue;
            }
            if (v >= static_cast<double>(count) || v != std::floor(v)) bad = true;
            else if (face.size() < 1024) face.push_back(base + static_cast<uint32_t>(v));
        }
        if (!bad) emit();
        else ++badFaces;
    }

    void box(const VNode& b, const Affine& m) {
        const double x = b.num("size", 0, 2) / 2, y = b.num("size", 1, 2) / 2, z = b.num("size", 2, 2) / 2;
        uint32_t v[8];
        for (int i = 0; i < 8; ++i) v[i] = addVertex(m, i & 1 ? x : -x, i & 2 ? y : -y, i & 4 ? z : -z);
        static const int faces[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
        for (const auto& f : faces) {
            for (int k : {0, 1, 2, 0, 2, 3}) mesh.indices.push_back(v[f[k]]);
        }
    }
};

// ------------------------------------------------------------------ STL / OBJ helpers

/// Shares identical vertices (STL repeats each vertex in every triangle).
struct VertexWelder {
    Model3DMesh& mesh;
    struct Key {
        uint32_t a, b, c;
        bool operator==(const Key& o) const { return a == o.a && b == o.b && c == o.c; }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const { return (k.a * 73856093u) ^ (k.b * 19349663u) ^ (k.c * 83492791u); }
    };
    std::unordered_map<Key, uint32_t, KeyHash> seen;

    uint32_t add(float x, float y, float z) {
        for (float v : {x, y, z})
            if (!std::isfinite(v) || std::fabs(v) > kMaxCoordinate) throw Model3DError("A vertex lies outside ±10⁶ units.");
        if (x == 0) x = 0;  // −0 and +0 are one vertex
        if (y == 0) y = 0;
        if (z == 0) z = 0;
        Key k{};
        std::memcpy(&k.a, &x, 4);
        std::memcpy(&k.b, &y, 4);
        std::memcpy(&k.c, &z, 4);
        auto it = seen.find(k);
        if (it != seen.end()) return it->second;
        if (mesh.vertexCount() >= Model3DLimits::maxVertices)
            throw Model3DError("The model has more than " + std::to_string(Model3DLimits::maxVertices) + " vertices.");
        const uint32_t index = static_cast<uint32_t>(mesh.vertexCount());
        mesh.positions.push_back(x);
        mesh.positions.push_back(y);
        mesh.positions.push_back(z);
        seen.emplace(k, index);
        return index;
    }

    void triangle(uint32_t a, uint32_t b, uint32_t c) {
        if (a == b || b == c || a == c) return;  // degenerate
        mesh.indices.push_back(a);
        mesh.indices.push_back(b);
        mesh.indices.push_back(c);
        checkTriangleBudget(mesh.triangleCount());
    }
};

uint32_t readU32(const std::string& s, size_t at) {
    uint32_t v = 0;
    for (int k = 3; k >= 0; --k) v = (v << 8) | static_cast<unsigned char>(s[at + static_cast<size_t>(k)]);
    return v;
}

float readF32(const std::string& s, size_t at) {
    const uint32_t bits = readU32(s, at);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

/// Colour of an OBJ material from its name ("black", "pin_metal", "gold"…): no .mtl files are read.
Model3DMesh::Group objMaterialColour(const std::string& rawName) {
    const std::string n = lowerAscii(rawName);
    Model3DMesh::Group g;
    auto has = [&](const char* k) { return n.find(k) != std::string::npos; };
    if (has("gold")) g = {0.86f, 0.70f, 0.30f, 1.0f, 0, 0};
    else if (has("pin") || has("metal") || has("silver") || has("tin") || has("steel") || has("lead"))
        g = {0.80f, 0.80f, 0.82f, 1.0f, 0, 0};
    else if (has("black") || has("body") || has("plastic") || has("epoxy")) g = {0.12f, 0.12f, 0.13f, 1.0f, 0, 0};
    else if (has("white")) g = {0.92f, 0.92f, 0.90f, 1.0f, 0, 0};
    else if (has("red")) g = {0.80f, 0.12f, 0.10f, 1.0f, 0, 0};
    else if (has("green")) g = {0.15f, 0.65f, 0.25f, 1.0f, 0, 0};
    else if (has("blue")) g = {0.15f, 0.30f, 0.80f, 1.0f, 0, 0};
    else if (has("glass") || has("clear") || has("lens")) g = {0.85f, 0.90f, 0.95f, 0.5f, 0, 0};
    return g;
}

uint64_t fnv1a(const void* data, size_t n, uint64_t h) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

std::string meshId(const Model3DMesh& m) {
    uint64_t h = 1469598103934665603ULL;
    h = fnv1a(m.format.data(), m.format.size(), h);
    h = fnv1a(m.positions.data(), m.positions.size() * sizeof(float), h);
    h = fnv1a(m.indices.data(), m.indices.size() * sizeof(uint32_t), h);
    for (const auto& g : m.groups) {
        const float c[4] = {g.r, g.g, g.b, g.a};
        h = fnv1a(c, sizeof c, h);
        h = fnv1a(&g.first, sizeof g.first, h);
        h = fnv1a(&g.count, sizeof g.count, h);
    }
    char buf[20];
    std::snprintf(buf, sizeof buf, "m%016llx", static_cast<unsigned long long>(h));
    return buf;
}

bool validMeshId(const std::string& id) {
    if (id.size() != 17 || id[0] != 'm') return false;
    return std::all_of(id.begin() + 1, id.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
}

/// Indices in range, groups covering every index, limits kept.
void validate(const Model3DMesh& m) {
    if (m.positions.size() % 3 || m.indices.size() % 3) throw Model3DError("The mesh data is incomplete.");
    if (m.vertexCount() > Model3DLimits::maxVertices) throw Model3DError("The mesh has too many vertices.");
    checkTriangleBudget(m.triangleCount());
    if (m.indices.empty()) throw Model3DError("The model has no triangles.");
    for (uint32_t i : m.indices)
        if (i >= m.vertexCount()) throw Model3DError("A triangle refers to a vertex that does not exist.");
    for (float v : m.positions)
        if (!std::isfinite(v) || std::fabs(v) > kMaxCoordinate) throw Model3DError("A vertex lies outside ±10⁶ units.");
    uint64_t covered = 0;
    for (const auto& g : m.groups) {
        if (g.first != covered || static_cast<uint64_t>(g.first) + g.count > m.indices.size())
            throw Model3DError("The mesh's material groups do not cover its triangles.");
        covered += g.count;
    }
    if (covered != m.indices.size()) throw Model3DError("The mesh's material groups do not cover its triangles.");
}
}  // namespace

std::array<double, 6> Model3DMesh::bounds() const {
    std::array<double, 6> b{};
    if (positions.empty()) return b;
    b = {1e300, 1e300, 1e300, -1e300, -1e300, -1e300};
    for (size_t i = 0; i + 2 < positions.size(); i += 3)
        for (int k = 0; k < 3; ++k) {
            b[k] = std::min(b[k], static_cast<double>(positions[i + k]));
            b[k + 3] = std::max(b[k + 3], static_cast<double>(positions[i + k]));
        }
    return b;
}

Model3DMesh parseVrml(const std::string& text, const std::string& name) {
    if (text.size() > Model3DLimits::maxFile) throw Model3DError("The file is larger than 32 MB.");
    const size_t start = text.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    if (start != std::string::npos && text.compare(start, 10, "#VRML V1.0") == 0)
        throw Model3DError("VRML 1.0 is not supported: save the model as VRML 2.0 (VRML97), as KiCad does.");
    if (start == std::string::npos || text.compare(start, 10, "#VRML V2.0") != 0) {
        // Headerless files are accepted when they parse; binary files are not.
        for (size_t i = 0; i < std::min<size_t>(text.size(), 512); ++i)
            if (text[i] == '\0') throw Model3DError("This is not a VRML text file.");
    }
    Model3DMesh mesh;
    mesh.name = name;
    mesh.format = "vrml";
    VrmlReader reader(text);
    const auto roots = reader.readScene();
    VrmlFlattener flat{mesh};
    for (const auto& r : roots) flat.walk(r, identity(), 0);
    if (flat.otherGeometry) mesh.warnings.push_back(std::to_string(flat.otherGeometry) + " shape(s) other than face sets or boxes skipped");
    if (flat.badFaces) mesh.warnings.push_back(std::to_string(flat.badFaces) + " face(s) with bad vertex indices skipped");
    if (flat.inlines) mesh.warnings.push_back(std::to_string(flat.inlines) + " Inline reference(s) to other files not followed");
    if (reader.skippedProtos()) mesh.warnings.push_back(std::to_string(reader.skippedProtos()) + " PROTO definition(s) skipped");
    if (reader.missingUses()) mesh.warnings.push_back(std::to_string(reader.missingUses()) + " USE of an undefined name skipped");
    finishGroups(mesh);
    return mesh;
}

Model3DMesh parseStl(const std::string& s, const std::string& name) {
    if (s.size() > Model3DLimits::maxFile) throw Model3DError("The file is larger than 32 MB.");
    Model3DMesh mesh;
    mesh.name = name;
    mesh.format = "stl";
    VertexWelder weld{mesh, {}};
    const bool binarySize = s.size() >= 84 && 84 + 50ULL * readU32(s, 80) == s.size();
    const size_t start = s.find_first_not_of(" \t\r\n");
    const bool asciiStart = start != std::string::npos && lowerAscii(s.substr(start, 5)) == "solid";
    if (binarySize) {
        // Binary STL; files whose 80-byte header happens to start with "solid" are binary when the size matches.
        const uint32_t n = readU32(s, 80);
        checkTriangleBudget(n);
        for (uint32_t i = 0; i < n; ++i) {
            const size_t at = 84 + 50ULL * i + 12;
            uint32_t v[3];
            for (int k = 0; k < 3; ++k)
                v[k] = weld.add(readF32(s, at + 12 * static_cast<size_t>(k)), readF32(s, at + 12 * static_cast<size_t>(k) + 4),
                                readF32(s, at + 12 * static_cast<size_t>(k) + 8));
            weld.triangle(v[0], v[1], v[2]);
        }
    } else if (asciiStart) {
        size_t pos = 0, line = 0;
        std::vector<uint32_t> loop;
        bool inLoop = false;
        while (pos < s.size()) {
            size_t end = s.find('\n', pos);
            if (end == std::string::npos) end = s.size();
            ++line;
            const char* p = s.c_str() + pos;
            const char* stop = s.c_str() + end;
            pos = end + 1;
            while (p < stop && (*p == ' ' || *p == '\t' || *p == '\r')) ++p;
            auto starts = [&](const char* w) {
                const size_t n = std::strlen(w);
                if (static_cast<size_t>(stop - p) < n) return false;
                for (size_t k = 0; k < n; ++k)
                    if (std::tolower(static_cast<unsigned char>(p[k])) != w[k]) return false;
                return true;
            };
            if (starts("vertex")) {
                if (!inLoop) throw Model3DError("line " + std::to_string(line) + ": vertex outside 'outer loop'");
                const char* q = p + 6;
                double v[3];
                for (double& c : v) {
                    char* e = nullptr;
                    c = std::strtod(q, &e);
                    if (e == q || e > stop) throw Model3DError("line " + std::to_string(line) + ": expected three numbers");
                    q = e;
                }
                if (loop.size() >= 64) throw Model3DError("line " + std::to_string(line) + ": facet has too many vertices");
                loop.push_back(weld.add(static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])));
            } else if (starts("outer")) {
                inLoop = true;
                loop.clear();
            } else if (starts("endloop")) {
                for (size_t k = 1; k + 1 < loop.size(); ++k) weld.triangle(loop[0], loop[k], loop[k + 1]);
                inLoop = false;
                loop.clear();
            }
        }
        if (inLoop) throw Model3DError("line " + std::to_string(line) + ": 'outer loop' never closed");
    } else {
        throw Model3DError("This is not an STL file (no 'solid' header, and the size does not match a binary STL).");
    }
    mesh.groups.push_back({0.62f, 0.63f, 0.66f, 1.0f, 0, static_cast<uint32_t>(mesh.indices.size())});
    finishGroups(mesh);
    return mesh;
}

Model3DMesh parseObj(const std::string& s, const std::string& name) {
    if (s.size() > Model3DLimits::maxFile) throw Model3DError("The file is larger than 32 MB.");
    for (size_t i = 0; i < std::min<size_t>(s.size(), 512); ++i)
        if (s[i] == '\0') throw Model3DError("This is not an OBJ text file.");
    Model3DMesh mesh;
    mesh.name = name;
    mesh.format = "obj";
    Model3DMesh::Group current;
    current.r = 0.62f;
    current.g = 0.63f;
    current.b = 0.66f;
    current.first = 0;
    size_t pos = 0, line = 0;
    int badFaces = 0;
    bool mtl = false;
    std::vector<uint32_t> face;
    while (pos < s.size()) {
        size_t end = s.find('\n', pos);
        if (end == std::string::npos) end = s.size();
        ++line;
        const char* p = s.c_str() + pos;
        const char* stop = s.c_str() + end;
        pos = end + 1;
        while (p < stop && (*p == ' ' || *p == '\t')) ++p;
        if (stop - p < 2) continue;
        auto fail = [&](const std::string& m) { throw Model3DError("line " + std::to_string(line) + ": " + m); };
        if (p[0] == 'v' && (p[1] == ' ' || p[1] == '\t')) {
            const char* q = p + 2;
            double v[3];
            for (double& c : v) {
                char* e = nullptr;
                c = std::strtod(q, &e);
                if (e == q || e > stop) fail("expected three numbers");
                q = e;
            }
            for (double c : v)
                if (!std::isfinite(c) || std::fabs(c) > kMaxCoordinate) fail("vertex outside ±10⁶ units");
            if (mesh.vertexCount() >= Model3DLimits::maxVertices)
                throw Model3DError("The model has more than " + std::to_string(Model3DLimits::maxVertices) + " vertices.");
            for (double c : v) mesh.positions.push_back(static_cast<float>(c));
        } else if (p[0] == 'f' && (p[1] == ' ' || p[1] == '\t')) {
            face.clear();
            const char* q = p + 2;
            bool bad = false;
            while (q < stop) {
                while (q < stop && (*q == ' ' || *q == '\t' || *q == '\r')) ++q;
                if (q >= stop) break;
                char* e = nullptr;
                const long long idx = std::strtoll(q, &e, 10);
                if (e == q) {
                    bad = true;
                    break;
                }
                q = e;
                while (q < stop && *q != ' ' && *q != '\t' && *q != '\r') ++q;  // "/vt/vn"
                const long long n = static_cast<long long>(mesh.vertexCount());
                const long long resolved = idx < 0 ? n + idx : idx - 1;  // 1-based, or relative to the end
                if (idx == 0 || resolved < 0 || resolved >= n) {
                    bad = true;
                    break;
                }
                if (face.size() < 1024) face.push_back(static_cast<uint32_t>(resolved));
            }
            if (bad || face.size() < 3) {
                ++badFaces;
                continue;
            }
            for (size_t k = 1; k + 1 < face.size(); ++k) {
                if (face[0] == face[k] || face[k] == face[k + 1] || face[0] == face[k + 1]) continue;
                mesh.indices.push_back(face[0]);
                mesh.indices.push_back(face[k]);
                mesh.indices.push_back(face[k + 1]);
            }
            checkTriangleBudget(mesh.triangleCount());
        } else if (stop - p > 7 && std::strncmp(p, "usemtl", 6) == 0) {
            current.count = static_cast<uint32_t>(mesh.indices.size()) - current.first;
            mesh.groups.push_back(current);
            std::string material(p + 6, stop);
            current = objMaterialColour(material);
            current.first = static_cast<uint32_t>(mesh.indices.size());
            mtl = true;
        }
    }
    current.count = static_cast<uint32_t>(mesh.indices.size()) - current.first;
    mesh.groups.push_back(current);
    if (badFaces) mesh.warnings.push_back(std::to_string(badFaces) + " face(s) with bad vertex indices skipped");
    if (mtl) mesh.warnings.push_back("Material colours guessed from their names (.mtl files are not read)");
    finishGroups(mesh);
    return mesh;
}

bool isModel3DFile(const std::string& name) {
    const std::string e = extensionOf(name);
    return e == "wrl" || e == "vrml" || e == "stl" || e == "obj" || e == "step" || e == "stp";
}

Model3DMesh parseModel3D(const std::string& bytes, const std::string& name) {
    const std::string e = extensionOf(name);
    if (e == "wrl" || e == "vrml") return parseVrml(bytes, name);
    if (e == "stl") return parseStl(bytes, name);
    if (e == "obj") return parseObj(bytes, name);
    if (e == "step" || e == "stp")
        throw Model3DError("STEP models are not supported (they need a CAD kernel to tessellate). Use the .wrl file "
                           "KiCad ships next to it, or export VRML, STL or OBJ from your CAD tool.");
    throw Model3DError("Not a 3D model SiEDA reads (.wrl, .stl, .obj).");
}

double defaultModelUnit(const std::string& format) { return format == "vrml" ? 2.54 : 1.0; }

// ------------------------------------------------------------------ registry

Model3DRegistry& Model3DRegistry::instance() {
    static Model3DRegistry r;
    return r;
}

std::string Model3DRegistry::add(Model3DMesh mesh) {
    validate(mesh);
    const std::string id = meshId(mesh);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!meshes_.count(id)) meshes_[id] = std::make_shared<const Model3DMesh>(std::move(mesh));
    return id;
}

std::shared_ptr<const Model3DMesh> Model3DRegistry::get(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = meshes_.find(id);
    return it == meshes_.end() ? nullptr : it->second;
}

// ------------------------------------------------------------------ alignment

std::array<double, 12> model3dTransform(const Model3DRef& r) {
    const double deg = kModelPi / 180;
    Affine m = scaling(r.unit * r.scale[0], r.unit * r.scale[1], r.unit * r.scale[2]);
    m = multiply(axisRotation(1, 0, 0, r.rotate[0] * deg), m);
    m = multiply(axisRotation(0, 1, 0, r.rotate[1] * deg), m);
    m = multiply(axisRotation(0, 0, 1, r.rotate[2] * deg), m);
    return multiply(translation(r.offset[0], r.offset[1], r.offset[2]), m);
}

bool model3dAlignedBounds(const Model3DRef& ref, std::array<double, 6>& out) {
    if (ref.empty()) return false;
    auto mesh = Model3DRegistry::instance().get(ref.id);
    if (!mesh || mesh->positions.empty()) return false;
    const Affine m = model3dTransform(ref);
    out = {1e300, 1e300, 1e300, -1e300, -1e300, -1e300};
    for (size_t i = 0; i + 2 < mesh->positions.size(); i += 3) {
        double p[3];
        apply(m, mesh->positions[i], mesh->positions[i + 1], mesh->positions[i + 2], p);
        for (int k = 0; k < 3; ++k) {
            out[k] = std::min(out[k], p[k]);
            out[k + 3] = std::max(out[k + 3], p[k]);
        }
    }
    return true;
}

Model3DRef model3dSeated(const Model3DRef& ref) {
    std::array<double, 6> b{};
    if (!model3dAlignedBounds(ref, b)) return ref;
    Model3DRef out = ref;
    out.offset[0] -= (b[0] + b[3]) / 2;
    out.offset[1] -= (b[1] + b[4]) / 2;
    out.offset[2] -= b[2];
    for (auto& v : out.offset) v = std::clamp(std::round(v * 1e4) / 1e4, -200.0, 200.0);
    return out;
}

// ------------------------------------------------------------------ the assembly mesh

namespace {
Surface surfaceFor(const Model3DMesh::Group& g) {
    const float hi = std::max({g.r, g.g, g.b}), lo = std::min({g.r, g.g, g.b});
    const float mean = (g.r + g.g + g.b) / 3;
    if (g.a < 0.9f) return Surface::Glass;
    if (g.r > 0.6f && g.g > 0.45f && g.b < 0.4f && g.r > g.b + 0.3f) return Surface::Gold;
    if (hi - lo < 0.08f && mean > 0.55f) return Surface::Tin;  // bright grey: plated leads and tabs
    return Surface::Plastic;
}
}  // namespace

bool appendModel3D(Mesh& out, const Model3DRef& ref, const PcbPlacement& pl, double boardTop, double boardBottom) {
    if (ref.empty()) return false;
    auto mesh = Model3DRegistry::instance().get(ref.id);
    if (!mesh) return false;
    const Affine m = model3dTransform(ref);
    // Model frame (x right, y up the footprint, z up) → board XY (y down) and height; bottom-side parts are mirrored
    // through the board.
    std::vector<double> world(mesh->positions.size());
    for (size_t i = 0; i + 2 < mesh->positions.size(); i += 3) {
        double p[3];
        apply(m, mesh->positions[i], mesh->positions[i + 1], mesh->positions[i + 2], p);
        Vec2 f{p[0], -p[1]};
        if (pl.bottom) f.x = -f.x;
        const Vec2 w = pl.position + rotate90(f, pl.rotation);
        world[i] = w.x;
        world[i + 1] = pl.bottom ? boardBottom - p[2] : boardTop + p[2];
        world[i + 2] = w.y;
    }
    for (const auto& g : mesh->groups) {
        const Surface surface = surfaceFor(g);
        for (uint32_t t = g.first; t + 2 < g.first + g.count && t + 2 < mesh->indices.size(); t += 3) {
            const double* a = &world[3 * mesh->indices[t]];
            const double* b = &world[3 * mesh->indices[t + 1]];
            const double* c = &world[3 * mesh->indices[t + 2]];
            double n[3] = {(b[1] - a[1]) * (c[2] - a[2]) - (b[2] - a[2]) * (c[1] - a[1]),
                           (b[2] - a[2]) * (c[0] - a[0]) - (b[0] - a[0]) * (c[2] - a[2]),
                           (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])};
            const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (!(len > 1e-18)) continue;  // degenerate after scaling
            const uint32_t base = static_cast<uint32_t>(out.vertexCount());
            for (const double* v : {a, b, c}) {
                for (int k = 0; k < 3; ++k) {
                    out.positions.push_back(static_cast<float>(v[k]));
                    out.normals.push_back(static_cast<float>(n[k] / len));
                }
                for (float col : {g.r, g.g, g.b, g.a}) out.colors.push_back(col);
                out.surfaces.push_back(static_cast<uint8_t>(surface));
            }
            for (uint32_t k = 0; k < 3; ++k) out.indices.push_back(base + k);
        }
    }
    return true;
}

// ------------------------------------------------------------------ project file

Json model3dToJson(const std::string& id, const Model3DMesh& mesh) {
    Json j = Json::object();
    j["id"] = id;
    j["name"] = mesh.name;
    j["format"] = mesh.format;
    Json pos = Json::array();
    for (float v : mesh.positions) pos.push(static_cast<double>(v));
    j["positions"] = pos;
    Json idx = Json::array();
    for (uint32_t i : mesh.indices) idx.push(static_cast<double>(i));
    j["indices"] = idx;
    Json groups = Json::array();
    for (const auto& g : mesh.groups) {
        Json gj = Json::array();
        for (double v : {static_cast<double>(g.r), static_cast<double>(g.g), static_cast<double>(g.b), static_cast<double>(g.a),
                         static_cast<double>(g.first), static_cast<double>(g.count)})
            gj.push(v);
        groups.push(gj);
    }
    j["groups"] = groups;
    return j;
}

std::string model3dFromJson(const Json& j) {
    if (!j.isObject()) throw Model3DError("A 3D model entry must be an object.");
    Model3DMesh m;
    m.name = j.get("name").asString("");
    if (m.name.size() > 255) m.name.resize(255);
    m.format = j.get("format").asString("");
    if (m.format != "vrml" && m.format != "stl" && m.format != "obj") throw Model3DError("Unknown 3D model format.");
    const auto& pos = j.get("positions").items();
    const auto& idx = j.get("indices").items();
    if (pos.size() > 3 * Model3DLimits::maxVertices || idx.size() > 3 * Model3DLimits::maxTriangles)
        throw Model3DError("The 3D model is too large.");
    m.positions.reserve(pos.size());
    for (const auto& v : pos) {
        if (!v.isNumber()) throw Model3DError("Bad 3D model vertex.");
        m.positions.push_back(static_cast<float>(v.asNumber()));
    }
    m.indices.reserve(idx.size());
    for (const auto& v : idx) {
        const double d = v.asNumber(-1);
        if (!(d >= 0) || d > 4e9 || d != std::floor(d)) throw Model3DError("Bad 3D model index.");
        m.indices.push_back(static_cast<uint32_t>(d));
    }
    for (const auto& g : j.get("groups").items()) {
        if (!g.isArray() || g.size() != 6) throw Model3DError("Bad 3D model material group.");
        Model3DMesh::Group gr;
        float* c[4] = {&gr.r, &gr.g, &gr.b, &gr.a};
        for (size_t k = 0; k < 4; ++k) *c[k] = static_cast<float>(std::clamp(g[k].asNumber(0.6), 0.0, 1.0));
        const double first = g[4].asNumber(-1), cnt = g[5].asNumber(-1);
        if (!(first >= 0 && cnt >= 0 && first + cnt <= static_cast<double>(m.indices.size())))
            throw Model3DError("Bad 3D model material group.");
        gr.first = static_cast<uint32_t>(first);
        gr.count = static_cast<uint32_t>(cnt);
        m.groups.push_back(gr);
        if (m.groups.size() > 100000) throw Model3DError("Too many 3D model material groups.");
    }
    validate(m);
    // Keep the saved id (the parts refer to it) when it is well formed; else derive it from the content.
    const std::string saved = j.get("id").asString("");
    if (!validMeshId(saved)) return Model3DRegistry::instance().add(std::move(m));
    const std::string derived = meshId(m);
    if (derived == saved) return Model3DRegistry::instance().add(std::move(m));
    // A float printed and read back differently: register under the saved id so the parts still find it.
    Model3DRegistry::instance().add(m);
    Model3DRegistry::instance().addAlias(saved, derived);
    return saved;
}

void Model3DRegistry::addAlias(const std::string& alias, const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = meshes_.find(id);
    if (it != meshes_.end() && !meshes_.count(alias)) meshes_[alias] = it->second;
}

Json models3dForParts(const std::vector<std::string>& customPartIds) {
    Json out = Json::array();
    std::set<std::string> done;
    for (const auto& pid : customPartIds) {
        const CustomPart* part = CustomPartRegistry::instance().find(pid);
        if (!part || part->spec.model3d.empty() || !done.insert(part->spec.model3d.id).second) continue;
        if (auto mesh = Model3DRegistry::instance().get(part->spec.model3d.id))
            out.push(model3dToJson(part->spec.model3d.id, *mesh));
    }
    return out;
}

void registerModels3d(const Json& models) {
    for (const auto& j : models.items()) {
        try {
            model3dFromJson(j);
        } catch (const std::exception&) {
            // A damaged model falls back to the generated body; the rest of the project still loads.
        }
    }
}

// ------------------------------------------------------------------ C API

std::string decodeBase64(const std::string& text) {
    std::string out;
    out.reserve(text.size() * 3 / 4);
    uint32_t buffer = 0;
    int bits = 0;
    for (char ch : text) {
        int v;
        if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
        else if (ch == '+' || ch == '-') v = 62;
        else if (ch == '/' || ch == '_') v = 63;
        else if (ch == '=' || ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') continue;
        else throw Model3DError("Invalid base64 data.");
        buffer = (buffer << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buffer >> bits) & 0xFF);
        }
    }
    return out;
}

Json model3dImportRequest(const Json& request) {
    Json out = Json::object();
    out["ok"] = false;
    out["error"] = "";
    try {
        const std::string name = request.get("name").asString("");
        if (name.empty()) throw Model3DError("The 3D model needs a file name.");
        std::string bytes;
        if (request.get("contentBase64").isString()) {
            if (request.get("contentBase64").asString().size() > Model3DLimits::maxFile / 3 * 4 + 16)
                throw Model3DError("The file is larger than 32 MB.");
            bytes = decodeBase64(request.get("contentBase64").asString());
        } else {
            bytes = request.get("content").asString("");
        }
        Model3DMesh mesh = parseModel3D(bytes, name);
        const auto b = mesh.bounds();
        const std::string format = mesh.format;
        const size_t vertices = mesh.vertexCount(), triangles = mesh.triangleCount();
        Json warnings = Json::array();
        for (const auto& w : mesh.warnings) warnings.push(w);
        const std::string id = Model3DRegistry::instance().add(std::move(mesh));
        out["ok"] = true;
        out["id"] = id;
        out["name"] = name;
        out["format"] = format;
        out["unit"] = defaultModelUnit(format);
        out["vertices"] = vertices;
        out["triangles"] = triangles;
        Json bj = Json::array();
        for (double v : b) bj.push(v);
        out["bounds"] = bj;
        out["warnings"] = warnings;
    } catch (const std::exception& e) {
        out["error"] = e.what();
    }
    return out;
}

Json model3dFitRequest(const Json& specJson) {
    Json out = Json::object();
    out["ok"] = false;
    out["error"] = "";
    try {
        const CustomPartSpec spec = customPartSpecFromJson(specJson);
        std::array<double, 6> b{};
        if (spec.model3d.empty()) throw Model3DError("The part has no 3D model.");
        if (!model3dAlignedBounds(spec.model3d, b)) throw Model3DError("The 3D model is not loaded: choose the file again.");
        Json bj = Json::array();
        for (double v : b) bj.push(v);
        out["bounds"] = bj;
        CustomPartSpec seated = spec;
        seated.model3d = model3dSeated(spec.model3d);
        out["seated"] = customPartSpecToJson(seated).get("model3d");
        out["ok"] = true;
    } catch (const std::exception& e) {
        out["error"] = e.what();
    }
    return out;
}

}  // namespace sieda
