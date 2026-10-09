// SiEDA Core — distributor part data: one normalised schema for manufacturer parts, their supplier offers (stock,
// price breaks, packaging, lead time) and lifecycle, read from the raw replies of the Nexar / Octopart GraphQL API,
// the DigiKey Product Information API v4 and the Mouser Search API v2.
//
// The app does the network calls (with the user's own API keys) and hands the reply bodies to the core, so every
// parser here is pure, deterministic and tested with recorded fixtures. Replies are untrusted input: size, nesting,
// counts, string lengths and numbers are all limited and nothing throws out of the public functions.
//
// Also here: price-break arithmetic (MOQ, order multiples, buying up to a cheaper break), the best offer for a
// quantity, the BOM cost roll-up per build quantity with stock and lifecycle (EOL / NRND) warnings, and matching a
// manufacturer part number to a part of the standard catalog.
#pragma once

#include <string>
#include <vector>

#include "sieda/Json.hpp"

namespace sieda {

/// Lifecycle of a manufacturer part, from the distributor's status text.
enum class PartLifecycle { Unknown = 0, Active, New, Nrnd, Eol, Obsolete };
/// "unknown", "active", "new", "nrnd", "eol", "obsolete".
const char* partLifecycleName(PartLifecycle l);
PartLifecycle partLifecycleFromName(const std::string& name);
/// Maps a status text ("Active", "Not Recommended for New Designs", "End of Life", "Last Time Buy", "Obsolete",
/// "Discontinued at Digi-Key", "New Product", "Production", "NRND", "EOL"…) to a lifecycle.
PartLifecycle partLifecycleFromText(const std::string& text);
/// True for lifecycles a design review should flag (NRND, EOL, obsolete).
bool lifecycleNeedsAttention(PartLifecycle l);

struct PriceBreak {
    int quantity = 1;
    double price = 0;  // unit price at this quantity and above
};

/// One supplier's offer of a part (one packaging: cut tape, reel, tube…).
struct SupplierOffer {
    std::string supplier;   // "Digi-Key", "Mouser", "Farnell", …
    std::string sku;        // the supplier's own part number
    long long stock = -1;   // units in stock; -1 = not reported
    int moq = 1;            // minimum order quantity
    int multiple = 1;       // order multiple
    std::string packaging;  // "Cut Tape", "Tape & Reel", "Tube"…
    std::string currency;   // ISO 4217 ("USD")
    int leadTimeDays = -1;  // factory lead time; -1 = not reported
    std::string url;
    std::vector<PriceBreak> prices;  // ascending quantity, positive prices
};

struct SupplierParameter {
    std::string name, value;
};

/// A manufacturer part with every offer found for it.
struct SupplierPart {
    std::string mpn, manufacturer, description, category, package;
    std::string datasheet, productUrl, imageUrl;
    PartLifecycle lifecycle = PartLifecycle::Unknown;
    std::string lifecycleText;  // the distributor's own words
    std::string rohs;
    std::vector<SupplierParameter> parameters;
    std::vector<SupplierOffer> offers;
    std::vector<std::string> sources;  // "nexar", "digikey", "mouser"
    long long totalStock() const;      // sum of reported stock (-1 when no offer reports any)
};

/// A distributor reply read into the schema. `error` is set (and `errorKind` says why: "auth", "rate_limit",
/// "quota", "bad_request", "service", "parse", "source") when the reply is an error or cannot be read.
struct SupplierSearchResult {
    std::string source;
    int total = 0;  // matches the distributor reports (may exceed parts.size())
    std::vector<SupplierPart> parts;
    std::string error, errorKind;
    std::vector<std::string> notes;  // entries skipped, limits hit
};

/// Limits applied to untrusted replies.
struct SupplierLimits {
    static constexpr size_t maxBody = 16u << 20;  // bytes
    static constexpr int maxDepth = 64;           // JSON nesting
    static constexpr size_t maxParts = 200;
    static constexpr size_t maxOffers = 64;
    static constexpr size_t maxPriceBreaks = 32;
    static constexpr size_t maxParameters = 60;
    static constexpr size_t maxString = 2000;     // bytes per text field
};

SupplierSearchResult parseNexarResponse(const std::string& body);
SupplierSearchResult parseDigikeyResponse(const std::string& body);
SupplierSearchResult parseMouserResponse(const std::string& body);
/// Dispatches on `source` ("nexar", "octopart", "digikey", "mouser"); an unknown source is an error result.
SupplierSearchResult parseSupplierResponse(const std::string& source, const std::string& body);

/// A price as distributors print it: "$0.452", "0,45 €", "1.234,56 €", "¥63", "1,234.50". NaN when unreadable.
double parsePriceText(const std::string& text);
/// Upper-case letters and digits only ("LM358DR", "AMS11173.3" → "AMS111733"): the key parts are matched on.
std::string normalizeMpn(const std::string& mpn);

/// What ordering `quantity` units from one offer costs.
struct PriceQuote {
    bool ok = false;           // false: the offer has no price
    int orderQuantity = 0;     // after MOQ, order multiple and, if cheaper, buying up to the next break
    double unitPrice = 0;
    double extended = 0;       // unitPrice × orderQuantity
    bool inStock = false;      // stock reported and ≥ orderQuantity
    std::string currency;
};
PriceQuote quoteOffer(const SupplierOffer& offer, int quantity);

/// The offer to buy `quantity` from: in stock first, then the lowest extended price; offers in `currency` first
/// ("" = any). `offerIndex` is -1 when no offer has a price.
struct OfferChoice {
    int offerIndex = -1;
    PriceQuote quote;
};
OfferChoice bestOffer(const SupplierPart& part, int quantity, const std::string& currency = "");

/// Parts merged across replies: same normalised MPN and manufacturer (or a missing manufacturer) become one part
/// with every offer (duplicate supplier + SKU offers dropped) and the first non-empty field of each kind.
std::vector<SupplierPart> mergeSupplierParts(const std::vector<SupplierSearchResult>& results);

Json supplierPartToJson(const SupplierPart& part, const std::string& currency = "");
/// Reads a part back from its JSON (the app's offline cache); lenient, never throws.
SupplierPart supplierPartFromJson(const Json& j);
/// {"schema":"sieda.supplier/1","source","total","error","errorKind","notes":[],"parts":[…]}.
/// Each part also carries "pricing": best unit price at 1, 10, 100 and 1000 pieces, and "stock".
Json supplierSearchToJson(const SupplierSearchResult& result, const std::string& currency = "");

/// BOM cost roll-up. request:
///   {"currency":"USD","quantities":[1,10,100,1000],"buildQuantity":5,"attritionPercent":2,"attritionMin":5 (spares per
///    line for assembly loss: the larger of the percentage and the minimum; both 0 by default),
///    "lines":[{"item","refs":[…],"quantity":per board,"mpn","manufacturer","dnp","embedded"}],
///    "parts":[normalised parts]}
/// → {"currency","lines":[{"item","refs","mpn","status":"priced|unpriced|no_mpn|not_found|dnp","lifecycle",
///     "lifecycleText","warning","datasheet","stock","offers":[{"boards","needed","orderQuantity","supplier","sku",
///     "unitPrice","extended","currency","inStock"}]}],
///    "totals":[{"boards","cost","perBoard","priced","unpriced","shortages","complete"}],
///    "warnings":[…],"mixedCurrency":bool}
Json supplierBomRollup(const Json& request);

/// Standard-catalog part for a manufacturer part number: the exact name, else a catalog part whose name is the MPN
/// less a packaging suffix (LM358D for LM358DR, …#PBF for …#TRPBF); `candidates` lists same-family parts that
/// need a check (different package). {"match":"<name>"|"","exact":bool,"note","candidates":[names]}.
Json catalogMatchForMpn(const std::string& mpn);

/// C API entry points (never throw).
Json supplierParseRequest(const std::string& source, const std::string& body, const std::string& currency);
/// {"results":[normalised results],"currency"} → {"parts":[merged],"errors":[{"source","error","errorKind"}]}
Json supplierMergeRequest(const Json& request);

}  // namespace sieda
