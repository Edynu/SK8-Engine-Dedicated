#include "skate3_text_texture.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace skate3::text_texture {

namespace {

struct Entry {
  Bitmap bitmap;
  // Cleared by SweepUnused, set by every Rasterise hit, so a name that stopped
  // being drawn eventually goes away while one still on screen never does.
  bool used_since_sweep = true;
};

std::mutex g_mutex;
std::unordered_map<std::string, Entry> g_cache;
const Bitmap g_empty;

[[nodiscard]] std::string CacheKey(const std::string& utf8,
                                   std::uint32_t pixel_height) {
  return std::to_string(pixel_height) + ":" + utf8;
}

#if defined(_WIN32)

// The outline is produced by dilating the glyph coverage, not by drawing the
// text again offset - an offset shadow only darkens one side, and a nameplate
// is seen against every possible background.
constexpr int kOutlineRadius = 2;
// Room for the outline on every side.
constexpr int kPadding = kOutlineRadius + 1;

[[nodiscard]] std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) {
    return {};
  }
  const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                         static_cast<int>(utf8.size()), nullptr,
                                         0);
  if (needed <= 0) {
    return {};
  }
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                      wide.data(), needed);
  return wide;
}

Bitmap RasteriseWin32(const std::string& utf8, std::uint32_t pixel_height) {
  Bitmap out;
  const std::wstring wide = Widen(utf8);
  if (wide.empty() || pixel_height == 0) {
    return out;
  }

  HDC screen = GetDC(nullptr);
  if (screen == nullptr) {
    return out;
  }
  HDC dc = CreateCompatibleDC(screen);
  ReleaseDC(nullptr, screen);
  if (dc == nullptr) {
    return out;
  }

  // A weight and face chosen to read at small sizes over a moving background;
  // ClearType is deliberately off (ANTIALIASED_QUALITY) because subpixel
  // output is only correct against the background it was rendered for, and
  // this is composited over arbitrary scene pixels.
  HFONT font = CreateFontW(
      -static_cast<int>(pixel_height), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
      FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
      ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
  if (font == nullptr) {
    DeleteDC(dc);
    return out;
  }
  HGDIOBJ old_font = SelectObject(dc, font);

  SIZE extent{};
  GetTextExtentPoint32W(dc, wide.c_str(), static_cast<int>(wide.size()),
                        &extent);
  const int width = extent.cx + kPadding * 2;
  const int height = extent.cy + kPadding * 2;
  if (width <= 0 || height <= 0) {
    SelectObject(dc, old_font);
    DeleteObject(font);
    DeleteDC(dc);
    return out;
  }

  // Top-down 32-bit DIB so the rows come out in the order a texture upload
  // wants, with no flip.
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP dib =
      CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (dib == nullptr || bits == nullptr) {
    SelectObject(dc, old_font);
    DeleteObject(font);
    DeleteDC(dc);
    return out;
  }
  HGDIOBJ old_bitmap = SelectObject(dc, dib);

  // GDI has no alpha, so the glyphs are drawn WHITE ON BLACK and the green
  // channel is read back as coverage. That is the whole trick: it turns a
  // colour-only API into an alpha mask.
  RECT rect{0, 0, width, height};
  FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(255, 255, 255));
  TextOutW(dc, kPadding, kPadding, wide.c_str(), static_cast<int>(wide.size()));
  GdiFlush();

  const auto* src = static_cast<const std::uint8_t*>(bits);
  const std::size_t pixel_count =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);

  std::vector<std::uint8_t> coverage(pixel_count, 0);
  for (std::size_t i = 0; i < pixel_count; ++i) {
    coverage[i] = src[i * 4 + 1];  // BGRA; green == the white we drew
  }

  // Dilate coverage into the outline mask: an outline texel is one near any
  // glyph texel. Separable would be faster; at nameplate sizes, run once per
  // unique name, this is not worth the extra code.
  std::vector<std::uint8_t> outline(pixel_count, 0);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      std::uint8_t best = 0;
      for (int oy = -kOutlineRadius; oy <= kOutlineRadius; ++oy) {
        const int sy = y + oy;
        if (sy < 0 || sy >= height) continue;
        for (int ox = -kOutlineRadius; ox <= kOutlineRadius; ++ox) {
          const int sx = x + ox;
          if (sx < 0 || sx >= width) continue;
          best = std::max(
              best, coverage[static_cast<std::size_t>(sy) * width + sx]);
        }
      }
      outline[static_cast<std::size_t>(y) * width + x] = best;
    }
  }

  out.width = static_cast<std::uint32_t>(width);
  out.height = static_cast<std::uint32_t>(height);
  out.pixels.resize(pixel_count * 4);
  for (std::size_t i = 0; i < pixel_count; ++i) {
    const float glyph = coverage[i] / 255.0f;
    const float halo = outline[i] / 255.0f;
    // White glyph over a black halo, both premultiplied into the colour by
    // the glyph's own coverage so the edge fades to the outline rather than
    // to grey. Alpha is the union of the two.
    const float alpha = std::max(glyph, halo);
    const std::uint8_t luminance =
        static_cast<std::uint8_t>(std::clamp(glyph * 255.0f, 0.0f, 255.0f));
    out.pixels[i * 4 + 0] = luminance;
    out.pixels[i * 4 + 1] = luminance;
    out.pixels[i * 4 + 2] = luminance;
    out.pixels[i * 4 + 3] =
        static_cast<std::uint8_t>(std::clamp(alpha * 255.0f, 0.0f, 255.0f));
  }

  SelectObject(dc, old_bitmap);
  DeleteObject(dib);
  SelectObject(dc, old_font);
  DeleteObject(font);
  DeleteDC(dc);
  return out;
}

#endif  // _WIN32

}  // namespace

const Bitmap& Rasterise(const std::string& utf8, std::uint32_t pixel_height) {
  if (utf8.empty()) {
    return g_empty;
  }
  const std::string key = CacheKey(utf8, pixel_height);
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_cache.find(key);
  if (it != g_cache.end()) {
    it->second.used_since_sweep = true;
    return it->second.bitmap;
  }
#if defined(_WIN32)
  Entry entry;
  entry.bitmap = RasteriseWin32(utf8, pixel_height);
  return g_cache.emplace(key, std::move(entry)).first->second.bitmap;
#else
  // No rasteriser off Windows: callers see an empty bitmap and draw nothing,
  // which degrades to "no nameplates" rather than to a crash.
  return g_empty;
#endif
}

void SweepUnused() {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (auto it = g_cache.begin(); it != g_cache.end();) {
    if (it->second.used_since_sweep) {
      it->second.used_since_sweep = false;
      ++it;
    } else {
      it = g_cache.erase(it);
    }
  }
}

}  // namespace skate3::text_texture
