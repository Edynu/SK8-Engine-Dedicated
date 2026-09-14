#pragma once

#include <iosfwd>

namespace skate3::owned_world_boundary {

// True when the custom-world runtime owns the active scene and retail
// dynamic-world actors must not be created behind it.
bool SuppressingRetailWorldActors();

// Narrower gates, true when the owned-world strip is on OR when the
// standalone retail-map cvar for that category is set. Split because
// suppressing ambient pedestrians is cheap while suppressing scene DMOs
// takes ordinary world props with it.
bool SuppressingRetailPedestrians();
bool SuppressingRetailSceneObjects();

// Appends compact machine-readable counters to the harness STATUS response.
void AppendTelemetry(std::ostream& out);

}  // namespace skate3::owned_world_boundary
