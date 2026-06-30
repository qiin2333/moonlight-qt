#pragma once

#include <memory>

#include <QString>

#include "file_mapping_messages.h"

namespace FileMapping {

class ProtocolClient {
public:
    virtual ~ProtocolClient();

    virtual Capability fetchCapability(int timeoutMs) = 0;
    virtual Error connectSession(const Capability& capability, int timeoutMs) = 0;
    virtual ListResult list(const QString& mappingId, const QString& path, int timeoutMs) = 0;
    virtual StatResult stat(const QString& mappingId, const QString& path, int timeoutMs) = 0;
    virtual ReadResult read(const QString& mappingId,
                            const QString& path,
                            quint64 offset,
                            quint32 length,
                            int timeoutMs) = 0;
};

using ProtocolClientPtr = std::shared_ptr<ProtocolClient>;

} // namespace FileMapping
