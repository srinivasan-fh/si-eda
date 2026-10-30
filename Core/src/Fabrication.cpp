#include "sieda/Documents.hpp"
#include "sieda/Fabrication.hpp"
#include "sieda/FabExchange.hpp"
#include "sieda/Panel.hpp"

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
#include "sieda/Dfm.hpp"
#include "sieda/Mechanical.hpp"
#include "sieda/Embedded.hpp"
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
    f.npth = static_cast<int>(s.holes.size());  // mounting holes and footprint NPTH pads (-NPTH.drl)
    for (const auto& pd : pads) {
        if (pd.npth) {
            ++f.npth;
        } else if (pd.throughHole) {
            ++f.pth;
            minDrill = std::min(minDrill, pd.drill);
            minRing = std::min(minRing, (std::min(pd.size.x, pd.size.y) - pd.drill) / 2);
        } else if (pd.smdLayer == kTopLayer || pd.smdLayer == s.bottomLayer()) {  // inner: embedded passives
            (pd.smdLayer == kTopLayer ? f.smdTop : f.smdBottom)++;
            smdByPart[pd.componentId].push_back(pd.position);
        }
    }
    for (const auto& [id, pts] : smdByPart)
        for (size_t a = 0; a < pts.size(); ++a)
            for (size_t b = a + 1; b < pts.size(); ++b) f.minPitch = std::min(f.minPitch, (pts[a] - pts[b]).length());
    f.minDrill = minDrill == std::numeric_limits<double>::max() ? 0 : minDrill;
    f.minRing = minRing == std::numeric_limits<double>::max() ? 0 : minRing;
    std::set<int> thtParts;
    for (const auto& pd : pads)
        if (pd.throughHole && !pd.npth) thtParts.insert(pd.componentId);  // NPTH pegs (USB-C) are not soldered
    f.tht = static_cast<int>(thtParts.size());
    for (const auto& c : p.schematic.components()) {
        if (!c.hasFootprint() || !c.pcb.placed || c.pcb.embedded()) continue;
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

}  // namespace

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

std::string recommendedSurfaceFinish(const Project& project) {
    std::string required = fabricationRequirements(project).finish;  // industry (no pure tin, RF silver …)
    if (!required.empty()) return required;
    return facts(project).minPitch < 0.65 ? "ENIG" : "HASL lead-free";
}

const std::vector<FabService>& fabServices() {
    static const std::vector<FabService> services = {
        {"jlcpcb", "JLCPCB", "China", "https://cart.jlcpcb.com/quote", {"jlcpcb-standard", "jlcpcb-advanced"}},
        {"pcbway", "PCBWay", "China", "https://www.pcbway.com/orderonline.aspx", {"pcbway-standard"}},
        {"oshpark", "OSH Park", "USA", "https://oshpark.com/", {"oshpark-2", "oshpark-4"}},
        {"eurocircuits", "Eurocircuits", "Europe", "https://www.eurocircuits.com/", {"eurocircuits-6c"}},
        {"lioncircuits", "Lion Circuits", "India", "https://www.lioncircuits.com/", {}},
        {"pcbpower", "PCB Power", "India", "https://www.pcbpower.com/", {}},
    };
    return services;
}

Json fabQuote(const Project& project, const Json& request) {
    Json out = Json::object();
    const std::string id = request.get("fab").asString("");
    const FabService* fab = nullptr;
    for (const auto& f : fabServices())
        if (f.id == id) fab = &f;
    if (!fab) {
        Json list = Json::array();
        for (const auto& f : fabServices()) {
            Json e = Json::object();
            e["id"] = f.id, e["name"] = f.name, e["country"] = f.country, e["orderUrl"] = f.orderUrl;
            list.push(e);
        }
        out["error"] = id.empty() ? "Pick a fab" : "Unknown fab \"" + id + "\"";
        out["services"] = list;
        return out;
    }
    const BoardSettings& s = project.pcb.settings;
    const BoardFacts f = facts(project);
    const int quantity = static_cast<int>(std::clamp(request.get("quantity").asNumber(5), 1.0, 1e6));
    out["fab"] = fab->id, out["name"] = fab->name, out["country"] = fab->country, out["orderUrl"] = fab->orderUrl;
    out["quantity"] = quantity;
    Json o = Json::object();
    const int layers = std::max(1, s.layerCount);
    const double area = f.sizeX * f.sizeY / 100;  // cm²
    std::set<long> drills;
    for (const auto& v : project.pcb.vias) drills.insert(std::lround(v.drill * 1000));
    for (const auto& pd : project.pcb.pads(project.schematic))
        if (pd.throughHole && pd.drill > 0) drills.insert(std::lround(pd.drill * 1000));
    for (const auto& h : s.holes) drills.insert(std::lround(h.drill * 1000));
    const std::string finish = recommendedSurfaceFinish(project);
    o["layers"] = layers, o["widthMm"] = f.sizeX, o["heightMm"] = f.sizeY, o["areaCm2"] = area;
    o["thicknessMm"] = s.thickness, o["material"] = boardLaminate(s).name, o["copperOz"] = s.copperWeightOz;
    o["finish"] = finish, o["solderMask"] = colourName(s.solderMask), o["silkscreen"] = silkColour(s);
    o["minTrackMm"] = f.minTrack, o["minSpaceMm"] = s.clearance, o["minDrillMm"] = f.minDrill, o["minAnnularRingMm"] = f.minRing;
    o["vias"] = f.vias, o["platedHoles"] = f.pth, o["nonPlatedHoles"] = f.npth, o["drillSizes"] = static_cast<int>(drills.size());
    o["hdi"] = s.hdi, o["viaInPad"] = s.viaInPad, o["impedanceControl"] = !s.pairRules.empty(), o["construction"] = s.construction;
    o["panel"] = std::to_string(std::max(1, s.panel.nx)) + " x " + std::to_string(std::max(1, s.panel.ny));
    out["order"] = o;
    // The fab's published capability, checked as the DRC's DFM report does.
    std::string pack;
    for (const auto& id2 : fab->packs)
        if (const DfmPack* p = findDfmPack(id2); p && (pack.empty() || layers > findDfmPack(pack)->maxLayers)) pack = id2;
    out["pack"] = pack;
    if (!pack.empty()) {
        PcbLayout pcb = project.pcb;
        if (pcb.settings.dfmPack != pack) pcb.settings.dfmOverrides.clear();
        pcb.settings.dfmPack = pack;
        const Json report = dfmReportJson(project.schematic, pcb);
        out["checks"] = report.get("rows");
        out["fits"] = report.get("pass");
    } else {
        out["checks"] = Json::array();
        out["fits"] = Json();  // no published limits: check the fab's capability page
    }
    const Json& t = request.get("priceTable");
    if (t.isObject()) {
        auto num = [](const Json& v, double d) {
            const double x = v.asNumber(d);
            return std::isfinite(x) && x >= 0 ? x : d;
        };
        double layerFactor = num(t.get("layers").get(std::to_string(layers)), 1), qtyFactor = 1;
        for (const auto& b : t.get("quantityFactors").items())
            if (b.isArray() && b.size() == 2 && quantity >= b[0].asNumber(1e18)) qtyFactor = num(b[1], qtyFactor);
        Json e = Json::object(), lines = Json::array();
        double total = 0;
        auto line = [&](const std::string& what, double amount) {
            if (amount <= 0) return;
            Json l = Json::object();
            l["item"] = what, l["amount"] = amount;
            lines.push(l);
            total += amount;
        };
        line("Setup", num(t.get("setup"), 0));
        line("Boards: " + std::to_string(quantity) + " x " + fmt("%.1f", area) + " cm2, " + std::to_string(layers) + " layers",
             num(t.get("perCm2"), 0) * area * quantity * layerFactor * qtyFactor);
        const Json& opt = t.get("options");
        if (finish.rfind("ENIG", 0) == 0) line("ENIG finish", num(opt.get("ENIG"), 0));
        if (!s.pairRules.empty()) line("Impedance control", num(opt.get("impedance"), 0));
        if (s.hdi) line("HDI (blind / buried / microvias)", num(opt.get("hdi"), 0));
        if (s.viaInPad) line("Via-in-pad, filled and capped", num(opt.get("viaInPad"), 0));
        if (s.solderMask != "green") line("Mask colour " + colourName(s.solderMask), num(opt.get("colour"), 0));
        const double minimum = num(t.get("minimum"), 0);
        if (total < minimum) line("Minimum order", minimum - total);
        e["currency"] = t.get("currency").asString("");
        e["lines"] = lines;
        e["total"] = total;
        e["perBoard"] = total / quantity;
        if (t.has("leadDays")) e["leadDays"] = num(t.get("leadDays"), 0);
        e["source"] = "your price table";
        out["estimate"] = e;
    }
    Json steps = Json::array();
    steps.push("Export the fabrication package (File > Export Fabrication Package).");
    steps.push("Open " + fab->orderUrl + " and upload <name>-gerbers.zip.");
    steps.push("Enter the order values above where the form asks for them; attach fab_drawing.pdf for notes.");
    if (pack.empty()) steps.push("Check the board against " + fab->name + "'s capability page: SiEDA has no published limits for it.");
    out["steps"] = steps;
    return out;
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
        const auto embedded = embeddedElements(project.schematic, s);
        if (!embedded.empty()) {
            o << "EMBEDDED PASSIVES (formed inside the board — not assembled)\n";
            for (const auto& e : embedded) {
                const Component* c = project.schematic.find(e.componentId);
                const std::string ref = c ? c->ref : "?";
                if (e.resistor)
                    o << "  " << ref << "  " << fmt("%g", e.value) << " ohm  L" << e.layer + 1 << "  " << e.material
                      << ", " << fmt("%.2f", e.squares) << " sq = " << fmt("%.2f", e.width) << " x "
                      << fmt("%.2f", e.length) << " mm, +/-" << fmt("%.0f", kEmbeddedResistorTolerance)
                      << " % untrimmed\n";
                else
                    o << "  " << ref << "  " << fmt("%g", e.value * 1e9) << " nF  L" << e.layer + 1 << "-L"
                      << e.layer2 + 1 << "  " << e.material << ", plates " << fmt("%.2f", e.width) << " x "
                      << fmt("%.2f", e.width) << " mm\n";
            }
            o << "  Resistor element artwork: *-In<n>_Resistor.gbr (keep the foil inside each rectangle); laser trim "
                 "where the BOM tolerance is tighter than +/-10 %. Test each element after lamination.\n\n";
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
    o << "  Non-plated           " << f.npth << " (mounting holes)\n";
    {  // Drill chart (IPC-2615): finished hole sizes with their counts, as on a drill drawing.
        std::map<long, std::array<int, 3>> chart;  // µm → plated pads, vias, non-plated
        for (const auto& p : project.pcb.pads(project.schematic))
            if (p.throughHole && p.drill > 0) ++chart[std::lround(p.drill * 1000)][p.npth ? 2 : 0];  // NPTH pads: non-plated
        for (const auto& v : project.pcb.vias) ++chart[std::lround(v.drill * 1000)][1];
        for (const auto& h : s.holes) ++chart[std::lround(h.drill * 1000)][2];
        if (!chart.empty()) {
            o << "  Drill chart          finished size   plated pads   vias   non-plated\n";
            for (const auto& [um, n] : chart)
                o << "                       " << fmt("%6.3f", um / 1000.0) << " mm   " << fmt("%11.0f", n[0]) << "   "
                  << fmt("%4.0f", n[1]) << "   " << fmt("%10.0f", n[2]) << "\n";
            o << "  Hole tolerance       finished size, plated +/-0.08 mm, non-plated +/-0.05 mm\n";
        }
    }
    o << "\n";
    o << "ASSEMBLY\n";
    o << "  Parts                " << f.partsTop << " top, " << f.partsBottom << " bottom (" << f.tht
      << " through-hole)\n";
    {
        BomSummary bom = summarizeBom(buildBom(project.variantSchematic(project.activeVariant)));
        if (!project.activeVariant.empty()) o << "  Variant              " << project.activeVariant << "\n";
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

std::vector<FabFile> fabricationPackage(const Project& project, const std::string& baseName, bool assembly3d) {
    // Router and pour copper is computed in floating point; the file keeps it on the 1 nm grid. Build the package
    // from the board as saved, so one made before saving matches one made after reopening byte for byte.
    auto off = [](double v) { return nm(v) != v; };
    auto offVec = [&](Vec2 p) { return off(p.x) || off(p.y); };
    const auto& tracks = project.pcb.tracks;
    const auto& vias = project.pcb.vias;
    if (std::any_of(tracks.begin(), tracks.end(), [&](const Track& t) {
            return offVec(t.a) || offVec(t.b) || off(t.width) || (t.arc && offVec(t.mid));
        }) ||
        std::any_of(vias.begin(), vias.end(), [&](const Via& v) {
            return offVec(v.position) || off(v.drill) || off(v.diameter);
        })) {
        Project onGrid = project;
        auto snap = [](Vec2& p) { p = {nm(p.x), nm(p.y)}; };
        for (auto& t : onGrid.pcb.tracks) {
            snap(t.a), snap(t.b), t.width = nm(t.width);
            if (t.arc) snap(t.mid);
        }
        for (auto& v : onGrid.pcb.vias) snap(v.position), v.drill = nm(v.drill), v.diameter = nm(v.diameter);
        return fabricationPackage(onGrid, baseName, assembly3d);
    }
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
    for (int u = 1; u <= 4; ++u)
        if (std::string user = exportUserLayerGerber(pcb, u); !user.empty())
            add(g + "-User_" + std::to_string(u) + ".gbr", user, "User drawing layer " + std::to_string(u));
    if (f.pth || f.vias) add(g + "-PTH.drl", exportExcellonDrill(sch, pcb, true), "Drill, plated (pads and vias)");
    for (int layer = 1; layer + 1 < layers; ++layer)
        if (std::string g2 = exportEmbeddedResistorGerber(sch, pcb, layer); !g2.empty())
            add(g + "-In" + std::to_string(layer) + "_Resistor.gbr", g2,
                "Embedded resistor foil, layer " + std::to_string(layer + 1) + " (thin-film element artwork)");
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

    // Assembly files follow the active design variant (fitted parts and values); the board is the same for all.
    const Schematic variantSch = project.activeVariant.empty() ? Schematic() : project.variantSchematic(project.activeVariant);
    const Schematic& asmSch = project.activeVariant.empty() ? sch : variantSch;
    const std::string variantNote = project.activeVariant.empty() ? "" : " (variant " + project.activeVariant + ")";
    const std::string a = "assembly/" + base;
    add(a + "-bom.csv", exportBomCsv(asmSch), "Bill of materials" + variantNote);
    add(a + "-bom_assembly.csv", exportAssemblyBomCsv(asmSch), "BOM for assembly houses (JLCPCB / PCBWay columns)" + variantNote);
    add(a + "-cpl.csv", exportCplCsv(asmSch, pcb), "Component placement list (CPL) for assembly" + variantNote);
    add(a + "-pick_and_place.csv", exportPickAndPlaceCsv(asmSch, pcb), "Pick and place (all columns)" + variantNote);
    add(a + "-assembly_top.svg", exportAssemblySvg(asmSch, pcb, false, project.name), "Assembly drawing, top" + variantNote);
    if (f.partsBottom)
        add(a + "-assembly_bottom.svg", exportAssemblySvg(asmSch, pcb, true, project.name), "Assembly drawing, bottom" + variantNote);
    add(a + "-test_points.csv", exportTestPointReport(asmSch, pcb), "Test points per net for the bed-of-nails / flying-probe fixture");
    add(a + "-interactive_bom.html", exportHtmlBom(project), "Interactive BOM and board viewer for hand assembly" + variantNote);
    add(base + "-bring_up_checklist.md", bringUpChecklist(project), "Bring-up checklist: shorts, rails, clocks, resets, interfaces");
    add(base + "-fab_drawing.pdf", exportBoardDrawingPdf(project, assembly3d),
        "Fabrication drawing: outline, dimensions, drill map and table, stack-up, notes, 3D / exploded / section views");
    add(base + "-net_lengths.csv", exportNetLengthReport(sch, pcb), "Routed length, segments and vias per net, against its length rule");
    add("mechanical/" + base + ".dxf", exportDxf(sch, pcb), "Board outline, holes, silk and courtyards for MCAD (DXF)");
    add("mechanical/" + base + ".FCMacro", exportMcadScript(sch, pcb, project.name, "freecad"),
        "FreeCAD macro: board and parts; run again to send the placement back (IDF)");
    add("mechanical/" + base + "_fusion.py", exportMcadScript(sch, pcb, project.name, "fusion"),
        "Autodesk Fusion script: board and parts; run again to send the placement back (IDF)");
    add(base + "-netlist.cir", exportSpiceNetlist(sch, project.name), "SPICE netlist");
    if (assembly3d) add("3d/" + base + ".stl", exportStl(buildAssemblyMesh(sch, pcb), base), "3D assembly (STL)");
    add(base + "-gerbers.zip", makeZip(zipped), "Gerbers + drills + job file + IPC netlist: upload this to the fab");
    add(base + "-ipc2581.xml", exportIpc2581(project), "IPC-2581C: copper, mask, silk, drills, nets, parts and BOM in one file");
    add(base + "-odb.tgz", exportOdbArchive(project), "ODB++ v7 job (.tgz): layers, drills, components and nets");
    for (auto& f : panelFiles(pcb, base, files)) files.push_back(std::move(f));
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
