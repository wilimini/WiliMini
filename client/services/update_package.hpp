#pragma once
// Streaming verification and extraction for update packages (issue #26).
// Platform-independent: file access goes through small interfaces so the
// same code runs on the PSP (sceIo) and in host tests. Uses zlib only.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
namespace pspapp {

class Sha256 {
  public:
    Sha256();
    void update(const void* data, size_t n);
    std::string finishHex(); // lowercase; object must not be reused afterwards
  private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    uint64_t total_ = 0;
    size_t fill_ = 0;
};

// Random-access source (a .part file on the Memory Stick, or memory in tests).
class ByteSource {
  public:
    virtual ~ByteSource() = default;
    virtual uint64_t size() const = 0;
    // Returns bytes read; fewer than n only at EOF or on error.
    virtual size_t read(uint64_t offset, void* dst, size_t n) = 0;
};
// Receives one file's bytes. begin() returning false skips nothing: it aborts.
class FileSink {
  public:
    virtual ~FileSink() = default;
    virtual bool begin(const std::string& path, uint64_t size) = 0;
    virtual bool write(const void* data, size_t n) = 0;
    virtual bool end(bool ok) = 0; // ok=false: discard the partial file
};

struct ZipEntry {
    std::string path;
    uint32_t crc = 0;
    uint64_t compressedSize = 0, size = 0, localOffset = 0;
    uint16_t method = 0; // 0 stored, 8 deflate
    bool isDir = false;
};
struct ZipLimits {
    size_t maxEntries = 4096;
    uint64_t maxFileSize = 256ull << 20;
    uint64_t maxTotalSize = 1024ull << 20;
};
enum class ZipError { Ok, Truncated, Unsupported, BadPath, TooLarge, BadCrc, Sink, Corrupt };

// Computes SHA-256 of the whole source in 16 KiB chunks.
std::string sha256Hex(ByteSource& src);
// Reads and validates the central directory. Rejects zip64, encryption,
// unsafe paths, duplicate paths and sizes over the limits.
ZipError listZip(ByteSource& src, std::vector<ZipEntry>& out, const ZipLimits& lim = {});
// Inflates one entry through a fixed 16 KiB window, checking size and CRC32.
// On any error the sink gets end(false).
ZipError extractEntry(ByteSource& src, const ZipEntry& e, FileSink& sink);

// Download resume helpers (HTTP Range).
// Header value for a request resuming after `have` bytes, e.g. "bytes=100-".
std::string rangeHeader(uint64_t have);
enum class ResumeAction { Append, Restart, Fail };
// Decides what to do with the server's reply to a (possibly ranged) request.
// status: HTTP status; contentRange: header value or empty;
// have: bytes already in .part; expectedTotal: asset size from the release
// JSON (0 = unknown).
ResumeAction resumeAction(int status, std::string_view contentRange, uint64_t have, uint64_t expectedTotal);
}
