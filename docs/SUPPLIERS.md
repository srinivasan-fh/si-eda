# Suppliers: live part search and BOM pricing

SiEDA searches the distributors directly with your own API keys. For every manufacturer part it shows stock, price
breaks, lifecycle (active, NRND, EOL, obsolete) and the datasheet, and places the part with its sourcing. The BOM
shows live prices, stock and lifecycle warnings, and the BOM cost for 1, 10, 100 and 1000 boards and your build
quantity.

- [Distributors and keys](#distributors-and-keys)
- [Finding and placing a part](#finding-and-placing-a-part)
- [Live pricing in the BOM](#live-pricing-in-the-bom)
- [How prices are worked out](#how-prices-are-worked-out)
- [Lifecycle](#lifecycle)
- [Offline cache, limits and errors](#offline-cache-limits-and-errors)
- [Privacy](#privacy)
- [C API and data format](#c-api-and-data-format)
- [Code map](#code-map)
- [Tests and fixture provenance](#tests-and-fixture-provenance)
- [Limits](#limits)

## Distributors and keys

| Distributor | API | Key you enter | What it gives |
|---|---|---|---|
| Octopart (Nexar) | Nexar Supply GraphQL API, `supSearchMpn` | Client ID and client secret of a Nexar application with the **Supply** scope (OAuth client credentials) | Offers of many distributors (DigiKey, Mouser, Farnell, Arrow, LCSC…) in one search, specs, lifecycle |
| DigiKey | Product Information API v4, `POST /products/v4/search/keyword` | Client ID and client secret of a DigiKey **Production** API application (client credentials) | DigiKey offers per packaging (cut tape, reel, Digi-Reel), status, parameters |
| Mouser | Search API v2, `POST /api/v2/search/keyword` | Search API key | Mouser offer, price breaks, lifecycle, lead time |

Enter them in **Settings → Suppliers**. Keys are kept in the macOS Keychain (like the AI provider keys), never in
the project file or the preferences. A distributor without a key is simply not searched; with no key at all the
search and pricing sheets say so and link to Settings. SiEDA ships no key.

Settings also choose the **currency** (USD by default; DigiKey and Nexar convert, Mouser answers in your account's
currency) and how long results are **cached** (24 hours by default).

## Finding and placing a part

1. In the **Component Library**, click **Find Parts Online…**.
2. Search by part number (`LM358DR`) or keywords (`CAN transceiver SOIC-8`). All configured distributors are asked
   at once; the same part from several distributors is shown once with every offer.
3. Pick a part: its lifecycle badge, offers (supplier, SKU, stock, MOQ, order multiple, packaging, lead time, price
   breaks), parameters and links to the datasheet and product page.
4. **Place Part** or **Add to Library**:
   - If the project library or the standard catalog has the part (same part number, ignoring case and separators,
     or the same part in other packing such as `MAX485ESA+` for the catalog's `MAX485ESA+T`), it is linked to that
     symbol and footprint. **Place Part** places it with its manufacturer, part number, the supplier's part number
     and the unit price at your build quantity, as one undo step.
   - Otherwise the pin-table editor opens prefilled with the part number, manufacturer, description, datasheet link,
     a package guessed from the distributor's package text and the pin count. **Read Pins from Datasheet** downloads
     the distributor's datasheet PDF and extracts the pins (AI, or the offline parser). **Save & Place** then places
     it with the same sourcing.

## Live pricing in the BOM

**BOM → Live Pricing…** looks up every line's manufacturer part number (four at a time, from the cache when fresh)
and shows per line the lifecycle, stock, the best offer at the build quantity, its unit price and the line total
(with the order quantity), plus warnings: NRND / EOL / obsolete parts and lines that are short of stock. The cost
strip gives the cost per board and the total at 1, 10, 100, 1000 boards and the build quantity; it turns orange when
lines are unpriced or short.

**Apply Prices to BOM** writes the supplier part number and unit price of each line's best offer at the build
quantity into the BOM, as one undo step; they then flow into the BOM and assembly exports like hand-entered prices.

## How prices are worked out

For an offer and a quantity:

- the order quantity is raised to the offer's MOQ and rounded up to its order multiple (full reels at DigiKey
  `Tape & Reel`, the reel quantity at Mouser);
- below the first price break, the first break's quantity is ordered;
- the unit price is the largest break at or below the order quantity;
- buying up to a later break is chosen when it costs less in total (100 at the 100-break can be cheaper than 90 at
  the 10-break).

The best offer for a line is in stock first (stock reported and covering the order), then the lowest total, among
offers in your currency (offers in other currencies are used only when no offer is in yours; the roll-up then says
the total mixes currencies).

**Spares** (Live Pricing → Spares, request `attritionPercent` / `attritionMin`): each line orders the larger of that
percentage and that many extra parts for assembly loss (2 % or 5 pieces is typical for cut-tape passives); 0 by default.

## Lifecycle

| Shown | From the distributor's words |
|---|---|
| ACTIVE | Active, Production |
| NEW | New Product, Preliminary, Sampling |
| NRND | Not Recommended for New Designs, NRND, Not For New Designs |
| EOL | End of Life, Last Time Buy, EOL, LTB |
| OBSOLETE | Obsolete, Discontinued |

When distributors disagree, the most severe status is shown. NRND, EOL and obsolete parts are flagged in the search,
in the BOM pricing sheet and in its warnings.

## Offline cache, limits and errors

- Results are cached per distributor, currency and query in `~/Library/Caches/<app>/SiEDA/Suppliers`. A fresh
  result (within the cache time) is used without a network call. When a distributor cannot be reached, is busy
  (HTTP 5xx) or rate-limits, an older cached result is shown and marked **offline** with its date. Errors are never
  cached; **Clear Cached Results** empties the cache (500 files at most are kept).
- Requests time out after 20 s (60 s in all). HTTP 429 with a `Retry-After` of up to 5 s is retried once; longer
  waits are reported ("rate limit reached: try again in 120 s"). Closing the sheet or starting a new search cancels
  the requests in flight. OAuth tokens are reused until shortly before they expire.
- A refused key ("Invalid unique identifier", HTTP 401) says to check it in Settings → Suppliers.
- Replies are untrusted input. The core refuses replies over 16 MB or nested deeper than 64 levels, reads at most
  200 parts, 64 offers per part, 32 price breaks per offer and 60 parameters, cuts text to 2000 bytes of valid UTF-8,
  keeps only `http(s)` links, and range-checks every number (prices 0…10⁷, quantities 1…10⁹, stock ≤ 10¹²).

## Privacy

Searches go from your Mac straight to the distributor you configured; SiEDA has no server in between. The query is
the part number or keywords you type (or the BOM's part numbers for live pricing). The Mouser API takes the key as a
URL parameter, as Mouser requires; it is sent over HTTPS only.

## C API and data format

```c
/* source: "nexar", "digikey", "mouser"; body: the reply as received; currency: preferred ("USD", NULL = any) */
char* sieda_supplier_parse(const char* source, const char* body, const char* currency);
char* sieda_supplier_merge(const char* request_json);       /* {"results":[…],"currency"} → {"parts","errors"} */
char* sieda_supplier_bom_rollup(const char* request_json);  /* see below */
char* sieda_supplier_catalog_match(const char* mpn);        /* {"match","exact","note","candidates"} */
```

The normalised schema (`sieda.supplier/1`):

```json
{"schema":"sieda.supplier/1","source":"mouser","total":3,"error":"","errorKind":"","notes":[],
 "parts":[{"mpn":"LM358DR","manufacturer":"Texas Instruments","description":"…","category":"…","package":"8-SOIC",
   "datasheet":"https://…","productUrl":"https://…","imageUrl":"","lifecycle":"active","lifecycleText":"Active",
   "rohs":"…","parameters":[{"name":"Package / Case","value":"8-SOIC"}],
   "offers":[{"supplier":"Mouser","sku":"595-LM358DR","stock":23764,"moq":1,"multiple":1,"packaging":"Reel",
     "currency":"USD","leadTimeDays":42,"url":"https://…","prices":[{"quantity":1,"price":0.45}]}],
   "sources":["mouser"],"stock":23764,
   "pricing":[{"quantity":1,"unitPrice":0.45,"orderQuantity":1,"currency":"USD","supplier":"Mouser","inStock":true}]}]}
```

`errorKind` is `auth`, `rate_limit`, `quota`, `bad_request`, `service`, `parse` or `source`. `stock` and
`leadTimeDays` are −1 when not reported.

Roll-up request and reply:

```json
{"currency":"USD","quantities":[1,10,100,1000],"buildQuantity":5,"attritionPercent":2,"attritionMin":5,
 "lines":[{"item":1,"refs":["U1","U2"],"quantity":2,"mpn":"LM358DR","manufacturer":"","dnp":false,"embedded":false}],
 "parts":[normalised parts]}
→ {"currency":"USD","buildQuantity":5,"mixedCurrency":false,"warnings":["U3: LM358DRG4 is not recommended …"],
   "lines":[{"item","refs","mpn","quantity","status":"priced|unpriced|no_mpn|not_found|dnp","lifecycle",
     "lifecycleText","warning","datasheet","stock",
     "offers":[{"boards","needed","orderQuantity","supplier","sku","unitPrice","extended","currency","inStock"}]}],
   "totals":[{"boards","cost","perBoard","priced","unpriced","shortages","complete"}]}
```

## Code map

| Layer | File | What |
|---|---|---|
| Core | `Core/include/sieda/Suppliers.hpp`, `Core/src/Suppliers.cpp` | Schema, Nexar / DigiKey / Mouser readers, price breaks, best offer, merge, roll-up, catalog match |
| C ABI | `Core/include/sieda/sieda_c.h` | `sieda_supplier_parse`, `_merge`, `_bom_rollup`, `_catalog_match` |
| Bridge | `SiEDA/Bridge/EDAEngine+Suppliers.swift` | `EDAEngine.parseSupplierReply`, `mergeSupplierResults`, `supplierBomRollup`, `catalogMatch` |
| Models | `SiEDA/Suppliers/SupplierModels.swift` | `SupplierPartInfo`, offers, roll-up types |
| Network | `SiEDA/Suppliers/SupplierClients.swift` | `NexarClient`, `DigiKeyClient`, `MouserClient`, `SupplierHTTP` (timeouts, 429), `SupplierTokenCache`, `SupplierCache`, `SupplierSearchEngine`, `SupplierSettings` (Keychain) |
| Placement | `SiEDA/Suppliers/SupplierPlacement.swift` | Catalog / library link, sourcing, prefilled draft; `DesignStore.placeLibraryPart`, `applySupplierPricing` |
| Views | `SiEDA/Views/Library/SupplierSearchView.swift`, `SiEDA/Views/BOM/BomLivePricingView.swift`, `SiEDA/Views/Settings/SupplierSettingsView.swift` | Find Parts Online, Live Pricing, Settings → Suppliers |

## Tests and fixture provenance

- Core (`Core/tests/core_tests.cpp`): `supplier_lifecycle_prices_and_quotes`, `supplier_parsers_read_recorded_replies`,
  `supplier_merge_rollup_and_catalog_match`, `supplier_parsers_survive_hostile_replies` (deep nesting, oversize,
  HTML error pages, wrong types, NaN / huge numbers, 1500 deterministic mutations of the fixtures), `supplier_c_api`
  (`Core/tests/c_api_test.c`). No test touches the network.
- App (`SiEDATests/SiEDATests.swift`, `SupplierSearchTests`): every request goes to a stub `URLProtocol` — the
  Mouser call and its API-key parameter, the fresh cache, the offline fallback, a refused key, 429 retry and long
  back-off, the Nexar token then GraphQL call with its bearer token, cancellation, Keychain-only credentials (an
  in-memory store in the test), placement with sourcing in one undo step, and the roll-up.
- **Fixture provenance.** The replies in `Core/tests/fixtures/suppliers/` were written by hand to the distributors'
  published response formats (Nexar Supply GraphQL schema, DigiKey Product Information v4 `KeywordResponse`, Mouser
  Search API v2 `SearchResponseRoot`, and their error envelopes). They were not recorded from a live account: this
  build environment has no distributor keys. Part numbers are real; stock figures and prices are illustrative.
  The readers accept the documented field names and the common older ones (DigiKey v3 `ManufacturerPartNumber`,
  `QuantityAvailable`, `StandardPricing` on the product).

## Limits

- Searches are keyword / part-number searches; parametric filtering at the distributor (by value, package…) is not
  exposed. The library's own parametric search (`cat:` `pkg:` `pins:` `mfr:`) covers the catalog.
- Only the three APIs above. LCSC, Farnell, Arrow and others appear through Octopart (Nexar) only.
- The readers follow the published formats; a distributor changing its reply format needs a reader update (the
  tests then show which fields moved).
- Linking to the catalog is by part number (exact, or packing suffix). A part of the same family in another package
  is offered as a "similar catalog part" note, never linked automatically.
- Prices are list prices from the distributors' public APIs; contract pricing (DigiKey `MyPricing`) is not used.
