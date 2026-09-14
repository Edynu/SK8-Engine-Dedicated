#if defined(SKATE3_ENABLE_CEF) && SKATE3_ENABLE_CEF

#include "skate3_cef_browser.h"

#include <include/cef_command_line.h>

#include <rex/filesystem.h>
#include <include/cef_task.h>

#include <cstdio>
#include <string>
#include <filesystem>
#include <system_error>
#include <cstring>
#include <utility>

namespace skate3::cef_internal {

namespace {

class Skate3CefApp : public CefApp {
 public:
  // Site isolation MUST be off for NUI to be usable.
  //
  // The NUI layer is a root document holding one iframe per resource, each
  // on its own https://<resource>/ origin. With Chromium's default site
  // isolation those iframes are out-of-process, and an out-of-process
  // iframe under OFF-SCREEN rendering renders correctly but never receives
  // forwarded input - SendMouseMoveEvent/SendMouseClickEvent/SendKeyEvent
  // all land in the root process and stop there. The symptom is a page that
  // looks perfect and is completely dead: verified directly with a DOM
  // event probe reading `mousemove 0  mousedown 0  keydown 0` while the
  // cursor sat on a button.
  //
  // Same-process iframes route input normally, so the fix is to keep the
  // whole NUI tree in one renderer. This is what FiveM does for its own
  // identically-shaped NUI layer.
  void OnBeforeCommandLineProcessing(
      const CefString& process_type,
      CefRefPtr<CefCommandLine> command_line) override {
    (void)process_type;
    command_line->AppendSwitch("disable-site-isolation-trials");
    command_line->AppendSwitchWithValue("disable-features",
                                        "IsolateOrigins,site-per-process");
  }

  IMPLEMENT_REFCOUNTING(Skate3CefApp);
};

// A hand-rolled CefTask rather than base::BindOnce/CefCreateClosureTask:
// CEF's bind machinery is a Chromium port with its own callback types, and
// a four-line task is less to reason about than making a std::function
// survive it.
class FunctionTask : public CefTask {
 public:
  explicit FunctionTask(std::function<void()> fn) : fn_(std::move(fn)) {}
  void Execute() override {
    if (fn_) {
      fn_();
    }
  }

 private:
  std::function<void()> fn_;

  IMPLEMENT_REFCOUNTING(FunctionTask);
};

unsigned long CurrentProcessId() { return GetCurrentProcessId(); }

// Takes an exclusive, process-lifetime claim on `directory` by holding a
// lock file inside it open with no sharing. Returns false when another live
// process already holds it.
//
// Used to decide whether this process may use the shared CEF profile - see
// EnsureRuntime. Deliberately a file rather than a named mutex: the thing
// being contended IS a directory, and a lock that lives beside it cannot go
// stale relative to it or collide with an unrelated install's name.
bool ClaimDirectory(const std::filesystem::path& directory) {
  static HANDLE claim = INVALID_HANDLE_VALUE;
  if (claim != INVALID_HANDLE_VALUE) {
    return true;  // already claimed by this process
  }
  const std::filesystem::path lock_path = directory / "skate3.lock";
  claim = CreateFileW(lock_path.c_str(), GENERIC_WRITE,
                      /*dwShareMode=*/0, nullptr, OPEN_ALWAYS,
                      FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY,
                      nullptr);
  return claim != INVALID_HANDLE_VALUE;
}

std::mutex g_runtime_mutex;
CefRefPtr<Skate3CefApp> g_app;
int g_runtime_refs = 0;
// Sticky: once CefInitialize has failed, it must not be retried - CEF does
// not support re-initialising in the same process after a failure, and a
// second attempt would look like a hang rather than a clean no-op.
bool g_runtime_failed = false;

}  // namespace

bool EnsureRuntime() {
  std::lock_guard<std::mutex> lock(g_runtime_mutex);
  if (g_runtime_failed) {
    return false;
  }
  if (g_runtime_refs > 0) {
    ++g_runtime_refs;
    return true;
  }

  CefMainArgs main_args(GetModuleHandle(nullptr));
  CefSettings settings;
  settings.windowless_rendering_enabled = true;
  settings.multi_threaded_message_loop = true;
  settings.no_sandbox = true;
  settings.log_severity = LOGSEVERITY_WARNING;

  // Per-INSTALL cache, not CEF's default shared one.
  //
  // Left unset, every copy of the game on the machine shares
  // %LOCALAPPDATA%\CEF\User Data - which CEF itself warns about at startup
  // ("Please customize CefSettings.root_cache_path... may lead to
  // unintended process singleton behavior"). Two local clients then fight
  // over one browser profile, and the visible symptom is one client
  // rendering a different (stale) version of a resource's UI than the
  // other, with nothing in the game to explain why.
  //
  // Rooting it next to the executable means the portable per-client
  // directories the local-multiplayer launcher stages each get their own,
  // which is exactly the isolation those directories exist to provide.
  //
  // The install directory alone is NOT enough. Chromium's singleton is per
  // profile directory, and a second instance that finds one already locked
  // hands its URL to the FIRST instance instead of opening its own - which
  // surfaces as a real browser window appearing outside the game while the
  // in-game overlay stays empty. Several instances launched from the same
  // folder is the normal way to test multiplayer locally, so that has to
  // work without depending on how they were launched.
  //
  // So the directory is CLAIMED rather than assumed: whoever holds the lock
  // file keeps the shared profile, and anyone who cannot take it falls back
  // to a private per-process one. That way the first instance still gets a
  // persistent profile across runs (localStorage a resource's page wrote
  // survives), and later instances get correctness over persistence, which
  // is the right way round.
  //
  // The lock handle is deliberately leaked for the process lifetime: it is
  // released when the process exits, which is exactly the lifetime the
  // claim needs to cover.
  const std::filesystem::path cache_base =
      rex::filesystem::GetExecutableFolder() / "cef_cache";
  std::filesystem::path cache_root = cache_base;
  std::error_code cache_error;
  std::filesystem::create_directories(cache_root, cache_error);
  if (!cache_error && !ClaimDirectory(cache_root)) {
    cache_root = cache_base / ("instance" + std::to_string(CurrentProcessId()));
    std::filesystem::create_directories(cache_root, cache_error);
  }
  if (cache_error) {
    // A read-only install directory is not a reason to refuse to start: CEF
    // falls back to its own default, which is what happened before this.
    std::fprintf(stderr, "[cef] could not create %s (%s); using CEF's "
                         "default shared cache\n",
                 cache_root.string().c_str(), cache_error.message().c_str());
  } else {
    CefString(&settings.root_cache_path) = cache_root.string();
    // Logged because "my second client opened a browser window" is
    // otherwise indistinguishable from "CEF is broken", and the cache path
    // is the whole answer.
    std::fprintf(stderr, "[cef] profile: %s\n", cache_root.string().c_str());
  }

  g_app = new Skate3CefApp();
  if (!CefInitialize(main_args, settings, g_app.get(), nullptr)) {
    std::fprintf(stderr, "[cef] CefInitialize failed\n");
    g_app = nullptr;
    g_runtime_failed = true;
    return false;
  }
  g_runtime_refs = 1;
  return true;
}

void ReleaseRuntime() {
  std::lock_guard<std::mutex> lock(g_runtime_mutex);
  if (g_runtime_refs <= 0) {
    return;
  }
  if (--g_runtime_refs > 0) {
    return;
  }
  CefShutdown();
  g_app = nullptr;
}

void RunOnCefUiThread(std::function<void()> fn) {
  if (CefCurrentlyOn(TID_UI)) {
    fn();
    return;
  }
  CefPostTask(TID_UI, new FunctionTask(std::move(fn)));
}

Surface::Surface(const char* tag, int default_width, int default_height,
                 int frame_rate)
    : tag_(tag),
      frame_rate_(frame_rate),
      view_width_(default_width),
      view_height_(default_height) {}

void Surface::CreateBrowser(const std::string& url) {
  CefWindowInfo window_info;
  window_info.SetAsWindowless(nullptr);

  CefBrowserSettings browser_settings;
  browser_settings.windowless_frame_rate = frame_rate_;
  // Alpha 0 is CEF's own opt-in to transparent OSR painting: the page's
  // own background shows through to the alpha channel, which is what lets
  // the overlay shader composite it over the game instead of over an
  // opaque white sheet.
  browser_settings.background_color = CefColorSetARGB(0, 0, 0, 0);

  CefBrowserHost::CreateBrowser(window_info, this, url, browser_settings,
                                nullptr, nullptr);
}

void Surface::CloseBrowser() {
  CefRefPtr<CefBrowserHost> host = Host();
  if (host) {
    host->CloseBrowser(true);
  }
}

void Surface::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  {
    std::lock_guard<std::mutex> lock(browser_mutex_);
    browser_ = browser;
  }
  // A windowless browser has no window to learn its state from, so nothing
  // has told Chromium it is visible or how big it is. Painting happens
  // regardless (we drive that ourselves through GetViewRect/OnPaint), but
  // MOUSE input does not: hit testing runs against the host-side view, and
  // an unsized or hidden one silently swallows every mouse event. Key
  // events keep working the whole time because they go to the focused
  // widget rather than through hit testing - which is exactly the symptom
  // this cost a debugging session to pin down (a page reading
  // "mousemove 0  mousedown 0  keydown 7" with the cursor on a button).
  CefRefPtr<CefBrowserHost> host = browser->GetHost();
  if (host) {
    host->WasHidden(false);
    host->WasResized();
  }
}

void Surface::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  std::lock_guard<std::mutex> lock(browser_mutex_);
  if (browser_ && browser_->IsSame(browser)) {
    browser_ = nullptr;
  }
}

void Surface::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) {
  (void)browser;
  rect = CefRect(0, 0, view_width_.load(std::memory_order_relaxed),
                 view_height_.load(std::memory_order_relaxed));
}

void Surface::OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                      const RectList& dirty_rects, const void* buffer,
                      int width, int height) {
  (void)browser;
  (void)dirty_rects;
  if (type != PET_VIEW) {
    return;  // popups (autocomplete-style dropdowns) not supported here.
  }
  const std::size_t bytes =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
  std::lock_guard<std::mutex> lock(frame_mutex_);
  frame_.resize(bytes);
  std::memcpy(frame_.data(), buffer, bytes);
  frame_width_ = static_cast<uint32_t>(width);
  frame_height_ = static_cast<uint32_t>(height);
  ++frame_serial_;
  frame_valid_ = true;
}

void Surface::OnLoadError(CefRefPtr<CefBrowser> browser,
                          CefRefPtr<CefFrame> frame, ErrorCode error_code,
                          const CefString& error_text,
                          const CefString& failed_url) {
  (void)browser;
  (void)frame;
  if (error_code == ERR_ABORTED) {
    return;  // navigation cancelled by a new navigation; not a real error.
  }
  std::fprintf(stderr, "[cef:%s] load error %d (%s) at %s\n", tag_,
               static_cast<int>(error_code), error_text.ToString().c_str(),
               failed_url.ToString().c_str());
}

bool Surface::OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                               cef_log_severity_t level,
                               const CefString& message,
                               const CefString& source, int line) {
  (void)browser;
  std::fprintf(stderr, "[cef:%s:%d] %s (%s:%d)\n", tag_,
               static_cast<int>(level), message.ToString().c_str(),
               source.ToString().c_str(), line);
  return false;  // let CEF's own default handling continue too.
}

const uint8_t* Surface::LatestFrame(uint32_t& width, uint32_t& height,
                                    uint64_t& serial) {
  std::lock_guard<std::mutex> lock(frame_mutex_);
  if (!frame_valid_) {
    width = 0;
    height = 0;
    serial = 0;
    return nullptr;
  }
  width = frame_width_;
  height = frame_height_;
  serial = frame_serial_;
  return frame_.data();
}

void Surface::Resize(int width, int height) {
  if (width <= 0 || height <= 0) {
    return;
  }
  const int previous_width =
      view_width_.exchange(width, std::memory_order_relaxed);
  const int previous_height =
      view_height_.exchange(height, std::memory_order_relaxed);
  if (previous_width == width && previous_height == height) {
    return;
  }
  CefRefPtr<CefBrowserHost> host = Host();
  if (host) {
    // Tells CEF the size GetViewRect will now report changed; it re-queries
    // and repaints at the new dimensions. A resize that lands BEFORE the
    // browser exists reaches no host at all, which is why OnAfterCreated
    // re-applies it unconditionally.
    host->WasResized();
  }
}

void Surface::CurrentSize(int& out_width, int& out_height) const {
  out_width = view_width_.load(std::memory_order_relaxed);
  out_height = view_height_.load(std::memory_order_relaxed);
}

void Surface::SetFocus(bool focus) {
  CefRefPtr<CefBrowserHost> host = Host();
  if (host) {
    host->SetFocus(focus);
  }
}

void Surface::SendMouseMove(int x, int y, uint32_t modifiers) {
  CefRefPtr<CefBrowserHost> host = Host();
  if (!host) {
    return;
  }
  CefMouseEvent event;
  event.x = x;
  event.y = y;
  event.modifiers = modifiers;
  host->SendMouseMoveEvent(event, false);
}

void Surface::SendMouseButton(int x, int y, int button, bool mouse_up,
                              uint32_t modifiers, int click_count) {
  CefRefPtr<CefBrowserHost> host = Host();
  if (!host) {
    return;
  }
  CefMouseEvent event;
  event.x = x;
  event.y = y;
  event.modifiers = modifiers;
  cef_mouse_button_type_t type = MBT_LEFT;
  if (button == 1) {
    type = MBT_MIDDLE;
  } else if (button == 2) {
    type = MBT_RIGHT;
  }
  host->SendMouseClickEvent(event, type, mouse_up, click_count);
}

void Surface::SendMouseWheel(int x, int y, int delta_x, int delta_y,
                             uint32_t modifiers) {
  CefRefPtr<CefBrowserHost> host = Host();
  if (!host) {
    return;
  }
  CefMouseEvent event;
  event.x = x;
  event.y = y;
  event.modifiers = modifiers;
  host->SendMouseWheelEvent(event, delta_x, delta_y);
}

void Surface::SendKeyEvent(int win_vk, bool is_down, bool is_char,
                           uint16_t utf16_char, uint32_t modifiers) {
  CefRefPtr<CefBrowserHost> host = Host();
  if (!host) {
    return;
  }
  CefKeyEvent event;
  event.modifiers = modifiers;
  if (is_char) {
    event.type = KEYEVENT_CHAR;
    event.character = static_cast<char16_t>(utf16_char);
    event.unmodified_character = event.character;
    event.windows_key_code = utf16_char;
  } else {
    event.type = is_down ? KEYEVENT_RAWKEYDOWN : KEYEVENT_KEYUP;
    event.windows_key_code = win_vk;
    event.native_key_code = win_vk;
  }
  host->SendKeyEvent(event);
}

void Surface::ExecuteJavaScript(const std::string& frame_name,
                                const std::string& code) {
  CefRefPtr<CefBrowser> browser;
  {
    std::lock_guard<std::mutex> lock(browser_mutex_);
    browser = browser_;
  }
  if (!browser) {
    return;
  }
  // CefBrowser/CefFrame are UI-thread-only, and every caller here is the
  // game's own render or script thread.
  RunOnCefUiThread([browser, frame_name, code]() {
    CefRefPtr<CefFrame> frame = frame_name.empty()
                                    ? browser->GetMainFrame()
                                    : browser->GetFrameByName(frame_name);
    if (!frame) {
      return;
    }
    frame->ExecuteJavaScript(code, frame->GetURL(), 0);
  });
}

CefRefPtr<CefBrowserHost> Surface::Host() {
  std::lock_guard<std::mutex> lock(browser_mutex_);
  return browser_ ? browser_->GetHost() : nullptr;
}

}  // namespace skate3::cef_internal

#endif  // SKATE3_ENABLE_CEF
