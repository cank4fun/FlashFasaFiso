#include "window.hpp"
#include "canvas.hpp"
#include "resources.h"
#include "ruffle.hpp"
#include "worker.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace fff {
namespace {
constexpr auto windowClass = L"FlashFasaFiso.Window";
constexpr UINT_PTR playerStartupTimer = 1;
constexpr UINT playerWindowPollMs = 250;
constexpr ULONGLONG playerStartupNoticeMs = 8000;
constexpr ULONGLONG playerCloseNoticeMs = 4000;
constexpr float titleHeight = 44, rowHeight = 58;
constexpr float settingsTop = 107, settingsGap = 14, settingsUpper = 163, settingsLower = 137;
constexpr UINT base = 0x111315, panel = 0x171a1f, inset = 0x1d2127, border = 0x303640;
constexpr UINT muted = 0xaab4c0, white = 0xe6e8ec, accent = 0x62d5c8, titleBar = 0x341c26;
constexpr UINT accentTint = 0x233a39, hoverColor = 0x252b34, warning = 0xf5bd8b;
constexpr std::size_t noRow = std::numeric_limits<std::size_t>::max();
constexpr std::size_t coverLimit = 32;

UINT window_dpi(HWND window) {
    static const auto current = std::bit_cast<decltype(&GetDpiForWindow)>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (current) {
        if (const auto dpi = current(window)) return dpi;
    }
    const auto dc = GetDC(window);
    if (!dc) return 96;
    const auto dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(window, dc);
    return dpi > 0 ? static_cast<UINT>(dpi) : 96;
}

int dpi_metric(int index, UINT dpi) {
    static const auto current = std::bit_cast<decltype(&GetSystemMetricsForDpi)>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi"));
    return current ? current(index, dpi) : GetSystemMetrics(index);
}

enum class Action {
    none, library, favorites, settings, refresh, gamesFolder, play, folder, favorite, entry,
    minimize, maximize, close, list, search, turkish, english, autoScan, rememberTab,
    confirmClose, scanNested, rememberSwf, savesFolder, clearCache
};
enum class PlayerStartup { waiting, ready, stopping, unresponsive };
struct Control { Action action; Rect rect; std::optional<Rect> clip = std::nullopt; };

struct Layout {
    float width = 0, height = 0, navWidth = 176;
    Rect listPanel{}, list{}, detail{}, search{}, openGames{}, refresh{}, settingsView{}, detailView{};
    std::array<Rect, 3> navigation{};
    std::array<Rect, 3> caption{};

    void size(float w, float h) {
        width = w;
        height = h;
        navWidth = w < 760 ? 120.0f : w < 945 ? 141.0f : 176.0f;
        const float padding = w < 760 ? 10.0f : w < 945 ? 16.0f : 22.0f;
        const float details = std::clamp(w * 0.285f, w < 760 ? 164.0f : 214.0f, 320.0f);
        const float left = navWidth + padding, right = w - padding, bottom = h - 44;
        const float divider = right - details;
        listPanel = {left, 160, divider - padding, bottom};
        list = {left + 5, 193, divider - padding - 11, std::max(193.0f, bottom - 6)};
        detail = {divider, 64, right, bottom};
        detailView = {divider, 104, right, std::max(104.0f, bottom)};
        settingsView = {left, 105, right, std::max(105.0f, bottom + 3)};
        search = {left, 109, divider - padding - 43, 145};
        refresh = {divider - padding - 35, 109, divider - padding, 145};
        openGames = box(12, h - 84, navWidth - 24, 34);
        for (unsigned i = 0; i < navigation.size(); ++i) {
            navigation[i] = box(10, 69.0f + 44.0f * i, navWidth - 20, 37);
            caption[i] = box(w - 138 + 46.0f * i, 0, 46, titleHeight);
        }
    }

    bool stack_settings() const { return settingsView.right - settingsView.left < 510; }
    bool stack_detail_buttons() const { return detail.right - detail.left < 200; }
    bool compact_details() const { return height < 480 || stack_detail_buttons(); }
    float detail_controls_height() const {
        return stack_detail_buttons() ? 134.0f : compact_details() ? 104.0f : 122.0f;
    }
    float detail_max_scroll() const {
        return std::max(0.0f, detailView.top + 66 + detail_controls_height() - detailView.bottom);
    }
    float settings_max_scroll() const {
        const float cards = settingsUpper + settingsGap + settingsLower;
        const float bottom = settingsTop + (stack_settings() ? 2 * cards + settingsGap : cards);
        return std::max(0.0f, bottom - settingsView.bottom);
    }
};

Rect scrollbar_thumb(Rect view, float offset, float maximum) {
    const float viewport = view.bottom - view.top;
    if (viewport <= 0 || maximum <= 0) return {};
    const float height = std::clamp(viewport * viewport / (viewport + maximum), std::min(24.0f, viewport), viewport);
    return box(view.right + 2, view.top + (viewport - height) * offset / maximum, 5, height);
}

struct Cover {
    ComPtr<ID2D1Bitmap> bitmap;
    std::uint64_t used = 0;
};

struct EntryChoices {
    const std::vector<std::wstring>& paths;
    std::wstring_view current;
    Language language;
};

void dialog_text(HWND dialog, Language language, Text title, Text prompt, Text accept) {
    SetWindowTextW(dialog, text(language, title).data());
    SetDlgItemTextW(dialog, IDC_DIALOG_PROMPT, text(language, prompt).data());
    SetDlgItemTextW(dialog, IDOK, text(language, accept).data());
    SetDlgItemTextW(dialog, IDCANCEL, text(language, Text::cancel).data());
}

INT_PTR CALLBACK close_dialog(HWND dialog, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_INITDIALOG) {
        dialog_text(dialog, static_cast<Language>(l), Text::closeTitle, Text::closePrompt, Text::close);
        SetFocus(GetDlgItem(dialog, IDCANCEL));
        return FALSE;
    }
    if (message == WM_COMMAND && (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL)) {
        EndDialog(dialog, LOWORD(w));
        return TRUE;
    }
    return FALSE;
}

INT_PTR CALLBACK choose_dialog(HWND dialog, UINT message, WPARAM w, LPARAM l) {
    const auto list = GetDlgItem(dialog, IDC_SWF_ENTRIES);
    if (message == WM_INITDIALOG) {
        const auto& choices = *reinterpret_cast<const EntryChoices*>(l);
        dialog_text(dialog, choices.language, Text::chooseEntry, Text::choosePrompt, Text::choose);
        int selection = 0, widest = 0, i = 0;
        const auto dc = GetDC(list);
        const auto font = reinterpret_cast<HFONT>(SendMessageW(list, WM_GETFONT, 0, 0));
        const auto previous = dc && font ? SelectObject(dc, font) : nullptr;
        for (const auto& path : choices.paths) {
            if (equal_name(path, choices.current)) selection = i;
            const auto added = SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(path.c_str()));
            if (added < 0) {
                if (previous) SelectObject(dc, previous);
                if (dc) ReleaseDC(list, dc);
                EndDialog(dialog, -1);
                return TRUE;
            }
            SIZE size{};
            if (dc && GetTextExtentPoint32W(dc, path.c_str(), static_cast<int>(path.size()), &size)) {
                widest = std::max(widest, static_cast<int>(size.cx) + 12);
            }
            ++i;
        }
        if (previous) SelectObject(dc, previous);
        if (dc) ReleaseDC(list, dc);
        SendMessageW(list, LB_SETHORIZONTALEXTENT, widest, 0);
        SendMessageW(list, LB_SETCURSEL, selection, 0);
        SetFocus(list);
        return FALSE;
    }
    if (message == WM_COMMAND) {
        const auto id = LOWORD(w);
        if (id == IDOK || (id == IDC_SWF_ENTRIES && HIWORD(w) == LBN_DBLCLK)) {
            const auto selection = SendMessageW(list, LB_GETCURSEL, 0, 0);
            if (selection != LB_ERR) EndDialog(dialog, selection + 1);
            return TRUE;
        }
        if (id == IDCANCEL) {
            EndDialog(dialog, 0);
            return TRUE;
        }
    }
    return FALSE;
}

struct SingleInstance {
    explicit SingleInstance(const fs::path& root) {
        const auto handle = CreateFileW(io_path(root).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return;
        BY_HANDLE_FILE_INFORMATION info{};
        const auto found = GetFileInformationByHandle(handle, &info);
        CloseHandle(handle);
        if (!found || (!info.nFileIndexHigh && !info.nFileIndexLow)) return;
        const auto name = L"Local\\FlashFasaFiso." + std::to_wstring(info.dwVolumeSerialNumber) + L"." +
            std::to_wstring(info.nFileIndexHigh) + L"." + std::to_wstring(info.nFileIndexLow);
        mutex = CreateMutexW(nullptr, FALSE, name.c_str());
        duplicate = mutex && GetLastError() == ERROR_ALREADY_EXISTS;
    }
    ~SingleInstance() { if (mutex) CloseHandle(mutex); }
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;
    HANDLE mutex = nullptr;
    bool duplicate = false;
};

struct PlayerWindows {
    DWORD process;
    HWND visible = nullptr, hidden = nullptr;
};

PlayerWindows player_windows(HANDLE process) {
    PlayerWindows windows{GetProcessId(process)};
    if (!windows.process) return windows;
    EnumWindows([](HWND window, LPARAM context) -> BOOL {
        auto& found = *reinterpret_cast<PlayerWindows*>(context);
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid != found.process || GetWindow(window, GW_OWNER) || !GetWindowTextLengthW(window) ||
            (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) return TRUE;
        if (IsWindowVisible(window)) {
            found.visible = window;
            return FALSE;
        }
        if (!found.hidden) found.hidden = window;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

class Window {
public:
    explicit Window(HINSTANCE instance) : instance_(instance), root_(executable_directory()),
        settingsPaths_{(root_ / L"games").native(), (root_ / L"runtime\\ruffle.exe").native(),
                       (root_ / L"data\\saves").native()} {}
    ~Window() {
        if (player_) CloseHandle(player_);
        if (editFont_) DeleteObject(editFont_);
        if (editBackground_) DeleteObject(editBackground_);
    }

    int run(int show);
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l);
    static LRESULT CALLBACK edit_procedure(HWND edit, UINT message, WPARAM w, LPARAM l,
                                          UINT_PTR, DWORD_PTR reference);

private:
    LRESULT message(UINT message, WPARAM w, LPARAM l);
    bool create();
    void resize();
    void paint();
    void chrome();
    void navigation();
    void library();
    void details();
    void settings();
    void toggle(Action action, Rect rect, Text label, bool checked);
    void settings_path(unsigned index, Rect rect);
    void pane_scrollbar(Rect view, float offset, float maximum);
    void button(Action action, Rect rect, std::wstring_view label, bool primary = false);
    void draw_cover(Rect rect, const Game& game, bool large);
    void redraw() { InvalidateRect(window_, nullptr, FALSE); }
    void release_target();
    void receive_library();
    void receive_covers();
    void request_covers();
    void rebuild_filter(bool resetOffset);
    const Game* selected() const;
    void select_at(std::size_t row);
    std::size_t row_at(float x, float y) const;
    void set_scroll(float offset);
    void set_pane_scroll(float offset);
    float max_scroll() const;
    Rect scroll_thumb() const;
    Rect pane_view() const { return page_ == Page::settings ? layout_.settingsView : layout_.detailView; }
    float pane_max_scroll() const;
    float pane_scroll() const { return page_ == Page::settings ? settingsScroll_ : detailScroll_; }
    void activate(Action action);
    void switch_page(Page page);
    void favorite();
    void pick_entry(bool startGame);
    void launch(const Game& game, std::wstring_view entry);
    void check_player_startup();
    void player_ready();
    void cancel_player_startup(bool closeAfter);
    void process_exited();
    void finish_close();
    void open_folder(Action action);
    void rescan();
    void change_setting(Action action);
    bool persist_settings();
    void update_language();
    std::wstring_view remembered_entry(const Game& game) const;
    std::wstring_view label(Text key) const { return text(state_.preferences.language, key); }
    std::wstring status_text() const;
    void set_status(Text key, bool error = false, std::wstring argument = {});
    void focus(Action action);
    bool key(WPARAM code, bool fromSearch);
    void tab();
    Action control_at(float x, float y) const;
    LRESULT hit_test(LPARAM l) const;
    void mouse_move(float x, float y);
    float dip(int px) const { return static_cast<float>(px) * 96.0f / static_cast<float>(dpi_); }
    int px(float value) const { return static_cast<int>(std::lround(value * static_cast<float>(dpi_) / 96.0f)); }

    HINSTANCE instance_;
    HWND window_ = nullptr, edit_ = nullptr, tooltips_ = nullptr;
    HFONT editFont_ = nullptr;
    HBRUSH editBackground_ = nullptr;
    UINT dpi_ = 96;
    const fs::path root_;
    std::array<std::wstring, 3> settingsPaths_;
    std::array<RECT, 3> pathRects_{};
    bool pathTooltipsActive_ = false;
    Canvas canvas_;
    Layout layout_;
    std::unique_ptr<Worker> worker_;
    State state_;
    std::map<std::wstring, std::wstring, OrdinalLess> sessionEntries_;
    std::vector<Game> games_;
    std::vector<std::size_t> filtered_;
    std::vector<Control> controls_;
    std::optional<Rect> controlClip_;
    std::map<std::wstring, Cover> covers_;
    std::uint64_t coverClock_ = 0, requestedScan_ = 0;
    unsigned generation_ = 0;
    Page page_ = Page::library;
    Action hover_ = Action::none, pressed_ = Action::none, focus_ = Action::list;
    std::size_t hoveredRow_ = noRow, selectedRow_ = noRow;
    std::wstring selectedId_, query_, statusArgument_;
    Text status_ = Text::loading;
    float scroll_ = 0, settingsScroll_ = 0, detailScroll_ = 0, dragAnchor_ = 0;
    int wheelRemainder_ = 0;
    bool dragging_ = false, draggingPane_ = false, tracking_ = false, ncTracking_ = false, scanning_ = true;
    bool dataReady_ = false, statusError_ = false, runtimeReady_ = false, recoveringGraphics_ = false;
    bool stateLoaded_ = false, pageTouched_ = false, settingsUnsaved_ = false, closing_ = false;
    bool personalDataLoaded_ = false, deferredScan_ = false, workerFailed_ = false;
    bool cacheError_ = false, incomplete_ = false, limited_ = false, cacheFallback_ = false, manualScanHint_ = false;
    unsigned badMetadata_ = 0;
    HANDLE player_ = nullptr;
    ULONGLONG playerStartedAt_ = 0;
    PlayerStartup playerStartup_ = PlayerStartup::waiting;
    bool closeAfterPlayer_ = false;
    std::wstring playingTitle_;
};

void Window::button(Action action, Rect rect, std::wstring_view label, bool primary) {
    controls_.push_back({action, rect, controlClip_});
    const bool over = hover_ == action;
    const bool down = pressed_ == action;
    canvas_.fill(rect, primary ? (down ? 0x3cab9f : over ? 0x84e3d8 : accent)
                               : (down ? accentTint : over ? hoverColor : inset), 6);
    canvas_.border(rect, primary ? accent : border, 6);
    if (focus_ == action && GetFocus() == window_) {
        canvas_.border({rect.left + 2, rect.top + 2, rect.right - 2, rect.bottom - 2}, primary ? 0x27665f : accent, 4);
    }
    canvas_.text(label, {rect.left + 5, rect.top, rect.right - 5, rect.bottom},
                 Font::strong, primary ? 0x112623 : white, true);
}

void Window::chrome() {
    canvas_.fill(box(0, 0, layout_.width, titleHeight), titleBar);
    canvas_.line(0, titleHeight, layout_.width, titleHeight, 0x592f42);
    canvas_.fill(box(13, 10, 33, 24), 0x593445, 6);
    canvas_.text(L"FFF", box(13, 10, 33, 24), Font::strong, 0xf8e8ef, true);
    canvas_.text(L"FlashFasaFiso", box(56, 0, 205, titleHeight), Font::title, white);
    constexpr Action actions[]{Action::minimize, Action::maximize, Action::close};
    for (unsigned i = 0; i < layout_.caption.size(); ++i) {
        const auto rect = layout_.caption[i];
        controls_.push_back({actions[i], rect});
        if (hover_ == actions[i] || pressed_ == actions[i]) {
            canvas_.fill(rect, i == 2 ? 0xa63b49 : pressed_ == actions[i] ? 0x694352 : 0x533240);
        }
        if (focus_ == actions[i] && GetFocus() == window_) {
            canvas_.border({rect.left + 5, rect.top + 5, rect.right - 5, rect.bottom - 5}, accent, 5);
        }
        const float x = (rect.left + rect.right) / 2, y = titleHeight / 2;
        if (i == 0) canvas_.line(x - 5, y, x + 5, y, white);
        else if (i == 1 && IsZoomed(window_)) {
            canvas_.line(x - 2, y - 6, x + 6, y - 6, white);
            canvas_.line(x + 6, y - 6, x + 6, y + 2, white);
            canvas_.border(box(x - 5, y - 3, 8, 8), white);
        } else if (i == 1) canvas_.border(box(x - 5, y - 5, 10, 10), white);
        else {
            canvas_.line(x - 5, y - 5, x + 5, y + 5, white);
            canvas_.line(x + 5, y - 5, x - 5, y + 5, white);
        }
    }
}

void Window::navigation() {
    canvas_.fill({0, titleHeight + 1, layout_.navWidth, layout_.height}, panel);
    canvas_.line(layout_.navWidth, titleHeight + 1, layout_.navWidth, layout_.height, border);
    constexpr Action actions[]{Action::library, Action::favorites, Action::settings};
    constexpr Text labels[]{Text::library, Text::favorites, Text::settings};
    constexpr std::wstring_view icons[]{L"\ue8f1", L"\ue734", L"\ue713"};
    for (unsigned i = 0; i < layout_.navigation.size(); ++i) {
        const auto rect = layout_.navigation[i];
        const auto active = static_cast<unsigned>(page_) == i;
        controls_.push_back({actions[i], rect});
        if (active || hover_ == actions[i] || pressed_ == actions[i]) {
            canvas_.fill(rect, active ? accentTint : hoverColor, 7);
        }
        if (active) canvas_.fill(box(rect.left, rect.top + 8, 3, 21), accent, 1.5f);
        if (focus_ == actions[i] && GetFocus() == window_) canvas_.border(rect, accent, 7);
        canvas_.text(icons[i], box(rect.left + 14, rect.top, 21, 37), Font::glyph, active ? accent : muted);
        canvas_.text(label(labels[i]), {rect.left + 40, rect.top, rect.right - 4, rect.bottom},
                     Font::strong, active ? white : muted);
    }
    button(Action::gamesFolder, layout_.openGames, label(Text::gamesFolder));
}

void Window::draw_cover(Rect rect, const Game& game, bool large) {
    canvas_.fill(rect, large ? 0x222b34 : 0x27333c, large ? 8.0f : 5.0f);
    const auto path = L"games/" + game.id + L"/" + game.cover;
    if (!game.cover.empty()) {
        if (const auto found = covers_.find(path); found != covers_.end()) {
            found->second.used = ++coverClock_;
            if (const auto bitmap = found->second.bitmap.Get()) {
                const auto size = bitmap->GetSize();
                const float ratio = std::min((rect.right - rect.left) / size.width, (rect.bottom - rect.top) / size.height);
                const float x = (rect.left + rect.right - ratio * size.width) / 2;
                const float y = (rect.top + rect.bottom - ratio * size.height) / 2;
                canvas_.target()->DrawBitmap(bitmap, box(x, y, ratio * size.width, ratio * size.height));
                return;
            }
        }
    }
    canvas_.text(L"\ue7fc", rect, Font::glyph, 0x91a8b5, true);
}

void Window::library() {
    const float left = layout_.listPanel.left;
    const float width = layout_.listPanel.right - left;
    const auto heading = label(page_ == Page::favorites ? Text::favorites : Text::libraryTitle);
    canvas_.text(heading, box(left, 57, width, 44), Font::heading, white);
    canvas_.fill(layout_.search, inset, 7);
    canvas_.border(layout_.search, GetFocus() == edit_ ? accent : border, 7);
    canvas_.text(L"\ue721", box(left + 11, 109, 22, 36), Font::glyph, muted);
    button(Action::refresh, layout_.refresh, L"↻");
    canvas_.fill(layout_.listPanel, panel, 9);
    canvas_.border(layout_.listPanel, border, 9);
    canvas_.text(label(Text::game), box(left + 16, 161, width - 32, 30), Font::strong, muted);
    const auto total = std::to_wstring(filtered_.size());
    canvas_.text(total, box(layout_.listPanel.right - 66, 161, 51, 30), Font::tiny, muted, true);
    canvas_.line(left + 1, 190, layout_.listPanel.right - 1, 190, border);
    if (layout_.list.bottom <= layout_.list.top || layout_.list.right <= layout_.list.left) return;
    if (filtered_.empty()) {
        const float mid = (layout_.list.top + layout_.list.bottom) / 2;
        const auto title = label(scanning_ && games_.empty() ? Text::scanning :
            !query_.empty() ? Text::noResults : page_ == Page::favorites ? Text::noFavorites : Text::noGames);
        const auto tip = label(scanning_ && games_.empty() ? Text::empty :
            !query_.empty() ? Text::searchTip : page_ == Page::favorites ? Text::favoritesTip : Text::gamesTip);
        const float visible = layout_.list.bottom - layout_.list.top;
        if (visible >= 116) canvas_.text(L"\ue7fc", box(left + 10, mid - 57, width - 20, 25), Font::glyph, accent, true);
        canvas_.text(title, box(left + 10, std::max(layout_.list.top, mid - 30), width - 20, 28), Font::title, white, true);
        if (visible >= 68) canvas_.text(tip, box(left + 9, mid, width - 18, 34), Font::body, muted, true, true);
        if (visible >= 150 && !scanning_ && query_.empty() && page_ == Page::library) {
            button(Action::gamesFolder, box(left + (width - 148) / 2, mid + 43, 148, 32), label(Text::openGames));
        }
        return;
    }
    const auto viewport = layout_.list;
    canvas_.target()->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_ALIASED);
    const auto first = static_cast<std::size_t>(scroll_ / rowHeight);
    const auto last = std::min(filtered_.size(), first + static_cast<std::size_t>((viewport.bottom - viewport.top) / rowHeight) + 2);
    for (auto i = first; i < last; ++i) {
        const auto& game = games_[filtered_[i]];
        const float top = viewport.top + static_cast<float>(i) * rowHeight - scroll_;
        const auto row = box(viewport.left, top + 3, viewport.right - viewport.left - 4, rowHeight - 5);
        if (i == selectedRow_ || i == hoveredRow_) {
            canvas_.fill(row, i == selectedRow_ ? accentTint : hoverColor, 6);
        } else {
            canvas_.line(row.left + 8, row.bottom, row.right - 6, row.bottom, border);
        }
        if (i == selectedRow_ && focus_ == Action::list && GetFocus() == window_) canvas_.border(row, 0x437e78, 6);
        draw_cover(box(row.left + 8, top + 9, 51, 39), game, false);
        canvas_.text(game.title, {row.left + 69, top + 8, row.right - 27, top + 30}, Font::strong, white);
        const auto metadata = game.genre.empty() ? (game.entries.size() == 1 ? L"SWF" :
            std::to_wstring(game.entries.size()) + L" SWF") : game.genre;
        canvas_.text(metadata, {row.left + 69, top + 30, row.right - 14, top + 51}, Font::tiny, muted);
        if (state_.favorites.contains(game.id)) {
            canvas_.text(L"\ue735", box(row.right - 24, top + 8, 18, 20), Font::glyph, accent, true);
        }
    }
    canvas_.target()->PopAxisAlignedClip();
    if (max_scroll() > 0) {
        pane_scrollbar(viewport, scroll_, max_scroll());
    }
}

void Window::details() {
    const auto area = layout_.detail;
    canvas_.fill(area, panel, 9);
    canvas_.border(area, border, 9);
    const float x = area.left + 14, right = area.right - 14, width = right - x;
    canvas_.text(label(Text::details), box(x, area.top + 5, width, 32), Font::strong, muted);
    canvas_.line(area.left + 1, area.top + 39, area.right - 1, area.top + 39, border);
    const auto view = layout_.detailView;
    const auto game = selected();
    if (!game) {
        canvas_.text(label(Text::selectGame), {x, view.top + 12, right, std::min(view.top + 48, view.bottom)},
                     Font::body, muted, true, true);
        return;
    }
    canvas_.target()->PushAxisAlignedClip(view, D2D1_ANTIALIAS_MODE_ALIASED);
    controlClip_ = view;
    const bool compact = layout_.compact_details();
    const bool stacked = layout_.stack_detail_buttons();
    const float maximum = layout_.detail_max_scroll();
    const float controlsTop = view.bottom + maximum - layout_.detail_controls_height();
    const float controlsY = controlsTop - detailScroll_;
    const float coverHeight = compact ? std::min(156.0f, controlsTop - (area.top + 134)) :
        std::clamp(area.bottom - area.top - 276, 67.0f, 156.0f);
    float textY = view.top + 3 - detailScroll_;
    if (coverHeight >= 44) {
        draw_cover(box(x, area.top + 52 - detailScroll_, width, coverHeight), *game, true);
        textY = area.top + 60 + coverHeight - detailScroll_;
    }
    canvas_.text(game->title, box(x, textY, width, compact ? 24.0f : 31.0f), Font::title, white);
    const auto entry = choose_entry(*game, remembered_entry(*game));
    canvas_.text(formatted(state_.preferences.language, Text::folderName, game->id),
                 box(x, textY + (compact ? 24.0f : 32.0f), width, 19), Font::tiny, muted);
    canvas_.text(entry.empty() ? std::wstring(label(Text::entryRequired)) : L"SWF  " + entry,
                 box(x, textY + (compact ? 43.0f : 51.0f), width, 19),
                 Font::tiny, entry.empty() ? warning : muted);
    if (controlsY - (textY + 74) >= 22) {
        canvas_.text(game->description, {x, textY + 74, right, controlsY - 4}, Font::body, muted, false, true);
    }
    auto playLabel = Text::play;
    if (player_) {
        if (playerStartup_ == PlayerStartup::ready) playLabel = Text::playing;
        else if (playerStartup_ == PlayerStartup::unresponsive) playLabel = Text::forceStartup;
        else playLabel = Text::cancelStartup;
    }
    button(Action::play, box(x, controlsY, width, compact ? 27.0f : 35.0f), label(playLabel), true);
    const auto favoriteLabel = label(state_.favorites.contains(game->id) ? Text::removeFavorite : Text::addFavorite);
    const float half = (width - 6) / 2;
    const float folderY = controlsY + (compact ? 34.0f : 42.0f);
    const float buttonHeight = compact ? 25.0f : 30.0f;
    button(Action::folder, box(x, folderY, stacked ? width : half, buttonHeight), label(Text::openFolder));
    button(Action::favorite, stacked ? box(x, controlsY + 66, width, buttonHeight) :
                                       box(x + half + 6, folderY, half, buttonHeight), favoriteLabel);
    if (game->entries.size() > 1) {
        const float entryY = controlsY + (stacked ? 98.0f : compact ? 66.0f : 80.0f);
        button(Action::entry, box(x, entryY, width, compact ? 25.0f : 29.0f), label(Text::chooseEntry));
    }
    controlClip_.reset();
    canvas_.target()->PopAxisAlignedClip();
    pane_scrollbar(view, detailScroll_, maximum);
}

void Window::settings() {
    const float left = layout_.listPanel.left;
    const float width = layout_.detail.right - left;
    canvas_.text(label(Text::settings), box(left, 57, width, 44), Font::heading, white);
    const auto view = layout_.settingsView;
    canvas_.target()->PushAxisAlignedClip(view, D2D1_ANTIALIAS_MODE_ALIASED);
    controlClip_ = view;
    constexpr float pad = 13;
    const bool stacked = layout_.stack_settings();
    const float top = settingsTop - settingsScroll_;
    const float libraryTop = stacked ? top + settingsUpper + settingsGap : top;
    const float lower = libraryTop + settingsUpper + settingsGap;
    const float portableTop = stacked ? lower + settingsLower + settingsGap : lower;
    const float cardWidth = stacked ? width : (width - settingsGap) / 2;
    const float inner = cardWidth - 2 * pad;
    const auto card = [&](float x, float y, float height, Text title) {
        const auto rect = box(x, y, cardWidth, height);
        canvas_.fill(rect, panel, 9);
        canvas_.border(rect, border, 9);
        canvas_.text(label(title), box(x + pad, y + 7, inner, 29), Font::title, white);
    };
    const auto& prefs = state_.preferences;
    const float second = stacked ? left : left + cardWidth + settingsGap;
    const float x = left + pad, rightX = second + pad;
    card(left, top, settingsUpper, Text::general);
    canvas_.text(label(Text::language), box(x, top + 39, inner - 140, 27), Font::body, muted);
    button(Action::turkish, box(x + inner - 134, top + 39, 64, 27), label(Text::turkish), prefs.language == Language::tr);
    button(Action::english, box(x + inner - 64, top + 39, 64, 27), label(Text::english), prefs.language == Language::en);
    toggle(Action::autoScan, box(x, top + 70, inner, 27), Text::autoScan, prefs.autoScan);
    toggle(Action::rememberTab, box(x, top + 99, inner, 27), Text::rememberTab, prefs.rememberTab);
    toggle(Action::confirmClose, box(x, top + 128, inner, 27), Text::confirmClose, prefs.confirmClose);

    card(second, libraryTop, settingsUpper, Text::library);
    canvas_.text(label(Text::gamesFolder), box(rightX, libraryTop + 38, inner, 20), Font::strong, muted);
    settings_path(0, box(rightX, libraryTop + 58, inner, 23));
    constexpr float smallButton = 89;
    button(Action::gamesFolder, box(rightX, libraryTop + 88, smallButton, 28), label(Text::openFolder));
    button(Action::refresh, box(rightX + smallButton + 7, libraryTop + 88, inner - smallButton - 7, 28), label(Text::rescan));
    toggle(Action::scanNested, box(rightX, libraryTop + 128, inner, 27), Text::scanNested, prefs.scanNested);

    card(left, lower, settingsLower, Text::playback);
    canvas_.text(label(runtimeReady_ ? Text::ruffleFound : Text::ruffleMissing),
                 box(x, lower + 38, inner, 21), Font::strong, runtimeReady_ ? accent : warning);
    settings_path(1, box(x, lower + 59, inner, 22));
    toggle(Action::rememberSwf, box(x, lower + 100, inner, 27), Text::rememberSwf, prefs.rememberSwf);

    card(second, portableTop, settingsLower, Text::portable);
    canvas_.text(label(Text::savesFolder), box(rightX, portableTop + 38, inner, 20), Font::strong, muted);
    settings_path(2, box(rightX, portableTop + 59, inner, 22));
    const float half = (inner - 7) / 2;
    button(Action::savesFolder, box(rightX, portableTop + 99, half, 29), label(Text::openSaves));
    button(Action::clearCache, box(rightX + half + 7, portableTop + 99, half, 29), label(Text::clearCache));
    controlClip_.reset();
    canvas_.target()->PopAxisAlignedClip();
    pane_scrollbar(view, settingsScroll_, layout_.settings_max_scroll());
}

void Window::settings_path(unsigned index, Rect rect) {
    canvas_.text(settingsPaths_[index], rect, Font::path, white);
    const float visibleTop = std::max(rect.top, layout_.settingsView.top);
    const RECT toolRect{px(rect.left), px(visibleTop), px(rect.right),
                        px(std::max(visibleTop, std::min(rect.bottom, layout_.settingsView.bottom)))};
    if (!tooltips_ || EqualRect(&toolRect, &pathRects_[index])) return;
    pathRects_[index] = toolRect;
    TTTOOLINFOW tool{};
    tool.cbSize = sizeof(tool);
    tool.hwnd = window_;
    tool.uId = index + 1;
    tool.rect = toolRect;
    SendMessageW(tooltips_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
}

void Window::toggle(Action action, Rect rect, Text name, bool checked) {
    controls_.push_back({action, rect, controlClip_});
    if (hover_ == action || pressed_ == action) canvas_.fill(rect, hoverColor, 5);
    if (focus_ == action && GetFocus() == window_) canvas_.border(rect, accent, 5);
    canvas_.text(label(name), {rect.left + 3, rect.top, rect.right - 40, rect.bottom}, Font::body, white);
    const float middle = (rect.top + rect.bottom) / 2;
    const auto track = box(rect.right - 36, middle - 8, 33, 16);
    canvas_.fill(track, checked ? accentTint : inset, 8);
    canvas_.border(track, checked ? accent : muted, 8);
    canvas_.fill(box(checked ? track.right - 13 : track.left + 3, middle - 5, 10, 10), checked ? accent : muted, 5);
}

void Window::pane_scrollbar(Rect view, float offset, float maximum) {
    if (maximum <= 0) return;
    canvas_.fill({view.right + 2, view.top, view.right + 7, view.bottom}, inset, 2);
    canvas_.fill(scrollbar_thumb(view, offset, maximum), 0x59636f, 2);
}

void Window::paint() {
    PAINTSTRUCT paint{};
    const auto dc = BeginPaint(window_, &paint);
    const bool newTarget = !canvas_.target();
    if (tooltips_ && pathTooltipsActive_ != (page_ == Page::settings)) {
        pathTooltipsActive_ = page_ == Page::settings;
        SendMessageW(tooltips_, TTM_ACTIVATE, pathTooltipsActive_, 0);
    }
    bool painted = false;
    if (canvas_.begin(window_, dpi_)) {
        controls_.clear();
        controlClip_.reset();
        canvas_.target()->Clear(D2D1::ColorF(base));
        chrome();
        navigation();
        if (page_ == Page::settings) settings();
        else {
            library();
            details();
        }
        const float left = layout_.listPanel.left, bottom = layout_.height - 36;
        canvas_.line(layout_.navWidth + 1, bottom, layout_.width, bottom, border);
        const auto text = status_text();
        canvas_.text(text, {left, bottom + 2, layout_.width - 13, layout_.height - 3},
                     Font::body, statusError_ ? warning : muted);
        painted = canvas_.finish();
        if (!painted) release_target();
        else if (newTarget) request_covers();
    }
    if (!painted) {
        const auto background = CreateSolidBrush(RGB(17, 19, 21));
        FillRect(dc, &paint.rcPaint, background);
        DeleteObject(background);
        auto note = paint.rcPaint;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(230, 232, 236));
        DrawTextW(dc, label(Text::graphicsFailed).data(), -1, &note, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    EndPaint(window_, &paint);
    if (!painted && !recoveringGraphics_) redraw();
    recoveringGraphics_ = !painted;
}

void Window::release_target() {
    covers_.clear();
    canvas_.release();
    ++generation_;
    request_covers();
}

void Window::receive_library() {
    for (auto& result : worker_->take_library()) {
        if (result.failed) {
            workerFailed_ = true;
            deferredScan_ = false;
            scanning_ = false;
            set_status(Text::workerFailed, true);
            continue;
        }
        if (result.cacheCleared) {
            if (*result.cacheCleared) cacheError_ = false;
            covers_.clear();
            ++generation_;
            request_covers();
            set_status(*result.cacheCleared ? Text::cacheCleared : Text::cacheClearFailed, !*result.cacheCleared);
            continue;
        }
        if (result.scanned && (result.scanId < requestedScan_ || result.nested != state_.preferences.scanNested)) continue;
        dataReady_ = result.dataReady;
        runtimeReady_ = result.runtimeReady;
        if (result.state) {
            state_ = std::move(*result.state);
            stateLoaded_ = true;
            personalDataLoaded_ = result.dataReady;
            if (selectedId_.empty()) selectedId_ = state_.selection;
            if (!pageTouched_ && state_.preferences.rememberTab) {
                page_ = state_.preferences.lastPage;
                focus_ = page_ == Page::settings ? Action::settings : Action::list;
                ShowWindow(edit_, page_ == Page::settings ? SW_HIDE : SW_SHOW);
            }
            update_language();
        }
        if (result.cached) {
            games_ = std::move(*result.cached);
            rebuild_filter(false);
        }
        if (!result.scanned) {
            scanning_ = result.scanning;
            manualScanHint_ = !state_.preferences.autoScan && games_.empty();
            set_status(scanning_ ? Text::scanning : Text::empty, !runtimeReady_ || !dataReady_ || state_.damaged);
            if (std::exchange(deferredScan_, false)) rescan();
            continue;
        }
        scanning_ = false;
        covers_.clear();
        ++generation_;
        auto& report = *result.scanned;
        cacheFallback_ = report.games.empty() && !games_.empty() && !result.gamesReady;
        if (report.complete || games_.empty() || (report.limited && report.errors == 0)) {
            games_ = std::move(report.games);
        }
        incomplete_ = !result.gamesReady || report.errors != 0;
        limited_ = report.limited;
        cacheError_ = result.cacheError;
        badMetadata_ = report.badMetadata;
        manualScanHint_ = false;
        set_status(scanning_ ? Text::scanning : Text::empty, !report.complete || !result.dataReady ||
                   result.cacheError || state_.damaged || !runtimeReady_ || report.badMetadata > 0);
        OutputDebugStringW((L"FlashFasaFiso: catalog " + std::to_wstring(games_.size()) +
                            L", scan ms " + std::to_wstring(result.scanMs) + L"\n").c_str());
        rebuild_filter(false);
    }
}

void Window::receive_covers() {
    const auto pixels = worker_->take_covers();
    for (const auto& sample : pixels) {
        if (sample.request.generation != generation_) continue;
        Cover cover{nullptr, ++coverClock_};
        if (sample.width && canvas_.target()) {
            const auto prop = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            canvas_.target()->CreateBitmap(D2D1::SizeU(sample.width, sample.height), sample.bytes.data(),
                                          sample.width * 4, prop, &cover.bitmap);
        } else if (sample.width) {
            continue;
        }
        covers_.insert_or_assign(sample.request.path, std::move(cover));
        if (covers_.size() > coverLimit) {
            const auto least = std::ranges::min_element(covers_, {}, [](const auto& item) { return item.second.used; });
            covers_.erase(least);
        }
    }
    if (!pixels.empty()) redraw();
}

void Window::request_covers() {
    if (!worker_ || workerFailed_) return;
    std::vector<CoverRequest> requests;
    const auto add = [&](const Game& game) {
        if (game.cover.empty() || requests.size() >= coverLimit) return;
        auto path = L"games/" + game.id + L"/" + game.cover;
        if (covers_.contains(path) || std::ranges::any_of(requests, [&](const auto& req) { return req.path == path; })) {
            return;
        }
        requests.push_back({std::move(path), generation_});
    };
    if (page_ != Page::settings) {
        if (const auto game = selected()) add(*game);
        const auto first = static_cast<std::size_t>(scroll_ / rowHeight);
        const auto count = static_cast<std::size_t>(std::max(0.0f, layout_.list.bottom - layout_.list.top) / rowHeight) + 3;
        for (auto i = first; i < std::min(filtered_.size(), first + count); ++i) add(games_[filtered_[i]]);
    }
    std::ranges::reverse(requests);
    worker_->covers(std::move(requests));
}

const Game* Window::selected() const {
    return selectedRow_ < filtered_.size() ? &games_[filtered_[selectedRow_]] : nullptr;
}

void Window::rebuild_filter(bool resetOffset) {
    filtered_.clear();
    selectedRow_ = noRow;
    const auto query = fold_search(query_);
    for (std::size_t i = 0; i < games_.size(); ++i) {
        const auto& game = games_[i];
        if ((page_ == Page::favorites && !state_.favorites.contains(game.id)) || !search_match(game, query)) continue;
        if (equal_name(game.id, selectedId_)) selectedRow_ = filtered_.size();
        filtered_.push_back(i);
    }
    if (selectedRow_ == noRow && !filtered_.empty()) {
        selectedRow_ = 0;
        selectedId_ = games_[filtered_[0]].id;
    }
    hoveredRow_ = noRow;
    scroll_ = resetOffset ? 0 : std::min(scroll_, max_scroll());
    if (selectedRow_ != noRow) select_at(selectedRow_);
    else request_covers();
    redraw();
}

void Window::select_at(std::size_t row) {
    if (row >= filtered_.size()) return;
    selectedRow_ = row;
    if (selectedId_ != games_[filtered_[row]].id) detailScroll_ = 0;
    selectedId_ = games_[filtered_[row]].id;
    const float top = static_cast<float>(row) * rowHeight;
    const float visible = layout_.list.bottom - layout_.list.top;
    if (top < scroll_) scroll_ = top;
    if (top + rowHeight > scroll_ + visible) scroll_ = top + rowHeight - visible;
    scroll_ = std::clamp(scroll_, 0.0f, max_scroll());
    request_covers();
    redraw();
}

std::size_t Window::row_at(float x, float y) const {
    if (page_ == Page::settings || !inside(layout_.list, x, y)) return noRow;
    const auto index = static_cast<std::size_t>((y - layout_.list.top + scroll_) / rowHeight);
    return index < filtered_.size() ? index : noRow;
}

float Window::max_scroll() const {
    return std::max(0.0f, static_cast<float>(filtered_.size()) * rowHeight - (layout_.list.bottom - layout_.list.top));
}

Rect Window::scroll_thumb() const {
    return scrollbar_thumb(layout_.list, scroll_, max_scroll());
}

void Window::set_scroll(float offset) {
    const float next = std::clamp(offset, 0.0f, max_scroll());
    if (scroll_ != next) {
        scroll_ = next;
        hoveredRow_ = noRow;
        request_covers();
        redraw();
    }
}

float Window::pane_max_scroll() const {
    if (page_ == Page::settings) return layout_.settings_max_scroll();
    return selected() ? layout_.detail_max_scroll() : 0;
}

void Window::set_pane_scroll(float offset) {
    auto& current = page_ == Page::settings ? settingsScroll_ : detailScroll_;
    const float next = std::clamp(offset, 0.0f, pane_max_scroll());
    if (current != next) {
        current = next;
        redraw();
        UpdateWindow(window_);
    }
}

std::wstring Window::status_text() const {
    if (page_ == Page::settings && settingsUnsaved_) return std::wstring(label(Text::settingsUnsaved));
    const auto language = state_.preferences.language;
    if (status_ != Text::empty) return formatted(language, status_, statusArgument_);
    auto result = formatted(language, Text::found, std::to_wstring(games_.size()));
    if (manualScanHint_) result += label(Text::scanManually);
    if (incomplete_) result += label(cacheFallback_ ? Text::cacheFallback : Text::incomplete);
    if (limited_) result += label(Text::scanLimited);
    if (badMetadata_) result += formatted(language, Text::badMetadata, std::to_wstring(badMetadata_));
    if (!dataReady_ || !personalDataLoaded_ || cacheError_) result += label(Text::dataWrite);
    if (state_.damaged) result += label(Text::stateDamaged);
    if (!runtimeReady_) result += label(Text::runtimeAbsent);
    return result;
}

void Window::set_status(Text key, bool error, std::wstring argument) {
    status_ = key;
    statusArgument_ = std::move(argument);
    statusError_ = error;
    redraw();
}

void Window::focus(Action action) {
    focus_ = action;
    const auto target = action == Action::search ? edit_ : window_;
    if (GetFocus() != target) SetFocus(target);
    redraw();
}

void Window::switch_page(Page page) {
    pageTouched_ = true;
    if (page_ == page) return;
    page_ = page;
    detailScroll_ = 0;
    pressed_ = hover_ = Action::none;
    focus(page == Page::settings ? Action::settings : Action::list);
    ShowWindow(edit_, page_ == Page::settings ? SW_HIDE : SW_SHOW);
    rebuild_filter(true);
    UpdateWindow(window_);
}

void Window::favorite() {
    const auto game = selected();
    if (!game) return;
    const bool added = state_.favorites.erase(game->id) == 0;
    if (added) state_.favorites.insert(game->id);
    const bool saved = personalDataLoaded_ && ensure_directory(root_, L"data") && save_favorites(root_ / L"data", state_);
    set_status(saved ? (added ? Text::favoriteAdded : Text::favoriteRemoved) : Text::favoriteUnsaved, !saved);
    if (page_ == Page::favorites) rebuild_filter(false);
}

std::wstring_view Window::remembered_entry(const Game& game) const {
    const auto& entries = state_.preferences.rememberSwf ? state_.entries : sessionEntries_;
    const auto remembered = entries.find(game.id);
    return remembered == entries.end() ? std::wstring_view{} : std::wstring_view(remembered->second);
}

void Window::pick_entry(bool startGame) {
    if (closing_ || closeAfterPlayer_) return;
    if (startGame && player_) {
        if (WaitForSingleObject(player_, 0) == WAIT_OBJECT_0) {
            process_exited();
            if (!IsWindow(window_)) return;
        } else {
            check_player_startup();
            if (playerStartup_ == PlayerStartup::ready) set_status(Text::alreadyPlaying);
            else cancel_player_startup(false);
            return;
        }
    }
    const auto game = selected();
    if (!game) return;
    const auto id = game->id;
    const auto choices = game->entries;
    const auto entry = choose_entry(*game, remembered_entry(*game));
    if (startGame && !entry.empty()) {
        launch(*game, entry);
        return;
    }
    if (choices.size() < 2) return;
    const EntryChoices data{choices, entry, state_.preferences.language};
    const auto chosen = DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_SELECT_SWF), window_, choose_dialog,
                                        reinterpret_cast<LPARAM>(&data));
    if (chosen < 0) set_status(Text::dialogFailed, true);
    if (chosen <= 0 || static_cast<std::size_t>(chosen) > choices.size()) return;
    const auto updated = find_game(games_, id);
    const auto& filename = choices[static_cast<std::size_t>(chosen) - 1];
    if (!updated || !std::ranges::any_of(updated->entries, [&](const auto& item) { return equal_name(filename, item); })) {
        set_status(Text::entryChanged, true);
        return;
    }
    if (state_.preferences.rememberSwf) {
        state_.entries.insert_or_assign(id, filename);
        const bool saved = personalDataLoaded_ && ensure_directory(root_, L"data") && save_entries(root_ / L"data", state_);
        set_status(saved ? Text::entrySaved : Text::entryUnsaved, !saved);
    } else {
        sessionEntries_.insert_or_assign(id, filename);
        set_status(Text::entrySession);
    }
    if (startGame) launch(*updated, filename);
}

void Window::launch(const Game& game, std::wstring_view entry) {
    if (player_) {
        set_status(Text::alreadyPlaying);
        return;
    }
    auto result = play(root_, game.id, entry);
    if (!result.process) {
        set_status(result.error, true, std::to_wstring(result.windowsError));
        return;
    }
    player_ = result.process;
    playerStartedAt_ = GetTickCount64();
    playingTitle_ = game.title;
    SetTimer(window_, playerStartupTimer, playerWindowPollMs, nullptr);
    set_status(Text::playerStarting, false, playingTitle_);
}

void Window::player_ready() {
    playerStartup_ = PlayerStartup::ready;
    KillTimer(window_, playerStartupTimer);
    set_status(Text::playerStarted, false, playingTitle_);
}

void Window::check_player_startup() {
    if (!player_ || playerStartup_ == PlayerStartup::ready) return;
    const auto elapsed = GetTickCount64() - playerStartedAt_;
    if (player_windows(player_).visible) {
        player_ready();
        if (std::exchange(closeAfterPlayer_, false)) finish_close();
    } else if (playerStartup_ == PlayerStartup::stopping && elapsed >= playerCloseNoticeMs) {
        KillTimer(window_, playerStartupTimer);
        playerStartup_ = PlayerStartup::unresponsive;
        closeAfterPlayer_ = false;
        set_status(Text::playerCancelFailed, true);
    } else if (playerStartup_ == PlayerStartup::waiting && elapsed >= playerStartupNoticeMs) {
        KillTimer(window_, playerStartupTimer);
        set_status(Text::playerNoWindow, true);
    }
}

void Window::cancel_player_startup(bool closeAfter) {
    if (closing_ || playerStartup_ == PlayerStartup::stopping) return;
    const bool force = playerStartup_ == PlayerStartup::unresponsive;
    const auto title = force ? Text::forceStartup : Text::cancelStartup;
    const auto prompt = force ? (closeAfter ? Text::playerForceClosePrompt : Text::playerForcePrompt) :
                               (closeAfter ? Text::playerCancelClosePrompt : Text::playerCancelPrompt);
    closing_ = true;
    const auto accepted = localized_message(window_, state_.preferences.language, prompt, title, IDI_QUESTION, true);
    closing_ = false;
    if (accepted != IDOK) return;

    if (WaitForSingleObject(player_, 0) == WAIT_OBJECT_0) {
        process_exited();
        if (closeAfter) finish_close();
        return;
    }
    const auto windows = player_windows(player_);
    if (playerStartup_ == PlayerStartup::ready || windows.visible || (windows.hidden && IsWindowVisible(windows.hidden))) {
        player_ready();
        if (closeAfter) finish_close();
        return;
    }
    DWORD pid = 0;
    if (windows.hidden) GetWindowThreadProcessId(windows.hidden, &pid);
    if (pid && pid == windows.process && IsWindowVisible(windows.hidden)) {
        player_ready();
        if (closeAfter) finish_close();
        return;
    }
    const bool requested = force ? TerminateProcess(player_, ERROR_CANCELLED) != FALSE :
        pid && pid == windows.process && PostMessageW(windows.hidden, WM_CLOSE, 0, 0);
    if (!requested) {
        KillTimer(window_, playerStartupTimer);
        playerStartup_ = PlayerStartup::unresponsive;
        set_status(Text::playerCancelFailed, true);
        return;
    }
    playerStartedAt_ = GetTickCount64();
    const auto timer = SetTimer(window_, playerStartupTimer, playerWindowPollMs, nullptr);
    playerStartup_ = timer ? PlayerStartup::stopping : PlayerStartup::unresponsive;
    closeAfterPlayer_ = closeAfter && timer;
    set_status(force ? Text::playerForcePending : Text::playerCancelPending);
}

void Window::process_exited() {
    DWORD code = 0;
    const auto ok = GetExitCodeProcess(player_, &code);
    if (!ok) code = GetLastError();
    CloseHandle(player_);
    player_ = nullptr;
    KillTimer(window_, playerStartupTimer);
    const auto startup = std::exchange(playerStartup_, PlayerStartup::waiting);
    const bool cancelling = startup == PlayerStartup::stopping || startup == PlayerStartup::unresponsive;
    if (ok && cancelling && (code == 0 || code == ERROR_CANCELLED)) set_status(Text::playerCancelled, false, playingTitle_);
    else if (ok && code == 0) set_status(Text::playerClosed, false, playingTitle_);
    else {
        set_status(Text::playerFailed, true, std::to_wstring(code));
    }
    playingTitle_.clear();
    if (std::exchange(closeAfterPlayer_, false)) finish_close();
}

void Window::finish_close() {
    const auto& prefs = state_.preferences;
    if (stateLoaded_ && (settingsUnsaved_ || (prefs.rememberTab && prefs.lastPage != page_) ||
        (!selectedId_.empty() && !equal_name(state_.selection, selectedId_)))) {
        if (!persist_settings()) OutputDebugStringW(L"FlashFasaFiso: settings.ini could not be saved.\n");
    }
    DestroyWindow(window_);
}

void Window::open_folder(Action action) {
    std::optional<fs::path> path;
    if (action == Action::gamesFolder || action == Action::savesFolder) {
        const auto relative = action == Action::gamesFolder ? L"games" : L"data/saves";
        ensure_directory(root_, relative);
        path = safe_existing(root_, relative, true);
    } else if (const auto game = selected()) {
        path = safe_existing(root_, L"games/" + game->id, true);
    }
    if (!path) {
        set_status(Text::folderUnavailable, true);
        return;
    }
    const auto opened = reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"explore", path->c_str(), nullptr,
                                                               nullptr, SW_SHOWNORMAL));
    if (opened <= 32) set_status(Text::explorerFailed, true, std::to_wstring(opened));
}

void Window::rescan() {
    if (workerFailed_) {
        set_status(Text::workerFailed, true);
        return;
    }
    if (!stateLoaded_) {
        deferredScan_ = true;
        return;
    }
    scanning_ = true;
    requestedScan_ = worker_->refresh(state_.preferences.scanNested);
    set_status(Text::scanning);
}

void Window::update_language() {
    SendMessageW(edit_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(label(Text::search).data()));
    redraw();
}

bool Window::persist_settings() {
    if (!selectedId_.empty()) state_.selection = selectedId_;
    if (state_.preferences.rememberTab) state_.preferences.lastPage = page_;
    settingsUnsaved_ = !personalDataLoaded_ || !ensure_directory(root_, L"data") || !save_settings(root_ / L"data", state_);
    return !settingsUnsaved_;
}

void Window::change_setting(Action action) {
    if (!stateLoaded_) return;
    auto& prefs = state_.preferences;
    switch (action) {
        case Action::turkish:
        case Action::english:
            prefs.language = action == Action::turkish ? Language::tr : Language::en;
            update_language();
            break;
        case Action::autoScan: prefs.autoScan = !prefs.autoScan; break;
        case Action::rememberTab: prefs.rememberTab = !prefs.rememberTab; break;
        case Action::confirmClose: prefs.confirmClose = !prefs.confirmClose; break;
        case Action::scanNested: prefs.scanNested = !prefs.scanNested; break;
        case Action::rememberSwf: prefs.rememberSwf = !prefs.rememberSwf; break;
        default: return;
    }
    const auto saved = persist_settings();
    if (action == Action::scanNested) rescan();
    else set_status(saved ? Text::settingsSaved : Text::settingsUnsaved, !saved);
}

void Window::activate(Action action) {
    switch (action) {
        case Action::library: switch_page(Page::library); break;
        case Action::favorites: switch_page(Page::favorites); break;
        case Action::settings: switch_page(Page::settings); break;
        case Action::refresh: rescan(); break;
        case Action::gamesFolder:
        case Action::folder:
        case Action::savesFolder: open_folder(action); break;
        case Action::clearCache:
            if (workerFailed_) set_status(Text::workerFailed, true);
            else if (stateLoaded_) worker_->clear_cache();
            break;
        case Action::play: pick_entry(true); break;
        case Action::favorite: favorite(); break;
        case Action::entry: pick_entry(false); break;
        case Action::minimize: ShowWindow(window_, SW_MINIMIZE); break;
        case Action::maximize: ShowWindow(window_, IsZoomed(window_) ? SW_RESTORE : SW_MAXIMIZE); break;
        case Action::close: SendMessageW(window_, WM_CLOSE, 0, 0); break;
        case Action::turkish:
        case Action::english:
        case Action::autoScan:
        case Action::rememberTab:
        case Action::confirmClose:
        case Action::scanNested:
        case Action::rememberSwf: change_setting(action); break;
        default: break;
    }
}

Action Window::control_at(float x, float y) const {
    for (const auto& control : controls_) {
        if (inside(control.rect, x, y) && (!control.clip || inside(*control.clip, x, y))) return control.action;
    }
    if (page_ != Page::settings && inside(layout_.search, x, y)) return Action::search;
    return Action::none;
}

void Window::tab() {
    std::vector<Action> order{Action::library, Action::favorites, Action::settings};
    if (page_ != Page::settings) {
        order.insert(order.end(), {Action::search, Action::refresh, Action::list});
        if (const auto game = selected()) {
            order.insert(order.end(), {Action::play, Action::folder, Action::favorite});
            if (game->entries.size() > 1) order.push_back(Action::entry);
        }
        order.push_back(Action::gamesFolder);
    } else {
        order.insert(order.end(), {Action::turkish, Action::english, Action::autoScan, Action::rememberTab,
            Action::confirmClose, Action::gamesFolder, Action::refresh, Action::scanNested,
            Action::rememberSwf, Action::savesFolder, Action::clearCache});
    }
    order.insert(order.end(), {Action::minimize, Action::maximize, Action::close});
    const auto current = std::ranges::find(order, focus_);
    const bool backwards = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const auto index = current == order.end() ? (backwards ? 0 : order.size() - 1) :
        static_cast<std::size_t>(current - order.begin());
    const auto next = order[(index + (backwards ? order.size() - 1 : 1)) % order.size()];
    focus(next);
    const auto control = std::ranges::find(controls_, next, &Control::action);
    if (control != controls_.end() && control->clip) {
        const auto& rect = control->rect;
        const auto& clip = control->clip;
        if (rect.top < clip->top) set_pane_scroll(pane_scroll() + rect.top - clip->top);
        else if (rect.bottom > clip->bottom) set_pane_scroll(pane_scroll() + rect.bottom - clip->bottom);
    }
}

bool Window::key(WPARAM code, bool fromSearch) {
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (ctrl && code == 'F') {
        if (page_ == Page::settings) switch_page(Page::library);
        focus(Action::search);
        SendMessageW(edit_, EM_SETSEL, 0, -1);
    } else if (code == VK_F5) activate(Action::refresh);
    else if (ctrl && code == 'D' && page_ != Page::settings) favorite();
    else if (ctrl && code == 'O') open_folder(selected() && page_ != Page::settings ? Action::folder : Action::gamesFolder);
    else if (ctrl && code == 'E' && page_ != Page::settings) pick_entry(false);
    else if (code == VK_TAB) tab();
    else if (code == VK_ESCAPE && fromSearch) {
        if (!query_.empty()) SetWindowTextW(edit_, L"");
        else focus(Action::list);
    } else if (code == VK_RETURN || (code == VK_SPACE && !fromSearch)) {
        if (fromSearch || focus_ == Action::list) activate(Action::play);
        else activate(focus_);
    } else if (page_ != Page::settings && !filtered_.empty() &&
               (code == VK_DOWN || code == VK_UP || (!fromSearch &&
                (code == VK_HOME || code == VK_END || code == VK_PRIOR || code == VK_NEXT)))) {
        const auto count = static_cast<int>(filtered_.size());
        int row = static_cast<int>(selectedRow_);
        const int page = std::max(1, static_cast<int>((layout_.list.bottom - layout_.list.top) / rowHeight));
        switch (code) {
            case VK_UP: --row; break;
            case VK_DOWN: ++row; break;
            case VK_HOME: row = 0; break;
            case VK_END: row = count - 1; break;
            case VK_PRIOR: row -= page; break;
            case VK_NEXT: row += page; break;
            default: break;
        }
        focus(Action::list);
        select_at(static_cast<std::size_t>(std::clamp(row, 0, count - 1)));
    } else return false;
    return true;
}

bool Window::create() {
    dpi_ = window_dpi(window_);
    editBackground_ = CreateSolidBrush(RGB(0x1d, 0x21, 0x27));
    edit_ = CreateWindowExW(0, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                            0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    if (!edit_ || !editBackground_ ||
        !SetWindowSubclass(edit_, edit_procedure, 1, reinterpret_cast<DWORD_PTR>(this))) {
        return false;
    }
    SendMessageW(edit_, EM_SETLIMITTEXT, 512, 0);
    SendMessageW(edit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    update_language();
    tooltips_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                               window_, nullptr, instance_, nullptr);
    if (tooltips_) {
        for (unsigned i = 0; i < settingsPaths_.size(); ++i) {
            TTTOOLINFOW tool{};
            tool.cbSize = sizeof(tool);
            tool.uFlags = TTF_SUBCLASS | TTF_TRANSPARENT;
            tool.hwnd = window_;
            tool.uId = i + 1;
            tool.lpszText = settingsPaths_[i].data();
            SendMessageW(tooltips_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        }
        SendMessageW(tooltips_, TTM_ACTIVATE, FALSE, 0);
    }
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUNDSMALL;
    DwmSetWindowAttribute(window_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    const auto font = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    editFont_ = font;
    SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), FALSE);
    resize();
    worker_ = std::make_unique<Worker>(window_, root_);
    return true;
}

void Window::resize() {
    RECT client{};
    GetClientRect(window_, &client);
    if (client.right <= 0 || client.bottom <= 0) return;
    layout_.size(dip(client.right), dip(client.bottom));
    if (edit_) SetWindowPos(edit_, nullptr, px(layout_.search.left + 36), px(layout_.search.top + 7),
                            std::max(1, px(layout_.search.right - layout_.search.left - 46)), px(22),
                            SWP_NOZORDER | SWP_NOACTIVATE);
    if (!canvas_.resize(client.right, client.bottom)) release_target();
    scroll_ = std::min(scroll_, max_scroll());
    settingsScroll_ = std::min(settingsScroll_, layout_.settings_max_scroll());
    detailScroll_ = std::min(detailScroll_, layout_.detail_max_scroll());
    request_covers();
    redraw();
}

LRESULT Window::hit_test(LPARAM position) const {
    POINT point{GET_X_LPARAM(position), GET_Y_LPARAM(position)};
    ScreenToClient(window_, &point);
    RECT r{};
    GetClientRect(window_, &r);
    if (!IsZoomed(window_)) {
        const auto padding = dpi_metric(SM_CXPADDEDBORDER, dpi_);
        const auto xBorder = dpi_metric(SM_CXSIZEFRAME, dpi_) + padding;
        const auto yBorder = dpi_metric(SM_CYSIZEFRAME, dpi_) + padding;
        const bool top = point.y < yBorder, bottom = point.y >= r.bottom - yBorder;
        const bool left = point.x < xBorder, right = point.x >= r.right - xBorder;
        if (top) return left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
        if (bottom) return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
    }
    const float x = dip(point.x), y = dip(point.y);
    if (inside(layout_.caption[1], x, y)) return HTMAXBUTTON;
    if (inside(layout_.caption[0], x, y) || inside(layout_.caption[2], x, y)) return HTCLIENT;
    return y < titleHeight ? HTCAPTION : HTCLIENT;
}

void Window::mouse_move(float x, float y) {
    if (dragging_) {
        const auto view = draggingPane_ ? pane_view() : layout_.list;
        const auto maximum = draggingPane_ ? pane_max_scroll() : max_scroll();
        const auto thumb = scrollbar_thumb(view, draggingPane_ ? pane_scroll() : scroll_, maximum);
        const float travel = (view.bottom - view.top) - (thumb.bottom - thumb.top);
        if (travel > 0) {
            const auto offset = (y - dragAnchor_ - view.top) * maximum / travel;
            if (draggingPane_) set_pane_scroll(offset);
            else set_scroll(offset);
        }
        return;
    }
    const auto control = control_at(x, y);
    const auto row = row_at(x, y);
    if (control != hover_ || row != hoveredRow_) {
        hover_ = control;
        hoveredRow_ = row;
        redraw();
    }
    if (!tracking_) {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0};
        tracking_ = TrackMouseEvent(&track) != FALSE;
    }
}

LRESULT CALLBACK Window::edit_procedure(HWND edit, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR reference) {
    auto& self = *reinterpret_cast<Window*>(reference);
    if (message == WM_NCDESTROY) RemoveWindowSubclass(edit, edit_procedure, id);
    if (message == WM_SETFOCUS) {
        self.focus_ = Action::search;
        self.redraw();
    }
    if (message == WM_KILLFOCUS) self.redraw();
    if (message == WM_KEYDOWN) {
        try {
            if (self.key(w, true)) return 0;
        } catch (...) {
            PostMessageW(self.window_, WM_CLOSE, 0, 0);
            return 0;
        }
    }
    if (message == WM_CHAR && (w == VK_TAB || w == VK_RETURN || w == VK_ESCAPE)) return 0;
    return DefSubclassProc(edit, message, w, l);
}

LRESULT Window::message(UINT message, WPARAM w, LPARAM l) {
    switch (message) {
        case WM_CREATE: return create() ? 0 : -1;
        case WM_CLOSE: {
            if (closing_ || closeAfterPlayer_) return 0;
            if (player_ && WaitForSingleObject(player_, 0) == WAIT_OBJECT_0) process_exited();
            check_player_startup();
            if (player_ && playerStartup_ != PlayerStartup::ready) {
                cancel_player_startup(true);
                return 0;
            }
            const auto& prefs = state_.preferences;
            if (prefs.confirmClose) {
                closing_ = true;
                const auto accepted = DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_CONFIRM_CLOSE), window_, close_dialog,
                                                       static_cast<LPARAM>(prefs.language));
                closing_ = false;
                if (accepted != IDOK) return 0;
            }
            finish_close();
            return 0;
        }
        case WM_DESTROY:
            worker_.reset();
            PostQuitMessage(0);
            return 0;
        case WM_PAINT: paint(); return 0;
        case WM_TIMER:
            if (w == playerStartupTimer) { check_player_startup(); return 0; }
            break;
        case WM_ACTIVATE:
            if (LOWORD(w) != WA_INACTIVE) check_player_startup();
            if (!IsWindow(window_)) return 0;
            break;
        case WM_ERASEBKGND: return 1;
        case WM_SIZE: if (w != SIZE_MINIMIZED) resize(); return 0;
        case WM_DISPLAYCHANGE: release_target(); redraw(); return 0;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(w);
            const auto frame = reinterpret_cast<const RECT*>(l);
            const auto newFont = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            if (newFont) {
                SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(newFont), TRUE);
                if (editFont_) DeleteObject(editFont_);
                editFont_ = newFont;
            }
            SetWindowPos(window_, nullptr, frame->left, frame->top, frame->right - frame->left,
                         frame->bottom - frame->top, SWP_NOACTIVATE | SWP_NOZORDER);
            resize();
            return 0;
        }
        case WM_NCCALCSIZE:
            if (w && IsZoomed(window_)) {
                auto& frame = reinterpret_cast<NCCALCSIZE_PARAMS*>(l)->rgrc[0];
                MONITORINFO info{sizeof(info), {}, {}, 0};
                if (GetMonitorInfoW(MonitorFromRect(&frame, MONITOR_DEFAULTTONEAREST), &info)) {
                    IntersectRect(&frame, &frame, &info.rcWork);
                }
            }
            return 0;
        case WM_GETMINMAXINFO: {
            auto& info = *reinterpret_cast<MINMAXINFO*>(l);
            info.ptMinTrackSize = {px(760), px(480)};
            return 0;
        }
        case WM_NCHITTEST: return hit_test(l);
        case WM_NCMOUSEMOVE:
            if (w == HTMAXBUTTON) {
                if (hover_ != Action::maximize) {
                    hover_ = Action::maximize;
                    hoveredRow_ = noRow;
                    redraw();
                }
                if (!ncTracking_) {
                    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE | TME_NONCLIENT, window_, 0};
                    ncTracking_ = TrackMouseEvent(&track) != FALSE;
                }
            } else if (hover_ == Action::maximize) {
                hover_ = Action::none;
                redraw();
            }
            break;
        case WM_NCMOUSELEAVE:
            ncTracking_ = false;
            if (hover_ == Action::maximize) { hover_ = Action::none; redraw(); }
            break;
        case WM_NCLBUTTONDOWN:
            if (w == HTMAXBUTTON) {
                pressed_ = Action::maximize;
                SetCapture(window_);
                redraw();
                return 0;
            }
            break;
        case WM_MOUSELEAVE:
            tracking_ = false;
            if (hover_ != Action::maximize) hover_ = Action::none;
            hoveredRow_ = noRow;
            redraw();
            return 0;
        case WM_MOUSEMOVE: mouse_move(dip(GET_X_LPARAM(l)), dip(GET_Y_LPARAM(l))); return 0;
        case WM_MOUSEWHEEL:
            {
                POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                ScreenToClient(window_, &point);
                const auto view = pane_view();
                const bool pane = page_ == Page::settings || (pane_max_scroll() > 0 &&
                    inside({view.left, layout_.detail.top, view.right + 10, view.bottom}, dip(point.x), dip(point.y)));
                UINT lines = 3;
                SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
                wheelRemainder_ += GET_WHEEL_DELTA_WPARAM(w);
                const auto steps = wheelRemainder_ / WHEEL_DELTA;
                wheelRemainder_ %= WHEEL_DELTA;
                const float height = pane ? view.bottom - view.top : layout_.list.bottom - layout_.list.top;
                const float amount = lines == WHEEL_PAGESCROLL ? height : static_cast<float>(lines) * (pane ? 28 : rowHeight);
                if (pane) set_pane_scroll(pane_scroll() - static_cast<float>(steps) * amount);
                else set_scroll(scroll_ - static_cast<float>(steps) * amount);
            }
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            const float x = dip(GET_X_LPARAM(l)), y = dip(GET_Y_LPARAM(l));
            const auto action = control_at(x, y);
            const auto view = pane_view();
            const auto maximum = pane_max_scroll();
            if (action == Action::search) {
                focus(action);
            } else if (action != Action::none) {
                if (action != Action::maximize && action != Action::minimize && action != Action::close) focus(action);
                pressed_ = action;
                SetCapture(window_);
                redraw();
            } else if (const auto row = row_at(x, y); row != noRow) {
                focus(Action::list);
                select_at(row);
                if (message == WM_LBUTTONDBLCLK) activate(Action::play);
            } else if (maximum > 0 && inside({view.right, view.top, view.right + 10, view.bottom}, x, y)) {
                const auto thumb = scrollbar_thumb(view, pane_scroll(), maximum);
                if (y >= thumb.top && y < thumb.bottom) {
                    dragging_ = draggingPane_ = true;
                    dragAnchor_ = y - thumb.top;
                    SetCapture(window_);
                } else {
                    const float page = view.bottom - view.top;
                    set_pane_scroll(pane_scroll() + (y < thumb.top ? -page : page));
                }
            } else if (page_ != Page::settings && max_scroll() > 0 &&
                       inside({layout_.list.right, layout_.list.top, layout_.list.right + 10, layout_.list.bottom}, x, y)) {
                const auto thumb = scroll_thumb();
                if (y >= thumb.top && y < thumb.bottom) {
                    dragging_ = true;
                    draggingPane_ = false;
                    dragAnchor_ = y - thumb.top;
                    SetCapture(window_);
                } else {
                    const float page = layout_.list.bottom - layout_.list.top;
                    set_scroll(scroll_ + (y < thumb.top ? -page : page));
                }
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            const auto action = std::exchange(pressed_, Action::none);
            dragging_ = false;
            draggingPane_ = false;
            if (GetCapture() == window_) ReleaseCapture();
            if (action != Action::none && action == control_at(dip(GET_X_LPARAM(l)), dip(GET_Y_LPARAM(l)))) activate(action);
            if (IsWindow(window_)) redraw();
            return 0;
        }
        case WM_CAPTURECHANGED:
            pressed_ = Action::none;
            dragging_ = false;
            draggingPane_ = false;
            redraw();
            return 0;
        case WM_CTLCOLOREDIT:
            SetTextColor(reinterpret_cast<HDC>(w), RGB(0xe6, 0xe8, 0xec));
            SetBkColor(reinterpret_cast<HDC>(w), RGB(0x1d, 0x21, 0x27));
            return reinterpret_cast<LRESULT>(editBackground_);
        case WM_COMMAND:
            if (reinterpret_cast<HWND>(l) == edit_ && HIWORD(w) == EN_CHANGE) {
                std::array<wchar_t, 513> text{};
                GetWindowTextW(edit_, text.data(), static_cast<int>(text.size()));
                query_ = text.data();
                rebuild_filter(true);
            }
            return 0;
        case WM_KEYDOWN: if (key(w, false)) return 0; break;
        case WM_SETFOCUS:
            if (focus_ == Action::search) focus_ = Action::list;
            redraw();
            return 0;
        case WM_KILLFOCUS: redraw(); return 0;
        case workerMessage: if (worker_) receive_library(); return 0;
        case coverMessage: if (worker_) receive_covers(); return 0;
        default: break;
    }
    return DefWindowProcW(window_, message, w, l);
}

LRESULT CALLBACK Window::procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
    auto self = reinterpret_cast<Window*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window, message, w, l);
    try {
        return self->message(message, w, l);
    } catch (...) {
        if (message == WM_CREATE) return -1;
        if (message == WM_NCCREATE) return FALSE;
        PostMessageW(window, WM_CLOSE, 0, 0);
        return 0;
    }
}

int Window::run(int show) {
    const SingleInstance instanceLock(root_);
    if (instanceLock.duplicate) {
        const auto data = safe_existing(root_, L"data", true);
        const auto language = data ? load_state(*data).preferences.language : defaultLanguage;
        localized_message(nullptr, language, Text::duplicate, Text::empty, IDI_INFORMATION);
        return 0;
    }
    INITCOMMONCONTROLSEX init{sizeof(init), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
    InitCommonControlsEx(&init);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = procedure;
    wc.hInstance = instance_;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = windowClass;
    if (!RegisterClassExW(&wc)) throw std::runtime_error("RegisterClassExW failed");
    POINT cursor{};
    GetCursorPos(&cursor);
    const auto monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO display{sizeof(display), {}, {}, 0};
    if (!GetMonitorInfoW(monitor, &display)) throw std::runtime_error("GetMonitorInfoW failed");
    const auto work = display.rcWork;
    const auto window = CreateWindowExW(0, windowClass, L"FlashFasaFiso", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        work.left + 30, work.top + 30, 1100, 700, nullptr, nullptr, instance_, this);
    if (!window) throw std::runtime_error("CreateWindowExW failed");
    const auto width = std::min(px(1100), static_cast<int>(work.right - work.left));
    const auto height = std::min(px(700), static_cast<int>(work.bottom - work.top));
    SetWindowPos(window, nullptr, work.left + (work.right - work.left - width) / 2,
        work.top + (work.bottom - work.top - height) / 2, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(window, show);
    UpdateWindow(window);
    for (;;) {
        const auto count = player_ ? 1u : 0u;
        const auto wait = MsgWaitForMultipleObjectsEx(count, player_ ? &player_ : nullptr, INFINITE,
                                                       QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait == WAIT_FAILED) return 1;
        if (player_ && wait == WAIT_OBJECT_0) process_exited();
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return static_cast<int>(msg.wParam);
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}
}

int run_window(HINSTANCE instance, int show) { return Window(instance).run(show); }

int localized_message(HWND owner, Language language, Text message, Text title, PCWSTR icon, bool confirm) {
    struct Message {
        Language language;
        PCWSTR title;
        PCWSTR body;
        PCWSTR icon;
        bool confirm;
    } data{language, title == Text::empty ? L"FlashFasaFiso" : text(language, title).data(),
           text(language, message).data(), icon, confirm};
    const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_LOCALIZED_MESSAGE), owner,
        [](HWND dialog, UINT event, WPARAM w, LPARAM l) -> INT_PTR {
            if (event == WM_INITDIALOG) {
                const auto& content = *reinterpret_cast<const Message*>(l);
                SetWindowTextW(dialog, content.title);
                SetDlgItemTextW(dialog, IDC_DIALOG_PROMPT, content.body);
                SetDlgItemTextW(dialog, IDOK, text(content.language, Text::ok).data());
                SetDlgItemTextW(dialog, IDCANCEL, text(content.language, Text::cancel).data());
                SendDlgItemMessageW(dialog, IDC_DIALOG_ICON, STM_SETICON,
                                    reinterpret_cast<WPARAM>(LoadIconW(nullptr, content.icon)), 0);
                if (!content.confirm) {
                    ShowWindow(GetDlgItem(dialog, IDCANCEL), SW_HIDE);
                    SendMessageW(dialog, DM_SETDEFID, IDOK, 0);
                }
                SetFocus(GetDlgItem(dialog, content.confirm ? IDCANCEL : IDOK));
                return FALSE;
            }
            if (event == WM_COMMAND && (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL)) {
                EndDialog(dialog, LOWORD(w));
                return TRUE;
            }
            return FALSE;
        }, reinterpret_cast<LPARAM>(&data));
    if (result != -1) return static_cast<int>(result);
    const auto style = confirm ? MB_OKCANCEL | MB_DEFBUTTON2 | MB_ICONQUESTION :
        MB_OK | (icon == IDI_ERROR ? MB_ICONERROR : MB_ICONINFORMATION);
    return MessageBoxW(owner, data.body, data.title, style);
}
}
