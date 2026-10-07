// Server metrics in the Prometheus text exposition format.
//
// No client library: the format is plain text and simple enough to emit by
// hand, which keeps the server dependency-free. Format reference:
// https://prometheus.io/docs/instrumenting/exposition_formats/
//
// Everything here is touched only by the single event-loop thread, so no
// atomics or locks are needed.
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>

#include "bitkv/db.h"

namespace bitkv {

class Metrics {
 public:
  // Latency buckets in seconds, chosen to resolve the interesting range for a
  // storage server: page-cache reads take microseconds, an fsync takes
  // hundreds of microseconds to milliseconds.
  static constexpr std::array<double, 11> kBuckets = {
      0.00001, 0.000025, 0.00005, 0.0001, 0.00025, 0.0005,
      0.001,   0.0025,   0.005,   0.01,   0.05};

  Metrics() : start_(std::chrono::steady_clock::now()) {}

  // Records one executed command. `name` must already be normalised: an
  // unbounded set of label values (e.g. echoing whatever a client typed)
  // would create unbounded time series in Prometheus, a classic outage cause.
  void ObserveCommand(const std::string& name, double seconds, bool error);
  void ConnectionOpened() { ++connections_total_; ++connected_; }
  void ConnectionClosed() { --connected_; }

  std::string Render(const Stats& db) const;

 private:
  std::chrono::steady_clock::time_point start_;
  std::map<std::string, uint64_t> commands_;
  uint64_t errors_ = 0;
  std::array<uint64_t, kBuckets.size()> bucket_counts_{};
  uint64_t duration_count_ = 0;
  double duration_sum_ = 0;
  uint64_t connections_total_ = 0;
  int64_t connected_ = 0;
};

// Maps a client-supplied command name to a fixed label set.
std::string NormalizeCommand(const std::string& raw);

}  // namespace bitkv
