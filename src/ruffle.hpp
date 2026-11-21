#pragma once
#include "library.hpp"

namespace fff {
std::wstring quote_arg(std::wstring_view argument);

struct LaunchResult {
    HANDLE process = nullptr;
    Text error = Text::empty;
    DWORD windowsError = 0;
};
LaunchResult play(const fs::path& root, std::wstring_view game, std::wstring_view entry);
}
