import AppKit
import Foundation
import SwiftUI
import UniformTypeIdentifiers

/// The built-in model back-end: SiEDA's own engine (Core/src/LocalModel.cpp) runs a GGUF model file on this Mac, so
/// the AI works offline with no other app installed (docs/AI.md, "Built-in model").
struct BuiltInProvider: AIProvider {
    var model: String  // a file name in LocalModelStore.folder

    var displayName: String { "Built-in" }
    var modelName: String { model }

    func complete(_ request: AIRequest) async throws -> String {
        let path = LocalModelStore.folder.appendingPathComponent(model).path
        guard FileManager.default.fileExists(atPath: path) else {
            throw AIProviderError.invalidResponse("The built-in model ‘\(model)’ is not installed. Download one in Settings → AI.")
        }
        let schema = (try? JSONSerialization.data(withJSONObject: request.schema, options: [.sortedKeys]))
            .map { String(decoding: $0, as: UTF8.self) } ?? "{}"
        let user = request.prompt + "\n\nAnswer with one JSON object that follows this JSON schema:\n" + schema
        return try await LocalLLM.run(path: path, system: request.system, user: user, maxTokens: min(request.maxTokens, 4096))
    }
}

/// One loaded model (kept between requests) and a serial queue that runs it off the main thread.
final class LocalLLM {
    private static let queue = DispatchQueue(label: "SiEDA.LocalLLM", qos: .userInitiated)
    private static var loaded: (path: String, model: LocalLLM)?

    private let handle: OpaquePointer

    private init(path: String) throws {
        var error: UnsafeMutablePointer<CChar>?
        guard let handle = sieda_llm_open(path, &error) else {
            throw AIProviderError.invalidResponse(EDAEngine.take(error) ?? "The model could not be opened.")
        }
        self.handle = handle
    }

    deinit { sieda_llm_close(handle) }

    /// Answers on the model's queue; cancelling the calling task stops the generation.
    static func run(path: String, system: String, user: String, maxTokens: Int) async throws -> String {
        let cancelled = CancelFlag()
        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { continuation in
                queue.async {
                    do {
                        if loaded?.path != path {
                            loaded = nil  // free the old model before mapping the new one
                            loaded = (path, try LocalLLM(path: path))
                        }
                        continuation.resume(returning: try loaded!.model.chat(system: system, user: user, maxTokens: maxTokens, cancelled: cancelled))
                    } catch {
                        continuation.resume(throwing: error)
                    }
                }
            }
        } onCancel: {
            cancelled.set()
        }
    }

    private func chat(system: String, user: String, maxTokens: Int, cancelled: CancelFlag) throws -> String {
        var error: UnsafeMutablePointer<CChar>?
        let options = "{\"maxTokens\":\(maxTokens),\"json\":true}"
        let context = Unmanaged.passUnretained(cancelled).toOpaque()
        let reply = sieda_llm_chat(handle, system, user, options, { _, context in
            Unmanaged<CancelFlag>.fromOpaque(context!).takeUnretainedValue().isSet ? 0 : 1
        }, context, &error)
        if cancelled.isSet { throw CancellationError() }
        guard let text = EDAEngine.take(reply) else {
            throw AIProviderError.invalidResponse(EDAEngine.take(error) ?? "The built-in model failed.")
        }
        return text
    }

    final class CancelFlag: @unchecked Sendable {
        private let lock = NSLock()
        private var value = false
        var isSet: Bool { lock.withLock { value } }
        func set() { lock.withLock { value = true } }
    }
}

/// Model files on this Mac, like `ollama pull / list / rm`: downloads (with progress), imports and deletes GGUF files
/// in Application Support/SiEDA/Models.
@MainActor
final class LocalModelStore: ObservableObject {
    struct Download: Identifiable {
        let file: String, title: String, size: String, url: String
        var id: String { file }
    }

    /// Open-licence (Apache 2.0) instruction models; any other GGUF file can be imported or downloaded by URL.
    static let catalog: [Download] = [
        Download(file: "qwen2.5-1.5b-instruct-q4_k_m.gguf", title: "Qwen 2.5 1.5B Instruct", size: "1.1 GB",
                 url: "https://huggingface.co/Qwen/Qwen2.5-1.5B-Instruct-GGUF/resolve/main/qwen2.5-1.5b-instruct-q4_k_m.gguf"),
        Download(file: "Qwen2.5-7B-Instruct-Q4_K_M.gguf", title: "Qwen 2.5 7B Instruct", size: "4.7 GB",
                 url: "https://huggingface.co/bartowski/Qwen2.5-7B-Instruct-GGUF/resolve/main/Qwen2.5-7B-Instruct-Q4_K_M.gguf"),
        Download(file: "qwen2.5-0.5b-instruct-q8_0.gguf", title: "Qwen 2.5 0.5B Instruct", size: "0.7 GB",
                 url: "https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/qwen2.5-0.5b-instruct-q8_0.gguf"),
    ]

    static let shared = LocalModelStore()

    nonisolated static var folder: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return base.appendingPathComponent("SiEDA/Models", isDirectory: true)
    }

    /// Installed model file names.
    nonisolated static func installed() -> [String] {
        let files = (try? FileManager.default.contentsOfDirectory(atPath: folder.path)) ?? []
        return files.filter { $0.lowercased().hasSuffix(".gguf") }.sorted()
    }

    @Published private(set) var files: [String] = LocalModelStore.installed()
    @Published private(set) var progress: [String: Double] = [:]
    @Published var lastError: String?
    private var tasks: [String: URLSessionDownloadTask] = [:]

    func refresh() { files = Self.installed() }

    func download(_ url: String, as file: String? = nil) {
        guard let source = URL(string: url), source.scheme == "https" else {
            lastError = "Enter an https:// link to a .gguf file."
            return
        }
        let name = file ?? source.lastPathComponent
        guard name.lowercased().hasSuffix(".gguf"), tasks[name] == nil else { return }
        let delegate = DownloadDelegate { [weak self] fraction in
            Task { @MainActor in self?.progress[name] = fraction }
        } finished: { [weak self] location, error in
            var failure = error
            if let location {
                do {
                    try FileManager.default.createDirectory(at: Self.folder, withIntermediateDirectories: true)
                    let target = Self.folder.appendingPathComponent(name)
                    try? FileManager.default.removeItem(at: target)
                    try FileManager.default.moveItem(at: location, to: target)
                } catch {
                    failure = error.localizedDescription
                }
            }
            Task { @MainActor in
                guard let self else { return }
                self.tasks[name] = nil
                self.progress[name] = nil
                if let failure { self.lastError = failure }
                self.refresh()
            }
        }
        let session = URLSession(configuration: .default, delegate: delegate, delegateQueue: nil)
        let task = session.downloadTask(with: source)
        tasks[name] = task
        progress[name] = 0
        lastError = nil
        task.resume()
        session.finishTasksAndInvalidate()
    }

    func cancel(_ file: String) { tasks[file]?.cancel() }

    func delete(_ file: String) {
        try? FileManager.default.removeItem(at: Self.folder.appendingPathComponent(file))
        refresh()
    }

    func importFile() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [UTType(filenameExtension: "gguf") ?? .data]
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            try FileManager.default.createDirectory(at: Self.folder, withIntermediateDirectories: true)
            try FileManager.default.copyItem(at: url, to: Self.folder.appendingPathComponent(url.lastPathComponent))
        } catch {
            lastError = error.localizedDescription
        }
        refresh()
    }
}

private final class DownloadDelegate: NSObject, URLSessionDownloadDelegate {
    let progress: (Double) -> Void
    let finished: (URL?, String?) -> Void

    init(progress: @escaping (Double) -> Void, finished: @escaping (URL?, String?) -> Void) {
        self.progress = progress
        self.finished = finished
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData _: Int64,
                    totalBytesWritten written: Int64, totalBytesExpectedToWrite total: Int64) {
        if total > 0 { progress(Double(written) / Double(total)) }
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
        let status = (downloadTask.response as? HTTPURLResponse)?.statusCode ?? 200
        guard (200..<300).contains(status) else {
            finished(nil, "The download failed (HTTP \(status)).")
            return
        }
        // The file at `location` is removed when this returns: move it to a temporary place first.
        let keep = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString + ".gguf")
        do {
            try FileManager.default.moveItem(at: location, to: keep)
            finished(keep, nil)
        } catch {
            finished(nil, error.localizedDescription)
        }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        if let error, (error as NSError).code != NSURLErrorCancelled { finished(nil, error.localizedDescription) }
        else if error != nil { finished(nil, nil) }
    }
}

/// Settings → AI → Built-in: the model files on this Mac and the ones to download.
struct BuiltInModelsView: View {
    @EnvironmentObject private var settings: AISettings
    @ObservedObject private var store = LocalModelStore.shared
    @State private var link = ""

    var body: some View {
        Text("Built-in models run on this Mac with SiEDA’s own engine: no other app, and no network once downloaded. Smaller models answer faster; larger ones design better.")
            .font(.caption)
            .foregroundStyle(Theme.textMuted)
        ForEach(store.files, id: \.self) { file in
            HStack {
                Image(systemName: settings.model(for: .builtIn) == file ? "checkmark.circle.fill" : "circle")
                    .foregroundStyle(Theme.skyBlue)
                Text(verbatim: file).lineLimit(1)
                Spacer()
                if settings.model(for: .builtIn) != file {
                    Button("Use") { settings.models[.builtIn] = file }
                }
                Button("Delete", role: .destructive) { store.delete(file) }
            }
        }
        ForEach(LocalModelStore.catalog.filter { !store.files.contains($0.file) }) { item in
            HStack {
                Text(verbatim: "\(item.title) · \(item.size)")
                Spacer()
                if let fraction = store.progress[item.file] {
                    ProgressView(value: fraction).frame(width: 120)
                    Button("Cancel") { store.cancel(item.file) }
                } else {
                    Button("Download") { store.download(item.url, as: item.file) }
                }
            }
        }
        HStack {
            TextField("Model URL (.gguf)", text: $link)
            Button("Download") { store.download(link) }
                .disabled(link.isEmpty)
            Button("Import GGUF File…") { store.importFile() }
        }
        ForEach(store.progress.keys.filter { key in !LocalModelStore.catalog.contains { $0.file == key } }.sorted(), id: \.self) { file in
            HStack {
                Text(verbatim: file).lineLimit(1)
                ProgressView(value: store.progress[file] ?? 0)
                Button("Cancel") { store.cancel(file) }
            }
        }
        if let error = store.lastError {
            Label(error, systemImage: "exclamationmark.triangle.fill").foregroundStyle(Theme.error)
        }
    }
}
