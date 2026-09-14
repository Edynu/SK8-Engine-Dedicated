#pragma once

// NUI: the FiveM-style HTML layer resources draw their own UI into.
//
// Shape of the thing, because it is not obvious from the API alone:
// there is exactly ONE off-screen browser for all of NUI, full-screen,
// transparent, permanently alive. It loads a built-in root document
// (https://skate3-nui/root.html) which holds one <iframe> per resource
// that declared a ui_page. That mirrors FiveM's own arrangement and buys
// two things a browser-per-resource would not: one texture to composite
// instead of N, and a natural stacking order for overlapping UIs.
//
// Pages are served over plain HTTP from the local admin server, at
//   http://127.0.0.1:<admin port>/nui/<resource>/<file>
// and the root document at .../nui/root.html, so the whole NUI tree is one
// origin and no cross-origin rules apply to it.
//
// The https://<resource>/ scheme handler is still registered, but ONLY for
// NUI callbacks, so a page ported from FiveM can keep its
//   fetch(`https://${GetParentResourceName()}/doThing`, {method:'POST'...})
// unchanged. A fetch never touches hit testing, so it is unaffected; the
// responses carry CORS headers because the page's own origin is now the
// admin server's.
//
// Files come from SetResourcePage below, not from disk: on the client,
// resources arrive over the network from the connected server and are
// never written out (see SyncResourcesFromServer).
//
// When built without SKATE3_ENABLE_CEF every function here is a harmless
// no-op, so callers never need their own #if guards.

#include "skate3_cef_input.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace skate3::cef_nui {

// Creates the NUI browser. `admin_http_port` is where its pages are
// fetched from - the same server SetNuiFileProvider is wired into. Safe to
// call before any resource exists: the root document simply has no frames
// until SetResourcePage adds one.
void Initialize(int admin_http_port);

// Backs GET /nui/<path> on the admin server: "root.html", "pages.json", or
// "<resource>/<file>". Returns false for anything else.
bool LookupFile(const std::string& path, std::string& out_data,
                std::string& out_mime);
void Shutdown();

// --- Resource pages -------------------------------------------------

// Publishes (or replaces) one resource's UI. `ui_page` is the manifest's
// own ui_page path; `files` maps every servable path within the resource
// (including ui_page itself) to its bytes. Adding a resource creates its
// iframe on the next root-document sync; replacing one reloads it.
//
// Passing an empty ui_page is legal and means "this resource has files but
// no page of its own" - nothing is framed, but its files stay fetchable by
// another resource's page.
void SetResourcePage(const std::string& resource, const std::string& ui_page,
                     std::unordered_map<std::string, std::string> files);
// Drops a resource's page and files, removing its iframe.
void RemoveResourcePage(const std::string& resource);
// Drops every published page at once. Used when the client leaves its
// server: all its resources came from there, so all their UI goes with it.
void ClearResourcePages();

// --- Lua -> page ----------------------------------------------------

// SendNUIMessage: delivers `json` to the resource's frame as an ordinary
// window message, so the page receives it through the FiveM-standard
//   window.addEventListener('message', event => { ... event.data ... })
// `json` must be a JSON-encoded value (the Lua native encodes the table it
// was handed before calling this).
void PostMessageToResource(const std::string& resource,
                           const std::string& json);

// --- page -> Lua ----------------------------------------------------

// Completes one NUI callback. Takes the JSON body to answer the page's
// fetch() with; "" is sent as an empty JSON object. Calling it twice is
// harmless (the second call is ignored), which is what lets a Lua handler
// respond immediately OR keep the cb around and answer later.
using RespondFn = std::function<void(const std::string& json_result)>;

// Called for every POST a resource's page makes to its own origin. Runs on
// a CEF thread, NOT the game thread. Injected rather than implemented here
// so this file stays free of the Lua host (which is shared with the
// dedicated server, where NUI does not exist).
using CallbackInvoker =
    std::function<void(const std::string& resource, const std::string& name,
                       const std::string& json_body, RespondFn respond)>;
void SetCallbackInvoker(CallbackInvoker invoker);

// --- Focus ----------------------------------------------------------

// SetNuiFocus. `has_focus` routes mouse/keyboard/gamepad into the page
// (and, unless SetKeepInput was told otherwise, stops that input reaching
// the skater); `has_cursor` shows a cursor to aim with.
//
// `resource` is the script that asked, and it decides WHICH page receives
// pointer input. Every resource with a ui_page gets a full-screen iframe,
// so without this the topmost one silently swallows every mouse event
// meant for the page underneath it - a page that renders perfectly,
// responds to the keyboard (keys go to the FOCUSED frame, which is a
// different thing entirely) and ignores the mouse completely. Only the
// focused resource's frame gets pointer-events; the rest stay click
// through, which is also what a passive HUD wants.
void SetFocus(const std::string& resource, bool has_focus, bool has_cursor);
// SetNuiFocusKeepInput. When true, gameplay keeps receiving input even
// while NUI holds focus - the FiveM idiom for a HUD or overlay you can
// click without parking the player.
void SetKeepInput(bool keep_input);
// Tells the off-screen browser it is focused again, without touching
// which resource owns pointer input. An OSR browser never gains or loses
// window focus by itself, so a click on a previously-blurred element only
// works if something re-asserts it.
void ReassertBrowserFocus();
bool HasFocus();
bool HasCursor();
bool KeepInput();

// --- Rendering ------------------------------------------------------

// True when the overlay has anything to draw: at least one resource page is
// published. The render pass skips NUI entirely otherwise, so a session
// with no UI resources pays nothing.
bool HasContent();

// The most recent OSR paint, BGRA32. `serial` counts paints: NUI is
// always on screen but a static page repaints rarely, so the render pass
// compares serials and re-uploads only when the pixels actually changed.
const uint8_t* LatestFrame(uint32_t& width, uint32_t& height,
                           uint64_t& serial);
// NUI is always full-screen, so this is driven by guest_output's size from
// the render pass every frame.
void Resize(int width, int height);
void CurrentSize(int& out_width, int& out_height);

// --- Input forwarding -----------------------------------------------
// Coordinates are in the browser's own pixel space, which for NUI is the
// guest-output pixel space (it is full-screen and drawn 1:1).

void SendMouseMove(int x, int y, uint32_t modifiers);
void SendMouseButton(int x, int y, int button, bool mouse_up,
                     uint32_t modifiers, int click_count);
void SendMouseWheel(int x, int y, int delta_x, int delta_y, uint32_t modifiers);
void SendKeyEvent(int win_vk, bool is_down, bool is_char, uint16_t utf16_char,
                  uint32_t modifiers);

// Moves the gamepad-driven pointer the root document draws. The game runs
// on a controller, so there is not always an OS cursor to composite over
// the page - see skate3_cef_gamepad.h. Position is in the same pixel space
// as the mouse events above; `visible` false hides it entirely (which is
// what a keyboard/mouse session wants).
void SetVirtualCursor(int x, int y, bool visible);

}  // namespace skate3::cef_nui
