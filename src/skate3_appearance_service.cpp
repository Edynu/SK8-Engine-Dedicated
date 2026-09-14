#include "skate3_appearance_service.h"

#include <httplib.h>

#include <rex/logging.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace skate3::appearance_service {

namespace {

// Pause between roster requests. Short on purpose: the request itself is a
// LONG POLL that the server parks until this client's answer actually
// changes, so the pacing lives on the server and this is only here to stop a
// server that answers instantly from spinning the worker.
constexpr auto kRosterInterval = std::chrono::milliseconds(100);

// Must exceed the server's long-poll budget, or every parked request would
// look like a timeout and trip the backoff below.
constexpr int kReadTimeoutSeconds = 30;

// A server that is not running the store must not be polled forever at full
// rate. After this many consecutive failures the poll backs right off; any
// success resets it.
constexpr int kFailuresBeforeBackoff = 3;
constexpr auto kBackoffInterval = std::chrono::seconds(15);

struct State {
  std::mutex mutex;
  std::condition_variable wake;
  std::thread worker;
  bool running = false;

  std::string host;
  std::uint16_t port = 0;
  std::uint32_t local_role = 0;

  // Pending upload, and what has already been uploaded. Identities are
  // content hashes, so "uploaded once" is permanently true.
  std::uint64_t pending_identity = 0;
  Blob pending_bytes;
  std::unordered_set<std::uint64_t> uploaded;

  // role -> identity this client already holds.
  std::unordered_map<std::uint32_t, std::uint64_t> interest;
  // Requests already satisfied or in flight, so a slow download is not
  // restarted every roster poll.
  std::unordered_map<std::uint32_t, std::uint64_t> requested;

  // role -> display name, as the server last reported it. Authoritative:
  // the server owns identity, including the "(2)" suffix it adds when two
  // connections present the same name.
  std::unordered_map<std::uint32_t, std::string> peer_names;

  // Last version the server reported for THIS client's visible roster. Sent
  // back on the next request so the server can park until it differs.
  std::uint64_t roster_version = 0;

  std::vector<Fetched> fetched;
  std::atomic<bool> store_present{false};
};

State& Get() {
  static State state;
  return state;
}

httplib::Client MakeClient(const std::string& host, std::uint16_t port) {
  httplib::Client client(host, port);
  // Connect fast (a server that is not there should fail immediately) but
  // read patiently: a roster request is expected to park server-side until
  // something changes.
  client.set_connection_timeout(2, 0);
  client.set_read_timeout(kReadTimeoutSeconds, 0);
  client.set_write_timeout(5, 0);
  return client;
}

std::string HexIdentity(std::uint64_t identity) {
  char text[17] = {};
  std::snprintf(text, sizeof(text), "%016llX",
                static_cast<unsigned long long>(identity));
  return text;
}

// Minimal reader for [{"role":N,"id":"HEX"},...]. A full JSON parser would be
// a dependency for one fixed shape this file also produces.
struct RosterEntry {
  std::uint32_t role = 0;
  std::uint64_t identity = 0;
  std::string name;
};

std::vector<RosterEntry> ParseRoster(const std::string& body) {
  std::vector<RosterEntry> roster;
  std::size_t cursor = 0;
  while (true) {
    const auto role_key = body.find("\"role\":", cursor);
    if (role_key == std::string::npos) {
      break;
    }
    const std::uint32_t role = static_cast<std::uint32_t>(
        std::strtoul(body.c_str() + role_key + 7, nullptr, 10));
    const auto id_key = body.find("\"id\":\"", role_key);
    if (id_key == std::string::npos) {
      break;
    }
    const std::uint64_t identity =
        std::strtoull(body.c_str() + id_key + 6, nullptr, 16);
    std::string name;
    const auto name_key = body.find("\"name\":\"", id_key);
    const auto next_role = body.find("\"role\":", id_key);
    if (name_key != std::string::npos &&
        (next_role == std::string::npos || name_key < next_role)) {
      for (std::size_t at = name_key + 8; at < body.size(); ++at) {
        if (body[at] == '\\' && at + 1 < body.size()) {
          name.push_back(body[++at]);
          continue;
        }
        if (body[at] == '"') {
          break;
        }
        name.push_back(body[at]);
      }
    }
    if (role != 0) {
      roster.push_back({role, identity, std::move(name)});
    }
    cursor = id_key + 6;
  }
  return roster;
}

// Sends the local appearance if one is pending. Returns false when the
// request failed, so the caller can count failures toward backing off.
bool UploadPending(State& state, httplib::Client& client) {
  std::uint64_t identity = 0;
  Blob bytes;
  std::uint32_t role = 0;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.pending_identity == 0 || state.pending_bytes == nullptr) {
      return true;
    }
    identity = state.pending_identity;
    bytes = state.pending_bytes;
    role = state.local_role;
  }

  const std::string path =
      "/api/appearance/" + HexIdentity(identity) +
      (role != 0 ? "?role=" + std::to_string(role) : std::string());
  const auto response = client.Post(
      path.c_str(),
      reinterpret_cast<const char*>(bytes->data()), bytes->size(),
      "application/octet-stream");
  if (!response || response->status / 100 != 2) {
    return false;
  }

  std::lock_guard<std::mutex> lock(state.mutex);
  state.uploaded.insert(identity);
  if (state.pending_identity == identity) {
    state.pending_identity = 0;
    state.pending_bytes = nullptr;
  }
  REXLOG_INFO("appearance-service: uploaded {} ({} bytes) as role {}",
              HexIdentity(identity), bytes->size(), role);
  return true;
}

// Polls the roster and downloads anything this client is missing for a peer
// it can currently see. Returns false on a transport failure.
bool PollRoster(State& state, httplib::Client& client) {
  std::string path;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    // The server answers for THIS role specifically - it decides who is
    // visible - and parks until its answer differs from `since`.
    path = "/api/appearance?role=" + std::to_string(state.local_role) +
           "&since=" + std::to_string(state.roster_version) + "&wait=1";
  }
  const auto response = client.Get(path.c_str());
  if (!response || response->status / 100 != 2) {
    return false;
  }
  state.store_present.store(true, std::memory_order_release);

  std::uint64_t version = 0;
  const auto version_key = response->body.find("\"version\":");
  if (version_key != std::string::npos) {
    version = std::strtoull(response->body.c_str() + version_key + 10, nullptr,
                            10);
  }

  const auto roster = ParseRoster(response->body);
  std::vector<std::pair<std::uint32_t, std::uint64_t>> wanted;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.roster_version = version;
    for (const auto& entry : roster) {
      const std::uint32_t role = entry.role;
      const std::uint64_t identity = entry.identity;
      if (!entry.name.empty()) {
        state.peer_names[role] = entry.name;
      }
      if (role == state.local_role || identity == 0) {
        continue;
      }
      // The server has already scoped this to peers in range, so the only
      // question left is whether this client still needs the bytes.
      const auto interest = state.interest.find(role);
      if (interest != state.interest.end() && interest->second == identity) {
        continue;
      }
      const auto requested = state.requested.find(role);
      if (requested != state.requested.end() && requested->second == identity) {
        continue;  // already downloaded or in flight
      }
      state.requested[role] = identity;
      wanted.push_back({role, identity});
    }
  }

  for (const auto& [role, identity] : wanted) {
    const auto blob = client.Get(("/api/appearance/" + HexIdentity(identity)).c_str());
    if (!blob || blob->status / 100 != 2 || blob->body.empty()) {
      // A 404 is normal: the server may not have this appearance, and the
      // peer-to-peer transfer remains the fallback. Forget the request so a
      // later upload of the same id is picked up.
      std::lock_guard<std::mutex> lock(state.mutex);
      const auto requested = state.requested.find(role);
      if (requested != state.requested.end() && requested->second == identity) {
        state.requested.erase(requested);
      }
      continue;
    }
    Fetched entry;
    entry.role = role;
    entry.identity = identity;
    entry.bytes = std::make_shared<const std::vector<std::uint8_t>>(
        blob->body.begin(), blob->body.end());
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      state.fetched.push_back(std::move(entry));
    }
    REXLOG_INFO("appearance-service: fetched {} for role {} ({} bytes)",
                HexIdentity(identity), role, blob->body.size());
  }
  return true;
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
              : kRosterInterval;
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
    const bool uploaded = UploadPending(state, client);
    const bool polled = PollRoster(state, client);
    const bool healthy = uploaded && polled;
    // Logged on the TRANSITION only. Silence here used to hide a store that
    // was never actually carrying anything: the peer-to-peer path won every
    // race, so nothing downstream ever noticed the uploads failing.
    if (!healthy && consecutive_failures == 0) {
      REXLOG_WARN("appearance-service: server store unreachable at {}:{} "
                  "(upload={} roster={})",
                  host, port, uploaded ? "ok" : "failed",
                  polled ? "ok" : "failed");
    } else if (healthy && consecutive_failures != 0) {
      REXLOG_INFO("appearance-service: server store reachable again at {}:{}",
                  host, port);
    }
    consecutive_failures = healthy ? 0 : consecutive_failures + 1;
  }
}

}  // namespace

void Configure(const std::string& host, std::uint16_t port,
               std::uint32_t local_role) {
  State& state = Get();
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    const bool same = state.host == host && state.port == port &&
                      state.local_role == local_role;
    state.host = host;
    state.port = port;
    state.local_role = local_role;
    if (!same) {
      // A different server knows nothing about what was uploaded to the
      // previous one, and peers must be re-fetched from it.
      state.uploaded.clear();
      state.requested.clear();
      state.fetched.clear();
      state.store_present.store(false, std::memory_order_release);
    }
    if (!state.running) {
      state.running = true;
      state.worker = std::thread(&WorkerMain);
    }
  }
  state.wake.notify_all();
}

void Shutdown() {
  State& state = Get();
  std::thread worker;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.running) {
      return;
    }
    state.running = false;
    worker = std::move(state.worker);
  }
  state.wake.notify_all();
  if (worker.joinable()) {
    worker.join();
  }
  std::lock_guard<std::mutex> lock(state.mutex);
  state.uploaded.clear();
  state.requested.clear();
  state.fetched.clear();
  state.pending_identity = 0;
  state.pending_bytes = nullptr;
}

void PublishLocal(std::uint64_t identity, const Blob& bytes) {
  if (identity == 0 || bytes == nullptr || bytes->empty()) {
    return;
  }
  State& state = Get();
  bool wake = false;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.running || state.uploaded.count(identity) != 0 ||
        state.pending_identity == identity) {
      return;
    }
    state.pending_identity = identity;
    state.pending_bytes = bytes;
    wake = true;
  }
  if (wake) {
    state.wake.notify_all();
  }
}

void SetPeerInterest(
    const std::vector<std::pair<std::uint32_t, std::uint64_t>>& peers) {
  State& state = Get();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.interest.clear();
  for (const auto& [role, identity] : peers) {
    state.interest[role] = identity;
  }
  // A peer that has gone away should not keep a request reserved: if it
  // comes back the roster will offer it again and this client must re-ask.
  for (auto entry = state.requested.begin();
       entry != state.requested.end();) {
    entry = state.interest.count(entry->first) == 0
                ? state.requested.erase(entry)
                : std::next(entry);
  }
}

std::string PeerName(std::uint32_t role) {
  State& state = Get();
  std::lock_guard<std::mutex> lock(state.mutex);
  const auto found = state.peer_names.find(role);
  return found == state.peer_names.end() ? std::string() : found->second;
}

std::vector<Fetched> TakeFetched() {
  State& state = Get();
  std::lock_guard<std::mutex> lock(state.mutex);
  std::vector<Fetched> fetched;
  fetched.swap(state.fetched);
  return fetched;
}

bool ServerHasStore() {
  return Get().store_present.load(std::memory_order_acquire);
}

}  // namespace skate3::appearance_service
