// Copyright (C) 2026 Marian Vid. Subject to GNU General Public License version 3.
#include "File.h"
#include "Cache.h"
#include "Client.h"
#include <algorithm>
#include <cstring>

namespace nc::vfs::s3 {

static constexpr uint64_t g_S3ReadAheadSize = 8ull * 1024 * 1024;
static constexpr uint64_t g_S3MinPartSize = 16ull * 1024 * 1024; // also the single-PUT threshold
static constexpr uint64_t g_S3MaxParts = 9'500;                    // S3 allows 10'000, keep a margin

static Error S3FileError(int _code)
{
    return Error{Error::POSIX, _code};
}

File::File(std::string_view _relative_path, const std::shared_ptr<S3Host> &_host)
    : VFSFile(_relative_path, _host), m_Host(*_host)
{
    if( auto location = m_Host.Map(_relative_path) )
        m_Location = std::move(*location);
}

File::~File()
{
    if( IsOpened() )
        std::ignore = Close();
}

uint64_t File::PartSizeFor(uint64_t _total_size) noexcept
{
    constexpr uint64_t mib = 1024 * 1024;
    const uint64_t needed = (_total_size + g_S3MaxParts - 1) / g_S3MaxParts;
    const uint64_t rounded = (needed + mib - 1) / mib * mib;
    return std::max(g_S3MinPartSize, rounded);
}

std::expected<void, Error> File::Open(unsigned long _open_flags, const VFSCancelChecker &_cancel_checker)
{
    if( IsOpened() )
        return std::unexpected(S3FileError(EINVAL));
    if( m_Location.key.empty() )
        return std::unexpected(S3FileError(EINVAL));
    if( _open_flags & VFSFlags::OF_Append )
        return std::unexpected(S3FileError(ENOTSUP));
    const bool read = _open_flags & VFSFlags::OF_Read;
    const bool write = _open_flags & VFSFlags::OF_Write;
    if( read == write )
        return std::unexpected(S3FileError(EINVAL)); // exactly one of read / write is supported

    const auto st = m_Host.Stat(Path(), 0, _cancel_checker);
    if( read ) {
        if( !st )
            return std::unexpected(st.error());
        if( !st->mode_bits.reg )
            return std::unexpected(S3FileError(EISDIR));
        m_Size = static_cast<int64_t>(st->size);
    }
    else {
        if( st && (_open_flags & VFSFlags::OF_NoExist) )
            return std::unexpected(S3FileError(EEXIST));
        if( !st && !(_open_flags & VFSFlags::OF_Create) )
            return std::unexpected(S3FileError(ENOENT));
        if( st && st->mode_bits.dir )
            return std::unexpected(S3FileError(EISDIR));
        m_Size = -1; // to be announced via SetUploadSize()
    }

    m_OpenFlags = _open_flags;
    m_Pos = 0;
    m_Window.clear();
    m_WindowOffset = 0;
    m_Buffer.clear();
    m_UploadId.clear();
    m_Parts.clear();
    m_Uploaded = false;
    return {};
}

bool File::IsOpened() const
{
    return m_OpenFlags != 0;
}

std::expected<void, Error> File::Close()
{
    if( !IsOpened() )
        return std::unexpected(S3FileError(EINVAL));

    std::expected<void, Error> result;
    if( (m_OpenFlags & VFSFlags::OF_Write) && !m_Uploaded ) {
        if( m_Size == 0 ) {
            result = FinishUpload(); // a zero-byte file without any Write() calls
        }
        else {
            AbortPendingUpload(); // an interrupted upload leaves nothing behind
        }
    }

    m_OpenFlags = 0;
    m_Size = -1;
    m_Pos = 0;
    m_Window = {};
    m_Buffer = {};
    m_Parts.clear();
    m_UploadId.clear();
    return result;
}

File::ReadParadigm File::GetReadParadigm() const
{
    return ReadParadigm::Random;
}

File::WriteParadigm File::GetWriteParadigm() const
{
    return WriteParadigm::Upload;
}

std::expected<size_t, Error> File::ReadAt(off_t _pos, void *_buf, size_t _size)
{
    if( !IsOpened() || !(m_OpenFlags & VFSFlags::OF_Read) || _pos < 0 )
        return std::unexpected(S3FileError(EINVAL));
    const auto pos = static_cast<uint64_t>(_pos);
    const auto size = static_cast<uint64_t>(m_Size);
    if( _size == 0 || pos >= size )
        return 0;

    const bool in_window = pos >= m_WindowOffset && pos < m_WindowOffset + m_Window.size();
    if( !in_window ) {
        const uint64_t length = std::min(std::max<uint64_t>(g_S3ReadAheadSize, _size), size - pos);
        auto data = m_Host.Client().GetObjectRange(m_Location.bucket, m_Location.key, pos, length);
        if( !data )
            return std::unexpected(data.error());
        if( data->empty() )
            return std::unexpected(S3FileError(EIO)); // the object shrank under our feet
        m_Window = std::move(*data);
        m_WindowOffset = pos;
    }

    const uint64_t offset_in_window = pos - m_WindowOffset;
    const size_t n = static_cast<size_t>(std::min<uint64_t>(_size, m_Window.size() - offset_in_window));
    std::memcpy(_buf, m_Window.data() + offset_in_window, n);
    return n;
}

std::expected<size_t, Error> File::Read(void *_buf, size_t _size)
{
    const auto n = ReadAt(static_cast<off_t>(m_Pos), _buf, _size);
    if( n )
        m_Pos += *n;
    return n;
}

std::expected<uint64_t, Error> File::Seek(off_t _off, int _basis)
{
    if( !IsOpened() || !(m_OpenFlags & VFSFlags::OF_Read) )
        return std::unexpected(S3FileError(EINVAL));
    int64_t target = 0;
    if( _basis == Seek_Set )
        target = _off;
    else if( _basis == Seek_Cur )
        target = static_cast<int64_t>(m_Pos) + _off;
    else if( _basis == Seek_End )
        target = m_Size + _off;
    else
        return std::unexpected(S3FileError(EINVAL));
    if( target < 0 )
        return std::unexpected(S3FileError(EINVAL));
    m_Pos = static_cast<uint64_t>(std::min<int64_t>(target, m_Size));
    return m_Pos;
}

std::expected<uint64_t, Error> File::Pos() const
{
    return m_Pos;
}

std::expected<uint64_t, Error> File::Size() const
{
    if( m_Size < 0 )
        return std::unexpected(S3FileError(EINVAL));
    return static_cast<uint64_t>(m_Size);
}

bool File::Eof() const
{
    if( !IsOpened() || m_Size < 0 )
        return true;
    return m_Pos >= static_cast<uint64_t>(m_Size);
}

std::expected<void, Error> File::SetUploadSize(size_t _size)
{
    if( !IsOpened() || !(m_OpenFlags & VFSFlags::OF_Write) || m_Size >= 0 )
        return std::unexpected(S3FileError(EINVAL));
    m_Size = static_cast<int64_t>(_size);
    m_PartSize = PartSizeFor(_size);
    if( _size == 0 )
        return FinishUpload();
    return {};
}

std::expected<size_t, Error> File::Write(const void *_buf, size_t _size)
{
    if( !IsOpened() || !(m_OpenFlags & VFSFlags::OF_Write) || m_Size < 0 || m_Uploaded )
        return std::unexpected(S3FileError(EINVAL));
    if( m_Pos + _size > static_cast<uint64_t>(m_Size) )
        return std::unexpected(S3FileError(EFBIG)); // more data than announced
    if( _size == 0 )
        return 0;

    const auto *bytes = static_cast<const std::byte *>(_buf);
    m_Buffer.insert(m_Buffer.end(), bytes, bytes + _size);
    m_Pos += _size;

    // Large uploads go out part by part, so memory usage stays bounded by one part.
    if( static_cast<uint64_t>(m_Size) > g_S3MinPartSize ) {
        while( m_Buffer.size() >= m_PartSize && m_Pos < static_cast<uint64_t>(m_Size) ) {
            if( const auto rc = UploadBufferedPart(m_PartSize); !rc ) {
                AbortPendingUpload();
                return std::unexpected(rc.error());
            }
        }
    }

    if( m_Pos == static_cast<uint64_t>(m_Size) ) {
        if( const auto rc = FinishUpload(); !rc ) {
            AbortPendingUpload();
            return std::unexpected(rc.error());
        }
    }
    return _size;
}

std::expected<void, Error> File::UploadBufferedPart(size_t _bytes)
{
    if( m_UploadId.empty() ) {
        auto id = m_Host.Client().CreateMultipartUpload(m_Location.bucket, m_Location.key);
        if( !id )
            return std::unexpected(id.error());
        m_UploadId = std::move(*id);
    }
    const int number = static_cast<int>(m_Parts.size()) + 1;
    auto etag = m_Host.Client().UploadPart(
        m_Location.bucket, m_Location.key, m_UploadId, number, std::span{m_Buffer.data(), _bytes});
    if( !etag )
        return std::unexpected(etag.error());
    m_Parts.push_back(CompletedPart{number, std::move(*etag)});
    m_Buffer.erase(m_Buffer.begin(), m_Buffer.begin() + static_cast<std::ptrdiff_t>(_bytes));
    return {};
}

std::expected<void, Error> File::FinishUpload()
{
    if( m_UploadId.empty() ) {
        // Everything fits into a single request.
        if( const auto rc = m_Host.Client().PutObject(m_Location.bucket, m_Location.key, m_Buffer); !rc )
            return rc;
    }
    else {
        if( !m_Buffer.empty() )
            if( const auto rc = UploadBufferedPart(m_Buffer.size()); !rc )
                return rc;
        if( const auto rc =
                m_Host.Client().CompleteMultipartUpload(m_Location.bucket, m_Location.key, m_UploadId, m_Parts);
            !rc )
            return rc;
        m_UploadId.clear();
    }
    m_Buffer = {};
    m_Uploaded = true;
    CommitUploadedToCache();
    return {};
}

void File::AbortPendingUpload() noexcept
{
    if( m_UploadId.empty() )
        return;
    std::ignore = m_Host.Client().AbortMultipartUpload(m_Location.bucket, m_Location.key, m_UploadId);
    m_UploadId.clear();
    m_Parts.clear();
}

void File::CommitUploadedToCache()
{
    if( const auto split = SplitParent(Path()) )
        m_Host.Cache().CommitUpsert(Path(), Entry{split->second, false, m_Size, std::time(nullptr)});
}

std::shared_ptr<VFSFile> File::Clone() const
{
    return std::make_shared<File>(Path(), std::static_pointer_cast<S3Host>(Host()));
}

} // namespace nc::vfs::s3
