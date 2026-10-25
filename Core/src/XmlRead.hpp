#pragma once
// Minimal XML reader shared by the IDX (Mechanical.cpp) and IPC-2581 route (RouteExchange.cpp) importers.
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace sieda {

/// Minimal XML reader for IDX: elements by local name (namespace prefixes dropped), attributes, text. Comments,
/// declarations and processing instructions are skipped; CDATA is text. Nesting is capped (hostile files).
struct XmlNode {
    std::string name, text;
    std::map<std::string, std::string> attrs;
    std::vector<XmlNode> kids;
    const XmlNode* child(const std::string& n) const {
        for (const auto& k : kids)
            if (k.name == n) return &k;
        return nullptr;
    }
    /// The element's number: its text, or its Value child's (IDX writes <tx><Value>1.5</Value></tx>).
    double number(double fallback = 0) const {
        const std::string& t = text.find_first_not_of(" \t\r\n") != std::string::npos ? text : (child("Value") ? child("Value")->text : text);
        char* end = nullptr;
        const double v = std::strtod(t.c_str(), &end);
        return end != t.c_str() && std::isfinite(v) ? v : fallback;
    }
};

/// Parses `s` into `root`'s children; false on malformed nesting or more than 256 levels.
bool parseXml(const std::string& s, XmlNode& root);

}  // namespace sieda
