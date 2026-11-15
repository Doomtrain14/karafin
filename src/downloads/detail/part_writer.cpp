// ftruncate is hidden by newlib in strict ISO mode (CMAKE_CXX_EXTENSIONS OFF).
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include "downloads/detail/part_writer.hpp"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace karafin::downloads::detail {

PartWriter::PartWriter(size_t bufferSize) : buffer_(new char[bufferSize]), capacity_(bufferSize) {}

PartWriter::~PartWriter() { close(true); }

bool PartWriter::open(const std::string& path, uint64_t base) {
    close(true);
    errno_ = 0;
    used_ = 0;
    int flags = O_WRONLY | O_CREAT;
    if (base == 0) flags |= O_TRUNC;
    fd_ = ::open(path.c_str(), flags, 0666);
    if (fd_ < 0) {
        errno_ = errno;
        return false;
    }
    if (base > 0) {
        struct stat st{};
        if (::fstat(fd_, &st) == 0 && static_cast<uint64_t>(st.st_size) > base &&
            ::ftruncate(fd_, static_cast<off_t>(base)) != 0) {
            errno_ = errno;
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        if (::lseek(fd_, static_cast<off_t>(base), SEEK_SET) < 0) {
            errno_ = errno;
            ::close(fd_);
            fd_ = -1;
            return false;
        }
    }
    base_ = written_ = base;
    return true;
}

bool PartWriter::writeOut() {
    size_t off = 0;
    while (off < used_) {
        const ssize_t n = ::write(fd_, buffer_.get() + off, used_ - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            errno_ = errno;
            return false;
        }
        if (n == 0) {
            errno_ = ENOSPC;
            return false;
        }
        off += static_cast<size_t>(n);
        written_ += static_cast<uint64_t>(n);
    }
    used_ = 0;
    return true;
}

bool PartWriter::write(std::string_view data) {
    if (fd_ < 0) return false;
    while (!data.empty()) {
        const size_t room = capacity_ - used_;
        const size_t n = data.size() < room ? data.size() : room;
        std::memcpy(buffer_.get() + used_, data.data(), n);
        used_ += n;
        data.remove_prefix(n);
        if (used_ == capacity_ && !writeOut()) return false;
    }
    return true;
}

bool PartWriter::flush(bool fsync) {
    if (fd_ < 0) return false;
    if (used_ > 0 && !writeOut()) return false;
    if (fsync && ::fsync(fd_) != 0) {
        errno_ = errno;
        return false;
    }
    return true;
}

bool PartWriter::truncateToBase() {
    used_ = 0;
    if (fd_ < 0) return false;
    if (written_ != base_) {
        if (::ftruncate(fd_, static_cast<off_t>(base_)) != 0 || ::lseek(fd_, static_cast<off_t>(base_), SEEK_SET) < 0) {
            errno_ = errno;
            return false;
        }
        written_ = base_;
    }
    return true;
}

bool PartWriter::rebaseToZero() {
    if (fd_ < 0 || written_ != base_) return false;
    if (base_ == 0) return true;
    if (::ftruncate(fd_, 0) != 0 || ::lseek(fd_, 0, SEEK_SET) < 0) {
        errno_ = errno;
        return false;
    }
    base_ = written_ = 0;
    return true;
}

bool PartWriter::close(bool keepBuffered) {
    if (fd_ < 0) return true;
    bool ok = true;
    if (!keepBuffered) used_ = 0;
    if (used_ > 0) ok = writeOut();
    if (::fsync(fd_) != 0 && ok) {
        errno_ = errno;
        ok = false;
    }
    if (::close(fd_) != 0 && ok) {
        errno_ = errno;
        ok = false;
    }
    fd_ = -1;
    used_ = 0;
    return ok;
}

}  // namespace karafin::downloads::detail
