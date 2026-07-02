import FileProvider
import Foundation
import UniformTypeIdentifiers

final class FileProviderExtension: NSObject, NSFileProviderReplicatedExtension {
    private let domain: NSFileProviderDomain

    required init(domain: NSFileProviderDomain) {
        self.domain = domain
        super.init()
    }

    func invalidate() {
    }

    func item(for identifier: NSFileProviderItemIdentifier,
              request: NSFileProviderRequest,
              completionHandler: @escaping (NSFileProviderItem?, Error?) -> Void) -> Progress {
        completionHandler(MoonlightFileProviderItem.mockItem(for: identifier), nil)
        return Progress(totalUnitCount: 1)
    }

    func fetchContents(for itemIdentifier: NSFileProviderItemIdentifier,
                       version requestedVersion: NSFileProviderItemVersion?,
                       request: NSFileProviderRequest,
                       completionHandler: @escaping (URL?, NSFileProviderItem?, Error?) -> Void) -> Progress {
        let error = NSError(domain: NSCocoaErrorDomain,
                            code: NSFeatureUnsupportedError,
                            userInfo: [NSLocalizedDescriptionKey: "Moonlight Host Files content fetch is not connected yet."])
        completionHandler(nil, nil, error)
        return Progress(totalUnitCount: 1)
    }

    func enumerator(for containerItemIdentifier: NSFileProviderItemIdentifier,
                    request: NSFileProviderRequest) throws -> NSFileProviderEnumerator {
        return FileProviderEnumerator(containerItemIdentifier: containerItemIdentifier)
    }
}

struct MoonlightFileProviderItem: NSFileProviderItem {
    let itemIdentifier: NSFileProviderItemIdentifier
    let parentItemIdentifier: NSFileProviderItemIdentifier
    let filename: String
    let contentType: UTType
    let capabilities: NSFileProviderItemCapabilities

    static func mockItem(for identifier: NSFileProviderItemIdentifier) -> MoonlightFileProviderItem {
        if identifier == .rootContainer {
            return MoonlightFileProviderItem(itemIdentifier: .rootContainer,
                                             parentItemIdentifier: .rootContainer,
                                             filename: "Moonlight Host Files",
                                             contentType: .folder,
                                             capabilities: [.allowsReading])
        }

        return MoonlightFileProviderItem(itemIdentifier: identifier,
                                         parentItemIdentifier: .rootContainer,
                                         filename: identifier.rawValue,
                                         contentType: .folder,
                                         capabilities: [.allowsReading, .allowsEnumerating])
    }
}
