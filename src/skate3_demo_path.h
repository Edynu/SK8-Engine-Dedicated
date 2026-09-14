#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <rex/ppc/context.h>

namespace rex::runtime {
class FunctionDispatcher;
}
namespace rex::input {
class InputSystem;
}

namespace skate3::demo_path {

void InstallHooks(rex::runtime::FunctionDispatcher* dispatcher);
// Provider for the merged UI pad state consumed by the user-facing frontend
// movie skip. Set during app setup, before the guest boots (the provider is
// read from the guest thread without further synchronization).
void SetUiInputProvider(std::function<rex::input::InputSystem*()> provider);
// The provider's current input system, or null before app setup. For other
// host features that poll the merged UI pad state.
rex::input::InputSystem* GetUiInputSystem();
bool ShouldForceIntroMovieComplete();
// True from process boot until the game reaches gameplay in direct-boot mode.
// The renderer uses this to replace frontend scenes with its native loading
// frame while profile/save services initialize.
bool DirectBootLoadingVisualActive();
void ObserveFrontEndState(uint32_t manager, uint32_t state_id,
                          uint32_t mode, uint32_t caller_lr);
uint32_t AutomationStage();
uint32_t LastRequestedFrontEndState();

// The front-end manager pointer seen by the SetFrontEndState hook, or 0
// before the front end has ever changed state. Requesting a state without one
// would be a call through a null `this`.
uint32_t LastFrontEndManager();

// How many times retail asked its front end to change state, and the last few
// requests. A screen that is reached WITHOUT this count moving is not driven
// by the state machine at all - which is the difference between "we have the
// wrong number" and "this is the wrong mechanism".
struct FrontEndStateEvent {
  uint32_t state_id = 0;
  uint32_t mode = 0;
  uint32_t manager = 0;
  uint32_t caller_lr = 0;
};
uint64_t FrontEndStateCallCount();
std::vector<FrontEndStateEvent> RecentFrontEndStates();

// Replays a pad sequence into the game, e.g. "start,down,down,a".
//
// WHY A MACRO. Retail's Edit Skater screen is not reached through the
// front-end state machine - measured: sub_82D0AFA0 is called exactly once per
// session, at press-start, and never again while navigating to the editor. So
// there is no state id to request. The screen is driven by the Flash front end
// responding to pad input, and replaying that input is the mechanism that
// actually reaches it.
//
// This is the SAME injection the demo path already uses to switch maps
// (QueueSyntheticInput), at the XAM layer the game reads its pad from - not
// OS-level key faking. It is still a macro, with a macro's fragility: it
// depends on where the cursor starts and how many rows a menu has.
//
// Tokens are comma-separated, with an optional ':ms' suffix overriding the
// delay after that input. Returns false with `error` set for an unknown token
// or a sequence already playing.
bool PlayInputSequence(const std::string& sequence, std::string& error);

bool SeenLanguageUpdate();

}  // namespace skate3::demo_path
