#include "input.h"

#include <windows.h>

#include <mutex>
#include <set>

#include "monitors.h"

namespace input {
namespace {

std::mutex gMutex;
std::set<uint16_t> gKeysDown;
std::set<uint8_t> gButtonsDown;

void send(INPUT& in) { SendInput(1, &in, sizeof(INPUT)); }

bool isExtended(uint16_t vk) {
    switch (vk) {
        case VK_RMENU: case VK_RCONTROL: case VK_INSERT: case VK_DELETE: case VK_HOME:
        case VK_END: case VK_PRIOR: case VK_NEXT: case VK_LEFT: case VK_RIGHT: case VK_UP:
        case VK_DOWN: case VK_NUMLOCK: case VK_DIVIDE: case VK_SNAPSHOT: case VK_LWIN:
        case VK_RWIN: case VK_APPS:
            return true;
        default:
            return false;
    }
}

}  // namespace

void mouseAbs(const hl::MonitorInfo& m, uint16_t x, uint16_t y) {
    int px = m.x + (int)((int64_t)x * (m.width - 1) / 65535);
    int py = m.y + (int)((int64_t)y * (m.height - 1) / 65535);
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw < 2 || vh < 2) return;
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = (LONG)(((int64_t)(px - vx) * 65535 + (vw - 1) / 2) / (vw - 1));
    in.mi.dy = (LONG)(((int64_t)(py - vy) * 65535 + (vh - 1) / 2) / (vh - 1));
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    send(in);
}

void mouseRel(int dx, int dy) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    send(in);
}

void mouseButton(uint8_t button, bool down) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    switch (button) {
        case hl::MOUSE_LEFT: in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
        case hl::MOUSE_RIGHT: in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
        case hl::MOUSE_MIDDLE: in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
        case hl::MOUSE_X1:
        case hl::MOUSE_X2:
            in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
            in.mi.mouseData = button == hl::MOUSE_X1 ? XBUTTON1 : XBUTTON2;
            break;
        default: return;
    }
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (down) gButtonsDown.insert(button);
        else gButtonsDown.erase(button);
    }
    send(in);
}

void scroll(int dy, int dx) {
    if (dy) {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_WHEEL;
        in.mi.mouseData = (DWORD)dy;
        send(in);
    }
    if (dx) {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_HWHEEL;
        in.mi.mouseData = (DWORD)dx;
        send(in);
    }
}

void key(uint16_t vk, bool down) {
    if (!vk || vk > 0xFE) return;
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP) | (isExtended(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (down) gKeysDown.insert(vk);
        else gKeysDown.erase(vk);
    }
    send(in);
}

void text(const std::string& utf8) {
    std::wstring w = fromUtf8(utf8);
    for (wchar_t c : w) {
        if (c == L'\n' || c == L'\r') {
            key(VK_RETURN, true);
            key(VK_RETURN, false);
            continue;
        }
        INPUT in[2] = {};
        in[0].type = in[1].type = INPUT_KEYBOARD;
        in[0].ki.wScan = in[1].ki.wScan = c;
        in[0].ki.dwFlags = KEYEVENTF_UNICODE;
        in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
    }
}

void releaseAll() {
    std::set<uint16_t> keys;
    std::set<uint8_t> buttons;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        keys.swap(gKeysDown);
        buttons.swap(gButtonsDown);
    }
    for (auto k : keys) key(k, false);
    for (auto b : buttons) mouseButton(b, false);
}

}  // namespace input
