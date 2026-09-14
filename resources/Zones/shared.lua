-- Zone shapes and containment. Pure maths, no engine calls, so the same
-- file works on the server (which knows every player's position through
-- GetSkaters) as on the client.
--
-- AXIS NOTE, because getting this wrong is silent and confusing: in this
-- engine Y is VERTICAL. The horizontal plane is X/Z. Polygons are therefore
-- described as a list of {x, z} points with a separate height range, not as
-- the {x, y} you would write in a top-down engine.

Zones = Zones or {}

local function newZone(name, kind)
    return {
        name = name,
        kind = kind,
        enterHandlers = {},
        exitHandlers = {},
        -- Whether the local player was inside as of the last sample. The
        -- polling loop owns this; nothing else should write it.
        inside = false,

        onEnter = function(self, fn)
            self.enterHandlers[#self.enterHandlers + 1] = fn
            return self
        end,
        onExit = function(self, fn)
            self.exitHandlers[#self.exitHandlers + 1] = fn
            return self
        end,
    }
end

-- A sphere, optionally clipped to a height band. The band matters more than
-- it looks: without it a zone on the ground also catches someone on a roof
-- directly above it.
function Zones.Sphere(name, x, y, z, radius, opts)
    local zone = newZone(name, 'sphere')
    opts = opts or {}
    zone.x, zone.y, zone.z = x, y, z
    zone.radius = radius
    zone.minY = opts.minY
    zone.maxY = opts.maxY
    zone.contains = function(self, px, py, pz)
        if self.minY and py < self.minY then return false end
        if self.maxY and py > self.maxY then return false end
        local dx, dz = px - self.x, pz - self.z
        local dy = (self.minY or self.maxY) and 0.0 or (py - self.y)
        return (dx * dx + dy * dy + dz * dz) <= (self.radius * self.radius)
    end
    zone.moveTo = function(self, nx, ny, nz)
        self.x, self.y, self.z = nx, ny, nz
        return self
    end
    return zone
end

-- An axis-aligned box, given as a centre and full extents.
function Zones.Box(name, x, y, z, sizeX, sizeY, sizeZ)
    local zone = newZone(name, 'box')
    zone.x, zone.y, zone.z = x, y, z
    zone.hx, zone.hy, zone.hz = sizeX * 0.5, sizeY * 0.5, sizeZ * 0.5
    zone.contains = function(self, px, py, pz)
        return math.abs(px - self.x) <= self.hx
           and math.abs(py - self.y) <= self.hy
           and math.abs(pz - self.z) <= self.hz
    end
    zone.moveTo = function(self, nx, ny, nz)
        self.x, self.y, self.z = nx, ny, nz
        return self
    end
    return zone
end

-- An arbitrary polygon in the X/Z plane, extruded between minY and maxY.
-- `points` is a list of {x, z} pairs in either winding order.
function Zones.Poly(name, points, minY, maxY)
    local zone = newZone(name, 'poly')
    zone.points = points
    zone.minY, zone.maxY = minY, maxY
    zone.contains = function(self, px, py, pz)
        if self.minY and py < self.minY then return false end
        if self.maxY and py > self.maxY then return false end
        -- Ray casting: count how many edges a ray from the point crosses.
        -- Odd means inside. Edges are treated as half-open on purpose so a
        -- point exactly on a shared vertex is counted once, not twice.
        local inside = false
        local count = #self.points
        local j = count
        for i = 1, count do
            local a, b = self.points[i], self.points[j]
            if (a[2] > pz) ~= (b[2] > pz) then
                local t = (pz - a[2]) / (b[2] - a[2])
                if px < a[1] + t * (b[1] - a[1]) then
                    inside = not inside
                end
            end
            j = i
        end
        return inside
    end
    return zone
end

-- Distance to a zone's centre on the horizontal plane. Used for "nearest
-- zone" prompts, where vertical distance would make a zone directly below
-- you look further away than one across the plaza.
function Zones.FlatDistance(zone, px, pz)
    if zone.x == nil then
        return math.huge  -- polygons have no single centre worth reporting.
    end
    local dx, dz = px - zone.x, pz - zone.z
    return math.sqrt(dx * dx + dz * dz)
end
