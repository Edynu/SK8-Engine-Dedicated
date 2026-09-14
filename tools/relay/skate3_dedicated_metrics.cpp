// Relay bandwidth and loop-hitch counters.
//
// Split out of the server's main translation unit: it shares nothing with
// the rest of the server beyond the numbers it publishes, so it has no
// business sitting next to the packet loop.

#include "skate3_dedicated_server.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace skate3::dedicated::metrics {
namespace {

struct RelayMetrics {
  static constexpr std::uint64_t kWindowMicroseconds = 1'000'000;
  static constexpr std::uint64_t kHitchThresholdMicroseconds = 20'000;

  std::atomic<std::uint64_t> bytes_in{0};
  std::atomic<std::uint64_t> bytes_out{0};
  std::atomic<std::uint64_t> packets_in{0};
  std::atomic<std::uint64_t> packets_out{0};

  // Rolled over once a second, the same shape as LuaScriptHost's resmon
  // window: *_last_second is last window's total, not a live partial one.
  std::atomic<std::uint64_t> bytes_in_last_second{0};
  std::atomic<std::uint64_t> bytes_out_last_second{0};
  std::atomic<std::uint64_t> hitches_last_second{0};
  std::atomic<std::uint64_t> worst_iteration_us_last_second{0};
  std::atomic<std::uint64_t> hitches_total{0};

  std::uint64_t window_bytes_in_ = 0;
  std::uint64_t window_bytes_out_ = 0;
  std::uint64_t window_hitches_ = 0;
  std::uint64_t window_worst_us_ = 0;
  std::uint64_t window_start_us_ = 0;

  void RecordReceive(std::size_t bytes) {
    bytes_in.fetch_add(bytes, std::memory_order_relaxed);
    packets_in.fetch_add(1, std::memory_order_relaxed);
    window_bytes_in_ += bytes;
  }
  void RecordSend(std::size_t bytes) {
    bytes_out.fetch_add(bytes, std::memory_order_relaxed);
    packets_out.fetch_add(1, std::memory_order_relaxed);
    window_bytes_out_ += bytes;
  }
  // Called once per relay-loop iteration, from the single thread that owns
  // the loop - only the *_last_second atomics are written here, so a
  // concurrent reader on the HTTP thread never observes a torn update.
  void RecordIteration(std::uint64_t duration_us, std::uint64_t now_us) {
    if (duration_us > kHitchThresholdMicroseconds) {
      ++window_hitches_;
      ++hitches_total;  // cumulative; never resets, unlike the windowed count
    }
    window_worst_us_ = std::max(window_worst_us_, duration_us);
    if (window_start_us_ == 0) {
      window_start_us_ = now_us;
    }
    if (now_us - window_start_us_ < kWindowMicroseconds) {
      return;
    }
    bytes_in_last_second.store(window_bytes_in_, std::memory_order_relaxed);
    bytes_out_last_second.store(window_bytes_out_, std::memory_order_relaxed);
    hitches_last_second.store(window_hitches_, std::memory_order_relaxed);
    worst_iteration_us_last_second.store(window_worst_us_,
                                         std::memory_order_relaxed);
    window_bytes_in_ = 0;
    window_bytes_out_ = 0;
    window_hitches_ = 0;
    window_worst_us_ = 0;
    window_start_us_ = now_us;
  }
};

RelayMetrics g_relay_metrics;

}  // namespace

std::string FormatTable() {
  const double kb_in = g_relay_metrics.bytes_in_last_second.load(
                          std::memory_order_relaxed) /
                      1024.0;
  const double kb_out = g_relay_metrics.bytes_out_last_second.load(
                           std::memory_order_relaxed) /
                       1024.0;
  char line[320];
  std::snprintf(
      line, sizeof(line),
      "down %.1f KB/s   up %.1f KB/s   hitches/s %llu (worst %.1f ms)   "
      "hitches total %llu   total in %llu KB, out %llu KB",
      kb_in, kb_out,
      static_cast<unsigned long long>(g_relay_metrics.hitches_last_second.load(
          std::memory_order_relaxed)),
      g_relay_metrics.worst_iteration_us_last_second.load(
          std::memory_order_relaxed) /
          1000.0,
      static_cast<unsigned long long>(
          g_relay_metrics.hitches_total.load(std::memory_order_relaxed)),
      static_cast<unsigned long long>(
          g_relay_metrics.bytes_in.load(std::memory_order_relaxed) / 1024),
      static_cast<unsigned long long>(
          g_relay_metrics.bytes_out.load(std::memory_order_relaxed) / 1024));
  return line;
}

// JSON twin of FormatRelayMetricsTable, for /api/metrics. Keys named to sit
// naturally alongside LuaScriptHost::ResourceMetricsJson's array under one
// dashboard payload rather than to match any particular client.
std::string Json() {
  char json[512];
  std::snprintf(
      json, sizeof(json),
      "{\"bytesInPerSec\":%llu,\"bytesOutPerSec\":%llu,"
      "\"hitchesPerSec\":%llu,\"worstIterationMs\":%.3f,"
      "\"hitchesTotal\":%llu,\"bytesInTotal\":%llu,\"bytesOutTotal\":%llu}",
      static_cast<unsigned long long>(g_relay_metrics.bytes_in_last_second.load(
          std::memory_order_relaxed)),
      static_cast<unsigned long long>(g_relay_metrics.bytes_out_last_second.load(
          std::memory_order_relaxed)),
      static_cast<unsigned long long>(g_relay_metrics.hitches_last_second.load(
          std::memory_order_relaxed)),
      g_relay_metrics.worst_iteration_us_last_second.load(
          std::memory_order_relaxed) /
          1000.0,
      static_cast<unsigned long long>(
          g_relay_metrics.hitches_total.load(std::memory_order_relaxed)),
      static_cast<unsigned long long>(
          g_relay_metrics.bytes_in.load(std::memory_order_relaxed)),
      static_cast<unsigned long long>(
          g_relay_metrics.bytes_out.load(std::memory_order_relaxed)));
  return json;
}


void RecordReceive(std::size_t bytes) { g_relay_metrics.RecordReceive(bytes); }

void RecordSend(std::size_t bytes) { g_relay_metrics.RecordSend(bytes); }

void RecordIteration(std::uint64_t now_us, std::uint64_t iteration_us) {
  g_relay_metrics.RecordIteration(iteration_us, now_us);
}

}  // namespace skate3::dedicated::metrics
