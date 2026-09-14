#include "skate3_steam_backend.h"

// Steam P2P has been removed as a transport. This project now ships one
// online path - a dedicated relay server (skate3_dedicated.exe) - and Steam
// lobbies/matchmaking/Steam Networking Messages are no longer part of it.
//
// WHY A STUB AND NOT A DELETED FILE. The full public API this header
// declares (skate3_steam_backend.h) is called from three places -
// skate3_app_common.cpp (StartAvailabilityMonitor at process startup),
// skate3_multiplayer.cpp (the transport's own steam_active branch) and
// skate3_multiplayer_session.cpp (the Direct Connect / session menu's
// steam_available flag) - and none of those need to change: every one of
// them already treats "Steam unavailable" as a normal, handled state (it is
// what happens today whenever a player has no Steam client running), so
// answering that unconditionally removes the capability with a change
// confined to this one file. Rewriting each call site instead would touch
// skate3_multiplayer.cpp's ~40 steam:: references in its core transport
// logic for no behavioural difference - real risk, no benefit.
//
// The actual removal is what is ABSENT here: no steam_api64.dll download, no
// LoadLibraryW, no GetProcAddress, no lobby creation, no Steam Networking
// Messages send/receive. Nothing in this file can cause steam_api64.dll to
// be loaded, because nothing here names it. `Initialize()` never runs
// (nothing calls it - StartAvailabilityMonitor above is the only entry
// point exercised at startup, and it now does nothing), so there is no path
// through this translation unit that touches the filesystem, the registry,
// or the network on Steam's behalf.

namespace skate3::multiplayer::steam {

bool Initialize() { return false; }
bool IsInitialized() { return false; }
void StartAvailabilityMonitor() {}
void StopAvailabilityMonitor() {}
void RequestAvailabilityCheck() {}
void Tick() {}

State GetState() {
  State state;
  state.status = "Removed - use Direct Connect to a dedicated server.";
  return state;
}

void RefreshLobbies() {}

bool HostLobby(const std::string& /*server_name*/,
              const std::string& /*host_name*/,
              const std::string& /*map_name*/,
              std::uint32_t /*max_players*/, std::uint32_t /*privacy*/,
              bool /*allow_late_join*/, std::uint64_t /*password_hash*/) {
  return false;
}

bool JoinLobby(std::uint64_t /*lobby_id*/, std::uint64_t /*password_hash*/) {
  return false;
}

void LeaveLobby() {}
void Shutdown() {}

bool TransportActive() { return false; }
std::uint32_t LocalRole() { return 0; }
std::uint64_t HostSteamId() { return 0; }
std::vector<Peer> LobbyPeers() { return {}; }

bool SendPacketToPeer(std::uint64_t /*steam_id*/, const void* /*bytes*/,
                      std::size_t /*byte_count*/,
                      PacketReliability /*reliability*/) {
  return false;
}

std::vector<Message> ReceiveMessages(std::size_t /*maximum_messages*/) {
  return {};
}

std::vector<PeerTransportStatus> PeerTransportStatuses() { return {}; }

}  // namespace skate3::multiplayer::steam
