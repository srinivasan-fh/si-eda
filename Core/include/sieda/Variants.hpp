// SiEDA Core — design variants: one schematic and board, several assembly options (fitted / not fitted, values).
#pragma once

#include <map>
#include <string>
#include <vector>

#include "sieda/Json.hpp"
#include "sieda/Schematic.hpp"

namespace sieda {

/// What a variant changes on one component. Anything left at its default follows the base design.
struct VariantPart {
    /// -1 = as in the base design (Sourcing::dnp), 0 = not fitted (DNP) in this variant, 1 = fitted.
    int fitted = -1;
    /// Value fitted in this variant ("" = the base value).
    std::string value;
    bool empty() const { return fitted < 0 && value.empty(); }
};

/// A named assembly variant ("Lite", "Pro", "EU"…). Keyed by component id, so designators may be re-annotated.
struct DesignVariant {
    std::string name;
    std::string description;
    std::map<int, VariantPart> parts;
};

/// The schematic as variant `v` is assembled: parts it does not fit are marked DNP and value overrides applied.
/// Connectivity is unchanged (a DNP part keeps its footprint and copper); net symbols are never affected.
Schematic applyVariant(const Schematic& base, const DesignVariant& v);

/// {"name","description","parts":[{"component","ref","fitted"?,"value"?}]} — parts of components that no longer
/// exist are left out.
Json variantToJson(const DesignVariant& v, const Schematic& sch);
/// Parses a variant (unknown or invalid part entries are skipped). Throws JsonError without a name.
DesignVariant variantFromJson(const Json& j);

}  // namespace sieda
