#include "Launcher.h"

#include <windows.h>
#include <shellapi.h>

#include "Library.h"
#include "Util.h"
#include "providers/Steam.h"

namespace lumen {

Launcher::~Launcher() {
    for (auto& s : sessions_)
        if (s.process) CloseHandle(s.process);
}

bool Launcher::Launch(Library& lib, Game& g, std::wstring& error) {
    if (IsRunning(g.id)) {
        error = L"O jogo já está em execução.";
        return false;
    }
    bool viaUri = !g.launchUri.empty();
    if (viaUri) {
        auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", g.launchUri.c_str(), nullptr, nullptr,
                                                         SW_SHOWNORMAL));
        if (r <= 32) {
            error = std::wstring(L"Não foi possível abrir a ") + PlatformName(g.platform) + L". Ela está instalada?";
            return false;
        }
    } else {
        if (g.exePath.empty() || !FileExists(g.exePath)) {
            error = L"Executável não encontrado. Ele foi movido ou desinstalado?";
            return false;
        }
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        sei.lpVerb = L"open";
        sei.lpFile = g.exePath.c_str();
        sei.lpParameters = g.args.empty() ? nullptr : g.args.c_str();
        sei.lpDirectory = g.installDir.empty() ? nullptr : g.installDir.c_str();
        sei.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&sei)) {
            DWORD e = GetLastError();
            error = e == ERROR_CANCELLED ? L"Inicialização cancelada." : L"Não foi possível iniciar o executável.";
            return false;
        }
        if (sei.hProcess) sessions_.push_back(Session{g.id, sei.hProcess, NowUnix()});
    }
    lib.RecordLaunch(g);
    return true;
}

void Launcher::OpenInstallFolder(const Game& g) const {
    if (!g.installDir.empty() && DirExists(g.installDir))
        ShellExecuteW(nullptr, L"open", g.installDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void Launcher::OpenInPlatform(const Game& g) const {
    std::wstring uri;
    switch (g.platform) {
        case Platform::Steam: uri = L"steam://nav/games/details/" + Widen(g.platformId); break;
        case Platform::Epic: uri = L"com.epicgames.launcher://apps"; break;
        case Platform::GOG: uri = L"goggalaxy://openGameView/" + Widen(g.platformId); break;
        default: return;
    }
    ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool Launcher::Poll(Library& lib, double now) {
    if (now < nextPoll_) return false;
    nextPoll_ = now + 1.5;
    bool ended = false;
    for (size_t i = 0; i < sessions_.size();) {
        if (WaitForSingleObject(sessions_[i].process, 0) == WAIT_OBJECT_0) {
            lib.RecordSession(sessions_[i].id, NowUnix() - sessions_[i].start);
            CloseHandle(sessions_[i].process);
            sessions_.erase(sessions_.begin() + static_cast<ptrdiff_t>(i));
            ended = true;
        } else {
            ++i;
        }
    }
    uint32_t app = SteamProvider::RunningAppId();
    std::string id = app ? "steam:" + std::to_string(app) : std::string();
    if (id != steamRunning_) {
        if (!steamRunning_.empty()) {
            lib.RecordSession(steamRunning_, NowUnix() - steamStart_);
            ended = true;
        }
        steamRunning_ = id;
        steamStart_ = NowUnix();
    }
    return ended;
}

bool Launcher::IsRunning(const std::string& id) const {
    if (id == steamRunning_) return true;
    for (auto& s : sessions_)
        if (s.id == id) return true;
    return false;
}

int64_t Launcher::RunningSince(const std::string& id) const {
    if (id == steamRunning_) return steamStart_;
    for (auto& s : sessions_)
        if (s.id == id) return s.start;
    return 0;
}

std::string Launcher::RunningGame() const {
    if (!steamRunning_.empty()) return steamRunning_;
    return sessions_.empty() ? std::string() : sessions_.front().id;
}

}  // namespace lumen
