#include "recomp/touch.h"

#include "port/settings.h"
#include "recomp/menu.h"
#include "recomp/hle.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>

namespace recomp::touch {
namespace {

enum Ctl { kLStick, kCStick, kA, kB, kX, kY, kZ, kL, kR, kStart, kUp, kDown, kLeft, kRight, kMenu, kCount, kNone = -1 };

struct Spot {
    float x = 0, y = 0, r = 0;
};

struct Stick {
    float ox = 0, oy = 0, cx = 0, cy = 0;
    bool held = false;
};

constexpr const char* kLabels[kCount] = {"", "C", "A", "B", "X", "Y", "Z", "L", "R", "START", "", "", "", "", "MENU"};
constexpr int kButtons[kCount] = {-1,
                                  -1,
                                  SDL_GAMEPAD_BUTTON_SOUTH,
                                  SDL_GAMEPAD_BUTTON_WEST,
                                  SDL_GAMEPAD_BUTTON_EAST,
                                  SDL_GAMEPAD_BUTTON_NORTH,
                                  SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
                                  -1,
                                  -1,
                                  SDL_GAMEPAD_BUTTON_START,
                                  SDL_GAMEPAD_BUTTON_DPAD_UP,
                                  SDL_GAMEPAD_BUTTON_DPAD_DOWN,
                                  SDL_GAMEPAD_BUTTON_DPAD_LEFT,
                                  SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
                                  -1};

bool sAvailable = false;
bool sOtherPad = false;
SDL_JoystickID sId = 0;
SDL_Joystick* sJoy = nullptr;
Spot sSpots[kCount];
Stick sSticks[2];
int sHeld[kCount] = {};
std::map<SDL_FingerID, int> sFingers;
float sW = 0, sH = 0;
int sPollFrames = 0;

bool enabled() { return sAvailable && port::settings().ui.touchControls && !sOtherPad; }

void layout() {
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    sW = ds.x;
    sH = ds.y;
    const float size = float(std::clamp(port::settings().ui.touchSize, 50, 200)) / 100.0f;
    const float u = std::min(sH, sW * 0.6f) * size;
    const float ax = sW - 0.22f * u, ay = sH - 0.27f * u;
    sSpots[kA] = {ax, ay, 0.085f * u};
    sSpots[kB] = {ax - 0.15f * u, ay + 0.09f * u, 0.055f * u};
    sSpots[kX] = {ax + 0.13f * u, ay - 0.05f * u, 0.055f * u};
    sSpots[kY] = {ax - 0.03f * u, ay - 0.15f * u, 0.055f * u};
    sSpots[kCStick] = {sW - 0.52f * u, sH - 0.19f * u, 0.09f * u};
    sSpots[kL] = {0.11f * u, 0.10f * u, 0.06f * u};
    sSpots[kR] = {sW - 0.11f * u, 0.10f * u, 0.06f * u};
    sSpots[kZ] = {sW - 0.11f * u, 0.25f * u, 0.05f * u};
    sSpots[kStart] = {sW * 0.5f + 0.10f * u, sH - 0.07f * u, 0.045f * u};
    sSpots[kMenu] = {sW * 0.5f - 0.10f * u, sH - 0.07f * u, 0.045f * u};
    const float dx = 0.13f * u, dy = 0.38f * u, arm = 0.065f * u, ar = 0.04f * u;
    sSpots[kUp] = {dx, dy - arm, ar};
    sSpots[kDown] = {dx, dy + arm, ar};
    sSpots[kLeft] = {dx - arm, dy, ar};
    sSpots[kRight] = {dx + arm, dy, ar};
    sSpots[kLStick] = {0.22f * u, sH - 0.24f * u, 0.12f * u};
    if (!sSticks[0].held) sSticks[0] = {sSpots[kLStick].x, sSpots[kLStick].y, sSpots[kLStick].x, sSpots[kLStick].y, false};
    if (!sSticks[1].held) sSticks[1] = {sSpots[kCStick].x, sSpots[kCStick].y, sSpots[kCStick].x, sSpots[kCStick].y, false};
}

void attach() {
    if (sId) return;
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = 0x045E;
    desc.product_id = 0x028E;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    desc.name = "Touch controls";
    sId = SDL_AttachVirtualJoystick(&desc);
    sJoy = sId ? SDL_OpenJoystick(sId) : nullptr;
    if (sJoy)
        for (int t : {SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER})
            SDL_SetJoystickVirtualAxis(sJoy, t, SDL_JOYSTICK_AXIS_MIN);
    log("[touch] controls attached (%u)", unsigned(sId));
}

void detach() {
    if (!sId) return;
    if (sJoy) SDL_CloseJoystick(sJoy);
    SDL_DetachVirtualJoystick(sId);
    sJoy = nullptr;
    sId = 0;
    sFingers.clear();
    std::fill(std::begin(sHeld), std::end(sHeld), 0);
    sSticks[0].held = sSticks[1].held = false;
    log("[touch] controls detached");
}

Sint16 axisValue(float v) { return Sint16(std::clamp(v, -1.0f, 1.0f) * 32767.0f); }

void push() {
    if (!sJoy) return;
    for (int i = 0; i < kCount; i++)
        if (kButtons[i] >= 0) SDL_SetJoystickVirtualButton(sJoy, kButtons[i], sHeld[i] > 0);
    SDL_SetJoystickVirtualAxis(sJoy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, sHeld[kL] > 0 ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN);
    SDL_SetJoystickVirtualAxis(sJoy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, sHeld[kR] > 0 ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN);
    const float lr = sSpots[kLStick].r, cr = sSpots[kCStick].r;
    const Stick& l = sSticks[0];
    const Stick& c = sSticks[1];
    SDL_SetJoystickVirtualAxis(sJoy, SDL_GAMEPAD_AXIS_LEFTX, l.held ? axisValue((l.cx - l.ox) / lr) : 0);
    SDL_SetJoystickVirtualAxis(sJoy, SDL_GAMEPAD_AXIS_LEFTY, l.held ? axisValue((l.cy - l.oy) / lr) : 0);
    SDL_SetJoystickVirtualAxis(sJoy, SDL_GAMEPAD_AXIS_RIGHTX, c.held ? axisValue((c.cx - c.ox) / cr) : 0);
    SDL_SetJoystickVirtualAxis(sJoy, SDL_GAMEPAD_AXIS_RIGHTY, c.held ? axisValue((c.cy - c.oy) / cr) : 0);
}

int hit(float x, float y, bool menuOnly) {
    int best = kNone;
    float bestD = 1e30f;
    for (int i = 0; i < kCount; i++) {
        if (i == kLStick) continue;
        if (menuOnly && i != kMenu) continue;
        const float d = std::hypot(x - sSpots[i].x, y - sSpots[i].y);
        const float reach = sSpots[i].r * (i == kCStick ? 1.6f : 1.3f);
        if (d < reach && d < bestD) {
            best = i;
            bestD = d;
        }
    }
    if (best == kNone && !menuOnly && x < sW * 0.45f && y > sH * 0.45f) best = kLStick;
    return best;
}

void press(SDL_FingerID finger, float x, float y) {
    const int c = hit(x, y, menu::isOpen());
    if (c == kNone) return;
    sFingers[finger] = c;
    if (c == kMenu) {
        menu::toggle();
        return;
    }
    if (c == kLStick || c == kCStick) {
        Stick& s = sSticks[c == kLStick ? 0 : 1];
        s.held = true;
        s.ox = c == kLStick ? x : sSpots[kCStick].x;
        s.oy = c == kLStick ? y : sSpots[kCStick].y;
        s.cx = x;
        s.cy = y;
    } else {
        sHeld[c]++;
    }
    push();
}

void move(SDL_FingerID finger, float x, float y) {
    const auto it = sFingers.find(finger);
    if (it == sFingers.end()) return;
    const int c = it->second;
    if (c != kLStick && c != kCStick) return;
    Stick& s = sSticks[c == kLStick ? 0 : 1];
    s.cx = x;
    s.cy = y;
    const float r = sSpots[c].r;
    const float d = std::hypot(s.cx - s.ox, s.cy - s.oy);
    if (c == kLStick && d > r * 1.5f) {
        s.ox = s.cx - (s.cx - s.ox) * (r * 1.5f) / d;
        s.oy = s.cy - (s.cy - s.oy) * (r * 1.5f) / d;
    }
    push();
}

void release(SDL_FingerID finger) {
    const auto it = sFingers.find(finger);
    if (it == sFingers.end()) return;
    const int c = it->second;
    sFingers.erase(it);
    if (c == kLStick || c == kCStick) sSticks[c == kLStick ? 0 : 1].held = false;
    else if (c != kMenu && sHeld[c] > 0) sHeld[c]--;
    push();
}

void pollPads() {
    if (--sPollFrames > 0) return;
    sPollFrames = 30;
    int n = 0;
    bool other = false;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&n)) {
        for (int i = 0; i < n; i++)
            if (ids[i] != sId) other = true;
        SDL_free(ids);
    }
    if (other != sOtherPad) {
        sOtherPad = other;
        log("[touch] %s", other ? "a controller is connected: touch controls hidden" : "no controller: touch controls shown");
    }
    if (enabled()) attach();
    else detach();
}

ImU32 color(float a, bool held) {
    const float opacity = float(std::clamp(port::settings().ui.touchOpacity, 10, 100)) / 100.0f;
    const float v = held ? 1.0f : 0.85f;
    return ImGui::GetColorU32(ImVec4(v, v, v, a * opacity * (held ? 1.4f : 1.0f)));
}

void drawButton(ImDrawList* dl, int i) {
    const Spot& s = sSpots[i];
    const bool held = sHeld[i] > 0;
    dl->AddCircleFilled(ImVec2(s.x, s.y), s.r, color(0.25f, held), 40);
    dl->AddCircle(ImVec2(s.x, s.y), s.r, color(0.7f, held), 40, 2.0f);
    const char* label = kLabels[i];
    if (*label) {
        const float fs = std::max(12.0f, s.r * (std::strlen(label) > 1 ? 0.45f : 0.9f));
        ImFont* font = ImGui::GetFont();
        const ImVec2 ts = font->CalcTextSizeA(fs, 1e9f, 0.0f, label);
        dl->AddText(font, fs, ImVec2(s.x - ts.x * 0.5f, s.y - ts.y * 0.5f), color(1.0f, held), label);
    }
}

void drawStick(ImDrawList* dl, int which) {
    const Stick& s = sSticks[which];
    const float r = sSpots[which == 0 ? kLStick : kCStick].r;
    dl->AddCircleFilled(ImVec2(s.ox, s.oy), r, color(0.18f, s.held), 48);
    dl->AddCircle(ImVec2(s.ox, s.oy), r, color(0.6f, s.held), 48, 2.0f);
    float kx = s.cx - s.ox, ky = s.cy - s.oy;
    const float d = std::hypot(kx, ky);
    if (d > r) {
        kx *= r / d;
        ky *= r / d;
    }
    dl->AddCircleFilled(ImVec2(s.ox + kx, s.oy + ky), r * 0.45f, color(0.55f, s.held), 32);
    if (which == 1) {
        ImFont* font = ImGui::GetFont();
        const float fs = std::max(12.0f, r * 0.4f);
        const ImVec2 ts = font->CalcTextSizeA(fs, 1e9f, 0.0f, "C");
        dl->AddText(font, fs, ImVec2(s.ox + kx - ts.x * 0.5f, s.oy + ky - ts.y * 0.5f), color(1.0f, s.held), "C");
    }
}

}

void init() {
#ifdef LM_MOBILE
    sAvailable = true;
#else
    const char* v = std::getenv("LM_TOUCH");
    sAvailable = v && *v == '1';
#endif
    if (!sAvailable) return;
    sPollFrames = 0;
    pollPads();
}

bool active() { return enabled() && sId != 0; }

bool available() { return sAvailable; }

bool handleEvent(const SDL_Event& ev) {
    if (!sAvailable) return false;
    switch (ev.type) {
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED: break;
    default: return false;
    }
    if (!enabled() || sW <= 0 || sH <= 0) return false;
    const float x = ev.tfinger.x * sW, y = ev.tfinger.y * sH;
    if (ev.type == SDL_EVENT_FINGER_DOWN) press(ev.tfinger.fingerID, x, y);
    else if (ev.type == SDL_EVENT_FINGER_MOTION) move(ev.tfinger.fingerID, x, y);
    else release(ev.tfinger.fingerID);
    return sFingers.count(ev.tfinger.fingerID) > 0;
}

void draw(bool menuOpen) {
    if (!sAvailable) return;
    pollPads();
    if (!enabled()) return;
    layout();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (menuOpen) {
        drawButton(dl, kMenu);
        return;
    }
    drawStick(dl, 0);
    drawStick(dl, 1);
    for (int i = kA; i < kCount; i++) drawButton(dl, i);
}

void shutdown() { detach(); }

}
