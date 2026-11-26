// [vk-enc-thread] PNH-A-01 DM_GFX_VENC (port-common/modules/vk-enc-thread, DESIGN.md; the encoder: android/vk_enc.c). The engine's side,
// included by xbg_game.cpp at file scope. One window per translated guest submission (xbg_submit_now): opened right after the engine lock
// is taken, closed before it is released (the object is declared after the lock_guard, so it is destroyed first). Inside the window this
// thread's vkCmd* calls are copied into the ring and made by dm-venc; window_end hands every one of them over, so a thread that takes the
// engine lock next (a game thread's fence wait that submits the frame, the idle poll) finds them all and its End / submit waits for them.
// Weak: a tree without vk_enc.c links and runs as before; with DM_GFX_VENC=0 both calls return at once.
extern "C" __attribute__((weak)) void dm_venc_window_begin(void);
extern "C" __attribute__((weak)) void dm_venc_window_end(void);
namespace {
struct VencWindow {
    VencWindow(){if(dm_venc_window_begin)dm_venc_window_begin();}
    ~VencWindow(){if(dm_venc_window_end)dm_venc_window_end();}
    VencWindow(const VencWindow&)=delete;VencWindow &operator=(const VencWindow&)=delete;
};
}
