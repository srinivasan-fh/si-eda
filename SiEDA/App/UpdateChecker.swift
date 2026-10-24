import AppKit
import Foundation

/// Help → Check for Updates… and the opt-in daily check: reads the latest GitHub release of SiEDA and, when it is
/// newer than this build, offers to open its page (the signed, notarised disk image is there). Nothing is downloaded
/// or installed without the user, and nothing is sent but the request itself.
enum UpdateChecker {
    static let autoKey = "updates.automatic"
    static let lastKey = "updates.lastCheck"
    static let latestURL = URL(string: "https://api.github.com/repos/srinivasan-fh/si-eda/releases/latest")!

    struct Release: Decodable {
        var tagName: String
        var htmlURL: URL
        enum CodingKeys: String, CodingKey { case tagName = "tag_name", htmlURL = "html_url" }
    }

    static var currentVersion: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "0"
    }

    /// Numeric dotted compare ("v1.10" > "1.9"); a trailing pre-release word ("1.2.0-beta") counts as older.
    static func isNewer(_ tag: String, than current: String) -> Bool {
        func parts(_ s: String) -> ([Int], Bool) {
            let trimmed = s.hasPrefix("v") ? String(s.dropFirst()) : s
            let main = trimmed.split(separator: "-", maxSplits: 1)
            return (main.first.map { $0.split(separator: ".").map { Int($0) ?? 0 } } ?? [], main.count > 1)
        }
        let (a, aPre) = parts(tag), (b, bPre) = parts(current)
        for i in 0..<max(a.count, b.count) {
            let x = i < a.count ? a[i] : 0, y = i < b.count ? b[i] : 0
            if x != y { return x > y }
        }
        return bPre && !aPre
    }

    /// Daily when the user turned it on in Settings; `interactive` (the menu) always checks and always answers.
    @MainActor
    static func check(interactive: Bool) {
        let defaults = UserDefaults.standard
        if !interactive {
            guard defaults.bool(forKey: autoKey) else { return }
            if let last = defaults.object(forKey: lastKey) as? Date, Date().timeIntervalSince(last) < 86_400 { return }
        }
        defaults.set(Date(), forKey: lastKey)
        Task {
            var request = URLRequest(url: latestURL, timeoutInterval: 15)
            request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
            let release = try? await JSONDecoder().decode(Release.self, from: URLSession.shared.data(for: request).0)
            await MainActor.run { answer(release, interactive: interactive) }
        }
    }

    @MainActor
    private static func answer(_ release: Release?, interactive: Bool) {
        let alert = NSAlert()
        if let release, isNewer(release.tagName, than: currentVersion) {
            alert.messageText = String(localized: "A new version of SiEDA is available")
            alert.informativeText = "\(release.tagName) (\(String(localized: "this is")) \(currentVersion))"
            alert.addButton(withTitle: String(localized: "Open Download Page"))
            alert.addButton(withTitle: String(localized: "Later"))
            if alert.runModal() == .alertFirstButtonReturn { NSWorkspace.shared.open(release.htmlURL) }
        } else if interactive {
            alert.messageText = release == nil ? String(localized: "Couldn't check for updates")
                                               : String(localized: "SiEDA is up to date")
            alert.informativeText = currentVersion
            alert.runModal()
        }
    }
}
