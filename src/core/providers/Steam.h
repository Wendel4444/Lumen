#pragma once
#include <string>
#include <vector>

#include "../Game.h"

namespace lumen {

// Reads the local Steam installation: libraries, app manifests, playtime (localconfig.vdf),
// achievement counts and descriptions cached by the Steam client, collections and art cache.
class SteamProvider {
public:
    bool Scan(std::vector<Game>& out, bool allowCdnArt);
    void RefreshPlaytime(std::vector<Game>& games) const;  // re-read localconfig.vdf only

    bool available() const { return !steamPath_.empty(); }
    const std::wstring& steamPath() const { return steamPath_; }
    const std::wstring& personaName() const { return persona_; }
    const std::wstring& avatarPath() const { return avatar_; }
    const std::string& steamId64() const { return steamId64_; }
    std::wstring LibraryCacheFile(const std::string& appId) const;  // per-game JSON cached by the client
    // Screenshots taken with Steam (F12) for a game, newest first, as {full image, thumbnail}.
    std::vector<std::pair<std::wstring, std::wstring>> Screenshots(const std::string& appId) const;

    static uint32_t RunningAppId();  // 0 when no Steam game is running

private:
    std::wstring steamPath_, userDir_, persona_, avatar_;
    std::string steamId64_;
};

}  // namespace lumen
