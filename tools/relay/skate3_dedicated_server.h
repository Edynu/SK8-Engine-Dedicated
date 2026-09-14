#pragma once

// Shared surface of the dedicated server.
//
// The server started as one translation unit with everything in an anonymous
// namespace. That was fine while it was a packet relay; it is not fine now
// that it owns appearances, player identity, routing buckets, script events
// and a Lua native surface that is expected to grow. This header is what
// lets those live in separate files: the pieces that genuinely have to be
// shared, and nothing else.
//
// WHAT STAYS CLIENT-AUTHORITATIVE: player movement. Clients send their own
// position and pose and the server forwards them. The server is
// authoritative over everything ABOUT a session - who is connected, what
// role they hold, who can see whom, what they are wearing, which routing
// bucket they are in - but it does not simulate skaters and is not trying
// to. Anything added here should respect that line.

#include "skate3_multiplayer_routing.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <WS2tcpip.h>
#include <WinSock2.h>
using SocketHandle = SOCKET;
inline constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
inline constexpr SocketHandle kInvalidSocket = -1;
#endif

namespace skate3::dedicated {

// Everything server.cfg and the command line can set. Parsed once at
// startup and then read-only, so it is passed by const reference rather
// than reached for through a global.
struct Options {
  int port = 27200;
  std::string token;
  float radius = 0.0f;
  std::string resources_dir = "resources";
  int admin_port = 27280;
  std::string config_path = "server.cfg";
  std::string hostname = "Skate 3 Server";
  int max_players = 16;
  skate3::multiplayer::routing::FidelityTiers tiers;
  // Hot zones: neighbour counts past which a crowded area is thinned.
  skate3::multiplayer::routing::CrowdPolicy crowd;
  // Root-snapshot rate in Hz, separate from the animation pose rate.
  std::int32_t pose_hz = 30;
  // Refresh rates for the detail bone groups. Fingers matter - a board grab
  // or a handplant is all hands - so they keep a real rate; facial
  // expression does not survive the distance, so it rides the head and
  // refreshes on keyframes only.
  std::int32_t hands_hz = 3;
  std::int32_t face_hz = 0;
  // Per-band animation rates in Hz. high_hz is what connected clients are
  // told to SEND at; the other two are what the relay thins that down to
  // for more distant recipients.
  std::int32_t high_hz = 20;
  std::int32_t medium_hz = 10;
  std::int32_t low_hz = 5;
  // Resources named by server.cfg's `ensure` lines. Empty means start
  // everything that was discovered, which is what this did before the
  // config file existed.
  std::vector<std::string> ensured;
  // Anything else in server.cfg, verbatim, for scripts to read through
  // GetConvar.
  std::unordered_map<std::string, std::string> convars;
};

// server.cfg first, then the command line, so a flag can override a file.
void ApplyConfigFile(const std::string &path, Options &options);
[[nodiscard]] Options ParseArgs(int argc, char **argv);

// Bandwidth and loop-hitch counters, published through /api/metrics and the
// netstat console command. Safe to call from any thread.
namespace metrics {

void RecordReceive(std::size_t bytes);
void RecordSend(std::size_t bytes);
// Called once per relay-loop iteration with how long that iteration took;
// iterations past the hitch threshold are counted separately.
void RecordIteration(std::uint64_t now_us, std::uint64_t iteration_us);

[[nodiscard]] std::string FormatTable();
[[nodiscard]] std::string Json();

}  // namespace metrics

}  // namespace skate3::dedicated
