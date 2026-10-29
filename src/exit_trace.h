#pragma once

namespace exittrace {

// Hook ExitProcess / TerminateProcess in ffxvi.exe's imports and log a stack trace when called.
void Install();

}  // namespace exittrace
