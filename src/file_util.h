// Thin wrappers over POSIX file I/O that handle the boring-but-critical parts:
// short writes, EINTR, and syncing the *directory* after creating or renaming
// a file (without that, a crash can leave the file's data on disk but its
// name missing from the directory).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "bitkv/status.h"

namespace bitkv {

// File naming: 000000001.data, 000000001.hint, plus a MERGE marker.
std::string DataFileName(const std::string& dir, uint32_t id);
std::string HintFileName(const std::string& dir, uint32_t id);
std::string MergeMarkerName(const std::string& dir);
std::string LockFileName(const std::string& dir);

// Returns the ids of every NNNNNNNNN.data file in dir, sorted ascending.
Status ListDataFiles(const std::string& dir, std::vector<uint32_t>* ids);

// Removes leftover *.tmp files from an interrupted merge.
Status RemoveTempFiles(const std::string& dir);

// Loops until all n bytes are written (write() may write fewer).
Status WriteAll(int fd, const char* data, size_t n);

// Loops until n bytes are read at `offset`. Fails if the file is shorter.
Status PreadAll(int fd, uint64_t offset, size_t n, char* buf);

Status ReadWholeFile(const std::string& path, std::string* out);

Status SyncFd(int fd);
Status SyncDir(const std::string& dir);

// Writes `contents` to path atomically: write path.tmp, fsync it, rename over
// path, fsync the directory. A reader sees either the old file or the new
// one, never a partial file.
Status AtomicWriteFile(const std::string& dir, const std::string& path,
                       const std::string& contents);

bool FileExists(const std::string& path);

std::string ErrnoMessage(const std::string& context);

}  // namespace bitkv
