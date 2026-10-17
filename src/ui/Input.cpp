#include "Input.h"

#include <windowsx.h>
#include <xinput.h>

#include <cmath>

namespace lumen::ui {

void Input::Emit(Key k) {
    frame_.keys.push_back(k);
    frame_.mouseMode = false;
}

bool Input::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, float scale) {
    if (msg != WM_MOUSEMOVE && msg != WM_MOUSELEAVE &&
        ((msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST)))
        activity_ = true;
    switch (msg) {
        case WM_MOUSEMOVE: {
            Vec2 p{GET_X_LPARAM(lp) / scale, GET_Y_LPARAM(lp) / scale};
            // Windows also sends synthetic moves (on repaint, after WM_MOUSELEAVE...): only motion relative
            // to the last real position counts as activity.
            if (std::fabs(p.x - lastRealMouse_.x) + std::fabs(p.y - lastRealMouse_.y) > 0.5f) {
                frame_.mouseMode = true;
                activity_ = true;
                lastRealMouse_ = p;
            }
            frame_.mouse = p;
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            return true;
        }
        case WM_MOUSELEAVE:
            frame_.mouse = {-1000, -1000};
            return true;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            SetCapture(hwnd);
            frame_.mouse = {GET_X_LPARAM(lp) / scale, GET_Y_LPARAM(lp) / scale};
            frame_.pressPos = frame_.mouse;
            frame_.mouseDown = frame_.pressed = true;
            frame_.mouseMode = true;
            double t = GetMessageTime() / 1000.0;
            if (msg == WM_LBUTTONDBLCLK || t - lastClick_ < GetDoubleClickTime() / 1000.0) frame_.doubleClick = true;
            lastClick_ = t;
            return true;
        }
        case WM_LBUTTONUP:
            ReleaseCapture();
            frame_.mouse = {GET_X_LPARAM(lp) / scale, GET_Y_LPARAM(lp) / scale};
            frame_.mouseDown = false;
            frame_.released = true;
            return true;
        case WM_RBUTTONUP:
            frame_.mouse = {GET_X_LPARAM(lp) / scale, GET_Y_LPARAM(lp) / scale};
            frame_.rightReleased = true;
            frame_.mouseMode = true;
            return true;
        case WM_XBUTTONUP:
            if (GET_XBUTTON_WPARAM(wp) == XBUTTON1) Emit(Key::Back);
            return true;
        case WM_MOUSEWHEEL:
            frame_.wheel += GET_WHEEL_DELTA_WPARAM(wp) / static_cast<float>(WHEEL_DELTA);
            frame_.mouseMode = true;
            return true;
        case WM_MOUSEHWHEEL:
            frame_.wheel -= GET_WHEEL_DELTA_WPARAM(wp) / static_cast<float>(WHEEL_DELTA);
            frame_.mouseMode = true;
            return true;
        case WM_CHAR: {
            wchar_t c = static_cast<wchar_t>(wp);
            if (c >= 32 && c != 127) frame_.text += c;
            return true;
        }
        case WM_SYSKEYDOWN:
            if (wp == VK_RETURN && (lp & (1 << 29))) {
                Emit(Key::Fullscreen);
                return true;
            }
            return false;
        case WM_KEYDOWN: {
            bool ctrl = GetKeyState(VK_CONTROL) < 0;
            frame_.ctrl = ctrl;
            frame_.shift = GetKeyState(VK_SHIFT) < 0;
            switch (wp) {
                case VK_LEFT: Emit(Key::Left); break;
                case VK_RIGHT: Emit(Key::Right); break;
                case VK_UP: Emit(Key::Up); break;
                case VK_DOWN: Emit(Key::Down); break;
                case VK_RETURN: Emit(Key::Accept); break;
                case VK_SPACE:
                    if (!textMode) Emit(Key::Accept);
                    break;
                case VK_ESCAPE: Emit(Key::Escape); Emit(Key::Back); break;
                case VK_BACK: Emit(Key::Backspace); if (!textMode) Emit(Key::Back); break;
                case VK_DELETE: Emit(Key::Delete); break;
                case VK_TAB: Emit(frame_.shift ? Key::PrevTab : Key::NextTab); break;
                case VK_PRIOR: Emit(Key::PageUp); break;
                case VK_NEXT: Emit(Key::PageDown); break;
                case VK_HOME: Emit(Key::Home); break;
                case VK_END: Emit(Key::End); break;
                case VK_F5: Emit(Key::Refresh); break;
                case VK_F11: Emit(Key::Fullscreen); break;
                case VK_BROWSER_BACK: Emit(Key::Back); break;
                default:
                    if (ctrl) {
                        if (wp == 'F') Emit(Key::Search);
                        else if (wp == 'N' || wp == 'O') Emit(Key::AddGame);
                        else if (wp == 'V') Emit(Key::Paste);
                        else if (wp == 'A') Emit(Key::SelectAll);
                        else if (wp == 'R') Emit(Key::Refresh);
                        else if (wp == VK_OEM_COMMA) Emit(Key::Settings);
                        else if (wp >= '1' && wp <= '5') Emit(static_cast<Key>(static_cast<int>(Key::Tab1) + (wp - '1')));
                    } else if (!textMode) {
                        if (wp == 'I') Emit(Key::Info);
                        else if (wp == 'F') Emit(Key::Favorite);
                        else if (wp == 'R') Emit(Key::Random);
                        else if (wp == 'M' || wp == VK_APPS) Emit(Key::Menu);
                        else if (wp == VK_OEM_2 /* '/' */) Emit(Key::Search);
                    }
                    if (wp == VK_APPS && textMode) Emit(Key::Menu);
                    break;
            }
            return true;
        }
    }
    return false;
}

void Input::PollGamepad(float dt) {
    // Polling empty XInput slots is slow, so look for a controller only every 2 seconds.
    XINPUT_STATE st{};
    bool found = padIndex_ >= 0 && XInputGetState(static_cast<DWORD>(padIndex_), &st) == ERROR_SUCCESS;
    if (!found) {
        padIndex_ = -1;
        padRescan_ -= dt;
        if (padRescan_ <= 0) {
            padRescan_ = 2.f;
            for (DWORD i = 0; i < XUSER_MAX_COUNT && !found; ++i) {
                found = XInputGetState(i, &st) == ERROR_SUCCESS;
                if (found) padIndex_ = static_cast<int>(i);
            }
        }
    }
    frame_.gamepad = found;
    if (!found) {
        padPrev_ = 0;
        heldDir_ = -1;
        return;
    }
    WORD b = st.Gamepad.wButtons;
    WORD pressed = b & ~padPrev_;
    padPrev_ = b;
    if (pressed & XINPUT_GAMEPAD_A) Emit(Key::Accept);
    if (pressed & XINPUT_GAMEPAD_B) Emit(Key::Back);
    if (pressed & XINPUT_GAMEPAD_Y) Emit(Key::Info);
    if (pressed & XINPUT_GAMEPAD_X) Emit(Key::Favorite);
    if (pressed & XINPUT_GAMEPAD_START) Emit(Key::Menu);
    if (pressed & XINPUT_GAMEPAD_BACK) Emit(Key::NextTab);
    if (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) Emit(Key::PageUp);
    if (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) Emit(Key::PageDown);

    // Directions from D-pad or left stick, with key-repeat.
    const float dead = 0.55f;
    float lx = st.Gamepad.sThumbLX / 32767.f, ly = st.Gamepad.sThumbLY / 32767.f;
    int dir = -1;
    if ((b & XINPUT_GAMEPAD_DPAD_LEFT) || lx < -dead) dir = 0;
    else if ((b & XINPUT_GAMEPAD_DPAD_RIGHT) || lx > dead) dir = 1;
    else if ((b & XINPUT_GAMEPAD_DPAD_UP) || ly > dead) dir = 2;
    else if ((b & XINPUT_GAMEPAD_DPAD_DOWN) || ly < -dead) dir = 3;
    static const Key dirs[] = {Key::Left, Key::Right, Key::Up, Key::Down};
    if (dir != heldDir_) {
        heldDir_ = dir;
        if (dir >= 0) Emit(dirs[dir]);
        repeatTimer_ = 0.38f;
    } else if (dir >= 0) {
        repeatTimer_ -= dt;
        if (repeatTimer_ <= 0) {
            Emit(dirs[dir]);
            repeatTimer_ = 0.085f;
        }
    }
    // Right stick scrolls.
    float ry = st.Gamepad.sThumbRY / 32767.f;
    if (std::fabs(ry) > 0.25f) {
        wheelAcc_ += ry * dt * 9.f;
        frame_.wheel += wheelAcc_;
        wheelAcc_ = 0;
    }
}

void Input::EndFrame() {
    frame_.keys.clear();
    frame_.text.clear();
    frame_.pressed = frame_.released = frame_.rightReleased = frame_.doubleClick = false;
    frame_.wheel = 0;
}

}  // namespace lumen::ui
