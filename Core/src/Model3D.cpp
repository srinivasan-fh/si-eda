// SiEDA Core — 3D models of parts: VRML 2.0 / STL / OBJ / STEP readers, the mesh registry, alignment and the meshes
// in the project file. See Model3D.hpp.
#include "sieda/Model3D.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
#include <unordered_map>

#include "sieda/Geometry.hpp"
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
    /// Nodes left to visit: a USE repeats its node, so a few lines nesting USEs can mean 2^40 visits.
    long long visitsLeft = 1000000;

    void walk(const VNodePtr& n, const Affine& m, int depth) {
        if (!n || depth > Model3DLimits::maxDepth || --visitsLeft < 0) return;
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

// ------------------------------------------------------------------ STEP (ISO 10303-21, AP203 / AP214) reader
//
// The exchange structure is parsed into entity instances (`#n = NAME(args);`, complex entities `(A(…) B(…))`),
// then the shape representations are walked: assembly placements (REPRESENTATION_RELATIONSHIP_WITH_TRANSFORMATION,
// MAPPED_ITEM), solids and shells, faces. Edges are sampled along their curves (lines, circles, ellipses, B-splines
// by de Boor, polylines), each face's boundary loops are mapped to the surface's parameters (planes, cylinders,
// cones, spheres, tori, surfaces of revolution; anything else to its best-fit plane) and triangulated there by ear
// clipping with the holes bridged in, so neighbouring faces share their boundary samples and closed shells stay
// closed. Full periodic faces without a seam (two circles on a cylinder, a whole sphere) are handled specially.

struct SVal {
    enum Kind : uint8_t { Null, Num, Str, Enum, Ref, List, Typed } kind = Null;
    double num = 0;          // Num, or the instance id of a Ref
    std::string str;         // Str, Enum, the name of a Typed value
    std::vector<SVal> list;  // List, the arguments of a Typed value
    int ref() const { return kind == Ref ? static_cast<int>(num) : 0; }
    const SVal& at(size_t i) const {
        static const SVal none;
        return i < list.size() ? list[i] : none;
    }
    bool isTrue() const { return kind == Enum && str == "T"; }
    double number(double def) const { return kind == Num ? num : kind == Typed && !list.empty() ? list[0].number(def) : def; }
};

struct SPart {  // one entity of a (complex) instance
    std::string type;
    std::vector<SVal> args;
};
using SEntity = std::vector<SPart>;
using SEntities = std::unordered_map<int, SEntity>;

constexpr size_t kStepMaxEntities = 2000000;
constexpr int kStepMaxNesting = 64;
constexpr size_t kStepMaxLoopPoints = 4096;  // boundary samples per face

class StepParser {
public:
    explicit StepParser(const std::string& s) : s_(s) {}

    SEntities parse() {
        SEntities out;
        bool data = false;
        while (true) {
            skipSpace();
            if (pos_ >= s_.size()) break;
            if (s_[pos_] == '#' && data) {
                instance(out);
                continue;
            }
            const std::string word = keyword();
            if (word.empty()) fail("unexpected character");
            if (word == "END-ISO-10303-21") break;
            data = word == "DATA";
            skipSpace();
            if (pos_ < s_.size() && s_[pos_] == '(') (void)value(0);  // header entities, DATA('…')
            expect(';');
        }
        return out;
    }

private:
    [[noreturn]] void fail(const std::string& message) const {
        int line = 1;
        for (size_t i = 0; i < pos_ && i < s_.size(); ++i) line += s_[i] == '\n';
        throw Model3DError("line " + std::to_string(line) + ": " + message);
    }

    void skipSpace() {
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == '/' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '*') {
                const size_t end = s_.find("*/", pos_ + 2);
                if (end == std::string::npos) fail("comment never closed");
                pos_ = end + 2;
            } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    void expect(char c) {
        skipSpace();
        if (pos_ >= s_.size() || s_[pos_] != c) fail(std::string("expected '") + c + "'");
        ++pos_;
    }

    static bool keywordChar(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '!';
    }

    std::string keyword() {
        skipSpace();
        const size_t start = pos_;
        while (pos_ < s_.size() && keywordChar(s_[pos_])) ++pos_;
        if (pos_ - start > 128) fail("name too long");
        return s_.substr(start, pos_ - start);
    }

    double number() {
        char* end = nullptr;
        const double v = std::strtod(s_.c_str() + pos_, &end);
        if (end == s_.c_str() + pos_ || !std::isfinite(v)) fail("bad number");
        pos_ = static_cast<size_t>(end - s_.c_str());
        return v;
    }

    int integer() {
        skipSpace();
        if (pos_ >= s_.size() || !std::isdigit(static_cast<unsigned char>(s_[pos_]))) fail("expected an instance number");
        long long v = 0;
        while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) {
            v = v * 10 + (s_[pos_++] - '0');
            if (v > 2000000000LL) fail("instance number too large");
        }
        return static_cast<int>(v);
    }

    void instance(SEntities& out) {
        ++pos_;  // '#'
        const int id = integer();
        expect('=');
        skipSpace();
        SEntity e;
        if (pos_ < s_.size() && s_[pos_] == '(') {  // complex entity: the parts in alphabetical order
            ++pos_;
            while (true) {
                skipSpace();
                if (pos_ < s_.size() && s_[pos_] == ')') break;
                e.push_back(part());
            }
            ++pos_;
        } else {
            e.push_back(part());
        }
        expect(';');
        if (out.size() >= kStepMaxEntities) fail("too many entity instances");
        out[id] = std::move(e);
    }

    SPart part() {
        SPart p;
        p.type = keyword();
        if (p.type.empty()) fail("expected an entity name");
        expect('(');
        p.args = listBody(1);
        return p;
    }

    std::vector<SVal> listBody(int depth) {  // after '(' up to and including ')'
        if (depth > kStepMaxNesting) fail("nested too deeply");
        std::vector<SVal> v;
        skipSpace();
        if (pos_ < s_.size() && s_[pos_] == ')') {
            ++pos_;
            return v;
        }
        while (true) {
            v.push_back(value(depth));
            skipSpace();
            if (pos_ < s_.size() && s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            expect(')');
            return v;
        }
    }

    SVal value(int depth) {
        skipSpace();
        if (pos_ >= s_.size()) fail("unexpected end of file");
        const char c = s_[pos_];
        SVal v;
        if (c == '(') {
            ++pos_;
            v.kind = SVal::List;
            v.list = listBody(depth + 1);
        } else if (c == '#') {
            ++pos_;
            v.kind = SVal::Ref;
            v.num = integer();
        } else if (c == '\'') {
            v.kind = SVal::Str;
            ++pos_;
            while (true) {
                const size_t q = s_.find('\'', pos_);
                if (q == std::string::npos) fail("string never closed");
                v.str.append(s_, pos_, q - pos_);
                pos_ = q + 1;
                if (pos_ < s_.size() && s_[pos_] == '\'') {  // '' is one quote
                    v.str += '\'';
                    ++pos_;
                } else {
                    break;
                }
            }
        } else if (c == '.') {
            v.kind = SVal::Enum;
            const size_t q = s_.find('.', pos_ + 1);
            if (q == std::string::npos || q - pos_ > 64) fail("bad enumeration");
            v.str = s_.substr(pos_ + 1, q - pos_ - 1);
            pos_ = q + 1;
        } else if (c == '$' || c == '*') {
            ++pos_;
        } else if (c == '"') {  // binary
            const size_t q = s_.find('"', pos_ + 1);
            if (q == std::string::npos) fail("binary never closed");
            pos_ = q + 1;
        } else if (std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+') {
            v.kind = SVal::Num;
            v.num = number();
        } else {
            v.kind = SVal::Typed;
            v.str = keyword();
            if (v.str.empty()) fail("unexpected character");
            expect('(');
            v.list = listBody(depth + 1);
        }
        return v;
    }

    const std::string& s_;
    size_t pos_ = 0;
};

double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

/// A placement: origin and right-handed unit axes.
struct Frame {
    Vec3 o, x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1};
    Affine affine() const { return {x.x, y.x, z.x, o.x, x.y, y.y, z.y, o.y, x.z, y.z, z.z, o.z}; }
    Affine inverse() const {  // rigid
        return {x.x, x.y, x.z, -dot(x, o), y.x, y.y, y.z, -dot(y, o), z.x, z.y, z.z, -dot(z, o)};
    }
    void orthonormalise() {
        if (z.length() < 1e-12) z = {0, 0, 1};
        z = z.normalized();
        x = x - z * dot(x, z);
        if (x.length() < 1e-9) x = std::fabs(z.z) < 0.9 ? Vec3{0, 0, 1}.cross(z) : Vec3{1, 0, 0}.cross(z);
        x = x.normalized();
        y = z.cross(x);
    }
};

struct BSplineCurve {
    int degree = 0;
    std::vector<Vec3> cp;
    std::vector<double> w, knots;  // knots expanded by multiplicity
    bool ok = false;

    Vec3 at(double t) const {  // de Boor (rational in homogeneous coordinates)
        const int n = static_cast<int>(cp.size());
        t = std::clamp(t, knots[static_cast<size_t>(degree)], knots[static_cast<size_t>(n)]);
        int k = degree;
        while (k + 1 < n && t >= knots[static_cast<size_t>(k) + 1]) ++k;
        std::vector<std::array<double, 4>> d(static_cast<size_t>(degree) + 1);
        for (int j = 0; j <= degree; ++j) {
            const size_t i = static_cast<size_t>(k - degree + j);
            d[static_cast<size_t>(j)] = {cp[i].x * w[i], cp[i].y * w[i], cp[i].z * w[i], w[i]};
        }
        for (int r = 1; r <= degree; ++r)
            for (int j = degree; j >= r; --j) {
                const double lo = knots[static_cast<size_t>(j + k - degree)], hi = knots[static_cast<size_t>(j + 1 + k - r)];
                const double a = hi > lo ? (t - lo) / (hi - lo) : 0;
                for (int c = 0; c < 4; ++c)
                    d[static_cast<size_t>(j)][static_cast<size_t>(c)] =
                        (1 - a) * d[static_cast<size_t>(j) - 1][static_cast<size_t>(c)] + a * d[static_cast<size_t>(j)][static_cast<size_t>(c)];
            }
        const auto& p = d[static_cast<size_t>(degree)];
        const double wd = std::fabs(p[3]) > 1e-12 ? p[3] : 1;
        return {p[0] / wd, p[1] / wd, p[2] / wd};
    }
};

using P2 = std::array<double, 2>;

/// Ear clipping of a counter-clockwise polygon given as indices into `pts`; always yields n − 2 triangles. The
/// fattest valid ear goes first (a seam-unwrapped band of two collinear rows is then zigzagged, not fanned, which
/// matters because points collinear here need not be collinear in space), a diagonal through another vertex is
/// not valid, and when no ear is found the flattest corner goes; past a work budget the rest is fanned.
void earClip(const std::vector<P2>& pts, std::vector<int> poly, std::vector<std::array<int, 3>>& out) {
    auto at = [&](int i) -> const P2& { return pts[static_cast<size_t>(i)]; };
    auto cross = [&](int a, int b, int c) {
        return (at(b)[0] - at(a)[0]) * (at(c)[1] - at(a)[1]) - (at(b)[1] - at(a)[1]) * (at(c)[0] - at(a)[0]);
    };
    auto len2 = [&](int a, int b) { return (at(b)[0] - at(a)[0]) * (at(b)[0] - at(a)[0]) + (at(b)[1] - at(a)[1]) * (at(b)[1] - at(a)[1]); };
    auto valid = [&](int a, int b, int c, const std::vector<int>& ring) {
        const double diag = len2(a, c);
        for (int p : ring) {
            if (p == a || p == b || p == c) continue;
            if (cross(a, b, p) > 0 && cross(b, c, p) > 0 && cross(c, a, p) > 0) return false;
            const double along = (at(p)[0] - at(a)[0]) * (at(c)[0] - at(a)[0]) + (at(p)[1] - at(a)[1]) * (at(c)[1] - at(a)[1]);
            if (std::fabs(cross(a, c, p)) <= 1e-9 * diag && along > 1e-9 * diag && along < diag * (1 - 1e-9)) return false;
        }
        return true;
    };
    long long budget = 20000000;
    std::vector<double> quality;
    while (poly.size() > 3) {
        const size_t n = poly.size();
        quality.assign(n, -1);
        size_t flattest = 0;
        double flat = 1e300;
        for (size_t i = 0; i < n; ++i) {
            const int a = poly[(i + n - 1) % n], b = poly[i], c = poly[(i + 1) % n];
            const double area = cross(a, b, c);
            if (std::fabs(area) < flat) {
                flat = std::fabs(area);
                flattest = i;
            }
            if (area > 0) quality[i] = area / (len2(a, b) + len2(b, c) + len2(c, a));
        }
        size_t best = n;
        while (budget > 0) {
            size_t i = n;
            for (size_t k = 0; k < n; ++k)
                if (quality[k] > 0 && (i == n || quality[k] > quality[i])) i = k;
            if (i == n) break;
            budget -= static_cast<long long>(n);
            if (valid(poly[(i + n - 1) % n], poly[i], poly[(i + 1) % n], poly)) {
                best = i;
                break;
            }
            quality[i] = -1;
        }
        if (budget <= 0) {
            for (size_t k = 1; k + 1 < n; ++k) out.push_back({poly[0], poly[k], poly[k + 1]});
            return;
        }
        if (best == n) best = flattest;
        out.push_back({poly[(best + n - 1) % n], poly[best], poly[(best + 1) % n]});
        poly.erase(poly.begin() + static_cast<std::ptrdiff_t>(best));
    }
    if (poly.size() == 3) out.push_back({poly[0], poly[1], poly[2]});
}

/// Joins each hole (clockwise) to the outer polygon (counter-clockwise) by a bridge to a visible vertex.
void bridgeHoles(const std::vector<P2>& pts, std::vector<int>& outer, std::vector<std::vector<int>> holes) {
    auto side = [&](const P2& a, const P2& b, const P2& p) { return (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0]); };
    auto crosses = [&](int a, int b, int c, int d) {  // proper intersection of segments ab and cd
        if (a == c || a == d || b == c || b == d) return false;
        const P2 &pa = pts[static_cast<size_t>(a)], &pb = pts[static_cast<size_t>(b)], &pc = pts[static_cast<size_t>(c)], &pd = pts[static_cast<size_t>(d)];
        return side(pa, pb, pc) * side(pa, pb, pd) < 0 && side(pc, pd, pa) * side(pc, pd, pb) < 0;
    };
    while (!holes.empty()) {
        std::vector<int> hole = std::move(holes.back());
        holes.pop_back();
        size_t h = 0;
        for (size_t i = 1; i < hole.size(); ++i)
            if (pts[static_cast<size_t>(hole[i])][0] > pts[static_cast<size_t>(hole[h])][0]) h = i;
        const P2& hp = pts[static_cast<size_t>(hole[h])];
        std::vector<std::pair<double, size_t>> candidates;
        for (size_t i = 0; i < outer.size(); ++i) {
            const P2& p = pts[static_cast<size_t>(outer[i])];
            candidates.push_back({(p[0] - hp[0]) * (p[0] - hp[0]) + (p[1] - hp[1]) * (p[1] - hp[1]), i});
        }
        std::sort(candidates.begin(), candidates.end());
        size_t chosen = candidates.front().second;
        for (size_t k = 0; k < candidates.size() && k < 16; ++k) {
            const int c = outer[candidates[k].second];
            bool visible = true;
            auto check = [&](const std::vector<int>& loop) {
                for (size_t i = 0; i < loop.size() && visible; ++i)
                    visible = !crosses(hole[h], c, loop[i], loop[(i + 1) % loop.size()]);
            };
            check(outer);
            check(hole);
            for (const auto& other : holes) check(other);
            if (visible) {
                chosen = candidates[k].second;
                break;
            }
        }
        std::vector<int> bridge;
        for (size_t i = 0; i <= hole.size(); ++i) bridge.push_back(hole[(h + i) % hole.size()]);
        bridge.push_back(outer[chosen]);
        outer.insert(outer.begin() + static_cast<std::ptrdiff_t>(chosen) + 1, bridge.begin(), bridge.end());
    }
}

double signedArea(const std::vector<P2>& pts, const std::vector<int>& poly) {
    double a = 0;
    for (size_t i = 0; i < poly.size(); ++i) {
        const P2 &p = pts[static_cast<size_t>(poly[i])], &q = pts[static_cast<size_t>(poly[(i + 1) % poly.size()])];
        a += p[0] * q[1] - q[0] * p[1];
    }
    return a / 2;
}

class StepModel {
public:
    StepModel(SEntities entities, Model3DMesh& mesh) : e_(std::move(entities)), mesh_(mesh), weld_{mesh, {}} {}

    void run() {
        readColours();
        const double unit = lengthUnitMm();
        const Affine base = scaling(unit, unit, unit);
        // Assembly structure: shape representations, their relationships (child placed in parent) and maps. The
        // parent of a relationship is found through the product structure: CONTEXT_DEPENDENT_SHAPE_REPRESENTATION
        // (rel, PRODUCT_DEFINITION_SHAPE(…, NAUO(…, parent, child))) and SHAPE_DEFINITION_REPRESENTATION
        // (PRODUCT_DEFINITION_SHAPE(…, parent), rep); without it rep_2 is taken as the parent.
        std::unordered_map<int, int> repOfDef, parentDefOfRel;
        for (int s : idsOf("SHAPE_DEFINITION_REPRESENTATION")) {
            const SPart* sdr = first(s);
            const SPart* shape = sdr->args.size() > 1 ? part(sdr->args[0].ref(), "PRODUCT_DEFINITION_SHAPE") : nullptr;
            if (shape && shape->args.size() > 2) repOfDef[shape->args[2].ref()] = sdr->args[1].ref();
        }
        for (int c : idsOf("CONTEXT_DEPENDENT_SHAPE_REPRESENTATION")) {
            const SPart* cd = first(c);
            const SPart* pds = cd->args.size() > 1 ? part(cd->args[1].ref(), "PRODUCT_DEFINITION_SHAPE") : nullptr;
            const SPart* nauo = pds && pds->args.size() > 2 ? first(pds->args[2].ref()) : nullptr;
            if (nauo && nauo->args.size() > 4) parentDefOfRel[cd->args[0].ref()] = nauo->args[3].ref();
        }
        std::set<int> reps, children;
        for (const auto& [id, ent] : e_)
            for (const auto& p : ent) {
                if (p.type == "SHAPE_REPRESENTATION" || (p.type.size() > 21 && p.type.compare(p.type.size() - 21, 21, "_SHAPE_REPRESENTATION") == 0))
                    reps.insert(id);
                if (p.type == "REPRESENTATION_RELATIONSHIP" || p.type == "SHAPE_REPRESENTATION_RELATIONSHIP") {
                    const int r1 = p.args.size() > 3 ? p.args[2].ref() : 0, r2 = p.args.size() > 3 ? p.args[3].ref() : 0;
                    if (!r1 || !r2 || r1 == r2) continue;
                    int parent = r2;
                    if (auto d = parentDefOfRel.find(id); d != parentDefOfRel.end())
                        if (auto r = repOfDef.find(d->second); r != repOfDef.end() && (r->second == r1 || r->second == r2)) parent = r->second;
                    const int child = parent == r1 ? r2 : r1;
                    Affine m = identity();
                    if (const SPart* t = part(id, "REPRESENTATION_RELATIONSHIP_WITH_TRANSFORMATION"))
                        if (const SPart* idt = part(t->args.empty() ? 0 : t->args[0].ref(), "ITEM_DEFINED_TRANSFORMATION"))
                            if (idt->args.size() > 3) m = multiply(placement(idt->args[3].ref()), placementInverse(idt->args[2].ref()));
                    links_[parent].push_back({child, m});
                    children.insert(child);
                }
                if (p.type == "REPRESENTATION_MAP" && p.args.size() > 1) children.insert(p.args[1].ref());
            }
        for (int r : reps)
            if (!children.count(r)) representation(r, base, 0);
        // Solids and shells no representation refers to (a bare file) are read as they are.
        for (const char* type : {"MANIFOLD_SOLID_BREP", "BREP_WITH_VOIDS", "FACETED_BREP", "SHELL_BASED_SURFACE_MODEL", "CLOSED_SHELL", "OPEN_SHELL"})
            for (int id : idsOf(type))
                if (!visited_.count(id)) item(id, base, 0);
        endGroup();
        if (!unitFound_) mesh_.warnings.push_back("No length unit in the file: millimetres assumed");
        auto count = [&](int n, const std::string& what) {
            if (n) mesh_.warnings.push_back(std::to_string(n) + " " + what);
        };
        count(splineFaces_, "face(s) on B-spline surfaces triangulated from their boundary");
        count(otherFaces_, "face(s) on unsupported surfaces (" + join(otherSurfaces_) + ") use their boundary polygon");
        count(fannedFaces_, "face(s) could not be triangulated and were fanned");
        count(otherEdges_, "edge(s) on unsupported curves (" + join(otherCurves_) + ") drawn straight");
        count(badFaces_, "face(s) with missing geometry skipped");
    }

private:
    struct Link {
        int child;
        Affine m;
    };

    static std::string join(const std::set<std::string>& s) {
        std::string out;
        for (const auto& t : s) out += (out.empty() ? "" : ", ") + t;
        return out;
    }

    const SEntity* entity(int id) const {
        auto it = e_.find(id);
        return it == e_.end() ? nullptr : &it->second;
    }
    /// The part of an instance with that entity type (a simple instance has one part).
    const SPart* part(int id, const char* type) const {
        const SEntity* e = entity(id);
        if (!e) return nullptr;
        for (const auto& p : *e)
            if (p.type == type) return &p;
        return nullptr;
    }
    const SPart* first(int id) const {
        const SEntity* e = entity(id);
        return e && !e->empty() ? &e->front() : nullptr;
    }
    bool is(int id, const char* type) const { return part(id, type) != nullptr; }
    std::vector<int> idsOf(const char* type) const {
        std::vector<int> ids;
        for (const auto& [id, ent] : e_)
            for (const auto& p : ent)
                if (p.type == type) ids.push_back(id);
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    // ---- geometry entities

    bool vec(const SVal& list, Vec3& out) const {
        if (list.kind != SVal::List || list.list.size() < 2) return false;
        out = {list.at(0).number(0), list.at(1).number(0), list.at(2).number(0)};
        return true;
    }
    bool point(int id, Vec3& out) const {
        const SPart* p = part(id, "CARTESIAN_POINT");
        return p && p->args.size() > 1 && vec(p->args[1], out);
    }
    Vec3 direction(int id, Vec3 def) const {
        const SPart* p = part(id, "DIRECTION");
        Vec3 d;
        return p && p->args.size() > 1 && vec(p->args[1], d) && d.length() > 1e-12 ? d.normalized() : def;
    }
    /// AXIS2_PLACEMENT_3D (origin, axis, ref direction) or AXIS1_PLACEMENT (origin, axis).
    Frame frame(int id) const {
        Frame f;
        const SPart* p = part(id, "AXIS2_PLACEMENT_3D");
        if (!p) p = part(id, "AXIS1_PLACEMENT");
        if (!p) return f;
        if (p->args.size() > 1) point(p->args[1].ref(), f.o);
        if (p->args.size() > 2 && p->args[2].kind == SVal::Ref) f.z = direction(p->args[2].ref(), {0, 0, 1});
        f.x = p->args.size() > 3 && p->args[3].kind == SVal::Ref ? direction(p->args[3].ref(), {0, 0, 0}) : Vec3{0, 0, 0};
        f.orthonormalise();
        return f;
    }
    Affine placement(int id) const {
        if (const SPart* p3 = part(id, "CARTESIAN_TRANSFORMATION_OPERATOR_3D")) {
            // Simple: (name, axis1, axis2, origin, scale, axis3); complex: CARTESIAN_TRANSFORMATION_OPERATOR
            // (axis1, axis2, origin, scale) with the _3D part (axis3).
            const SPart* p = entity(id)->size() == 1 ? p3 : part(id, "CARTESIAN_TRANSFORMATION_OPERATOR");
            const size_t o = entity(id)->size() == 1 ? 1 : 0;
            const SVal& axis3 = entity(id)->size() == 1 ? p3->args.size() > 5 ? p3->args[5] : SVal() : p3->args.empty() ? SVal() : p3->args[0];
            Frame f;
            if (!p) return f.affine();
            f.x = p->args.size() > o ? direction(p->args[o].ref(), {1, 0, 0}) : Vec3{1, 0, 0};
            f.y = p->args.size() > o + 1 ? direction(p->args[o + 1].ref(), {0, 1, 0}) : Vec3{0, 1, 0};
            f.z = direction(axis3.ref(), f.x.cross(f.y));
            if (p->args.size() > o + 2) point(p->args[o + 2].ref(), f.o);
            const double s = p->args.size() > o + 3 ? p->args[o + 3].number(1) : 1;
            return multiply(f.affine(), scaling(s, s, s));
        }
        return frame(id).affine();
    }
    Affine placementInverse(int id) const { return frame(id).inverse(); }

    bool vertex(int id, Vec3& out) const {
        const SPart* v = part(id, "VERTEX_POINT");
        return v && v->args.size() > 1 ? point(v->args[1].ref(), out) : point(id, out);
    }

    BSplineCurve bspline(int id) const {
        BSplineCurve c;
        const SEntity* e = entity(id);
        if (!e) return c;
        const SVal *degree = nullptr, *points = nullptr, *mults = nullptr, *knots = nullptr, *weights = nullptr;
        if (e->size() == 1) {  // B_SPLINE_CURVE_WITH_KNOTS(name, degree, points, form, closed, self_intersect, mults, knots, spec)
            const auto& a = e->front().args;
            if (a.size() < 3) return c;
            degree = &a[1];
            points = &a[2];
            if (a.size() > 7) {
                mults = &a[6];
                knots = &a[7];
            }
        } else {
            if (const SPart* p = part(id, "B_SPLINE_CURVE"); p && p->args.size() > 1) {
                degree = &p->args[0];
                points = &p->args[1];
            }
            if (const SPart* p = part(id, "B_SPLINE_CURVE_WITH_KNOTS"); p && p->args.size() > 1) {
                mults = &p->args[0];
                knots = &p->args[1];
            }
            if (const SPart* p = part(id, "RATIONAL_B_SPLINE_CURVE"); p && !p->args.empty()) weights = &p->args[0];
        }
        if (!degree || !points || points->kind != SVal::List) return c;
        c.degree = static_cast<int>(degree->number(0));
        if (c.degree < 1 || c.degree > 16 || points->list.size() <= static_cast<size_t>(c.degree) || points->list.size() > 4096) return c;
        for (const auto& r : points->list) {
            Vec3 p;
            if (!point(r.ref(), p)) return c;
            c.cp.push_back(p);
        }
        const size_t n = c.cp.size(), d = static_cast<size_t>(c.degree);
        c.w.assign(n, 1.0);
        if (weights && weights->list.size() == n)
            for (size_t i = 0; i < n; ++i) c.w[i] = std::max(1e-6, weights->list[i].number(1));
        if (mults && knots && mults->kind == SVal::List && knots->kind == SVal::List && mults->list.size() == knots->list.size())
            for (size_t i = 0; i < knots->list.size() && c.knots.size() <= n + d; ++i) {
                const int m = static_cast<int>(mults->list[i].number(1));
                for (int k = 0; k < std::clamp(m, 1, c.degree + 1); ++k) c.knots.push_back(knots->list[i].number(0));
            }
        if (c.knots.size() != n + d + 1 || !std::is_sorted(c.knots.begin(), c.knots.end())) {  // uniform, clamped
            c.knots.clear();
            for (size_t i = 0; i < n + d + 1; ++i)
                c.knots.push_back(i <= d ? 0.0 : i >= n ? 1.0 : static_cast<double>(i - d) / static_cast<double>(n - d));
        }
        c.ok = c.knots[n] > c.knots[d];
        return c;
    }

    /// Samples of an EDGE_CURVE from its start vertex to its end vertex (file units); empty when unreadable.
    const std::vector<Vec3>& edge(int id) {
        auto it = edges_.find(id);
        if (it != edges_.end()) return it->second;
        std::vector<Vec3>& pts = edges_[id];
        const SPart* e = part(id, "EDGE_CURVE");
        Vec3 a, b;
        if (!e || e->args.size() < 5 || !vertex(e->args[1].ref(), a) || !vertex(e->args[2].ref(), b)) return pts;
        int curve = e->args[3].ref();
        bool same = e->args[4].isTrue();
        if (const SPart* t = part(curve, "TRIMMED_CURVE"); t && t->args.size() > 4) {
            same = same == t->args[4].isTrue();
            curve = t->args[1].ref();
        }
        pts.push_back(a);
        std::vector<Vec3> inner;
        const SPart* c = first(curve);
        const std::string type = c ? c->type : "";
        if ((type == "CIRCLE" || type == "ELLIPSE") && c->args.size() > 2) {
            const Frame f = frame(c->args[1].ref());
            const double ra = std::fabs(c->args[2].number(0)), rb = type == "ELLIPSE" && c->args.size() > 3 ? std::fabs(c->args[3].number(0)) : ra;
            if (ra > 0 && rb > 0) {
                auto angle = [&](Vec3 p) { return std::atan2(dot(p - f.o, f.y) / rb, dot(p - f.o, f.x) / ra); };
                double sweep = std::fmod(angle(b) - angle(a) + 4 * kModelPi, 2 * kModelPi);
                if ((b - a).length() < 1e-9 * std::max(1.0, ra) || sweep < 1e-9) sweep = 2 * kModelPi;
                if (!same) sweep -= 2 * kModelPi;
                const int segments = std::max(1, static_cast<int>(std::ceil(std::fabs(sweep) / (kModelPi / 12) - 1e-9)));
                for (int i = 1; i < segments; ++i) {
                    const double t = angle(a) + sweep * i / segments;
                    inner.push_back(f.o + f.x * (ra * std::cos(t)) + f.y * (rb * std::sin(t)));
                }
            }
        } else if (type.find("B_SPLINE_CURVE") != std::string::npos || is(curve, "B_SPLINE_CURVE")) {
            const BSplineCurve s = bspline(curve);
            if (s.ok) {
                const int segments = std::clamp(4 * static_cast<int>(s.cp.size() - static_cast<size_t>(s.degree)), 4, 48);
                const double t0 = s.knots[static_cast<size_t>(s.degree)], t1 = s.knots[s.cp.size()];
                for (int i = 1; i < segments; ++i) inner.push_back(s.at(t0 + (t1 - t0) * i / segments));
                if (!same) std::reverse(inner.begin(), inner.end());
            }
        } else if (type == "POLYLINE" && c->args.size() > 1 && c->args[1].kind == SVal::List) {
            for (size_t i = 1; i + 1 < c->args[1].list.size() && i < 64; ++i) {
                Vec3 p;
                if (point(c->args[1].list[i].ref(), p)) inner.push_back(p);
            }
            if (!same) std::reverse(inner.begin(), inner.end());
        } else if (type != "LINE") {
            ++otherEdges_;
            if (otherCurves_.size() < 8) otherCurves_.insert(type.empty() ? "missing" : type);
        }
        pts.insert(pts.end(), inner.begin(), inner.end());
        pts.push_back(b);
        return pts;
    }

    /// The points of a loop in order (file units), without the closing repeat.
    std::vector<Vec3> loop(int id) {
        std::vector<Vec3> pts;
        auto add = [&](Vec3 p) {
            if (pts.size() < kStepMaxLoopPoints && (pts.empty() || (pts.back() - p).length() > 0)) pts.push_back(p);
        };
        if (const SPart* p = part(id, "POLY_LOOP"); p && p->args.size() > 1 && p->args[1].kind == SVal::List) {
            for (const auto& r : p->args[1].list) {
                Vec3 v;
                if (point(r.ref(), v)) add(v);
            }
        } else if (const SPart* l = part(id, "EDGE_LOOP"); l && l->args.size() > 1 && l->args[1].kind == SVal::List) {
            for (const auto& r : l->args[1].list) {
                const SPart* oe = part(r.ref(), "ORIENTED_EDGE");  // (name, *, *, edge, orientation)
                const int edgeId = oe && oe->args.size() > 4 ? oe->args[3].ref() : r.ref();
                std::vector<Vec3> e = edge(edgeId);
                if (oe && oe->args.size() > 4 && !oe->args[4].isTrue()) std::reverse(e.begin(), e.end());
                for (const auto& p : e) add(p);
            }
        }
        return pts;  // may end on its first point: on a seam that point has another parameter
    }

    // ---- the assembly tree

    void representation(int id, const Affine& m, int depth) {
        if (depth > 24 || ++visits_ > 100000) throw Model3DError("The assembly structure is too deep or too large.");
        const SPart* r = first(id);
        if (r && r->args.size() > 1 && r->args[1].kind == SVal::List)
            for (const auto& it : r->args[1].list) item(it.ref(), m, depth);
        auto links = links_.find(id);
        if (links != links_.end())
            for (const Link& l : links->second) representation(l.child, multiply(m, l.m), depth + 1);
    }

    void item(int id, const Affine& m, int depth) {
        const SPart* p = first(id);
        if (!p || depth > 24) return;
        const std::string& t = p->type;
        if (t == "MAPPED_ITEM" && p->args.size() > 2) {  // (name, REPRESENTATION_MAP(origin, rep), target)
            const SPart* map = part(p->args[1].ref(), "REPRESENTATION_MAP");
            if (!map || map->args.size() < 2) return;
            const Affine local = multiply(placement(p->args[2].ref()), placementInverse(map->args[0].ref()));
            representation(map->args[1].ref(), multiply(m, local), depth + 1);
        } else if (t == "MANIFOLD_SOLID_BREP" || t == "BREP_WITH_VOIDS" || t == "FACETED_BREP") {
            visited_.insert(id);
            const Model3DMesh::Group colour = colourOf(id, defaultColour_);
            if (p->args.size() > 1) shell(p->args[1].ref(), m, colour);
            if (p->args.size() > 2 && p->args[2].kind == SVal::List)
                for (const auto& v : p->args[2].list) shell(v.ref(), m, colour);
        } else if (t == "SHELL_BASED_SURFACE_MODEL" && p->args.size() > 1 && p->args[1].kind == SVal::List) {
            visited_.insert(id);
            const Model3DMesh::Group colour = colourOf(id, defaultColour_);
            for (const auto& v : p->args[1].list) shell(v.ref(), m, colour);
        } else if (t == "CLOSED_SHELL" || t == "OPEN_SHELL") {
            shell(id, m, colourOf(id, defaultColour_));
        }
    }

    void shell(int id, const Affine& m, const Model3DMesh::Group& colour) {
        const SPart* s = first(id);
        if (!s || s->args.size() < 2 || s->args[1].kind != SVal::List) return;
        visited_.insert(id);
        const Model3DMesh::Group own = colourOf(id, colour);
        for (const auto& f : s->args[1].list) face(f.ref(), m, own);
    }

    // ---- faces

    enum class Surface { Plane, Cylinder, Cone, Sphere, Torus, Revolution, Other };

    void face(int id, const Affine& m, const Model3DMesh::Group& colour) {
        const SPart* f = first(id);
        // A plain FACE (faceted B-reps: polygon loops, no surface) is triangulated from its boundary.
        const bool plain = f && f->type == "FACE" && f->args.size() == 2;
        if (!f || (!plain && f->type != "ADVANCED_FACE" && f->type != "FACE_SURFACE") || f->args.size() < (plain ? 2u : 3u) ||
            f->args[1].kind != SVal::List) {
            ++badFaces_;
            return;
        }
        std::vector<std::vector<Vec3>> loops;
        for (const auto& b : f->args[1].list) {
            const SPart* bound = first(b.ref());  // FACE_BOUND / FACE_OUTER_BOUND(name, loop, orientation)
            if (!bound || bound->args.size() < 3) continue;
            std::vector<Vec3> pts = loop(bound->args[1].ref());
            if (!bound->args[2].isTrue()) std::reverse(pts.begin(), pts.end());
            if (pts.size() >= 3) loops.push_back(std::move(pts));
        }
        if (loops.empty()) {
            ++badFaces_;
            return;
        }
        beginGroup(colourOf(id, colour));
        size_t total = 0;
        for (const auto& l : loops) total += l.size();
        if (total > kStepMaxLoopPoints) {
            ++fannedFaces_;
            for (const auto& l : loops) fan(l, m);
            return;
        }
        tessellate(loops, plain ? 0 : f->args[2].ref(), f->args.size() > 3 ? f->args[3].isTrue() : true, m);
    }

    Surface surfaceKind(int id, Frame& f, double& r1, double& r2) {
        const SPart* s = first(id);
        const std::string type = s ? s->type : "";
        if (s && s->args.size() > 1) f = frame(s->args[1].ref());
        r1 = s && s->args.size() > 2 ? std::fabs(s->args[2].number(1)) : 1;
        r2 = s && s->args.size() > 3 ? std::fabs(s->args[3].number(1)) : r1;
        if (type == "PLANE") return Surface::Plane;
        if (type == "CYLINDRICAL_SURFACE") return Surface::Cylinder;
        if (type == "CONICAL_SURFACE") return Surface::Cone;
        if (type == "SPHERICAL_SURFACE") return Surface::Sphere;
        if (type == "TOROIDAL_SURFACE" || type == "DEGENERATE_TOROIDAL_SURFACE") return Surface::Torus;
        if (type == "SURFACE_OF_REVOLUTION" && s->args.size() > 2) {
            f = frame(s->args[2].ref());
            return Surface::Revolution;
        }
        if (type.find("B_SPLINE_SURFACE") != std::string::npos || is(id, "B_SPLINE_SURFACE")) ++splineFaces_;
        else if (id != 0) {  // 0: a plain FACE, which has no surface
            ++otherFaces_;
            if (otherSurfaces_.size() < 8) otherSurfaces_.insert(type.empty() ? "missing" : type);
        }
        return Surface::Other;
    }

    void tessellate(std::vector<std::vector<Vec3>>& loops, int surfaceId, bool sameSense, const Affine& m) {
        Frame f;
        double r1 = 1, r2 = 1;
        const Surface kind = surfaceKind(surfaceId, f, r1, r2);
        if (kind == Surface::Other) {  // best-fit plane of the largest loop (Newell normal)
            Vec3 n, c;
            size_t best = 0;
            for (size_t k = 0; k < loops.size(); ++k) {
                Vec3 nk;
                for (size_t i = 0; i < loops[k].size(); ++i) nk = nk + loops[k][i].cross(loops[k][(i + 1) % loops[k].size()]);
                if (nk.length() > n.length()) {
                    n = nk;
                    best = k;
                }
            }
            for (const auto& p : loops[best]) c = c + p * (1.0 / static_cast<double>(loops[best].size()));
            f.o = c;
            f.z = n;
            f.x = {0, 0, 0};
            f.orthonormalise();
        }
        if (kind == Surface::Revolution) {  // the radius of the loop points from the axis sets the scale
            r1 = 0;
            for (const auto& l : loops)
                for (const auto& p : l) {
                    const Vec3 d = p - f.o;
                    r1 = std::max(r1, (d - f.z * dot(d, f.z)).length());
                }
            if (r1 <= 0) r1 = 1;
        }
        const bool periodicU = kind == Surface::Cylinder || kind == Surface::Cone || kind == Surface::Sphere || kind == Surface::Torus ||
                               kind == Surface::Revolution;
        const bool periodicV = kind == Surface::Torus;
        const double pu = 2 * kModelPi * r1, pv = 2 * kModelPi * r2;
        auto param = [&](Vec3 p, bool& onAxis) -> P2 {
            const Vec3 d = p - f.o;
            const double dx = dot(d, f.x), dy = dot(d, f.y), dz = dot(d, f.z);
            onAxis = periodicU && std::hypot(dx, dy) < 1e-6 * (r1 + 1);  // an apex or pole: no longitude of its own
            switch (kind) {
                case Surface::Sphere: return {r1 * std::atan2(dy, dx), r1 * std::atan2(dz, std::hypot(dx, dy))};
                case Surface::Torus: return {r1 * std::atan2(dy, dx), r2 * std::atan2(dz, std::hypot(dx, dy) - r1)};
                case Surface::Plane:
                case Surface::Other: return {dx, dy};
                default: return {r1 * std::atan2(dy, dx), dz};
            }
        };
        auto unwrap = [](double v, double prev, double period) {
            return period > 0 ? v + period * std::round((prev - v) / period) : v;
        };
        // Parameters per loop, unwrapped along the loop; a loop that ends a full turn from where it started has
        // no seam (a whole circle on a cylinder).
        std::vector<std::vector<P2>> uv(loops.size());
        std::vector<int> turns(loops.size(), 0);
        for (size_t k = 0; k < loops.size(); ++k) {
            int apex = -1;
            for (const auto& p : loops[k]) {
                bool onAxis = false;
                P2 q = param(p, onAxis);
                if (onAxis) {
                    q[0] = uv[k].empty() ? 0 : uv[k].back()[0];
                    apex = static_cast<int>(uv[k].size());
                } else if (!uv[k].empty()) {
                    if (periodicU) q[0] = unwrap(q[0], uv[k].back()[0], pu);
                    if (periodicV) q[1] = unwrap(q[1], uv[k].back()[1], pv);
                }
                uv[k].push_back(q);
            }
            // A seam followed up to an apex and back down: the way down lies one period over, so the loop closes
            // around the cap instead of folding onto itself.
            if (apex >= 0 && uv[k].size() > 2 && (loops[k].back() - loops[k].front()).length() == 0) {
                const double shift = pu * std::round((uv[k].front()[0] - uv[k].back()[0]) / pu);
                for (size_t i = static_cast<size_t>(apex) + 1; shift != 0 && i < uv[k].size(); ++i) uv[k][i][0] += shift;
            }
            if (uv[k].size() > 3 && std::hypot(uv[k].back()[0] - uv[k][0][0], uv[k].back()[1] - uv[k][0][1]) < 1e-9 * (1 + std::fabs(uv[k][0][0]) + std::fabs(uv[k][0][1]))) {
                uv[k].pop_back();
                loops[k].pop_back();
            }
            if (periodicU || periodicV) {
                const P2 &a = uv[k].front(), &b = uv[k].back();
                const double du = periodicU ? unwrap(a[0], b[0], pu) - a[0] : 0, dv = periodicV ? unwrap(a[1], b[1], pv) - a[1] : 0;
                turns[k] = static_cast<int>(std::round(du / std::max(pu, 1e-300))) + static_cast<int>(std::round(dv / std::max(pv, 1e-300)));
            }
        }
        const int wrapping = static_cast<int>(std::count_if(turns.begin(), turns.end(), [](int t) { return t != 0; }));
        if (wrapping == 2 && loops.size() == 2) {  // a band between two seamless loops: join them into one polygon
            auto& a = uv[0];
            auto& b = uv[1];
            auto mean = [](const std::vector<P2>& v, int c) {
                double s = 0;
                for (const auto& q : v) s += q[c];
                return s / static_cast<double>(v.size());
            };
            const double su = periodicU ? pu * std::round((mean(a, 0) - mean(b, 0)) / pu) : 0, sv = periodicV ? pv * std::round((mean(a, 1) - mean(b, 1)) / pv) : 0;
            for (auto& q : b) q = {q[0] + su, q[1] + sv};
            auto dist = [](const P2& p, const P2& q) { return std::hypot(p[0] - q[0], p[1] - q[1]); };
            if (dist(a.back(), b.front()) > dist(a.back(), b.back())) {
                std::reverse(b.begin(), b.end());
                std::reverse(loops[1].begin(), loops[1].end());
            }
            a.insert(a.end(), b.begin(), b.end());
            loops[0].insert(loops[0].end(), loops[1].begin(), loops[1].end());
            uv.resize(1);
            loops.resize(1);
        } else if (wrapping == 1 && loops.size() == 1) {  // a seamless cap: close the rim across its apex / pole / centre
            Vec3 apex;
            for (const auto& p : loops[0]) apex = apex + p * (1.0 / static_cast<double>(loops[0].size()));
            if (kind == Surface::Cone && r2 > 1e-6 && r2 < kModelPi / 2 - 1e-6) apex = f.o - f.z * (r1 / std::tan(r2));
            if (kind == Surface::Sphere) {  // the pole the loop turns counter-clockwise about
                Vec3 n;
                for (size_t i = 0; i < loops[0].size(); ++i) n = n + (loops[0][i] - f.o).cross(loops[0][(i + 1) % loops[0].size()] - f.o);
                apex = f.o + f.z * (dot(n, f.z) >= 0 ? r1 : -r1);
            }
            bool onAxis = false;
            const double v = param(apex, onAxis)[1];
            uv[0].push_back({uv[0].back()[0], v});  // the apex row: one point in space, a period wide here
            uv[0].push_back({uv[0].front()[0], v});
            loops[0].push_back(apex);
            loops[0].push_back(apex);
        }
        // Outer loop = the largest; made counter-clockwise, holes clockwise.
        std::vector<P2> pts;
        std::vector<std::vector<int>> polys(loops.size());
        std::vector<Vec3> world;
        for (size_t k = 0; k < loops.size(); ++k)
            for (size_t i = 0; i < loops[k].size(); ++i) {
                polys[k].push_back(static_cast<int>(pts.size()));
                pts.push_back(uv[k][i]);
                world.push_back(loops[k][i]);
            }
        size_t outer = 0;
        double outerArea = 0;
        std::vector<double> areas(loops.size());
        for (size_t k = 0; k < loops.size(); ++k) {
            areas[k] = signedArea(pts, polys[k]);
            if (std::fabs(areas[k]) > std::fabs(outerArea)) {
                outerArea = areas[k];
                outer = k;
            }
        }
        double extent = 0;
        for (const auto& q : pts) extent = std::max({extent, std::fabs(q[0] - pts[0][0]), std::fabs(q[1] - pts[0][1])});
        if (!(std::fabs(outerArea) > 1e-12 * extent * extent)) {  // flat in parameter space
            if ((kind == Surface::Sphere || kind == Surface::Torus) && wrapping == 0) {
                grid(kind, f, r1, r2, sameSense, m);
                return;
            }
            ++fannedFaces_;
            for (auto& l : loops) fan(l, m);
            return;
        }
        const bool flip = outerArea < 0;
        std::vector<int> poly = polys[outer];
        std::vector<std::vector<int>> holes;
        for (size_t k = 0; k < loops.size(); ++k) {
            if (k == outer) continue;
            std::vector<int> h = polys[k];
            if (areas[k] > 0) std::reverse(h.begin(), h.end());  // holes clockwise
            holes.push_back(std::move(h));
        }
        if (flip) std::reverse(poly.begin(), poly.end());
        if (!holes.empty()) bridgeHoles(pts, poly, std::move(holes));
        std::vector<std::array<int, 3>> tris;
        earClip(pts, poly, tris);
        refine(kind, f, r1, r2, pts, world, tris);
        emit(world, tris, flip, m);
    }

    void emit(const std::vector<Vec3>& world, const std::vector<std::array<int, 3>>& tris, bool flip, const Affine& m) {
        for (const auto& t : tris) {
            const uint32_t a = vertex(world[static_cast<size_t>(t[0])], m), b = vertex(world[static_cast<size_t>(t[1])], m),
                           c = vertex(world[static_cast<size_t>(t[2])], m);
            weld_.triangle(a, flip ? c : b, flip ? b : c);
        }
    }

    /// Adds interior points on a facet grid (15° around the axis, 15° of latitude, 30° around a torus tube) to a
    /// face on a sphere or torus, each on the surface, inserted into the boundary triangulation with Lawson edge
    /// flips (boundary edges are never flipped, so neighbours still share the boundary samples). Without them a
    /// doubly curved face would span its boundary with flat chords.
    void refine(Surface kind, const Frame& f, double r1, double r2, std::vector<P2>& pts, std::vector<Vec3>& world,
                std::vector<std::array<int, 3>>& tris) {
        if ((kind != Surface::Sphere && kind != Surface::Torus) || tris.empty()) return;
        const double du = 2 * kModelPi * r1 / 24, dv = kind == Surface::Sphere ? r1 * kModelPi / 12 : r2 * kModelPi / 6;
        auto surface = [&](const P2& q) {
            const Vec3 radial = f.x * std::cos(q[0] / r1) + f.y * std::sin(q[0] / r1);
            if (kind == Surface::Sphere) return f.o + radial * (r1 * std::cos(q[1] / r1)) + f.z * (r1 * std::sin(q[1] / r1));
            return f.o + radial * (r1 + r2 * std::cos(q[1] / r2)) + f.z * (r2 * std::sin(q[1] / r2));
        };
        auto at = [&](int i) -> const P2& { return pts[static_cast<size_t>(i)]; };
        auto cross = [&](int a, int b, const P2& c) {
            return (at(b)[0] - at(a)[0]) * (c[1] - at(a)[1]) - (at(b)[1] - at(a)[1]) * (c[0] - at(a)[0]);
        };
        auto inCircle = [&](int a, int b, int c, int d) {  // d inside the circumcircle of the counter-clockwise a, b, c
            const double ax = at(a)[0] - at(d)[0], ay = at(a)[1] - at(d)[1], bx = at(b)[0] - at(d)[0], by = at(b)[1] - at(d)[1],
                         cx = at(c)[0] - at(d)[0], cy = at(c)[1] - at(d)[1];
            return (ax * ax + ay * ay) * (bx * cy - cx * by) - (bx * bx + by * by) * (ax * cy - cx * ay) + (cx * cx + cy * cy) * (ax * by - bx * ay) > 0;
        };
        std::map<std::pair<int, int>, int> owner;  // directed edge → triangle
        auto own = [&](int t) {
            for (int k = 0; k < 3; ++k) owner[{tris[static_cast<size_t>(t)][static_cast<size_t>(k)], tris[static_cast<size_t>(t)][static_cast<size_t>((k + 1) % 3)]}] = t;
        };
        auto disown = [&](int t) {
            for (int k = 0; k < 3; ++k) owner.erase({tris[static_cast<size_t>(t)][static_cast<size_t>(k)], tris[static_cast<size_t>(t)][static_cast<size_t>((k + 1) % 3)]});
        };
        for (size_t t = 0; t < tris.size(); ++t) own(static_cast<int>(t));
        std::vector<std::pair<int, int>> fixed;  // the boundary: edges with nothing on the other side
        for (const auto& [e, t] : owner)
            if (!owner.count({e.second, e.first})) fixed.push_back(e);
        P2 lo = pts[0], hi = pts[0];
        for (const auto& q : pts) lo = {std::min(lo[0], q[0]), std::min(lo[1], q[1])}, hi = {std::max(hi[0], q[0]), std::max(hi[1], q[1])};
        const double margin = 0.25 * std::min(du, dv), eps = 1e-9 * (du * du + dv * dv);
        std::vector<std::pair<int, int>> stack;
        for (double v = std::ceil(lo[1] / dv) * dv; v < hi[1] && pts.size() < 4 * kStepMaxLoopPoints; v += dv)
            for (double u = std::ceil(lo[0] / du) * du; u < hi[0]; u += du) {
                const P2 g{u, v};
                bool clear = true;
                for (const auto& [a, b] : fixed) {  // not close to the boundary (that would give slivers)
                    const double len2 = (at(b)[0] - at(a)[0]) * (at(b)[0] - at(a)[0]) + (at(b)[1] - at(a)[1]) * (at(b)[1] - at(a)[1]);
                    const double along = std::clamp(((g[0] - at(a)[0]) * (at(b)[0] - at(a)[0]) + (g[1] - at(a)[1]) * (at(b)[1] - at(a)[1])) / std::max(len2, 1e-300), 0.0, 1.0);
                    const double dx = at(a)[0] + along * (at(b)[0] - at(a)[0]) - g[0], dy = at(a)[1] + along * (at(b)[1] - at(a)[1]) - g[1];
                    if (dx * dx + dy * dy < margin * margin) {
                        clear = false;
                        break;
                    }
                }
                int t = -1;
                for (size_t i = 0; clear && i < tris.size() && t < 0; ++i)
                    if (cross(tris[i][0], tris[i][1], g) > eps && cross(tris[i][1], tris[i][2], g) > eps && cross(tris[i][2], tris[i][0], g) > eps)
                        t = static_cast<int>(i);
                if (t < 0) continue;  // outside the face, or on an edge
                const int gi = static_cast<int>(pts.size());
                pts.push_back(g);
                world.push_back(surface(g));
                const std::array<int, 3> old = tris[static_cast<size_t>(t)];
                disown(t);
                tris[static_cast<size_t>(t)] = {old[0], old[1], gi};
                tris.push_back({old[1], old[2], gi});
                tris.push_back({old[2], old[0], gi});
                for (int k : {t, static_cast<int>(tris.size()) - 2, static_cast<int>(tris.size()) - 1}) own(k);
                stack = {{old[0], old[1]}, {old[1], old[2]}, {old[2], old[0]}};
                for (int guard = 0; !stack.empty() && guard < 10000; ++guard) {  // Lawson flips of the edges facing gi
                    const auto [a, b] = stack.back();
                    stack.pop_back();
                    auto mine = owner.find({a, b}), other = owner.find({b, a});
                    if (mine == owner.end() || other == owner.end()) continue;  // a boundary edge
                    const int tg = mine->second, tn = other->second;
                    int d = tris[static_cast<size_t>(tn)][0];
                    for (int v3 : tris[static_cast<size_t>(tn)])
                        if (v3 != a && v3 != b) d = v3;
                    if (!inCircle(a, b, gi, d) || cross(a, d, at(gi)) <= 0 || cross(d, b, at(gi)) <= 0) continue;
                    disown(tg);
                    disown(tn);
                    tris[static_cast<size_t>(tg)] = {a, d, gi};
                    tris[static_cast<size_t>(tn)] = {d, b, gi};
                    own(tg);
                    own(tn);
                    stack.push_back({a, d});
                    stack.push_back({d, b});
                }
            }
    }

    /// Fan of a loop from its first point (the fallback for a boundary that could not be triangulated).
    void fan(const std::vector<Vec3>& l, const Affine& m) {
        const uint32_t a = vertex(l[0], m);
        for (size_t i = 1; i + 1 < l.size(); ++i) weld_.triangle(a, vertex(l[i], m), vertex(l[i + 1], m));
    }

    /// A whole sphere or torus (a face bounded only by seams).
    void grid(Surface kind, const Frame& f, double r1, double r2, bool sameSense, const Affine& m) {
        const int nu = 24, nv = 12;
        auto at = [&](int i, int j) {
            const double u = 2 * kModelPi * i / nu;
            if (kind == Surface::Sphere) {
                const double v = kModelPi * j / nv - kModelPi / 2;
                return vertex(f.o + (f.x * std::cos(u) + f.y * std::sin(u)) * (r1 * std::cos(v)) + f.z * (r1 * std::sin(v)), m);
            }
            const double v = 2 * kModelPi * j / nv;
            return vertex(f.o + (f.x * std::cos(u) + f.y * std::sin(u)) * (r1 + r2 * std::cos(v)) + f.z * (r2 * std::sin(v)), m);
        };
        for (int i = 0; i < nu; ++i)
            for (int j = 0; j < nv; ++j) {
                const uint32_t a = at(i, j), b = at(i + 1, j), c = at(i + 1, j + 1), d = at(i, j + 1);
                weld_.triangle(a, sameSense ? b : c, sameSense ? c : b);
                weld_.triangle(a, sameSense ? c : d, sameSense ? d : c);
            }
    }

    uint32_t vertex(Vec3 p, const Affine& m) {
        double out[3];
        apply(m, p.x, p.y, p.z, out);
        return weld_.add(static_cast<float>(out[0]), static_cast<float>(out[1]), static_cast<float>(out[2]));
    }

    // ---- colours and units

    void readColours() {
        for (const char* type : {"STYLED_ITEM", "OVER_RIDING_STYLED_ITEM"})
            for (int id : idsOf(type)) {
                const SPart* s = first(id);  // (name, styles, item)
                Model3DMesh::Group g;
                int budget = 4096;
                if (s->args.size() > 2 && findColour(s->args[1], g, 0, budget)) colours_[s->args[2].ref()] = g;
            }
    }
    bool findColour(const SVal& v, Model3DMesh::Group& g, int depth, int& budget) const {
        if (depth > 10 || --budget < 0) return false;
        if (v.kind == SVal::Ref) {
            const SEntity* e = entity(v.ref());
            if (!e) return false;
            for (const auto& p : *e) {
                if (p.type == "COLOUR_RGB" && p.args.size() > 3) {
                    g.r = static_cast<float>(std::clamp(p.args[1].number(0.6), 0.0, 1.0));
                    g.g = static_cast<float>(std::clamp(p.args[2].number(0.6), 0.0, 1.0));
                    g.b = static_cast<float>(std::clamp(p.args[3].number(0.6), 0.0, 1.0));
                    return true;
                }
                for (const auto& a : p.args)
                    if (findColour(a, g, depth + 1, budget)) return true;
            }
            return false;
        }
        if (v.kind == SVal::List)
            for (const auto& a : v.list)
                if (findColour(a, g, depth + 1, budget)) return true;
        return false;
    }
    Model3DMesh::Group colourOf(int id, const Model3DMesh::Group& def) const {
        auto it = colours_.find(id);
        return it == colours_.end() ? def : it->second;
    }
    void beginGroup(const Model3DMesh::Group& g) {
        if (!mesh_.groups.empty() && mesh_.groups.back().r == g.r && mesh_.groups.back().g == g.g && mesh_.groups.back().b == g.b) return;
        endGroup();
        mesh_.groups.push_back({g.r, g.g, g.b, 1.0f, static_cast<uint32_t>(mesh_.indices.size()), 0});
    }
    void endGroup() {
        if (!mesh_.groups.empty()) mesh_.groups.back().count = static_cast<uint32_t>(mesh_.indices.size()) - mesh_.groups.back().first;
    }

    /// Millimetres per unit of a LENGTH_UNIT instance: SI_UNIT with a prefix, or CONVERSION_BASED_UNIT (inch…).
    double unitMm(int id, int depth) const {
        if (depth > 4 || !is(id, "LENGTH_UNIT")) return 0;
        if (const SPart* si = part(id, "SI_UNIT"); si && si->args.size() > 1 && si->args[1].kind == SVal::Enum && si->args[1].str == "METRE") {
            static const std::pair<const char*, double> prefixes[] = {{"MILLI", 1}, {"CENTI", 10}, {"DECI", 100}, {"MICRO", 1e-3}, {"NANO", 1e-6}, {"KILO", 1e6}};
            if (si->args[0].kind != SVal::Enum) return 1000;
            for (const auto& [name, mm] : prefixes)
                if (si->args[0].str == name) return mm;
        }
        if (const SPart* cb = part(id, "CONVERSION_BASED_UNIT"); cb && cb->args.size() > 1) {
            const SPart* mu = first(cb->args[1].ref());  // MEASURE_WITH_UNIT(LENGTH_MEASURE(v), unit)
            if (mu && mu->args.size() > 1) {
                const double base = unitMm(mu->args[1].ref(), depth + 1), v = mu->args[0].number(0);
                if (base > 0 && v > 0 && std::isfinite(v)) return v * base;
            }
        }
        return 0;
    }
    double lengthUnitMm() {
        std::vector<int> ids;
        for (int c : idsOf("GLOBAL_UNIT_ASSIGNED_CONTEXT")) {
            const SPart* g = part(c, "GLOBAL_UNIT_ASSIGNED_CONTEXT");
            if (!g->args.empty() && g->args[0].kind == SVal::List)
                for (const auto& u : g->args[0].list) ids.push_back(u.ref());
        }
        for (int u : idsOf("LENGTH_UNIT")) ids.push_back(u);
        for (int u : ids)
            if (const double mm = unitMm(u, 0); mm > 0 && mm < 1e7) {
                unitFound_ = true;
                return mm;
            }
        return 1;
    }

    SEntities e_;
    Model3DMesh& mesh_;
    VertexWelder weld_;
    std::unordered_map<int, std::vector<Vec3>> edges_;
    std::unordered_map<int, Model3DMesh::Group> colours_;
    std::map<int, std::vector<Link>> links_;
    std::set<int> visited_;
    std::set<std::string> otherSurfaces_, otherCurves_;
    Model3DMesh::Group defaultColour_;
    int visits_ = 0, splineFaces_ = 0, otherFaces_ = 0, fannedFaces_ = 0, otherEdges_ = 0, badFaces_ = 0;
    bool unitFound_ = false;
};

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
    if (flat.visitsLeft < 0) mesh.warnings.push_back("stopped after 1 000 000 nodes (nodes reused by USE many times over)");
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

Model3DMesh parseStep(const std::string& text, const std::string& name) {
    if (text.size() > Model3DLimits::maxFile) throw Model3DError("The file is larger than 32 MB.");
    const size_t start = text.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    if (start == std::string::npos || text.compare(start, 12, "ISO-10303-21") != 0)
        throw Model3DError("This is not a STEP file (no ISO-10303-21 header).");
    Model3DMesh mesh;
    mesh.name = name;
    mesh.format = "step";
    StepModel model(StepParser(text).parse(), mesh);
    model.run();
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
    if (e == "step" || e == "stp") return parseStep(bytes, name);
    throw Model3DError("Not a 3D model SiEDA reads (.wrl, .stl, .obj, .step).");
}

double defaultModelUnit(const std::string& format) { return format == "vrml" ? 2.54 : 1.0; }

// ------------------------------------------------------------------ registry

Model3DRegistry& Model3DRegistry::instance() {
    static Model3DRegistry r;
    return r;
}

namespace {
size_t meshBytes(const Model3DMesh& m) {
    return m.positions.size() * sizeof(float) + m.indices.size() * sizeof(uint32_t) + m.groups.size() * sizeof(Model3DMesh::Group);
}
}  // namespace

std::string Model3DRegistry::add(Model3DMesh mesh) {
    validate(mesh);
    const std::string id = meshId(mesh);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Entry& e = meshes_[id];
        if (!e.mesh) e.mesh = std::make_shared<const Model3DMesh>(std::move(mesh));
        e.used = ++clock_;
    }
    trim();
    return id;
}

std::shared_ptr<const Model3DMesh> Model3DRegistry::get(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = meshes_.find(id);
    if (it == meshes_.end()) return nullptr;
    it->second.used = ++clock_;
    return it->second.mesh;
}

void Model3DRegistry::setBudget(size_t bytes) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        budget_ = bytes;
    }
    trim();
}

size_t Model3DRegistry::bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::set<const Model3DMesh*> seen;  // an alias shares its mesh
    size_t total = 0;
    for (const auto& [id, e] : meshes_)
        if (seen.insert(e.mesh.get()).second) total += meshBytes(*e.mesh);
    return total;
}

void Model3DRegistry::trim() {
    if (bytes() <= budget_) return;
    const std::set<std::string> library = CustomPartRegistry::instance().model3dIds();  // before our lock (its own)
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<const Model3DMesh*, long> entries;  // registry entries per mesh (aliases): more holders = held elsewhere
    std::map<const Model3DMesh*, size_t> size;
    for (const auto& [id, e] : meshes_) ++entries[e.mesh.get()], size[e.mesh.get()] = meshBytes(*e.mesh);
    size_t total = 0;
    for (const auto& [m, b] : size) total += b;
    std::vector<std::pair<uint64_t, std::string>> order;  // least recently used first
    for (const auto& [id, e] : meshes_) order.push_back({e.used, id});
    std::sort(order.begin(), order.end());
    for (const auto& [used, id] : order) {
        if (total <= budget_) break;
        auto it = meshes_.find(id);
        const Model3DMesh* m = it->second.mesh.get();
        // A library part's, a project's, or the newest (just imported: no part refers to it yet).
        if (library.count(id) || it->second.mesh.use_count() > entries[m] || used == clock_) continue;
        // Drop the mesh with every alias of it.
        for (auto a = meshes_.begin(); a != meshes_.end();) a = a->second.mesh.get() == m && !library.count(a->first) ? meshes_.erase(a) : std::next(a);
        if (std::none_of(meshes_.begin(), meshes_.end(), [&](const auto& kv) { return kv.second.mesh.get() == m; })) total -= size[m];
    }
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
    if (it != meshes_.end() && !meshes_.count(alias)) meshes_[alias] = {it->second.mesh, ++clock_};
}

Json models3dForParts(const std::vector<std::string>& customPartIds, const std::vector<std::string>& extraModels) {
    Json out = Json::array();
    std::set<std::string> done;
    for (const auto& id : extraModels)
        if (auto mesh = id.empty() || done.count(id) ? nullptr : Model3DRegistry::instance().get(id))
            out.push(model3dToJson(id, *mesh)), done.insert(id);
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
