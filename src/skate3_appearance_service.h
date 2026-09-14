#pragma once

// Client half of the server-held appearance store (see
// skate3_appearance_store.h for the server half and why it exists).
//
// The client uploads its own appearance to the server ONCE per identity, and
// fetches a peer's when it sees a peer wearing something it does not have.
// That replaces a peer-to-peer UDP fanout which cost one full transfer per
// peer and, worse, transmitted on the SENDER's schedule - reliably before a
// still-loading joiner could accept it.
//
// ALL HTTP HAPPENS ON THIS MODULE'S OWN THREAD. The networking core runs on
// a fixed tick and must never block on a socket read; a server that has gone
// away would otherwise stall the whole session rather than just delaying an
// appearance. The core interacts only through small, mutex-guarded handoffs
// that never block.
//
// This is additive: the existing UDP transfer stays exactly as it is, and a
// server without the store simply answers 404 (or nothing) and the peer path
// still works. Nothing here is required for a session to function.

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace skate3::appearance_service {

using Blob = std::shared_ptr<const std::vector<std::uint8_t>>;

// One appearance fetched from the server, ready for the core to install.
struct Fetched {
  std::uint32_t role = 0;
  std::uint64_t identity = 0;
  Blob bytes;
};

// Points the service at a server's admin HTTP endpoint - the same one the
// resource sync already uses, learned from the relay's register ack. Safe to
// call again on reconnect; a different endpoint resets everything in flight.
void Configure(const std::string& host, std::uint16_t port,
               std::uint32_t local_role);

// Stops the worker and forgets all state. Safe to call when not configured.
void Shutdown();

// Offers this client's own appearance for upload. Cheap to call every tick:
// an identity already uploaded (or in flight) is ignored, so the upload
// happens once per appearance, not once per tick. Passing identity 0 simply
// means "nothing to publish yet".
void PublishLocal(std::uint64_t identity, const Blob& bytes);

// Tells the service which peers this client currently has, and which
// appearance identity it already holds for each (0 meaning none yet).
//
// This is what scopes fetching to peers that actually matter: the caller's
// peer list is already interest-managed by the relay, so a peer beyond the
// configured radius is simply not in it and its appearance is never
// downloaded. A peer coming into range appears here and is fetched then.
void SetPeerInterest(
    const std::vector<std::pair<std::uint32_t, std::uint64_t>>& peers);

// That peer's display name as the SERVER reports it, or empty if the server
// has not named them yet. The server owns identity here - including the
// "(2)" suffix it appends when two connections present the same name - and
// it re-sends the answer whenever it changes, which is why this is preferred
// over the one-shot "skate3:playerNamed" script event: a client that was not
// listening at that exact moment used to show a role number forever.
std::string PeerName(std::uint32_t role);

// Removes and returns everything downloaded since the last call. Called from
// the networking tick, which installs each one.
std::vector<Fetched> TakeFetched();

// Whether the configured server has answered an appearance request at all.
// False means "no store there" - the peer-to-peer path is carrying
// appearances - and is not an error.
bool ServerHasStore();

}  // namespace skate3::appearance_service
