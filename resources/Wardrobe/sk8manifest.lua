name = "Wardrobe"

-- Reading what a skater can wear, and what they are wearing.
--
-- READ ONLY. Retail's own clothing menu decides what to SHOW from career
-- progress; this lists everything createacharacter.big actually contains, so
-- a locked shirt is just an id like any other. Applying a piece is a
-- separate problem (retail has to rebuild the character) and is not here
-- yet - see src/skate3_clothing.h.
--
-- The ids this lists are MODEL ids - the same ones a worn piece names as its
-- model. A recipe piece also carries an ASSET id, which is a different hash
-- space and is NOT in this catalogue (measured: 12/12 model ids matched, 0/12
-- asset ids). So these are the ids a setter would write.
--
-- Console commands:
--   clothing                  categories and how many items each has
--   clothing <category>       ids in that category
--   clothing <category> <f>   ids matching a substring
--   outfit                    what the local skater is wearing right now
--   wear <category> <n|0x id> override a category's model
--   wearlist                  active overrides
--   wearclear [category]      drop one override, or all of them
--   festate                   id of the last front-end screen shown
--   editskater [seq]          walk to retail's Edit Skater screen
--   padinput <tokens>         replay pad presses, e.g. start,down,down,a
client_scripts = { "client.lua" }
