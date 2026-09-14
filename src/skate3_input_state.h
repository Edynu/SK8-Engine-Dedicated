#pragma once

// Control state for scripts: IsControlPressed and friends.
//
// Two things make this more than a passthrough to the input system.
//
// First, EDGES. A script asks "was this just pressed?", and it asks once per
// script tick - but input is sampled on the UI thread at frame rate, which
// is not the same clock. Edges are therefore accumulated between script
// ticks rather than being read live, so a press that happened between two
// script ticks is still reported exactly once and never twice. Reading
// `held` live instead would silently lose short presses.
//
// Second, GATING. Reads come from the raw pad and from ImGui's own keyboard
// state, both of which stay live while the gameplay input gate is shut (a
// menu, the F7 console, NUI focus). That is deliberate for the pad - a
// marker you hold a direction on must work regardless - but it means a
// script polling controls while the player is typing in the console will
// see those keystrokes. ScriptsShouldReadKeyboard() reports whether the
// keyboard currently belongs to gameplay, and the keyboard natives honour
// it so typing "restart Zones" in the console cannot also drive a game mode.
//
// Third, the D-PAD comes from two places and neither alone is enough.
// GetUiGamepadState reports only real controllers: the SDK's keyboard/mouse
// driver deliberately excludes itself from that path (see its GetStateUi -
// "exposing the emulated pad here would double-apply every keypress in
// overlay navigation"), so a keyboard player pressing the arrow key BOUND to
// D-pad up produced nothing at all for scripts, while the game itself saw
// the press. The sampler therefore resolves the same keybind cvars the
// keyboard driver uses and reports the D-pad as "the real pad's bit, OR the
// bound key". The keyboard half is gated, the pad half is not.
//
// The left stick is exposed as its own set of directional controls
// (LSTICKUP and friends) rather than being folded into the D-pad names. A
// game mode that wants "hold up at the marker" should be able to mean the
// D-pad specifically - steering a skateboard constantly pushes the stick,
// and silently counting that as a D-pad press would fire markers by
// accident.

#include <cstdint>
#include <string_view>

namespace skate3::input_state {

// Every control a script can ask about. Pad first, then keyboard; the
// numbering is internal and never crosses into Lua, which uses names.
enum class Control : int {
  kNone = 0,

  // Gamepad buttons
  kPadA, kPadB, kPadX, kPadY,
  kPadLeftShoulder, kPadRightShoulder,
  kPadBack, kPadStart,
  kPadLeftThumb, kPadRightThumb,
  kPadDpadUp, kPadDpadDown, kPadDpadLeft, kPadDpadRight,
  // Analog, reported as pressed past a threshold and readable as a value.
  kPadLeftTrigger, kPadRightTrigger,
  // Left stick as four digital directions, deliberately separate from the
  // D-pad - see the note above.
  kPadLeftStickUp, kPadLeftStickDown, kPadLeftStickLeft, kPadLeftStickRight,

  // Keyboard
  kKeySpace, kKeyEnter, kKeyEscape, kKeyTab,
  kKeyShift, kKeyControl, kKeyAlt,
  kKeyUp, kKeyDown, kKeyLeft, kKeyRight,
  kKeyA, kKeyB, kKeyC, kKeyD, kKeyE, kKeyF, kKeyG, kKeyH, kKeyI, kKeyJ,
  kKeyK, kKeyL, kKeyM, kKeyN, kKeyO, kKeyP, kKeyQ, kKeyR, kKeyS, kKeyT,
  kKeyU, kKeyV, kKeyW, kKeyX, kKeyY, kKeyZ,
  kKey0, kKey1, kKey2, kKey3, kKey4, kKey5, kKey6, kKey7, kKey8, kKey9,

  kCount,
};

// Analog axes, read through GetControlValue rather than as buttons.
enum class Axis : int {
  kNone = 0,
  kLeftStickX, kLeftStickY,
  kRightStickX, kRightStickY,
  kLeftTrigger, kRightTrigger,
  kCount,
};

// Name -> control, case-insensitive. Returns kNone for anything unknown, so
// a typo in a script surfaces as "never pressed" rather than as a crash.
Control ControlFromName(std::string_view name);
Axis AxisFromName(std::string_view name);

// One frame of raw input, published from the UI thread.
struct Sample {
  bool pad_connected = false;
  uint16_t pad_buttons = 0;
  int16_t thumb_lx = 0, thumb_ly = 0, thumb_rx = 0, thumb_ry = 0;
  uint8_t left_trigger = 0, right_trigger = 0;
  // Indexed by Control; only the keyboard range is read.
  bool keys[static_cast<int>(Control::kCount)] = {};
  // False while something other than gameplay owns the keyboard.
  bool keyboard_belongs_to_gameplay = true;
  // The keys currently bound to each D-pad direction, in the order
  // up/down/left/right. Resolved by the sampler from the same keybind cvars
  // the SDK's keyboard driver reads, so rebinding follows. OR'd into the
  // D-pad controls, and only while the keyboard belongs to gameplay.
  bool keyboard_dpad[4] = {};
};

// Called once per frame from the UI thread with freshly polled input.
void Publish(const Sample& sample);

// Called once per script tick, BEFORE any script runs, to hand that tick
// the edges accumulated since the previous one.
void BeginScriptFrame();

bool IsPressed(Control control);
bool IsJustPressed(Control control);
bool IsJustReleased(Control control);
float Value(Axis axis);
bool PadConnected();
bool ScriptsShouldReadKeyboard();

}  // namespace skate3::input_state
