#pragma once
// Starts games and observes running sessions:
//  - executables started by Lumen are tracked through their process handle;
//  - Steam games are detected through HKCU\Software\Valve\Steam\RunningAppID.
#include <cstdint>
#include <string>
#include <vector>

#include "Game.h"

namespace lumen {

class Library;

class Launcher {
public:
    ~Launcher();
    bool Launch(Library& lib, Game& g, std::wstring& error);
    void OpenInstallFolder(const Game& g) const;
    void OpenInPlatform(const Game& g) const;
    bool HasPlatformPage(const Game& g) const { return g.platform != Platform::Custom; }

    // Call ~every frame; cheap (internally throttled). Returns true when a session ended
    // and platform data (e.g. Steam playtime) should be refreshed.
    bool Poll(Library& lib, double now);

    bool IsRunning(const std::string& id) const;
    int64_t RunningSince(const std::string& id) const;
    std::string RunningGame() const;

private:
    struct Session {
        std::string id;
        void* process = nullptr;
        int64_t start = 0;
    };
    std::vector<Session> sessions_;
    std::string steamRunning_;
    int64_t steamStart_ = 0;
    double nextPoll_ = 0;
};

}  // namespace lumen
