#include "skate3_prop_service.h"

#include <httplib.h>

#include <rex/logging.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace skate3::prop_service {

namespace {

// Delivery is a long poll: the request PARKS on the server until the world
// actually changes, so a placement reaches every nearby client at once
// instead of on each one's next tick. This is only the gap between one
// parked request returning and the next being made.
constexpr auto kPollInterval = std::chrono::milliseconds(50);

// Streamed further than props are drawn (the default LOD is 300) so a prop
// ARRIVES before it needs drawing. Without that margin it would pop in at
// exactly the moment it became visible.
constexpr float kStreamRadius = 450.0f;

// Moving this far is reason enough to re-ask: new ground means new props.
constexpr float kResendDistance = 40.0f;

// Movement is not the only reason the answer changes - another player can
// place a prop beside someone standing still - but that case is now handled
// by the server waking the parked request, not by re-asking on a timer.

constexpr int kFailuresBeforeBackoff = 3;
constexpr auto kBackoffInterval = std::chrono::seconds(15);

struct State {
  std::mutex mutex;
  std::condition_variable wake;
  // TWO threads, deliberately.
  //
  // The streaming request PARKS on the server for up to twenty seconds so a
  // placement can wake it. A thread blocked in that HTTP read cannot be
  // reached by a condition-variable notify, so it cannot also be the thread
  // that sends placements: typing propplace would queue the placement and
  // then wait out the park before sending it, which looks exactly like the
  // prop not appearing until much later.
  std::thread worker;   // long-polls for nearby props
  std::thread sender;   // posts placements, so it is never blocked by a park
  bool running = false;

  std::string host;
  std::uint16_t port = 0;

  bool position_valid = false;
  float x = 0.0f, y = 0.0f, z = 0.0f;
  bool queried_once = false;
  float queried_x = 0.0f, queried_y = 0.0f, queried_z = 0.0f;
  // Store version this client has seen; the next request parks until the
  // server goes past it.
  std::uint64_t version = 0;

  // Ids already handed to the frame loop. Props are never removed from a
  // client (the engine has no despawn), so this only grows per session.
  std::unordered_set<std::uint32_t> known;
  std::vector<Spawn> pending;

  // Every prop this client has ever heard about, kept by id rather than
  // drained like `pending` - ListKnownProps (a script asking "what's
  // synced in the world") needs the WHOLE set every time it asks, not just
  // what arrived since the last ask. Filled at the exact same point
  // `known` is (see the ParseProps loop below); never removed for the
  // same reason `known` never shrinks.
  std::unordered_map<std::uint32_t, Spawn> all_props;

  // Placements waiting to be POSTed, so the caller never blocks.
  std::vector<Spawn> outbox;

  std::atomic<bool> store_present{false};
};

State& Get() {
  static State state;
  return state;
}

httplib::Client MakeClient(const std::string& host, std::uint16_t port) {
  httplib::Client client(host, port);
  client.set_connection_timeout(2, 0);
  // Longer than the server's 20s park, or every long poll would look like a
  // timeout and the client would back off from a perfectly healthy server.
  client.set_read_timeout(30, 0);
  client.set_write_timeout(5, 0);
  return client;
}

// Reads [{"id":N,"path":"...","x":..,"y":..,"z":..,"lod":..}] - the exact
// shape the relay writes, so a general JSON parser would be a dependency for
// one fixed format produced a few lines away.
std::vector<Spawn> ParseProps(const std::string& body) {
  std::vector<Spawn> props;
  std::size_t cursor = 0;
  const auto number = [&body](std::size_t from, const char* key) {
    const auto at = body.find(key, from);
    return at == std::string::npos
               ? 0.0f
               : std::strtof(body.c_str() + at + std::char_traits<char>::length(key),
                             nullptr);
  };
  while (true) {
    const auto id_key = body.find("\"id\":", cursor);
    if (id_key == std::string::npos) {
      break;
    }
    Spawn prop;
    prop.id = static_cast<std::uint32_t>(
        std::strtoul(body.c_str() + id_key + 5, nullptr, 10));
    const auto path_key = body.find("\"path\":\"", id_key);
    if (path_key == std::string::npos) {
      break;
    }
    std::size_t at = path_key + 8;
    while (at < body.size() && body[at] != '"') {
      if (body[at] == '\\' && at + 1 < body.size()) {
        ++at;
      }
      prop.path.push_back(body[at]);
      ++at;
    }
    prop.x = number(at, "\"x\":");
    prop.y = number(at, "\"y\":");
    prop.z = number(at, "\"z\":");
    prop.lod = number(at, "\"lod\":");
    if (prop.id != 0 && !prop.path.empty()) {
      props.push_back(std::move(prop));
    }
    cursor = at;
  }
  return props;
}

void SendPlacements(State& state, httplib::Client& client) {
  std::vector<Spawn> outbox;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    outbox.swap(state.outbox);
  }
  for (const Spawn& prop : outbox) {
    char body[512] = {};
    std::snprintf(body, sizeof(body), "%s|%.3f|%.3f|%.3f|%.1f",
                  prop.path.c_str(), prop.x, prop.y, prop.z, prop.lod);
    const auto response = client.Post("/api/props", body, "text/plain");
    if (!response || response->status / 100 != 2) {
      // Put it back: a placement is a deliberate act and losing it silently
      // would look like the game ignoring the player.
      std::lock_guard<std::mutex> lock(state.mutex);
      state.outbox.push_back(prop);
      return;
    }
    REXLOG_INFO("prop-service: placed {} at ({:.1f}, {:.1f}, {:.1f})",
                prop.path, prop.x, prop.y, prop.z);
    // No need to force a re-query: the placement bumps the store version,
    // which wakes this client's own parked request along with everyone
    // else's.
  }
}

bool PollNearby(State& state, httplib::Client& client) {
  float x = 0.0f, y = 0.0f, z = 0.0f;
  std::uint64_t since = 0;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.position_valid) {
      return true;  // nothing to ask about yet; not a failure
    }
    x = state.x;
    y = state.y;
    z = state.z;
    since = state.version;
    const float dx = x - state.queried_x;
    const float dy = y - state.queried_y;
    const float dz = z - state.queried_z;
    // Moving into new ground has to re-ask even if the world has not
    // changed, because the ANSWER is position-dependent: props that were
    // outside the radius are now inside it. `since = 0` asks the server not
    // to park, so this returns straight away with whatever is here.
    if (state.queried_once &&
        dx * dx + dy * dy + dz * dz >= kResendDistance * kResendDistance) {
      since = 0;
    }
  }

  char query[192] = {};
  std::snprintf(query, sizeof(query),
                "/api/props?x=%.2f&y=%.2f&z=%.2f&r=%.1f&since=%llu", x, y, z,
                kStreamRadius, static_cast<unsigned long long>(since));
  const auto response = client.Get(query);
  if (!response || response->status / 100 != 2) {
    return false;
  }
  state.store_present.store(true, std::memory_order_release);

  // {"version":N,"props":[...]} - the version is what the next request waits
  // past, so a client never re-parks on a change it has already seen.
  std::uint64_t version = since;
  const auto version_key = response->body.find("\"version\":");
  if (version_key != std::string::npos) {
    version = std::strtoull(response->body.c_str() + version_key + 10, nullptr,
                            10);
  }
  const std::vector<Spawn> props = ParseProps(response->body);
  std::size_t added = 0;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.queried_once = true;
    state.queried_x = x;
    state.queried_y = y;
    state.queried_z = z;
    state.version = version;
    for (const Spawn& prop : props) {
      if (state.known.insert(prop.id).second) {
        state.pending.push_back(prop);
        ++added;
      }
      // Independent of `known`/`pending` above: a prop already handed to
      // the frame loop still belongs in the full listing, so this is not
      // gated on insert() succeeding.
      state.all_props[prop.id] = prop;
    }
  }
  if (added != 0) {
    REXLOG_INFO("prop-service: {} new prop(s) near ({:.0f}, {:.0f}, {:.0f})",
                added, x, y, z);
  }
  return true;
}

// Placements only. Sleeps on the condition variable until there is
// something to send, so it costs nothing while idle and reacts instantly.
void SenderMain() {
  State& state = Get();
  while (true) {
    std::string host;
    std::uint16_t port = 0;
    {
      std::unique_lock<std::mutex> lock(state.mutex);
      state.wake.wait(lock, [&] { return !state.running || !state.outbox.empty(); });
      if (!state.running) {
        return;
      }
      host = state.host;
      port = state.port;
    }
    if (host.empty() || port == 0) {
      continue;
    }
    httplib::Client client = MakeClient(host, port);
    SendPlacements(state, client);
    // A failed send puts the placement back; pause briefly so a server that
    // is down does not turn this into a spin loop.
    bool retry_pending = false;
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      retry_pending = !state.outbox.empty();
    }
    if (retry_pending) {
      std::unique_lock<std::mutex> lock(state.mutex);
      state.wake.wait_for(lock, std::chrono::seconds(2),
                          [&] { return !state.running; });
    }
  }
}

void WorkerMain() {
  State& state = Get();
  int consecutive_failures = 0;
  while (true) {
    std::string host;
    std::uint16_t port = 0;
    {
      std::unique_lock<std::mutex> lock(state.mutex);
      const std::chrono::milliseconds interval =
          consecutive_failures >= kFailuresBeforeBackoff
              ? std::chrono::duration_cast<std::chrono::milliseconds>(
                    kBackoffInterval)
              : kPollInterval;
      state.wake.wait_for(lock, interval, [&] { return !state.running; });
      if (!state.running) {
        return;
      }
      host = state.host;
      port = state.port;
    }
    if (host.empty() || port == 0) {
      continue;
    }
    httplib::Client client = MakeClient(host, port);
    consecutive_failures = PollNearby(state, client)
                               ? 0
                               : consecutive_failures + 1;
  }
}

}  // namespace

void Configure(const std::string& host, std::uint16_t port) {
  State& state = Get();
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    const bool same = state.host == host && state.port == port;
    state.host = host;
    state.port = port;
    if (!same) {
      // A different server has a different world. Nothing already spawned
      // can be unspawned, but nothing new should be attributed to it.
      state.known.clear();
      state.pending.clear();
      state.all_props.clear();
      state.queried_once = false;
      state.store_present.store(false, std::memory_order_release);
    }
    if (!state.running) {
      state.running = true;
      state.worker = std::thread(&WorkerMain);
      state.sender = std::thread(&SenderMain);
    }
  }
  state.wake.notify_all();
}

void Shutdown() {
  State& state = Get();
  std::thread worker;
  std::thread sender;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.running) {
      return;
    }
    state.running = false;
    worker = std::move(state.worker);
    sender = std::move(state.sender);
  }
  state.wake.notify_all();
  // The long poll can still be parked on the server; its own read timeout
  // bounds this, which is why that timeout is finite rather than infinite.
  if (worker.joinable()) {
    worker.join();
  }
  if (sender.joinable()) {
    sender.join();
  }
  std::lock_guard<std::mutex> lock(state.mutex);
  state.known.clear();
  state.pending.clear();
  state.all_props.clear();
  state.outbox.clear();
  state.queried_once = false;
}

void SetLocalPosition(float x, float y, float z) {
  State& state = Get();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.x = x;
  state.y = y;
  state.z = z;
  state.position_valid = true;
}

void Place(const std::string& path, float x, float y, float z, float lod) {
  if (path.empty()) {
    return;
  }
  State& state = Get();
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.outbox.push_back({0, path, x, y, z, lod});
  }
  state.wake.notify_all();
}

std::vector<Spawn> TakeSpawns() {
  State& state = Get();
  std::lock_guard<std::mutex> lock(state.mutex);
  std::vector<Spawn> spawns;
  spawns.swap(state.pending);
  return spawns;
}

bool ServerHasStore() {
  return Get().store_present.load(std::memory_order_acquire);
}

std::vector<Spawn> ListKnownProps() {
  State& state = Get();
  std::lock_guard<std::mutex> lock(state.mutex);
  std::vector<Spawn> result;
  result.reserve(state.all_props.size());
  for (const auto& [id, spawn] : state.all_props) {
    (void)id;
    result.push_back(spawn);
  }
  return result;
}

}  // namespace skate3::prop_service
