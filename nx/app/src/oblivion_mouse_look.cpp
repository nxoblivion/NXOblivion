// reOblivion (2026-11-19): the mouse looks around as on the PC.
//
// The 360 build lost the PC's DirectInput mouse (its OSInputGlobals reads only the pads), but its player look update,
// sub_82417860 (PlayerCharacter slot 0x8A), still turns the player the PC's way: it makes the right stick into mouse
// counts (past the stick's dead zone, through fXenonHorizLookSpeed / fXenonVertLookSpeed and the iXenon*LookAccel
// curve), r20 across and r19 down, then turns by counts * fMouseSensitivity (0.002 radians a count) and pitches by the
// same, with bInvertYValues flipping r19 after this point. The manifest's [[entrypoint.midasm_hook]] at 0x82418024 calls
// this where both paths (stick moved, stick still) meet, before the check that zeroes them while the controls are off.
//
// The counts added are the mouse's own (rex::input::mnk::TakeMouseLook, mnk_mouse_direct): no dead zone, no stick
// acceleration and no speed limit, the turn exactly as far as the mouse went. A pad's stick still adds its own.

#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/ppc/context.h>

#include <cstdint>

void OblivionMouseLookHook(PPCRegister& r20, PPCRegister& r19) {
  int32_t dx = 0;
  int32_t dy = 0;
  if (!rex::input::mnk::TakeMouseLook(&dx, &dy) || (dx == 0 && dy == 0)) {
    return;
  }
  // The code after reads the low words (extsw).
  r20.u64 = uint64_t(int64_t(int32_t(r20.u32) + dx));
  r19.u64 = uint64_t(int64_t(int32_t(r19.u32) + dy));
}
