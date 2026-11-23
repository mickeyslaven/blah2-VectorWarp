#pragma once
// Shared cache policy for the recording writer and acquisition digest reader.
// File bytes, format, and close-time durability remain owned by their callers.
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

namespace blah2 {
struct RecordingFileCacheIo {
  static void prepare(int fd) {
#if defined(__APPLE__) && defined(F_NOCACHE)
    // Darwin's file-local cache hint also covers the digest read pass.
    (void)::fcntl(fd, F_NOCACHE, 1);
#else
    (void)fd;
#endif
  }
  static bool writeback(int fd, uint64_t offset, uint64_t bytes, bool wait) {
#if defined(__linux__) && defined(SYNC_FILE_RANGE_WRITE)
    const unsigned flags = SYNC_FILE_RANGE_WRITE |
      (wait ? SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WAIT_AFTER : 0);
    int result;
    do { result = ::sync_file_range(fd, offset, bytes, flags); }
    while (result < 0 && errno == EINTR);
    if (!result) return true;
    if (errno == ENOSYS || errno == EINVAL || errno == EOPNOTSUPP) return false;
    throw std::runtime_error("Recording range writeback failed");
#else
    (void)fd; (void)offset; (void)bytes; (void)wait;
    return false;
#endif
  }
  static void flush(int fd) {
    int result;
    do { result = ::fsync(fd); } while (result < 0 && errno == EINTR);
    if (result) throw std::runtime_error("Recording cache flush failed");
  }
  static void discard(int fd, uint64_t offset, uint64_t bytes) {
#if defined(POSIX_FADV_DONTNEED)
    // Advice may be unsupported. It must never change byte correctness or
    // turn a successfully persisted recording into a recording failure.
    if (bytes) (void)::posix_fadvise(fd, offset, bytes, POSIX_FADV_DONTNEED);
#else
    (void)fd; (void)offset; (void)bytes;
#endif
  }
};

template<class Io = RecordingFileCacheIo>
class RecordingFileCache {
  uint64_t released = 0, pending = 0;
  bool rangeWriteback = true;
public:
  static constexpr uint64_t windowBytes = 16ULL << 20;
  // Called only by the existing background writer. Keep at most two windows
  // awaiting writeback; disk stalls still meet the bounded queue's fail-closed
  // contract and never make acquisition wait for cache capacity.
  void written(int fd, uint64_t end) {
    const uint64_t complete = end - end % windowBytes;
    if (complete <= pending) return;
    if (pending > released) {
      if (!Io::writeback(fd, released, pending - released, true)) {
        rangeWriteback = false;
        Io::flush(fd);
      }
      Io::discard(fd, released, pending - released);
      released = pending;
    }
    if (rangeWriteback && Io::writeback(fd, released, complete - released, false)) {
      pending = complete;
    } else {
      rangeWriteback = false;
      Io::flush(fd);
      Io::discard(fd, released, complete - released);
      released = pending = complete;
    }
  }
  // A sequential digest has already consumed these bytes. Unlike writeback,
  // this does not require any filesystem synchronization or metadata changes.
  void consumed(int fd, uint64_t end, bool finished = false) {
    const uint64_t complete = finished ? end : end - end % windowBytes;
    if (complete <= released) return;
    Io::discard(fd, released, complete - released);
    released = complete;
  }
  static void durable(int fd, uint64_t end) {
    // Caller completed its final fsync, including the rewritten header.
    Io::discard(fd, 0, end);
  }
};
}
