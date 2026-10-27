// Copyright (C) 2026 Marian Vid. Subject to GNU General Public License version 3.
#pragma once

#include "S3Host.h"
#include "PathMapper.h"
#include "Xml.h"
#include <VFS/VFSFile.h>
#include <vector>

namespace nc::vfs::s3 {

// An S3 object opened either for reading (random access via ranged GETs with read-ahead)
// or for writing (an upload of a size announced in advance: a single PUT or a multipart upload).
class File final : public VFSFile
{
public:
    File(std::string_view _relative_path, const std::shared_ptr<S3Host> &_host);
    ~File() override;

    std::expected<void, Error> Open(unsigned long _open_flags, const VFSCancelChecker &_cancel_checker) override;
    bool IsOpened() const override;
    std::expected<void, Error> Close() override;

    ReadParadigm GetReadParadigm() const override;
    WriteParadigm GetWriteParadigm() const override;

    std::expected<size_t, Error> Read(void *_buf, size_t _size) override;
    std::expected<size_t, Error> ReadAt(off_t _pos, void *_buf, size_t _size) override;
    std::expected<uint64_t, Error> Seek(off_t _off, int _basis) override;
    std::expected<uint64_t, Error> Pos() const override;
    std::expected<uint64_t, Error> Size() const override;
    bool Eof() const override;

    std::expected<void, Error> SetUploadSize(size_t _size) override;
    std::expected<size_t, Error> Write(const void *_buf, size_t _size) override;

    std::shared_ptr<VFSFile> Clone() const override;

    // Exposed for unit tests: the part size used for an upload of the given total size.
    static uint64_t PartSizeFor(uint64_t _total_size) noexcept;

private:
    std::expected<void, Error> FinishUpload();
    std::expected<void, Error> UploadBufferedPart(size_t _bytes);
    void AbortPendingUpload() noexcept;
    void CommitUploadedToCache();

    S3Host &m_Host;
    Location m_Location;
    unsigned long m_OpenFlags = 0;
    int64_t m_Size = -1;
    uint64_t m_Pos = 0;

    // reading
    uint64_t m_WindowOffset = 0;
    std::string m_Window;

    // writing
    std::vector<std::byte> m_Buffer;
    std::string m_UploadId;
    std::vector<CompletedPart> m_Parts;
    uint64_t m_PartSize = 0;
    bool m_Uploaded = false;
};

} // namespace nc::vfs::s3
