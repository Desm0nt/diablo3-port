// [vk-enc-thread] DM_GFX_VENC: the translation thread's vkCmd* calls are recorded on a thread of their own (dm-venc), batched
// through a single-producer ring (port-common/modules/vk-enc-thread, DESIGN.md). vk_enc.c; the hooks are:
//   vk_driver.c dm_vk_resolve        every resolved slot goes through dm_venc_wrap (with DM_GFX_VENC=0 it returns the driver's pointer)
//   xbg_game.cpp xbg_submit_now      dm_venc_window_begin right after the engine lock is taken, dm_venc_window_end before it is released
// Adapted from peanuts-native 85eeb7a (igawa6), GPL-3.0-or-later: dm/gfx/vk_android/vka_enc.c.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

// vk_driver.c: the trampoline slot `slot` named `name` resolved to the driver's `real`; returns what the slot should hold (a wrapper
// of this file for the calls it handles while DM_GFX_VENC is on, else real). Called under vk_driver.c's lock.
void *dm_venc_wrap(unsigned slot, const char *name, void *real);
// The engine, with its lock held: calls of this thread between begin and end go through the ring (window). end publishes every
// call of the window before the caller releases the lock, so another thread that takes the lock finds them all handed over.
void dm_venc_window_begin(void);
void dm_venc_window_end(void);
// 1 = the encoder is in use (DM_GFX_VENC 1 / 2 and not turned off at start), 0 = every call goes to the driver directly.
int dm_venc_on(void);

#ifdef __cplusplus
}
#endif
