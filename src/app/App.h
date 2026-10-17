#pragma once
// Application shell: window, frame loop, navigation between screens, modal sheets and toasts.
// Screens are implemented in Showcase.cpp, LibraryView.cpp, StatsView.cpp and SettingsView.cpp.
#include <windows.h>
#include <shellapi.h>

#include <deque>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "core/Deals.h"
#include "core/Launcher.h"
#include "core/Library.h"
#include "core/Metadata.h"
#include "core/Settings.h"
#include "core/Trailers.h"
#include "core/UserStore.h"
#include "ui/Gfx.h"
#include "ui/ImageCache.h"
#include "ui/Input.h"
#include "ui/Sound.h"
#include "ui/Ui.h"
#include "ui/Video.h"

namespace lumen {

enum class Tab { Home = 0, Library = 1, Deals = 2, Stats = 3, Settings = 4 };

struct LaunchOptions {
    std::wstring snapshot;  // render one frame to this PNG and exit (used for automated visual checks)
    int width = 1920, height = 1080;
    Tab tab = Tab::Home;
    bool page = false;
    bool sheet = false;
    bool profile = false;
    bool notifyTest = false;  // --notify-test: send a sample wellbeing notification
    bool light = false;
    int game = 0;
    float scroll = 0;
    float wait = 4.f;
    std::wstring search;
    std::wstring addPath;  // --add <exe>: add a game (also forwarded to a running instance)
    bool startInTray = false;  // --tray: start hidden (used when launched with Windows)
    bool celebrate = false;    // --celebrate: preview the achievement celebration (snapshot testing)
    bool launchFx = false;     // --launchfx: preview the launch animation (snapshot testing)
    bool defaults = false;     // --defaults: snapshot with factory settings
    bool restoreLook = false;  // --restore-look: turn motion, blur, online data and reminders back on
};

// Something worth celebrating, detected from real data after a play session.
struct Celebration {
    enum class Kind { Achievements, Milestone } kind = Kind::Achievements;
    std::string gameId;
    int count = 0;  // new achievements, or hours for a milestone
    double start = -1;
};

struct SheetItem {
    std::wstring label;
    wchar_t glyph = 0;
    std::wstring detail;
    bool checkable = false;
    bool checked = false;
    bool destructive = false;
    bool keepOpen = false;
    std::function<void()> action;
};

struct Sheet {
    bool open = false;
    std::wstring title, subtitle;
    std::vector<SheetItem> items;
    bool textInput = false;
    std::wstring text, placeholder;
    std::function<void(const std::wstring&)> onSubmit;
    int focus = 0;
    double openedAt = 0;
};

class App {
public:
    int Run(HINSTANCE inst, const LaunchOptions& opts);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    // --- lifecycle
    bool CreateMainWindow(HINSTANCE inst);
    void Frame();
    void ApplyTheme();
    void ToggleFullscreen();
    void BackgroundTick();  // runs even when the window is hidden: sessions, reminders, saving
    void CheckPlayReminders();
    void SetupTray();
    void RemoveTray();
    void ShowFromTray();
    void Notify(const std::wstring& title, const std::wstring& body);
    void SendTestNotification();
    void SaveIfNeeded(bool force = false);
    void RefreshAll(bool announce);
    void RebuildHomeList();
    MetadataConfig MetaConfig() const;

    // --- shared actions
    void SwitchTab(Tab t);
    void OpenGame(const std::string& id, Tab returnTo);
    void CloseGamePage();
    void LaunchGame(Game& g);
    void ToggleFavorite(Game& g);
    void OpenGameMenu(Game& g);
    void OpenCategorySheet(const std::string& id);
    void OpenTextSheet(const std::wstring& title, const std::wstring& placeholder,
                       std::function<void(const std::wstring&)> onSubmit);
    void AddGameDialog();
    void AddGameFromPath(const std::wstring& path, bool openPage);
    void HandleDroppedFile(const std::wstring& path);
    bool PickFile(const wchar_t* title, bool images, std::wstring& out);
    void ShowToast(const std::wstring& text, wchar_t glyph = 0);
    void PickRandomGame();
    Game* SelectedGame();
    void RequestGameData(const Game& g);
    void RequestStoreArt();
    void OpenStatusSheet(const std::string& id);
    void OpenNotesSheet(const std::string& id);
    void OpenArtSheet(const std::string& id);
    void PlaySfx(ui::Sfx s);
    void UpdateTrailer();
    void AttractTick();
    void RefreshProgress();  // re-read playtime/achievements and celebrate what changed
    bool Animating() const;  // something on screen needs full frame rate

    // --- time & wellbeing
    int64_t TodaySeconds() const;  // play time observed today (finished sessions + the running one)
    static int64_t LocalMidnight(int64_t unix);
    static bool LaunchAtStartup();
    static void SetLaunchAtStartup(bool on);
    const std::vector<std::pair<std::wstring, std::wstring>>& ScreenshotsOf(const Game& g);

    // --- drawing
    void DrawTopBar(ui::Ctx& c, float alpha);
    void DrawStatus(ui::Ctx& c, float alpha);
    void DrawShowcase(ui::Ctx& c);
    void DrawBackground(ui::Ctx& c, Game* g, float pageT);
    void DrawGamePage(ui::Ctx& c, Game& g, float pageT);
    void DrawAchievementsSection(ui::Ctx& c, Game& g, float y, float& outBottom);
    void DrawAboutSection(ui::Ctx& c, Game& g, float y, float& outBottom);
    void DrawScreenshotsSection(ui::Ctx& c, Game& g, float y, float& outBottom);
    void DrawNewsSection(ui::Ctx& c, Game& g, float y, float& outBottom);
    void DrawAmbient(ui::Ctx& c, Game& g);
    void DrawLaunchFx(ui::Ctx& c);
    void DrawCelebration(ui::Ctx& c);
    bool HasFreshNews(const Game& g) const;
    void DrawGreeting(ui::Ctx& c, float alpha);
    void DrawLibrary(ui::Ctx& c);
    void DrawStats(ui::Ctx& c);
    void DrawDeals(ui::Ctx& c);
    void DrawSettings(ui::Ctx& c);
    void DrawSheet(ui::Ctx& c);
    void DrawProfile(ui::Ctx& c);
    void DrawAvatar(ui::Vec2 center, float radius);
    void DrawToast(ui::Ctx& c);
    bool HandleSheetInput(ui::Ctx& c);
    void HandleShowcaseKeys(ui::Ctx& c);

    // --- services
    Settings settings_;
    UserStore user_;
    Library library_{settings_, user_};
    MetadataService meta_;
    Launcher launcher_;
    ui::Gfx gfx_;
    ui::ImageCache images_;
    ui::Input input_;
    ui::Theme theme_;
    ui::Ctx* ctx_ = nullptr;

    // --- window
    HWND hwnd_ = nullptr;
    bool fullscreen_ = false;
    bool fullscreenPending_ = false;
    WINDOWPLACEMENT savedPlacement_{sizeof(WINDOWPLACEMENT)};
    bool inFrame_ = false;
    bool active_ = true;
    bool hand_ = false;
    double time_ = 0;
    double lastInput_ = 0;  // drives idle throttling (wall clock)
    double lastFrameWall_ = 0;
    LARGE_INTEGER qpcFreq_{}, qpcLast_{}, qpcStart_{};
    double Wall() const;    // seconds since start
    LaunchOptions opts_;
    bool snapshotMode_ = false;

    // --- persistence
    bool settingsDirty_ = false;
    double lastSave_ = 0;
    double lastActivityRefresh_ = 0;
    bool needPlaytimeRefresh_ = false;
    double playtimeRefreshAt_ = 0;

    // --- navigation
    Tab tab_ = Tab::Home;
    Tab prevTab_ = Tab::Home;
    double tabChangedAt_ = -10;
    float pageT_ = 0;  // 0 = catalog, 1 = game page (animated)

    struct ShowcaseState {
        std::vector<int> list;  // library indices shown in the carousel
        int sel = 0;
        std::string selId;
        int row = 1;            // 0 = action buttons, 1 = carousel / page content
        int button = 0;
        bool page = false;
        Tab returnTab = Tab::Home;
        float scroll = 0;       // carousel scroll (logical units)
        float pageScroll = 0, pageScrollTarget = 0, pageMax = 0;
        std::string bgId, bgPrevId;
        double bgChangedAt = -10;
        std::string pendingBg;
        double pendingSince = 0;
        float wheelAcc = 0;
        int listRevision = -1;
        double launchedAt = -10;
        std::string launchedId;
    } show_;

    struct LibraryState {
        std::wstring search;
        Filter filter = Filter::All;
        std::wstring category;
        PlayStatus status = PlayStatus::None;
        int zone = 2;           // 0 toolbar, 1 chips, 2 grid
        int toolbarFocus = 0;
        int chipFocus = 0;
        int sel = 0;
        float scroll = 0, scrollTarget = 0;
        bool editing = false;
        std::vector<int> results;
    } lib_;

    struct StatsState {
        float scroll = 0, scrollTarget = 0;
    } stats_;

    struct SettingsState {
        int focus = 0;
        float scroll = 0, scrollTarget = 0;
        bool editingKey = false;
        std::wstring keyDraft;
    } set_;

    Sheet sheet_;
    struct Toast {
        std::wstring text;
        wchar_t glyph = 0;
        double shownAt = -10;
    } toast_;

    // tray & wellbeing notifications
    NOTIFYICONDATAW tray_{};
    bool trayAdded_ = false;
    HICON balloonIcon_ = nullptr;  // kept alive: Explorer loads it after Shell_NotifyIcon returns
    bool quitting_ = false;
    int64_t ownBuild_ = 0;  // this executable's build time, compared when a newer build is launched
    UINT taskbarCreatedMsg_ = 0;
    std::string reminderGameId_;
    int64_t reminderStart_ = 0;
    int64_t remindersSent_ = 0;

    // profile popover (opened from the avatar)
    bool profileOpen_ = false;
    double profileOpenedAt_ = 0;
    ui::Rect avatarRect_;

    // store deals
    DealsService deals_;
    struct DealItem {
        size_t store = 0;
        Deal deal;
        bool owned = false;  // a game with this name is already in the library
    };
    struct DealsState {
        std::vector<StoreDeals> stores;
        std::vector<DealItem> items;
        int filter = 0;  // 0 = all stores, otherwise index into stores + 1
        int sort = 0;    // 0 biggest discount, 1 lowest price
        int sel = 0;
        int zone = 1;    // 0 chips, 1 grid
        float scroll = 0, scrollTarget = 0;
        double lastForced = -1000;
        double lastRequest = -1e9;
    } dealsUi_;

    // trailers, launch animation, celebrations, showcase mode
    TrailerService trailers_;
    ui::VideoPlayer video_;
    std::string videoGameId_, videoWantId_;
    double videoWantSince_ = 0;
    std::set<std::string> videoFailed_;
    struct LaunchFx {
        std::string id;
        double start = -10;
    } launchFx_;
    std::deque<Celebration> celebrations_;
    double nextAttract_ = 0, awakeUntil_ = 0, secondRefreshAt_ = 0;

    // sounds, dynamic accent, controller, daily goal
    ui::SoundBoard sound_;
    ui::Color dynamicAccent_{0, 0, 0, 0};
    bool padConnected_ = false;
    int64_t goalNotifiedDay_ = 0;
    struct ShotCache {
        std::string id;
        double at = -100;
        std::vector<std::pair<std::wstring, std::wstring>> shots;
    } shots_;

    // status bar
    double lastStatusPoll_ = -10;
    bool online_ = true;
    int battery_ = -1;
    bool charging_ = false;
};

}  // namespace lumen
