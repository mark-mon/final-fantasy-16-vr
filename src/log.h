#pragma once

namespace vr {
void LogInit(const wchar_t* path);
const char* WallClock();   // "HH:MM:SS" local time
void Log(const char* fmt, ...);
}

#define LOG(...) ::vr::Log(__VA_ARGS__)
