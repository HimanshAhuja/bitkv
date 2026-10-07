#include "metrics.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>

namespace bitkv {

std::string NormalizeCommand(const std::string& raw) {
  static const std::set<std::string> kKnown = {
      "ping", "echo", "set", "get", "del", "exists", "incr", "decr",
      "dbsize", "compact", "info", "config", "command", "quit"};
  std::string s = raw;
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return kKnown.count(s) ? s : "unknown";
}

void Metrics::ObserveCommand(const std::string& name, double seconds, bool error) {
  ++commands_[name];
  if (error) ++errors_;
  ++duration_count_;
  duration_sum_ += seconds;
  for (size_t i = 0; i < kBuckets.size(); ++i)
    if (seconds <= kBuckets[i]) ++bucket_counts_[i];  // cumulative buckets
}

namespace {

void Line(std::string* out, const char* name, const char* type, const char* help) {
  *out += "# HELP ";
  *out += name;
  *out += " ";
  *out += help;
  *out += "\n# TYPE ";
  *out += name;
  *out += " ";
  *out += type;
  *out += "\n";
}

std::string Num(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.10g", v);
  return buf;
}

void Sample(std::string* out, const char* name, double v, const std::string& labels = "") {
  *out += name;
  if (!labels.empty()) *out += "{" + labels + "}";
  *out += " " + Num(v) + "\n";
}

}  // namespace

std::string Metrics::Render(const Stats& db) const {
  std::string o;
  const double uptime =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();

  Line(&o, "bitkv_up", "gauge", "1 if the server is running.");
  Sample(&o, "bitkv_up", 1);
  Line(&o, "bitkv_uptime_seconds", "gauge", "Seconds since the server started.");
  Sample(&o, "bitkv_uptime_seconds", uptime);

  Line(&o, "bitkv_keys", "gauge", "Number of live keys.");
  Sample(&o, "bitkv_keys", double(db.live_keys));
  Line(&o, "bitkv_data_files", "gauge", "Number of data files on disk.");
  Sample(&o, "bitkv_data_files", double(db.data_files));
  Line(&o, "bitkv_disk_bytes", "gauge", "Total bytes in data files.");
  Sample(&o, "bitkv_disk_bytes", double(db.disk_bytes));
  Line(&o, "bitkv_live_bytes", "gauge", "Bytes of records still referenced by the index.");
  Sample(&o, "bitkv_live_bytes", double(db.live_bytes));
  Line(&o, "bitkv_space_amplification", "gauge",
       "disk_bytes / live_bytes. Compaction drives this toward 1.");
  Sample(&o, "bitkv_space_amplification", db.SpaceAmplification());
  Line(&o, "bitkv_recovered_torn_bytes", "gauge",
       "Bytes truncated from torn writes during the last startup.");
  Sample(&o, "bitkv_recovered_torn_bytes", double(db.recovered_torn_bytes));

  Line(&o, "bitkv_fsyncs_total", "counter", "fdatasync calls issued.");
  Sample(&o, "bitkv_fsyncs_total", double(db.fsyncs));
  Line(&o, "bitkv_writes_total", "counter", "Successful puts and deletes.");
  Sample(&o, "bitkv_writes_total", double(db.puts));

  Line(&o, "bitkv_connected_clients", "gauge", "Currently open client connections.");
  Sample(&o, "bitkv_connected_clients", double(connected_));
  Line(&o, "bitkv_connections_total", "counter", "Client connections accepted.");
  Sample(&o, "bitkv_connections_total", double(connections_total_));

  Line(&o, "bitkv_commands_total", "counter", "Commands executed, by command.");
  for (const auto& [cmd, n] : commands_)
    Sample(&o, "bitkv_commands_total", double(n), "command=\"" + cmd + "\"");
  Line(&o, "bitkv_command_errors_total", "counter", "Commands that returned an error.");
  Sample(&o, "bitkv_command_errors_total", double(errors_));

  Line(&o, "bitkv_command_duration_seconds", "histogram", "Command execution time.");
  for (size_t i = 0; i < kBuckets.size(); ++i)
    Sample(&o, "bitkv_command_duration_seconds_bucket", double(bucket_counts_[i]),
           "le=\"" + Num(kBuckets[i]) + "\"");
  Sample(&o, "bitkv_command_duration_seconds_bucket", double(duration_count_), "le=\"+Inf\"");
  Sample(&o, "bitkv_command_duration_seconds_sum", duration_sum_);
  Sample(&o, "bitkv_command_duration_seconds_count", double(duration_count_));
  return o;
}

}  // namespace bitkv
