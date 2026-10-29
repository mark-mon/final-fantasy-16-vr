#pragma once
#include <windows.h>

// DirectInput 8 hooks on the game's own DirectInput objects (this DLL is the dinput8 proxy, so every object the game
// creates passes through DirectInput8Create). Used to
//  - find out which input path the game really reads (logs each device's first GetDeviceState / GetDeviceData),
//  - inject key / button presses for automated tests (dev_pad) independent of window focus tricks.
namespace dinput_hook {

// Wrap a freshly created IDirectInput8 (A or W): hooks CreateDevice, and through it the devices' state calls.
void OnCreated(void* directInput8);

// Inject for holdMs: keyboard scan codes (DIK_*, up to 4) and joystick buttons (bit i = rgbButtons[i]).
void Inject(const unsigned char* dikCodes, int nCodes, unsigned buttonBits, int holdMs);

}  // namespace dinput_hook
