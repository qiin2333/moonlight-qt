#pragma once

#include "backend/nvcomputer.h"
#include "protocol/file_mapping_client.h"

#include <memory>

class FileMappingClient;

class FileMappingProtocolAdapter : public FileMapping::ProtocolClient
{
public:
    explicit FileMappingProtocolAdapter(NvComputer computer);
    ~FileMappingProtocolAdapter() override;

    FileMapping::Capability fetchCapability(int timeoutMs) override;
    FileMapping::Error connectSession(const FileMapping::Capability& capability, int timeoutMs) override;
    QList<FileMapping::RemoteMapping> mappings() const override;
    FileMapping::ListResult list(const QString& mappingId, const QString& path, int timeoutMs) override;
    FileMapping::StatResult stat(const QString& mappingId, const QString& path, int timeoutMs) override;
    FileMapping::ReadResult read(const QString& mappingId,
                                 const QString& path,
                                 quint64 offset,
                                 quint32 length,
                                 int timeoutMs) override;

private:
    FileMappingClient& client();

    NvComputer m_Computer;
    std::unique_ptr<FileMappingClient> m_Client;
    QList<FileMapping::RemoteMapping> m_Mappings;
};
