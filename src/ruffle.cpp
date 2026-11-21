#include "ruffle.hpp"
#include <bcrypt.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace fff {
namespace {
std::wstring percent_path(std::wstring_view text) {
    constexpr std::wstring_view hex = L"0123456789ABCDEF";
    std::wstring encoded;
    for (const auto byte : encode_utf8(text)) {
        const auto c = static_cast<unsigned char>(byte);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            encoded.push_back(c);
        } else {
            encoded += {L'%', hex[c >> 4], hex[c & 15]};
        }
    }
    return encoded;
}

std::wstring game_storage(std::wstring_view game) {
    auto bytes = encode_utf8(game);
    if (bytes.empty()) return {};
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    std::array<UCHAR, 32> hash{};
    const auto status = BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(bytes.data()),
                                  static_cast<ULONG>(bytes.size()), hash.data(), static_cast<ULONG>(hash.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) return {};
    constexpr std::wstring_view hex = L"0123456789abcdef";
    std::wstring path = L"data/saves/";
    for (const auto b : hash) path += {hex[b >> 4], hex[b & 15]};
    return path;
}

std::optional<Text> check_swf_header(const fs::path& path) {
    const auto handle = CreateFileW(io_path(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return Text::unreadableSwf;
    const std::unique_ptr<void, decltype(&CloseHandle)> file(handle, CloseHandle);
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE))) {
        return Text::unreadableSwf;
    }
    std::array<BYTE, 8> header{};
    DWORD read = 0;
    if (!ReadFile(handle, header.data(), static_cast<DWORD>(header.size()), &read, nullptr)) return Text::unreadableSwf;
    const auto format = header[0];
    if (read != header.size() || (format != 'F' && format != 'C' && format != 'Z') || header[1] != 'W' || header[2] != 'S') {
        return Text::invalidSwfHeader;
    }
    const auto length = std::uint32_t(header[4]) | (std::uint32_t(header[5]) << 8) |
                        (std::uint32_t(header[6]) << 16) | (std::uint32_t(header[7]) << 24);
    if (length < header.size() || (format == 'F' && !info.nFileSizeHigh && info.nFileSizeLow < length)) {
        return Text::invalidSwfHeader;
    }
    return {};
}
}

std::wstring quote_arg(std::wstring_view argument) {
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const auto c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        result.append(c == L'\"' ? backslashes * 2 + 1 : backslashes, L'\\');
        result += c;
        backslashes = 0;
    }
    result.append(backslashes * 2, L'\\');
    result += L'"';
    return result;
}

LaunchResult play(const fs::path& root, std::wstring_view game, std::wstring_view entry) {
    const auto runtime = safe_existing(root, L"runtime/ruffle.exe", false);
    if (!runtime) return {nullptr, Text::runtimeNotFound};
    if (!relative_path(game) || game.find(L'/') != game.npos || !relative_path(entry) ||
        entry.size() <= 4 || !equal_name(entry.substr(entry.size() - 4), L".swf")) {
        return {nullptr, Text::invalidSwf};
    }
    const auto gameRelative = L"games/" + std::wstring(game);
    const auto directory = safe_existing(root, gameRelative, true);
    const auto swf = safe_existing(root, gameRelative + L"/" + std::wstring(entry), false);
    if (!directory || !swf) return {nullptr, Text::unreadableSwf};
    if (const auto error = check_swf_header(*swf)) return {nullptr, *error};
    const auto executable = io_path(*runtime);
    DWORD binaryType = 0;
    if (!GetBinaryTypeW(executable.c_str(), &binaryType)) return {nullptr, Text::createProcessFailed, GetLastError()};
    if (binaryType != SCS_32BIT_BINARY && binaryType != SCS_64BIT_BINARY) {
        return {nullptr, Text::createProcessFailed, ERROR_BAD_EXE_FORMAT};
    }

    if (!ensure_directory(root, L"data")) return {nullptr, Text::ruffleDirsFailed};
    SECURITY_ATTRIBUTES lockAttributes{sizeof(lockAttributes), nullptr, TRUE};
    const auto lockPath = io_path(root / L"data" / L"ruffle.lock");
    HANDLE lock = CreateFileW(lockPath.c_str(), GENERIC_READ, 0, &lockAttributes, OPEN_ALWAYS,
                              FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (lock == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        return {nullptr, error == ERROR_SHARING_VIOLATION ? Text::alreadyPlaying : Text::ruffleDirsFailed, error};
    }
    const std::unique_ptr<void, decltype(&CloseHandle)> ownedLock(lock, CloseHandle);
    BY_HANDLE_FILE_INFORMATION lockInfo{};
    if (!GetFileInformationByHandle(lock, &lockInfo) ||
        (lockInfo.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        return {nullptr, Text::ruffleDirsFailed};
    }

    const auto storage = game_storage(game);
    if (storage.empty() || !ensure_directory(root, storage) || !ensure_directory(root, L"data/ruffle-config") ||
        !ensure_directory(root, L"data/cache/ruffle/log")) {
        return {nullptr, Text::ruffleDirsFailed};
    }
    const auto preferences = root / L"data\\ruffle-config\\preferences.toml";
    if (GetFileAttributesW(io_path(preferences).c_str()) == INVALID_FILE_ATTRIBUTES &&
        GetLastError() == ERROR_FILE_NOT_FOUND &&
        !atomic_write(preferences, "enable_openh264 = false\nrecent_limit = 0\n")) {
        return {nullptr, Text::ruffleConfigWrite};
    }
    if (!safe_existing(root, L"data/ruffle-config/preferences.toml", false)) {
        return {nullptr, Text::ruffleConfigRead};
    }

    const auto movie = L"file:///FlashFasaFiso/" + percent_path(game) + L"/" + percent_path(entry);
    const auto saves = fs::path(storage).make_preferred();
    const std::wstring arguments[]{
        runtime->native(), L"--storage", L"disk", L"--save-directory", (root / saves).native(),
        L"--config", (root / L"data\\ruffle-config").native(),
        L"--cache-directory", (root / L"data\\cache\\ruffle").native(),
        L"--tcp-connections", L"deny", L"--filesystem-access-mode", L"deny", L"--open-url-mode", L"deny",
        L"--spoof-url", movie, L"--", swf->native()
    };
    std::wstring command;
    for (const auto& argument : arguments) {
        if (!command.empty()) command += L' ';
        command += quote_arg(argument);
    }
    if (command.size() >= 32767) return {nullptr, Text::commandTooLong};
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    if (!attributeBytes) return {nullptr, Text::createProcessFailed, GetLastError()};
    std::vector<std::byte> attributeBuffer(attributeBytes);
    auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes)) {
        return {nullptr, Text::createProcessFailed, GetLastError()};
    }
    const std::unique_ptr<_PROC_THREAD_ATTRIBUTE_LIST, decltype(&DeleteProcThreadAttributeList)>
        ownedAttributes(attributes, DeleteProcThreadAttributeList);
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &lock, sizeof(lock),
                                   nullptr, nullptr)) {
        return {nullptr, Text::createProcessFailed, GetLastError()};
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        EXTENDED_STARTUPINFO_PRESENT, nullptr, runtime->parent_path().c_str(),
                        &startup.StartupInfo, &process)) {
        return {nullptr, Text::createProcessFailed, GetLastError()};
    }
    CloseHandle(process.hThread);
    return {process.hProcess};
}
}
