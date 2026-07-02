import FileProvider
import Foundation
import UniformTypeIdentifiers

final class FileProviderEnumerator: NSObject, NSFileProviderEnumerator {
    private let containerItemIdentifier: NSFileProviderItemIdentifier

    init(containerItemIdentifier: NSFileProviderItemIdentifier) {
        self.containerItemIdentifier = containerItemIdentifier
        super.init()
    }

    func invalidate() {
    }

    func enumerateItems(for observer: NSFileProviderEnumerationObserver,
                        startingAt page: NSFileProviderPage) {
        if containerItemIdentifier == .rootContainer {
            let item = MoonlightFileProviderItem(itemIdentifier: NSFileProviderItemIdentifier("mock-shared-folder"),
                                                 parentItemIdentifier: .rootContainer,
                                                 filename: "Shared Folder",
                                                 typeIdentifier: UTType.folder.identifier,
                                                 capabilities: [.allowsReading, .allowsEnumerating])
            observer.didEnumerate([item])
        }
        observer.finishEnumerating(upTo: nil)
    }

    func enumerateChanges(for observer: NSFileProviderChangeObserver,
                          from anchor: NSFileProviderSyncAnchor) {
        observer.finishEnumeratingChanges(upTo: anchor, moreComing: false)
    }

    func currentSyncAnchor(completionHandler: @escaping (NSFileProviderSyncAnchor?) -> Void) {
        completionHandler(NSFileProviderSyncAnchor("moonlight-mock-anchor".data(using: .utf8)!))
    }
}
