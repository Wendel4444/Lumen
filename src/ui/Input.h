#pragma once
// Unifies keyboard, mouse and XInput controllers into per-frame actions.
#include <windows.h>

#include <string>
#include <vector>

#include "Gfx.h"

namespace lumen::ui {

enum class Key {
    Left, Right, Up, Down,
    Accept, Back, Info, Favorite, Menu,
    NextTab, PrevTab, Search, PageUp, PageDown, Home, End,
    Random, Refresh, Fullscreen, AddGame, Settings,
    Tab1, Tab2, Tab3, Tab4, Tab5,
    Backspace, Delete, Paste, SelectAll, Escape
};

struct InputFrame {
    std::vector<Key> keys;
    std::wstring text;  // typed characters
    Vec2 mouse{-1, -1};
    Vec2 pressPos{-1, -1};
    bool mouseDown = false, pressed = false, released = false, rightReleased = false, doubleClick = false;
    float wheel = 0;    // notches, positive = up
    bool mouseMode = false;  // last interaction came from the mouse
    bool gamepad = false;    // a controller is connected
    bool ctrl = false, shift = false;

    bool Has(Key k) const {
        for (Key x : keys)
            if (x == k) return true;
        return false;
    }
};

class Input {
public:
    // Returns true if the message was consumed.
    bool HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, float scale);
    void PollGamepad(float dt);
    InputFrame& frame() { return frame_; }
    void EndFrame();  // clears per-frame events
    bool ConsumeActivity() {
        bool a = activity_;
        activity_ = false;
        return a;
    }
    bool textMode = false;  // when a text field is focused, letters go to text instead of shortcuts
private:
    void Emit(Key k);
    InputFrame frame_;
    bool activity_ = false;
    Vec2 lastRealMouse_{-1, -1};
    WORD padPrev_ = 0;
    int padIndex_ = -1;
    float padRescan_ = 0;
    float repeatTimer_ = 0;
    int heldDir_ = -1;
    float wheelAcc_ = 0;
    double lastClick_ = 0;
};

}  // namespace lumen::ui
