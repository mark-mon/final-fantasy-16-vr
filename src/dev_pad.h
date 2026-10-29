#pragma once

// Dev / automated testing: <game>\ff16vr_pad.trigger makes the game see gamepad buttons (through the vrkit XInput
// import hook, so it works whichever window has focus). Content: button names separated by spaces, optionally a
// hold time in ms, e.g. "A", "Start", "Down 200", "A Enter". Pad: A B X Y Start Back LB RB L3 R3 Up Down Left Right
// (XInput + DirectInput joystick). Keyboard (DirectInput): Enter Esc Space Tab Backspace KUp KDown KLeft KRight E F Q.
namespace dev_pad {
void Install();  // hooks the game's XInput imports (vrkit) and starts the trigger watcher
}
