/// @file TraceWriter.hpp
/// @brief Buffered, allocation-free writer for the binary trace.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <pthread.h>

namespace leakhunter::agent {

/// Appends records to the trace file.
///
/// Constraints that shape this class:
///   * it must never allocate -- it is called from inside `malloc`;
///   * it must be safe from any thread;
///   * it must be usable before dynamic initialisation runs, so every member is
///     trivially constructible and the whole object lives in BSS.
///
/// Hence the raw pthread mutex and fixed buffer instead of std::mutex and
/// std::vector. Losing data is preferred over blocking indefinitely: if the
/// underlying write fails, records are counted as dropped and reported in the
/// trace summary rather than silently vanishing.
class TraceWriter {
public:
    static constexpr std::size_t kBufferSize = 1U << 20;  // 1 MiB

    /// Opens @p path for writing (truncating). @return false on failure.
    [[nodiscard]] bool open(const char* path) noexcept;

    /// One contiguous piece of a record.
    struct Chunk {
        const void* data;
        std::size_t bytes;
    };

    /// Appends @p count chunks as a single logical record: they all land in the
    /// buffer under one lock, so records from different threads never
    /// interleave. Scatter/gather rather than one buffer keeps variable-length
    /// payloads (stack frames, symbol names) copy-free at the call site.
    void appendRecord(const Chunk* chunks, std::size_t count) noexcept;

    void flush() noexcept;
    void close() noexcept;

    /// Best-effort flush from a fatal signal handler.
    ///
    /// Deliberately does NOT take the mutex: pthread_mutex_lock is not
    /// async-signal-safe, and the thread that holds it may be the one that just
    /// died. Taking it could deadlock and lose everything; skipping it risks a
    /// single torn record at the very end. Losing one record beats losing the
    /// whole trace of a program that just crashed.
    ///
    /// Idempotent, and safe to race with itself: only the first caller writes.
    void flushFromSignal() noexcept;

    [[nodiscard]] bool isOpen() const noexcept { return fileDescriptor_ >= 0; }
    [[nodiscard]] std::uint64_t droppedRecords() const noexcept { return droppedRecords_; }

    /// errno from the write that broke the trace, 0 if none has.
    ///
    /// `EBADF` means the target closed our descriptor -- the daemonisation
    /// idiom, usually. Naming that beats leaving the host to guess from an empty
    /// file, which it previously did, wrongly, by suggesting static linking.
    [[nodiscard]] int lastWriteErrno() const noexcept { return lastWriteErrno_; }

    /// Locks/unlocks around fork() so a forked child never inherits a held mutex.
    void lock() noexcept;
    void unlock() noexcept;

    /// Cuts a forked child loose from the parent's trace. Caller must hold the
    /// mutex (the atfork prepare handler does).
    ///
    /// Stopping the child from *recording* is not enough, and that gap silently
    /// corrupted every trace from a program that forked without exec'ing:
    ///
    ///   * the child inherits a **copy of the buffer**, still holding the
    ///     parent's unflushed records -- including the file header. Anything
    ///     that flushes in the child (the interposed `_exit`, a crash handler)
    ///     writes a second copy of all of it;
    ///   * the child inherits the **same open file description**, so it shares
    ///     the parent's file *offset*. Every byte the child writes pushes the
    ///     parent's next flush further into the file.
    ///
    /// The result was a trace containing the child's copy of the header and
    /// records, then a second header where the parent's flush landed. The host
    /// read the first copy, hit the second header, and reported a fraction of
    /// the leaks with no end marker. Measured: 50 leaks out of 100.
    ///
    /// So the child drops the buffer unwritten and closes its descriptor. Only
    /// the child's descriptor goes away; the parent's is untouched.
    void abandonInChild() noexcept;

private:
    /// Caller must hold the mutex.
    void appendLocked(const void* data, std::size_t bytes) noexcept;
    void flushLocked() noexcept;

    pthread_mutex_t mutex_{};
    int fileDescriptor_ = -1;
    std::size_t used_ = 0;
    std::uint64_t droppedRecords_ = 0;
    int lastWriteErrno_ = 0;
    std::atomic_flag signalFlushDone_ = ATOMIC_FLAG_INIT;
    unsigned char buffer_[kBufferSize]{};
};

}  // namespace leakhunter::agent
