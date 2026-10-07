#include "file_util.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace bitkv {

std::string ErrnoMessage(const std::string& context) {
  return context + ": " + std::strerror(errno);
}

static std::string IdName(const std::string& dir, uint32_t id, const char* ext) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%09u.%s", id, ext);
  return dir + "/" + buf;
}

std::string DataFileName(const std::string& dir, uint32_t id) { return IdName(dir, id, "data"); }
std::string HintFileName(const std::string& dir, uint32_t id) { return IdName(dir, id, "hint"); }
std::string MergeMarkerName(const std::string& dir) { return dir + "/MERGE"; }
std::string LockFileName(const std::string& dir) { return dir + "/LOCK"; }

static bool ParseDataName(const char* name, uint32_t* id) {
  // Exactly 9 digits followed by ".data".
  if (std::strlen(name) != 14 || std::strcmp(name + 9, ".data") != 0) return false;
  uint32_t v = 0;
  for (int i = 0; i < 9; ++i) {
    if (name[i] < '0' || name[i] > '9') return false;
    v = v * 10 + uint32_t(name[i] - '0');
  }
  *id = v;
  return true;
}

Status ListDataFiles(const std::string& dir, std::vector<uint32_t>* ids) {
  ids->clear();
  DIR* d = ::opendir(dir.c_str());
  if (!d) return Status::IOError(ErrnoMessage("opendir " + dir));
  while (dirent* e = ::readdir(d)) {
    uint32_t id;
    if (ParseDataName(e->d_name, &id)) ids->push_back(id);
  }
  ::closedir(d);
  std::sort(ids->begin(), ids->end());
  return Status::OK();
}

Status RemoveTempFiles(const std::string& dir) {
  DIR* d = ::opendir(dir.c_str());
  if (!d) return Status::IOError(ErrnoMessage("opendir " + dir));
  std::vector<std::string> doomed;
  while (dirent* e = ::readdir(d)) {
    std::string n = e->d_name;
    if (n.size() > 4 && n.compare(n.size() - 4, 4, ".tmp") == 0) doomed.push_back(dir + "/" + n);
  }
  ::closedir(d);
  for (const auto& p : doomed) ::unlink(p.c_str());
  return doomed.empty() ? Status::OK() : SyncDir(dir);
}

Status WriteAll(int fd, const char* data, size_t n) {
  while (n > 0) {
    ssize_t w = ::write(fd, data, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return Status::IOError(ErrnoMessage("write"));
    }
    data += w;
    n -= static_cast<size_t>(w);
  }
  return Status::OK();
}

Status PreadAll(int fd, uint64_t offset, size_t n, char* buf) {
  while (n > 0) {
    ssize_t r = ::pread(fd, buf, n, static_cast<off_t>(offset));
    if (r < 0) {
      if (errno == EINTR) continue;
      return Status::IOError(ErrnoMessage("pread"));
    }
    if (r == 0) return Status::Corruption("unexpected end of file");
    buf += r;
    n -= static_cast<size_t>(r);
    offset += static_cast<uint64_t>(r);
  }
  return Status::OK();
}

Status ReadWholeFile(const std::string& path, std::string* out) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return Status::IOError(ErrnoMessage("open " + path));
  struct stat st;
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    return Status::IOError(ErrnoMessage("fstat " + path));
  }
  out->resize(static_cast<size_t>(st.st_size));
  Status s = st.st_size == 0 ? Status::OK() : PreadAll(fd, 0, out->size(), out->data());
  ::close(fd);
  return s;
}

Status SyncFd(int fd) {
#if defined(__APPLE__)
  // On macOS, fsync() only pushes data to the drive, which may keep it in a
  // volatile cache and still lose it on power failure. F_FULLFSYNC asks the
  // drive itself to flush. Linux's fdatasync already issues that flush.
  if (::fcntl(fd, F_FULLFSYNC) != 0) return Status::IOError(ErrnoMessage("F_FULLFSYNC"));
#else
  // fdatasync skips flushing metadata like mtime that we never read back, so
  // it is cheaper than fsync. File *size* changes are still flushed, which is
  // what an append-only log needs.
  if (::fdatasync(fd) != 0) return Status::IOError(ErrnoMessage("fdatasync"));
#endif
  return Status::OK();
}

Status SyncDir(const std::string& dir) {
  int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return Status::IOError(ErrnoMessage("open dir " + dir));
  int rc = ::fsync(fd);
  ::close(fd);
  if (rc != 0) return Status::IOError(ErrnoMessage("fsync dir " + dir));
  return Status::OK();
}

Status AtomicWriteFile(const std::string& dir, const std::string& path,
                       const std::string& contents) {
  const std::string tmp = path + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return Status::IOError(ErrnoMessage("open " + tmp));
  Status s = WriteAll(fd, contents.data(), contents.size());
  if (s.ok()) s = SyncFd(fd);
  ::close(fd);
  if (!s.ok()) {
    ::unlink(tmp.c_str());
    return s;
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return Status::IOError(ErrnoMessage("rename " + tmp));
  }
  return SyncDir(dir);
}

bool FileExists(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0;
}

}  // namespace bitkv
