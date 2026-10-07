// RESP (REdis Serialization Protocol) v2: just enough to talk to redis-cli
// and redis-benchmark.
//
// A client sends each command as an array of bulk strings:
//
//   *3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n     ->  ["SET","foo","bar"]
//
// The parser is incremental: bytes arrive in arbitrary chunks from the
// socket, so a command can be split across reads, and one read can contain
// several pipelined commands. ParseCommand either consumes one complete
// command or reports that it needs more bytes, never consuming partial input.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace bitkv::resp {

enum class ParseResult { kOk, kIncomplete, kError };

// Parses one command from the front of buf. On kOk, *consumed bytes form
// the command and args holds its arguments. Also accepts the plain-text
// "inline" form (e.g. "PING\r\n") that telnet users type.
ParseResult ParseCommand(std::string_view buf, std::vector<std::string>* args,
                         size_t* consumed, std::string* error);

// Reply encoders. Each appends to *out.
void SimpleString(std::string* out, std::string_view s);  // +OK
void Error(std::string* out, std::string_view msg);         // -ERR ...
void Integer(std::string* out, long long v);                // :42
void Bulk(std::string* out, std::string_view s);            // $3\r\nbar
void NullBulk(std::string* out);                            // $-1
void ArrayHeader(std::string* out, size_t n);               // *2

}  // namespace bitkv::resp
