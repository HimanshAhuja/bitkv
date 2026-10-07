#pragma once

#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "bitkv/db.h"
#include "file_util.h"

namespace bitkv::test {

// A fresh directory under /tmp, deleted recursively when it goes out of scope.
class TempDir {
 public:
  TempDir() {
    char tmpl[] = "/tmp/bitkv-test-XXXXXX";
    path_ = ::mkdtemp(tmpl);
  }
  ~TempDir() {
    ::nftw(path_.c_str(),
           [](const char* p, const struct stat*, int, struct FTW*) { return ::remove(p); }, 16,
           FTW_DEPTH | FTW_PHYS);
  }
  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

inline std::unique_ptr<DB> OpenOrDie(const std::string& dir, Options opts = {}) {
  std::unique_ptr<DB> db;
  Status s = DB::Open(opts, dir, &db);
  if (!s.ok()) std::abort();
  return db;
}

inline std::vector<uint32_t> DataFiles(const std::string& dir) {
  std::vector<uint32_t> ids;
  ListDataFiles(dir, &ids);
  return ids;
}

inline uint64_t FileSize(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0 ? uint64_t(st.st_size) : 0;
}

}  // namespace bitkv::test
