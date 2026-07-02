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

    func createItem(basedOn itemTemplate: NSFileProviderItem,
                    fields: NSFileProviderItemFields,
                    contents url: URL?,
                    options: NSFileProviderCreateItemOptions = [],
                    request: NSFileProviderRequest,
                    completionHandler: @escaping (NSFileProviderItem?, NSFileProviderItemFields, Bool, Error?) -> Void) -> Progress {
        completionHandler(nil, fields, false, readOnlyError())
        return Progress(totalUnitCount: 1)
    }

    func modifyItem(_ item: NSFileProviderItem,
                    baseVersion version: NSFileProviderItemVersion,
                    changedFields: NSFileProviderItemFields,
                    contents newContents: URL?,
                    options: NSFileProviderModifyItemOptions = [],
                    request: NSFileProviderRequest,
                    completionHandler: @escaping (NSFileProviderItem?, NSFileProviderItemFields, Bool, Error?) -> Void) -> Progress {
        completionHandler(nil, changedFields, false, readOnlyError())
        return Progress(totalUnitCount: 1)
    }

    func deleteItem(identifier: NSFileProviderItemIdentifier,
                    baseVersion version: NSFileProviderItemVersion,
                    options: NSFileProviderDeleteItemOptions = [],
                    request: NSFileProviderRequest,
                    completionHandler: @escaping (Error?) -> Void) -> Progress {
        completionHandler(readOnlyError())
        return Progress(totalUnitCount: 1)
    }

    private func readOnlyError() -> Error {
        NSError(domain: NSCocoaErrorDomain,
                code: NSFeatureUnsupportedError,
                userInfo: [NSLocalizedDescriptionKey: "Moonlight Host Files are read-only in this session."])
    }
}

struct MoonlightFileProviderItem: NSFileProviderItem {
    let itemIdentifier: NSFileProviderItemIdentifier
    let parentItemIdentifier: NSFileProviderItemIdentifier
    let filename: String
    let typeIdentifier: String
    let capabilities: NSFileProviderItemCapabilities

    var contentType: UTType {
        UTType(typeIdentifier) ?? .data
    }

    static func mockItem(for identifier: NSFileProviderItemIdentifier) -> MoonlightFileProviderItem {
        if identifier == .rootContainer {
            return MoonlightFileProviderItem(itemIdentifier: .rootContainer,
                                             parentItemIdentifier: .rootContainer,
                                             filename: "Moonlight Host Files",
                                             typeIdentifier: UTType.folder.identifier,
                                             capabilities: [.allowsReading])
        }

        return MoonlightFileProviderItem(itemIdentifier: identifier,
                                         parentItemIdentifier: .rootContainer,
                                         filename: identifier.rawValue,
                                         typeIdentifier: UTType.folder.identifier,
                                         capabilities: [.allowsReading, .allowsEnumerating])
    }
}
