/**
 * @file        rex/input/mnk/mnk_input_driver.h
 * @brief       Keyboard/mouse input driver - maps MnK to Xbox 360 controller.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/input/input_driver.h>
#include <rex/platform.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>

namespace rex::input::mnk {

void SetMouseLookActive(bool active);
bool IsMouseLookActive();

// reOblivion (2026-11-19): the game's own mouse look, with mnk_mouse_direct. The game calls this where it turns the
// player (once a frame, on its thread) and gets the mouse's movement since the last call in whole counts (pixels times
// mnk_sensitivity; the fraction carries over), x right, y down. False (and zeros) without a driver, with mnk_mouse_direct
// off or mouse look paused. Each call also marks the game as in play for a moment: while it is, the mouse leaves the
// right stick to the game and the binds prefixed "Game:" apply; otherwise (the menus) those prefixed "Menu:" do.
bool TakeMouseLook(int32_t* dx, int32_t* dy);

class MnkInputDriver final : public InputDriver,
                             public rex::ui::WindowInputListener,
                             public rex::ui::WindowListener {
 public:
  explicit MnkInputDriver(rex::ui::Window* window, size_t window_z_order);
  ~MnkInputDriver() override;

  X_STATUS Setup() override;

  void EnumerateDevices(std::vector<DeviceInfo>& out) override;
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override;
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                 X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags,
                              X_INPUT_KEYSTROKE* out_keystroke) override;

  void OnWindowAvailable(rex::ui::Window* window) override;

  // WindowInputListener
  void OnKeyDown(rex::ui::KeyEvent& e) override;
  void OnKeyUp(rex::ui::KeyEvent& e) override;
  void OnMouseDown(rex::ui::MouseEvent& e) override;
  void OnMouseUp(rex::ui::MouseEvent& e) override;
  void OnMouseMove(rex::ui::MouseEvent& e) override;

  // WindowListener
  void OnClosing(rex::ui::UIEvent& e) override;
  void OnLostFocus(rex::ui::UISetupEvent& e) override;
  void OnGotFocus(rex::ui::UISetupEvent& e) override;

#if REX_PLATFORM_WEB
  // reOblivion web: the page's keyboard and mouse (rex_web_key / rex_web_mouse), from the page's thread.
  static void WebKey(int vk, bool down);
  static void WebMouse(float dx, float dy);
  // reOblivion web (2026-11-19): a touch screen's on-page stick (web/shell.html): stick 0 the left, 1 the right; x, y in
  // -1..1, y up. Held until the page sets it again (0, 0 when let go).
  static void WebStick(int stick, float x, float y);
#endif

  // TakeMouseLook's work, on the driver.
  bool TakeLook(int32_t* dx, int32_t* dy);

 private:
  bool IsEnabled() const;
  // Whether the game took the mouse's movement lately (TakeMouseLook): it is in play, not in a menu.
  bool InGame() const;
  // The mouse moved (pixels): for the right stick and for the game's own look.
  void AddMouseMove(float dx, float dy);
  void SetKeyState(uint16_t vk, bool down);
  void EnqueueKeystroke(uint16_t vk_pad, bool down);

  // Called from the guest thread. The rest of the capture path stays on the UI
  // thread, since every Window call in it reaches SDL.
  void QueueMouseCaptureUpdate(bool should_capture);
  void ApplyMouseCaptureFromUIThread();
  void ReleaseMouseCaptureFromUIThread(rex::ui::Window* window);
  void RecenterCursorFromUIThread(int32_t x, int32_t y);
  // Safe to call from any thread.
  void DetachFromWindow();

  // Only the UI thread writes it, so only guest thread access needs the lock.
  rex::ui::Window* attached_window_ = nullptr;

  std::mutex state_mutex_;
  bool key_down_[256] = {};
  // Preserve a press until the guest has sampled it at least once. UI key
  // down/up pairs may both arrive between two XInputGetState calls.
  bool key_pressed_[256] = {};

  // Mouse delta tracking. Fractional because relative motion arrives in
  // fractions of a pixel, and truncating each event drops slow movement.
  // Filled on the UI thread, drained on the guest thread, hence the lock.
  float mouse_dx_ = 0.0f;
  float mouse_dy_ = 0.0f;
  // The same movement for the game's own mouse look (TakeMouseLook), in counts, and when it last took it
  // (steady_clock nanoseconds, 0 never).
  float look_dx_ = 0.0f;
  float look_dy_ = 0.0f;
  std::atomic<int64_t> look_taken_ns_{0};
  // Keyboard walking and running (keybind_run / keybind_always_run / keybind_auto_move), on the guest thread under
  // state_mutex_: the toggles and the binds' state at the last sample (for their presses).
  bool always_run_ = true;
  bool auto_move_ = false;
  bool always_run_was_down_ = false;
  bool auto_move_was_down_ = false;
#if REX_PLATFORM_WEB
  // The page's touch sticks (WebStick), under state_mutex_.
  float web_stick_[2][2] = {};
#endif

  // UI thread only.
  int32_t prev_mouse_x_ = 0;
  int32_t prev_mouse_y_ = 0;
  bool mouse_captured_ = false;
  // Whether the window has the pointer locked and is reporting relative motion.
  bool relative_mouse_mode_ = false;
  // Cursor visibility to restore on capture release - the window owner may run
  // an auto-hide policy that capture must not permanently override.
  rex::ui::Window::CursorVisibility precapture_cursor_visibility_ =
      rex::ui::Window::CursorVisibility::kVisible;

  // Guest thread to UI thread. The queued flag coalesces the posts.
  std::atomic<bool> mouse_capture_requested_{false};
  std::atomic<bool> mouse_capture_update_queued_{false};

  std::atomic<bool> has_focus_{true};

  // Keystroke queue
  std::queue<X_INPUT_KEYSTROKE> keystroke_queue_;

  // Packet number incremented on state change
  uint32_t packet_number_ = 0;
};

}  // namespace rex::input::mnk
