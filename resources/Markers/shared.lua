Markers = Markers or {}

-- How close the player must be for a marker to show its name and prompt.
-- Retail's own sign-up markers become interactive at roughly this range.
Markers.ACTIVATE_RADIUS = 6.0

-- The icon stays visible further out than the prompt, so a marker reads as
-- a destination before it reads as something you can act on.
Markers.VISIBLE_RADIUS = 120.0

-- How long D-pad up must be held. Long enough that it cannot fire from the
-- same press that was steering a menu, short enough not to feel stuck.
Markers.HOLD_MS = 500

-- Marker types. Only the presentation differs; the interaction is the same.
Markers.TYPES = {
    signup = { label = 'Sign Up', glyph = '\u{25B2}' },
    info   = { label = 'View',    glyph = '\u{2139}' },
    start  = { label = 'Start',   glyph = '\u{25B6}' },
}

-- How far the radar reaches. Larger than the marker draw distance on
-- purpose: the radar's job is telling you a marker exists somewhere over
-- there, which is useful well before the icon itself is worth drawing.
Markers.RADAR_RANGE = 250.0
