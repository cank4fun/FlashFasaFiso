#include "library.hpp"
#include "localization.hpp"
#include "ruffle.hpp"
#include <shellapi.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace fff;

static void check(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
}

static void file(const fs::path& path, std::string_view value = "fixture") {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << value;
    check(out.good(), "fixture write");
}

int main() try {
    const auto root = fs::current_path() / (L"fixture-" + std::to_wstring(GetCurrentProcessId()));
    check(fs::create_directory(root), "fresh fixture root");
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove_all(path, ec); } } cleanup{root};
    const auto games = root / L"games";
    file(games / L"ŞİMŞEK 雪" / L"başla.SWF");
    file(games / L"ŞİMŞEK 雪" / L"assets" / L"extra.swf");
    file(games / L"ŞİMŞEK 雪" / L"game.ini", "\xef\xbb\xbf[game]\r\ntitle=Işık 雪\r\ngenre=Bulmaca\n");
    file(games / L"İkinci" / L"sub" / L"one.swf");
    file(games / L"İkinci" / L"sub" / L"two.swf");
    file(games / L"İkinci" / L"game.ini", "entry=sub/two.swf\n");
    file(games / L"Bozuk" / L"one.swf");
    file(games / L"Bozuk" / L"two.swf");
    file(games / L"Bozuk" / L"game.ini", "title=\xff\nentry=../İkinci/sub/two.swf");
    file(games / L"Loose.swf");
    file(games / L"Empty" / L"readme.txt");
    file(games / L"Aday" / L"one.swf");
    file(games / L"Aday" / L"assets" / L"main.swf");
    file(games / L"Aday" / L"game.ini", "entry=assets/main.swf");
    const auto result = scan(games);
    check(result.complete && result.games.size() == 4, "one folder per game");
    check(result.badMetadata == 1, "malformed UTF-8 detection");
    const auto& first = *find_game(result.games, L"ŞİMŞEK 雪");
    const auto& second = *find_game(result.games, L"İkinci");
    const auto& bad = *find_game(result.games, L"Bozuk");
    check(first.title == L"Işık 雪" && first.genre == L"Bulmaca", "UTF-8 metadata");
    check(first.entries.size() == 1 && choose_entry(first, L"") == L"başla.SWF", "top level SWF");
    check(choose_entry(second, L"sub/one.swf") == L"sub/one.swf", "saved selection priority");
    check(choose_entry(second, L"gone.swf") == L"sub/two.swf", "valid metadata fallback");
    check(choose_entry(*find_game(result.games, L"Aday"), L"") == L"assets/main.swf", "explicit nested metadata");
    check(choose_entry(bad, L"").empty(), "ambiguous entry");
    check(search_match(first, fold_search(L"IŞIK")) && search_match(result.games[0], L""), "search Turkish");
    const auto unicodeGames = root / L"unicode-games";
    file(unicodeGames / L"Unicode" / L"main.swf");
    file(unicodeGames / L"Unicode" / L"game.ini", "title=Cafe\xcc\x81\n");
    const auto unicodeResult = scan(unicodeGames);
    check(unicodeResult.games.size() == 1 && unicodeResult.games.front().title == L"Cafe\u0301" &&
          search_match(unicodeResult.games.front(), fold_search(L"CAF\u00c9")) &&
          search_match(first, fold_search(L"I\u0307S\u0327IK")), "canonically equivalent Unicode search");
    file(unicodeGames / L"Unicode" / L"game.ini",
         "title=Valid title\nunknown=ignored\ndescription=" + std::string(1001, 'a') + "\n");
    const auto invalidMetadata = scan(unicodeGames);
    check(invalidMetadata.complete && invalidMetadata.badMetadata == 1 && invalidMetadata.games.size() == 1 &&
          invalidMetadata.games.front().title == L"Valid title" && invalidMetadata.games.front().description.empty(),
          "invalid known metadata is reported while valid fields are kept");
    check(!relative_path(L"../a") && !relative_path(L"a/../b") && !relative_path(L"a\\b") &&
          !relative_path(L"C:/x") && !relative_path(L"a:stream") && !relative_path(L"//x") &&
          !relative_path(L"NUL.txt") && !relative_path(L"a. "), "relative path validation");
    check(!safe_existing(games, L"İkinci/sub/missing.swf", false), "stale file");
    check(!safe_existing(games, L"İkinci/sub", false), "reject directory for file");
    const auto longGames = root / L"long-games";
    const std::wstring longEntry = std::wstring(130, L'ş') + L"/" + std::wstring(130, L'雪') + L"/oyun.swf";
    auto longPath = longGames / L"Uzun";
    for (const auto& component : fs::path(longEntry)) longPath /= component;
    file(io_path(longPath));
    const auto longResult = scan(longGames);
    check(longResult.complete && longResult.games.size() == 1 &&
          choose_entry(longResult.games.front(), L"") == longEntry, "nested Win32 long path");
    check(scan(longGames, false).games.empty(), "flat scan skips nested-only game folders");
    const auto flat = scan(games, false);
    const auto* flatSecond = find_game(flat.games, L"İkinci");
    check(flatSecond && flatSecond->entries == std::vector<std::wstring>{L"sub/two.swf"},
          "flat scan keeps explicitly declared nested SWF");

    const auto limitGames = root / L"limit-games";
    const auto rootCandidates = limitGames / L"Root";
    const auto nestedCandidates = limitGames / L"Nested" / L"sub";
    for (unsigned i = 0; i < 1024; ++i) {
        const auto name = L"entry-" + std::to_wstring(i) + L".swf";
        file(rootCandidates / name);
        file(nestedCandidates / name);
    }
    file(rootCandidates / L"zzz.txt");
    file(nestedCandidates / L"zzz.txt");
    const auto atLimit = scan(limitGames);
    check(atLimit.complete && !atLimit.limited && atLimit.games.size() == 2 &&
          atLimit.games[0].entries.size() == 1024 && atLimit.games[1].entries.size() == 1024,
          "exact SWF limit ignores extra non-candidates in root and nested scans");
    file(rootCandidates / L"assets" / L"main.swf");
    file(rootCandidates / L"game.ini", "entry=assets/main.swf\n");
    const auto metadataOverflow = scan(limitGames);
    const auto* rootGame = find_game(metadataOverflow.games, L"Root");
    check(!metadataOverflow.complete && metadataOverflow.limited && metadataOverflow.errors == 0 && rootGame &&
          rootGame->entries.size() == 1024 &&
          choose_entry(*rootGame, L"") == L"assets/main.swf", "candidate limit preserves explicit metadata entry");
    file(rootCandidates / L"zzz.swf");
    file(nestedCandidates / L"zzz.swf");
    const auto scanOverflow = scan(limitGames);
    check(!scanOverflow.complete && scanOverflow.limited && scanOverflow.errors == 0 && scanOverflow.games.size() == 2 &&
          scanOverflow.games[0].entries.size() == 1024 && scanOverflow.games[1].entries.size() == 1024,
          "root and nested overflow retain bounded usable catalog");

    check(text(Language::tr, Text::settings) == L"Ayarlar" &&
          text(Language::en, Text::settings) == L"Settings" &&
          text(static_cast<Language>(99), Text::settings) == L"Settings", "translations and fallback");

    const auto data = root / L"data";
    fs::create_directory(data);
    State state;
    check(load_state(data).preferences.language == Language::en, "a clean installation defaults to English");
    file(data / L"settings.ini", "FFF settings 1\nselection=ŞİMŞEK 雪\n");
    const auto legacy = load_state(data);
    check(legacy.selection == L"ŞİMŞEK 雪" && legacy.preferences.language == Language::en &&
          legacy.preferences == Preferences{} && !legacy.damaged,
          "v1 settings migrate with defaults");
    file(data / L"settings.ini", "FFF settings 2\nlanguage=tr\n");
    check(load_state(data).preferences.language == Language::tr, "an existing Turkish preference is retained");
    state.preferences = {Language::en, false, true, true, false, false, Page::favorites};
    state.favorites.insert(first.id);
    state.entries[second.id] = L"sub/one.swf";
    state.selection = first.id;
    check(save_favorites(data, state) && save_entries(data, state) && save_settings(data, state), "atomic state write");
    state.favorites.insert(second.id);
    check(save_favorites(data, state), "atomic replacement");
    const auto loaded = load_state(data);
    check(loaded.favorites == state.favorites && loaded.entries == state.entries &&
          loaded.selection == state.selection && loaded.preferences == state.preferences && !loaded.damaged,
          "state round trip");
    file(data / L"settings.ini", "FFF settings 2\r\nlanguage=invalid\r\nauto_scan=0\r\nconfirm_close=bad\nlast_tab=settings\r\n");
    const auto damaged = load_state(data);
    check(damaged.damaged && damaged.preferences.language == Language::en && !damaged.preferences.autoScan &&
          !damaged.preferences.confirmClose && damaged.preferences.lastPage == Page::settings,
          "invalid preferences fall back individually");
    file(data / L"settings.ini", "\xff");
    check(load_state(data).damaged && load_state(data).preferences == Preferences{}, "invalid UTF-8 defaults");
    check(save_settings(data, state), "save settings again");
    const auto settingsFile = data / L"settings.ini";
    check(SetFileAttributesW(settingsFile.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE, "read-only settings fixture");
    state.preferences.language = Language::tr;
    const bool settingsDenied = !save_settings(data, state);
    const bool settingsPreserved = load_state(data).preferences.language == Language::en;
    check(SetFileAttributesW(settingsFile.c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE, "restore settings attributes");
    check(settingsDenied && settingsPreserved, "failed setting save preserves previous settings");
    const auto protectedFile = data / L"favorites.ini";
    check(SetFileAttributesW(protectedFile.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE, "read-only fixture");
    state.favorites.clear();
    const bool rejectedWrite = !save_favorites(data, state);
    const bool preserved = load_state(data).favorites == loaded.favorites;
    check(SetFileAttributesW(protectedFile.c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE, "restore fixture attributes");
    check(rejectedWrite && preserved, "failed replacement preserves original state");
    file(data / L"favorites.ini", "invalid\n");
    check(load_state(data).damaged && load_state(data).favorites.empty(), "corrupt state");

    const auto recovery = root / L"recovery";
    file(recovery / L"favorites.ini", "FFF favorites 1\nPrevious\n");
    file(recovery / L"library.ini", "FFF entries 1\nPrevious\tgame.swf\ninvalid\n");
    const auto locked = CreateFileW((recovery / L"favorites.ini").c_str(), GENERIC_READ, 0, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(locked != INVALID_HANDLE_VALUE, "lock favorites fixture");
    auto recovered = load_state(recovery);
    CloseHandle(locked);
    check(recovered.damaged && recovered.favorites.empty() && recovered.entries.contains(L"Previous"),
          "unreadable favorites and partial recovery of malformed entries");
    recovered.favorites.insert(L"New");
    recovered.entries.emplace(L"New", L"main.swf");
    recovered.selection = L"New";
    const auto wroteFavorites = save_favorites(recovery, recovered);
    const auto wroteEntries = save_entries(recovery, recovered);
    check(save_settings(recovery, recovered), "unaffected settings remain writable");
    const auto preservedState = load_state(recovery);
    check(!wroteFavorites && preservedState.favorites == std::set<std::wstring, OrdinalLess>{L"Previous"},
          "temporarily unreadable favorites are preserved after access returns");
    check(!wroteEntries && preservedState.damaged && !preservedState.entries.contains(L"New"),
          "malformed entries are preserved when saving recovered state");
    file(recovery / L"settings.ini", "FFF settings 2\nlanguage=invalid\nauto_scan=0\n");
    auto brokenSettings = load_state(recovery);
    brokenSettings.preferences.autoScan = true;
    check(!save_settings(recovery, brokenSettings) && !load_state(recovery).preferences.autoScan,
          "malformed settings survive automatic persistence");

    fs::create_directory(data / L"cache");
    check(save_cache(data, result.games), "cache write");
    const auto catalog = load_cache(data);
    check(catalog && *catalog == result.games, "cache round trip");
    check(!load_cache(data, false) && save_cache(data, flat.games, false) && !load_cache(data) &&
          load_cache(data, false) == flat.games, "cache respects the scan policy");
    file(data / L"saves" / L"keep.sol");
    file(data / L"cache" / L"ruffle" / L"log" / L"keep.log");
    check(clear_catalog_cache(root) && clear_catalog_cache(root) && !load_cache(data, false) &&
          fs::exists(data / L"saves" / L"keep.sol") && fs::exists(data / L"cache" / L"ruffle" / L"log" / L"keep.log") &&
          fs::exists(games / L"Loose.swf") && fs::exists(data / L"library.ini") &&
          fs::exists(data / L"favorites.ini") && fs::exists(data / L"settings.ini"), "catalog-only idempotent cleanup");
    fs::create_directory(data / L"cache" / L"catalog.ini");
    file(data / L"cache" / L"catalog.ini" / L"keep");
    check(!clear_catalog_cache(root) && fs::exists(data / L"cache" / L"catalog.ini" / L"keep"), "refuse to clear a directory");
    fs::remove(data / L"cache" / L"catalog.ini" / L"keep");
    fs::remove(data / L"cache" / L"catalog.ini");
    file(data / L"cache" / L"catalog.ini", "FFF catalog 1\nG\t../escape\tBad");
    check(!load_cache(data), "unsafe cache");
    file(data / L"cache" / L"catalog.ini", "FFF catalog 1\nG\tGame\tGame\t\t\t\t\nE\tfirst.swf\nE\tfirst.swf\n");
    const auto duplicates = load_cache(data);
    file(data / L"cache" / L"catalog.ini", "FFF catalog 1\nG\tGame\tGame\t\t\tmissing.swf\t\nE\tfirst.swf\n");
    const auto missingMetadataEntry = load_cache(data);
    check(!duplicates && !missingMetadataEntry, "cache rejects duplicate entries and unmatched metadata entries");
    check(!scan(root / L"missing").complete, "missing scan root");
    file(root / L"blocked");
    check(!ensure_directory(root, L"blocked/new"), "blocked data directory");

    for (const std::wstring original : {L"", L"metin 雪", L"foo\\", L"x\"y", L"a\\\" b\\\\"}) {
        const auto command = L"ruffle.exe " + quote_arg(original);
        int count = 0;
        auto arguments = CommandLineToArgvW(command.c_str(), &count);
        const bool equal = arguments && count == 2 && original == arguments[1];
        LocalFree(arguments);
        check(equal, "Win32 argument escaping");
    }
    std::cout << "Catalog, selection, Unicode, state, cache and Win32 quoting passed.\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
