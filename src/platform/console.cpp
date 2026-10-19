#include "aiagent/platform/console.hpp"

#include <csignal>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>

namespace {

void (*g_stop_handler)(int) = nullptr;

BOOL WINAPI ConsoleControlHandler(DWORD control_type) {
    switch (control_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            if (g_stop_handler != nullptr) {
                g_stop_handler(0);
            }
            return TRUE;
        default:
            return FALSE;
    }
}

}  // namespace
#endif

namespace aiagent::platform {

void InstallStopHandler(void (*handler)(int)) {
#ifdef _WIN32
    g_stop_handler = handler;
    SetConsoleCtrlHandler(ConsoleControlHandler, TRUE);
#else
    std::signal(SIGINT, handler);
    std::signal(SIGTERM, handler);
#endif
}

bool EnableVirtualTerminal() {
#ifdef _WIN32
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == nullptr || output == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD mode = 0;
    if (!GetConsoleMode(output, &mode)) {
        return false;
    }
    if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0) {
        return true;
    }
    return SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
    return true;
#endif
}

void ConfigureConsoleUtf8() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
}

}  // namespace aiagent::platform
