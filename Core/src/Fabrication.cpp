#include "sieda/Fabrication.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>

#include "sieda/Bom.hpp"
#include "sieda/Reliability.hpp"
#include "sieda/Stackup.hpp"
#include "sieda/Export.hpp"
#include "sieda/Json.hpp"
#include "sieda/Mesh.hpp"

namespace sieda {

namespace {
std::string fmt(const char* f, double v) {
    char b[64];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

std::string safeBase(const std::string& name) {
    std::string out;
    for (char ch : name) {
        char c = std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' ? ch : '_';
        if (c == '_' && (out.empty() || out.back() == '_')) continue;  // "Demo: LED" → "Demo_LED"
        out += c;
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? "board" : out;
}

/// Gerber X2 file function and polarity of a Gerber in the package (from its own %TF attributes).
std::pair<std::string, std::string> fileAttributes(const std::string& gerber) {
    auto attr = [&](const std::string& key) {
        size_t at = gerber.find("%TF." + key + ",");
        if (at == std::string::npos) return std::string();
        at += 5 + key.size();
        return gerber.substr(at, gerber.find("*%", at) - at);
    };
    return {attr("FileFunction"), attr("FilePolarity")};
}

bool endsWith(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

struct BoardFacts {
    double minTrack = 0, minDrill = 0, minRing = 0, minPitch = std::numeric_limits<double>::max();
    int pth = 0, vias = 0, npth = 0, smdTop = 0, smdBottom = 0, partsTop = 0, partsBottom = 0, tht = 0;
    double sizeX = 0, sizeY = 0;
};

BoardFacts facts(const Project& p) {
    BoardFacts f;
    const auto& s = p.pcb.settings;
    f.minTrack = s.trackWidth;
    for (const auto& t : p.pcb.tracks) f.minTrack = std::min(f.minTrack, t.width);
    double minDrill = std::numeric_limits<double>::max(), minRing = minDrill;
    for (const auto& v : p.pcb.vias) {
        ++f.vias;
        minDrill = std::min(minDrill, v.drill);
        minRing = std::min(minRing, (v.diameter - v.drill) / 2);
    }
    auto pads = p.pcb.pads(p.schematic);
    std::map<int, std::vector<Vec2>> smdByPart;
    for (const auto& pd : pads) {
        if (pd.throughHole) {
            ++f.pth;
            minDrill = std::min(minDrill, pd.drill);
            minRing = std::min(minRing, (std::min(pd.size.x, pd.size.y) - pd.drill) / 2);
        } else {
            (pd.smdLayer == kTopLayer ? f.smdTop : f.smdBottom)++;
            smdByPart[pd.componentId].push_back(pd.position);
        }
    }
    for (const auto& [id, pts] : smdByPart)
        for (size_t a = 0; a < pts.size(); ++a)
            for (size_t b = a + 1; b < pts.size(); ++b) f.minPitch = std::min(f.minPitch, (pts[a] - pts[b]).length());
    f.minDrill = minDrill == std::numeric_limits<double>::max() ? 0 : minDrill;
    f.minRing = minRing == std::numeric_limits<double>::max() ? 0 : minRing;
    f.npth = static_cast<int>(s.holes.size());
    std::set<int> thtParts;
    for (const auto& pd : pads)
        if (pd.throughHole) thtParts.insert(pd.componentId);
    f.tht = static_cast<int>(thtParts.size());
    for (const auto& c : p.schematic.components()) {
        if (!c.hasFootprint() || !c.pcb.placed) continue;
        (c.pcb.bottom ? f.partsBottom : f.partsTop)++;
    }
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (const auto& v : s.outlinePolygon()) {
        x0 = std::min(x0, v.x), y0 = std::min(y0, v.y), x1 = std::max(x1, v.x), y1 = std::max(y1, v.y);
    }
    f.sizeX = x1 - x0, f.sizeY = y1 - y0;
    return f;
}

std::string colourName(const std::string& id) {
    std::string n = id;
    if (!n.empty()) n[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(n[0])));
    return n;
}

std::string silkColour(const BoardSettings& s) {
    const SolderMaskStyle* style = findSolderMask(s.solderMask);
    return style && style->silk.r < 0.5f ? "Black" : "White";
}

uint32_t crc32(const std::string& data) {
    static std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char ch : data) c = table[(c ^ ch) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
}  // namespace

std::string recommendedSurfaceFinish(const Project& project) {
    std::string required = fabricationRequirements(project).finish;  // industry (no pure tin, RF silver …)
    if (!required.empty()) return required;
    return facts(project).minPitch < 0.65 ? "ENIG" : "HASL lead-free";
}

std::string makeZip(const std::vector<std::pair<std::string, std::string>>& files) {
    std::string out, central;
    auto u16 = [](std::string& s, uint16_t v) { s += static_cast<char>(v & 0xFF); s += static_cast<char>(v >> 8); };
    auto u32 = [&](std::string& s, uint32_t v) { u16(s, static_cast<uint16_t>(v & 0xFFFF)); u16(s, static_cast<uint16_t>(v >> 16)); };
    const uint16_t dosTime = 0, dosDate = (1 << 5) | 1;  // 1980-01-01: reproducible archives
    for (const auto& [name, data] : files) {
        uint32_t offset = static_cast<uint32_t>(out.size()), crc = crc32(data), size = static_cast<uint32_t>(data.size());
        u32(out, 0x04034b50); u16(out, 20); u16(out, 0); u16(out, 0); u16(out, dosTime); u16(out, dosDate);
        u32(out, crc); u32(out, size); u32(out, size); u16(out, static_cast<uint16_t>(name.size())); u16(out, 0);
        out += name;
        out += data;
        u32(central, 0x02014b50); u16(central, 20); u16(central, 20); u16(central, 0); u16(central, 0);
        u16(central, dosTime); u16(central, dosDate); u32(central, crc); u32(central, size); u32(central, size);
        u16(central, static_cast<uint16_t>(name.size())); u16(central, 0); u16(central, 0); u16(central, 0);
        u16(central, 0); u32(central, 0); u32(central, offset);
        central += name;
    }
    uint32_t centralOffset = static_cast<uint32_t>(out.size());
    out += central;
    u32(out, 0x06054b50); u16(out, 0); u16(out, 0);
    u16(out, static_cast<uint16_t>(files.size())); u16(out, static_cast<uint16_t>(files.size()));
    u32(out, static_cast<uint32_t>(central.size())); u32(out, centralOffset); u16(out, 0);
    return out;
}

std::string exportBackdrill(const Project& project) {
    auto stubs = viaStubs(project);
    if (stubs.empty()) return "";
    const auto& s = project.pcb.settings;
    const auto& nets = project.schematic.nets();
    std::ostringstream o;
    o << "M48\n; SiEDA backdrill program — remove unused via barrel stubs on high-speed nets\n"
      << "; drill = via drill + 0.20 mm; depth measured from the drilled side; never cut the listed must-not-cut layer\n"
      << "; #@! TF.FileFunction,NonPlated,1," << s.layerCount << ",Backdrill\nMETRIC,TZ\n";
    std::map<std::string, std::vector<const ViaStub*>> tools;
    for (const auto& st : stubs) tools[fmt("%.3f", st.drill + 0.2)].push_back(&st);
    int tool = 1;
    for (const auto& [d, list] : tools) o << "T" << tool++ << "C" << d << "\n";
    o << "%\nG90\nG05\n";
    tool = 1;
    for (const auto& [d, list] : tools) {
        o << "T" << tool++ << "\n";
        for (const ViaStub* st : list) {
            std::string net = st->net >= 0 && st->net < static_cast<int>(nets.size()) ? nets[static_cast<size_t>(st->net)].name : "?";
            if (st->bottomStub > 0.2)
                o << "; " << net << ": from bottom, depth " << fmt("%.3f", st->bottomStub - 0.05) << " mm, must not cut "
                  << copperLayerName(st->lastLayer, s.layerCount) << "\n";
            if (st->topStub > 0.2)
                o << "; " << net << ": from top, depth " << fmt("%.3f", st->topStub - 0.05) << " mm, must not cut "
                  << copperLayerName(st->firstLayer, s.layerCount) << "\n";
            o << "X" << fmt("%.4f", st->position.x) << "Y" << fmt("%.4f", s.height - st->position.y) << "\n";
        }
    }
    o << "T0\nM30\n";
    return o.str();
}

std::string exportGerberJob(const Project& project, const std::vector<FabFile>& files) {
    const auto& s = project.pcb.settings;
    const BoardFacts f = facts(project);
    const int layers = std::max(1, s.layerCount);
    Json root = Json::object();
    Json header = Json::object();
    Json software = Json::object();
    software["Vendor"] = "SiEDA";
    software["Application"] = "SiEDA";
    software["Version"] = "1.0";
    header["GenerationSoftware"] = software;
    std::time_t now = std::time(nullptr);
    char date[32];
    std::strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
    header["CreationDate"] = std::string(date);
    root["Header"] = header;

    Json general = Json::object();
    Json id = Json::object();
    id["Name"] = project.name;
    id["Revision"] = "1";
    general["ProjectId"] = id;
    Json size = Json::object();
    size["X"] = f.sizeX;
    size["Y"] = f.sizeY;
    general["Size"] = size;
    general["LayerNumber"] = layers;
    general["BoardThickness"] = s.thickness;
    std::string finish = recommendedSurfaceFinish(project);
    general["Finish"] = finish.rfind("ENIG", 0) == 0                     ? "ENIG"
                        : finish.find("SnPb") != std::string::npos          ? "HAL SnPb"
                        : finish.find("silver") != std::string::npos        ? "ImAg"
                                                                            : "HAL Pb-free";
    root["GeneralSpecs"] = general;

    Json rules = Json::array();
    Json outer = Json::object();
    outer["Layers"] = "Outer";
    outer["PadToPad"] = s.clearance;
    outer["PadToTrack"] = s.clearance;
    outer["TrackToTrack"] = s.clearance;
    outer["MinLineWidth"] = f.minTrack;
    rules.push(outer);
    root["DesignRules"] = rules;

    Json attrs = Json::array();
    for (const auto& file : files) {
        if (!endsWith(file.path, ".gbr")) continue;
        auto [function, polarity] = fileAttributes(file.content);
        Json a = Json::object();
        a["Path"] = file.path.substr(file.path.find_last_of('/') + 1);
        a["FileFunction"] = function;
        a["FilePolarity"] = polarity.empty() ? "Positive" : polarity;
        attrs.push(a);
    }
    root["FilesAttributes"] = attrs;

    // Stack-up, top to bottom.
    Json stack = Json::array();
    const double cu = 0.035 * std::max(0.5, s.copperWeightOz);
    auto layer = [&](const char* type, const std::string& name, double thickness, const std::string& colour,
                     const std::string& material) {
        Json l = Json::object();
        l["Type"] = type;
        l["Name"] = name;
        if (thickness > 0) l["Thickness"] = thickness;
        if (!colour.empty()) l["Color"] = colour;
        if (!material.empty()) l["Material"] = material;
        stack.push(l);
    };
    const std::string mask = colourName(s.solderMask), silk = silkColour(s);
    layer("Legend", "Top silkscreen", 0, silk, "");
    layer("SolderPaste", "Top paste", 0, "", "");
    layer("SolderMask", "Top solder mask", 0.01, mask, "");
    double dielectric = layers > 1 ? (s.thickness - layers * cu) / (layers - 1) : s.thickness - cu;
    for (int i = 0; i < layers; ++i) {
        layer("Copper", "L" + std::to_string(i + 1), cu, "", "");
        if (i < layers - 1 || layers == 1)
            layer("Dielectric", i == 0 ? "Core" : "Prepreg / core " + std::to_string(i + 1), dielectric, "",
                  project.pcb.settings.material != "fr4" ? boardLaminate(project.pcb.settings).name
                  : project.industry == "space"          ? "Polyimide"
                                                         : "FR4");
    }
    if (layers > 1) {
        layer("SolderMask", "Bottom solder mask", 0.01, mask, "");
        if (f.partsBottom > 0) {
            layer("SolderPaste", "Bottom paste", 0, "", "");
            layer("Legend", "Bottom silkscreen", 0, silk, "");
        }
    }
    root["MaterialStackup"] = stack;
    return root.dump(true) + "\n";
}

std::string fabricationNotes(const Project& project, const std::vector<FabFile>& files) {
    const auto& s = project.pcb.settings;
    const BoardFacts f = facts(project);
    const int layers = std::max(1, s.layerCount);
    const std::string finish = recommendedSurfaceFinish(project);
    std::ostringstream o;
    o << "FABRICATION NOTES — " << project.name << "\n";
    o << std::string(22 + project.name.size(), '=') << "\n\n";
    o << "Upload " << "the *-gerbers.zip to the board house; use the assembly/ files for SMT assembly.\n\n";
    o << "BOARD\n";
    o << "  Size                 " << fmt("%.2f", f.sizeX) << " x " << fmt("%.2f", f.sizeY) << " mm"
      << (s.hasCustomOutline() ? " (shaped outline — route along board-Edge_Cuts)" : " (rectangle)") << "\n";
    o << "  Layers               " << layers << (layers == 1 ? " (single-sided, copper on top)" : "") << "\n";
    const FabricationRequirements req = fabricationRequirements(project);
    o << "  Material             " << req.material << "\n";
    o << "  Thickness            " << fmt("%.2f", s.thickness) << " mm\n";
    o << "  Copper weight        " << fmt("%.1f", s.copperWeightOz) << " oz outer"
      << (layers > 2 ? ", " + fmt("%.1f", std::min(s.copperWeightOz, 1.0)) + " oz inner" : "") << "\n";
    o << "  Surface finish       " << finish
      << (finish == "ENIG" ? " (fine-pitch parts: flat pads, " + fmt("%.2f", f.minPitch) + " mm pitch)" : "") << "\n";
    o << "  Solder mask          " << colourName(s.solderMask) << (layers == 1 ? ", top only" : ", both sides")
      << "; vias tented\n";
    o << "  Silkscreen           " << silkColour(s) << ", top" << (f.partsBottom > 0 ? " and bottom" : "") << "\n";
    o << "  IPC class            " << req.ipcClass << " (IPC-6012 / IPC-A-610; design rules: " << s.rulePreset << ")\n";
    o << "  Solder               " << req.solder << "\n";
    o << "  Conformal coating    " << (s.coated() ? s.coating + " (IPC-CC-830)" : std::string("none")) << "\n";
    o << "  Electrical test      100 % (flying probe) against the IPC-D-356A netlist\n";
    {
        const NetClassification cls = classifyNets(project);
        if (!cls.rf.empty() || !cls.diffPairs.empty())
            o << "  Impedance control    " << fmt("%.0f", s.singleEndedImpedance) << " ohm single-ended / "
              << fmt("%.0f", s.differentialImpedance) << " ohm differential +/-10 % (RF and pair nets; widths from the "
              << "IPC-2141 stack-up — fab to adjust for its own prepreg)\n";
        else
            o << "  Impedance control    none\n";
        o << "  Castellated holes: none   Edge connector: none\n\n";
    }
    {
        int through = 0, blind = 0, buried = 0, micro = 0;
        for (const auto& v : project.pcb.vias) {
            std::string k = viaKind(v, layers);
            through += k == "through";
            blind += k == "blind";
            buried += k == "buried";
            micro += k == "microvia";
        }
        if (blind + buried + micro > 0 || s.viaInPad) {
            o << "HDI (IPC-2226)\n";
            o << "  Vias                 " << through << " through, " << blind << " blind, " << buried << " buried, "
              << micro << " laser microvias (" << fmt("%.2f", s.microviaDrill) << " mm drill, "
              << fmt("%.2f", s.microviaDiameter) << " mm pad)\n";
            o << "  Drill files          one Excellon file per span (*-L<a>-L<b>.drl); sequential lamination as needed\n";
            if (micro > 0) o << "  Microvias            copper-filled; stack only filled microvias, staggered preferred\n";
            if (s.viaInPad)
                o << "  Via-in-pad           VIPPO — vias in pads filled with non-conductive epoxy and plated over "
                     "(IPC-4761 Type VII)\n";
            o << "\n";
        }
    }
    o << "RELIABILITY (industry profile: " << project.industry << ")\n";
    for (const auto& n : req.notes) o << "  - " << n << "\n";
    o << "\n";
    o << "MINIMUM FEATURES (check against the fab's capabilities)\n";
    o << "  Track width          " << fmt("%.3f", f.minTrack) << " mm\n";
    o << "  Clearance            " << fmt("%.3f", s.clearance) << " mm (fab minimum checked by DRC: "
      << fmt("%.3f", s.minClearance) << " mm)\n";
    if (f.minDrill > 0) o << "  Drill                " << fmt("%.3f", f.minDrill) << " mm\n";
    if (f.minRing > 0) o << "  Annular ring         " << fmt("%.3f", f.minRing) << " mm\n";
    o << "  Edge clearance       " << fmt("%.2f", s.edgeClearance) << " mm\n\n";
    o << "HOLES\n";
    o << "  Plated (pads)        " << f.pth << "\n  Vias                 " << f.vias << "\n";
    o << "  Non-plated           " << f.npth << " (mounting holes)\n\n";
    o << "ASSEMBLY\n";
    o << "  Parts                " << f.partsTop << " top, " << f.partsBottom << " bottom (" << f.tht
      << " through-hole)\n";
    {
        BomSummary bom = summarizeBom(buildBom(project.schematic));
        o << "  BOM lines            " << bom.lines << (bom.dnp ? " (" + std::to_string(bom.dnp) + " parts not fitted: DNP)" : "")
          << (bom.missingMpn ? ", " + std::to_string(bom.missingMpn) + " still without a manufacturer part number" : "")
          << "\n";
    }
    o << "  SMD pads             " << f.smdTop << " top, " << f.smdBottom << " bottom\n";
    o << "  Stencil              " << (f.smdTop ? "top" : "") << (f.smdTop && f.smdBottom ? " + " : "")
      << (f.smdBottom ? "bottom" : "") << (f.smdTop || f.smdBottom ? "" : "not needed (no SMD pads)") << "\n";
    o << "  Coordinates          mm, origin at the board's lower-left corner (same as the Gerbers)\n\n";
    o << "FILES\n";
    for (const auto& file : files) o << "  " << file.path << "\n      " << file.description << "\n";
    return o.str();
}

std::vector<FabFile> fabricationPackage(const Project& project, const std::string& baseName) {
    const auto& sch = project.schematic;
    const auto& pcb = project.pcb;
    const auto& s = pcb.settings;
    const std::string base = safeBase(baseName.empty() ? project.name : baseName);
    const int layers = std::max(1, s.layerCount);
    const BoardFacts f = facts(project);
    std::vector<FabFile> files;
    auto add = [&](const std::string& path, std::string content, const std::string& what) {
        files.push_back({path, std::move(content), what});
    };
    const std::string g = "gerbers/" + base;

    for (int layer = 0; layer < layers; ++layer) {
        std::string suffix = layer == 0 ? "F_Cu" : (layer == layers - 1 ? "B_Cu" : "In" + std::to_string(layer) + "_Cu");
        add(g + "-" + suffix + ".gbr", exportCopperGerber(sch, pcb, layer),
            "Copper L" + std::to_string(layer + 1) + (layer == 0 ? " (top)" : layer == layers - 1 ? " (bottom)" : " (inner)"));
    }
    add(g + "-F_Mask.gbr", exportGerber(sch, pcb, GerberLayer::TopMask), "Solder mask top");
    if (layers > 1) add(g + "-B_Mask.gbr", exportGerber(sch, pcb, GerberLayer::BottomMask), "Solder mask bottom");
    if (f.smdTop) add(g + "-F_Paste.gbr", exportGerber(sch, pcb, GerberLayer::TopPaste), "Solder paste top (stencil)");
    if (f.smdBottom) add(g + "-B_Paste.gbr", exportGerber(sch, pcb, GerberLayer::BottomPaste), "Solder paste bottom (stencil)");
    add(g + "-F_Silkscreen.gbr", exportGerber(sch, pcb, GerberLayer::TopSilk), "Silkscreen top (outlines, designators)");
    if (f.partsBottom)
        add(g + "-B_Silkscreen.gbr", exportGerber(sch, pcb, GerberLayer::BottomSilk), "Silkscreen bottom");
    add(g + "-Edge_Cuts.gbr", exportGerber(sch, pcb, GerberLayer::EdgeCuts), "Board outline (profile)");
    if (f.pth || f.vias) add(g + "-PTH.drl", exportExcellonDrill(sch, pcb, true), "Drill, plated (pads and vias)");
    for (auto [from, to] : viaSpans(pcb))
        add(g + "-L" + std::to_string(from + 1) + "-L" + std::to_string(to + 1) + ".drl", exportExcellonSpan(pcb, from, to),
            to - from == 1 ? "Drill, laser microvias L" + std::to_string(from + 1) + "–L" + std::to_string(to + 1)
                           : "Drill, blind / buried vias L" + std::to_string(from + 1) + "–L" + std::to_string(to + 1));
    if (f.npth) add(g + "-NPTH.drl", exportExcellonDrill(sch, pcb, false), "Drill, non-plated (mounting holes)");
    add(g + "-ipc356.ipc", exportIpcD356(sch, pcb, project.name), "IPC-D-356A netlist (electrical test)");
    if (s.backdrill)
        if (std::string bd = exportBackdrill(project); !bd.empty())
            add(g + "-backdrill.drl", bd, "Backdrill program (via stubs on high-speed nets)");
    // The job file lists the Gerbers above.
    add(g + "-job.gbrjob", exportGerberJob(project, files), "Gerber X2 job file (stack-up, finish, colours)");

    std::vector<std::pair<std::string, std::string>> zipped;
    for (const auto& file : files) zipped.push_back({file.path.substr(8), file.content});  // strip "gerbers/"

    const std::string a = "assembly/" + base;
    add(a + "-bom.csv", exportBomCsv(sch), "Bill of materials");
    add(a + "-bom_assembly.csv", exportAssemblyBomCsv(sch), "BOM for assembly houses (JLCPCB / PCBWay columns)");
    add(a + "-cpl.csv", exportCplCsv(sch, pcb), "Component placement list (CPL) for assembly");
    add(a + "-pick_and_place.csv", exportPickAndPlaceCsv(sch), "Pick and place (all columns)");
    add(a + "-assembly_top.svg", exportAssemblySvg(sch, pcb, false, project.name), "Assembly drawing, top");
    if (f.partsBottom) add(a + "-assembly_bottom.svg", exportAssemblySvg(sch, pcb, true, project.name), "Assembly drawing, bottom");
    add(base + "-netlist.cir", exportSpiceNetlist(sch, project.name), "SPICE netlist");
    add("3d/" + base + ".stl", exportStl(buildAssemblyMesh(sch, pcb), base), "3D assembly (STL)");
    add(base + "-gerbers.zip", makeZip(zipped), "Gerbers + drills + job file + IPC netlist: upload this to the fab");
    // Notes last, so they list every file (themselves included).
    files.push_back({"fab_notes.txt", "", "Order sheet: these notes"});
    files.back().content = fabricationNotes(project, files);
    return files;
}

bool writeFabricationPackage(const Project& project, const std::string& dir, std::vector<std::string>* written,
                             std::string* error, const std::string& base) {
    namespace fs = std::filesystem;
    try {
        for (const auto& file : fabricationPackage(project, base)) {
            fs::path path = fs::path(dir) / file.path;
            fs::create_directories(path.parent_path());
            std::ofstream out(path, std::ios::binary);
            out.write(file.content.data(), static_cast<std::streamsize>(file.content.size()));
            if (!out) {
                if (error) *error = "Could not write " + path.string();
                return false;
            }
            if (written) written->push_back(file.path);
        }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

}  // namespace sieda
