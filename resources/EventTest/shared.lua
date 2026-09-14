-- Runs on BOTH sides, before the side-specific script. Proves shared_scripts
-- load first and land in the same Lua state.
SKATE_SHARED_VERSION = "0.1"

function DescribeSide(side)
    return string.format('%s (shared v%s)', side, SKATE_SHARED_VERSION)
end
