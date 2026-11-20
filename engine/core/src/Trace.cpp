#include "sr/core/Trace.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <string>

namespace sr::core {

void SetThreadName(const char* name) {
#ifdef SR_TRACY
    tracy::SetThreadName(name);
#endif
#ifdef _WIN32
    // SetThreadDescription exists from Windows 10 1607; look it up so older systems still start.
    using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static const auto setDescription = reinterpret_cast<SetDescription>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")));
    if (setDescription != nullptr) {
        std::wstring wide;
        for (const char* c = name; *c != '\0'; ++c) wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*c)));
        setDescription(GetCurrentThread(), wide.c_str());
    }
#else
    (void)name;
#endif
}

void RaiseThreadPriority() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
}

bool ProfilerConnected() {
#ifdef SR_TRACY
    return TracyIsConnected;
#else
    return false;
#endif
}

void TraceMessage(const char* text, std::size_t size) {
#ifdef SR_TRACY
    TracyMessage(text, size);
#else
    (void)text;
    (void)size;
#endif
}

} // namespace sr::core
