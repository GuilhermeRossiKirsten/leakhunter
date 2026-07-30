#include "TraceWriter.hpp"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

namespace leakhunter::agent {
namespace {

/// write(2) can return short or be interrupted; neither is an error.
/// @return false when the descriptor is genuinely broken.
[[nodiscard]] bool writeFully(int fileDescriptor, const unsigned char* data, std::size_t bytes,
                              int* failureErrno) noexcept {
    std::size_t written = 0;
    while (written < bytes) {
        const ssize_t chunk = ::write(fileDescriptor, data + written, bytes - written);
        if (chunk < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (failureErrno != nullptr) {
                *failureErrno = errno;
            }
            return false;
        }
        if (chunk == 0) {
            if (failureErrno != nullptr) {
                *failureErrno = 0;  // wrote nothing and did not fail; treat as broken
            }
            return false;
        }
        written += static_cast<std::size_t>(chunk);
    }
    return true;
}

/// Moves @p descriptor to a number above kSafeFdBase, out of the way of the
/// daemonisation idiom.
///
/// `for (int fd = 3; fd < 256; ++fd) close(fd);` is a standard way to shed
/// inherited descriptors, and it closes ours: writes then fail with EBADF and
/// the trace ends wherever the program happened to be. Sitting above the loop's
/// bound makes the common form of that harmless.
///
/// This is mitigation, not a guarantee. A program that uses `close_range(3, ~0U)`
/// or `closefrom()` still takes the descriptor away, which is why the write path
/// also reports EBADF explicitly instead of leaving a mysterious empty trace.
///
/// @return the relocated descriptor, or @p descriptor unchanged if relocation
///         was not possible (a low RLIMIT_NOFILE, most likely).
[[nodiscard]] int relocateHigh(int descriptor) noexcept {
    constexpr int kSafeFdBase = 512;
    if (descriptor < 0 || descriptor >= kSafeFdBase) {
        return descriptor;
    }

#if defined(F_DUPFD_CLOEXEC)
    const int moved = ::fcntl(descriptor, F_DUPFD_CLOEXEC, kSafeFdBase);
#else
    const int moved = ::fcntl(descriptor, F_DUPFD, kSafeFdBase);
    if (moved >= 0) {
        ::fcntl(moved, F_SETFD, FD_CLOEXEC);
    }
#endif
    if (moved < 0) {
        return descriptor;
    }

    ::close(descriptor);
    return moved;
}

}  // namespace

bool TraceWriter::open(const char* path) noexcept {
    if (path == nullptr || *path == '\0') {
        return false;
    }

    ::pthread_mutex_init(&mutex_, nullptr);

    // O_CLOEXEC matters: the target may fork+exec, and the child must not
    // inherit a descriptor into a trace it is not allowed to write.
    const int opened = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    fileDescriptor_ = relocateHigh(opened);
    used_ = 0;
    droppedRecords_ = 0;
    lastWriteErrno_ = 0;
    return fileDescriptor_ >= 0;
}

void TraceWriter::appendLocked(const void* data, std::size_t bytes) noexcept {
    if (bytes == 0 || data == nullptr) {
        return;
    }

    const auto* source = static_cast<const unsigned char*>(data);

    if (bytes > kBufferSize) {
        // Larger than the buffer: flush what we have and write it straight
        // through. Only reachable if kMaxFrames ever grows past the buffer.
        flushLocked();
        if (fileDescriptor_ >= 0 &&
            !writeFully(fileDescriptor_, source, bytes, &lastWriteErrno_)) {
            ::close(fileDescriptor_);
            fileDescriptor_ = -1;
            ++droppedRecords_;
        }
        return;
    }

    if (used_ + bytes > kBufferSize) {
        flushLocked();
    }

    std::memcpy(buffer_ + used_, source, bytes);
    used_ += bytes;
}

void TraceWriter::flushLocked() noexcept {
    if (used_ == 0) {
        return;
    }
    if (fileDescriptor_ < 0) {
        ++droppedRecords_;
        used_ = 0;
        return;
    }

    if (!writeFully(fileDescriptor_, buffer_, used_, &lastWriteErrno_)) {
        // Disk full, or the descriptor was closed under us. Stop writing and
        // remember both that the trace is incomplete and why -- EBADF here means
        // the target closed our descriptor, which is worth saying out loud
        // rather than leaving as an empty file to be guessed at.
        ::close(fileDescriptor_);
        fileDescriptor_ = -1;
        ++droppedRecords_;
    }
    used_ = 0;
}

void TraceWriter::appendRecord(const Chunk* chunks, std::size_t count) noexcept {
    if (chunks == nullptr || count == 0) {
        return;
    }

    ::pthread_mutex_lock(&mutex_);
    for (std::size_t i = 0; i < count; ++i) {
        appendLocked(chunks[i].data, chunks[i].bytes);
    }
    ::pthread_mutex_unlock(&mutex_);
}

void TraceWriter::flush() noexcept {
    ::pthread_mutex_lock(&mutex_);
    flushLocked();
    ::pthread_mutex_unlock(&mutex_);
}

void TraceWriter::close() noexcept {
    ::pthread_mutex_lock(&mutex_);
    flushLocked();
    if (fileDescriptor_ >= 0) {
        ::close(fileDescriptor_);
        fileDescriptor_ = -1;
    }
    ::pthread_mutex_unlock(&mutex_);
}

void TraceWriter::flushFromSignal() noexcept {
    if (signalFlushDone_.test_and_set(std::memory_order_acq_rel)) {
        return;  // another signal already did this
    }
    if (fileDescriptor_ < 0 || used_ == 0) {
        return;
    }

    // Read `used_` once: another thread may still be appending, and a value
    // that is merely stale gives us a shorter, still-valid prefix.
    const std::size_t pending = used_;
    (void)writeFully(fileDescriptor_, buffer_, pending > kBufferSize ? kBufferSize : pending,
                     nullptr);
    ::fsync(fileDescriptor_);
}

void TraceWriter::lock() noexcept { ::pthread_mutex_lock(&mutex_); }
void TraceWriter::unlock() noexcept { ::pthread_mutex_unlock(&mutex_); }

void TraceWriter::abandonInChild() noexcept {
    // Discard, do not flush: these bytes are the parent's, and writing them
    // would duplicate them and advance the shared file offset.
    used_ = 0;

    if (fileDescriptor_ >= 0) {
        ::close(fileDescriptor_);
        fileDescriptor_ = -1;
    }

    // Make flushFromSignal() a no-op here too, so a crash in the child cannot
    // reach for a descriptor that is gone.
    signalFlushDone_.test_and_set(std::memory_order_release);

    // Not counted as dropped: nothing was lost, the records still live in the
    // parent's buffer. Counting them would make the parent's own trace claim it
    // was incomplete when it is not.
}

}  // namespace leakhunter::agent
