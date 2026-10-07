import AppKit
import UniformTypeIdentifiers

/// Exchange with mechanical CAD and design review: IDF placement import, compare with another version.
extension DesignStore {
    private func chooseText(_ message: String, extensions: [String]) -> (URL, String)? {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = extensions.compactMap { UTType(filenameExtension: $0) } + [.data]
        panel.allowsMultipleSelection = false
        panel.message = message
        guard panel.runModal() == .OK, let url = panel.url else { return nil }
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        do {
            return (url, try String(contentsOf: url, encoding: .utf8))
        } catch {
            present(error, title: "Could not read \(url.lastPathComponent)")
            return nil
        }
    }

    /// Asks for an IDF board file (.emn) from MCAD and moves the parts it places. One undo step.
    func importIDFPlacement() {
        guard let picked = chooseText("Choose the IDF board file (.emn) from mechanical CAD.", extensions: ["emn"]) else { return }
        let (url, text) = picked
        var moved: [String] = []
        performExternalEdit("MCAD placement from \(url.lastPathComponent)") { engine in
            moved = engine.importIDFPlacement(text)
            return !moved.isEmpty
        }
        statusMessage = moved.isEmpty ? "\(url.lastPathComponent): no part moved"
                                      : "Moved \(moved.count) parts: \(moved.prefix(8).joined(separator: ", "))"
    }

    /// Asks for another version of the project and shows what changed from it to the open design.
    func compareWithFile() {
        guard let picked = chooseText("Choose the earlier version to compare the open design with.",
                                      extensions: ["siedaproj"]) else { return }
        let (url, text) = picked
        designDiff = DesignDiff(title: url.lastPathComponent, text: engine.diffText(from: text))
    }
}

struct DesignDiff: Identifiable {
    var title: String
    var text: String
    var id: String { title + text }
}
