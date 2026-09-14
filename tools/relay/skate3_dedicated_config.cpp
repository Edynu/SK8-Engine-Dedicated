// server.cfg and command-line parsing.
//
// Split out of the main translation unit: configuration is read once at
// startup and touches nothing else in the server, so it does not belong
// next to the packet loop.

#include "skate3_dedicated_server.h"

#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace skate3::dedicated {
namespace {

}  // namespace

void ApplyConfigFile(const std::string &path, Options &options) {
  std::ifstream file(path);
  if (!file) {
    return;
  }
  std::string line;
  while (std::getline(file, line)) {
    // Strip comments and surrounding whitespace.
    const auto comment = line.find('#');
    if (comment != std::string::npos) {
      line.erase(comment);
    }
    const auto first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos) {
      continue;
    }
    const auto last = line.find_last_not_of(" \t\r");
    line = line.substr(first, last - first + 1);

    const auto space = line.find_first_of(" \t");
    if (space == std::string::npos) {
      continue;
    }
    const std::string key = line.substr(0, space);
    std::string value = line.substr(space + 1);
    const auto value_start = value.find_first_not_of(" \t");
    if (value_start == std::string::npos) {
      continue;
    }
    value = value.substr(value_start);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
      value = value.substr(1, value.size() - 2);
    }

    if (key == "sv_hostname") {
      options.hostname = value;
    } else if (key == "sv_maxplayers") {
      options.max_players = std::atoi(value.c_str());
    } else if (key == "sv_token" || key == "sv_password") {
      options.token = value;
    } else if (key == "sv_port") {
      options.port = std::atoi(value.c_str());
    } else if (key == "sv_adminport") {
      options.admin_port = std::atoi(value.c_str());
    } else if (key == "sv_resources") {
      options.resources_dir = value;
    } else if (key == "sv_radius") {
      options.radius = static_cast<float>(std::atof(value.c_str()));
    } else if (key == "fidelity_high") {
      options.tiers.high_distance = static_cast<float>(std::atof(value.c_str()));
    } else if (key == "fidelity_medium") {
      options.tiers.medium_distance =
          static_cast<float>(std::atof(value.c_str()));
    } else if (key == "hands_hz") {
      options.hands_hz = std::atoi(value.c_str());
    } else if (key == "face_hz") {
      options.face_hz = std::atoi(value.c_str());
    } else if (key == "pose_hz") {
      options.pose_hz = std::atoi(value.c_str());
    } else if (key == "high_hz") {
      options.high_hz = std::atoi(value.c_str());
    } else if (key == "medium_hz") {
      options.medium_hz = std::atoi(value.c_str());
    } else if (key == "low_hz") {
      options.low_hz = std::atoi(value.c_str());
    } else if (key == "crowd_medium_above") {
      options.crowd.medium_above =
          static_cast<std::uint32_t>(std::atoi(value.c_str()));
    } else if (key == "crowd_low_above") {
      options.crowd.low_above =
          static_cast<std::uint32_t>(std::atoi(value.c_str()));
    } else if (key == "fidelity_low") {
      options.tiers.low_distance = static_cast<float>(std::atof(value.c_str()));
    } else if (key == "ensure" || key == "start") {
      options.ensured.push_back(value);
    } else {
      options.convars[key] = value;
    }
  }
}

namespace {

std::optional<std::string_view> ValueAfter(std::string_view arg,
                                           std::string_view prefix) {
  if (arg.substr(0, prefix.size()) != prefix) {
    return std::nullopt;
  }
  return arg.substr(prefix.size());
}

}  // namespace

Options ParseArgs(int argc, char **argv) {
  Options options;
  // An explicit --config= has to be found before the file is read.
  for (int i = 1; i < argc; ++i) {
    if (const auto value = ValueAfter(argv[i], "--config=")) {
      options.config_path = std::string(*value);
    }
  }
  ApplyConfigFile(options.config_path, options);
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (const auto value = ValueAfter(arg, "--port=")) {
      options.port = std::atoi(std::string(*value).c_str());
    } else if (const auto value = ValueAfter(arg, "--token=")) {
      options.token = std::string(*value);
    } else if (const auto value = ValueAfter(arg, "--radius=")) {
      options.radius = static_cast<float>(std::atof(std::string(*value).c_str()));
    } else if (const auto value = ValueAfter(arg, "--resources=")) {
      options.resources_dir = std::string(*value);
    } else if (const auto value = ValueAfter(arg, "--admin-port=")) {
      options.admin_port = std::atoi(std::string(*value).c_str());
    }
  }
  return options;
}

// Relay-owned role allocation. VisualRelayRouter authenticates and routes;
// deciding *which* free role a new connection gets is a policy choice that
// belongs to the relay process, not the shared header.
// Roles are handed out LEAST-RECENTLY-RELEASED rather than lowest-free.
//
// Lowest-free means a player who leaves has their id given straight to the
// next person to join, and everything keyed by player id then attaches to
// the wrong human: a scoreboard row, a game mode's per-player state, a
// pending event addressed to whoever id 1 was a moment ago. Recycling it
// immediately is the one allocation order guaranteed to cause that.
//
// A rejoining player therefore gets a fresh id - 1 and 2 in use means the
// next assignment is 3, even if 1 has just left. The id space is bounded
// (the protocol carries a role as a 16-bit field and this relay caps at
// 100), so ids cannot be strictly monotonic forever; a released one does
// come back, but only after every other free id has been used first, which
// is the longest reuse delay a bounded space allows.

}  // namespace skate3::dedicated
