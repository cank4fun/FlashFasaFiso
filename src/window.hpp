#pragma once
#include <windows.h>
#include "localization.hpp"

namespace fff {
int run_window(HINSTANCE instance, int show);
int localized_message(HWND owner, Language language, Text message, Text title, PCWSTR icon, bool confirm = false);
}
