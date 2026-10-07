#include "resp.h"

#include <charconv>

namespace bitkv::resp {
namespace {

constexpr long long kMaxArgs = 1024 * 1024;
constexpr long long kMaxBulk = 512ll * 1024 * 1024;
constexpr size_t kMaxInline = 64 * 1024;

// Finds "\r\n" at or after pos. Returns npos if absent.
size_t FindCrlf(std::string_view buf, size_t pos) {
  while (pos + 1 < buf.size()) {
    size_t r = buf.find('\r', pos);
    if (r == std::string_view::npos || r + 1 >= buf.size()) return std::string_view::npos;
    if (buf[r + 1] == '\n') return r;
    pos = r + 1;
  }
  return std::string_view::npos;
}

bool ParseInt(std::string_view s, long long* v) {
  if (s.empty()) return false;
  auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), *v);
  return ec == std::errc() && p == s.data() + s.size();
}

ParseResult ParseInline(std::string_view buf, std::vector<std::string>* args,
                        size_t* consumed, std::string* error) {
  size_t nl = buf.find('\n');
  if (nl == std::string_view::npos) {
    if (buf.size() > kMaxInline) {
      *error = "inline command too long";
      return ParseResult::kError;
    }
    return ParseResult::kIncomplete;
  }
  std::string_view line = buf.substr(0, nl);
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
  args->clear();
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && line[i] == ' ') ++i;
    size_t j = i;
    while (j < line.size() && line[j] != ' ') ++j;
    if (j > i) args->emplace_back(line.substr(i, j - i));
    i = j;
  }
  *consumed = nl + 1;
  return ParseResult::kOk;
}

}  // namespace

ParseResult ParseCommand(std::string_view buf, std::vector<std::string>* args,
                         size_t* consumed, std::string* error) {
  if (buf.empty()) return ParseResult::kIncomplete;
  if (buf[0] != '*') return ParseInline(buf, args, consumed, error);

  size_t pos = 0;
  size_t eol = FindCrlf(buf, pos);
  if (eol == std::string_view::npos) return ParseResult::kIncomplete;
  long long n;
  if (!ParseInt(buf.substr(1, eol - 1), &n) || n < 1 || n > kMaxArgs) {
    *error = "Protocol error: invalid multibulk length";
    return ParseResult::kError;
  }
  pos = eol + 2;

  std::vector<std::string> out;
  out.reserve(static_cast<size_t>(n));
  for (long long i = 0; i < n; ++i) {
    if (pos >= buf.size()) return ParseResult::kIncomplete;
    if (buf[pos] != '$') {
      *error = "Protocol error: expected '$'";
      return ParseResult::kError;
    }
    eol = FindCrlf(buf, pos);
    if (eol == std::string_view::npos) return ParseResult::kIncomplete;
    long long len;
    if (!ParseInt(buf.substr(pos + 1, eol - pos - 1), &len) || len < 0 || len > kMaxBulk) {
      *error = "Protocol error: invalid bulk length";
      return ParseResult::kError;
    }
    pos = eol + 2;
    const size_t need = static_cast<size_t>(len) + 2;
    if (buf.size() - pos < need) return ParseResult::kIncomplete;
    if (buf[pos + len] != '\r' || buf[pos + len + 1] != '\n') {
      *error = "Protocol error: bulk string not terminated by CRLF";
      return ParseResult::kError;
    }
    out.emplace_back(buf.substr(pos, static_cast<size_t>(len)));
    pos += need;
  }
  *args = std::move(out);
  *consumed = pos;
  return ParseResult::kOk;
}

void SimpleString(std::string* out, std::string_view s) {
  out->push_back('+');
  out->append(s);
  out->append("\r\n");
}

void Error(std::string* out, std::string_view msg) {
  out->push_back('-');
  out->append(msg);
  out->append("\r\n");
}

void Integer(std::string* out, long long v) {
  out->push_back(':');
  out->append(std::to_string(v));
  out->append("\r\n");
}

void Bulk(std::string* out, std::string_view s) {
  out->push_back('$');
  out->append(std::to_string(s.size()));
  out->append("\r\n");
  out->append(s);
  out->append("\r\n");
}

void NullBulk(std::string* out) { out->append("$-1\r\n"); }

void ArrayHeader(std::string* out, size_t n) {
  out->push_back('*');
  out->append(std::to_string(n));
  out->append("\r\n");
}

}  // namespace bitkv::resp
