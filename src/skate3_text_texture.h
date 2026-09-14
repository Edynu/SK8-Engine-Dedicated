#pragma once

// Rasterises a string into an RGBA bitmap, so text can be drawn as a textured
// billboard by the native renderer.
//
// WHY NOT RETAIL'S FONT. Sk8::Render::cFont::DrawstringLocal (sub_82808388) is
// the glyph emitter behind the game's own trick names and challenge prompts,
// and reusing it would match retail exactly. It needs three things we do not
// have: the cFont object pointer, the argument layout, and - the real risk -
// permission to run outside retail's own 2D phase bracket, which its own hook
// comment suggests it is picky about. That is a live-probing job. This is the
// path that works today and does not depend on any of it.
//
// WHY NOT A GLYPH ATLAS. An atlas is the right answer for lots of changing
// text. Nameplates are the opposite: a handful of strings that change when
// somebody joins or renames, and then never again. Rasterising the whole
// string once and drawing ONE quad is less code, fewer draws, and lets the
// shadow be baked in rather than drawn as a second pass.
//
// WHY GDI. It is already on every machine this runs on, so it adds no
// dependency and no font asset to ship. The project is Windows-only in
// practice; on anything else this returns an empty bitmap and callers draw
// nothing rather than failing.

#include <cstdint>
#include <string>
#include <vector>

namespace skate3::text_texture {

struct Bitmap {
  // RGBA8, row-major, top-down. Empty when rasterisation failed or the string
  // was empty - callers must handle that rather than assume a texture.
  std::vector<std::uint8_t> pixels;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

// Rasterises `utf8` at roughly `pixel_height` pixels tall, white with a soft
// dark outline baked into the alpha so it stays readable over both bright sky
// and dark geometry - the same reason the old ImGui nameplates drew a shadow
// pass, done once at rasterisation instead of every frame.
//
// Cached: repeated calls with the same text and height return the same bitmap
// without touching GDI again, which is what makes it reasonable to call this
// per player per frame.
[[nodiscard]] const Bitmap& Rasterise(const std::string& utf8,
                                      std::uint32_t pixel_height);

// Drops cached bitmaps not asked for since the previous sweep. Called
// occasionally so a session that has seen many names does not hold them all.
void SweepUnused();

}  // namespace skate3::text_texture
