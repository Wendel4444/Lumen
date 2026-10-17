#include "App.h"

#include <dwmapi.h>
#include <windowsx.h>
#include <netlistmgr.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <random>

#include "core/Util.h"

namespace lumen {

using namespace ui;
using D2D1::Matrix3x2F;

namespace {
constexpr float kMargin = 72.f;
const wchar_t* kTabNames[] = {L"Início", L"Biblioteca", L"Ofertas", L"Estatísticas", L"Ajustes"};
constexpr int kTabCount = 5;

float EaseOut(float t) {
    t = std::clamp(t, 0.f, 1.f);
    return 1.f - (1.f - t) * (1.f - t) * (1.f - t);
}

bool SystemPrefersDark() {
    return RegReadDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        L"AppsUseLightTheme", 0) == 0;
}

LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return app ? app->WndProc(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}
}  // namespace

// --- Lifecycle -------------------------------------------------------------

double App::Wall() const {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - qpcStart_.QuadPart) / static_cast<double>(qpcFreq_.QuadPart);
}

int App::Run(HINSTANCE inst, const LaunchOptions& opts) {
    QueryPerformanceFrequency(&qpcFreq_);
    QueryPerformanceCounter(&qpcStart_);
    qpcLast_ = qpcStart_;
    ownBuild_ = OwnBuildTime();
    VideoPlayer::Startup();
    opts_ = opts;
    snapshotMode_ = !opts.snapshot.empty();
    settings_.Load();
    user_.Load();
    if (opts.restoreLook && !snapshotMode_) {
        // Undo look-related switches (some were flipped by the old mis-click bug in Settings).
        settings_.reduceMotion = false;
        settings_.blur = true;
        settings_.onlineData = true;
        settings_.remindInterval = 2;
        settingsDirty_ = true;
    }
    if (snapshotMode_) {
        if (opts.defaults) settings_ = Settings{};  // render with factory settings (nothing is saved)
        if (opts.light) settings_.theme = ThemeMode::Light;
        tab_ = opts.tab;
    }
    ApplyTheme();

    library_.Refresh();
    images_.SetOnline(settings_.onlineData);
    images_.Start();
    trailers_.SetOnline(settings_.onlineData);
    trailers_.Start();
    deals_.SetOnline(settings_.onlineData);
    deals_.Start();
    meta_.SetConfig(MetaConfig());
    meta_.Start();
    RequestStoreArt();

    RebuildHomeList();
    if (!settings_.lastSelected.empty()) {
        for (size_t i = 0; i < show_.list.size(); ++i)
            if (library_.games()[show_.list[i]].id == settings_.lastSelected) show_.sel = static_cast<int>(i);
    }
    if (snapshotMode_ && !show_.list.empty()) show_.sel = std::clamp(opts.game, 0, static_cast<int>(show_.list.size()) - 1);
    if (!show_.list.empty()) show_.selId = library_.games()[show_.list[show_.sel]].id;

    if (!CreateMainWindow(inst)) return 1;
    if (!gfx_.Init(hwnd_)) {
        MessageBoxW(hwnd_, L"Não foi possível inicializar o Direct2D/Direct3D neste computador.", L"Lumen",
                    MB_ICONERROR);
        return 1;
    }
    ctx_ = new Ctx(gfx_, images_, input_.frame(), theme_, settings_);

    if (!opts.addPath.empty()) AddGameFromPath(opts.addPath, true);
    if (snapshotMode_) {
        if (opts.page && !opts.search.empty()) {
            for (const auto& gm : library_.games())
                if (ContainsInsensitive(gm.name, opts.search)) {
                    OpenGame(gm.id, Tab::Home);
                    break;
                }
        } else if (opts.page && !show_.list.empty() && opts.addPath.empty()) {
            OpenGame(show_.selId, Tab::Home);
        }
        if (opts.tab != Tab::Home) tab_ = opts.tab;
        lib_.search = opts.search;
        tabChangedAt_ = -10;
        if (opts.sheet && SelectedGame()) OpenGameMenu(*SelectedGame());
        profileOpen_ = opts.profile;
        if (opts.celebrate && SelectedGame())
            celebrations_.push_back(Celebration{Celebration::Kind::Achievements, SelectedGame()->id, 3});
        if (opts.launchFx && SelectedGame()) launchFx_ = {SelectedGame()->id, opts.wait - 1.0};
    } else {
        sound_.Init();
        SetupTray();
        // Started with Windows: stay in the tray (reminders work, no window in the way).
        if (!(opts.startInTray && trayAdded_)) {
            ShowWindow(hwnd_, SW_SHOW);
            if (settings_.startFullscreen) ToggleFullscreen();
        }
        if (opts.notifyTest) SendTestNotification();
        // Keep the startup entry pointing at this executable if the user enabled it.
        if (LaunchAtStartup()) SetLaunchAtStartup(true);
    }

    MSG msg{};
    bool snapshotRequested = false;
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (snapshotMode_) {
            if (opts.page) {
                show_.pageScrollTarget = opts.scroll;
                show_.pageScroll = opts.scroll;
            }
            if (opts.tab == Tab::Library) lib_.scroll = lib_.scrollTarget = opts.scroll;
            if (opts.tab == Tab::Settings && opts.scroll > 0) set_.scroll = set_.scrollTarget = opts.scroll;
            if (opts.tab == Tab::Deals && opts.scroll > 0) dealsUi_.scroll = dealsUi_.scrollTarget = opts.scroll;
            if (!snapshotRequested && time_ >= opts.wait && Wall() >= opts.wait) {  // real time too: downloads, video
                gfx_.RequestSnapshot(opts.snapshot);
                snapshotRequested = true;
                Frame();
                int code = gfx_.SnapshotDone() ? 0 : 2;
                video_.Close();
                DestroyWindow(hwnd_);
                images_.Stop();
                meta_.Stop();
                trailers_.Stop();
                deals_.Stop();
                return code;
            }
            Frame();
            continue;
        }
        BackgroundTick();
        if (IsIconic(hwnd_) || !IsWindowVisible(hwnd_)) {
            // Minimized or in the tray: no drawing, just keep watching game sessions.
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 1000, QS_ALLINPUT);
            QueryPerformanceCounter(&qpcLast_);
            continue;
        }
        // Idle (animations settled) or in the background (usually while a game runs): sleep until input,
        // poll the controller ~11x per second and redraw only once a second for the clock and late data.
        if (!active_ || (Wall() - lastInput_ > 4.0 && !Animating())) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, active_ ? 90 : 250, QS_ALLINPUT);
            if (active_) {
                input_.PollGamepad(0.09f);
                if (!input_.frame().keys.empty()) lastInput_ = Wall();
            }
            bool woke = active_ && (Wall() - lastInput_ <= 4.0 || Animating());
            if (!woke && Wall() - lastFrameWall_ < (active_ ? 1.0 : 2.0)) continue;
        }
        Frame();
    }
done:
    RemoveTray();
    SaveIfNeeded(true);
    video_.Close();
    images_.Stop();
    meta_.Stop();
    trailers_.Stop();
    deals_.Stop();
    VideoPlayer::Shutdown();
    delete ctx_;
    ctx_ = nullptr;
    gfx_.Shutdown();
    return static_cast<int>(msg.wParam);
}

bool App::CreateMainWindow(HINSTANCE inst) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = StaticWndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = CreateSolidBrush(RGB(11, 11, 13));
    wc.lpszClassName = L"LumenWindow";
    RegisterClassExW(&wc);

    int w = opts_.width, h = opts_.height;
    DWORD style = WS_OVERLAPPEDWINDOW;
    RECT rc{0, 0, w, h};
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (!snapshotMode_) {
        RECT work;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        int ww = work.right - work.left, wh = work.bottom - work.top;
        w = std::min(ww - 80, std::max(1280, ww * 85 / 100));
        h = std::min(wh - 60, w * 9 / 16);
        rc = {0, 0, w, h};
        AdjustWindowRectEx(&rc, style, FALSE, 0);
        x = work.left + (ww - (rc.right - rc.left)) / 2;
        y = work.top + (wh - (rc.bottom - rc.top)) / 2;
    } else {
        AdjustWindowRectEx(&rc, style, FALSE, 0);
    }
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"Lumen", style, x, y, rc.right - rc.left, rc.bottom - rc.top,
                            nullptr, nullptr, inst, this);
    if (!hwnd_) return false;
    DragAcceptFiles(hwnd_, TRUE);
    ApplyTheme();
    return true;
}

void App::ApplyTheme() {
    bool dark = settings_.theme == ThemeMode::Dark ||
                (settings_.theme == ThemeMode::System && SystemPrefersDark());
    theme_ = MakeTheme(dark, settings_.accent);
    if (hwnd_) {
        BOOL d = dark;
        DwmSetWindowAttribute(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &d, sizeof(d));
        COLORREF caption = dark ? RGB(11, 11, 13) : RGB(242, 242, 247);
        DwmSetWindowAttribute(hwnd_, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    }
}

void App::ToggleFullscreen() {
    if (inFrame_) {  // never restyle/resize the window while a frame is being drawn
        fullscreenPending_ = true;
        return;
    }
    DWORD style = GetWindowLongW(hwnd_, GWL_STYLE);
    if (!fullscreen_) {
        MONITORINFO mi{sizeof(mi)};
        if (GetWindowPlacement(hwnd_, &savedPlacement_) &&
            GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongW(hwnd_, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
            SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            fullscreen_ = true;
        }
    } else {
        SetWindowLongW(hwnd_, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd_, &savedPlacement_);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen_ = false;
    }
}

LRESULT App::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    bool handled = input_.HandleMessage(hwnd, msg, wp, lp, gfx_.scale);
    if (input_.ConsumeActivity()) lastInput_ = Wall();
    if (handled && msg != WM_SYSKEYDOWN) return 0;
    switch (msg) {
        case WM_DPICHANGED: {
            // Layout is resolution-independent; just take the size Windows suggests for the new monitor.
            auto* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) {
                gfx_.Resize(LOWORD(lp), HIWORD(lp));
                if (ctx_ && !snapshotMode_) Frame();  // keep drawing during live resize
            }
            return 0;
        case WM_PAINT:
            ValidateRect(hwnd, nullptr);
            if (ctx_ && !snapshotMode_) Frame();
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize = {1024, 640};
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, hand_ ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_ACTIVATEAPP:
            active_ = wp != 0;
            if (active_ && Wall() - lastActivityRefresh_ > 20.0) {
                // Coming back (often from a game): pick up new playtime and achievements.
                lastActivityRefresh_ = Wall();
                needPlaytimeRefresh_ = true;
                playtimeRefreshAt_ = Wall();
            }
            return 0;
        case WM_CLOSE:
            if (settings_.keepInTray && trayAdded_ && !quitting_) {
                ShowWindow(hwnd, SW_HIDE);
                if (!settings_.trayHintShown) {
                    settings_.trayHintShown = true;
                    settingsDirty_ = true;
                    Notify(L"O Lumen continua por aqui",
                           L"Ele fica na bandeja para avisar sobre seu tempo de jogo. Clique no ícone para abrir "
                           L"ou use “Sair” no menu dele.");
                }
                SaveIfNeeded(true);
                return 0;
            }
            break;
        case WM_APP + 1:  // tray icon events (NOTIFYICON_VERSION_4)
            switch (LOWORD(lp)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                case NIN_BALLOONUSERCLICK:
                case WM_LBUTTONDBLCLK:
                    ShowFromTray();
                    break;
                case WM_CONTEXTMENU: {
                    HMENU menu = CreatePopupMenu();
                    AppendMenuW(menu, MF_STRING, 1, L"Abrir Lumen");
                    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                    AppendMenuW(menu, MF_STRING, 2, L"Sair");
                    SetForegroundWindow(hwnd);
                    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                                             GET_X_LPARAM(wp), GET_Y_LPARAM(wp), 0, hwnd, nullptr);
                    DestroyMenu(menu);
                    if (cmd == 1) ShowFromTray();
                    if (cmd == 2) {
                        quitting_ = true;
                        DestroyWindow(hwnd);
                    }
                    break;
                }
            }
            return 0;
        case WM_SETTINGCHANGE:
            if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) ApplyTheme();
            return 0;
        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wp);
            UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                wchar_t path[MAX_PATH * 2];
                if (DragQueryFileW(drop, i, path, ARRAYSIZE(path))) HandleDroppedFile(path);
            }
            DragFinish(drop);
            return 0;
        }
        case WM_COPYDATA: {
            auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lp);
            if (cds && cds->dwData == 0x4C4E) {
                SendTestNotification();
                return TRUE;
            }
            if (cds && cds->dwData == 0x4C55 && cds->cbData == sizeof(int64_t)) {
                // A newer build was launched: step aside so it can take over.
                int64_t theirs = *static_cast<const int64_t*>(cds->lpData);
                if (theirs > ownBuild_) {
                    quitting_ = true;
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                    return TRUE;
                }
                return FALSE;
            }
            if (cds && cds->dwData == 0x4C41 && cds->cbData >= sizeof(wchar_t)) {
                std::wstring path(static_cast<const wchar_t*>(cds->lpData), cds->cbData / sizeof(wchar_t) - 1);
                AddGameFromPath(path, true);
                return TRUE;
            }
            return FALSE;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    if (msg == taskbarCreatedMsg_ && taskbarCreatedMsg_ != 0) {  // Explorer restarted: re-add the tray icon
        trayAdded_ = false;
        SetupTray();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// --- Tray, background work and wellbeing reminders ---------------------------

void App::SetupTray() {
    if (trayAdded_ || snapshotMode_) return;
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");
    tray_ = {};
    tray_.cbSize = sizeof(tray_);
    tray_.hWnd = hwnd_;
    tray_.uID = 1;
    tray_.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    tray_.uCallbackMessage = WM_APP + 1;
    tray_.hIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    wcscpy_s(tray_.szTip, L"Lumen");
    if (Shell_NotifyIconW(NIM_ADD, &tray_)) {
        tray_.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &tray_);
        trayAdded_ = true;
    }
}

void App::RemoveTray() {
    if (trayAdded_) Shell_NotifyIconW(NIM_DELETE, &tray_);
    trayAdded_ = false;
    if (balloonIcon_) DestroyIcon(balloonIcon_);
    balloonIcon_ = nullptr;
}

void App::ShowFromTray() {
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd_);
    lastInput_ = Wall();
}

void App::Notify(const std::wstring& title, const std::wstring& body) {
    if (!trayAdded_) SetupTray();  // e.g. Explorer was restarted
    // A tray balloon is shown by Windows 10/11 as a regular notification (Action Center),
    // without needing an installer or an AppUserModelID registration.
    NOTIFYICONDATAW n = tray_;
    n.uFlags = NIF_INFO;
    wcsncpy_s(n.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(n.szInfo, body.c_str(), _TRUNCATE);
    // Explorer reads the balloon icon asynchronously, after this call returns: the handle must stay
    // alive (it is owned by the app until exit). Destroying it right away silently drops the notification.
    if (!balloonIcon_)
        balloonIcon_ = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                     GetSystemMetrics(SM_CXICON) * 2,
                                                     GetSystemMetrics(SM_CYICON) * 2, 0));
    n.hBalloonIcon = balloonIcon_;
    n.dwInfoFlags = balloonIcon_ ? (NIIF_USER | NIIF_LARGE_ICON) : NIIF_INFO;
    BOOL ok = trayAdded_ && Shell_NotifyIconW(NIM_MODIFY, &n);
    if (!ok && trayAdded_) {
        // The icon may have vanished from the tray: add it again and retry once.
        trayAdded_ = false;
        SetupTray();
        n.hWnd = tray_.hWnd;
        ok = trayAdded_ && Shell_NotifyIconW(NIM_MODIFY, &n);
    }
    if (!ok) {
        std::string prev;
        ReadFileBytes(JoinPath(AppDataDir(), L"notify.log"), prev);
        WriteFileAtomic(JoinPath(AppDataDir(), L"notify.log"),
                        prev + "notification failed: tray=" + std::to_string(trayAdded_) +
                            " error=" + std::to_string(GetLastError()) + "\n");
    }
}

void App::PlaySfx(Sfx s) {
    if (settings_.uiSounds && !snapshotMode_) sound_.Play(s);
}

int64_t App::LocalMidnight(int64_t unix) {
    __time64_t t = unix;
    tm lt{};
    if (_localtime64_s(&lt, &t) != 0) return unix - unix % 86400;
    lt.tm_hour = lt.tm_min = lt.tm_sec = 0;
    lt.tm_isdst = -1;
    return static_cast<int64_t>(_mktime64(&lt));
}

int64_t App::TodaySeconds() const {
    int64_t now = NowUnix(), midnight = LocalMidnight(now);
    int64_t total = user_.SecondsBetween(midnight, now);
    std::string running = launcher_.RunningGame();
    if (!running.empty()) total += now - std::max(midnight, launcher_.RunningSince(running));
    return total;
}

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

bool App::LaunchAtStartup() {
    return !RegReadString(HKEY_CURRENT_USER, kRunKey, L"Lumen").empty();
}

void App::SetLaunchAtStartup(bool on) {
    if (!on) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, L"Lumen");
        return;
    }
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --tray";
    RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, L"Lumen", REG_SZ, cmd.c_str(),
                    static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
}

const std::vector<std::pair<std::wstring, std::wstring>>& App::ScreenshotsOf(const Game& g) {
    // Listing the folder is cheap but not free: refresh at most every 10 s per game.
    if (shots_.id != g.id || time_ - shots_.at > 10.0) {
        shots_.id = g.id;
        shots_.at = time_;
        shots_.shots.clear();
        if (g.platform == Platform::Steam) shots_.shots = library_.steam().Screenshots(g.platformId);
    }
    return shots_.shots;
}

void App::OpenStatusSheet(const std::string& id) {
    Game* g = library_.Find(id);
    if (!g) return;
    sheet_ = Sheet{};
    sheet_.open = true;
    sheet_.openedAt = time_;
    sheet_.title = L"Status";
    sheet_.subtitle = g->name;
    static const PlayStatus all[] = {PlayStatus::None, PlayStatus::Queued, PlayStatus::Playing, PlayStatus::Finished,
                                     PlayStatus::Dropped};
    static const wchar_t glyphs[] = {glyph::Cancel, glyph::Clock, glyph::Game, glyph::Checkmark, glyph::Hide};
    for (int i = 0; i < 5; ++i) {
        SheetItem it;
        it.label = PlayStatusName(all[i]);
        it.glyph = glyphs[i];
        it.checkable = true;
        it.checked = g->status == static_cast<int>(all[i]);
        PlayStatus s = all[i];
        it.action = [this, id, s] {
            if (Game* gg = library_.Find(id)) {
                library_.SetStatus(*gg, s);
                PlaySfx(Sfx::Toggle);
                ShowToast(s == PlayStatus::None ? L"Status removido" : std::wstring(L"Marcado como ") + PlayStatusName(s),
                          glyph::Checkmark);
            }
        };
        sheet_.items.push_back(std::move(it));
    }
    PlaySfx(Sfx::Select);
}

void App::OpenNotesSheet(const std::string& id) {
    Game* g = library_.Find(id);
    if (!g) return;
    OpenTextSheet(L"Notas · " + g->name, L"Ex.: parei no capítulo 5, falta o chefe do castelo",
                  [this, id](const std::wstring& text) {
                      if (Game* gg = library_.Find(id)) {
                          library_.SetNotes(*gg, text);
                          ShowToast(gg->notes.empty() ? L"Notas apagadas" : L"Notas salvas", glyph::Edit);
                      }
                  });
    sheet_.text = g->notes;
}

void App::DrawGreeting(Ctx& c, float alpha) {
    if (alpha <= 0.01f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    const wchar_t* hello = st.wHour >= 5 && st.wHour < 12 ? L"Bom dia" : st.wHour >= 12 && st.wHour < 18 ? L"Boa tarde"
                                                                                                         : L"Boa noite";
    std::wstring name = library_.steam().personaName();
    std::wstring first = std::wstring(hello) + (name.empty() ? L"" : L", " + name);
    int64_t today = TodaySeconds();
    int goal = settings_.DailyGoalMinutes();
    std::wstring second;
    if (today >= 60) second = FormatPlaytime(today / 60) + L" jogados hoje" + (goal ? L" de " + FormatPlaytime(goal) : L"");
    else if (!launcher_.RunningGame().empty()) second = L"jogando agora";
    else second = goal ? L"meta de hoje: " + FormatPlaytime(goal) : L"pronto para jogar?";

    Gfx& g = gfx_;
    g.PushAlpha(alpha);
    TextStyle a = Body(17, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    a.shadow = theme_.dark;
    float w = g.Measure(first, a).x;
    float x = 72, y = 98;
    g.Text(first, {x, y, w + 4, 24}, a);
    TextStyle b = Body(17, theme_.text2);
    b.shadow = theme_.dark;
    g.Text(L"  ·  " + second, {x + w, y, 600, 24}, b);
    if (goal && today > 0) {
        float frac = std::min(1.f, static_cast<float>(today) / (goal * 60.f));
        Progress(c, {x, y + 32, 180, 4}, frac, frac >= 1.f ? Color::Hex(0xFF9F0A) : theme_.accent, theme_.control);
    }
    g.PopAlpha();
}

void App::SendTestNotification() {
    Game* g = SelectedGame();
    Notify(L"Você está jogando há 1 h",
           (g ? g->name : std::wstring(L"Seu jogo")) + L" · Que tal uma pausa rápida para se alongar e beber água?");
}

namespace {
std::wstring FormatDuration(int64_t seconds) {
    int64_t m = std::max<int64_t>(1, seconds / 60);
    if (m < 60) return std::to_wstring(m) + L" min";
    int64_t h = m / 60, r = m % 60;
    return std::to_wstring(h) + L" h" + (r ? L" " + std::to_wstring(r) + L" min" : L"");
}
}  // namespace

void App::BackgroundTick() {
    double now = Wall();
    if (launcher_.Poll(library_, now)) {
        needPlaytimeRefresh_ = true;
        playtimeRefreshAt_ = now + 5.0;  // Steam writes playtime shortly after the game exits
        secondRefreshAt_ = now + 75.0;   // ...and achievement caches a bit later
    }
    if (secondRefreshAt_ > 0 && now >= secondRefreshAt_) {
        secondRefreshAt_ = 0;
        needPlaytimeRefresh_ = true;
        playtimeRefreshAt_ = now;
    }
    if (needPlaytimeRefresh_ && now >= playtimeRefreshAt_) {
        needPlaytimeRefresh_ = false;
        RefreshProgress();
        if (Game* g = SelectedGame())
            meta_.RequestAchievements(*g, library_.steam().LibraryCacheFile(g->platformId), true);
    }
    CheckPlayReminders();
    AttractTick();
    SaveIfNeeded();
}

void App::CheckPlayReminders() {
    std::string id = launcher_.RunningGame();
    if (id != reminderGameId_) {
        // The previous session ended: a short summary, if it was long enough to matter.
        if (!reminderGameId_.empty() && settings_.sessionSummary) {
            int64_t secs = NowUnix() - reminderStart_;
            if (secs >= 5 * 60) {
                const Game* g = library_.Find(reminderGameId_);
                std::wstring name = g ? g->name : L"Seu jogo";
                Notify(L"Sessão encerrada · " + FormatDuration(secs),
                       name + L": você jogou " + FormatDuration(secs) + L" agora. Bom descanso!");
            }
        }
        reminderGameId_ = id;
        reminderStart_ = id.empty() ? 0 : launcher_.RunningSince(id);
        remindersSent_ = 0;
    }
    // Daily limit (optional): one notification per day, while a game is running.
    int goal = settings_.DailyGoalMinutes() * 60;
    if (!id.empty() && goal > 0) {
        int64_t day = LocalMidnight(NowUnix());
        int64_t today = TodaySeconds();
        if (goalNotifiedDay_ != day && today >= goal) {
            goalNotifiedDay_ = day;
            Notify(L"Meta diária atingida",
                   L"Você já jogou " + FormatDuration(today) + L" hoje (meta: " + FormatDuration(goal) +
                       L"). Que tal encerrar por aqui?");
        }
    }
    int interval = settings_.RemindMinutes() * 60;
    if (id.empty() || interval <= 0) return;
    int64_t elapsed = NowUnix() - reminderStart_;
    if (elapsed < (remindersSent_ + 1) * interval) return;
    remindersSent_ = elapsed / interval;
    const Game* g = library_.Find(id);
    std::wstring name = g ? g->name : L"seu jogo";
    static const wchar_t* tips[] = {
        L"Que tal uma pausa rápida para se alongar e beber água?",
        L"Descanse os olhos: olhe para algo distante por 20 segundos.",
        L"Levante um pouco, mexa os ombros e volte com tudo.",
        L"Seu progresso não vai a lugar nenhum. Uma pausa curta faz bem.",
    };
    Notify(L"Você está jogando há " + FormatDuration(elapsed),
           name + L" · " + tips[(remindersSent_ - 1) % 4]);
}

MetadataConfig App::MetaConfig() const {
    MetadataConfig cfg;
    cfg.online = settings_.onlineData;
    cfg.steamApiKey = settings_.steamApiKey;
    cfg.steamId64 = library_.steam().steamId64();
    return cfg;
}

void App::SaveIfNeeded(bool force) {
    if (snapshotMode_) return;
    static bool userDirty = false;
    if (library_.consumeDirty()) userDirty = true;
    if (!userDirty && !settingsDirty_) return;
    if (!force && Wall() - lastSave_ < 1.0) return;
    lastSave_ = Wall();
    if (userDirty) user_.Save();
    if (settingsDirty_) settings_.Save();
    userDirty = settingsDirty_ = false;
}

void App::RebuildHomeList() {
    LibraryQuery q;
    q.sort = SortMode::Recent;
    show_.list = library_.Query(q);
    show_.listRevision = library_.revision();
    show_.sel = 0;
    for (size_t i = 0; i < show_.list.size(); ++i)
        if (library_.games()[show_.list[i]].id == show_.selId) show_.sel = static_cast<int>(i);
    if (!show_.list.empty()) show_.selId = library_.games()[show_.list[show_.sel]].id;
    else show_.selId.clear();
}

void App::RefreshAll(bool announce) {
    library_.Refresh();
    meta_.SetConfig(MetaConfig());
    meta_.ForgetAll();
    meta_.ReapplyDetails(library_);
    RequestStoreArt();
    RebuildHomeList();
    if (Game* g = SelectedGame()) RequestGameData(*g);
    if (announce) {
        int n = library_.CountWhere(Filter::All);
        ShowToast(L"Biblioteca atualizada · " + std::to_wstring(n) + (n == 1 ? L" jogo" : L" jogos"), glyph::Refresh);
    }
}

Game* App::SelectedGame() {
    if (show_.list.empty()) return nullptr;
    show_.sel = std::clamp(show_.sel, 0, static_cast<int>(show_.list.size()) - 1);
    int idx = show_.list[show_.sel];
    if (idx < 0 || idx >= static_cast<int>(library_.games().size())) return nullptr;
    return &library_.games()[idx];
}

void App::RequestStoreArt() {
    // GOG keeps no artwork on disk: fetch it up front so covers appear in the catalog, not only on the game page.
    for (const auto& g : library_.games())
    {
        if (g.platform == Platform::GOG && (g.cover.empty() || g.hero.empty())) meta_.RequestDetails(g);
        if (settings_.onlineData && g.platform == Platform::Steam && !g.isApp) meta_.RequestNews(g);  // "novidade" badges
    }
}

void App::RequestGameData(const Game& g) {
    meta_.RequestDetails(g);
    meta_.RequestAchievements(g, library_.steam().LibraryCacheFile(g.platformId));
}

// --- Navigation & actions ---------------------------------------------------

void App::SwitchTab(Tab t) {
    if (t == tab_ && !(t == Tab::Home && show_.page)) return;
    PlaySfx(Sfx::Toggle);
    if (tab_ == Tab::Home && show_.page && t != Tab::Home) show_.page = false;
    prevTab_ = tab_;
    tab_ = t;
    tabChangedAt_ = time_;
    lib_.editing = false;
    set_.editingKey = false;
}

void App::OpenGame(const std::string& id, Tab returnTo) {
    int libIdx = library_.IndexOf(id);
    if (libIdx < 0) return;
    PlaySfx(Sfx::Select);
    auto it = std::find(show_.list.begin(), show_.list.end(), libIdx);
    if (it == show_.list.end()) {
        show_.list.insert(show_.list.begin(), libIdx);
        it = show_.list.begin();
    }
    show_.sel = static_cast<int>(it - show_.list.begin());
    show_.selId = id;
    show_.page = true;
    show_.returnTab = returnTo;
    show_.row = 0;
    show_.button = 0;
    show_.pageScroll = show_.pageScrollTarget = 0;
    if (tab_ != Tab::Home) {
        prevTab_ = tab_;
        tab_ = Tab::Home;
        tabChangedAt_ = time_;
    }
    RequestGameData(library_.games()[libIdx]);
}

void App::CloseGamePage() {
    PlaySfx(Sfx::Back);
    show_.page = false;
    show_.row = 1;
    show_.pageScrollTarget = 0;
    if (show_.returnTab != Tab::Home) SwitchTab(show_.returnTab);
}

void App::LaunchGame(Game& g) {
    std::wstring err;
    if (launcher_.Launch(library_, g, err)) {
        PlaySfx(Sfx::Launch);
        launchFx_ = {g.id, time_};  // cinematic "opening" overlay
        video_.Close();             // free the decoder for the game
        videoGameId_.clear();
        if (g.status == static_cast<int>(PlayStatus::None) || g.status == static_cast<int>(PlayStatus::Queued))
            library_.SetStatus(g, PlayStatus::Playing);  // starting a game moves it out of the backlog
        show_.launchedAt = time_;
        show_.launchedId = g.id;
        ShowToast(L"Abrindo " + g.name + L"…", glyph::Play);
    } else {
        ShowToast(err, glyph::Info);
    }
}

void App::ToggleFavorite(Game& g) {
    library_.SetFavorite(g, !g.favorite);
    PlaySfx(Sfx::Toggle);
    ShowToast(g.favorite ? L"Adicionado aos favoritos" : L"Removido dos favoritos",
              g.favorite ? glyph::HeartFill : glyph::Heart);
}

void App::ShowToast(const std::wstring& text, wchar_t g) {
    toast_.text = text;
    toast_.glyph = g;
    toast_.shownAt = time_;
}

void App::PickRandomGame() {
    if (show_.list.size() < 2) return;
    // Favour games with little playtime: the point is rediscovering the library.
    std::vector<double> weights;
    for (int idx : show_.list) {
        const Game& g = library_.games()[idx];
        double h = std::max<int64_t>(0, g.TotalMinutes()) / 60.0;
        weights.push_back(1.0 / (1.0 + h * 0.15) + (g.favorite ? 0.5 : 0.0));
    }
    weights[show_.sel] = 0;
    static std::mt19937 rng{std::random_device{}()};
    std::discrete_distribution<int> dist(weights.begin(), weights.end());
    int pick = dist(rng);
    SwitchTab(Tab::Home);
    show_.page = false;
    show_.row = 1;
    show_.sel = pick;
    const Game& g = library_.games()[show_.list[pick]];
    show_.selId = g.id;
    ShowToast(L"Que tal jogar " + g.name + L" hoje?", glyph::Shuffle);
}

bool App::PickFile(const wchar_t* title, bool images, std::wstring& out) {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return false;
    COMDLG_FILTERSPEC exe[] = {{L"Executáveis", L"*.exe;*.lnk;*.bat;*.url"}, {L"Todos os arquivos", L"*.*"}};
    COMDLG_FILTERSPEC img[] = {{L"Imagens", L"*.png;*.jpg;*.jpeg;*.webp;*.bmp"}, {L"Todos os arquivos", L"*.*"}};
    dlg->SetFileTypes(2, images ? img : exe);
    dlg->SetTitle(title);
    if (!images) {
        DWORD o = 0;
        dlg->GetOptions(&o);
        dlg->SetOptions(o | FOS_NODEREFERENCELINKS | FOS_FILEMUSTEXIST);
    }
    if (dlg->Show(hwnd_) != S_OK) return false;
    ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(&item))) return false;
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return false;
    out = path;
    CoTaskMemFree(path);
    QueryPerformanceCounter(&qpcLast_);  // the dialog blocked the loop; don't animate the gap
    return true;
}

void App::AddGameDialog() {
    std::wstring exe;
    if (PickFile(L"Escolha o executável do jogo", false, exe)) AddGameFromPath(exe, true);
}

void App::HandleDroppedFile(const std::wstring& path) {
    std::wstring ext = FileExt(path);
    bool image = ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".webp" || ext == L".bmp";
    if (!image) {
        AddGameFromPath(path, true);
        return;
    }
    // An image dropped on a game page becomes its art; the shape decides where it goes.
    Game* g = (tab_ == Tab::Home && show_.page) ? SelectedGame() : nullptr;
    if (!g) {
        ShowToast(L"Abra a página de um jogo e solte a imagem lá para usá-la como arte", glyph::Image);
        return;
    }
    UINT w = 0, h = 0;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (SUCCEEDED(gfx_.wic()->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                        WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)))
        frame->GetSize(&w, &h);
    if (!w || !h) {
        ShowToast(L"Não foi possível ler essa imagem", glyph::Info);
        return;
    }
    ArtKind kind = h > w ? ArtKind::Cover : ext == L".png" ? ArtKind::Logo : ArtKind::Hero;
    const wchar_t* what = kind == ArtKind::Cover ? L"Capa" : kind == ArtKind::Logo ? L"Logo" : L"Arte de fundo";
    if (library_.SetArt(*g, kind, path)) ShowToast(std::wstring(what) + L" atualizada", glyph::Image);
    else ShowToast(L"Não foi possível usar essa imagem", glyph::Info);
}

void App::AddGameFromPath(const std::wstring& exe, bool openPage) {
    std::string id;
    if (library_.AddCustomGame(exe, &id)) {
        RebuildHomeList();
        Game* g = library_.Find(id);
        ShowToast(L"“" + (g ? g->name : FileStem(exe)) + L"” foi adicionado", glyph::Add);
        if (openPage) OpenGame(id, tab_ == Tab::Home ? Tab::Home : tab_);
    } else if (!id.empty()) {
        ShowToast(L"Esse executável já está na biblioteca", glyph::Info);
        if (openPage) OpenGame(id, tab_);
    } else {
        ShowToast(L"Não foi possível adicionar esse arquivo", glyph::Info);
    }
}

void App::OpenTextSheet(const std::wstring& title, const std::wstring& placeholder,
                        std::function<void(const std::wstring&)> onSubmit) {
    sheet_ = Sheet{};
    sheet_.open = true;
    sheet_.title = title;
    sheet_.textInput = true;
    sheet_.placeholder = placeholder;
    sheet_.onSubmit = std::move(onSubmit);
    sheet_.openedAt = time_;
}

void App::OpenCategorySheet(const std::string& id) {
    Game* g = library_.Find(id);
    if (!g) return;
    sheet_ = Sheet{};
    sheet_.open = true;
    sheet_.openedAt = time_;
    sheet_.title = L"Categorias";
    sheet_.subtitle = g->name;
    for (const auto& cat : user_.categories) {
        SheetItem it;
        it.label = cat;
        it.glyph = glyph::Tag;
        it.checkable = true;
        it.checked = std::find(g->categories.begin(), g->categories.end(), cat) != g->categories.end();
        it.keepOpen = true;
        it.action = [this, id, cat] {
            if (Game* gg = library_.Find(id)) library_.ToggleCategory(*gg, cat);
        };
        sheet_.items.push_back(std::move(it));
    }
    if (!g->platformCollections.empty()) {
        std::wstring cols;
        for (const auto& c : g->platformCollections) cols += (cols.empty() ? L"" : L", ") + c;
        SheetItem info;
        info.label = L"Coleções da Steam";
        info.detail = cols;
        info.glyph = glyph::Library;
        sheet_.items.push_back(std::move(info));
    }
    SheetItem add;
    add.label = L"Nova categoria…";
    add.glyph = glyph::Add;
    add.action = [this, id] {
        OpenTextSheet(L"Nova categoria", L"Ex.: Cooperativo, Para terminar, Relaxantes", [this, id](const std::wstring& name) {
            std::wstring n = Trim(name);
            if (n.empty()) return;
            library_.CreateCategory(n);
            if (Game* gg = library_.Find(id)) {
                if (std::find(gg->categories.begin(), gg->categories.end(), n) == gg->categories.end())
                    library_.ToggleCategory(*gg, n);
            }
            ShowToast(L"Categoria “" + n + L"” criada", glyph::Tag);
        });
    };
    sheet_.items.push_back(std::move(add));
}

void App::OpenArtSheet(const std::string& id) {
    Game* game = library_.Find(id);
    if (!game) return;
    sheet_ = Sheet{};
    sheet_.open = true;
    sheet_.openedAt = time_;
    sheet_.title = L"Artes";
    sheet_.subtitle = game->name + L" · ou solte uma imagem na página do jogo";
    auto add = [&](std::wstring label, std::function<void()> fn, std::wstring detail) {
        SheetItem it;
        it.label = std::move(label);
        it.glyph = glyph::Image;
        it.detail = std::move(detail);
        it.action = std::move(fn);
        sheet_.items.push_back(std::move(it));
    };
    auto pick = [this, id](ArtKind kind, const wchar_t* title, const wchar_t* done) {
        std::wstring f;
        if (PickFile(title, true, f))
            if (Game* g = library_.Find(id))
                ShowToast(library_.SetArt(*g, kind, f) ? done : L"Não foi possível usar essa imagem", glyph::Image);
    };
    add(L"Alterar capa…", [pick] { pick(ArtKind::Cover, L"Escolha uma capa (vertical, 2:3)", L"Capa atualizada"); },
        L"Imagem vertical, proporção 2:3");
    add(L"Alterar arte de fundo…", [pick] { pick(ArtKind::Hero, L"Escolha uma arte de fundo (horizontal)", L"Fundo atualizado"); },
        L"Imagem larga, de preferência 3840 px");
    add(L"Alterar logo…", [pick] { pick(ArtKind::Logo, L"Escolha um logo (PNG transparente)", L"Logo atualizado"); },
        L"PNG com fundo transparente");
    const UserGameData* ud = user_.Find(id);
    if (ud && (!ud->coverOverride.empty() || !ud->heroOverride.empty() || !ud->logoOverride.empty())) {
        SheetItem it;
        it.label = L"Restaurar artes originais";
        it.glyph = glyph::Refresh;
        it.action = [this, id] {
            if (Game* g = library_.Find(id)) {
                library_.ClearArt(*g);
                RebuildHomeList();
                ShowToast(L"Artes originais restauradas", glyph::Refresh);
            }
        };
        sheet_.items.push_back(std::move(it));
    }
    PlaySfx(Sfx::Select);
}

void App::OpenGameMenu(Game& game) {
    PlaySfx(Sfx::Select);
    std::string id = game.id;
    sheet_ = Sheet{};
    sheet_.open = true;
    sheet_.openedAt = time_;
    sheet_.title = game.name;
    sheet_.subtitle = std::wstring(PlatformName(game.platform)) + L" · Opções do jogo";
    auto add = [&](std::wstring label, wchar_t g, std::function<void()> fn, std::wstring detail = {}) {
        SheetItem it;
        it.label = std::move(label);
        it.glyph = g;
        it.action = std::move(fn);
        it.detail = std::move(detail);
        sheet_.items.push_back(std::move(it));
        return &sheet_.items.back();
    };
    add(L"Jogar", glyph::Play, [this, id] { if (Game* g = library_.Find(id)) LaunchGame(*g); });
    SheetItem* fav = add(L"Favorito", glyph::Heart, [this, id] { if (Game* g = library_.Find(id)) ToggleFavorite(*g); });
    fav->checkable = true;
    fav->checked = game.favorite;
    add(L"Status…", glyph::Checkmark, [this, id] { OpenStatusSheet(id); },
        game.status ? PlayStatusName(static_cast<PlayStatus>(game.status)) : L"");
    add(L"Categorias…", glyph::Tag, [this, id] { OpenCategorySheet(id); });
    add(L"Notas…", glyph::Edit, [this, id] { OpenNotesSheet(id); }, game.notes);
    add(L"Artes…", glyph::Image, [this, id] { OpenArtSheet(id); }, L"Capa, fundo e logo");
    if (!game.installDir.empty())
        add(L"Abrir pasta do jogo", glyph::Folder, [this, id] { if (Game* g = library_.Find(id)) launcher_.OpenInstallFolder(*g); });
    if (launcher_.HasPlatformPage(game))
        add(std::wstring(L"Abrir na ") + PlatformName(game.platform), glyph::Link,
            [this, id] { if (Game* g = library_.Find(id)) launcher_.OpenInPlatform(*g); });
    if (game.platform == Platform::Steam)
        add(L"Atualizar conquistas", glyph::Trophy, [this, id] {
            if (Game* g = library_.Find(id)) {
                library_.RefreshPlaytime();
                meta_.RequestAchievements(*g, library_.steam().LibraryCacheFile(g->platformId), true);
                ShowToast(L"Atualizando conquistas…", glyph::Refresh);
            }
        });
    add(game.hidden ? L"Mostrar na biblioteca" : L"Ocultar da biblioteca", game.hidden ? glyph::View : glyph::Hide,
        [this, id] {
            if (Game* g = library_.Find(id)) {
                library_.SetHidden(*g, !g->hidden);
                ShowToast(g->hidden ? L"Jogo oculto · veja em Biblioteca › Ocultos" : L"Jogo visível novamente",
                          g->hidden ? glyph::Hide : glyph::View);
                bool wasPage = show_.page;
                RebuildHomeList();
                if (wasPage && g->hidden) CloseGamePage();
            }
        });
    if (game.platform == Platform::Custom) {
        add(L"Renomear…", glyph::Edit, [this, id] {
            OpenTextSheet(L"Renomear jogo", L"Nome que aparece na biblioteca", [this, id](const std::wstring& name) {
                if (library_.RenameCustomGame(id, name)) ShowToast(L"Nome atualizado", glyph::Edit);
            });
            if (Game* g = library_.Find(id)) sheet_.text = g->name;
        });
        SheetItem* rm = add(L"Remover da biblioteca", glyph::Delete, [this, id] {
            std::wstring name = library_.Find(id) ? library_.Find(id)->name : L"";
            if (show_.page) CloseGamePage();
            library_.RemoveCustomGame(id);
            RebuildHomeList();
            ShowToast(L"“" + name + L"” foi removido (o arquivo não foi apagado)", glyph::Delete);
        });
        rm->destructive = true;
    }
}

// --- Frame -----------------------------------------------------------------

void App::Frame() {
    if (inFrame_ || !ctx_) return;
    inFrame_ = true;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = static_cast<float>(now.QuadPart - qpcLast_.QuadPart) / static_cast<float>(qpcFreq_.QuadPart);
    qpcLast_ = now;
    dt = std::clamp(dt, 0.f, 0.05f);
    if (snapshotMode_) {
        dt = 1.f / 60.f;  // deterministic animation for snapshots
        time_ += dt;
    } else {
        time_ = Wall();
    }
    lastFrameWall_ = Wall();
    gfx_.now = time_;

    if (active_ || snapshotMode_) input_.PollGamepad(dt);
    if (!input_.frame().keys.empty() || input_.frame().wheel != 0) lastInput_ = Wall();
    if (time_ - toast_.shownAt < 3.5) lastInput_ = Wall();  // keep toasts animating smoothly
    meta_.Poll(library_);
    images_.Tick(gfx_);
    UpdateTrailer();
    if (!gfx_.BeginFrame()) {
        input_.EndFrame();
        inFrame_ = false;
        return;
    }

    Ctx& c = *ctx_;
    c.dt = dt;
    c.t = time_;
    c.wantHand = false;
    c.backdrop = {};
    InputFrame& in = input_.frame();
    input_.textMode = (tab_ == Tab::Library && !show_.page) || set_.editingKey || (sheet_.open && sheet_.textInput);

    // Global shortcuts
    bool typing = set_.editingKey || (sheet_.open && sheet_.textInput);
    if (in.Has(Key::Fullscreen) && !snapshotMode_) ToggleFullscreen();
    if (!sheet_.open && !typing) {
        if (in.Has(Key::NextTab) || in.Has(Key::PrevTab)) {
            int t = static_cast<int>(tab_) + (in.Has(Key::NextTab) ? 1 : kTabCount - 1);
            SwitchTab(static_cast<Tab>(t % kTabCount));
        }
        for (int i = 0; i < kTabCount; ++i)
            if (in.Has(static_cast<Key>(static_cast<int>(Key::Tab1) + i))) SwitchTab(static_cast<Tab>(i));
        if (in.Has(Key::Settings)) SwitchTab(Tab::Settings);
        if (in.Has(Key::Refresh)) RefreshAll(true);
        if (in.Has(Key::AddGame)) AddGameDialog();
        if (in.Has(Key::Search)) {
            SwitchTab(Tab::Library);
            lib_.editing = true;
            lib_.zone = 0;
        }
        if (in.Has(Key::Random) && tab_ != Tab::Library) PickRandomGame();
    }
    if (sheet_.open) {
        HandleSheetInput(c);
        in.keys.clear();
        in.text.clear();
        profileOpen_ = false;
    }
    if (profileOpen_) {
        if (in.Has(Key::Back) || in.Has(Key::Menu)) profileOpen_ = false;
        in.keys.clear();
        in.text.clear();
    }
    // Full-screen moments (launch animation, celebrations): any key or click dismisses them.
    bool launching = time_ - launchFx_.start < 3.2;
    bool celebrating = !celebrations_.empty() && celebrations_.front().start >= 0;
    if ((launching || celebrating) && (!in.keys.empty() || in.released)) {
        if (launching) launchFx_.start = -10;
        else celebrations_.pop_front();
        in.keys.clear();
        in.text.clear();
        in.released = false;
    }
    c.blockMouse = sheet_.open || profileOpen_ || launching || celebrating;

    pageT_ = c.Anim(Id("pageT"), show_.page && tab_ == Tab::Home ? 1.f : 0.f, 9.f);

    // Controller plugged in / out
    if (in.gamepad != padConnected_) {
        padConnected_ = in.gamepad;
        if (!snapshotMode_) {
            ShowToast(padConnected_ ? L"Controle conectado" : L"Controle desconectado", glyph::Game);
            if (padConnected_ && settings_.fullscreenOnController && !fullscreen_) ToggleFullscreen();
        }
    }

    // "Cor do jogo": the accent follows the artwork of the game in focus.
    if (settings_.accent == kDynamicAccent) {
        const Game* focus = nullptr;
        if (tab_ == Tab::Library && !lib_.results.empty())
            focus = &library_.games()[lib_.results[std::clamp(lib_.sel, 0, static_cast<int>(lib_.results.size()) - 1)]];
        else focus = SelectedGame();
        Color target = focus ? VividFrom(GameColor(c, *focus)) : kAccents[0];
        if (dynamicAccent_.a <= 0) dynamicAccent_ = target;
        dynamicAccent_ = Color::Lerp(dynamicAccent_, target, 1.f - std::exp(-dt * 4.f));
        theme_.accent = dynamicAccent_;
    }

    // Navigation sound: selection moved by keyboard, controller, wheel or click (not by hovering).
    int prevSel = show_.sel, prevLib = lib_.sel, prevSet = set_.focus, prevSheet = sheet_.focus;
    bool navInput = !in.keys.empty() || in.wheel != 0 || in.released;

    // Screen with a short fade/slide when switching tabs.
    float k = EaseOut(static_cast<float>((time_ - tabChangedAt_) / 0.32));
    if (tab_ != Tab::Home) gfx_.FillRect({0, 0, gfx_.W, gfx_.H}, theme_.bg);
    gfx_.PushAlpha(tab_ == Tab::Home ? 1.f : k);
    gfx_.PushTransform(Matrix3x2F::Translation(0, (1.f - k) * 16.f * c.Motion()));
    switch (tab_) {
        case Tab::Home: DrawShowcase(c); break;
        case Tab::Library: DrawLibrary(c); break;
        case Tab::Deals: DrawDeals(c); break;
        case Tab::Stats: DrawStats(c); break;
        case Tab::Settings: DrawSettings(c); break;
    }
    gfx_.PopTransform();
    gfx_.PopAlpha();

    if (navInput && (show_.sel != prevSel || lib_.sel != prevLib || set_.focus != prevSet || sheet_.focus != prevSheet))
        PlaySfx(Sfx::Move);

    DrawTopBar(c, 1.f - pageT_);
    if (tab_ == Tab::Home && !show_.list.empty())
        DrawGreeting(c, (1.f - pageT_) * EaseOut(static_cast<float>((time_ - tabChangedAt_) / 0.32)));
    DrawStatus(c, 1.f);
    DrawProfile(c);
    DrawSheet(c);
    DrawLaunchFx(c);
    DrawCelebration(c);
    DrawToast(c);

    LARGE_INTEGER drawEnd, flushEnd;
    QueryPerformanceCounter(&drawEnd);
    gfx_.EndFrame();
    QueryPerformanceCounter(&flushEnd);
    hand_ = c.wantHand;
    // Diagnostics: set LUMEN_PROFILE=1 to log the average CPU cost of building and flushing a frame
    // (flush includes Direct2D tessellation and Present, which may wait for vsync).
    static const bool profile = _wgetenv(L"LUMEN_PROFILE") != nullptr;
    if (profile) {
        static double acc = 0, accFlush = 0, last = 0;
        static int frames = 0;
        acc += static_cast<double>(drawEnd.QuadPart - now.QuadPart) * 1000.0 / qpcFreq_.QuadPart;
        accFlush += static_cast<double>(flushEnd.QuadPart - drawEnd.QuadPart) * 1000.0 / qpcFreq_.QuadPart;
        ++frames;
        if (Wall() - last > 2.0) {
            last = Wall();
            std::string line = "build " + std::to_string(acc / frames) + " ms, flush+present " +
                               std::to_string(accFlush / frames) + " ms, " + std::to_string(frames) + " frames, active=" +
                               std::to_string(active_) + " idle=" + std::to_string(Wall() - lastInput_) + "s\n";
            accFlush = 0;
            std::string prev;
            ReadFileBytes(JoinPath(AppDataDir(), L"profile.log"), prev);
            WriteFileAtomic(JoinPath(AppDataDir(), L"profile.log"), prev + line);
            acc = 0;
            frames = 0;
        }
    }
    input_.EndFrame();
    static double lastCollect = 0;
    if (time_ - lastCollect > 10) {
        lastCollect = time_;
        c.Collect();
    }
    if (!show_.selId.empty() && settings_.lastSelected != show_.selId && !snapshotMode_) {
        settings_.lastSelected = show_.selId;
        settingsDirty_ = true;
    }
    SaveIfNeeded();
    inFrame_ = false;
    if (fullscreenPending_) {
        fullscreenPending_ = false;
        ToggleFullscreen();
    }
}

// --- Chrome: navigation bar, status, sheet, toast --------------------------

void App::DrawTopBar(Ctx& c, float alpha) {
    if (alpha <= 0.01f) return;
    Gfx& g = gfx_;
    g.PushAlpha(alpha);
    TextStyle ts = Body(15, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    ts.align = Align::Center;
    ts.valign = VAlign::Center;
    float widths[kTabCount], total = 8;
    for (int i = 0; i < kTabCount; ++i) {
        widths[i] = g.Measure(kTabNames[i], ts).x + 36;
        total += widths[i];
    }
    Rect pill{kMargin, 32, total, 46};
    bool overArt = tab_ == Tab::Home;
    if (overArt) Glass(c, pill, 23, 0.9f);
    else g.FillRound(pill, 23, theme_.control);
    float pos = c.Anim(Id("navpos"), static_cast<float>(static_cast<int>(tab_)), 16.f);
    // Sliding selection knob (fractional index interpolated across segment widths).
    int i0 = std::clamp(static_cast<int>(std::floor(pos)), 0, kTabCount - 1), i1 = std::min(kTabCount - 1, i0 + 1);
    float fr = pos - i0;
    float x0 = pill.x + 4;
    for (int i = 0; i < i0; ++i) x0 += widths[i];
    float kx = x0 + widths[i0] * fr, kw = widths[i0] + (widths[i1] - widths[i0]) * fr;
    g.FillRound({kx, pill.y + 4, kw, pill.h - 8}, (pill.h - 8) * 0.5f, theme_.focusFill);
    float x = pill.x + 4;
    for (int i = 0; i < kTabCount; ++i) {
        Rect cell{x, pill.y, widths[i], pill.h};
        bool sel = static_cast<int>(tab_) == i;
        float h = c.Anim(Id("navhover", i), c.Hover(cell) && !sel ? 1.f : 0.f, 20.f);
        if (c.Hover(cell)) c.wantHand = true;
        float selK = std::clamp(1.f - std::fabs(pos - i), 0.f, 1.f);
        ts.color = Color::Lerp(Color::Lerp(theme_.text2, theme_.text, h), theme_.focusText, selK);
        g.Text(kTabNames[i], cell, ts);
        if (c.Click(cell) && alpha > 0.5f) {
            if (i == 0 && tab_ == Tab::Home) show_.page = false;
            SwitchTab(static_cast<Tab>(i));
        }
        x += widths[i];
    }
    g.PopAlpha();
}

void App::DrawStatus(Ctx& c, float alpha) {
    Gfx& g = gfx_;
    if (time_ - lastStatusPoll_ > 5.0) {
        lastStatusPoll_ = time_;
        SYSTEM_POWER_STATUS ps{};
        if (GetSystemPowerStatus(&ps) && ps.BatteryFlag != 128 && ps.BatteryFlag != 255 && ps.BatteryLifePercent <= 100) {
            battery_ = ps.BatteryLifePercent;
            charging_ = ps.ACLineStatus == 1;
        } else {
            battery_ = -1;
        }
        ComPtr<INetworkListManager> nlm;
        if (SUCCEEDED(CoCreateInstance(__uuidof(NetworkListManager), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&nlm)))) {
            VARIANT_BOOL connected = VARIANT_TRUE;
            if (SUCCEEDED(nlm->get_IsConnectedToInternet(&connected))) online_ = connected == VARIANT_TRUE;
        }
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t clock[8];
    swprintf_s(clock, L"%02d:%02d", st.wHour, st.wMinute);

    TextStyle ts = Body(17, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    ts.valign = VAlign::Center;
    float clockW = g.Measure(clock, ts).x;
    std::wstring batt = battery_ >= 0 ? std::to_wstring(battery_) + L"%" : L"";
    float battW = batt.empty() ? 0 : g.Measure(batt, ts).x + 40;
    float w = 24 + 26 + 18 + battW + clockW + 18 + 34 + 8;
    Rect pill{g.W - kMargin - w, 32, w, 46};
    g.PushAlpha(alpha);
    // Full-screen toggle (also F11 / Alt+Enter)
    if (IconButton(c, Id("fullscreen"), {pill.x - 14 - 23, pill.cy()}, 46,
                   fullscreen_ ? glyph::BackToWindow : glyph::FullScreen, false) &&
        !snapshotMode_)
        ToggleFullscreen();
    if (tab_ == Tab::Home) Glass(c, pill, 23, 0.9f);
    else g.FillRound(pill, 23, theme_.control);
    float x = pill.x + 22;
    g.Icon(online_ ? glyph::Wifi : glyph::WifiOff, {x + 10, pill.cy()}, 17, online_ ? theme_.text : theme_.text3);
    x += 26 + 16;
    if (!batt.empty()) {
        // Battery drawn as a shape: crisp at any size and independent of icon-font versions.
        Rect body{x, pill.cy() - 7.5f, 26, 15};
        g.StrokeRound(body, 4, theme_.text.Fade(0.8f), 1.5f);
        g.FillRound({body.r() + 1.5f, pill.cy() - 3, 2.5f, 6}, 1, theme_.text.Fade(0.8f));
        float frac = battery_ / 100.f;
        Color fill = battery_ <= 15 && !charging_ ? Color::Hex(0xFF453A) : (charging_ ? Color::Hex(0x30D158) : theme_.text);
        g.FillRound({body.x + 3, body.y + 3, std::max(2.f, (body.w - 6) * frac), body.h - 6}, 2, fill);
        x += 34;
        g.Text(batt, {x, pill.y, 60, pill.h}, ts);
        x += battW - 34;
    }
    g.Text(clock, {x, pill.y, clockW + 4, pill.h}, ts);
    x += clockW + 16;
    Vec2 av{x + 17, pill.cy()};
    // The avatar opens the profile panel.
    avatarRect_ = {av.x - 22, av.y - 22, 44, 44};
    bool hover = !sheet_.open && c.in.mouseMode && avatarRect_.Contains(c.in.mouse);
    if (hover) c.wantHand = true;
    float h = c.Anim(Id("avatarhover"), hover || profileOpen_ ? 1.f : 0.f, 18.f);
    float sc = 1.f + 0.08f * h * c.Motion();
    g.PushTransform(Matrix3x2F::Scale(sc, sc, D2D1::Point2F(av.x, av.y)));
    DrawAvatar(av, 17);
    if (h > 0.01f) g.StrokeCircle(av, 20, theme_.text.Fade(h * 0.9f), 2.f);
    g.PopTransform();
    if (!sheet_.open && c.in.released && avatarRect_.Contains(c.in.mouse) && avatarRect_.Contains(c.in.pressPos)) {
        profileOpen_ = !profileOpen_;
        profileOpenedAt_ = time_;
    }
    g.PopAlpha();
}

void App::DrawAvatar(Vec2 center, float radius) {
    Gfx& g = gfx_;
    ID2D1Bitmap1* avatar = library_.steam().avatarPath().empty() ? nullptr : images_.Get(library_.steam().avatarPath());
    if (avatar) {
        g.Image(avatar, {center.x - radius, center.y - radius, radius * 2, radius * 2}, radius);
        return;
    }
    g.FillCircle(center, radius, theme_.accent);
    std::wstring name = library_.steam().personaName();
    std::wstring initial = name.empty() ? L"L" : ToUpper(name.substr(0, 1));
    TextStyle it = Body(radius * 0.9f, Color::White(), DWRITE_FONT_WEIGHT_BOLD);
    it.align = Align::Center;
    it.valign = VAlign::Center;
    g.Text(initial, {center.x - radius, center.y - radius, radius * 2, radius * 2}, it);
}

void App::DrawProfile(Ctx& c) {
    Gfx& g = gfx_;
    float t = c.Anim(Id("profileT"), profileOpen_ ? 1.f : 0.f, profileOpen_ ? 16.f : 20.f);
    if (t <= 0.01f) return;
    bool savedBlock = c.blockMouse;
    c.blockMouse = !profileOpen_;

    // Aggregate real numbers (same rules as the Statistics screen).
    int games = 0, achUnlocked = 0;
    int64_t minutes = 0;
    for (const auto& gm : library_.games()) {
        if (gm.hidden || (gm.isApp && !settings_.showApps)) continue;
        ++games;
        minutes += std::max<int64_t>(0, gm.TotalMinutes());
        if (gm.achTotal > 0) achUnlocked += gm.achUnlocked;
    }
    std::string running = launcher_.RunningGame();
    const Game* playing = running.empty() ? nullptr : library_.Find(running);
    const std::string& steamId = library_.steam().steamId64();

    struct Action {
        std::wstring label;
        wchar_t glyph;
        std::function<void()> fn;
        bool destructive = false;
    };
    std::vector<Action> actions;
    if (!steamId.empty())
        actions.push_back({L"Ver perfil na Steam", glyph::Link, [steamId] {
                               std::wstring url = L"https://steamcommunity.com/profiles/" + Widen(steamId);
                               ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                           }});
    actions.push_back({L"Surpreenda-me", glyph::Shuffle, [this] { PickRandomGame(); }});
    actions.push_back({L"Estatísticas", glyph::Chart, [this] { SwitchTab(Tab::Stats); }});
    actions.push_back({L"Ajustes", glyph::Settings, [this] { SwitchTab(Tab::Settings); }});
    actions.push_back({fullscreen_ ? L"Sair da tela cheia" : L"Tela cheia",
                       fullscreen_ ? glyph::BackToWindow : glyph::FullScreen, [this] { ToggleFullscreen(); }});
    actions.push_back({L"Sair do Lumen", glyph::Cancel, [this] {
                           quitting_ = true;
                           PostMessageW(hwnd_, WM_CLOSE, 0, 0);
                       }, true});

    float w = 380, rowH = 52;
    float h = 24 + 72 + 20 + 88 + 76 + (playing ? 72 : 0) + 12 + actions.size() * rowH + 14;
    Rect panel{g.W - 72 - w, 92, w, h};
    float sc = 0.95f + 0.05f * t;
    g.PushAlpha(t);
    // Grow out of the avatar, like a popover.
    g.PushTransform(Matrix3x2F::Scale(sc, sc, D2D1::Point2F(avatarRect_.cx(), panel.y)) *
                    Matrix3x2F::Translation(0, (1 - t) * -8.f * c.Motion()));
    g.SoftShadow(panel, 24, 34, Color::Black(theme_.dark ? 0.45f : 0.16f), 14);
    g.FillRound(panel, 24, theme_.dark ? Color::Hex(0x1C1C1E, 0.97f) : Color::Hex(0xFFFFFF, 0.98f));
    g.StrokeRound(panel, 24, theme_.glassStroke, 1);

    // Header
    Vec2 av{panel.x + 24 + 36, panel.y + 24 + 36};
    DrawAvatar(av, 36);
    std::wstring name = library_.steam().personaName();
    TextStyle ns = Body(22, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    ns.font = FontRole::Display;
    g.Text(name.empty() ? L"Jogador" : name, {av.x + 52, panel.y + 30, w - 140, 30}, ns);
    std::wstring sub = library_.steam().available() ? L"Conta Steam" : L"Perfil local";
    g.FillCircle({av.x + 58, panel.y + 74}, 4, online_ ? Color::Hex(0x30D158) : theme_.text3);
    g.Text(sub + (online_ ? L"  ·  online" : L"  ·  offline"), {av.x + 68, panel.y + 64, w - 150, 20}, Body(14, theme_.text2));

    // Stats
    float y = panel.y + 24 + 72 + 20;
    Rect statsR{panel.x + 16, y, w - 32, 76};
    g.FillRound(statsR, 16, theme_.control);
    struct Stat {
        std::wstring value, label;
    } stats[3] = {{std::to_wstring(games), games == 1 ? L"JOGO" : L"JOGOS"},
                  {FormatHoursShort(minutes), L"JOGADAS"},
                  {FormatInt(achUnlocked), L"CONQUISTAS"}};
    float cw = statsR.w / 3;
    for (int i = 0; i < 3; ++i) {
        Rect cell{statsR.x + i * cw, statsR.y, cw, statsR.h};
        TextStyle vs = Body(22, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        vs.font = FontRole::Display;
        vs.align = Align::Center;
        g.Text(stats[i].value, {cell.x, cell.y + 12, cell.w, 30}, vs);
        g.Text(stats[i].label, {cell.x, cell.y + 46, cell.w, 16}, Caps(10, theme_.text2, Align::Center));
        if (i > 0) g.FillRect({cell.x, cell.y + 18, 1, cell.h - 36}, theme_.separator);
    }
    y += 88;

    // Today: Apple-Watch-like ring against the optional daily goal
    {
        int64_t today = TodaySeconds();
        int goal = settings_.DailyGoalMinutes();
        Rect tr{panel.x + 16, y, w - 32, 64};
        Vec2 rc{tr.x + 32, tr.cy()};
        float frac = goal ? std::min(1.f, today / (goal * 60.f)) : (today > 0 ? 1.f : 0.f);
        float anim = c.AnimFrom(Id("todayring"), frac, 0.f, 5.f);
        Color ringCol = goal && today >= goal * 60 ? Color::Hex(0xFF9F0A) : theme_.accent;
        Ring(c, rc, 20, 6, anim, ringCol, theme_.control);
        g.Text(L"HOJE", {rc.x + 36, tr.y + 12, 200, 14}, Caps(10, theme_.text2));
        std::wstring v = today >= 60 ? FormatPlaytime(today / 60) : L"0 min";
        if (goal) v += L" de " + FormatPlaytime(goal);
        g.Text(v, {rc.x + 36, tr.y + 28, w - 120, 24}, Body(17, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD));
        if (!goal)
            g.Text(L"defina uma meta em Ajustes", {tr.r() - 190, tr.y + 30, 180, 20}, [&] {
                TextStyle t = Body(12, theme_.text3);
                t.align = Align::Trailing;
                return t;
            }());
        y += 76;
    }

    // Now playing
    if (playing) {
        Rect np{panel.x + 16, y, w - 32, 60};
        g.FillRound(np, 16, theme_.accent.Fade(0.14f));
        Rect thumb{np.x + 10, np.y + 8, 30, 44};
        GameArt(c, *playing, thumb, 6, ArtStyle::Portrait);
        g.Text(L"JOGANDO AGORA", {thumb.r() + 14, np.y + 11, 200, 14}, Caps(10, theme_.accent));
        int64_t mins = (NowUnix() - launcher_.RunningSince(running)) / 60;
        g.Text(playing->name + L"  ·  " + (mins < 1 ? std::wstring(L"agora mesmo") : FormatPlaytime(mins)),
               {thumb.r() + 14, np.y + 28, np.w - 70, 22}, Body(15, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD));
        y += 72;
    }

    // Actions
    g.FillRect({panel.x + 24, y + 4, w - 48, 1}, theme_.separator);
    y += 12;
    for (size_t i = 0; i < actions.size(); ++i) {
        Rect row{panel.x + 8, y, w - 16, rowH - 4};
        bool hov = c.Hover(row);
        if (hov) c.wantHand = true;
        float f = c.Anim(Id("profilerow", i), hov ? 1.f : 0.f, 22.f);
        if (f > 0.01f) g.FillRound(row, 12, theme_.control.Fade(f * 1.4f));
        Color fg = actions[i].destructive ? Color::Hex(0xFF453A) : theme_.text;
        g.Icon(actions[i].glyph, {row.x + 26, row.cy()}, 16, actions[i].destructive ? fg : theme_.text2);
        TextStyle ls = Body(16, fg, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        ls.valign = VAlign::Center;
        g.Text(actions[i].label, {row.x + 52, row.y, row.w - 60, row.h}, ls);
        if (c.Click(row) && profileOpen_) {
            profileOpen_ = false;
            auto fn = actions[i].fn;
            fn();
            break;
        }
        y += rowH;
    }
    g.PopTransform();
    g.PopAlpha();

    // Click outside (but not on the avatar, which toggles) closes.
    if (profileOpen_ && c.in.released && !panel.Contains(c.in.mouse) && !avatarRect_.Contains(c.in.mouse) &&
        time_ - profileOpenedAt_ > 0.15)
        profileOpen_ = false;
    c.blockMouse = savedBlock;
}

bool App::HandleSheetInput(Ctx& c) {
    InputFrame& in = c.in;
    if (sheet_.textInput) {
        for (wchar_t ch : in.text)
            if (sheet_.text.size() < 240) sheet_.text += ch;  // long enough for notes
        if (in.Has(Key::Paste) && OpenClipboard(hwnd_)) {
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                    sheet_.text = (sheet_.text + Trim(p)).substr(0, 240);
                    GlobalUnlock(h);
                }
            }
            CloseClipboard();
        }
        if (in.Has(Key::Backspace) && !sheet_.text.empty()) sheet_.text.pop_back();
        if (in.Has(Key::Accept)) {
            auto fn = sheet_.onSubmit;
            std::wstring text = sheet_.text;
            sheet_.open = false;
            if (fn) fn(text);
            return true;
        }
        if (in.Has(Key::Escape)) sheet_.open = false;
        return true;
    }
    int n = static_cast<int>(sheet_.items.size());
    if (in.Has(Key::Up)) sheet_.focus = (sheet_.focus + n - 1) % std::max(1, n);
    if (in.Has(Key::Down)) sheet_.focus = (sheet_.focus + 1) % std::max(1, n);
    if (in.Has(Key::Back) || in.Has(Key::Menu)) sheet_.open = false;
    if (in.Has(Key::Accept) && sheet_.focus < n) {
        SheetItem& it = sheet_.items[sheet_.focus];
        if (it.checkable) it.checked = !it.checked;
        if (!it.keepOpen) sheet_.open = false;
        if (it.action) {
            auto fn = it.action;  // the action may replace the sheet
            fn();
        }
    }
    return true;
}

void App::DrawSheet(Ctx& c) {
    Gfx& g = gfx_;
    float t = c.Anim(Id("sheetT"), sheet_.open ? 1.f : 0.f, sheet_.open ? 14.f : 18.f);
    if (t <= 0.01f) return;
    g.FillRect({0, 0, g.W, g.H}, Color::Black(0.45f * t));
    bool savedBlock = c.blockMouse;
    c.blockMouse = !sheet_.open;
    float w = 560;
    float headH = sheet_.subtitle.empty() ? 76 : 96;
    float bodyH = sheet_.textInput ? 150 : sheet_.items.size() * 58.f + 16;
    Rect panel{(g.W - w) * 0.5f, (g.H - headH - bodyH) * 0.5f, w, headH + bodyH};
    float sc = 0.94f + 0.06f * EaseOut(t);
    g.PushAlpha(t);
    g.PushTransform(Matrix3x2F::Scale(sc, sc, D2D1::Point2F(panel.cx(), panel.cy())));
    g.SoftShadow(panel, 26, 40, Color::Black(theme_.dark ? 0.5f : 0.18f), 18);
    g.FillRound(panel, 26, theme_.dark ? Color::Hex(0x1C1C1E, 0.97f) : Color::Hex(0xFFFFFF, 0.98f));
    g.StrokeRound(panel, 26, theme_.glassStroke, 1);

    TextStyle title = Body(22, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    title.font = FontRole::Display;
    g.Text(sheet_.title, {panel.x + 28, panel.y + 24, panel.w - 56, 30}, title);
    if (!sheet_.subtitle.empty())
        g.Text(sheet_.subtitle, {panel.x + 28, panel.y + 56, panel.w - 56, 22}, Body(15, theme_.text2));

    float y = panel.y + headH;
    if (sheet_.textInput) {
        Rect field{panel.x + 24, y, panel.w - 48, 50};
        TextField(c, Id("sheetfield"), field, sheet_.text, sheet_.placeholder.c_str(), true, glyph::Edit);
        Rect cancel{panel.r() - 24 - 260 - 12, y + 72, 130, 46}, ok{panel.r() - 24 - 130, y + 72, 130, 46};
        BtnSpec bc{L"Cancelar"};
        BtnSpec bo{L"Salvar"};
        bo.kind = BtnKind::Primary;
        if (Button(c, Id("sheetcancel"), cancel, bc)) sheet_.open = false;
        if (Button(c, Id("sheetok"), ok, bo) && sheet_.open) {
            auto fn = sheet_.onSubmit;
            std::wstring text = sheet_.text;
            sheet_.open = false;
            if (fn) fn(text);
        }
    } else {
        for (size_t i = 0; i < sheet_.items.size(); ++i) {
            SheetItem& it = sheet_.items[i];
            Rect row{panel.x + 10, y, panel.w - 20, 54};
            bool hover = c.Hover(row);
            if (hover && c.in.mouseMode) sheet_.focus = static_cast<int>(i);
            bool focused = sheet_.focus == static_cast<int>(i);
            float f = c.Anim(Id("sheetrow", i), focused ? 1.f : 0.f, 22.f);
            if (f > 0.01f) g.FillRound(row, 14, theme_.control.Fade(f * 1.4f));
            if (hover) c.wantHand = true;
            Color fg = it.destructive ? Color::Hex(0xFF453A) : theme_.text;
            if (it.glyph) g.Icon(it.glyph, {row.x + 30, row.cy()}, 17, it.destructive ? fg : theme_.text2);
            TextStyle ls = Body(17, fg, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            ls.valign = VAlign::Center;
            if (!it.detail.empty()) {
                g.Text(it.label, {row.x + 58, row.y + 6, row.w - 120, 24}, ls);
                g.Text(it.detail, {row.x + 58, row.y + 29, row.w - 120, 20}, Body(13, theme_.text2));
            } else {
                g.Text(it.label, {row.x + 58, row.y, row.w - 120, row.h}, ls);
            }
            if (it.checkable) {
                float ck = c.Anim(Id("sheetck", i), it.checked ? 1.f : 0.f, 20.f);
                Vec2 cc{row.r() - 32, row.cy()};
                g.FillCircle(cc, 13, Color::Lerp(theme_.control, theme_.accent, ck));
                if (ck > 0.05f) g.Icon(glyph::Checkmark, cc, 13, Color::White(ck));
            }
            if (c.Click(row)) {
                sheet_.focus = static_cast<int>(i);
                if (it.checkable) it.checked = !it.checked;
                if (!it.keepOpen) sheet_.open = false;
                if (it.action) {
                    auto fn = it.action;
                    fn();
                    break;  // the action may have replaced the sheet
                }
            }
            y += 58;
        }
    }
    g.PopTransform();
    g.PopAlpha();
    // Click outside closes.
    if (sheet_.open && c.in.released && !panel.Contains(c.in.mouse) && !panel.Contains(c.in.pressPos) &&
        time_ - sheet_.openedAt > 0.2)
        sheet_.open = false;
    c.blockMouse = savedBlock;
}

void App::DrawToast(Ctx& c) {
    double age = time_ - toast_.shownAt;
    if (age > 3.2 || toast_.text.empty()) return;
    float t = EaseOut(static_cast<float>(std::min(age / 0.25, (3.2 - age) / 0.35)));
    Gfx& g = gfx_;
    TextStyle ts = Body(16, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    ts.valign = VAlign::Center;
    float tw = std::min(g.Measure(toast_.text, ts).x, g.W * 0.6f);
    float w = tw + (toast_.glyph ? 76 : 48);
    Rect r{(g.W - w) * 0.5f, 104 - (1 - t) * 14 * c.Motion(), w, 50};
    g.PushAlpha(t);
    g.SoftShadow(r, 25, 26, Color::Black(0.35f), 10);
    g.FillRound(r, 25, theme_.dark ? Color::Hex(0x2C2C2E, 0.96f) : Color::Hex(0xFFFFFF, 0.97f));
    g.StrokeRound(r, 25, theme_.glassStroke, 1);
    float x = r.x + 24;
    if (toast_.glyph) {
        g.Icon(toast_.glyph, {x + 9, r.cy()}, 17, theme_.accent);
        x += 30;
    }
    g.Text(toast_.text, {x, r.y, tw + 4, r.h}, ts);
    g.PopAlpha();
}

}  // namespace lumen
