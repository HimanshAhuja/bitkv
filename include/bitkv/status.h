// Status: the result of every fallible bitkv operation.
//
// We return a Status instead of throwing exceptions because storage code
// hits *expected* failures constantly (key not found, a torn record at the
// tail of a file, a full disk). Those are not exceptional; they are data the
// caller must branch on. This mirrors the convention in LevelDB and RocksDB.
#pragma once

#include <string>
#include <utility>

namespace bitkv {

class Status {
 public:
  enum class Code { kOk, kNotFound, kCorruption, kIOError, kInvalidArgument };

  Status() = default;  // OK

  static Status OK() { return Status(); }
  static Status NotFound(std::string msg = "") { return {Code::kNotFound, std::move(msg)}; }
  static Status Corruption(std::string msg) { return {Code::kCorruption, std::move(msg)}; }
  static Status IOError(std::string msg) { return {Code::kIOError, std::move(msg)}; }
  static Status InvalidArgument(std::string msg) {
    return {Code::kInvalidArgument, std::move(msg)};
  }

  bool ok() const { return code_ == Code::kOk; }
  bool IsNotFound() const { return code_ == Code::kNotFound; }
  bool IsCorruption() const { return code_ == Code::kCorruption; }
  bool IsIOError() const { return code_ == Code::kIOError; }
  Code code() const { return code_; }
  const std::string& message() const { return msg_; }

  std::string ToString() const {
    switch (code_) {
      case Code::kOk: return "OK";
      case Code::kNotFound: return "NotFound: " + msg_;
      case Code::kCorruption: return "Corruption: " + msg_;
      case Code::kIOError: return "IOError: " + msg_;
      case Code::kInvalidArgument: return "InvalidArgument: " + msg_;
    }
    return "Unknown";
  }

 private:
  Status(Code c, std::string m) : code_(c), msg_(std::move(m)) {}
  Code code_ = Code::kOk;
  std::string msg_;
};

}  // namespace bitkv
