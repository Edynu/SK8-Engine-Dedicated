#pragma once

// INTERNAL to the CEF-backed overlays (skate3_cef_console.cpp,
// skate3_cef_nui.cpp). Unlike their public headers, this one includes real
// CEF headers, so it must only ever be included from a translation unit
// already guarded by SKATE3_ENABLE_CEF.
//
// Why this exists: CefInitialize/CefShutdown are process-global and may run
// exactly once, but the project now has TWO independent off-screen browsers
// - the F7 dev console and the NUI layer that hosts resources' ui_pages.
// Surface is the piece they have in common (an OSR browser plus its paint
// buffer and input plumbing); EnsureRuntime/ReleaseRuntime own the single
// process-wide CEF lifetime underneath both.

#if !defined(SKATE3_ENABLE_CEF) || !SKATE3_ENABLE_CEF
#error "skate3_cef_browser.h included in a build without CEF enabled"
#endif

// CEF's own headers pull in <windows.h> transitively (HWND et al.) without
// guarding against its unqualified min/max macros, which then collide with
// std::min/std::max used inside CEF's own headers (base/cef_ref_counted.h,
// internal/cef_types_wrappers.h) - matches this project's own per-file
// convention (see skate3_app_common.cpp) rather than a global define.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <include/cef_app.h>
#include <include/cef_client.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace skate3::cef_internal {

// Starts CEF if it is not running yet and takes a reference. Returns false
// if CefInitialize failed - callers must then behave as if CEF is absent
// (every Surface method is a safe no-op in that state, so a failed init
// degrades to "the overlay never appears" rather than a crash).
bool EnsureRuntime();
// Drops one reference; runs CefShutdown when the last one goes away. Every
// surface must have had CloseBrowser() called first.
void ReleaseRuntime();

// Runs `fn` on CEF's browser-process UI thread, immediately if already on
// it. CefBrowser/CefFrame methods are UI-thread-only, so every frame lookup
// and ExecuteJavaScript below goes through this.
void RunOnCefUiThread(std::function<void()> fn);

// One off-screen browser: its paint buffer, its logical size, and the input
// funnel into it. All methods are safe to call before the browser has
// actually been created (they no-op), which matters because CreateBrowser
// is asynchronous - the game's render thread starts asking for frames well
// before the first one exists.
class Surface : public CefClient,
                public CefRenderHandler,
                public CefLoadHandler,
                public CefDisplayHandler,
                public CefLifeSpanHandler {
 public:
  // `tag` only labels log output (so console vs. nui errors are
  // distinguishable). `frame_rate` is CEF's windowless_frame_rate: the
  // upper bound on OnPaint calls per second for this browser.
  Surface(const char* tag, int default_width, int default_height,
          int frame_rate);

  void CreateBrowser(const std::string& url);
  void CloseBrowser();

  // CefClient
  CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }

  // CefLifeSpanHandler
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

  // CefRenderHandler
  void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override;
  void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
               const RectList& dirty_rects, const void* buffer, int width,
               int height) override;

  // CefLoadHandler / CefDisplayHandler - bring-up visibility only; these
  // files are rex-free (shared build shape with the other Lua/console
  // files), so stderr rather than REXLOG.
  void OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                   ErrorCode error_code, const CefString& error_text,
                   const CefString& failed_url) override;
  bool OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                        cef_log_severity_t level, const CefString& message,
                        const CefString& source, int line) override;

  // The most recent paint, BGRA32, width*height*4 bytes - or nullptr if no
  // frame has arrived yet. Backed by a mutex-guarded copy, not a view into
  // CEF's own memory, so this is safe to call from the render thread while
  // CEF paints on its own.
  //
  // `serial` counts paints. CEF only repaints when the page actually
  // changes, so a caller that uploads this to the GPU can compare serials
  // and skip the copy entirely on the (common) frames where a static page
  // has not moved - which for a full-screen NUI layer is an 8MB memcpy plus
  // a GPU copy saved on most frames.
  const uint8_t* LatestFrame(uint32_t& width, uint32_t& height,
                             uint64_t& serial);

  // Changes the logical view size, triggering a re-layout and repaint (a
  // no-op if the size did not actually change, so this is safe to call
  // every frame).
  void Resize(int width, int height);
  void CurrentSize(int& out_width, int& out_height) const;

  // An OSR browser has no native window, so nothing tells it when it is
  // focused - the app must say so explicitly. Without this, Chromium's
  // "focus the clicked form control" behaviour never runs.
  void SetFocus(bool focus);

  void SendMouseMove(int x, int y, uint32_t modifiers);
  void SendMouseButton(int x, int y, int button, bool mouse_up,
                       uint32_t modifiers, int click_count);
  void SendMouseWheel(int x, int y, int delta_x, int delta_y,
                      uint32_t modifiers);
  void SendKeyEvent(int win_vk, bool is_down, bool is_char,
                    uint16_t utf16_char, uint32_t modifiers);

  // Runs `code` in the frame named `frame_name`, or in the main frame when
  // it is empty. Silently does nothing if that frame does not exist (yet) -
  // an iframe for a resource whose page is still loading is an ordinary,
  // transient state, not an error.
  void ExecuteJavaScript(const std::string& frame_name,
                         const std::string& code);

  CefRefPtr<CefBrowserHost> Host();

 private:
  const char* tag_;
  const int frame_rate_;

  std::atomic<int> view_width_;
  std::atomic<int> view_height_;

  std::mutex browser_mutex_;
  CefRefPtr<CefBrowser> browser_;

  std::mutex frame_mutex_;
  std::vector<uint8_t> frame_;
  uint32_t frame_width_ = 0;
  uint32_t frame_height_ = 0;
  uint64_t frame_serial_ = 0;
  bool frame_valid_ = false;

  IMPLEMENT_REFCOUNTING(Surface);
};

}  // namespace skate3::cef_internal
