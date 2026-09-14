local mine = {}
local watching = false

local function remember(id)
  if id ~= nil then
    mine[#mine + 1] = id
  end
  return id
end

local function playerPos()
  local skater = GetPlayerSkater()
  return skater.x, skater.y, skater.z
end

-- Every retail icon baked into the exe. Asked for rather than hard-coded, so
-- this keeps working when the extracted set changes.
RegisterCommand('markertextures', function()
  local names = GetMarkerTextures()
  print(string.format('[markertest] %d embedded textures:', #names))
  for i, name in ipairs(names) do
    print(string.format('   %2d  %s', i, name))
  end
  print('[markertest] use one as texture = "<name>" in CreateMarker')
end)

RegisterCommand('markerhere', function()
  local x, y, z = playerPos()
  if x == nil then return end

  local id = remember(CreateMarker({
    x = x, y = y, z = z,

    -- radius is NOT the visual size: it is how close you have to stand for
    -- this marker to become the active one. Kept separate from `size` on
    -- purpose - a big icon you can only trigger from underfoot, or a small one
    -- with a generous catch, are both things a game mode wants.
    radius = 3.0,
    size = 1.5,

    -- Retail's own art, extracted offline and embedded (challenge_1 is the
    -- camera icon). Omit it and you get the built-in procedural ring; an
    -- unknown name also falls back to the ring.
    texture = 'challenge_1',

    -- Still applies to a texture: the icon is TINTED by it, so one icon can
    -- serve several states. 0xAARRGGBB - keep alpha at FF unless you want it
    -- faded. White leaves the art exactly as retail drew it.
    color = 0xFFFFFFFF,

    hold = 500,
    text = 'Test marker',

    -- Runs as a scheduler thread, so it may wait. That is the whole reason
    -- markers are natives: "activate, wait, then do the next thing" is the
    -- shape every game mode wants.
    onUse = function(used)
      print(string.format('[markertest] marker %d used', used))
      SetMarkerText(used, 'Used! reopening in 2s')
      Skate.Wait(2000)
      SetMarkerText(used, 'Test marker')
      print('[markertest] marker ' .. used .. ' ready again')
    end,
  }))

  if id == nil then
    print('[markertest] CreateMarker failed')
  else
    print(string.format('[markertest] marker %d at %.1f %.1f %.1f - stand on it and hold D-pad up',
                        id, x, y, z))
  end
end)

-- One marker per embedded texture, in a ring, so the whole icon set can be
-- eyeballed at once - which is the only way to tell which one is which.
RegisterCommand('markericons', function()
  local x, y, z = playerPos()
  if x == nil then return end

  local names = GetMarkerTextures()
  local count = #names
  if count == 0 then
    print('[markertest] no embedded textures')
    return
  end
  for i, name in ipairs(names) do
    local angle = (i - 1) * (2 * math.pi / count)
    remember(CreateMarker({
      x = x + math.cos(angle) * 8.0,
      y = y,
      z = z + math.sin(angle) * 8.0,
      radius = 2.0,
      size = 1.5,
      texture = name,
      color = 0xFFFFFFFF,
      hold = 400,
      text = name,
      onUse = function(used)
        print(string.format('[markertest] %s (id %d)', name, used))
      end,
    }))
  end
  print(string.format('[markertest] %d icons in a ring, each labelled', count))
end)

-- Overlapping radii: exactly one should ever be active, and it should be
-- whichever is NEAREST rather than whichever was created first.
RegisterCommand('markerring', function()
  local x, y, z = playerPos()
  if x == nil then return end

  for i = 1, 5 do
    local angle = (i - 1) * (2 * math.pi / 5)
    local label = 'Ring ' .. i
    remember(CreateMarker({
      x = x + math.cos(angle) * 4.0,
      y = y,
      z = z + math.sin(angle) * 4.0,
      radius = 3.0,
      size = 1.0,
      texture = 'challenge_1',
      color = 0xFFFFCC33,
      hold = 800,
      text = label,
      onUse = function(used)
        print(string.format('[markertest] %s (id %d) used', label, used))
      end,
    }))
  end
  print('[markertest] 5 overlapping markers - only the nearest should prompt')
end)

RegisterCommand('markerclear', function()
  local removed = 0
  for _, id in ipairs(mine) do
    if DestroyMarker(id) then
      removed = removed + 1
    end
  end
  mine = {}
  print(string.format('[markertest] removed %d marker(s)', removed))
end)

-- Prints on change only. A per-frame print would bury the transitions this is
-- meant to show: entering a marker, the hold climbing, and the release.
RegisterCommand('markerwatch', function()
  if watching then
    watching = false
    print('[markertest] watch off')
    return
  end
  watching = true
  print('[markertest] watch on - walk onto a marker and hold D-pad up')
  Skate.CreateThread(function()
    local last = ''
    while watching do
      local id, progress = GetActiveMarker()
      local line
      if id == nil then
        line = 'no active marker'
      else
        line = string.format('active=%d hold=%d%%', id, math.floor(progress * 100 + 0.5))
      end
      if line ~= last then
        print('[markertest] ' .. line)
        last = line
      end
      Skate.Wait(50)
    end
  end)
end)

print('[markertest] ready - markerhere, markericons, markertextures, markerring, markerclear, markerwatch')
