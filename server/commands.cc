#include "commands.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <climits>

#include "resp.h"

namespace bitkv {
namespace {

std::string Upper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return s;
}

void WrongArity(std::string* out, const std::string& cmd) {
  resp::Error(out, "ERR wrong number of arguments for '" + cmd + "' command");
}

void DbError(std::string* out, const Status& s) { resp::Error(out, "ERR " + s.ToString()); }

}  // namespace

void ExecuteCommand(DB* db, const std::vector<std::string>& args, std::string* out,
                    bool* quit) {
  *quit = false;
  if (args.empty()) return;
  const std::string cmd = Upper(args[0]);
  const size_t argc = args.size();

  if (cmd == "PING") {
    if (argc == 1) resp::SimpleString(out, "PONG");
    else if (argc == 2) resp::Bulk(out, args[1]);
    else WrongArity(out, "ping");

  } else if (cmd == "ECHO") {
    if (argc != 2) return WrongArity(out, "echo");
    resp::Bulk(out, args[1]);

  } else if (cmd == "SET") {
    if (argc != 3) return argc < 3 ? WrongArity(out, "set") : resp::Error(out, "ERR syntax error");
    Status s = db->Put(args[1], args[2]);
    s.ok() ? resp::SimpleString(out, "OK") : DbError(out, s);

  } else if (cmd == "GET") {
    if (argc != 2) return WrongArity(out, "get");
    std::string v;
    Status s = db->Get(args[1], &v);
    if (s.ok()) resp::Bulk(out, v);
    else if (s.IsNotFound()) resp::NullBulk(out);
    else DbError(out, s);

  } else if (cmd == "DEL" || cmd == "EXISTS") {
    if (argc < 2) return WrongArity(out, cmd == "DEL" ? "del" : "exists");
    long long n = 0;
    std::string scratch;
    for (size_t i = 1; i < argc; ++i) {
      Status g = db->Get(args[i], &scratch);
      if (!g.ok()) {
        if (!g.IsNotFound()) return DbError(out, g);
        continue;
      }
      if (cmd == "DEL") {
        Status s = db->Delete(args[i]);
        if (!s.ok()) return DbError(out, s);
      }
      ++n;
    }
    resp::Integer(out, n);

  } else if (cmd == "INCR" || cmd == "DECR") {
    if (argc != 2) return WrongArity(out, cmd == "INCR" ? "incr" : "decr");
    // Read-modify-write. Atomic with respect to other clients because the
    // server runs every command on its single event-loop thread, the same
    // reason INCR is atomic in Redis.
    std::string v;
    Status s = db->Get(args[1], &v);
    long long cur = 0;
    if (s.ok()) {
      auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), cur);
      if (ec != std::errc() || p != v.data() + v.size())
        return resp::Error(out, "ERR value is not an integer or out of range");
    } else if (!s.IsNotFound()) {
      return DbError(out, s);
    }
    const long long delta = cmd == "INCR" ? 1 : -1;
    if ((delta > 0 && cur == LLONG_MAX) || (delta < 0 && cur == LLONG_MIN))
      return resp::Error(out, "ERR increment or decrement would overflow");
    cur += delta;
    s = db->Put(args[1], std::to_string(cur));
    s.ok() ? resp::Integer(out, cur) : DbError(out, s);

  } else if (cmd == "DBSIZE") {
    resp::Integer(out, static_cast<long long>(db->Size()));

  } else if (cmd == "COMPACT") {
    // Not a Redis command: triggers a bitkv merge.
    Status s = db->Merge();
    s.ok() ? resp::SimpleString(out, "OK") : DbError(out, s);

  } else if (cmd == "INFO") {
    Stats st = db->GetStats();
    std::string info = "# bitkv\r\n";
    info += "live_keys:" + std::to_string(st.live_keys) + "\r\n";
    info += "data_files:" + std::to_string(st.data_files) + "\r\n";
    info += "disk_bytes:" + std::to_string(st.disk_bytes) + "\r\n";
    info += "live_bytes:" + std::to_string(st.live_bytes) + "\r\n";
    info += "space_amplification:" + std::to_string(st.SpaceAmplification()) + "\r\n";
    info += "fsyncs:" + std::to_string(st.fsyncs) + "\r\n";
    info += "writes:" + std::to_string(st.puts) + "\r\n";
    info += "recovered_torn_bytes:" + std::to_string(st.recovered_torn_bytes) + "\r\n";
    resp::Bulk(out, info);

  } else if (cmd == "CONFIG" || cmd == "COMMAND") {
    // redis-cli and redis-benchmark probe these at startup. An empty array
    // tells them "nothing configured" and lets them proceed.
    resp::ArrayHeader(out, 0);

  } else if (cmd == "QUIT") {
    resp::SimpleString(out, "OK");
    *quit = true;

  } else {
    resp::Error(out, "ERR unknown command '" + args[0] + "'");
  }
}

}  // namespace bitkv
