#include "library.hpp"
#include "window.hpp"
#include <objbase.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

int main() {
    if (!fff::safe_existing(fff::executable_directory(), L"fff-dpi-test.marker", false)) {
        std::cerr << "Run this test in an isolated fixture containing fff-dpi-test.marker.\n";
        return 1;
    }
    std::atomic_int uiResult{-1};
    std::jthread ui([&] {
        const auto result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (FAILED(result)) { uiResult = 1; return; }
        try {
            uiResult = fff::run_window(GetModuleHandleW(nullptr), SW_SHOWNORMAL);
        } catch (...) {
            uiResult = 1;
        }
        CoUninitialize();
    });

    HWND window = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!window && uiResult == -1 && std::chrono::steady_clock::now() < deadline) {
        HWND candidate = nullptr;
        while ((candidate = FindWindowExW(nullptr, candidate, L"FlashFasaFiso.Window", nullptr))) {
            DWORD process = 0;
            GetWindowThreadProcessId(candidate, &process);
            if (process == GetCurrentProcessId() && IsWindowVisible(candidate)) {
                window = candidate;
                break;
            }
        }
        if (!window) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!window) {
        std::cerr << "Could not find the test launcher window.\n";
        if (uiResult == -1) ExitProcess(1);
        return 1;
    }

    bool allPassed = true;
    for (const int dpi : {96, 144}) {
        const auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
        const RECT proposed{60, 60, 60 + px(760), 60 + px(480)};
        SendMessageW(window, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&proposed));
        MINMAXINFO limits{};
        SendMessageW(window, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&limits));
        RECT client{};
        GetClientRect(window, &client);
        const auto hit = [&](int x, int y) {
            POINT point{px(x), px(y)};
            ClientToScreen(window, &point);
            return SendMessageW(window, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y));
        };
        const bool passed = limits.ptMinTrackSize.x == px(760) && limits.ptMinTrackSize.y == px(480) &&
            client.right == px(760) && client.bottom == px(480) &&
            hit(200, 22) == HTCAPTION && hit(760 - 69, 22) == HTMAXBUTTON &&
            hit(2, 25) == HTLEFT && hit(760 - 23, 22) == HTCLIENT;
        std::cout << dpi << " DPI synthetic WM_DPICHANGED: " << client.right << 'x' << client.bottom
                  << ", caption/snap/resize/close hit tests " << (passed ? "passed" : "failed") << '\n';
        allPassed = allPassed && passed;
    }

    for (int tab = 0; tab < 3; ++tab) SendMessageW(window, WM_KEYDOWN, VK_TAB, 0);
    SendMessageW(window, WM_KEYDOWN, VK_RETURN, 0);
    const bool maximized = IsZoomed(window) != FALSE;
    if (maximized) SendMessageW(window, WM_KEYDOWN, VK_SPACE, 0);
    const bool captionKeysPassed = maximized && !IsZoomed(window);
    std::cout << "Caption keyboard maximize/restore " << (captionKeysPassed ? "passed" : "failed") << '\n';
    allPassed = allPassed && captionKeysPassed;

    PostMessageW(window, WM_CLOSE, 0, 0);
    ui.join();
    return allPassed && uiResult == 0 ? 0 : 1;
}
