/**
 * @file        input/mnk/mnk_input_driver.cpp
 * @brief       Keyboard/mouse input driver implementation.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/input/mnk/mnk_input_driver.h>

#include <rex/cvar.h>
#include <rex/platform.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string_view>

REXCVAR_DEFINE_BOOL(mnk_mode, false, "Input", "Enable keyboard/mouse controller emulation");
REXCVAR_DEFINE_BOOL(mnk_mouse, false, "Input",
                    "Use the mouse for the right stick. Off means the right stick comes "
                    "from the keybind_rstick_* keys only");
REXCVAR_DEFINE_BOOL(mnk_exact_modifiers, true, "Input",
                    "A bind without modifiers fires only while no modifier is held (so the D-pad can share the "
                    "arrow keys as Shift+Up). Off: plain binds ignore held modifiers, so a bare Control or Shift "
                    "bind (sneak, run) works together with movement keys");
REXCVAR_DEFINE_DOUBLE(mnk_sensitivity, 1.0, "Input", "Mouse sensitivity for right stick")
    .range(0.01, 10.0);
REXCVAR_DEFINE_INT32(mnk_mouse_deadzone, 0, "Input",
                     "Right stick deflection a mouse movement starts from (0-32767): the game's own dead zone, so a "
                     "slow movement turns the camera instead of vanishing in it (the 360's is about 8689)")
    .range(0, 32767);
REXCVAR_DEFINE_BOOL(mnk_mouse_direct, false, "Input",
                    "The game turns the player by the mouse itself (rex::input::mnk::TakeMouseLook, a game hook): "
                    "no right stick, dead zone or stick acceleration while it plays; the right stick in its menus");
REXCVAR_DEFINE_DOUBLE(mnk_walk_deflection, 1.0, "Input",
                      "Left stick deflection of the movement keys while walking (keybind_run / keybind_always_run); "
                      "1 runs always. Oblivion runs past 0.8 of the stick")
    .range(0.1, 1.0);
REXCVAR_DEFINE_BOOL(mnk_always_run, true, "Input",
                    "Movement keys start out running (keybind_always_run toggles it, keybind_run inverts it held)");
REXCVAR_DEFINE_STRING(keybind_run, "", "Input/Keybinds/Movement", "Held: walk while running always, run otherwise");
REXCVAR_DEFINE_STRING(keybind_always_run, "", "Input/Keybinds/Movement", "Toggles running always");
REXCVAR_DEFINE_STRING(keybind_auto_move, "", "Input/Keybinds/Movement",
                      "Toggles moving forward by itself (until a forward or back key)");

REXCVAR_DEFINE_STRING(keybind_a, "Semicolon,Space", "Input/Keybinds/Controller", "A button");
REXCVAR_DEFINE_STRING(keybind_b, "Quote,Backspace", "Input/Keybinds/Controller", "B button");
REXCVAR_DEFINE_STRING(keybind_x, "L", "Input/Keybinds/Controller", "X button");
REXCVAR_DEFINE_STRING(keybind_y, "P", "Input/Keybinds/Controller", "Y button");
REXCVAR_DEFINE_STRING(keybind_left_trigger, "Q,I", "Input/Keybinds/Controller", "Left trigger");
REXCVAR_DEFINE_STRING(keybind_right_trigger, "E,O", "Input/Keybinds/Controller", "Right trigger");
REXCVAR_DEFINE_STRING(keybind_left_shoulder, "1", "Input/Keybinds/Controller", "Left shoulder");
REXCVAR_DEFINE_STRING(keybind_right_shoulder, "3", "Input/Keybinds/Controller", "Right shoulder");
REXCVAR_DEFINE_STRING(keybind_lstick_up, "W", "Input/Keybinds/Controller", "Left stick up");
REXCVAR_DEFINE_STRING(keybind_lstick_down, "S", "Input/Keybinds/Controller", "Left stick down");
REXCVAR_DEFINE_STRING(keybind_lstick_left, "A", "Input/Keybinds/Controller", "Left stick left");
REXCVAR_DEFINE_STRING(keybind_lstick_right, "D", "Input/Keybinds/Controller", "Left stick right");
REXCVAR_DEFINE_STRING(keybind_lstick_press, "F", "Input/Keybinds/Controller", "Left stick press");
REXCVAR_DEFINE_STRING(keybind_rstick_up, "Up", "Input/Keybinds/Controller", "Right stick up");
REXCVAR_DEFINE_STRING(keybind_rstick_down, "Down", "Input/Keybinds/Controller", "Right stick down");
REXCVAR_DEFINE_STRING(keybind_rstick_left, "Left", "Input/Keybinds/Controller", "Right stick left");
REXCVAR_DEFINE_STRING(keybind_rstick_right, "Right", "Input/Keybinds/Controller",
                      "Right stick right");
REXCVAR_DEFINE_STRING(keybind_rstick_press, "K", "Input/Keybinds/Controller", "Right stick press");
REXCVAR_DEFINE_STRING(keybind_dpad_up, "Shift+Up", "Input/Keybinds/Controller", "D-pad up");
REXCVAR_DEFINE_STRING(keybind_dpad_down, "Shift+Down", "Input/Keybinds/Controller", "D-pad down");
REXCVAR_DEFINE_STRING(keybind_dpad_left, "Shift+Left", "Input/Keybinds/Controller", "D-pad left");
REXCVAR_DEFINE_STRING(keybind_dpad_right, "Shift+Right", "Input/Keybinds/Controller",
                      "D-pad right");
REXCVAR_DEFINE_STRING(keybind_back, "Z,Tab", "Input/Keybinds/Controller", "Back button");
REXCVAR_DEFINE_STRING(keybind_start, "X,Return", "Input/Keybinds/Controller", "Start button");
REXCVAR_DEFINE_STRING(keybind_guide, "", "Input/Keybinds/Controller", "Guide button");

namespace rex::input::mnk {

namespace {

// A single device, so its handle is a constant.
constexpr rex::input::DeviceId kMnkDevice = static_cast<rex::input::DeviceId>(0x4D4E4B00);

// Bind values are a comma-separated list of alternatives, each optionally
// carrying modifier prefixes: "Q,I" or "Shift+W". Modifier matching is exact,
// so "Up" stays silent while Shift is held and "Shift+Up" stays silent without
// it. That is what lets the D-pad share the arrow keys. A consequence is
// that binding a bare modifier name ("Shift") can never fire, since holding it
// makes the live mask non-zero while the bind wants zero.
constexpr uint8_t kModShift = 1u << 0;
constexpr uint8_t kModCtrl = 1u << 1;
constexpr uint8_t kModAlt = 1u << 2;

uint8_t LiveModifiers(const bool (&key_down)[256]) {
  uint8_t mods = 0;
  if (key_down[static_cast<uint16_t>(rex::ui::VirtualKey::kShift)])
    mods |= kModShift;
  if (key_down[static_cast<uint16_t>(rex::ui::VirtualKey::kControl)])
    mods |= kModCtrl;
  if (key_down[static_cast<uint16_t>(rex::ui::VirtualKey::kMenu)])
    mods |= kModAlt;
  return mods;
}

// Strips leading modifier prefixes off 'token', advancing it to the bare key
// name and returning the mask they require.
uint8_t TakeModifiers(std::string_view& token) {
  uint8_t mods = 0;
  for (;;) {
    size_t plus = token.find('+');
    if (plus == std::string_view::npos || plus == 0) {
      break;
    }
    std::string_view head = token.substr(0, plus);
    uint8_t bit = 0;
    if (head == "Shift") {
      bit = kModShift;
    } else if (head == "Ctrl" || head == "Control") {
      bit = kModCtrl;
    } else if (head == "Alt") {
      bit = kModAlt;
    } else {
      break;
    }
    mods |= bit;
    token.remove_prefix(plus + 1);
  }
  return mods;
}

std::string_view TrimSpaces(std::string_view s) {
  while (!s.empty() && s.front() == ' ') {
    s.remove_prefix(1);
  }
  while (!s.empty() && s.back() == ' ') {
    s.remove_suffix(1);
  }
  return s;
}

// reOblivion (2026-11-19): a "Game:" or "Menu:" prefix keeps a key to one context, in play or in the menus
// (MnkInputDriver::InGame); the rest apply in both. One key can so do one thing in play and another in a menu.
bool TokenPressed(const bool (&key_down)[256], std::string_view token, uint8_t live_mods, bool in_game) {
  if (token.substr(0, 5) == "Game:" || token.substr(0, 5) == "Menu:") {
    if ((token[0] == 'G') != in_game) {
      return false;
    }
    token.remove_prefix(5);
  }
  uint8_t want = TakeModifiers(token);
  if (want != live_mods && (want != 0 || REXCVAR_GET(mnk_exact_modifiers))) {
    return false;
  }
  rex::ui::VirtualKey vk = rex::ui::ParseVirtualKey(token);
  if (vk == rex::ui::VirtualKey::kNone) {
    return false;
  }
  uint16_t idx = static_cast<uint16_t>(vk);
  return idx < 256 && key_down[idx];
}

std::atomic<bool> mouse_look_active{true};

// The driver TakeMouseLook and the page (web) reach.
std::atomic<MnkInputDriver*> g_driver{nullptr};

// How long after the game last took the mouse's movement it still counts as in play: its look update runs every frame,
// so a few frames' worth.
constexpr int64_t kInGameNs = 250'000'000;

int64_t SteadyNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

void SetMouseLookActive(bool active) {
  mouse_look_active.store(active, std::memory_order_relaxed);
}

bool IsMouseLookActive() {
  return mouse_look_active.load(std::memory_order_relaxed);
}

using rex::ui::VirtualKey;

bool TakeMouseLook(int32_t* dx, int32_t* dy) {
  *dx = 0;
  *dy = 0;
  MnkInputDriver* driver = g_driver.load(std::memory_order_acquire);
  return driver && driver->TakeLook(dx, dy);
}

bool MnkInputDriver::TakeLook(int32_t* dx, int32_t* dy) {
  if (!IsEnabled() || !REXCVAR_GET(mnk_mouse_direct)) {
    return false;
  }
  look_taken_ns_.store(SteadyNs(), std::memory_order_relaxed);
  std::lock_guard lock(state_mutex_);
  if (!REXCVAR_GET(mnk_mouse) || !IsMouseLookActive() || !has_focus_) {
    look_dx_ = 0.0f;
    look_dy_ = 0.0f;
    return false;
  }
  *dx = static_cast<int32_t>(look_dx_);  // toward zero: the fraction stays for the next frame
  *dy = static_cast<int32_t>(look_dy_);
  look_dx_ -= float(*dx);
  look_dy_ -= float(*dy);
  return true;
}

void MnkInputDriver::AddMouseMove(float dx, float dy) {
  const float sensitivity = float(REXCVAR_GET(mnk_sensitivity));
  std::lock_guard lock(state_mutex_);
  mouse_dx_ += dx;
  mouse_dy_ += dy;
  look_dx_ += dx * sensitivity;
  look_dy_ += dy * sensitivity;
}

bool MnkInputDriver::InGame() const {
  if (!REXCVAR_GET(mnk_mouse_direct)) {
    return true;  // no game hook: every bind applies as in play
  }
  const int64_t taken = look_taken_ns_.load(std::memory_order_relaxed);
  return taken != 0 && SteadyNs() - taken < kInGameNs;
}

#if REX_PLATFORM_WEB
// reOblivion web: the page's keyboard and mouse go to the driver (web/shell.html calls rex_web_key / rex_web_mouse on
// the page's thread). SDL's own keyboard events are proxied to the UI thread's worker and did not reach the game, and a
// pointer lock can only be asked for by the page in a click's handler; so the page reads them itself.
void MnkInputDriver::WebKey(int vk, bool down) {
  MnkInputDriver* driver = g_driver.load(std::memory_order_acquire);
  if (!driver)
    return;
  std::lock_guard lock(driver->state_mutex_);
  if (vk < 0) {  // the page lost the focus: every key up
    std::memset(driver->key_down_, 0, sizeof(driver->key_down_));
    driver->mouse_dx_ = 0.0f;
    driver->mouse_dy_ = 0.0f;
    driver->look_dx_ = 0.0f;
    driver->look_dy_ = 0.0f;
    driver->auto_move_ = false;
    std::memset(driver->web_stick_, 0, sizeof(driver->web_stick_));
    return;
  }
  if (down)
    driver->has_focus_ = true;
  driver->SetKeyState(static_cast<uint16_t>(vk), down);
}

void MnkInputDriver::WebMouse(float dx, float dy) {
  MnkInputDriver* driver = g_driver.load(std::memory_order_acquire);
  if (!driver)
    return;
  driver->AddMouseMove(dx, dy);
}

void MnkInputDriver::WebStick(int stick, float x, float y) {
  MnkInputDriver* driver = g_driver.load(std::memory_order_acquire);
  if (!driver || stick < 0 || stick > 1)
    return;
  std::lock_guard lock(driver->state_mutex_);
  driver->web_stick_[stick][0] = std::clamp(x, -1.0f, 1.0f);
  driver->web_stick_[stick][1] = std::clamp(y, -1.0f, 1.0f);
  if (x != 0.0f || y != 0.0f)
    driver->has_focus_ = true;
}
#endif

MnkInputDriver::MnkInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {
  always_run_ = REXCVAR_GET(mnk_always_run);
  g_driver.store(this, std::memory_order_release);
}

MnkInputDriver::~MnkInputDriver() {
  MnkInputDriver* self = this;
  g_driver.compare_exchange_strong(self, nullptr);
  // Detach handled by OnClosing; if window outlives the driver, clean up here.
  DetachFromWindow();
}

X_STATUS MnkInputDriver::Setup() {
  return X_STATUS_SUCCESS;
}

void MnkInputDriver::OnWindowAvailable(rex::ui::Window* window) {
  if (window) {
    {
      std::lock_guard lock(state_mutex_);
      attached_window_ = window;
    }
    window->AddInputListener(this, window_z_order());
    window->AddListener(this);
  }
}

void MnkInputDriver::OnClosing(rex::ui::UIEvent&) {
  DetachFromWindow();
}

void MnkInputDriver::DetachFromWindow() {
  rex::ui::Window* window = attached_window_;
  if (!window) {
    return;
  }
  window->app_context().CallInUIThreadSynchronous([this, window] {
    // Detach first so nothing new is queued, then run out what already was.
    {
      std::lock_guard lock(state_mutex_);
      attached_window_ = nullptr;
    }
    if (mouse_capture_update_queued_.load(std::memory_order_relaxed)) {
      window->app_context().ExecutePendingFunctionsFromUIThread();
    }
    ReleaseMouseCaptureFromUIThread(window);
    window->RemoveInputListener(this);
    window->RemoveListener(this);
  });
}

bool MnkInputDriver::IsEnabled() const {
  return REXCVAR_GET(mnk_mode);
}

static bool IsBindPressed(const bool (&key_down)[256], const std::string& cvar_val, bool in_game) {
  const uint8_t live_mods = LiveModifiers(key_down);
  std::string_view rest(cvar_val);
  while (!rest.empty()) {
    size_t comma = rest.find(',');
    std::string_view token = rest.substr(0, comma);
    if (comma == std::string_view::npos) {
      rest = std::string_view();
    } else {
      rest.remove_prefix(comma + 1);
    }
    token = TrimSpaces(token);
    if (!token.empty() && TokenPressed(key_down, token, live_mods, in_game)) {
      return true;
    }
  }
  return false;
}

void MnkInputDriver::EnumerateDevices(std::vector<DeviceInfo>& out) {
  // Disabled means no device at all, so it never occupies a guest user slot.
  if (!IsEnabled()) {
    return;
  }
  DeviceInfo info;
  info.id = kMnkDevice;
  info.name = "Keyboard and Mouse";
  info.synthetic = true;
  out.push_back(info);
}

X_RESULT MnkInputDriver::GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                               X_INPUT_CAPABILITIES* out_caps) {
  if (!IsEnabled() || id != kMnkDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;
    out_caps->sub_type = 0x01;
    out_caps->flags = 0;
    out_caps->gamepad.buttons = 0xFFFF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    out_caps->vibration.left_motor_speed = 0xFFFF;
    out_caps->vibration.right_motor_speed = 0xFFFF;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  if (!IsEnabled() || id != kMnkDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Mass Effect test harness (host only): MASSEFFECT_AUTO_INPUT="T:BTN[:SECONDS];..." scripts the pad
  // from the first poll, ignoring window focus, so unattended runs can get past menus and move.
  // BTN: A B X Y START BACK LB RB LT RT UP DOWN LEFT RIGHT LX+ LX- LY+ LY- RX+ RX- RY+ RY-.
  if (const char* script = std::getenv("MASSEFFECT_AUTO_INPUT"); script && *script) {
    static const auto start = std::chrono::steady_clock::now();
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    uint16_t b = 0; uint8_t lt = 0, rt = 0; int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    std::string_view rest(script);
    while (!rest.empty()) {
      const size_t semi = rest.find(';');
      std::string_view item = rest.substr(0, semi);
      rest = semi == std::string_view::npos ? std::string_view{} : rest.substr(semi + 1);
      const size_t c1 = item.find(':');
      if (c1 == std::string_view::npos) continue;
      const size_t c2 = item.find(':', c1 + 1);
      const double at = std::atof(std::string(item.substr(0, c1)).c_str());
      const std::string btn(item.substr(c1 + 1, c2 == std::string_view::npos ? std::string_view::npos : c2 - c1 - 1));
      const double dur = c2 == std::string_view::npos ? 0.25 : std::atof(std::string(item.substr(c2 + 1)).c_str());
      if (t < at || t >= at + dur) continue;
      if (btn == "A") b |= X_INPUT_GAMEPAD_A; else if (btn == "B") b |= X_INPUT_GAMEPAD_B;
      else if (btn == "X") b |= X_INPUT_GAMEPAD_X; else if (btn == "Y") b |= X_INPUT_GAMEPAD_Y;
      else if (btn == "START") b |= X_INPUT_GAMEPAD_START; else if (btn == "BACK") b |= X_INPUT_GAMEPAD_BACK;
      else if (btn == "LB") b |= X_INPUT_GAMEPAD_LEFT_SHOULDER; else if (btn == "RB") b |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
      else if (btn == "UP") b |= X_INPUT_GAMEPAD_DPAD_UP; else if (btn == "DOWN") b |= X_INPUT_GAMEPAD_DPAD_DOWN;
      else if (btn == "LEFT") b |= X_INPUT_GAMEPAD_DPAD_LEFT; else if (btn == "RIGHT") b |= X_INPUT_GAMEPAD_DPAD_RIGHT;
      else if (btn == "LT") lt = 0xFF; else if (btn == "RT") rt = 0xFF;
      else if (btn == "LX+") lx = INT16_MAX; else if (btn == "LX-") lx = -INT16_MAX;
      else if (btn == "LY+") ly = INT16_MAX; else if (btn == "LY-") ly = -INT16_MAX;
      else if (btn == "RX+") rx = INT16_MAX; else if (btn == "RX-") rx = -INT16_MAX;
      else if (btn == "RY+") ry = INT16_MAX; else if (btn == "RY-") ry = -INT16_MAX;
    }
    if (out_state) {
      std::memset(out_state, 0, sizeof(*out_state));
      out_state->packet_number = ++packet_number_;
      out_state->gamepad.buttons = b;
      out_state->gamepad.left_trigger = lt;
      out_state->gamepad.right_trigger = rt;
      out_state->gamepad.thumb_lx = int16_t(lx);
      out_state->gamepad.thumb_ly = int16_t(ly);
      out_state->gamepad.thumb_rx = int16_t(rx);
      out_state->gamepad.thumb_ry = int16_t(ry);
    }
    return X_ERROR_SUCCESS;
  }

  // Mouse look is opt in. Without this gate, keyboard input alone would hide
  // and lock the cursor, breaking the ImGui overlays.
#if !REX_PLATFORM_WEB  // the page locks the pointer itself (web/shell.html)
  QueueMouseCaptureUpdate(IsEnabled() && REXCVAR_GET(mnk_mouse) && IsMouseLookActive() &&
                          has_focus_ && is_active());
#endif

  if (!is_active() || !has_focus_) {
    if (out_state) {
      std::memset(out_state, 0, sizeof(*out_state));
      out_state->packet_number = packet_number_;
    }
    return X_ERROR_SUCCESS;
  }

  // In play or in a menu ("Game:" / "Menu:" binds; the mouse's way into the game).
  const bool in_game = InGame();

  std::lock_guard lock(state_mutex_);

  bool sampled_keys[256];
  for (size_t i = 0; i < std::size(sampled_keys); ++i) {
    sampled_keys[i] = key_down_[i] || key_pressed_[i];
    key_pressed_[i] = false;
  }

  uint16_t buttons = 0;
  if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_a), in_game))
    buttons |= X_INPUT_GAMEPAD_A;
  if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_b), in_game))
    buttons |= X_INPUT_GAMEPAD_B;
  if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_x), in_game))
    buttons |= X_INPUT_GAMEPAD_X;
  if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_y), in_game))
    buttons |= X_INPUT_GAMEPAD_Y;
  if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_left_shoulder), in_game))
    buttons |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
  if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_right_shoulder), in_game))
    buttons |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_lstick_press), in_game))
    buttons |= X_INPUT_GAMEPAD_LEFT_THUMB;
  if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_rstick_press), in_game))
    buttons |= X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_back), in_game))
    buttons |= X_INPUT_GAMEPAD_BACK;
  if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_start), in_game))
    buttons |= X_INPUT_GAMEPAD_START;
  if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_guide), in_game))
    buttons |= X_INPUT_GAMEPAD_GUIDE;
  if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_dpad_up), in_game))
    buttons |= X_INPUT_GAMEPAD_DPAD_UP;
  if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_dpad_down), in_game))
    buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
  if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_dpad_left), in_game))
    buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
  if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_dpad_right), in_game))
    buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;

  uint8_t lt = IsBindPressed(sampled_keys, REXCVAR_GET(keybind_left_trigger), in_game) ? 0xFF : 0;
  uint8_t rt = IsBindPressed(sampled_keys, REXCVAR_GET(keybind_right_trigger), in_game) ? 0xFF : 0;

  int32_t lx = 0;
  int32_t ly = 0;
  const bool forward = IsBindPressed(sampled_keys, REXCVAR_GET(keybind_lstick_up), in_game);
  const bool back = IsBindPressed(sampled_keys, REXCVAR_GET(keybind_lstick_down), in_game);
  if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_lstick_left), in_game))
    lx -= INT16_MAX;
  if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_lstick_right), in_game))
    lx += INT16_MAX;
  if (forward)
    ly += INT16_MAX;
  if (back)
    ly -= INT16_MAX;

  // reOblivion (2026-11-19): walking and running on the keyboard, as on the PC (Run held, Always Run and Auto Move
  // toggled). The 360 walks below a part of the stick's travel and runs past it, so a walk is a shorter deflection.
  const bool always_run_down = IsBindPressed(sampled_keys, REXCVAR_GET(keybind_always_run), in_game);
  if (always_run_down && !always_run_was_down_)
    always_run_ = !always_run_;
  always_run_was_down_ = always_run_down;
  const bool auto_move_down = IsBindPressed(sampled_keys, REXCVAR_GET(keybind_auto_move), in_game);
  if (auto_move_down && !auto_move_was_down_)
    auto_move_ = !auto_move_;
  auto_move_was_down_ = auto_move_down;
  if (forward || back || !in_game)
    auto_move_ = false;  // a forward or back key ends it, so does a menu
  if (auto_move_)
    ly = INT16_MAX;
  const bool running = always_run_ != IsBindPressed(sampled_keys, REXCVAR_GET(keybind_run), in_game);
  const double walk = REXCVAR_GET(mnk_walk_deflection);
  if (in_game && !running && walk < 1.0 && (lx != 0 || ly != 0)) {
    const double length = std::sqrt(double(lx) * lx + double(ly) * ly);
    lx = int32_t(lx * walk * INT16_MAX / length);
    ly = int32_t(ly * walk * INT16_MAX / length);
  }

  int32_t rx = 0;
  int32_t ry = 0;
  if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_rstick_left), in_game))
    rx -= INT16_MAX;
  if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_rstick_right), in_game))
    rx += INT16_MAX;
  if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_rstick_up), in_game))
    ry += INT16_MAX;
  if (IsBindPressed(sampled_keys, REXCVAR_GET(keybind_rstick_down), in_game))
    ry -= INT16_MAX;

  // With mnk_mouse_direct the game takes the mouse itself while it plays (TakeMouseLook); the menus still get it here.
  if (REXCVAR_GET(mnk_mouse) && IsMouseLookActive() && !(REXCVAR_GET(mnk_mouse_direct) && in_game)) {
    double sensitivity = REXCVAR_GET(mnk_sensitivity);
    constexpr double kBaseScale = 200.0;
    // reOblivion web (2026-11-18): past the game's dead zone (mnk_mouse_deadzone) when the mouse moved at all.
    const double deadzone = double(REXCVAR_GET(mnk_mouse_deadzone));
    auto deflect = [&](float delta) -> int32_t {
      if (delta == 0.0f)
        return 0;
      const double v = std::abs(double(delta)) * sensitivity * kBaseScale + deadzone;
      return static_cast<int32_t>(delta < 0.0f ? -v : v);
    };
    rx += deflect(mouse_dx_);
    ry += deflect(-mouse_dy_);
  }
#if REX_PLATFORM_WEB
  lx += int32_t(web_stick_[0][0] * INT16_MAX);
  ly += int32_t(web_stick_[0][1] * INT16_MAX);
  rx += int32_t(web_stick_[1][0] * INT16_MAX);
  ry += int32_t(web_stick_[1][1] * INT16_MAX);
#endif
  // Drained unconditionally: deltas keep accumulating in OnMouseMove while the
  // mouse is off, and toggling it on would otherwise dump the whole backlog
  // into one frame as a camera snap.
  mouse_dx_ = 0.0f;
  mouse_dy_ = 0.0f;
  if (!in_game) {
    look_dx_ = 0.0f;  // what the menus moved is not the game's to turn by afterwards
    look_dy_ = 0.0f;
  }

  auto clamp16 = [](int32_t v) -> int16_t {
    return static_cast<int16_t>(std::clamp(v, (int32_t)INT16_MIN, (int32_t)INT16_MAX));
  };

  packet_number_++;

  if (out_state) {
    out_state->packet_number = packet_number_;
    out_state->gamepad.buttons = buttons;
    out_state->gamepad.left_trigger = lt;
    out_state->gamepad.right_trigger = rt;
    out_state->gamepad.thumb_lx = clamp16(lx);
    out_state->gamepad.thumb_ly = clamp16(ly);
    out_state->gamepad.thumb_rx = clamp16(rx);
    out_state->gamepad.thumb_ry = clamp16(ry);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) {
  if (!IsEnabled() || id != kMnkDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::GetDeviceKeystroke(DeviceId id, uint32_t flags,
                                            X_INPUT_KEYSTROKE* out_keystroke) {
  if (!IsEnabled() || id != kMnkDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  std::lock_guard lock(state_mutex_);
  if (keystroke_queue_.empty()) {
    return X_ERROR_EMPTY;
  }
  if (out_keystroke) {
    *out_keystroke = keystroke_queue_.front();
  }
  keystroke_queue_.pop();
  return X_ERROR_SUCCESS;
}

void MnkInputDriver::EnqueueKeystroke(uint16_t vk_pad, bool down) {
  X_INPUT_KEYSTROKE ks = {};
  ks.virtual_key = vk_pad;
  ks.unicode = 0;
  ks.flags = down ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP;
  // InputSystem stamps the guest user this device is assigned to.
  ks.user_index = 0;
  ks.hid_code = 0;
  keystroke_queue_.push(ks);
}

void MnkInputDriver::QueueMouseCaptureUpdate(bool should_capture) {
  mouse_capture_requested_.store(should_capture, std::memory_order_relaxed);
  bool already_queued = false;
  if (!mouse_capture_update_queued_.compare_exchange_strong(already_queued, true)) {
    return;
  }
  std::lock_guard lock(state_mutex_);
  if (!attached_window_) {
    mouse_capture_update_queued_.store(false, std::memory_order_relaxed);
    return;
  }
  // Deferred, not CallInUIThread: running inline would re-enter state_mutex_.
  attached_window_->app_context().CallInUIThreadDeferred([this] {
    mouse_capture_update_queued_.store(false, std::memory_order_relaxed);
    ApplyMouseCaptureFromUIThread();
  });
}

void MnkInputDriver::ApplyMouseCaptureFromUIThread() {
  rex::ui::Window* window = attached_window_;
  if (!window) {
    return;
  }
  bool should_capture = mouse_capture_requested_.load(std::memory_order_relaxed);
  if (should_capture == mouse_captured_) {
    return;
  }
  if (!should_capture) {
    ReleaseMouseCaptureFromUIThread(window);
    return;
  }
  mouse_captured_ = true;
  precapture_cursor_visibility_ = window->GetCursorVisibility();
  window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
  window->CaptureMouse();
  relative_mouse_mode_ = window->SetRelativeMouseMode(true);
  if (!relative_mouse_mode_) {
    REXLOG_WARN("Pointer lock unavailable, mouse look falls back to recentering the cursor");
  }
  // Reset deltas to avoid a spike on capture start
  std::lock_guard lock(state_mutex_);
  mouse_dx_ = 0.0f;
  mouse_dy_ = 0.0f;
}

void MnkInputDriver::ReleaseMouseCaptureFromUIThread(rex::ui::Window* window) {
  if (!mouse_captured_) {
    return;
  }
  mouse_captured_ = false;
  window->SetCursorVisibility(precapture_cursor_visibility_);
  window->SetRelativeMouseMode(false);
  relative_mouse_mode_ = false;
  window->ReleaseMouse();
}

void MnkInputDriver::RecenterCursorFromUIThread(int32_t x, int32_t y) {
  rex::ui::Window* window = attached_window_;
  if (!window) {
    return;
  }
  int32_t width = int32_t(window->GetActualPhysicalWidth());
  int32_t height = int32_t(window->GetActualPhysicalHeight());
  if (width <= 0 || height <= 0) {
    return;
  }
  // Only once the pointer has drifted well off the middle, to keep warps rare.
  if (std::abs(x - width / 2) < width / 4 && std::abs(y - height / 2) < height / 4) {
    return;
  }
  int32_t center_x = 0;
  int32_t center_y = 0;
  if (window->WarpMouseToCenter(center_x, center_y)) {
    prev_mouse_x_ = center_x;
    prev_mouse_y_ = center_y;
  }
}

void MnkInputDriver::SetKeyState(uint16_t vk, bool down) {
  if (vk < 256) {
    if (down && !key_down_[vk]) {
      key_pressed_[vk] = true;
    }
    key_down_[vk] = down;
  }
}

void MnkInputDriver::OnKeyDown(rex::ui::KeyEvent& e) {
  if (!IsEnabled() || !has_focus_)
    return;
  std::lock_guard lock(state_mutex_);
  uint16_t vk = static_cast<uint16_t>(e.virtual_key());
  SetKeyState(vk, true);
}

void MnkInputDriver::OnKeyUp(rex::ui::KeyEvent& e) {
  if (!IsEnabled())
    return;
  std::lock_guard lock(state_mutex_);
  uint16_t vk = static_cast<uint16_t>(e.virtual_key());
  SetKeyState(vk, false);
}

void MnkInputDriver::OnMouseDown(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !has_focus_)
    return;
  std::lock_guard lock(state_mutex_);
  switch (e.button()) {
    case rex::ui::MouseEvent::Button::kLeft:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kLButton), true);
      break;
    case rex::ui::MouseEvent::Button::kRight:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kRButton), true);
      break;
    case rex::ui::MouseEvent::Button::kMiddle:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kMButton), true);
      break;
    default:
      break;
  }
}

void MnkInputDriver::OnMouseUp(rex::ui::MouseEvent& e) {
  if (!IsEnabled())
    return;
  std::lock_guard lock(state_mutex_);
  switch (e.button()) {
    case rex::ui::MouseEvent::Button::kLeft:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kLButton), false);
      break;
    case rex::ui::MouseEvent::Button::kRight:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kRButton), false);
      break;
    case rex::ui::MouseEvent::Button::kMiddle:
      SetKeyState(static_cast<uint16_t>(VirtualKey::kMButton), false);
      break;
    default:
      break;
  }
}

void MnkInputDriver::OnMouseMove(rex::ui::MouseEvent& e) {
  if (!IsEnabled() || !has_focus_)
    return;
  int32_t x = e.x();
  int32_t y = e.y();
  {
    if (relative_mouse_mode_) {
      // The pointer is locked, so the absolute position no longer moves.
      AddMouseMove(float(e.dx()), float(e.dy()));
    } else {
      AddMouseMove(float(x - prev_mouse_x_), float(y - prev_mouse_y_));
    }
  }
  prev_mouse_x_ = x;
  prev_mouse_y_ = y;
  // Without a pointer lock the cursor still has to be kept off the edges.
  if (mouse_captured_ && !relative_mouse_mode_) {
    RecenterCursorFromUIThread(x, y);
  }
}

void MnkInputDriver::OnLostFocus(rex::ui::UISetupEvent&) {
  has_focus_ = false;
  // Withdraw the request too, or an update queued before the focus loss grabs
  // the cursor straight back.
  mouse_capture_requested_.store(false, std::memory_order_relaxed);
  {
    std::lock_guard lock(state_mutex_);
    std::memset(key_down_, 0, sizeof(key_down_));
    mouse_dx_ = 0.0f;
    mouse_dy_ = 0.0f;
    look_dx_ = 0.0f;
    look_dy_ = 0.0f;
    auto_move_ = false;
  }
  if (attached_window_) {
    ReleaseMouseCaptureFromUIThread(attached_window_);
  }
}

void MnkInputDriver::OnGotFocus(rex::ui::UISetupEvent&) {
  has_focus_ = true;
}

}  // namespace rex::input::mnk

#if REX_PLATFORM_WEB
#include <emscripten/emscripten.h>
// reOblivion web: called by the page (web/shell.html) on its own thread. vk: a Windows virtual key (rex::ui::VirtualKey;
// 1, 2, 4 the mouse buttons), -1 every key up. dx, dy: the locked pointer's movement in CSS pixels.
extern "C" EMSCRIPTEN_KEEPALIVE void rex_web_key(int vk, int down) {
  rex::input::mnk::MnkInputDriver::WebKey(vk, down != 0);
}
extern "C" EMSCRIPTEN_KEEPALIVE void rex_web_mouse(double dx, double dy) {
  rex::input::mnk::MnkInputDriver::WebMouse(static_cast<float>(dx), static_cast<float>(dy));
}
// stick: 0 the left, 1 the right; x, y in -1..1 with y up (the page's touch sticks).
extern "C" EMSCRIPTEN_KEEPALIVE void rex_web_stick(int stick, double x, double y) {
  rex::input::mnk::MnkInputDriver::WebStick(stick, static_cast<float>(x), static_cast<float>(y));
}
#endif
