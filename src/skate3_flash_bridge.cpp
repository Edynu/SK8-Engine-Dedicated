#include "skate3_flash_bridge.h"

#include "generated/skate3_init.h"

#include <atomic>
#include <array>

#include <rex/logging.h>

namespace skate3::flash_bridge {

namespace {

struct Pending {
  std::uint32_t method_id = 0;
  std::array<std::uint32_t, kMaxArguments> arguments{};
  int argument_count = 0;
};

Pending g_pending;
std::atomic<bool> g_armed{false};

}  // namespace

bool RequestCall(std::uint32_t method_id, const std::uint32_t* arguments,
                 int argument_count) {
  if (argument_count < 0 || argument_count > kMaxArguments) {
    return false;
  }
  bool expected = false;
  if (!g_armed.compare_exchange_strong(expected, true,
                                       std::memory_order_acq_rel)) {
    return false;
  }
  // Written after the claim and read after the release below, so the pending
  // slot is only touched by one side at a time.
  g_pending.method_id = method_id;
  g_pending.argument_count = argument_count;
  for (int index = 0; index < argument_count; ++index) {
    g_pending.arguments[index] = arguments[index];
  }
  return true;
}

void ApplyPending(PPCContext& ctx, std::uint8_t* base) {
  if (!g_armed.load(std::memory_order_acquire)) {
    return;
  }
  const Pending call = g_pending;

  // A COPY of the context: the callee clobbers the volatile registers and
  // builds its own frame, while the guest function whose tick this is has not
  // finished with its own. r1 still points at a live guest stack, which is
  // what the callee needs.
  PPCContext invocation = ctx;
  invocation.r3.u32 = call.method_id;
  invocation.r4.u32 = 0;
  invocation.r5.u32 = 0;
  invocation.r6.u32 = static_cast<std::uint32_t>(call.argument_count);
  // r7..r10 in order, matching every call site observed.
  std::uint32_t* const slots[kMaxArguments] = {
      &invocation.r7.u32, &invocation.r8.u32, &invocation.r9.u32,
      &invocation.r10.u32};
  for (int index = 0; index < call.argument_count; ++index) {
    *slots[index] = call.arguments[index];
  }
  REXLOG_INFO("flash-bridge: calling method {} with {} argument(s)",
              call.method_id, call.argument_count);
  sub_825D3B38(invocation, base);
  REXLOG_INFO("flash-bridge: method {} returned", call.method_id);

  // Released only after the call, so a second request cannot overwrite the
  // slot while it is being read.
  g_armed.store(false, std::memory_order_release);
}

}  // namespace skate3::flash_bridge
