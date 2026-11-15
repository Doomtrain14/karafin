#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

// Buffered append writer for <file>.part. Data is collected in a large buffer and written with
// few large write() calls (much faster on the SD card than small writes); sync() flushes the
// buffer and fsyncs. Not thread-safe (one per job).
namespace karafin::downloads::detail {

class PartWriter {
public:
    static constexpr size_t kDefaultBufferSize = 2u * 1024 * 1024;

    explicit PartWriter(size_t bufferSize = kDefaultBufferSize);
    ~PartWriter();
    PartWriter(const PartWriter&) = delete;
    PartWriter& operator=(const PartWriter&) = delete;

    // Opens (creating) `path` and positions at `base`: 0 truncates the file; otherwise the file
    // is cut to `base` bytes if it is longer.
    bool open(const std::string& path, uint64_t base);
    bool isOpen() const { return fd_ >= 0; }

    // Buffers `data`; writes the buffer to disk when full. False on a write error (see errorNumber()).
    bool write(std::string_view data);
    // Writes the buffer; with `fsync` also commits it to the card.
    bool flush(bool fsync);
    // Drops buffered (not yet written) bytes.
    void discardBuffered() { used_ = 0; }
    // Cuts the file back to the open() offset and drops the buffer (response body was invalid).
    bool truncateToBase();
    // The server ignored the Range request: treat the bytes received so far as the start of the
    // file. Only possible while nothing was written to disk since open() (returns false otherwise).
    bool rebaseToZero();
    // Flushes (unless `keepBuffered` is false), fsyncs and closes.
    bool close(bool keepBuffered = true);

    uint64_t base() const { return base_; }
    uint64_t size() const { return written_ + used_; }  // logical size incl. buffered bytes
    uint64_t written() const { return written_; }       // bytes on disk
    bool wroteSinceOpen() const { return written_ > base_; }
    int errorNumber() const { return errno_; }

private:
    bool writeOut();

    std::unique_ptr<char[]> buffer_;
    size_t capacity_ = 0;
    size_t used_ = 0;
    int fd_ = -1;
    uint64_t base_ = 0;
    uint64_t written_ = 0;
    int errno_ = 0;
};

}  // namespace karafin::downloads::detail
