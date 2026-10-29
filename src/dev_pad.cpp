#include "dev_pad.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "config.h"
#include "log.h"
#include "dinput_hook.h"
#include "vrkit_input.h"
#include "vrkit_options.h"

namespace dev_pad {
namespace {

unsigned short ButtonBit(const char* n) {
    struct { const char* name; unsigned short bit; } k[] = {
        {"a", 0x1000}, {"b", 0x2000}, {"x", 0x4000}, {"y", 0x8000}, {"start", 0x0010}, {"back", 0x0020},
        {"lb", 0x0100}, {"rb", 0x0200}, {"l3", 0x0040}, {"r3", 0x0080},
        {"up", 0x0001}, {"down", 0x0002}, {"left", 0x0004}, {"right", 0x0008}};
    for (auto& e : k)
        if (!_stricmp(n, e.name)) return e.bit;
    return 0;
}

unsigned char KeyCode(const char* n) {  // DirectInput scan codes (DIK_*)
    struct { const char* name; unsigned char dik; } k[] = {
        {"enter", 0x1C}, {"esc", 0x01}, {"escape", 0x01}, {"space", 0x39}, {"tab", 0x0F}, {"backspace", 0x0E},
        {"kup", 0xC8}, {"kdown", 0xD0}, {"kleft", 0xCB}, {"kright", 0xCD}, {"e", 0x12}, {"f", 0x21}, {"q", 0x10}};
    for (auto& e : k)
        if (!_stricmp(n, e.name)) return e.dik;
    return 0;
}

DWORD WINAPI Watch(LPVOID) {
    const std::wstring trigger = vr::g_gameDir + L"\\ff16vr_pad.trigger";
    for (;;) {
        Sleep(100);
        if (GetFileAttributesW(trigger.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        char buf[128] = {};
        if (FILE* f = _wfopen(trigger.c_str(), L"r")) {
            fgets(buf, sizeof(buf), f);
            fclose(f);
        }
        DeleteFileW(trigger.c_str());
        unsigned short bits = 0;
        unsigned char keys[4];
        int nKeys = 0;
        int hold = 150;
        int stick[4] = {0, 0, 0, 0};
        bool sticks = false;
        char* ctx = nullptr;
        for (char* t = strtok_s(buf, " \t\r\n,", &ctx); t; t = strtok_s(nullptr, " \t\r\n,", &ctx)) {
            if (t[0] >= '0' && t[0] <= '9') hold = atoi(t);
            else if ((t[0] == 'l' || t[0] == 'r' || t[0] == 'L' || t[0] == 'R') && (t[1] == 'x' || t[1] == 'y' || t[1] == 'X' || t[1] == 'Y') && t[2] == '=') {
                const int i = ((t[0] | 32) == 'r' ? 2 : 0) + ((t[1] | 32) == 'y' ? 1 : 0);  // lx=.. ly=.. rx=.. ry=.. (-100..100 %)
                int v = atoi(t + 3) * 32767 / 100;
                stick[i] = v < -32767 ? -32767 : v > 32767 ? 32767 : v;
                sticks = true;
            }
            else if (unsigned char k = KeyCode(t)) { if (nKeys < 4) keys[nKeys++] = k; }
            else bits |= ButtonBit(t);
        }
        vrkit::input::InstallHooks();
        if (bits) vrkit::input::Inject(bits, hold);
        if (sticks) vrkit::input::InjectSticks((short)stick[0], (short)stick[1], (short)stick[2], (short)stick[3], hold);
        // DirectInput: keys, and the pad's buttons as joystick buttons (Xbox pad under DirectInput: A=0 B=1 X=2 Y=3
        // LB=4 RB=5 Back=6 Start=7 L3=8 R3=9)
        unsigned joy = 0;
        const unsigned short xb[10] = {0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200, 0x0020, 0x0010, 0x0040, 0x0080};
        for (int i = 0; i < 10; ++i)
            if (bits & xb[i]) joy |= 1u << i;
        if (nKeys || joy) dinput_hook::Inject(keys, nKeys, joy, hold);
        long calls = 0, ok = 0;
        vrkit::input::GameCallCounts(&calls, &ok);
        DWORD fgPid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &fgPid);
        LOG("Pad trigger: buttons 0x%04X for %d ms (game XInput calls so far: %ld, with a pad: %ld; game window %s)", bits,
            hold, calls, ok, fgPid == GetCurrentProcessId() ? "in front" : "NOT in front");
    }
}

}  // namespace

void Install() {
    vrkit::SetLog([](const char* m) { LOG("%s", m); });
    vrkit::input::InstallHooks();
    HANDLE t = CreateThread(nullptr, 0, Watch, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

}  // namespace dev_pad
