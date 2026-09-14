#include "skate3_cef_console.h"

#if defined(SKATE3_ENABLE_CEF) && SKATE3_ENABLE_CEF

#include "skate3_cef_browser.h"

#include <string>

namespace skate3::cef_console {

namespace {

// Startup default, used only until the first real Resize() call (the
// render pass calls it every frame once it knows guest_output's size, so
// this is visible for at most a few frames right after Initialize()).
constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 400;

// The console is a text surface that only repaints when a log line arrives
// or a key is typed; 30 is plenty and keeps CEF off the CPU otherwise.
constexpr int kFrameRate = 30;

CefRefPtr<cef_internal::Surface> g_surface;

}  // namespace

void Initialize(int admin_http_port, const std::string& admin_token) {
  if (g_surface) {
    return;
  }
  // Port 0 means the admin server bound nothing (every port in the scan
  // range was taken). There is no origin to load, and pointing a browser at
  // :0 would just fail opaquely later.
  if (admin_http_port <= 0) {
    return;
  }
  if (!cef_internal::EnsureRuntime()) {
    return;
  }
  g_surface = new cef_internal::Surface("console", kDefaultWidth,
                                        kDefaultHeight, kFrameRate);
  g_surface->CreateBrowser("http://127.0.0.1:" +
                           std::to_string(admin_http_port) +
                           "/console.html?token=" + admin_token);
}

void Shutdown() {
  if (!g_surface) {
    return;
  }
  g_surface->CloseBrowser();
  g_surface = nullptr;
  cef_internal::ReleaseRuntime();
}

const uint8_t* LatestFrame(uint32_t& width, uint32_t& height,
                           uint64_t& serial) {
  if (!g_surface) {
    width = 0;
    height = 0;
    serial = 0;
    return nullptr;
  }
  return g_surface->LatestFrame(width, height, serial);
}

// The public ModifierFlags values are passed straight through to CEF, so a
// silent divergence would break selection/clipboard in ways that look like
// a logic bug rather than a constant mismatch.
static_assert(static_cast<uint32_t>(kModShift) == EVENTFLAG_SHIFT_DOWN);
static_assert(static_cast<uint32_t>(kModControl) == EVENTFLAG_CONTROL_DOWN);
static_assert(static_cast<uint32_t>(kModAlt) == EVENTFLAG_ALT_DOWN);
static_assert(static_cast<uint32_t>(kModLeftMouseButton) ==
              EVENTFLAG_LEFT_MOUSE_BUTTON);
static_assert(static_cast<uint32_t>(kModMiddleMouseButton) ==
              EVENTFLAG_MIDDLE_MOUSE_BUTTON);
static_assert(static_cast<uint32_t>(kModRightMouseButton) ==
              EVENTFLAG_RIGHT_MOUSE_BUTTON);

void SendMouseMove(int x, int y, uint32_t modifiers) {
  if (g_surface) {
    g_surface->SendMouseMove(x, y, modifiers);
  }
}

void SendMouseButton(int x, int y, int button, bool mouse_up,
                     uint32_t modifiers, int click_count) {
  if (g_surface) {
    g_surface->SendMouseButton(x, y, button, mouse_up, modifiers, click_count);
  }
}

void SetFocus(bool focus) {
  if (g_surface) {
    g_surface->SetFocus(focus);
  }
}

void Resize(int width, int height) {
  if (g_surface) {
    g_surface->Resize(width, height);
  }
}

void CurrentSize(int& out_width, int& out_height) {
  if (!g_surface) {
    out_width = kDefaultWidth;
    out_height = kDefaultHeight;
    return;
  }
  g_surface->CurrentSize(out_width, out_height);
}

void SendMouseWheel(int x, int y, int delta_x, int delta_y,
                    uint32_t modifiers) {
  if (g_surface) {
    g_surface->SendMouseWheel(x, y, delta_x, delta_y, modifiers);
  }
}

void SendKeyEvent(int win_vk, bool is_down, bool is_char, uint16_t utf16_char,
                  uint32_t modifiers) {
  if (g_surface) {
    g_surface->SendKeyEvent(win_vk, is_down, is_char, utf16_char, modifiers);
  }
}

}  // namespace skate3::cef_console

#else  // !SKATE3_ENABLE_CEF - harmless no-op stubs, see the header comment.

namespace skate3::cef_console {

void Initialize(int) {}
void Shutdown() {}
const uint8_t* LatestFrame(uint32_t& width, uint32_t& height,
                           uint64_t& serial) {
  width = 0;
  height = 0;
  serial = 0;
  return nullptr;
}
void Resize(int, int) {}
void CurrentSize(int& out_width, int& out_height) {
  out_width = 0;
  out_height = 0;
}
void SendMouseMove(int, int, uint32_t) {}
void SendMouseButton(int, int, int, bool, uint32_t, int) {}
void SetFocus(bool) {}
void SendMouseWheel(int, int, int, int, uint32_t) {}
void SendKeyEvent(int, bool, bool, uint16_t, uint32_t) {}

}  // namespace skate3::cef_console

#endif  // SKATE3_ENABLE_CEF
