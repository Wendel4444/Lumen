#pragma once
// Subtle, console-like interface sounds. Synthesized at startup (no audio files to ship) and played
// asynchronously through the system mixer.
#include <cstdint>
#include <vector>

namespace lumen::ui {

enum class Sfx { Move, Select, Back, Launch, Toggle, Count };

class SoundBoard {
public:
    void Init();
    void Play(Sfx s);

private:
    std::vector<uint8_t> wav_[static_cast<int>(Sfx::Count)];
    bool ready_ = false;
};

}  // namespace lumen::ui
