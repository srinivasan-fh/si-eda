import AppKit
import Foundation
import PDFKit
import UniformTypeIdentifiers
import Vision

/// A datasheet loaded from disk: extracted text plus (when suitable) the original file for multimodal models.
struct DatasheetDocument {
    var fileName: String
    var pages: [String]
    var attachment: AIAttachment?

    var text: String { pages.joined(separator: "\n") }
    var pageCount: Int { pages.count }

    /// PDFs up to this size/page count are sent natively to providers that read PDFs.
    static let maxNativePDFBytes = 30 * 1024 * 1024
    static let maxNativePDFPages = 100

    enum LoadError: LocalizedError {
        case unreadable(String)
        case unsupported(String)

        var errorDescription: String? {
            switch self {
            case .unreadable(let name): return "Could not read \(name)."
            case .unsupported(let ext): return "Unsupported datasheet format “.\(ext)”. Use PDF, an image or a text file."
            }
        }
    }

    static func load(url: URL) throws -> DatasheetDocument {
        let name = url.lastPathComponent
        let type = UTType(filenameExtension: url.pathExtension.lowercased())
        if type?.conforms(to: .pdf) == true {
            guard let data = try? Data(contentsOf: url), let pdf = PDFDocument(data: data) else {
                throw LoadError.unreadable(name)
            }
            var pages: [String] = []
            for index in 0..<pdf.pageCount {
                let text = pdf.page(at: index)?.string ?? ""
                pages.append("--- Page \(index + 1) ---\n" + text)
            }
            let native = data.count <= maxNativePDFBytes && pdf.pageCount <= maxNativePDFPages
            return DatasheetDocument(fileName: name, pages: pages,
                                     attachment: native ? AIAttachment(kind: .pdf, data: data, fileName: name) : nil)
        }
        if let type, type.conforms(to: .image) {
            guard let data = try? Data(contentsOf: url) else { throw LoadError.unreadable(name) }
            let mime = type.preferredMIMEType ?? "image/png"
            let supported = ["image/png", "image/jpeg", "image/gif", "image/webp"].contains(mime)
            let attachmentData = supported ? data : (pngData(from: data) ?? data)
            let attachmentMime = supported ? mime : "image/png"
            return DatasheetDocument(fileName: name, pages: [recognizeText(in: data)],
                                     attachment: AIAttachment(kind: .image(mimeType: attachmentMime), data: attachmentData, fileName: name))
        }
        if let type, type.conforms(to: .text) || type.conforms(to: .commaSeparatedText) || url.pathExtension.lowercased() == "md" {
            guard let text = try? String(contentsOf: url, encoding: .utf8) else { throw LoadError.unreadable(name) }
            return DatasheetDocument(fileName: name, pages: [text], attachment: nil)
        }
        throw LoadError.unsupported(url.pathExtension)
    }

    /// Vision OCR (accurate mode) for pinout screenshots and scanned tables.
    static func recognizeText(in data: Data) -> String {
        guard let image = NSImage(data: data), let cg = image.cgImage(forProposedRect: nil, context: nil, hints: nil) else {
            return ""
        }
        let request = VNRecognizeTextRequest()
        request.recognitionLevel = .accurate
        request.usesLanguageCorrection = false
        let handler = VNImageRequestHandler(cgImage: cg, options: [:])
        try? handler.perform([request])
        let lines = (request.results ?? []).compactMap { $0.topCandidates(1).first?.string }
        return lines.joined(separator: "\n")
    }

    private static func pngData(from data: Data) -> Data? {
        guard let image = NSImage(data: data), let tiff = image.tiffRepresentation,
              let rep = NSBitmapImageRep(data: tiff) else { return nil }
        return rep.representation(using: .png, properties: [:])
    }

    /// Text for providers that cannot read the file: pin-related pages first, within `budget` characters.
    func textExcerpt(budget: Int) -> (text: String, notice: String?) {
        let full = text
        if full.count <= budget { return (full, nil) }
        let keywords = ["pin configuration", "pin description", "pin functions", "pinout", "pin no", "pin name", "terminal functions"]
        let ranked = pages.enumerated().sorted { a, b in
            let sa = keywords.filter { a.element.lowercased().contains($0) }.count
            let sb = keywords.filter { b.element.lowercased().contains($0) }.count
            return sa != sb ? sa > sb : a.offset < b.offset
        }
        var chosen: [(Int, String)] = []
        var used = 0
        for (index, page) in ranked where used + page.count <= budget {
            chosen.append((index, page))
            used += page.count
        }
        chosen.sort { $0.0 < $1.0 }
        let notice = "The datasheet text is long (\(pageCount) pages); sent \(chosen.count) pages, pin-description pages first."
        return (chosen.map { $0.1 }.joined(separator: "\n"), notice)
    }
}

/// The Datasheet Analyst agent: datasheet → editable custom part definition.
enum DatasheetAnalyst {
    static let system = """
    You are the Datasheet Analyst agent inside SiEDA, a professional EDA suite. Extract the component \
    definition needed to create a schematic symbol and PCB footprint from the datasheet.
    - Use the package variant the user asks for; otherwise the most common one in the ordering table.
    - List EVERY pin of that package with its number exactly as printed (1, 2 … or EP for an exposed pad), the \
    short pin name (VCC, GND, TRIG, PB0 …) and an electrical type: power_in for supply/ground pins, power_out for \
    regulator outputs, input, output, bidirectional for I/O, open_collector for open-drain/collector outputs, \
    passive for analog/passive terminals, no_connect for NC pins.
    - Choose the package type from the allowed list (SOIC, TSSOP, DIP, QFN, LQFP, SOT23, HEADER, TO220) that \
    matches the physical package; pin_count is the number of package terminals (excluding an exposed pad).
    - name is the base part number (e.g. NE555, ATtiny85, LM7805); ref_prefix is U for ICs, Q for transistors, \
    J for connectors. Put anything uncertain in notes.
    """

    struct Result {
        var spec: CustomPartSpec
        var notes: [String]
        var notice: String?
    }

    static func request(for document: DatasheetDocument, hint: String, provider: AIProvider) -> (AIRequest, String?) {
        var attachments: [AIAttachment] = []
        var notice: String?
        var body = ""
        if let attachment = document.attachment,
           (attachment.kind == .pdf && provider.acceptsPDF) || (attachment.kind != .pdf && provider.acceptsImages) {
            attachments = [attachment]
            if attachment.kind != .pdf, !document.text.isEmpty {
                body = "<ocr_text>\n\(document.text)\n</ocr_text>\n\n"
            }
        } else {
            let excerpt = document.textExcerpt(budget: 180_000)
            notice = excerpt.notice
            if document.attachment?.kind == .pdf, !provider.acceptsPDF {
                notice = [notice, "\(provider.displayName) does not read PDFs directly; extracted text was sent instead."]
                    .compactMap { $0 }.joined(separator: " ")
            } else if document.attachment == nil, document.fileName.lowercased().hasSuffix(".pdf") {
                notice = [notice, "The PDF is too large to upload; extracted text was sent instead."]
                    .compactMap { $0 }.joined(separator: " ")
            }
            body = "<datasheet_text>\n\(excerpt.text)\n</datasheet_text>\n\n"
        }
        let prompt = """
        <file_name>\(document.fileName)</file_name>
        <package_hint>\(hint.isEmpty ? "none" : hint)</package_hint>

        \(body)Extract the component definition for SiEDA's custom component library.
        """
        return (AIRequest(system: system, prompt: prompt, schemaName: "component_definition",
                          schema: DatasheetSchema.componentDefinition, maxTokens: 16_000, attachments: attachments),
                notice)
    }

    static func extract(_ document: DatasheetDocument, hint: String, provider: AIProvider) async throws -> Result {
        let (aiRequest, notice) = request(for: document, hint: hint, provider: provider)
        let text = try await provider.complete(aiRequest)
        let extraction = try JSONExtraction.decode(DatasheetSchema.Extraction.self, from: text)
        var spec = extraction.spec(datasheet: document.fileName)
        if let forced = PackageKind.guess(hint) { spec.package.type = forced.rawValue }
        if spec.pins.isEmpty { throw AIProviderError.invalidResponse("No pins were found in the datasheet.") }
        return Result(spec: spec, notes: extraction.notes ?? [], notice: notice)
    }
}

/// Deterministic pin-table parser used by the offline provider (and as a fallback without an API key).
enum PinTableParser {
    struct Row {
        var number: Int
        var name: String
        var description: String
    }

    private static let stopWords: Set<String> = ["FIGURE", "TABLE", "PAGE", "SECTION", "NOTE", "NOTES", "V", "MA", "MHZ", "KHZ",
                                                 "TYP", "MIN", "MAX", "UNIT", "UNITS", "AND", "THE", "TO", "OF", "FOR"]

    static func rows(in text: String) -> [Row] {
        let numberFirst = try? NSRegularExpression(pattern: #"^\s*(?:pin\s*)?(\d{1,3})[\s.:|]+([A-Za-z][A-Za-z0-9_/+\-#~]{0,19})\b\s*[|:\-–]?\s*(.*)$"#,
                                                   options: [.caseInsensitive])
        let nameFirst = try? NSRegularExpression(pattern: #"^\s*([A-Za-z][A-Za-z0-9_/+\-#~]{0,15})[\s|]+(\d{1,3})\b\s*[|:\-–]?\s*(.*)$"#)
        var found: [Int: Row] = [:]
        for raw in text.components(separatedBy: .newlines) {
            let line = raw.trimmingCharacters(in: .whitespaces)
            guard !line.isEmpty, line.count < 240 else { continue }
            let range = NSRange(line.startIndex..., in: line)
            func group(_ m: NSTextCheckingResult, _ i: Int) -> String {
                guard let r = Range(m.range(at: i), in: line) else { return "" }
                return String(line[r])
            }
            var number: Int?
            var name = ""
            var description = ""
            if let m = numberFirst?.firstMatch(in: line, range: range) {
                number = Int(group(m, 1))
                name = group(m, 2)
                description = group(m, 3)
            } else if let m = nameFirst?.firstMatch(in: line, range: range) {
                name = group(m, 1)
                number = Int(group(m, 2))
                description = group(m, 3)
            }
            guard let n = number, n >= 1, n <= 256, !stopWords.contains(name.uppercased()), found[n] == nil else { continue }
            found[n] = Row(number: n, name: name, description: description.trimmingCharacters(in: .whitespaces))
        }
        // Keep the longest run of consecutive pin numbers starting at 1.
        var run = 0
        while found[run + 1] != nil { run += 1 }
        guard run >= 2 else { return [] }
        return (1...run).compactMap { found[$0] }
    }

    static func partName(in text: String, fallback: String) -> String {
        let head = String(text.prefix(4000))
        guard let regex = try? NSRegularExpression(pattern: #"\b([A-Z]{1,5}[0-9]{2,5}[A-Z0-9\-]{0,6})\b"#) else { return fallback }
        var counts: [String: Int] = [:]
        for m in regex.matches(in: head, range: NSRange(head.startIndex..., in: head)) {
            if let r = Range(m.range(at: 1), in: head) { counts[String(head[r]), default: 0] += 1 }
        }
        return counts.max { $0.value < $1.value }?.key ?? fallback
    }

    /// JSON matching `DatasheetSchema.componentDefinition`.
    static func extractionJSON(text: String, fileName: String, hint: String) -> String {
        let rows = Self.rows(in: text)
        let stem = (fileName as NSString).deletingPathExtension
        let name = partName(in: text, fallback: stem.isEmpty ? "PART" : stem)
        let package = PackageKind.guess(hint) ?? PackageKind.guess(text.prefix(20_000).description) ?? (rows.count <= 3 ? .sot23 : .soic)
        let pins: [[String: Any]] = rows.map {
            ["number": String($0.number), "name": $0.name.uppercased(),
             "type": PinElectricalType.infer(name: $0.name, description: $0.description).rawValue,
             "description": $0.description]
        }
        let object: [String: Any] = [
            "name": name,
            "manufacturer": "",
            "description": "",
            "ref_prefix": "U",
            "default_value": name,
            "package": ["type": package.rawValue, "pin_count": rows.count] as [String: Any],
            "pins": pins,
            "notes": rows.isEmpty
                ? ["No pin table was recognised. Add the pins manually or use an AI model."]
                : ["Parsed offline from the pin table; verify pin types against the datasheet."],
        ]
        guard let data = try? JSONSerialization.data(withJSONObject: object) else { return "{}" }
        return String(decoding: data, as: UTF8.self)
    }
}
