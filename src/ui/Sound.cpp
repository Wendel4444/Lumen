#include "Sound.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lumen::ui {

namespace {
constexpr int kRate = 44100;
constexpr float kPi = 3.14159265f;

struct Tone {
    float startHz, endHz;  // linear glide
    float seconds;
    float amp;
    float delay = 0;       // offset from the start of the sound
};

// Renders a few overlapping tones into a 16-bit mono WAV image (header + samples).
std::vector<uint8_t> Render(std::initializer_list<Tone> tones) {
    float total = 0;
    for (const Tone& t : tones) total = std::max(total, t.delay + t.seconds);
    size_t n = static_cast<size_t>(total * kRate) + 1;
    std::vector<float> mix(n, 0.f);
    for (const Tone& t : tones) {
        size_t start = static_cast<size_t>(t.delay * kRate), len = static_cast<size_t>(t.seconds * kRate);
        float phase = 0;
        for (size_t i = 0; i < len && start + i < n; ++i) {
            float u = static_cast<float>(i) / len;
            float hz = t.startHz + (t.endHz - t.startHz) * u;
            phase += 2 * kPi * hz / kRate;
            float attack = std::min(1.f, i / (0.004f * kRate));  // 4 ms fade-in: no click
            float decay = std::exp(-5.f * u);
            // Sine plus a quiet octave for a softer, "glassy" timbre.
            mix[start + i] += t.amp * attack * decay * (std::sin(phase) + 0.25f * std::sin(2 * phase));
        }
    }
    std::vector<uint8_t> out(44 + n * 2);
    auto put32 = [&](size_t off, uint32_t v) { std::memcpy(&out[off], &v, 4); };
    auto put16 = [&](size_t off, uint16_t v) { std::memcpy(&out[off], &v, 2); };
    std::memcpy(&out[0], "RIFF", 4);
    put32(4, static_cast<uint32_t>(36 + n * 2));
    std::memcpy(&out[8], "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);  // PCM
    put16(22, 1);  // mono
    put32(24, kRate);
    put32(28, kRate * 2);
    put16(32, 2);
    put16(34, 16);
    std::memcpy(&out[36], "data", 4);
    put32(40, static_cast<uint32_t>(n * 2));
    for (size_t i = 0; i < n; ++i) {
        float v = std::max(-1.f, std::min(1.f, mix[i]));
        int16_t s = static_cast<int16_t>(v * 32000);
        std::memcpy(&out[44 + i * 2], &s, 2);
    }
    return out;
}
}  // namespace

void SoundBoard::Init() {
    wav_[static_cast<int>(Sfx::Move)] = Render({{1760, 1700, 0.045f, 0.07f}});
    wav_[static_cast<int>(Sfx::Toggle)] = Render({{1320, 1320, 0.05f, 0.07f}, {1980, 1980, 0.05f, 0.04f, 0.03f}});
    wav_[static_cast<int>(Sfx::Select)] = Render({{880, 880, 0.08f, 0.08f}, {1318, 1318, 0.12f, 0.08f, 0.05f}});
    wav_[static_cast<int>(Sfx::Back)] = Render({{1318, 1318, 0.08f, 0.07f}, {880, 880, 0.12f, 0.07f, 0.05f}});
    wav_[static_cast<int>(Sfx::Launch)] = Render({{523, 1046, 0.35f, 0.07f},
                                                  {659, 1318, 0.35f, 0.05f, 0.04f},
                                                  {784, 1568, 0.40f, 0.05f, 0.08f}});
    ready_ = true;
}

void SoundBoard::Play(Sfx s) {
    if (!ready_) return;
    const auto& w = wav_[static_cast<int>(s)];
    // SND_ASYNC with SND_MEMORY: the buffer must outlive playback; it lives as long as the app.
    PlaySoundW(reinterpret_cast<LPCWSTR>(w.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

}  // namespace lumen::ui
