#include "window.hpp"
#include "library.hpp"
#include <objbase.h>

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int show) {
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(initialized)) return 1;
    int result = 1;
    try {
        result = fff::run_window(instance, show);
    } catch (...) {
        auto language = fff::defaultLanguage;
        try {
            if (const auto data = fff::safe_existing(fff::executable_directory(), L"data", true)) {
                language = fff::load_state(*data).preferences.language;
            }
        } catch (...) {
        }
        fff::localized_message(nullptr, language, fff::Text::startFailed, fff::Text::empty, IDI_ERROR);
    }
    CoUninitialize();
    return result;
}
