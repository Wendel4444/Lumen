#pragma once
#include <vector>

#include "../Game.h"

namespace lumen {

// Epic Games Launcher: install manifests (*.item) + the launcher's catalog cache for art and descriptions.
class EpicProvider {
public:
    bool Scan(std::vector<Game>& out);
};

// GOG Galaxy / offline installers register games under HKLM\SOFTWARE\WOW6432Node\GOG.com\Games.
class GogProvider {
public:
    bool Scan(std::vector<Game>& out);
};

}  // namespace lumen
