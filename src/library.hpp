#pragma once
#include "localization.hpp"

#include <windows.h>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace fff {
namespace fs = std::filesystem;

struct OrdinalLess {
    bool operator()(std::wstring_view a, std::wstring_view b) const;
};
bool equal_name(std::wstring_view a, std::wstring_view b);
std::wstring fold_search(std::wstring_view text);
std::optional<std::wstring> decode_utf8(std::string_view text);
std::string encode_utf8(std::wstring_view text);
bool relative_path(std::wstring_view path);
fs::path io_path(const fs::path& path);
std::optional<fs::path> safe_existing(const fs::path& root, std::wstring_view relative, bool directory);
bool ensure_directory(const fs::path& root, std::wstring_view relative);
bool atomic_write(const fs::path& target, std::string_view contents);
fs::path executable_directory();

struct Game {
    std::wstring id, title, genre, description, metadataEntry, cover;
    std::vector<std::wstring> entries;
    std::wstring searchKey;
    bool operator==(const Game&) const = default;
};

struct ScanResult {
    std::vector<Game> games;
    unsigned errors = 0, badMetadata = 0;
    bool complete = true;
    bool limited = false;
};

ScanResult scan(const fs::path& games, bool nested = true, std::stop_token stop = {});
const Game* find_game(const std::vector<Game>& games, std::wstring_view id);
std::wstring choose_entry(const Game& game, std::wstring_view remembered);
bool search_match(const Game& game, std::wstring_view foldedQuery);

enum class Page { library, favorites, settings };

struct Preferences {
    Language language = defaultLanguage;
    bool autoScan = true, rememberTab = false, confirmClose = false, scanNested = true, rememberSwf = true;
    Page lastPage = Page::library;
    bool operator==(const Preferences&) const = default;
};

enum class StateFileStatus { untracked, missing, loaded, unreadable, malformed };

struct State {
    std::set<std::wstring, OrdinalLess> favorites;
    std::map<std::wstring, std::wstring, OrdinalLess> entries;
    std::wstring selection;
    Preferences preferences;
    StateFileStatus favoritesFile = StateFileStatus::untracked;
    StateFileStatus entriesFile = StateFileStatus::untracked;
    StateFileStatus settingsFile = StateFileStatus::untracked;
    bool damaged = false;
};
State load_state(const fs::path& data);
bool save_favorites(const fs::path& data, const State& state);
bool save_entries(const fs::path& data, const State& state);
bool save_settings(const fs::path& data, const State& state);
std::optional<std::vector<Game>> load_cache(const fs::path& data, bool nested = true);
bool save_cache(const fs::path& data, const std::vector<Game>& games, bool nested = true);
bool clear_catalog_cache(const fs::path& root);
}
