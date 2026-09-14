#pragma once

// Client half of the server-held prop store (skate3_prop_store.h).
//
// The client tells this module where the player is; the module asks the
// server what props are near that point and hands back the ones this client
// has not spawned yet. Placing a prop goes the same way: it is POSTed to the
// server, which assigns the id and streams it back like any other, so there
// is exactly one way a shared prop comes into existence.
//
// ALL HTTP HAPPENS ON THIS MODULE'S OWN THREAD, for the same reason the
// appearance service has one: the frame loop must never block on a socket,
// and a server that has gone away should delay a prop rather than stall the
// game. The frame loop only touches small mutex-guarded handoffs.
//
// This is deliberately NOT a Lua resource. Which geometry a client loads is
// engine work: it runs against the player's position every second, it has to
// keep working while scripts are restarted, and a broken game mode must not
// be able to leave the world half-loaded. Lua decides the rules of a game;
// this decides what exists.

#include <cstdint>
#include <string>
#include <vector>

namespace skate3::prop_service {

// One prop the server says is nearby and this client has not spawned.
struct Spawn {
  std::uint32_t id = 0;
  std::string path;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float lod = 300.0f;
};

// Points the service at a server's admin HTTP endpoint - the same one
// resource sync and the appearance store already use, learned from the
// relay's register ack. Safe to call again on reconnect.
void Configure(const std::string& host, std::uint16_t port);

// Stops the worker and forgets every streamed prop. Safe when unconfigured.
void Shutdown();

// The point to stream around, pushed from the frame loop once it knows the
// player's position. Until this is called nothing is requested: asking about
// (0,0,0) would stream in whatever happens to sit at the world origin.
void SetLocalPosition(float x, float y, float z);

// Asks the server to create a prop for everyone. Returns immediately; the
// prop arrives back through the normal stream like any other.
void Place(const std::string& path, float x, float y, float z, float lod);

// Removes and returns props to spawn. Called from the frame loop, which
// spawns each one. A prop is handed out ONCE - the service remembers what it
// has given, so a caller that drops one loses it until reconnect.
std::vector<Spawn> TakeSpawns();

// Whether the configured server answered a prop request at all. False means
// "no prop store there", which is not an error - an older server simply has
// no props to stream.
bool ServerHasStore();

// Every synced prop this client has ever heard about (spawned already or
// not), by id - unlike TakeSpawns this does not drain anything, so it is
// safe to call from anywhere, any number of times, without stealing a prop
// the frame loop still needs to spawn. Order is unspecified.
std::vector<Spawn> ListKnownProps();

}  // namespace skate3::prop_service
