// [present-thread] PERF-D07 DM_GFX_PRESENT_THREAD (DOOM 2016 xbg_game.cpp pt_mode / pt_before_submit / pt_drain; port-common/modules/
// present-thread). The engine's side of the Android presenter's present thread (android/vulkan_present.cpp, android/present_thread.h).
// Included by xbg_game.cpp at file scope, before the first queue call.
//   DM_GFX_PRESENT_THREAD  0  (default) present inline as before: every helper below is one compare and the old call
//                          1  the present (fence, acquire, blit, queue present) runs on the presenter's thread dm-present
//                          2  the same protocol run inline on this thread (queue lock, hand-off call): the cost of the protocol alone
// Rules (as DOOM 2016): (1) anything this engine submits goes after every present handed to the present thread has its blit submitted
// (before_submit: the blit reads the frame's image and the next slots transition and write it again - the same queue order as presenting
// inline); (2) every queue call takes the presenter's queue lock (one VkQueue, two threads); (3) a direct queue drain (finish, shutdown)
// drains the present thread first; (4) a failed present on the thread stops the game here, on the translation thread, as an inline
// failure did. Weak: a presenter without a present thread (other platforms) leaves the mode at 0.
// Line "NVN PERF present_thread ..." next to every NVN PERF line while the mode is on.
extern "C" __attribute__((weak)) void dm_present_thread_set(int mode);
extern "C" __attribute__((weak)) int dm_present_async(VkImage source,VkFormat format,uint32_t width,uint32_t height,const int32_t crop[4],int32_t interval);
extern "C" __attribute__((weak)) uint64_t dm_present_wait_submitted(void);
extern "C" __attribute__((weak)) uint64_t dm_present_drain(void);
extern "C" __attribute__((weak)) int dm_present_last_error(void);
extern "C" __attribute__((weak)) void dm_present_queue_lock(void);
extern "C" __attribute__((weak)) void dm_present_queue_unlock(void);
extern "C" __attribute__((weak)) void dm_present_thread_stats(uint64_t out[4]);
namespace {
namespace ptq {
int mode=0;uint64_t wait_ns=0,waits=0,drain_ns=0,drains=0,handed=0;
void init(){
    const char *e=getenv("DM_GFX_PRESENT_THREAD");int m=e && *e?atoi(e):0;if(m<0 || m>2)m=0;
    const bool have=dm_present_thread_set && dm_present_async && dm_present_wait_submitted && dm_present_drain && dm_present_last_error && dm_present_queue_lock && dm_present_queue_unlock;
    if(m && !have){xbg_logf("NVN DM_GFX_PRESENT_THREAD=%d ignored: this presenter has no present thread",m);m=0;}
    mode=m;if(have)dm_present_thread_set(m);
    if(m)xbg_logf("NVN DM_GFX_PRESENT_THREAD=%d: %s",m,m==1?"fence / acquire / blit / queue present on the presenter's thread dm-present; every submission waits for the handed blits":"the present thread's protocol (queue lock, hand-off) run inline");
}
void check(){if(int e=dm_present_last_error())xbg_fatalf("Game presentation failed: %d (present thread)",e);}
void before_submit(){if(!mode)return;const uint64_t w=dm_present_wait_submitted();if(w){wait_ns+=w;++waits;}check();}
void drain(){if(!mode)return;const uint64_t w=dm_present_drain();if(w){drain_ns+=w;++drains;}check();}
VkResult submit(VkQueue q,uint32_t n,const VkSubmitInfo *si,VkFence f){if(!mode)return vkQueueSubmit(q,n,si,f);dm_present_queue_lock();const VkResult r=vkQueueSubmit(q,n,si,f);dm_present_queue_unlock();return r;}
VkResult wait_idle(VkQueue q){if(!mode)return vkQueueWaitIdle(q);dm_present_queue_lock();const VkResult r=vkQueueWaitIdle(q);dm_present_queue_unlock();return r;}
int handoff(VkImage source,VkFormat format,uint32_t width,uint32_t height,const int32_t crop[4],int32_t interval){++handed;return dm_present_async(source,format,width,height,crop,interval);}
// Per frame over the NVN PERF window (f frames): presents handed, the present thread's time in a present, the translation thread's waits
// for a free hand-off slot / for the handed blits before a submission (count a frame) / for the thread to finish (finish, shutdown).
void perf(double f){
    if(!mode || f<=0)return;
    uint64_t s[4]={};if(dm_present_thread_stats)dm_present_thread_stats(s);
    static uint64_t p[4]={};
    const double jobs=double(s[0]-p[0]);
    xbg_logf("NVN PERF present_thread mode=%d handed=%.2f jobs=%.2f busy_ms=%.3f full_ms=%.3f wait_ms=%.3f waits=%.2f drain_ms=%.3f drains=%llu",mode,double(handed)/f,jobs/f,
        jobs>0?double(s[1]-p[1])/1e6/jobs:0.0,double(s[2]-p[2])/1e6/f,double(wait_ns)/1e6/f,double(waits)/f,double(drain_ns)/1e6/f,(unsigned long long)drains);
    for(int i=0;i<4;i++)p[i]=s[i];
    handed=wait_ns=waits=drain_ns=drains=0;
}
}   // namespace ptq
}   // namespace
