// SiEDA Core — distributor replies (Nexar / Octopart, DigiKey v4, Mouser v2) read into one schema, price breaks,
// best offers, the BOM cost roll-up and catalog matching. See Suppliers.hpp and docs/SUPPLIERS.md.
#include "sieda/Suppliers.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>

#include "sieda/StandardParts.hpp"

namespace sieda {

// ------------------------------------------------------------------ lifecycle

const char* partLifecycleName(PartLifecycle l) {
    switch (l) {
        case PartLifecycle::Active: return "active";
        case PartLifecycle::New: return "new";
        case PartLifecycle::Nrnd: return "nrnd";
        case PartLifecycle::Eol: return "eol";
        case PartLifecycle::Obsolete: return "obsolete";
        case PartLifecycle::Unknown: break;
    }
    return "unknown";
}

PartLifecycle partLifecycleFromName(const std::string& name) {
    for (PartLifecycle l : {PartLifecycle::Active, PartLifecycle::New, PartLifecycle::Nrnd, PartLifecycle::Eol,
                            PartLifecycle::Obsolete})
        if (name == partLifecycleName(l)) return l;
    return PartLifecycle::Unknown;
}

namespace {
std::string lowerAscii(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// True if `word` appears in `text` as a whole word (both lower case).
bool hasWord(const std::string& text, const std::string& word) {
    for (size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1)) {
        const bool startOk = at == 0 || !std::isalnum(static_cast<unsigned char>(text[at - 1]));
        const size_t end = at + word.size();
        const bool endOk = end >= text.size() || !std::isalnum(static_cast<unsigned char>(text[end]));
        if (startOk && endOk) return true;
    }
    return false;
}

int severity(PartLifecycle l) {
    switch (l) {
        case PartLifecycle::Obsolete: return 5;
        case PartLifecycle::Eol: return 4;
        case PartLifecycle::Nrnd: return 3;
        case PartLifecycle::New: return 2;
        case PartLifecycle::Active: return 1;
        case PartLifecycle::Unknown: break;
    }
    return 0;
}
}  // namespace

PartLifecycle partLifecycleFromText(const std::string& raw) {
    const std::string t = lowerAscii(raw);
    if (t.empty()) return PartLifecycle::Unknown;
    // Order matters: "Not Recommended for New Designs" mentions "new", "Discontinued" is not "active".
    if (t.find("obsolete") != std::string::npos || t.find("discontinued") != std::string::npos ||
        t.find("no longer") != std::string::npos || hasWord(t, "obs"))
        return PartLifecycle::Obsolete;
    if (t.find("end of life") != std::string::npos || t.find("end-of-life") != std::string::npos || hasWord(t, "eol") ||
        t.find("last time buy") != std::string::npos || hasWord(t, "ltb"))
        return PartLifecycle::Eol;
    if (t.find("not recommended") != std::string::npos || hasWord(t, "nrnd") ||
        t.find("not for new design") != std::string::npos)
        return PartLifecycle::Nrnd;
    if (t.find("preliminary") != std::string::npos || t.find("new product") != std::string::npos ||
        t.find("pre-release") != std::string::npos || t.find("prerelease") != std::string::npos ||
        t.find("introduction") != std::string::npos || hasWord(t, "new") || t.find("sampling") != std::string::npos)
        return PartLifecycle::New;
    if (t.find("active") != std::string::npos || t.find("production") != std::string::npos ||
        t.find("in stock") != std::string::npos)
        return PartLifecycle::Active;
    return PartLifecycle::Unknown;
}

bool lifecycleNeedsAttention(PartLifecycle l) {
    return l == PartLifecycle::Nrnd || l == PartLifecycle::Eol || l == PartLifecycle::Obsolete;
}

long long SupplierPart::totalStock() const {
    long long sum = -1;
    for (const auto& o : offers)
        if (o.stock >= 0) sum = (sum < 0 ? 0 : sum) + o.stock;
    return sum;
}

// ------------------------------------------------------------------ text and numbers

namespace {
constexpr double kMaxPrice = 1e7;
constexpr long long kMaxCount = 1000000000000LL;  // 10^12 units
constexpr int kMaxQuantity = 1000000000;           // 10^9

/// Valid UTF-8 with control characters as spaces, runs of white space collapsed, trimmed and cut to `limit` bytes
/// on a character boundary.
std::string cleanText(const std::string& s, size_t limit = SupplierLimits::maxString) {
    std::string out;
    out.reserve(std::min(s.size(), limit));
    bool space = false;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        bool valid = true;
        if (c < 0x80) {
            len = 1;
        } else if ((c & 0xE0) == 0xC0 && c >= 0xC2) {
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
        } else if ((c & 0xF8) == 0xF0 && c <= 0xF4) {
            len = 4;
        } else {
            valid = false;
        }
        if (valid && i + len > s.size()) valid = false;
        for (size_t k = 1; valid && k < len; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) valid = false;
        if (valid && len == 3) {  // no overlongs or surrogates
            const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0)) valid = false;
        }
        if (valid && len == 4) {
            const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            if ((c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 >= 0x90)) valid = false;
        }
        if (!valid) len = 1;
        const bool white = valid && len == 1 && (c < 0x20 || c == 0x7F || c == ' ');
        if (white) {
            space = !out.empty();
            i += len;
            continue;
        }
        const size_t need = (space ? 1 : 0) + (valid ? len : 1);
        if (out.size() + need > limit) break;
        if (space) out += ' ';
        space = false;
        if (valid) out.append(s, i, len);
        else out += '?';
        i += len;
    }
    return out;
}

/// A JSON value as text: strings cleaned, numbers printed, anything else empty.
std::string text(const Json& j, size_t limit = SupplierLimits::maxString) {
    if (j.isString()) return cleanText(j.asString(), limit);
    if (j.isNumber()) {
        const double v = j.asNumber();
        if (!std::isfinite(v)) return {};
        if (v == std::floor(v) && std::fabs(v) < 1e15) return std::to_string(static_cast<long long>(v));
        char buf[32];
        std::snprintf(buf, sizeof buf, "%g", v);
        return buf;
    }
    return {};
}

/// http(s) links only ("//host/…" becomes https); anything else (javascript:, file:, relative) is dropped.
std::string link(const Json& j) {
    std::string u = text(j);
    if (u.rfind("//", 0) == 0) u = "https:" + u;
    const std::string l = lowerAscii(u.substr(0, 8));
    if (l.rfind("https://", 0) != 0 && l.rfind("http://", 0) != 0) return {};
    if (u.find(' ') != std::string::npos) return {};
    return u;
}

/// A count from a number or text ("1234", "1,234", "1234 In Stock"); -1 when there is none.
long long count(const Json& j) {
    if (j.isNumber()) {
        const double v = j.asNumber();
        if (!std::isfinite(v) || v < 0) return -1;
        return static_cast<long long>(std::min(v, static_cast<double>(kMaxCount)));
    }
    if (!j.isString()) return -1;
    const std::string& s = j.asString();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    long long v = 0;
    bool any = false;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (c >= '0' && c <= '9') {
            any = true;
            v = std::min(kMaxCount, v * 10 + (c - '0'));
        } else if ((c == ',' || c == '\'' || c == '.') && any && i + 1 < s.size() &&
                   std::isdigit(static_cast<unsigned char>(s[i + 1]))) {
            continue;  // thousands separator
        } else {
            break;
        }
    }
    return any ? v : -1;
}

int clampInt(long long v, int lo, int hi) { return static_cast<int>(std::clamp<long long>(v, lo, hi)); }

/// Lead time text ("42 Days", "6 Weeks", "12") in days; `weeks` is the unit of a bare number. -1 when unreadable.
int leadDays(const Json& j, bool weeks) {
    const long long n = count(j);
    if (n < 0) return -1;
    std::string t = lowerAscii(text(j));
    bool w = weeks;
    if (t.find("week") != std::string::npos || t.find("wk") != std::string::npos) w = true;
    if (t.find("day") != std::string::npos) w = false;
    return clampInt(w ? n * 7 : n, 0, 3650);
}

/// Pre-scan: the reply's nesting stays within the limit (the JSON reader itself recurses).
bool nestingWithin(const std::string& s, int limit) {
    int depth = 0;
    bool inString = false, escape = false;
    for (char c : s) {
        if (inString) {
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') inString = true;
        else if (c == '[' || c == '{') {
            if (++depth > limit) return false;
        } else if (c == ']' || c == '}') {
            --depth;
        }
    }
    return true;
}

bool readBody(const std::string& body, SupplierSearchResult& r, Json& root) {
    if (body.size() > SupplierLimits::maxBody) {
        r.error = "The reply is larger than 16 MB.";
        r.errorKind = "parse";
        return false;
    }
    size_t start = body.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        r.error = "The reply is empty.";
        r.errorKind = "parse";
        return false;
    }
    if (body[start] != '{' && body[start] != '[') {
        r.error = "The reply is not JSON (" + cleanText(body.substr(start, 80), 80) + ").";
        r.errorKind = body[start] == '<' ? "service" : "parse";
        return false;
    }
    if (!nestingWithin(body, SupplierLimits::maxDepth)) {
        r.error = "The reply is nested too deeply.";
        r.errorKind = "parse";
        return false;
    }
    try {
        root = Json::parse(body);
    } catch (const std::exception& e) {
        r.error = std::string("The reply cannot be read: ") + cleanText(e.what(), 200);
        r.errorKind = "parse";
        return false;
    }
    if (!root.isObject()) {
        r.error = "The reply is not a JSON object.";
        r.errorKind = "parse";
        return false;
    }
    return true;
}

/// Price ladder: positive finite prices at quantities ≥ 1, ascending, one per quantity.
void addBreak(SupplierOffer& o, long long quantity, double price) {
    if (!std::isfinite(price) || price <= 0 || price > kMaxPrice || quantity < 1) return;
    if (o.prices.size() >= SupplierLimits::maxPriceBreaks) return;
    const int q = clampInt(quantity, 1, kMaxQuantity);
    for (const auto& b : o.prices)
        if (b.quantity == q) return;
    o.prices.push_back({q, price});
}

void finishOffer(SupplierOffer& o) {
    std::sort(o.prices.begin(), o.prices.end(), [](const PriceBreak& a, const PriceBreak& b) { return a.quantity < b.quantity; });
    o.moq = std::clamp(o.moq, 1, kMaxQuantity);
    o.multiple = std::clamp(o.multiple, 1, kMaxQuantity);
    if (o.currency.size() > 8) o.currency.resize(8);
    for (auto& c : o.currency) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
}

std::string currencyCode(const Json& j) {
    std::string c = text(j, 16);
    std::string out;
    for (char ch : c)
        if (std::isalpha(static_cast<unsigned char>(ch))) out += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return out.size() == 3 ? out : std::string();
}

void addParameter(SupplierPart& p, const std::string& name, const std::string& value) {
    if (name.empty() || value.empty() || p.parameters.size() >= SupplierLimits::maxParameters) return;
    for (const auto& q : p.parameters)
        if (q.name == name && q.value == value) return;
    p.parameters.push_back({cleanText(name, 120), cleanText(value, 300)});
}

void addPart(SupplierSearchResult& r, SupplierPart p) {
    if (p.mpn.empty()) {
        r.notes.push_back("An entry without a manufacturer part number was skipped.");
        return;
    }
    if (r.parts.size() >= SupplierLimits::maxParts) {
        if (r.notes.empty() || r.notes.back().rfind("Only the first", 0) != 0)
            r.notes.push_back("Only the first " + std::to_string(SupplierLimits::maxParts) + " parts were read.");
        return;
    }
    for (auto& o : p.offers) finishOffer(o);
    if (p.lifecycle == PartLifecycle::Unknown) p.lifecycle = partLifecycleFromText(p.lifecycleText);
    p.sources = {r.source};
    r.parts.push_back(std::move(p));
}

std::string errorKindForStatus(long long status) {
    if (status == 401 || status == 403) return "auth";
    if (status == 429) return "rate_limit";
    if (status >= 500) return "service";
    return "bad_request";
}
}  // namespace

double parsePriceText(const std::string& raw) {
    // Keep digits and separators of the first number in the text.
    std::string s;
    bool started = false;
    for (char c : raw) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            started = true;
            s += c;
        } else if ((c == '.' || c == ',') && started) {
            s += c;
        } else if (started && (c == ' ' || c == '\'')) {
            continue;  // "1 234,56"
        } else if (started) {
            break;
        }
    }
    while (!s.empty() && (s.back() == '.' || s.back() == ',')) s.pop_back();
    if (s.empty()) return std::numeric_limits<double>::quiet_NaN();
    const size_t lastDot = s.rfind('.'), lastComma = s.rfind(',');
    std::string num;
    if (lastDot != std::string::npos && lastComma != std::string::npos) {
        const char decimal = lastDot > lastComma ? '.' : ',';
        for (char c : s)
            if (std::isdigit(static_cast<unsigned char>(c))) num += c;
            else if (c == decimal) num += '.';
    } else if (lastComma != std::string::npos) {
        // Only commas: "0,452" / "12,5" are decimals; "1,234" / "12,345,678" are thousands.
        const size_t commas = static_cast<size_t>(std::count(s.begin(), s.end(), ','));
        const size_t after = s.size() - lastComma - 1;
        const bool decimal = commas == 1 && (after != 3 || s.compare(0, 2, "0,") == 0);
        for (char c : s)
            if (std::isdigit(static_cast<unsigned char>(c))) num += c;
            else if (decimal) num += '.';
    } else {
        // Only dots: several dots are thousands ("1.234.567"); one dot is the decimal point.
        const size_t dots = static_cast<size_t>(std::count(s.begin(), s.end(), '.'));
        for (char c : s)
            if (std::isdigit(static_cast<unsigned char>(c))) num += c;
            else if (dots == 1) num += '.';
    }
    if (num.empty() || num.size() > 24) return std::numeric_limits<double>::quiet_NaN();
    char* end = nullptr;
    const double v = std::strtod(num.c_str(), &end);
    if (!end || *end != '\0' || !std::isfinite(v)) return std::numeric_limits<double>::quiet_NaN();
    return v;
}

std::string normalizeMpn(const std::string& mpn) {
    std::string out;
    for (char c : mpn)
        if (std::isalnum(static_cast<unsigned char>(c))) out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// ------------------------------------------------------------------ Mouser Search API v2

SupplierSearchResult parseMouserResponse(const std::string& body) {
    SupplierSearchResult r;
    r.source = "mouser";
    Json root;
    if (!readBody(body, r, root)) return r;
    const Json& results = root.get("SearchResults");
    for (const auto& p : results.get("Parts").items()) {
        if (!p.isObject()) continue;
        SupplierPart part;
        part.mpn = text(p.get("ManufacturerPartNumber"), 120);
        part.manufacturer = text(p.get("Manufacturer"), 200);
        part.description = text(p.get("Description"));
        part.category = text(p.get("Category"), 200);
        part.datasheet = link(p.get("DataSheetUrl"));
        part.productUrl = link(p.get("ProductDetailUrl"));
        part.imageUrl = link(p.get("ImagePath"));
        part.lifecycleText = text(p.get("LifecycleStatus"), 120);
        part.rohs = text(p.get("ROHSStatus"), 120);
        SupplierOffer o;
        o.supplier = "Mouser";
        o.sku = text(p.get("MouserPartNumber"), 120);
        if (o.sku == "N/A") o.sku.clear();
        o.stock = p.has("AvailabilityInStock") && !p.get("AvailabilityInStock").isNull() ? count(p.get("AvailabilityInStock"))
                                                                                        : count(p.get("Availability"));
        if (o.stock < 0 && lowerAscii(text(p.get("Availability"))).find("none") != std::string::npos) o.stock = 0;
        o.moq = clampInt(std::max(1LL, count(p.get("Min"))), 1, kMaxQuantity);
        o.multiple = clampInt(std::max(1LL, count(p.get("Mult"))), 1, kMaxQuantity);
        o.leadTimeDays = leadDays(p.get("LeadTime"), false);
        o.url = part.productUrl;
        for (const auto& a : p.get("ProductAttributes").items()) {
            const std::string name = text(a.get("AttributeName"), 120), value = text(a.get("AttributeValue"), 300);
            if (name == "Packaging" && o.packaging.empty()) o.packaging = value;
            addParameter(part, name, value);
        }
        for (const auto& b : p.get("PriceBreaks").items()) {
            if (o.currency.empty()) o.currency = currencyCode(b.get("Currency"));
            const Json& price = b.get("Price");
            const double v = price.isNumber() ? price.asNumber() : parsePriceText(text(price, 64));
            addBreak(o, count(b.get("Quantity")), v);
        }
        if (!o.prices.empty() || o.stock >= 0 || !o.sku.empty()) part.offers.push_back(std::move(o));
        addPart(r, std::move(part));
    }
    r.total = clampInt(std::max<long long>(count(results.get("NumberOfResult")), static_cast<long long>(r.parts.size())), 0,
                       kMaxQuantity);
    if (r.parts.empty()) {
        // Mouser reports a bad key, an exhausted quota or a bad request in "Errors" (with HTTP 200).
        for (const auto& e : root.get("Errors").items()) {
            const std::string message = text(e.get("Message"), 400);
            const std::string code = lowerAscii(text(e.get("Code")) + " " + text(e.get("ResourceKey")) + " " + message);
            r.error = message.empty() ? "Mouser reported an error." : "Mouser: " + message;
            if (code.find("identifier") != std::string::npos || code.find("apikey") != std::string::npos ||
                code.find("api key") != std::string::npos || code.find("unauthor") != std::string::npos)
                r.errorKind = "auth";
            else if (code.find("too many") != std::string::npos || code.find("toomany") != std::string::npos ||
                     code.find("rate") != std::string::npos)
                r.errorKind = "rate_limit";
            else if (code.find("exceed") != std::string::npos || code.find("quota") != std::string::npos ||
                     code.find("limit") != std::string::npos)
                r.errorKind = "quota";
            else
                r.errorKind = "bad_request";
            break;
        }
    }
    return r;
}

// ------------------------------------------------------------------ DigiKey Product Information API v4

SupplierSearchResult parseDigikeyResponse(const std::string& body) {
    SupplierSearchResult r;
    r.source = "digikey";
    Json root;
    if (!readBody(body, r, root)) return r;
    // Errors: RFC 7807 problem details (v4: title, status, detail) or the v3 envelope (StatusCode, ErrorMessage).
    const long long status = std::max(count(root.get("status")), count(root.get("StatusCode")));
    if (status >= 400 && !root.has("Products") && !root.has("ExactMatches")) {
        std::string message = text(root.get("detail"), 400);
        if (message.empty()) message = text(root.get("ErrorMessage"), 400);
        if (message.empty()) message = text(root.get("title"), 400);
        if (message.empty()) message = "HTTP " + std::to_string(status);
        r.error = "DigiKey: " + message;
        r.errorKind = errorKindForStatus(status);
        return r;
    }
    std::string currency = currencyCode(root.get("SearchLocaleUsed").get("Currency"));
    if (currency.empty()) currency = "USD";
    std::set<std::string> seen;
    auto read = [&](const Json& p) {
        if (!p.isObject()) return;
        SupplierPart part;
        part.mpn = text(p.has("ManufacturerProductNumber") ? p.get("ManufacturerProductNumber") : p.get("ManufacturerPartNumber"), 120);
        const Json& maker = p.get("Manufacturer");
        part.manufacturer = text(maker.isObject() ? (maker.has("Name") ? maker.get("Name") : maker.get("Value")) : maker, 200);
        const Json& desc = p.get("Description");
        if (desc.isObject()) {
            part.description = text(desc.get("ProductDescription"));
            if (part.description.empty()) part.description = text(desc.get("DetailedDescription"));
        } else {
            part.description = text(p.get("ProductDescription"));
            if (part.description.empty()) part.description = text(p.get("DetailedDescription"));
        }
        const Json& category = p.get("Category");
        part.category = text(category.isObject() ? category.get("Name") : category, 200);
        part.datasheet = link(p.has("DatasheetUrl") ? p.get("DatasheetUrl") : p.get("PrimaryDatasheet"));
        part.productUrl = link(p.get("ProductUrl"));
        part.imageUrl = link(p.has("PhotoUrl") ? p.get("PhotoUrl") : p.get("PrimaryPhoto"));
        const Json& st = p.get("ProductStatus");
        part.lifecycleText = text(st.isObject() ? st.get("Status") : st, 120);
        if (part.lifecycleText.empty() && p.get("EndOfLife").asBool()) part.lifecycleText = "End of Life";
        if (part.lifecycleText.empty() && p.get("Discontinued").asBool()) part.lifecycleText = "Discontinued";
        part.rohs = text(p.get("Classifications").get("RohsStatus"), 120);
        std::string supplierPackage, caseName;
        for (const auto& a : p.get("Parameters").items()) {
            const std::string name = text(a.has("ParameterText") ? a.get("ParameterText") : a.get("Parameter"), 120);
            const std::string value = text(a.has("ValueText") ? a.get("ValueText") : a.get("Value"), 300);
            if (name == "Supplier Device Package") supplierPackage = value;
            if (name == "Package / Case") caseName = value;
            addParameter(part, name, value);
        }
        part.package = !supplierPackage.empty() ? supplierPackage : caseName;
        const int lead = leadDays(p.get("ManufacturerLeadWeeks"), true);
        for (const auto& v : p.get("ProductVariations").items()) {
            if (part.offers.size() >= SupplierLimits::maxOffers) break;
            SupplierOffer o;
            o.supplier = "Digi-Key";
            o.sku = text(v.get("DigiKeyProductNumber"), 120);
            const Json& pt = v.get("PackageType");
            o.packaging = text(pt.isObject() ? pt.get("Name") : pt, 120);
            o.stock = count(v.get("QuantityAvailableforPackageType"));
            o.moq = clampInt(std::max(1LL, count(v.get("MinimumOrderQuantity"))), 1, kMaxQuantity);
            const long long standard = count(v.get("StandardPackage"));
            // Full reels are sold in whole reels; cut tape, tubes and trays by the piece.
            if (lowerAscii(o.packaging).find("reel") != std::string::npos && lowerAscii(o.packaging).find("digi") == std::string::npos &&
                standard > 1)
                o.multiple = clampInt(standard, 1, kMaxQuantity);
            o.currency = currency;
            o.leadTimeDays = lead;
            o.url = part.productUrl;
            for (const auto& b : v.get("StandardPricing").items())
                addBreak(o, count(b.get("BreakQuantity")), b.get("UnitPrice").asNumber(std::numeric_limits<double>::quiet_NaN()));
            part.offers.push_back(std::move(o));
        }
        if (part.offers.empty()) {  // v3 replies, or a product without variations
            SupplierOffer o;
            o.supplier = "Digi-Key";
            o.sku = text(p.get("DigiKeyPartNumber"), 120);
            o.stock = count(p.get("QuantityAvailable"));
            o.moq = clampInt(std::max(1LL, count(p.get("MinimumOrderQuantity"))), 1, kMaxQuantity);
            o.currency = currency;
            o.leadTimeDays = lead;
            o.url = part.productUrl;
            for (const auto& b : p.get("StandardPricing").items())
                addBreak(o, count(b.get("BreakQuantity")), b.get("UnitPrice").asNumber(std::numeric_limits<double>::quiet_NaN()));
            if (o.prices.empty()) addBreak(o, 1, p.get("UnitPrice").asNumber(std::numeric_limits<double>::quiet_NaN()));
            if (!o.prices.empty() || o.stock >= 0 || !o.sku.empty()) part.offers.push_back(std::move(o));
        }
        const std::string key = normalizeMpn(part.mpn) + "|" + lowerAscii(part.manufacturer);
        if (!seen.insert(key).second) return;  // exact match listed again among the products
        addPart(r, std::move(part));
    };
    for (const auto& p : root.get("ExactMatches").items()) read(p);
    for (const auto& p : root.get("Products").items()) read(p);
    r.total = clampInt(std::max<long long>(count(root.get("ProductsCount")), static_cast<long long>(r.parts.size())), 0,
                       kMaxQuantity);
    return r;
}

// ------------------------------------------------------------------ Nexar (Octopart) Supply GraphQL API

SupplierSearchResult parseNexarResponse(const std::string& body) {
    SupplierSearchResult r;
    r.source = "nexar";
    Json root;
    if (!readBody(body, r, root)) return r;
    const Json& data = root.get("data");
    std::vector<const Json*> hits;  // the "part" objects
    long long total = -1;
    for (const char* field : {"supSearchMpn", "supSearch"}) {
        const Json& search = data.get(field);
        if (!search.isObject()) continue;
        total = std::max(total, count(search.get("hits")));
        for (const auto& item : search.get("results").items())
            if (item.get("part").isObject()) hits.push_back(&item.get("part"));
    }
    for (const auto& match : data.get("supMultiMatch").items()) {
        total = std::max(total, 0LL) + std::max(0LL, count(match.get("hits")));
        for (const auto& part : match.get("parts").items())
            if (part.isObject()) hits.push_back(&part);
    }
    for (const Json* pp : hits) {
        const Json& p = *pp;
        SupplierPart part;
        part.mpn = text(p.get("mpn"), 120);
        part.manufacturer = text(p.get("manufacturer").get("name"), 200);
        part.description = text(p.get("shortDescription"));
        if (part.description.empty())
            for (const auto& d : p.get("descriptions").items())
                if (part.description.empty()) part.description = text(d.get("text"));
        part.category = text(p.get("category").get("name"), 200);
        part.datasheet = link(p.get("bestDatasheet").get("url"));
        part.productUrl = link(p.get("octopartUrl"));
        part.imageUrl = link(p.get("bestImage").get("url"));
        for (const auto& s : p.get("specs").items()) {
            const Json& attr = s.get("attribute");
            const std::string shortName = lowerAscii(text(attr.get("shortname"), 120));
            const std::string name = text(attr.get("name"), 120);
            const std::string value = text(s.get("displayValue"), 300);
            if (shortName == "lifecyclestatus") part.lifecycleText = value;
            else if (shortName == "case_package") part.package = value;
            else if (shortName == "rohs") part.rohs = value;
            addParameter(part, name.empty() ? shortName : name, value);
        }
        for (const auto& seller : p.get("sellers").items()) {
            const std::string company = text(seller.get("company").get("name"), 200);
            for (const auto& offer : seller.get("offers").items()) {
                if (part.offers.size() >= SupplierLimits::maxOffers) break;
                SupplierOffer o;
                o.supplier = company.empty() ? "Distributor" : company;
                o.sku = text(offer.get("sku"), 120);
                o.stock = count(offer.get("inventoryLevel"));
                o.moq = clampInt(std::max(1LL, count(offer.get("moq"))), 1, kMaxQuantity);
                o.multiple = clampInt(std::max(1LL, count(offer.get("orderMultiple"))), 1, kMaxQuantity);
                o.packaging = text(offer.get("packaging"), 120);
                o.leadTimeDays = count(offer.get("factoryLeadDays")) >= 0 ? clampInt(count(offer.get("factoryLeadDays")), 0, 3650) : -1;
                o.url = link(offer.get("clickUrl"));
                // Prices in the requested (converted) currency when Nexar converted them, else the seller's own.
                for (const auto& b : offer.get("prices").items()) {
                    const double converted = b.get("convertedPrice").asNumber(0);
                    const std::string convertedCurrency = currencyCode(b.get("convertedCurrency"));
                    const bool useConverted = converted > 0 && !convertedCurrency.empty();
                    const std::string cur = useConverted ? convertedCurrency : currencyCode(b.get("currency"));
                    if (o.currency.empty()) o.currency = cur;
                    if (cur != o.currency) continue;  // one ladder per offer
                    addBreak(o, count(b.get("quantity")),
                             useConverted ? converted : b.get("price").asNumber(std::numeric_limits<double>::quiet_NaN()));
                }
                part.offers.push_back(std::move(o));
            }
        }
        addPart(r, std::move(part));
    }
    r.total = clampInt(std::max<long long>(total, static_cast<long long>(r.parts.size())), 0, kMaxQuantity);
    if (r.parts.empty()) {
        for (const auto& e : root.get("errors").items()) {
            const std::string message = text(e.get("message"), 400);
            const std::string code = lowerAscii(text(e.get("extensions").get("code")) + " " + message);
            r.error = "Nexar: " + (message.empty() ? std::string("the query failed") : message);
            if (code.find("auth") != std::string::npos || code.find("token") != std::string::npos) r.errorKind = "auth";
            else if (code.find("too many") != std::string::npos || code.find("rate") != std::string::npos)
                r.errorKind = "rate_limit";
            else if (code.find("quota") != std::string::npos || code.find("limit") != std::string::npos)
                r.errorKind = "quota";
            else if (code.find("valid") != std::string::npos || code.find("syntax") != std::string::npos)
                r.errorKind = "bad_request";
            else
                r.errorKind = "service";
            break;
        }
    }
    return r;
}

SupplierSearchResult parseSupplierResponse(const std::string& source, const std::string& body) {
    const std::string s = lowerAscii(source);
    if (s == "nexar" || s == "octopart") return parseNexarResponse(body);
    if (s == "digikey" || s == "digi-key") return parseDigikeyResponse(body);
    if (s == "mouser") return parseMouserResponse(body);
    SupplierSearchResult r;
    r.source = cleanText(source, 40);
    r.error = "Unknown supplier source '" + r.source + "'.";
    r.errorKind = "source";
    return r;
}

// ------------------------------------------------------------------ prices

PriceQuote quoteOffer(const SupplierOffer& offer, int quantity) {
    PriceQuote q;
    q.currency = offer.currency;
    if (offer.prices.empty()) return q;
    const long long multiple = std::max(1, offer.multiple);
    auto roundUp = [&](long long n) {
        n = std::max(1LL, n);
        return std::min<long long>(kMaxQuantity, (n + multiple - 1) / multiple * multiple);
    };
    auto unitAt = [&](long long n) {
        double unit = offer.prices.front().price;
        for (const auto& b : offer.prices)
            if (b.quantity <= n) unit = b.price;
        return unit;
    };
    long long order = roundUp(std::max<long long>({1LL, quantity, offer.moq}));
    if (order < offer.prices.front().quantity) order = roundUp(offer.prices.front().quantity);
    long long bestOrder = order;
    double bestExtended = unitAt(order) * static_cast<double>(order);
    // Buying up to a later break can cost less in total (100 at the 100-break vs 90 at the 10-break).
    for (const auto& b : offer.prices) {
        if (b.quantity <= order) continue;
        const long long n = roundUp(b.quantity);
        const double ext = unitAt(n) * static_cast<double>(n);
        if (ext < bestExtended - 1e-9) {
            bestExtended = ext;
            bestOrder = n;
        }
    }
    q.ok = true;
    q.orderQuantity = static_cast<int>(bestOrder);
    q.unitPrice = unitAt(bestOrder);
    q.extended = bestExtended;
    q.inStock = offer.stock >= 0 && offer.stock >= bestOrder;
    return q;
}

OfferChoice bestOffer(const SupplierPart& part, int quantity, const std::string& currency) {
    OfferChoice best;
    bool anyInCurrency = false;
    if (!currency.empty())
        for (const auto& o : part.offers)
            if (o.currency == currency && !o.prices.empty()) anyInCurrency = true;
    for (size_t i = 0; i < part.offers.size(); ++i) {
        const SupplierOffer& o = part.offers[i];
        if (anyInCurrency && o.currency != currency) continue;
        const PriceQuote q = quoteOffer(o, quantity);
        if (!q.ok) continue;
        bool better = best.offerIndex < 0;
        if (!better) {
            if (q.inStock != best.quote.inStock) better = q.inStock;
            else better = q.extended < best.quote.extended - 1e-12;
        }
        if (better) {
            best.offerIndex = static_cast<int>(i);
            best.quote = q;
        }
    }
    return best;
}

// ------------------------------------------------------------------ merge

namespace {
std::string makerKey(const std::string& m) {
    std::string k = lowerAscii(m);
    std::string out;
    for (char c : k)
        if (std::isalnum(static_cast<unsigned char>(c))) out += c;
    return out;
}

bool sameMaker(const std::string& a, const std::string& b) {
    const std::string x = makerKey(a), y = makerKey(b);
    if (x.empty() || y.empty()) return true;
    const size_t n = std::min<size_t>({x.size(), y.size(), 6});
    return x.compare(0, n, y, 0, n) == 0;
}

void fill(std::string& into, const std::string& from) {
    if (into.empty()) into = from;
}
}  // namespace

std::vector<SupplierPart> mergeSupplierParts(const std::vector<SupplierSearchResult>& results) {
    std::vector<SupplierPart> merged;
    for (const auto& r : results) {
        for (const auto& p : r.parts) {
            const std::string key = normalizeMpn(p.mpn);
            auto it = std::find_if(merged.begin(), merged.end(), [&](const SupplierPart& m) {
                return normalizeMpn(m.mpn) == key && sameMaker(m.manufacturer, p.manufacturer);
            });
            if (it == merged.end()) {
                if (merged.size() >= SupplierLimits::maxParts * 2) continue;
                merged.push_back(p);
                continue;
            }
            SupplierPart& m = *it;
            fill(m.manufacturer, p.manufacturer);
            fill(m.description, p.description);
            fill(m.category, p.category);
            fill(m.package, p.package);
            fill(m.datasheet, p.datasheet);
            fill(m.productUrl, p.productUrl);
            fill(m.imageUrl, p.imageUrl);
            fill(m.rohs, p.rohs);
            // The most severe lifecycle any distributor reports is the one to act on.
            if (severity(p.lifecycle) > severity(m.lifecycle)) {
                m.lifecycle = p.lifecycle;
                m.lifecycleText = p.lifecycleText;
            }
            for (const auto& q : p.parameters) addParameter(m, q.name, q.value);
            for (const auto& o : p.offers) {
                if (m.offers.size() >= SupplierLimits::maxOffers) break;
                const bool dup = std::any_of(m.offers.begin(), m.offers.end(), [&](const SupplierOffer& x) {
                    return lowerAscii(x.supplier) == lowerAscii(o.supplier) && x.sku == o.sku && x.packaging == o.packaging;
                });
                if (!dup) m.offers.push_back(o);
            }
            for (const auto& s : p.sources)
                if (std::find(m.sources.begin(), m.sources.end(), s) == m.sources.end()) m.sources.push_back(s);
        }
    }
    return merged;
}

// ------------------------------------------------------------------ JSON

Json supplierPartToJson(const SupplierPart& p, const std::string& currency) {
    Json j = Json::object();
    j["mpn"] = p.mpn;
    j["manufacturer"] = p.manufacturer;
    j["description"] = p.description;
    j["category"] = p.category;
    j["package"] = p.package;
    j["datasheet"] = p.datasheet;
    j["productUrl"] = p.productUrl;
    j["imageUrl"] = p.imageUrl;
    j["lifecycle"] = partLifecycleName(p.lifecycle);
    j["lifecycleText"] = p.lifecycleText;
    j["rohs"] = p.rohs;
    Json params = Json::array();
    for (const auto& q : p.parameters) {
        Json pj = Json::object();
        pj["name"] = q.name;
        pj["value"] = q.value;
        params.push(pj);
    }
    j["parameters"] = params;
    Json offers = Json::array();
    for (const auto& o : p.offers) {
        Json oj = Json::object();
        oj["supplier"] = o.supplier;
        oj["sku"] = o.sku;
        oj["stock"] = static_cast<double>(o.stock);
        oj["moq"] = o.moq;
        oj["multiple"] = o.multiple;
        oj["packaging"] = o.packaging;
        oj["currency"] = o.currency;
        oj["leadTimeDays"] = o.leadTimeDays;
        oj["url"] = o.url;
        Json prices = Json::array();
        for (const auto& b : o.prices) {
            Json bj = Json::object();
            bj["quantity"] = b.quantity;
            bj["price"] = b.price;
            prices.push(bj);
        }
        oj["prices"] = prices;
        offers.push(oj);
    }
    j["offers"] = offers;
    Json sources = Json::array();
    for (const auto& s : p.sources) sources.push(s);
    j["sources"] = sources;
    j["stock"] = static_cast<double>(p.totalStock());
    Json pricing = Json::array();
    for (int qty : {1, 10, 100, 1000}) {
        const OfferChoice c = bestOffer(p, qty, currency);
        if (c.offerIndex < 0) continue;
        Json pj = Json::object();
        pj["quantity"] = qty;
        pj["unitPrice"] = c.quote.unitPrice;
        pj["orderQuantity"] = c.quote.orderQuantity;
        pj["currency"] = c.quote.currency;
        pj["supplier"] = p.offers[static_cast<size_t>(c.offerIndex)].supplier;
        pj["inStock"] = c.quote.inStock;
        pricing.push(pj);
    }
    j["pricing"] = pricing;
    return j;
}

SupplierPart supplierPartFromJson(const Json& j) {
    SupplierPart p;
    if (!j.isObject()) return p;
    p.mpn = text(j.get("mpn"), 120);
    p.manufacturer = text(j.get("manufacturer"), 200);
    p.description = text(j.get("description"));
    p.category = text(j.get("category"), 200);
    p.package = text(j.get("package"), 300);
    p.datasheet = link(j.get("datasheet"));
    p.productUrl = link(j.get("productUrl"));
    p.imageUrl = link(j.get("imageUrl"));
    p.lifecycleText = text(j.get("lifecycleText"), 120);
    p.lifecycle = partLifecycleFromName(text(j.get("lifecycle"), 20));
    if (p.lifecycle == PartLifecycle::Unknown) p.lifecycle = partLifecycleFromText(p.lifecycleText);
    p.rohs = text(j.get("rohs"), 120);
    for (const auto& q : j.get("parameters").items()) addParameter(p, text(q.get("name"), 120), text(q.get("value"), 300));
    for (const auto& oj : j.get("offers").items()) {
        if (p.offers.size() >= SupplierLimits::maxOffers || !oj.isObject()) break;
        SupplierOffer o;
        o.supplier = text(oj.get("supplier"), 200);
        o.sku = text(oj.get("sku"), 120);
        o.stock = count(oj.get("stock"));
        o.moq = clampInt(std::max(1LL, count(oj.get("moq"))), 1, kMaxQuantity);
        o.multiple = clampInt(std::max(1LL, count(oj.get("multiple"))), 1, kMaxQuantity);
        o.packaging = text(oj.get("packaging"), 120);
        o.currency = currencyCode(oj.get("currency"));
        o.leadTimeDays = count(oj.get("leadTimeDays")) >= 0 ? clampInt(count(oj.get("leadTimeDays")), 0, 3650) : -1;
        o.url = link(oj.get("url"));
        for (const auto& b : oj.get("prices").items())
            addBreak(o, count(b.get("quantity")), b.get("price").asNumber(std::numeric_limits<double>::quiet_NaN()));
        finishOffer(o);
        p.offers.push_back(std::move(o));
    }
    for (const auto& s : j.get("sources").items()) {
        const std::string src = text(s, 20);
        if (!src.empty() && p.sources.size() < 8) p.sources.push_back(src);
    }
    return p;
}

Json supplierSearchToJson(const SupplierSearchResult& r, const std::string& currency) {
    Json j = Json::object();
    j["schema"] = "sieda.supplier/1";
    j["source"] = r.source;
    j["total"] = r.total;
    j["error"] = r.error;
    j["errorKind"] = r.errorKind;
    Json notes = Json::array();
    for (const auto& n : r.notes) notes.push(n);
    j["notes"] = notes;
    Json parts = Json::array();
    for (const auto& p : r.parts) parts.push(supplierPartToJson(p, currency));
    j["parts"] = parts;
    return j;
}

// ------------------------------------------------------------------ BOM roll-up

Json supplierBomRollup(const Json& request) {
    Json out = Json::object();
    std::string currency = currencyCode(request.get("currency"));
    if (currency.empty()) currency = "USD";
    out["currency"] = currency;
    std::vector<SupplierPart> parts;
    for (const auto& pj : request.get("parts").items()) {
        if (parts.size() >= 2000) break;
        SupplierPart p = supplierPartFromJson(pj);
        if (!p.mpn.empty()) parts.push_back(std::move(p));
    }
    std::multimap<std::string, size_t> byMpn;
    for (size_t i = 0; i < parts.size(); ++i) byMpn.emplace(normalizeMpn(parts[i].mpn), i);

    std::set<int> boardSet;
    for (const auto& q : request.get("quantities").items()) {
        const long long n = count(q);
        if (n >= 1 && boardSet.size() < 16) boardSet.insert(clampInt(n, 1, 1000000));
    }
    const long long build = count(request.get("buildQuantity"));
    if (build >= 1) boardSet.insert(clampInt(build, 1, 1000000));
    if (boardSet.empty()) boardSet = {1, 10, 100, 1000};
    const std::vector<int> boards(boardSet.begin(), boardSet.end());
    const int buildBoards = build >= 1 ? clampInt(build, 1, 1000000) : boards.front();

    struct Total {
        double cost = 0;
        int priced = 0, unpriced = 0, shortages = 0;
    };
    std::vector<Total> totals(boards.size());
    bool mixed = false;
    Json lines = Json::array(), warnings = Json::array();
    size_t lineCount = 0;
    for (const auto& lj : request.get("lines").items()) {
        if (++lineCount > 5000) break;
        Json line = Json::object();
        line["item"] = lj.get("item").asInt(0);
        Json refs = Json::array();
        std::string firstRefs;
        for (const auto& r : lj.get("refs").items()) {
            if (refs.size() >= 2000) break;
            const std::string ref = text(r, 40);
            refs.push(ref);
            if (refs.size() <= 3) firstRefs += (firstRefs.empty() ? "" : ", ") + ref;
        }
        if (refs.size() > 3) firstRefs += ", …";
        line["refs"] = refs;
        const std::string mpn = text(lj.get("mpn"), 120), maker = text(lj.get("manufacturer"), 200);
        line["mpn"] = mpn;
        const int perBoard = clampInt(std::max(0LL, count(lj.get("quantity"))), 0, 100000);
        line["quantity"] = perBoard;
        const bool skip = lj.get("dnp").asBool() || lj.get("embedded").asBool() || perBoard == 0;
        const SupplierPart* part = nullptr;
        if (!mpn.empty()) {
            auto range = byMpn.equal_range(normalizeMpn(mpn));
            for (auto it = range.first; it != range.second; ++it) {
                const SupplierPart& cand = parts[it->second];
                if (!part || (sameMaker(cand.manufacturer, maker) && !sameMaker(part->manufacturer, maker))) part = &cand;
            }
        }
        std::string status = skip ? "dnp" : mpn.empty() ? "no_mpn" : !part ? "not_found" : "priced";
        line["lifecycle"] = part ? partLifecycleName(part->lifecycle) : "unknown";
        line["lifecycleText"] = part ? part->lifecycleText : "";
        line["datasheet"] = part ? part->datasheet : "";
        line["stock"] = part ? static_cast<double>(part->totalStock()) : -1.0;
        std::string warning;
        if (part && !skip && lifecycleNeedsAttention(part->lifecycle)) {
            const char* what = part->lifecycle == PartLifecycle::Nrnd ? "is not recommended for new designs"
                               : part->lifecycle == PartLifecycle::Eol ? "is at end of life"
                                                                       : "is obsolete";
            warning = mpn + " " + what + (part->lifecycleText.empty() ? "" : " (" + part->lifecycleText + ")");
        }
        Json offers = Json::array();
        bool anyPrice = false;
        for (size_t k = 0; k < boards.size(); ++k) {
            if (skip || !part) {
                if (!skip) ++totals[k].unpriced;
                continue;
            }
            const long long needed = static_cast<long long>(perBoard) * boards[k];
            const OfferChoice c = bestOffer(*part, clampInt(needed, 1, kMaxQuantity), currency);
            if (c.offerIndex < 0) {
                ++totals[k].unpriced;
                continue;
            }
            anyPrice = true;
            const SupplierOffer& o = part->offers[static_cast<size_t>(c.offerIndex)];
            Json oj = Json::object();
            oj["boards"] = boards[k];
            oj["needed"] = static_cast<double>(needed);
            oj["orderQuantity"] = c.quote.orderQuantity;
            oj["supplier"] = o.supplier;
            oj["sku"] = o.sku;
            oj["unitPrice"] = c.quote.unitPrice;
            oj["extended"] = c.quote.extended;
            oj["currency"] = c.quote.currency;
            oj["inStock"] = c.quote.inStock;
            offers.push(oj);
            if (!c.quote.currency.empty() && c.quote.currency != currency) mixed = true;
            totals[k].cost += c.quote.extended;
            ++totals[k].priced;
            if (!c.quote.inStock) ++totals[k].shortages;
            if (boards[k] == buildBoards && !c.quote.inStock && warning.empty())
                warning = mpn + ": " + (o.stock >= 0 ? "only " + std::to_string(o.stock) + " in stock at " + o.supplier
                                                       : "stock not reported by " + o.supplier) +
                          " for " + std::to_string(needed) + " needed";
        }
        if (status == "priced" && !anyPrice) status = "unpriced";
        line["status"] = status;
        line["warning"] = warning;
        line["offers"] = offers;
        if (!warning.empty() && warnings.size() < 200) warnings.push((firstRefs.empty() ? "" : firstRefs + ": ") + warning);
        lines.push(line);
    }
    Json tj = Json::array();
    for (size_t k = 0; k < boards.size(); ++k) {
        Json t = Json::object();
        t["boards"] = boards[k];
        t["cost"] = totals[k].cost;
        t["perBoard"] = totals[k].cost / boards[k];
        t["priced"] = totals[k].priced;
        t["unpriced"] = totals[k].unpriced;
        t["shortages"] = totals[k].shortages;
        t["complete"] = totals[k].unpriced == 0;
        tj.push(t);
    }
    out["lines"] = lines;
    out["totals"] = tj;
    out["warnings"] = warnings;
    out["mixedCurrency"] = mixed;
    out["buildQuantity"] = buildBoards;
    return out;
}

// ------------------------------------------------------------------ catalog matching

namespace {
/// Orderable suffixes that change only the packing (reel, tube) or the plating mark, never the package.
const std::vector<std::string>& packingSuffixes() {
    static const std::vector<std::string> s = {"TRPBF", "TRMPBF", "PBF", "REEL7", "REEL", "RL7", "RL", "TR", "G4",
                                               "RG4", "E4", "R", "T"};
    return s;
}

/// `n` and `n` less one packing suffix (keeping at least 5 characters).
std::vector<std::string> packingBases(const std::string& n) {
    std::vector<std::string> out = {n};
    for (const auto& suf : packingSuffixes())
        if (n.size() >= suf.size() + 5 && n.compare(n.size() - suf.size(), suf.size(), suf) == 0)
            out.push_back(n.substr(0, n.size() - suf.size()));
    return out;
}

/// "LM358DR" → "LM358", "STM32F405RGT6" → "STM32F405", "AMS1117-3.3" → "AMS1117" (letters, then digits).
std::string family(const std::string& mpn) {
    std::string u;
    for (char c : mpn) u += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    size_t i = 0;
    while (i < u.size() && std::isalpha(static_cast<unsigned char>(u[i]))) ++i;
    const size_t digits = i;
    while (i < u.size() && std::isdigit(static_cast<unsigned char>(u[i]))) ++i;
    if (i == digits) return {};
    // STM32F405: a letter and digits again belong to the family name.
    if (i + 1 < u.size() && std::isalpha(static_cast<unsigned char>(u[i])) && std::isdigit(static_cast<unsigned char>(u[i + 1]))) {
        ++i;
        while (i < u.size() && std::isdigit(static_cast<unsigned char>(u[i]))) ++i;
    }
    return i >= 4 ? u.substr(0, i) : std::string();
}
}  // namespace

Json catalogMatchForMpn(const std::string& rawMpn) {
    Json out = Json::object();
    const std::string mpn = cleanText(rawMpn, 120);
    const std::string key = normalizeMpn(mpn);
    out["match"] = "";
    out["exact"] = false;
    out["note"] = "";
    Json candidates = Json::array();
    if (key.size() < 2) {
        out["candidates"] = candidates;
        return out;
    }
    const auto& parts = standardParts();
    const StandardPart* exact = nullptr;
    const StandardPart* packing = nullptr;
    const auto mpnBases = packingBases(key);
    for (const auto& p : parts) {
        const std::string name = normalizeMpn(p.spec.name);
        if (name == key) {
            exact = &p;
            break;
        }
        if (!packing) {
            for (const auto& a : packingBases(name))
                if (std::find(mpnBases.begin(), mpnBases.end(), a) != mpnBases.end()) packing = &p;
        }
    }
    if (exact) {
        out["match"] = exact->spec.name;
        out["exact"] = true;
    } else if (packing) {
        out["match"] = packing->spec.name;
        out["note"] = "Same part in other packing (" + packing->spec.name + "): check the package against the datasheet.";
    }
    const std::string fam = family(mpn);
    if (!fam.empty()) {
        for (const auto& p : parts) {
            if (candidates.size() >= 8) break;
            if ((exact && &p == exact) || (packing && &p == packing)) continue;
            if (family(p.spec.name) == fam) candidates.push(p.spec.name);
        }
    }
    out["candidates"] = candidates;
    return out;
}

// ------------------------------------------------------------------ C API entry points

Json supplierParseRequest(const std::string& source, const std::string& body, const std::string& currency) {
    try {
        return supplierSearchToJson(parseSupplierResponse(source, body), currencyCode(Json(currency)));
    } catch (const std::exception& e) {
        SupplierSearchResult r;
        r.source = cleanText(source, 40);
        r.error = std::string("The reply cannot be read: ") + cleanText(e.what(), 200);
        r.errorKind = "parse";
        return supplierSearchToJson(r);
    }
}

Json supplierMergeRequest(const Json& request) {
    const std::string currency = currencyCode(request.get("currency"));
    std::vector<SupplierSearchResult> results;
    Json errors = Json::array();
    for (const auto& rj : request.get("results").items()) {
        if (results.size() >= 16) break;
        SupplierSearchResult r;
        r.source = text(rj.get("source"), 40);
        r.error = text(rj.get("error"), 400);
        r.errorKind = text(rj.get("errorKind"), 40);
        if (!r.error.empty()) {
            Json e = Json::object();
            e["source"] = r.source;
            e["error"] = r.error;
            e["errorKind"] = r.errorKind;
            errors.push(e);
        }
        for (const auto& pj : rj.get("parts").items()) {
            if (r.parts.size() >= SupplierLimits::maxParts) break;
            SupplierPart p = supplierPartFromJson(pj);
            if (p.mpn.empty()) continue;
            if (p.sources.empty() && !r.source.empty()) p.sources = {r.source};
            r.parts.push_back(std::move(p));
        }
        results.push_back(std::move(r));
    }
    Json out = Json::object();
    Json parts = Json::array();
    for (const auto& p : mergeSupplierParts(results)) parts.push(supplierPartToJson(p, currency));
    out["parts"] = parts;
    out["errors"] = errors;
    return out;
}

}  // namespace sieda
