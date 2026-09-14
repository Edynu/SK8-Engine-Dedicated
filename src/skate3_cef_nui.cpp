#include "skate3_cef_nui.h"

#if defined(SKATE3_ENABLE_CEF) && SKATE3_ENABLE_CEF

#include "skate3_cef_browser.h"

#include <include/cef_parser.h>
#include <include/cef_request.h>
#include <include/cef_resource_handler.h>
#include <include/cef_scheme.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace skate3::cef_nui {

namespace {

// Where NUI's pages are fetched from. Set by Initialize; "" until then.
std::string g_page_origin;

// Full-screen, so these are only placeholders until the render pass calls
// Resize() with guest_output's real size on its first NUI frame.
constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 720;
// UI wants to animate; unlike the dev console this is a real interface.
constexpr int kFrameRate = 60;

CefRefPtr<cef_internal::Surface> g_surface;

// ---------------------------------------------------------------------
// Resource registry
// ---------------------------------------------------------------------

struct PageEntry {
  std::string display_name;  // as the manifest spells it, e.g. "EventTest"
  std::string ui_page;
  std::unordered_map<std::string, std::string> files;
};

std::mutex g_pages_mutex;
// Keyed by the lowercased resource name, because that is what survives a
// round trip through a URL host - Chromium lowercases hosts, so a resource
// called "EventTest" is reachable only as https://eventtest/.
std::unordered_map<std::string, PageEntry> g_pages;
// Monotonic, so the root document can ignore a page list that lost a race
// with a newer push (see the version check in the root script).
uint32_t g_pages_version = 0;

std::mutex g_invoker_mutex;
CallbackInvoker g_invoker;

// The resource whose frame currently receives pointer input, "" for none.
std::string g_focus_resource;
std::atomic<bool> g_has_focus{false};
std::atomic<bool> g_has_cursor{false};
std::atomic<bool> g_keep_input{false};

// Last virtual-cursor state actually pushed into the page, so a stationary
// cursor costs no ExecuteJavaScript at all.
std::mutex g_cursor_mutex;
int g_cursor_x = -1;
int g_cursor_y = -1;
bool g_cursor_visible = false;

std::string ToLowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string EscapeJsonString(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (const char c : value) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          static const char kHex[] = "0123456789abcdef";
          out += "\\u00";
          out += kHex[(c >> 4) & 0xF];
          out += kHex[c & 0xF];
        } else {
          out += c;
        }
    }
  }
  return out;
}

// Leading slashes and any "./" the page's own relative URL resolution
// produced are stripped so a request for "/ui/app.js" finds the file the
// manifest declared as "ui/app.js".
std::string NormalizeResourcePath(std::string path) {
  while (!path.empty() && path.front() == '/') {
    path.erase(path.begin());
  }
  while (path.rfind("./", 0) == 0) {
    path.erase(0, 2);
  }
  return path;
}

const char* MimeForPath(const std::string& path) {
  const std::size_t dot = path.rfind('.');
  const std::string ext =
      dot == std::string::npos ? std::string() : ToLowerAscii(path.substr(dot));
  if (ext == ".html" || ext == ".htm") return "text/html";
  if (ext == ".js" || ext == ".mjs") return "text/javascript";
  if (ext == ".css") return "text/css";
  if (ext == ".json") return "application/json";
  if (ext == ".png") return "image/png";
  if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
  if (ext == ".gif") return "image/gif";
  if (ext == ".svg") return "image/svg+xml";
  if (ext == ".webp") return "image/webp";
  if (ext == ".woff") return "font/woff";
  if (ext == ".woff2") return "font/woff2";
  if (ext == ".ttf") return "font/ttf";
  if (ext == ".otf") return "font/otf";
  if (ext == ".mp3") return "audio/mpeg";
  if (ext == ".ogg") return "audio/ogg";
  if (ext == ".wav") return "audio/wav";
  if (ext == ".mp4") return "video/mp4";
  return "application/octet-stream";
}

// FiveM UIs universally build their callback URL from
// GetParentResourceName(), so every HTML document this serves gets that
// function defined before any of its own scripts run. Injected here rather
// than through a render-process handler because we already own the bytes:
// no CefApp subprocess plumbing needed for something this small.
std::string InjectResourceShim(const std::string& html,
                               const std::string& host) {
  std::string shim =
      "<script>window.GetParentResourceName=function(){return \"" +
      EscapeJsonString(host) +
      "\";};</script>";
  // Before <head>'s first child if there is a head, otherwise at the very
  // top - either way ahead of the page's own <script> tags.
  static constexpr char kHeadOpen[] = "<head>";
  const std::size_t head = ToLowerAscii(html).find(kHeadOpen);
  if (head != std::string::npos) {
    const std::size_t insert = head + std::strlen(kHeadOpen);
    return html.substr(0, insert) + shim + html.substr(insert);
  }
  return shim + html;
}

std::string BuildPagesJson() {
  // Caller holds g_pages_mutex.
  std::string json = "{\"version\":" + std::to_string(g_pages_version) +
                     ",\"pages\":[";
  bool first = true;
  for (const auto& [host, entry] : g_pages) {
    if (entry.ui_page.empty()) {
      continue;  // files-only resource; nothing to frame.
    }
    if (!first) {
      json += ',';
    }
    first = false;
    json += "{\"name\":\"" + EscapeJsonString(host) + "\",\"src\":\"" +
            EscapeJsonString(g_page_origin) + "/nui/" +
            EscapeJsonString(host) + "/" +
            EscapeJsonString(NormalizeResourcePath(entry.ui_page)) + "\"}";
  }
  json += "]}";
  return json;
}

const char kRootDocument[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>skate3 nui</title>
<style>
html,body{margin:0;padding:0;width:100%;height:100%;overflow:hidden;background:transparent}
#frames{position:absolute;left:0;top:0;right:0;bottom:0}
#frames iframe{position:absolute;left:0;top:0;width:100%;height:100%;border:0;display:block;background:transparent;pointer-events:none}
#frames iframe.input{pointer-events:auto}
#cursor{position:absolute;left:0;top:0;width:24px;height:24px;pointer-events:none;display:none;z-index:2147483647;filter:drop-shadow(0 1px 2px rgba(0,0,0,.65))}
</style></head>
<body>
<div id="frames"></div>
<svg id="cursor" viewBox="0 0 24 24"><path d="M3 1.5 L3 18 L7.6 14 L10.4 20.5 L13.2 19.3 L10.5 13 L17 13 Z" fill="#ffffff" stroke="#101010" stroke-width="1.4" stroke-linejoin="round"/></svg>
<script>
(function(){
  var version = -1;
  var frames = document.getElementById('frames');
  var cursor = document.getElementById('cursor');
  window.__skate3_setPages = function(payload){
    if (!payload || typeof payload.version !== 'number') return;
    if (payload.version <= version) return;
    version = payload.version;
    var wanted = {};
    (payload.pages || []).forEach(function(p){ wanted[p.name] = p; });
    Array.prototype.slice.call(frames.children).forEach(function(el){
      var p = wanted[el.name];
      if (!p) { el.parentNode.removeChild(el); return; }
      if (el.src !== p.src) { el.src = p.src; }
      delete wanted[el.name];
    });
    Object.keys(wanted).forEach(function(name){
      var f = document.createElement('iframe');
      f.name = name;
      f.setAttribute('allowtransparency', 'true');
      f.src = wanted[name].src;
      frames.appendChild(f);
    });
    // Newly created frames start click-through; re-assert the current
    // owner so a resource that already holds focus keeps its input.
    if (window.__skate3_inputFrame) {
      window.__skate3_setInputFrame(window.__skate3_inputFrame);
    }
  };
  window.__skate3_setInputFrame = function(name){
    window.__skate3_inputFrame = name;
    Array.prototype.slice.call(frames.children).forEach(function(el){
      var on = !!name && el.name === name;
      el.classList.toggle('input', on);
      el.style.zIndex = on ? '2' : '1';
    });
  };

  window.__skate3_cursor = function(x, y, visible){
    if (!visible) { cursor.style.display = 'none'; return; }
    cursor.style.display = 'block';
    cursor.style.transform = 'translate(' + x + 'px,' + y + 'px)';
  };
  // Covers the case where the root document finished loading AFTER a page
  // was published: the push it missed is recovered here, and the version
  // check above discards this list if a newer push already landed.
  fetch('/nui/pages.json')
    .then(function(r){ return r.json(); })
    .then(window.__skate3_setPages)
    .catch(function(){});
})();
</script>
</body></html>
)HTML";

// ---------------------------------------------------------------------
// Resource handlers
// ---------------------------------------------------------------------

// Serves a block of bytes already in hand. Everything NUI serves except a
// pending callback response is this.
class MemoryResourceHandler : public CefResourceHandler {
 public:
  MemoryResourceHandler(std::string data, std::string mime, int status,
                        bool cors = false)
      : data_(std::move(data)),
        mime_(std::move(mime)),
        status_(status),
        cors_(cors) {}

  bool Open(CefRefPtr<CefRequest> request, bool& handle_request,
            CefRefPtr<CefCallback> callback) override {
    (void)request;
    (void)callback;
    handle_request = true;  // data is ready; no async continuation needed.
    return true;
  }

  void GetResponseHeaders(CefRefPtr<CefResponse> response,
                          int64_t& response_length,
                          CefString& redirectUrl) override {
    (void)redirectUrl;
    response->SetMimeType(mime_);
    response->SetStatus(status_);
    if (cors_) {
      CefResponse::HeaderMap headers;
      headers.insert({"Access-Control-Allow-Origin", "*"});
      headers.insert({"Access-Control-Allow-Methods", "POST, OPTIONS"});
      headers.insert({"Access-Control-Allow-Headers", "Content-Type"});
      response->SetHeaderMap(headers);
    }
    response_length = static_cast<int64_t>(data_.size());
  }

  bool Read(void* data_out, int bytes_to_read, int& bytes_read,
            CefRefPtr<CefResourceReadCallback> callback) override {
    (void)callback;
    const std::size_t remaining = data_.size() - offset_;
    if (remaining == 0) {
      bytes_read = 0;
      return false;  // completion
    }
    const std::size_t count =
        std::min(remaining, static_cast<std::size_t>(bytes_to_read));
    std::memcpy(data_out, data_.data() + offset_, count);
    offset_ += count;
    bytes_read = static_cast<int>(count);
    return true;
  }

  void Cancel() override {}

 private:
  std::string data_;
  std::string mime_;
  int status_;
  bool cors_ = false;
  std::size_t offset_ = 0;

  IMPLEMENT_REFCOUNTING(MemoryResourceHandler);
};

// One NUI callback in flight. The Lua handler on the other side may answer
// immediately or hold on to its cb and answer much later, so this supports
// both: whichever happens, Respond() is called exactly once and the fetch()
// completes. Everything is under mutex_ because Respond() can arrive on any
// thread, concurrently with Open() still running on CEF's.
class CallbackResourceHandler : public CefResourceHandler {
 public:
  CallbackResourceHandler(std::string resource, std::string name)
      : resource_(std::move(resource)), name_(std::move(name)) {}

  bool Open(CefRefPtr<CefRequest> request, bool& handle_request,
            CefRefPtr<CefCallback> callback) override {
    const std::string body = ReadPostBody(request);

    CallbackInvoker invoker;
    {
      std::lock_guard<std::mutex> lock(g_invoker_mutex);
      invoker = g_invoker;
    }
    if (!invoker) {
      // No Lua host wired up (or already torn down): answer immediately
      // rather than leaving the page's fetch() hanging forever.
      std::lock_guard<std::mutex> lock(mutex_);
      result_ = "{}";
      responded_ = true;
      handle_request = true;
      return true;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      continuation_ = callback;
    }

    CefRefPtr<CallbackResourceHandler> self(this);
    invoker(resource_, name_, body, [self](const std::string& json_result) {
      self->Respond(json_result);
    });

    std::lock_guard<std::mutex> lock(mutex_);
    if (responded_) {
      // The handler answered synchronously, from inside the invoke above.
      // Take the simple path and skip the continuation entirely - Respond
      // saw open_returned_ still false and deliberately did not call it.
      continuation_ = nullptr;
      handle_request = true;
      return true;
    }
    open_returned_ = true;
    handle_request = false;  // answered later, via CefCallback::Continue.
    return true;
  }

  void GetResponseHeaders(CefRefPtr<CefResponse> response,
                          int64_t& response_length,
                          CefString& redirectUrl) override {
    (void)redirectUrl;
    std::lock_guard<std::mutex> lock(mutex_);
    response->SetMimeType("application/json");
    response->SetStatus(200);
    // The page lives on the admin server's origin now, so its POST here is
    // cross-origin and the answer has to say so or fetch() rejects it.
    CefResponse::HeaderMap headers;
    headers.insert({"Access-Control-Allow-Origin", "*"});
    response->SetHeaderMap(headers);
    response_length = static_cast<int64_t>(result_.size());
  }

  bool Read(void* data_out, int bytes_to_read, int& bytes_read,
            CefRefPtr<CefResourceReadCallback> callback) override {
    (void)callback;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t remaining = result_.size() - offset_;
    if (remaining == 0) {
      bytes_read = 0;
      return false;
    }
    const std::size_t count =
        std::min(remaining, static_cast<std::size_t>(bytes_to_read));
    std::memcpy(data_out, result_.data() + offset_, count);
    offset_ += count;
    bytes_read = static_cast<int>(count);
    return true;
  }

  void Cancel() override {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_ = true;
    continuation_ = nullptr;
  }

 private:
  void Respond(const std::string& json_result) {
    CefRefPtr<CefCallback> continuation;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (responded_ || cancelled_) {
        return;  // a second cb() call, or the page navigated away.
      }
      responded_ = true;
      result_ = json_result.empty() ? "{}" : json_result;
      if (!open_returned_) {
        // Still inside Open(); it will notice responded_ and take the
        // synchronous path itself.
        return;
      }
      continuation = continuation_;
      continuation_ = nullptr;
    }
    if (continuation) {
      continuation->Continue();
    }
  }

  static std::string ReadPostBody(const CefRefPtr<CefRequest>& request) {
    CefRefPtr<CefPostData> post_data = request->GetPostData();
    if (!post_data) {
      return std::string();
    }
    CefPostData::ElementVector elements;
    post_data->GetElements(elements);
    std::string body;
    for (const auto& element : elements) {
      if (element->GetType() != PDE_TYPE_BYTES) {
        continue;  // file uploads are not a thing NUI callbacks do.
      }
      const std::size_t size = element->GetBytesCount();
      const std::size_t offset = body.size();
      body.resize(offset + size);
      element->GetBytes(size, body.data() + offset);
    }
    return body;
  }

  const std::string resource_;
  const std::string name_;

  std::mutex mutex_;
  CefRefPtr<CefCallback> continuation_;
  std::string result_;
  std::size_t offset_ = 0;
  bool responded_ = false;
  bool open_returned_ = false;
  bool cancelled_ = false;

  IMPLEMENT_REFCOUNTING(CallbackResourceHandler);
};

// One factory instance per registered host. `host` empty means the root
// document's own origin.
class NuiSchemeHandlerFactory : public CefSchemeHandlerFactory {
 public:
  explicit NuiSchemeHandlerFactory(std::string host) : host_(std::move(host)) {}

  CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser> browser,
                                       CefRefPtr<CefFrame> frame,
                                       const CefString& scheme_name,
                                       CefRefPtr<CefRequest> request) override {
    (void)browser;
    (void)frame;
    (void)scheme_name;

    CefURLParts parts;
    if (!CefParseURL(request->GetURL(), parts)) {
      return new MemoryResourceHandler("bad url", "text/plain", 400);
    }
    const std::string path =
        NormalizeResourcePath(CefString(&parts.path).ToString());

    // Callbacks only - pages themselves are served over HTTP (see the
    // header). This origin exists purely so FiveM's
    // fetch(`https://${GetParentResourceName()}/cb`) keeps working.
    const std::string method = request->GetMethod().ToString();
    if (method == "OPTIONS") {
      // The page's origin is now the admin server, so a JSON POST is
      // preflighted.
      return new MemoryResourceHandler(std::string(), "text/plain", 204,
                                       /*cors=*/true);
    }
    if (method == "POST") {
      return new CallbackResourceHandler(ResourceDisplayName(), path);
    }
    return new MemoryResourceHandler(
        "NUI callbacks only; pages are served over http", "text/plain", 404,
        /*cors=*/true);
  }

 private:
  std::string ResourceDisplayName() const {
    std::lock_guard<std::mutex> lock(g_pages_mutex);
    const auto it = g_pages.find(host_);
    return it == g_pages.end() ? host_ : it->second.display_name;
  }

  const std::string host_;

  IMPLEMENT_REFCOUNTING(NuiSchemeHandlerFactory);
};

// Pushes the current page list into the root document. Harmless before the
// root has loaded: ExecuteJavaScript no-ops without a frame, and the root's
// own /pages.json fetch covers that case when it finishes loading.
void SyncRootPages() {
  if (!g_surface) {
    return;
  }
  std::string json;
  {
    std::lock_guard<std::mutex> lock(g_pages_mutex);
    json = BuildPagesJson();
  }
  g_surface->ExecuteJavaScript(
      std::string(), "window.__skate3_setPages && window.__skate3_setPages(" +
                         json + ");");
}

}  // namespace

void Initialize(int admin_http_port) {
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
  g_page_origin = "http://127.0.0.1:" + std::to_string(admin_http_port);
  g_surface = new cef_internal::Surface("nui", kDefaultWidth, kDefaultHeight,
                                        kFrameRate);
  g_surface->CreateBrowser(g_page_origin + "/nui/root.html");
}

bool LookupFile(const std::string& path, std::string& out_data,
                std::string& out_mime) {
  const std::string clean = NormalizeResourcePath(path);
  if (clean == "root.html" || clean.empty()) {
    out_data = kRootDocument;
    out_mime = "text/html";
    return true;
  }
  if (clean == "pages.json") {
    std::lock_guard<std::mutex> lock(g_pages_mutex);
    out_data = BuildPagesJson();
    out_mime = "application/json";
    return true;
  }
  // "<resource>/<file>"
  const std::size_t slash = clean.find('/');
  if (slash == std::string::npos) {
    return false;
  }
  const std::string host = ToLowerAscii(clean.substr(0, slash));
  const std::string file = clean.substr(slash + 1);
  std::lock_guard<std::mutex> lock(g_pages_mutex);
  const auto entry = g_pages.find(host);
  if (entry == g_pages.end()) {
    return false;
  }
  const auto found = entry->second.files.find(file);
  if (found == entry->second.files.end()) {
    return false;
  }
  out_mime = MimeForPath(file);
  // The shim gives the page GetParentResourceName(), which is what its
  // callback URLs are built from.
  out_data = out_mime == "text/html"
                 ? InjectResourceShim(found->second, host)
                 : found->second;
  return true;
}

void Shutdown() {
  if (!g_surface) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_invoker_mutex);
    g_invoker = nullptr;
  }
  ClearResourcePages();
  g_surface->CloseBrowser();
  g_surface = nullptr;
  cef_internal::ReleaseRuntime();
}

void SetResourcePage(const std::string& resource, const std::string& ui_page,
                     std::unordered_map<std::string, std::string> files) {
  const std::string host = ToLowerAscii(resource);
  if (host.empty()) {
    return;
  }
  bool first_registration = false;
  {
    std::lock_guard<std::mutex> lock(g_pages_mutex);
    first_registration = g_pages.find(host) == g_pages.end();
    PageEntry& entry = g_pages[host];
    entry.display_name = resource;
    entry.ui_page = ui_page;
    entry.files = std::move(files);
    ++g_pages_version;
  }
  if (first_registration) {
    CefRegisterSchemeHandlerFactory("https", host,
                                    new NuiSchemeHandlerFactory(host));
  }
  SyncRootPages();
}

void RemoveResourcePage(const std::string& resource) {
  const std::string host = ToLowerAscii(resource);
  bool removed = false;
  {
    std::lock_guard<std::mutex> lock(g_pages_mutex);
    removed = g_pages.erase(host) > 0;
    if (removed) {
      ++g_pages_version;
    }
  }
  if (!removed) {
    return;
  }
  CefRegisterSchemeHandlerFactory("https", host, nullptr);
  SyncRootPages();
}

void ClearResourcePages() {
  std::vector<std::string> hosts;
  {
    std::lock_guard<std::mutex> lock(g_pages_mutex);
    if (g_pages.empty()) {
      return;
    }
    hosts.reserve(g_pages.size());
    for (const auto& [host, entry] : g_pages) {
      (void)entry;
      hosts.push_back(host);
    }
    g_pages.clear();
    ++g_pages_version;
  }
  // Unregistered outside the lock: CefRegisterSchemeHandlerFactory can run
  // a factory's destructor, and that factory's own methods take this mutex.
  for (const std::string& host : hosts) {
    CefRegisterSchemeHandlerFactory("https", host, nullptr);
  }
  SyncRootPages();
}

void PostMessageToResource(const std::string& resource,
                           const std::string& json) {
  if (!g_surface) {
    return;
  }
  // Delivered as a real window message so the page's own
  // window.addEventListener('message', ...) - the FiveM idiom - receives it
  // with the decoded value in event.data.
  g_surface->ExecuteJavaScript(ToLowerAscii(resource),
                               "window.postMessage(" + json + ", '*');");
}

void SetCallbackInvoker(CallbackInvoker invoker) {
  std::lock_guard<std::mutex> lock(g_invoker_mutex);
  g_invoker = std::move(invoker);
}

void SetFocus(const std::string& resource, bool has_focus, bool has_cursor) {
  const std::string host = has_focus ? ToLowerAscii(resource) : std::string();
  {
    std::lock_guard<std::mutex> lock(g_pages_mutex);
    g_focus_resource = host;
  }
  g_has_focus.store(has_focus, std::memory_order_release);
  g_has_cursor.store(has_cursor, std::memory_order_release);
  if (g_surface) {
    g_surface->SetFocus(has_focus);
    // Hands pointer input to this resource's frame alone. Releasing focus
    // passes "" and makes every frame click-through again, so a HUD left on
    // screen never steals a click meant for the game.
    g_surface->ExecuteJavaScript(
        std::string(),
        "window.__skate3_setInputFrame && window.__skate3_setInputFrame(\"" +
            EscapeJsonString(host) + "\");");
  }
  if (!has_cursor) {
    SetVirtualCursor(0, 0, false);
  }
}

void SetKeepInput(bool keep_input) {
  g_keep_input.store(keep_input, std::memory_order_release);
}

void ReassertBrowserFocus() {
  if (g_surface && g_has_focus.load(std::memory_order_acquire)) {
    g_surface->SetFocus(true);
  }
}

bool HasFocus() { return g_has_focus.load(std::memory_order_acquire); }
bool HasCursor() { return g_has_cursor.load(std::memory_order_acquire); }
bool KeepInput() { return g_keep_input.load(std::memory_order_acquire); }
bool HasContent() {
  std::lock_guard<std::mutex> lock(g_pages_mutex);
  for (const auto& [host, entry] : g_pages) {
    (void)host;
    if (!entry.ui_page.empty()) {
      return true;
    }
  }
  return false;
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

void SetVirtualCursor(int x, int y, bool visible) {
  if (!g_surface) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_cursor_mutex);
    if (g_cursor_visible == visible &&
        (!visible || (g_cursor_x == x && g_cursor_y == y))) {
      return;  // nothing moved; skip the round trip into the page.
    }
    g_cursor_visible = visible;
    g_cursor_x = x;
    g_cursor_y = y;
  }
  g_surface->ExecuteJavaScript(
      std::string(), "window.__skate3_cursor && window.__skate3_cursor(" +
                         std::to_string(x) + "," + std::to_string(y) + "," +
                         (visible ? "true" : "false") + ");");
}

}  // namespace skate3::cef_nui

#else  // !SKATE3_ENABLE_CEF - harmless no-op stubs, see the header comment.

namespace skate3::cef_nui {

void Initialize(int) {}
void Shutdown() {}
bool LookupFile(const std::string&, std::string&, std::string&) {
  return false;
}
void SetResourcePage(const std::string&, const std::string&,
                     std::unordered_map<std::string, std::string>) {}
void RemoveResourcePage(const std::string&) {}
void ClearResourcePages() {}
void PostMessageToResource(const std::string&, const std::string&) {}
void SetCallbackInvoker(CallbackInvoker) {}
void SetFocus(const std::string&, bool, bool) {}
void SetKeepInput(bool) {}
void ReassertBrowserFocus() {}
bool HasFocus() { return false; }
bool HasCursor() { return false; }
bool KeepInput() { return false; }
bool HasContent() { return false; }
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
void SendMouseWheel(int, int, int, int, uint32_t) {}
void SendKeyEvent(int, bool, bool, uint16_t, uint32_t) {}
void SetVirtualCursor(int, int, bool) {}

}  // namespace skate3::cef_nui

#endif  // SKATE3_ENABLE_CEF
