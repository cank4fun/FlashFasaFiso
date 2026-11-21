#include "library.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cwctype>
#include <limits>
#include <stdexcept>

namespace fff {
namespace {
constexpr DWORD badAttributes = INVALID_FILE_ATTRIBUTES;
constexpr std::size_t maxGames = 50000, maxEntries = 1024;
constexpr DWORD metadataLimit = 32 * 1024, dataLimit = 32 * 1024 * 1024;
constexpr std::wstring_view catalog_header(bool nested) {
    return nested ? L"FFF catalog 1" : L"FFF catalog 2 flat";
}
constexpr auto favoritesHeader = L"FFF favorites 1";
constexpr auto entriesHeader = L"FFF entries 1";
constexpr auto settingsHeader = L"FFF settings 2";
constexpr std::pair<std::wstring_view, bool Preferences::*> settingFlags[]{
    {L"auto_scan", &Preferences::autoScan},
    {L"remember_tab", &Preferences::rememberTab},
    {L"confirm_close", &Preferences::confirmClose},
    {L"scan_nested", &Preferences::scanNested},
    {L"remember_swf", &Preferences::rememberSwf}
};
constexpr std::wstring_view pageNames[]{L"library", L"favorites", L"settings"};

DWORD attributes(const fs::path& path) {
    return GetFileAttributesW(io_path(path).c_str());
}

bool ordinary(DWORD attr, bool directory) {
    return attr != badAttributes && !(attr & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) &&
           bool(attr & FILE_ATTRIBUTE_DIRECTORY) == directory;
}

struct TextFile {
    std::optional<std::wstring> value;
    StateFileStatus status = StateFileStatus::unreadable;
};

TextFile unavailable_text(DWORD error) {
    return {{}, error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                    ? StateFileStatus::missing : StateFileStatus::unreadable};
}

bool can_save(StateFileStatus status) {
    return status != StateFileStatus::unreadable && status != StateFileStatus::malformed;
}

TextFile read_text(const fs::path& dir, std::wstring_view leaf, DWORD maximum) {
    const auto parent = attributes(dir);
    if (parent == badAttributes) return unavailable_text(GetLastError());
    if (!ordinary(parent, true)) return {};
    const auto path = dir / fs::path(leaf);
    const auto attr = attributes(path);
    if (attr == badAttributes) return unavailable_text(GetLastError());
    if (!ordinary(attr, false)) return {};
    auto file = CreateFileW(io_path(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info) || !ordinary(info.dwFileAttributes, false)) {
        CloseHandle(file);
        return {};
    }
    if (info.nFileSizeHigh != 0 || info.nFileSizeLow > maximum) {
        CloseHandle(file);
        return {{}, StateFileStatus::malformed};
    }
    std::string bytes(info.nFileSizeLow, '\0');
    DWORD read = 0;
    const auto ok = ReadFile(file, bytes.data(), info.nFileSizeLow, &read, nullptr);
    CloseHandle(file);
    if (!ok || read != bytes.size()) return {};
    if (bytes.starts_with("\xef\xbb\xbf")) bytes.erase(0, 3);
    auto value = decode_utf8(bytes);
    const auto status = value ? StateFileStatus::loaded : StateFileStatus::malformed;
    return {std::move(value), status};
}

std::wstring_view trim(std::wstring_view s) {
    constexpr std::wstring_view spaces = L" \r\t";
    const auto first = s.find_first_not_of(spaces);
    return first == s.npos ? std::wstring_view{} : s.substr(first, s.find_last_not_of(spaces) - first + 1);
}

std::wstring_view take(std::wstring_view& rest, wchar_t delimiter) {
    const auto index = rest.find(delimiter);
    const auto item = rest.substr(0, index);
    rest = index == rest.npos ? std::wstring_view{} : rest.substr(index + 1);
    return item;
}

bool swf(std::wstring_view path) {
    return path.size() > 4 && equal_name(path.substr(path.size() - 4), L".swf");
}

bool game_id(std::wstring_view s) {
    return relative_path(s) && s.find(L'/') == s.npos;
}

void index(Game& game) {
    game.searchKey = fold_search(game.title + L"\n" + game.id + L"\n" + game.genre);
}

bool display_text(std::wstring_view s, std::size_t maximum) {
    return s.size() <= maximum && std::ranges::none_of(s, [](wchar_t c) { return c < 32 || c == 127; });
}

void read_metadata(const fs::path& directory, Game& game, ScanResult& report) {
    auto source = read_text(directory, L"game.ini", metadataLimit);
    if (!source.value) {
        if (source.status != StateFileStatus::missing) ++report.badMetadata;
        return;
    }
    bool malformed = false;
    std::wstring_view contents = *source.value;
    while (!contents.empty()) {
        auto row = trim(take(contents, L'\n'));
        if (row.empty() || row.front() == L';' || row.front() == L'#' ||
            (row.starts_with(L"[") && row.ends_with(L"]"))) continue;
        if (row.find(L'=') == row.npos) {
            malformed = true;
            continue;
        }
        const auto key = trim(take(row, L'='));
        const auto value = trim(row);
        const auto set_field = [&](std::wstring& field, std::size_t maximum) {
            if (display_text(value, maximum)) field = value;
            else malformed = true;
        };
        if (equal_name(key, L"title")) {
            if (!value.empty()) set_field(game.title, 250);
        } else if (equal_name(key, L"genre")) {
            set_field(game.genre, 120);
        } else if (equal_name(key, L"description")) {
            set_field(game.description, 1000);
        } else if (equal_name(key, L"entry")) {
            if (relative_path(value) && swf(value) && safe_existing(directory, value, false)) game.metadataEntry = value;
            else if (!value.empty()) malformed = true;
        }
    }
    if (malformed) ++report.badMetadata;
}

template<class Visitor>
bool enumerate(const fs::path& directory, std::stop_token stop, Visitor visit) {
    WIN32_FIND_DATAW item{};
    const auto pattern = io_path(directory) / L"*";
    const auto handle = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &item, FindExSearchNameMatch, nullptr, 0);
    if (handle == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    struct SearchHandle {
        HANDLE value;
        ~SearchHandle() { FindClose(value); }
    } guard{handle};
    do {
        if (stop.stop_requested()) return false;
        const std::wstring_view name = item.cFileName;
        if (game_id(name) && !(item.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            !visit(name, item.dwFileAttributes)) return false;
    } while (FindNextFileW(handle, &item));
    return GetLastError() == ERROR_NO_MORE_FILES;
}

struct CollectionReport {
    bool failed = false;
    bool limited = false;
};

CollectionReport collect(const fs::path& directory, std::wstring_view prefix, Game& game, unsigned depth,
                         std::stop_token stop) {
    CollectionReport report;
    const bool listed = enumerate(directory, stop, [&](std::wstring_view name, DWORD attr) {
        if (ordinary(attr, false) && swf(name)) {
            if (game.entries.size() >= maxEntries) {
                report.limited = true;
                return false;
            }
            game.entries.push_back(std::wstring(prefix) + std::wstring(name));
        } else if (depth && ordinary(attr, true)) {
            const auto next = collect(directory / fs::path(name), std::wstring(prefix) + std::wstring(name) + L"/",
                                      game, depth - 1, stop);
            report.failed |= next.failed;
            report.limited |= next.limited;
            if (report.limited) return false;
        }
        return true;
    });
    report.failed |= !listed && !report.limited;
    return report;
}

bool has_entry(const Game& game, std::wstring_view entry) {
    return std::ranges::any_of(game.entries, [&](const auto& item) { return equal_name(item, entry); });
}

void ordered_entries(Game& game) {
    std::ranges::sort(game.entries, OrdinalLess{});
    const auto tail = std::ranges::unique(game.entries, equal_name);
    game.entries.erase(tail.begin(), tail.end());
}

bool valid_cover(std::wstring_view name) {
    constexpr std::array<std::wstring_view, 4> covers{L"cover.png", L"cover.jpg", L"cover.jpeg", L"cover.webp"};
    return std::ranges::any_of(covers, [&](auto candidate) { return equal_name(candidate, name); });
}

std::wstring normalize_for_search(std::wstring_view text) {
    const auto sourceLength = static_cast<int>(text.size());
    const auto estimate = NormalizeString(NormalizationC, text.data(), sourceLength, nullptr, 0);
    if (estimate <= 0) return std::wstring(text);
    std::wstring normalized(static_cast<std::size_t>(estimate), L'\0');
    auto length = NormalizeString(NormalizationC, text.data(), sourceLength, normalized.data(), estimate);
    if (length < 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        normalized.resize(static_cast<std::size_t>(-length));
        length = NormalizeString(NormalizationC, text.data(), sourceLength, normalized.data(), -length);
    }
    if (length <= 0) return std::wstring(text);
    normalized.resize(static_cast<std::size_t>(length));
    return normalized;
}
}

bool OrdinalLess::operator()(std::wstring_view a, std::wstring_view b) const {
    if (a.empty() || b.empty()) return a.empty() && !b.empty();
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE)
           == CSTR_LESS_THAN;
}

bool equal_name(std::wstring_view a, std::wstring_view b) {
    if (a.empty() || b.empty()) return a.empty() && b.empty();
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE)
           == CSTR_EQUAL;
}

std::wstring fold_search(std::wstring_view text) {
    if (text.empty()) return {};
    auto prepared = normalize_for_search(text);
    for (auto& c : prepared) if (c == L'İ' || c == L'ı') c = L'i';
    const auto size = static_cast<int>(prepared.size());
    const auto length = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, prepared.data(), size,
                                      nullptr, 0, nullptr, nullptr, 0);
    if (!length) return prepared;
    std::wstring folded(static_cast<std::size_t>(length), L'\0');
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, prepared.data(), size,
                       folded.data(), length, nullptr, nullptr, 0)) return prepared;
    return folded;
}

std::optional<std::wstring> decode_utf8(std::string_view text) {
    if (text.empty()) return std::wstring{};
    if (text.size() > INT_MAX || text.find('\0') != text.npos) return {};
    const auto size = static_cast<int>(text.size());
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, nullptr, 0);
    if (!length) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, result.data(), length)) return {};
    return result;
}

std::string encode_utf8(std::wstring_view text) {
    if (text.empty() || text.size() > INT_MAX) return {};
    const auto size = static_cast<int>(text.size());
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), size, nullptr, 0, nullptr, nullptr);
    if (!length) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), size, result.data(), length, nullptr, nullptr)) return {};
    return result;
}

bool relative_path(std::wstring_view path) {
    if (path.empty() || path.size() > 4096 || path.ends_with(L'/')) return false;
    while (!path.empty()) {
        const auto component = take(path, L'/');
        if (component.empty() || component.size() > 255 || component.ends_with(L'.') || component.ends_with(L' ') ||
            component.find_first_of(L"\\:<>\"|?*") != component.npos || !display_text(component, 255)) return false;
        const auto base = trim(component.substr(0, component.find(L'.')));
        if (equal_name(base, L"CON") || equal_name(base, L"PRN") || equal_name(base, L"AUX") ||
            equal_name(base, L"NUL") || equal_name(base, L"CONIN$") || equal_name(base, L"CONOUT$")) return false;
        if (base.size() == 4 && (equal_name(base.substr(0, 3), L"COM") || equal_name(base.substr(0, 3), L"LPT")) &&
            std::wstring_view(L"123456789¹²³").find(base.back()) != base.npos) return false;
    }
    return true;
}

fs::path io_path(const fs::path& path) {
    const auto& value = path.native();
    if (value.starts_with(L"\\\\?\\") || !path.is_absolute()) return path;
    if (value.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + value.substr(2);
    return L"\\\\?\\" + value;
}

std::optional<fs::path> safe_existing(const fs::path& root, std::wstring_view relative, bool directory) {
    if (!relative_path(relative) || !ordinary(attributes(root), true)) return {};
    auto path = root;
    while (!relative.empty()) {
        path /= fs::path(take(relative, L'/'));
        if (!ordinary(attributes(path), directory || !relative.empty())) return {};
    }
    return path;
}

bool ensure_directory(const fs::path& root, std::wstring_view relative) {
    if (!relative_path(relative) || !ordinary(attributes(root), true)) return false;
    auto path = root;
    while (!relative.empty()) {
        path /= fs::path(take(relative, L'/'));
        if (CreateDirectoryW(io_path(path).c_str(), nullptr)) continue;
        if (!ordinary(attributes(path), true)) return false;
    }
    return true;
}

bool atomic_write(const fs::path& target, std::string_view contents) {
    if (contents.size() > dataLimit || !ordinary(attributes(target.parent_path()), true)) return false;
    const auto existing = attributes(target);
    if (existing != badAttributes && !ordinary(existing, false)) return false;
    static std::atomic_uint sequence = 0;
    const auto temp = target.parent_path() /
                      (L".fff-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                       std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence) + L".tmp");
    const auto tempPath = io_path(temp);
    const auto file = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const auto size = static_cast<DWORD>(contents.size());
    const bool ready = WriteFile(file, contents.data(), size, &written, nullptr) && written == size && FlushFileBuffers(file);
    CloseHandle(file);
    if (ready && MoveFileExW(tempPath.c_str(), io_path(target).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    DeleteFileW(tempPath.c_str());
    return false;
}

fs::path executable_directory() {
    std::wstring buffer(32768, L'\0');
    const auto count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!count || count >= buffer.size()) throw std::runtime_error("GetModuleFileNameW failed");
    buffer.resize(count);
    return fs::path(buffer).parent_path();
}

ScanResult scan(const fs::path& games, bool nested, std::stop_token stop) {
    ScanResult report;
    if (!ordinary(attributes(games), true)) {
        report.complete = false;
        report.errors = 1;
        return report;
    }
    bool gameLimit = false;
    const auto listed = enumerate(games, stop, [&](std::wstring_view id, DWORD attr) {
        if (!ordinary(attr, true)) return true;
        Game game;
        game.id = id;
        game.title = id;
        const auto directory = games / fs::path(id);
        auto candidates = collect(directory, L"", game, 0, stop);
        if (candidates.failed) {
            ++report.errors;
            return !stop.stop_requested();
        }
        read_metadata(directory, game, report);
        if (nested && game.entries.empty()) candidates = collect(directory, L"", game, 2, stop);
        if (candidates.failed) ++report.errors;
        if (game.entries.empty() && game.metadataEntry.empty()) return !stop.stop_requested();
        if (report.games.size() >= maxGames) {
            gameLimit = true;
            return false;
        }
        if (!game.metadataEntry.empty() && !has_entry(game, game.metadataEntry)) {
            if (game.entries.size() >= maxEntries) {
                candidates.limited = true;
                game.entries.back() = game.metadataEntry;
            } else {
                game.entries.push_back(game.metadataEntry);
            }
        }
        report.limited |= candidates.limited;
        ordered_entries(game);
        for (auto cover : {L"cover.png", L"cover.jpg", L"cover.jpeg", L"cover.webp"}) {
            if (safe_existing(directory, cover, false)) {
                game.cover = cover;
                break;
            }
        }
        index(game);
        report.games.push_back(std::move(game));
        return !stop.stop_requested();
    });
    if (!listed && !gameLimit) ++report.errors;
    report.limited |= gameLimit;
    report.complete = report.errors == 0 && !report.limited;
    std::ranges::sort(report.games, [](const Game& a, const Game& b) {
        return OrdinalLess{}(equal_name(a.title, b.title) ? a.id : a.title,
                             equal_name(a.title, b.title) ? b.id : b.title);
    });
    return report;
}

const Game* find_game(const std::vector<Game>& games, std::wstring_view id) {
    const auto match = std::ranges::find_if(games, [&](const Game& game) { return equal_name(game.id, id); });
    return match == games.end() ? nullptr : &*match;
}

std::wstring choose_entry(const Game& game, std::wstring_view remembered) {
    for (const auto candidate : {remembered, std::wstring_view(game.metadataEntry)}) {
        const auto match = std::ranges::find_if(game.entries, [&](const auto& item) { return equal_name(item, candidate); });
        if (match != game.entries.end()) return *match;
    }
    return game.entries.size() == 1 ? game.entries.front() : std::wstring{};
}

bool search_match(const Game& game, std::wstring_view foldedQuery) {
    return game.searchKey.find(foldedQuery) != std::wstring::npos;
}

State load_state(const fs::path& data) {
    State state;
    for (const bool favorites : {true, false}) {
        const auto file = read_text(data, favorites ? L"favorites.ini" : L"library.ini", dataLimit);
        auto& status = favorites ? state.favoritesFile : state.entriesFile;
        status = file.status;
        if (!file.value) {
            state.damaged |= !can_save(status);
            continue;
        }
        std::wstring_view rows = *file.value;
        if (trim(take(rows, L'\n')) != (favorites ? favoritesHeader : entriesHeader)) {
            status = StateFileStatus::malformed;
            state.damaged = true;
            continue;
        }
        while (!rows.empty()) {
            auto row = take(rows, L'\n');
            if (row.ends_with(L'\r')) row.remove_suffix(1);
            if (row.empty()) continue;
            const auto id = take(row, L'\t');
            if (!game_id(id) || (favorites ? !row.empty() : !relative_path(row) || !swf(row))) {
                status = StateFileStatus::malformed;
                state.damaged = true;
                continue;
            }
            if (favorites) state.favorites.emplace(id);
            else state.entries.insert_or_assign(std::wstring(id), std::wstring(row));
        }
    }
    const auto settings = read_text(data, L"settings.ini", metadataLimit);
    state.settingsFile = settings.status;
    state.damaged |= !can_save(settings.status);
    if (settings.value) {
        const auto malformed = [&] {
            state.settingsFile = StateFileStatus::malformed;
            state.damaged = true;
        };
        std::wstring_view rows = *settings.value;
        const auto header = trim(take(rows, L'\n'));
        const bool legacy = header == L"FFF settings 1";
        if (!legacy && header != settingsHeader) {
            malformed();
            return state;
        }
        while (!rows.empty()) {
            auto row = take(rows, L'\n');
            if (row.ends_with(L'\r')) row.remove_suffix(1);
            if (trim(row).empty()) continue;
            if (row.find(L'=') == row.npos) {
                malformed();
                continue;
            }
            const auto key = trim(take(row, L'='));
            if (key == L"selection") {
                if (row.empty() || game_id(row)) state.selection = row;
                else malformed();
                continue;
            }
            if (legacy) continue;
            const auto value = trim(row);
            if (key == L"language") {
                if (value == L"tr" || value == L"en") state.preferences.language = value == L"en" ? Language::en : Language::tr;
                else malformed();
            } else if (key == L"last_tab") {
                const auto page = std::ranges::find(pageNames, value);
                if (page != std::end(pageNames)) state.preferences.lastPage = static_cast<Page>(page - std::begin(pageNames));
                else malformed();
            } else {
                for (const auto& [name, flag] : settingFlags) {
                    if (key != name) continue;
                    if (value == L"0" || value == L"1") state.preferences.*flag = value == L"1";
                    else malformed();
                    break;
                }
            }
        }
    }
    return state;
}

bool save_favorites(const fs::path& data, const State& state) {
    if (!can_save(state.favoritesFile)) return false;
    std::wstring rows = std::wstring(favoritesHeader) + L"\n";
    for (const auto& id : state.favorites) rows += id + L"\n";
    const auto bytes = encode_utf8(rows);
    return !bytes.empty() && atomic_write(data / L"favorites.ini", bytes);
}

bool save_entries(const fs::path& data, const State& state) {
    if (!can_save(state.entriesFile)) return false;
    std::wstring rows = std::wstring(entriesHeader) + L"\n";
    for (const auto& [id, entry] : state.entries) rows += id + L"\t" + entry + L"\n";
    const auto bytes = encode_utf8(rows);
    return !bytes.empty() && atomic_write(data / L"library.ini", bytes);
}

bool save_settings(const fs::path& data, const State& state) {
    if (!can_save(state.settingsFile)) return false;
    const auto& prefs = state.preferences;
    std::wstring rows = std::wstring(settingsHeader) + L"\nselection=" + state.selection + L"\nlanguage=";
    rows += prefs.language == Language::en ? L"en\n" : L"tr\n";
    rows += L"last_tab=";
    rows += pageNames[static_cast<unsigned>(prefs.lastPage)];
    rows += L'\n';
    for (const auto& [name, flag] : settingFlags) {
        rows += name;
        rows += prefs.*flag ? L"=1\n" : L"=0\n";
    }
    const auto bytes = encode_utf8(rows);
    return !bytes.empty() && atomic_write(data / L"settings.ini", bytes);
}

std::optional<std::vector<Game>> load_cache(const fs::path& data, bool nested) {
    const auto dir = safe_existing(data, L"cache", true);
    if (!dir) return {};
    const auto file = read_text(*dir, L"catalog.ini", dataLimit);
    if (!file.value) return {};
    std::wstring_view rows = *file.value;
    if (take(rows, L'\n') != catalog_header(nested)) return {};
    std::vector<Game> games;
    std::set<std::wstring, OrdinalLess> ids;
    std::set<std::wstring, OrdinalLess> entryIds;
    const auto complete_game = [&] {
        if (games.empty()) return true;
        const auto& metadataEntry = games.back().metadataEntry;
        return !entryIds.empty() && (metadataEntry.empty() || entryIds.contains(metadataEntry));
    };
    while (!rows.empty()) {
        auto row = take(rows, L'\n');
        const auto kind = take(row, L'\t');
        if (kind == L"G") {
            if (games.size() >= maxGames || !complete_game()) return {};
            entryIds.clear();
            Game game;
            game.id = take(row, L'\t');
            game.title = take(row, L'\t');
            game.genre = take(row, L'\t');
            game.description = take(row, L'\t');
            game.metadataEntry = take(row, L'\t');
            game.cover = row;
            if (!game_id(game.id) || !ids.insert(game.id).second || game.title.empty() || !display_text(game.title, 255) ||
                !display_text(game.genre, 120) || !display_text(game.description, 1000) ||
                (!game.metadataEntry.empty() && (!relative_path(game.metadataEntry) || !swf(game.metadataEntry))) ||
                (!game.cover.empty() && !valid_cover(game.cover))) return {};
            index(game);
            games.push_back(std::move(game));
        } else if (kind == L"E") {
            if (games.empty() || entryIds.size() >= maxEntries + 1 || !relative_path(row) || !swf(row) ||
                !entryIds.emplace(row).second) return {};
            games.back().entries.emplace_back(row);
        } else {
            return {};
        }
    }
    if (!complete_game()) return {};
    return games;
}

bool save_cache(const fs::path& data, const std::vector<Game>& games, bool nested) {
    const auto dir = safe_existing(data, L"cache", true);
    if (!dir) return false;
    std::wstring text = std::wstring(catalog_header(nested)) + L"\n";
    for (const auto& game : games) {
        text += L"G\t" + game.id + L"\t" + game.title + L"\t" + game.genre + L"\t" +
                game.description + L"\t" + game.metadataEntry + L"\t" + game.cover + L"\n";
        for (const auto& entry : game.entries) text += L"E\t" + entry + L"\n";
        if (text.size() > dataLimit) return false;
    }
    const auto bytes = encode_utf8(text);
    return !bytes.empty() && atomic_write(*dir / L"catalog.ini", bytes);
}

bool clear_catalog_cache(const fs::path& root) {
    const auto missing = [](DWORD error) { return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND; };
    if (!ordinary(attributes(root), true)) return false;
    auto dir = root;
    for (const auto* part : {L"data", L"cache"}) {
        dir /= part;
        const auto attr = attributes(dir);
        if (attr == badAttributes) return missing(GetLastError());
        if (!ordinary(attr, true)) return false;
    }
    const auto file = CreateFileW(io_path(dir / L"catalog.ini").c_str(), DELETE | FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return missing(GetLastError());
    BY_HANDLE_FILE_INFORMATION info{};
    FILE_DISPOSITION_INFO disposition{TRUE};
    const bool deleted = GetFileInformationByHandle(file, &info) && ordinary(info.dwFileAttributes, false) &&
                         SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition));
    CloseHandle(file);
    return deleted;
}
}
