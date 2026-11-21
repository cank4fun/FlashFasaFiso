# FlashFasaFiso

[English](README.md) | [Türkçe](README.tr.md)

FlashFasaFiso is a portable launcher for local Flash games. Search your library, mark favorites, and play in a separate Ruffle Desktop window.

## Getting started

Download `FlashFasaFiso-v1.0.0-win64.zip` from [GitHub Releases](https://github.com/cank4fun/FlashFasaFiso/releases/latest). On **Windows 11 x64**, extract the archive and run `FlashFasaFiso.exe`. Ruffle Desktop 0.8.0 is included. The launcher creates `games/` and `data/` as needed in a writable folder.

Game files are not included. Add SWF files you have permission to use, with one folder per game under `games/`. Keep each game's assets in place and press **F5** or click **Rescan**.

```text
FlashFasaFiso/
  FlashFasaFiso.exe
  runtime/ruffle.exe
  games/Example Game/game.swf
  data/
```

## Using the library

Use **Ctrl+F** to search, **Ctrl+D** to toggle a favorite, and **Enter** to play. For games with several SWFs, use **Choose Entry File**.

The launcher scans each game's root folder for SWFs. With subfolder scanning enabled, it looks up to two levels deeper if there are none at the root. An optional UTF-8 `game.ini` accepts `title`, `genre`, `entry`, and `description`. Write `entry` relative to the game folder with `/` separators. Name covers `cover.png`, `cover.jpg`, or `cover.jpeg`. `cover.webp` works when WIC supports WebP decoding.

Games keep running when you close the launcher. Close Ruffle before opening another game from the same folder. If Ruffle starts without a window, **Cancel Startup** requests a normal exit. Forcing it to exit requires a second confirmation.

## Settings and saves

New profiles start in English. Choose English or Turkish in **Settings**. Changes take effect immediately and persist across launches. Windows menus, Ruffle, and game content use their own language settings.

| Path | Contents |
| --- | --- |
| `data/settings.ini` | Preferences and the last selection |
| `data/favorites.ini` | Favorites |
| `data/library.ini` | Saved SWF choices |
| `data/saves/` | Ruffle game saves |
| `data/ruffle-config/` | Ruffle preferences |
| `data/cache/ruffle/log/` | Ruffle logs |

**Clear Cache** removes the catalog cache and in-memory covers. Move the whole FlashFasaFiso folder to keep your settings and saves. Renaming a game folder or entry SWF can change its ID or save key.

## Build and test

Use Visual Studio 2022 17.14 with MSVC x64 tools and the Windows SDK, plus CMake 3.25 or later. Run these commands in PowerShell from the source folder.

```powershell
.\build-windows.ps1
.\build-windows.ps1 -Configuration Debug
.\build-windows.ps1 -Package
```

The script builds and runs CTest. `-Package` fetches the pinned Ruffle archive if needed, verifies its SHA-256, and prepares `out/FlashFasaFiso/`. After packaging, run `powershell.exe -NoProfile -File tests/verify-windows.ps1` from an interactive Windows desktop. The tests use Python 3 to generate a test SWF and catalog.

The launcher uses C++23, Win32, Direct2D, and DirectWrite. A background worker scans the library and decodes covers with Windows Imaging Component (WIC). Add a complete `README.<language-code>.md` for each new UI language.

## Compatibility and licensing

Compatibility varies by game. The launcher makes no network requests and starts Ruffle with SWF TCP sockets and browser redirects disabled. Games that load external HTTP(S) resources through Ruffle may need a network connection. Ruffle limits noninteractive file access to the entry SWF's directory, which can affect games with a nested entry file. The portable save URL can affect games that rely on their original absolute URL.

Ruffle 0.8.0 needs writable `data/` for its startup log. Drive-letter changes were tested with `SUBST`. Windows 10 and physical USB media were not tested.

FlashFasaFiso's code is [MIT-licensed](LICENSE). Ruffle uses MIT or Apache-2.0 and has separate [license and third-party notices](LICENSES/Ruffle-LICENSE.md) and [source links](LICENSES/Ruffle-SOURCE.md). Games and their assets retain their own rights.
