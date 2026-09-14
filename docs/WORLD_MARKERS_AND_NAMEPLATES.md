# World markers and nameplates

Two features that ended up sharing one renderer: in-world markers a script
creates, and the floating names over other players. Both are world-space
billboards drawn by the native renderer, and neither uses ImGui or NUI.

## Why not the obvious approaches

**Retail's own session marker** was the first candidate and cannot back this.
Reading the bindings' recompiled bodies (`src/skate3_retail_markers.h` has the
detail) shows it is a **singleton**: one marker, no text, no type, and
`SetSessionMarker` takes **no position** - it marks wherever the player is
standing. The whole of "is there a marker" is one byte at `+12` off the block at
`0x83067060 + 4`. A `CreateMarker`/`DestroyMarker` API returning handles for many
cannot be built on that.

**A Lua resource drawing its own icons** is what `resources/Markers` still does:
`WorldToScreen` per frame feeding an HTML overlay. It works, but it pays a Lua
tick plus a NUI message every frame, and the marker is not in the scene, so it
cannot be occluded by the world.

**ImGui** is what the nameplates used to use. It draws fine, but it puts ImGui in
the shipped rendering path for a permanent gameplay element, and a name drawn
into the foreground draw list is a flat screen sprite rather than a thing in the
world.

## What it is now

`skate3_world_markers.{h,cpp}` owns marker state and activation and knows
nothing about Lua or the GPU: `Update()` runs on the app thread once a frame,
`Snapshot()` is pulled by the render thread, and activation is published through
a callback the script layer installs. `skate3_player_nameplates.{h,cpp}` does the
same for names.

Drawing is `src/native/shaders/marker.hlsl` plus a pass in
`skate3_native_scene_gpu.cpp`, modelled on the in-world spline pipeline (which is
retail's own marker-beam replay, so the precedent is exact rather than
approximate). It reuses the existing `ui_ring` vertex ring buffer and runs inside
the scene pass, so it gets MSAA and the HDR encode for free.

### Three pipelines, one shader

| Pipeline | Depth | Used by |
|---|---|---|
| `pso_marker_depth` | test on, no write | Marker rings - occluded by the world like any world object |
| `pso_marker_overlay` | **off** | Nameplates - a name must never be swallowed by geometry |
| `pso_marker_text` | off, textured | Text, and markers using retail art |

The depth split is the point. Always-on-top here still beats the old ImGui
overlay, because the quad stays world-anchored and perspective-scaled - the name
shrinks with distance and sits correctly in the scene.

### Billboarding

Cylindrical: the quad rotates around Y only, so markers stand upright like a sign
instead of tipping to meet an overhead camera. Computed CPU-side from
`CameraPosition`, which is the only camera state exposed - no view matrix needed.

`BillboardRight()` and `EmitTextBillboard()` are shared by both features
deliberately. The first version had two copies of the basis and they disagreed:
`right` had the wrong sign, which is **invisible on a symmetric ring** and
glaringly obvious the moment text was drawn through it. One place to get it
right.

Orientation is pinned by two facts rather than trial and error: `WorldToScreen`
documents that **NDC +y is the top**, so world +Y maps to the top of the quad;
and the text rasteriser returns a top-down bitmap, so texture row 0 is `v=0` and
the quad's top corners sample `v=0`.

## Text

No glyph atlas and no retail font. Each distinct string is rasterised **once**
into an RGBA bitmap (`skate3_text_texture.{h,cpp}`, GDI) and drawn as **one**
quad.

That suits the workload: nameplates are a handful of strings that change when
somebody joins or renames and then never again - the opposite of what an atlas is
good at. It also lets the outline be baked in at rasterisation instead of drawn
as a second pass every frame.

Two details that matter:

- GDI has no alpha, so glyphs are drawn **white on black** and the green channel
  is read back as coverage. That is what turns a colour-only API into an alpha
  mask.
- The outline is a **dilation of the coverage**, not an offset shadow. An offset
  shadow only darkens one side, and a nameplate is seen against every possible
  background. ClearType is off deliberately: subpixel output is only correct
  against the background it was rendered for, and this composites over arbitrary
  scene pixels.

Rasterised at a fixed pixel height and scaled in **world** space, so the cache
holds one bitmap per string rather than one per string per distance - otherwise
walking toward someone would upload a new texture every frame.

`Sk8::Render::cFont::DrawstringLocal` (`sub_82808388`, already bracketed in
`skate3_native_render.cpp`) remains the route to exact retail parity if it is
ever wanted. It needs the `cFont` object, the argument layout, and permission to
run outside retail's own 2D phase bracket - a live-probing job, not a static one.

## Icons

Retail's own marker art is **extracted offline and embedded in the exe** -
`tools/extract_rw4_icons.py` into `src/generated/skate3_marker_icons.h`, 26
icons at 32x32, named by the Flash character id the asset's own TOC carries
(`challenge_1`, `challenge_4`, ...). See `docs/NATIVE_DISCOVERY.md` for the
format chain and the credit for the RW4 layout.

`GetMarkerTextures()` returns the list, so the names live in one place and
cannot drift from what was actually embedded.

Capturing it at runtime from the renderer's texture store was tried first and
does not work: front-end art is unloaded with its screen and retail challenge
markers do not exist on most maps, so the texture is absent exactly when a game
mode wants it. The cvar for that experiment
(`skate3_marker_icon_key`, with `skate3_native_render_scene_ui_tex_log` to find
a key) still exists as an override, but embedding is the supported path.

A marker with no `texture`, or an unknown one, draws the procedural ring -
a shape computed from the quad's UV with `fwidth` antialiasing, so it stays sharp
at any distance and needs no asset at all. `color` tints either, so one icon can
serve several states.

## Script API

See `docs/lua_natives.json` (category `Markers`) for the full surface.
`radius` and `size` are deliberately separate: `radius` is the activation
distance, `size` is the drawn size, and a large icon triggered only from
underfoot - or a small one with a generous catch - are both things a game mode
wants.

`onUse` runs as a scheduler thread, so it **may call `Skate.Wait`**. That is the
reason markers are worth having as natives at all: "activate, wait, then do the
next thing" is the shape every game mode needs, and it would not be writable if
the callback had to return immediately.

Activation is a **hold** of D-pad up, not a tap, because D-pad up is not a
dedicated button - a tap would fire markers while the player was doing something
else. Releasing re-arms it, so holding the button across a completed activation
cannot fire a second one, and walking between markers mid-hold restarts the hold
rather than carrying progress to a marker the player only just reached.

`resources/MarkerTest` exercises all of it: `markericons` drops one marker per
embedded texture in a labelled ring, which is the only practical way to see which
icon is which.

## Known gaps

- `world_markers::DestroyAllForResource` exists but is **not wired**: there is no
  C++ resource-stop hook, so a restarted resource leaves its markers standing.
- `world_markers::Snapshot()` allocates a vector per frame on the render thread.
  Fine at a handful of markers; worth a reusable buffer if this grows.
- Marker labels always draw on top (the depth-disabled text pipeline) even when
  the ring itself is occluded. Deliberate - a half-swallowed label is unreadable -
  but it does mean a label can be visible through a wall.
