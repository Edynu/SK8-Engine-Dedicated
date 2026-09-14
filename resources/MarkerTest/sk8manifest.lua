name = "MarkerTest"

-- Exercises the native in-world markers.
--
-- These are NOT the old Markers resource's approach. There, markers were a
-- WorldToScreen call per frame feeding an HTML overlay; here the marker is a
-- real thing in the world - C++ owns its position, proximity and activation,
-- and the native renderer draws it as a depth-tested billboard. Retail's own
-- marker could not be reused: it is a singleton with no text, no type and no
-- settable position (see src/skate3_retail_markers.h).
--
--   markerhere     drop a marker where you are standing
--   markerring     five markers in a ring around you, to check nearest-wins
--   markerclear    remove them all
--   markerwatch    print the active marker and hold progress as they change
--
-- Stand on one and hold D-pad up to activate it.
client_scripts = { "client.lua" }
