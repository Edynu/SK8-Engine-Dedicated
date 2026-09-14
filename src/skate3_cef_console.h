#pragma once

// CEF (Chromium Embedded Framework) integration for the in-game dev
// console. Client-only. When built without SKATE3_ENABLE_CEF, every
// function here is a harmless no-op (LatestFrame always reports no frame),
// so callers (skate3_dev_console_dialog.cpp, the native render pass in
// skate3_native_scene_gpu.cpp) never need their own #if guards.

#include "skate3_cef_input.h"

#include <cstdint>
#include <string>

namespace skate3::cef_console {

// Starts CEF (off-screen rendering, its own background UI thread) and
// navigates a single fixed browser instance at the local admin HTTP
// server's console page. Call once, after the admin HTTP server (whose
// port this points at) is already listening.
//
// `admin_token` is the client's per-run secret for the admin routes, passed
// in the page's query string. It is what separates THIS page - which is
// allowed to run console commands - from a resource's NUI page, which is
// server-supplied HTML served from the same 127.0.0.1 origin and must not
// be. Separate browser instances cannot read each other's URLs, so a NUI
// page has no way to learn it. See AdminHttpServer::AdminAccess.
void Initialize(int admin_http_port, const std::string& admin_token);
void Shutdown();

// CEF's off-screen-rendered browser has no real native window, so nothing
// tells it when it should be considered focused - the app must say so
// explicitly. Without this, Chromium's default "focus the clicked form
// control" behavior does not run (the page still receives raw mouse
// events, but a click on <input> does not move DOM focus into it), while
// keyboard navigation like Tab still works because it does not depend on
// this flag. Call true when the console becomes visible/clicked, false
// when it is hidden.
void SetFocus(bool focus);

// The most recent OSR paint, BGRA32, width*height*4 bytes - or nullptr if
// no frame has arrived yet. Safe to call from the render thread; CEF paints
// asynchronously on its own thread, so this is backed by a mutex-guarded
// double buffer, not a direct view into CEF's own memory.
//
// `serial` counts paints, so the render pass can skip re-uploading a frame
// it already has in its texture.
const uint8_t* LatestFrame(uint32_t& width, uint32_t& height,
                           uint64_t& serial);

// Modifier bits for the input forwarding below, re-exported from the
// shared CEF input vocabulary so existing call sites keep spelling them
// cef_console::kModShift. See skate3_cef_input.h for what they mean and
// why their values must match CEF's own.
using cef_input::ModifierFlags;
using cef_input::kModNone;
using cef_input::kModShift;
using cef_input::kModControl;
using cef_input::kModAlt;
using cef_input::kModLeftMouseButton;
using cef_input::kModMiddleMouseButton;
using cef_input::kModRightMouseButton;

// Input forwarding - call only while the console is visible/focused.
// Coordinates are in the browser's own logical pixel space (see
// skate3_dev_console_dialog.cpp for the screen-rect -> browser-space
// mapping).
void SendMouseMove(int x, int y, uint32_t modifiers);
// button: 0 = left, 1 = middle, 2 = right. click_count 2 = double-click
// (Blink's own word-select gesture), 3 = triple (line select).
void SendMouseButton(int x, int y, int button, bool mouse_up,
                     uint32_t modifiers, int click_count);
void SendMouseWheel(int x, int y, int delta_x, int delta_y, uint32_t modifiers);
// is_char: true for a text/character event (utf16_char is the codepoint);
// false for a raw key down/up (win_vk is a Windows virtual-key code).
void SendKeyEvent(int win_vk, bool is_down, bool is_char, uint16_t utf16_char,
                  uint32_t modifiers);

// Fixed on-screen docking position (flush against the top-left corner,
// FiveM-style - drawn 1:1, no scaling). The SIZE is not fixed: the console
// spans the full guest-output width and a fraction of its height, updated
// every frame via Resize() below, so it fills the screen at whatever
// resolution the game is running at rather than a hardcoded pixel size that
// only looked right at one resolution.
constexpr int kConsoleScreenX = 0;
constexpr int kConsoleScreenY = 0;
constexpr float kConsoleHeightFraction = 0.36f;

// Changes the browser's logical view size, triggering a re-layout and
// repaint at the new dimensions (CEF's WasResized(), called if the size
// actually changed - a no-op otherwise). Safe to call every frame; the size
// actually driving this is guest_output's, which only changes on a
// resolution/window change. Before the first call (or before the SDK is
// enabled), the browser uses a small built-in default so it has SOME size
// to render at during startup.
void Resize(int width, int height);

// The console's current logical size, matching what GetViewRect last
// reported and OnPaint's buffer is sized to. (0, 0) is never returned - see
// Resize's own comment about the startup default.
void CurrentSize(int& out_width, int& out_height);

}  // namespace skate3::cef_console
