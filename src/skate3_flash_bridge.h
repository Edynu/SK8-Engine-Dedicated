#pragma once

// Calling retail's Flash front end from our own code.
//
// The front end is an APT/Flash movie. C++ drives it by invoking named
// ActionScript methods through one dispatcher:
//
//   sub_825D3B38(method_id, 0, 0, argc, arg1, arg2, ...)
//
// where method_id indexes a table of 61 method-path strings at 0x8302C608
// ("_global.ScreenManager.OpenScreen" is 6) and each argument is a plain
// pointer - a string argument is an ordinary NUL-terminated char*. See
// docs/frontend_bridge.md for how this was established.
//
// A CALL IS QUEUED, NOT MADE HERE. Retail code has to run on the guest CPU
// thread with a live context, and the callers of this are script threads
// which have neither. The queued call is made at the next physics tick.

#include <cstdint>

#include <rex/ppc/context.h>

namespace skate3::flash_bridge {

// Up to four arguments, which covers every observed call site (the widest
// seen is two).
constexpr int kMaxArguments = 4;

// Returns false when a call is already queued and has not run yet - two
// front-end calls racing through one slot would silently lose one.
bool RequestCall(std::uint32_t method_id, const std::uint32_t* arguments,
                 int argument_count);

// Runs a queued call. Must be called from a guest hook with a valid context.
void ApplyPending(PPCContext& ctx, std::uint8_t* base);

}  // namespace skate3::flash_bridge
