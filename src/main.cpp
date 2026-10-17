// Lumen — visual game library for Windows.
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <string>

#include "app/App.h"
#include "core/Util.h"

static lumen::LaunchOptions ParseArgs() {
    lumen::LaunchOptions o;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--snapshot") o.snapshot = next();
        else if (a == L"--size") {
            std::wstring s = next();
            size_t x = s.find(L'x');
            if (x != std::wstring::npos) {
                o.width = _wtoi(s.substr(0, x).c_str());
                o.height = _wtoi(s.substr(x + 1).c_str());
            }
        } else if (a == L"--tab") {
            std::wstring t = next();
            o.tab = t == L"library" ? lumen::Tab::Library
                  : t == L"deals"   ? lumen::Tab::Deals
                  : t == L"stats"   ? lumen::Tab::Stats
                  : t == L"settings" ? lumen::Tab::Settings
                                    : lumen::Tab::Home;
        } else if (a == L"--page") o.page = true;
        else if (a == L"--sheet") o.sheet = true;
        else if (a == L"--profile") o.profile = true;
        else if (a == L"--notify-test") o.notifyTest = true;
        else if (a == L"--tray") o.startInTray = true;
        else if (a == L"--celebrate") o.celebrate = true;
        else if (a == L"--launchfx") o.launchFx = true;
        else if (a == L"--defaults") o.defaults = true;
        else if (a == L"--restore-look") o.restoreLook = true;
        else if (a == L"--light") o.light = true;
        else if (a == L"--game") o.game = _wtoi(next().c_str());
        else if (a == L"--scroll") o.scroll = static_cast<float>(_wtof(next().c_str()));
        else if (a == L"--wait") o.wait = static_cast<float>(_wtof(next().c_str()));
        else if (a == L"--search") o.search = next();
        else if (a == L"--add") o.addPath = next();
    }
    LocalFree(argv);
    return o;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    lumen::LaunchOptions opts = ParseArgs();
    HANDLE mutex = nullptr;
    if (opts.snapshot.empty()) {
        // One library window at a time: a second launch just brings the first one forward.
        mutex = CreateMutexW(nullptr, TRUE, L"Lumen.SingleInstance");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND w = FindWindowW(L"LumenWindow", nullptr);
            // A newer build replaces an older one that is still running (often hidden in the tray):
            // the old instance saves and quits, then we take over the single-instance mutex.
            int64_t build = lumen::OwnBuildTime();
            COPYDATASTRUCT upd{0x4C55 /* "LU": update */, sizeof(build), &build};
            if (w && SendMessageW(w, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&upd)) == TRUE) {
                DWORD r = WaitForSingleObject(mutex, 15000);
                if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) w = nullptr;
            }
            if (w) {
                if (opts.notifyTest) {
                    COPYDATASTRUCT nt{0x4C4E /* "LN": notification test */, 0, nullptr};
                    SendMessageW(w, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&nt));
                    return 0;
                }
                if (!opts.addPath.empty()) {
                    COPYDATASTRUCT cds{0x4C41 /* "LA": add game */, static_cast<DWORD>((opts.addPath.size() + 1) * sizeof(wchar_t)),
                                       const_cast<wchar_t*>(opts.addPath.c_str())};
                    SendMessageW(w, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds));
                }
                if (!IsWindowVisible(w)) ShowWindow(w, SW_SHOW);  // was living in the tray
                if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
                SetForegroundWindow(w);
                return 0;
            }
        }
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int code;
    {
        lumen::App app;
        code = app.Run(inst, opts);
    }
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return code;
}
