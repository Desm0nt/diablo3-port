#include <sys/system_properties.h>
#include <unordered_map>
#include <cmath>
// 安卓Vulkan呈现与窗口生命周期；依据原WSI桥及PR9/10的可回退呈现/钉核实验。
// Android Vulkan presentation and window lifetimes, based on the original WSI bridge and reversible PR9/10 overlap/affinity experiments.
#define VK_USE_PLATFORM_ANDROID_KHR
#include "vulkan_present.h"
#ifdef DM_GAME_DOOM
extern "C" void dm_aperf_present(ANativeWindow*,float);
#endif
#include "../gfx/vulkan/nvn_vk.h"
#include "../gfx/xtrace.h"
#include "gpu_floor.h"   // [gpu-floor] PNH-C01: GPU clock floor while frames run late (port-common gpu-floor)
#include "../gfx/vulkan/engine/xbg_device_fault.h"   // [device-fault] FN-64 (port-common device-fault)
#include "../host/cpu_topo.h"   // [cpu-place] the translator's pin comes from the placement plan unless a marker sets it
#include <android/log.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <algorithm>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <sched.h>
#include <unistd.h>
#include <time.h>
// [nonadreno] (from the DOOM 2016 port, 7c0da0e) Mali: vkWaitForFences returns VK_ERROR_INITIALIZATION_FAILED when its ppoll() is
// interrupted by a signal (Skyline fence_cycle.h cbc896c8f8, idea only); retried. Other results unchanged, so Adreno sees the same calls.
static VkResult dm_present_wait_fences(VkDevice d,uint32_t n,const VkFence *f,VkBool32 all,uint64_t timeout){
    VkResult r=vkWaitForFences(d,n,f,all,timeout);
    for(unsigned tries=0;r==VK_ERROR_INITIALIZATION_FAILED && tries<1000;++tries)r=vkWaitForFences(d,n,f,all,timeout);
    return r;}
#define vkWaitForFences dm_present_wait_fences
// [present-thread] PERF-D07 DM_GFX_PRESENT_THREAD (port-common/modules/present-thread): with the present thread on, two threads use the
// one VkQueue, so every queue call of this file takes queue_mutex (the engine's take it through dm_present_queue_lock); the blit's
// submission and the queue present of the job the present thread runs are reported to the hand-off (present_thread.h). Mode 0: no lock.
#include "present_thread.h"
static std::atomic<int> pt_cfg{0};static std::mutex pt_queue_mutex;static thread_local bool pt_on_thread=false;
static void pt_mark_submitted(void);static void pt_stamp_present(void);
struct PtQLock{bool on;PtQLock():on(pt_cfg.load(std::memory_order_relaxed)!=0){if(on)pt_queue_mutex.lock();}~PtQLock(){if(on)pt_queue_mutex.unlock();}};
static VkResult pt_queue_submit(VkQueue q,uint32_t n,const VkSubmitInfo *s,VkFence f){VkResult r;{PtQLock l;r=vkQueueSubmit(q,n,s,f);}if(r==VK_SUCCESS && pt_on_thread)pt_mark_submitted();return r;}
static VkResult pt_queue_present(VkQueue q,const VkPresentInfoKHR *p){VkResult r;{PtQLock l;r=vkQueuePresentKHR(q,p);}if(pt_on_thread)pt_stamp_present();return r;}
static VkResult pt_queue_wait_idle(VkQueue q){PtQLock l;return vkQueueWaitIdle(q);}
#define vkQueueSubmit pt_queue_submit
#define vkQueuePresentKHR pt_queue_present
#define vkQueueWaitIdle pt_queue_wait_idle
extern "C" double dm_present_period(void);
// 错过上屏节拍的帧数（晚于「上次期望时刻 + 周期」2 ms 以上）；自适应出图档按它降档 / 试升
// Frames that missed their display slot (more than 2 ms later than "previous desired time + period"); the adaptive present tier steps down / probes up by it
static std::atomic<uint64_t> dm_present_late{0};
extern "C" uint64_t dm_present_late_count(void){return dm_present_late.load(std::memory_order_relaxed);}
namespace {
std::mutex window_mutex;std::condition_variable window_cv;
// Vulkan状态仅宿主线程使用；UI锁只保护窗口快照，不跨GPU等待。
// Serialize Vulkan callers separately; never hold the UI window lock across GPU waits.
std::mutex render_mutex;
ANativeWindow *desired=nullptr,*active=nullptr;uint64_t generation=0,active_generation=0;
VkInstance instance;VkPhysicalDevice physical;VkDevice device;VkQueue queue;uint32_t family;
VkSurfaceKHR surface;VkSwapchainKHR swapchain;VkFormat swap_format;VkExtent2D extent;
VkSurfaceTransformFlagBitsKHR created_transform;VkExtent2D created_surface_extent;
// 无旋转 blit 由系统合成器旋转；记录属性以免 SUBOPTIMAL 触发逐帧重建。
// The compositor rotates our unrotated blit; cache properties to avoid rebuilding every SUBOPTIMAL frame.
bool surface_properties_changed(){
    VkSurfaceCapabilitiesKHR c{};
    if(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&c)!=VK_SUCCESS)return true;
    return c.currentTransform!=created_transform || c.currentExtent.width!=created_surface_extent.width || c.currentExtent.height!=created_surface_extent.height;
}
std::vector<VkImage> images;VkCommandPool command_pool;VkCommandBuffer command;
VkSemaphore acquired;std::vector<VkSemaphore> finished;VkFence fence;bool ready=false,swap_dirty=false;std::string shader_path;
// 缺省开启（标记文件 gfx-present-overlap 为 '0' 时关）：最多两次呈现 blit 在途，不在每帧后清空队列，游戏可在 GPU 收尾本帧时翻译下一帧。
// On by default (marker file gfx-present-overlap = '0' turns it off): keep up to two present blits in flight instead of draining the queue
// after every frame, so the game translates the next frame while the GPU finishes this one. Each in-flight frame has
// its own pool, acquire semaphore and fence; slot 0 is the original set. Later engine barriers on the source image are
// ALL_COMMANDS and its release retires with a later submission, which covers this blit in queue order.
bool overlap=false;VkCommandPool overlap_pool;VkCommandBuffer overlap_command;VkSemaphore overlap_acquired;VkFence overlap_fence;
bool in_flight[2];unsigned flight=0;
// Diagnostic (marker gfx-present-timing; overlap mode): GPU time of each present's clear and blit, from two timestamps per
// in-flight frame, read once that frame's fence has signalled; averaged over 300 presents.
bool present_timing=false;VkQueryPool present_queries=VK_NULL_HANDLE;bool present_stamped[2];double present_tick_ns=1,present_gpu_ns=0;unsigned present_timed=0;
// 游戏线程（调用呈现的线程；同步翻译时也是翻译线程）钉在最高频的核上。Redmi K50 Ultra 上调度器一直把它放在 A710
// （cpu4–6，MIUI 封顶 2.57 GHz），从不用空着的 X2（cpu7，封顶 2.25 GHz）；钉到 X2 后营地在 3D 比例 0.6 下从 40.2 升到
// 44.3 fps（同进程 A/B，CPU 17.1 → 14.3 ms）。MIUI 会改线程的可用核掩码，所以每 120 次呈现核对一次。
// 标记 gfx-no-pin 关闭，gfx-pin-cpu = 十六进制掩码 指定核；gfx-ab-toggle = N 加 gfx-ab-target = pin 时每 N 秒钉 / 放开交替。
// The game thread (the caller of present, which also translates in sync mode) runs on the cores with the highest maximum
// frequency. On the Redmi K50 Ultra the scheduler kept it on the A710 cores (cpu4-6, capped at 2.57 GHz by MIUI) and never
// used the idle X2 (cpu7, capped at 2.25 GHz); pinned to the X2 the camp went from 40.2 to 44.3 fps at 3D scale 0.6
// (in-process A/B, CPU 17.1 -> 14.3 ms). MIUI rewrites thread affinity masks, so the pin is checked every 120 presents.
// Marker gfx-no-pin turns it off and gfx-pin-cpu = hex mask picks the cores; gfx-ab-toggle = N with gfx-ab-target = pin
// alternates pinned and released every N seconds, in phase with the engine's A/B (its PERF lines carry ab=).
// [cpu-place] Without any of these markers the pin is no longer made here: hle_cpu_place.c places this thread (and the guest threads) by
// the capacity plan and the gear's "CPU cores" mode, checks the masks every 2 s and again after onResume; pin_update only tells it
// which thread presents. With a marker this code pins as before and the module leaves the presenting thread alone.
unsigned pin_mask=0;int pin_ab=0;int pin_state=-1;unsigned pin_presents=0,pin_drifts=0,pin_failures=0;cpu_set_t pin_original;bool pin_have_original=false;bool pin_markers=false;
unsigned fastest_cores(){
    unsigned best=0,mask=0,present=0;
    for(int c=0;c<32;c++){char path[96];snprintf(path,sizeof path,"/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq",c);FILE *f=fopen(path,"rb");if(!f)continue;
        unsigned v=0;if(fscanf(f,"%u",&v)==1 && v){present|=1u<<c;if(v>best){best=v;mask=0;}if(v==best)mask|=1u<<c;}fclose(f);}
    return mask==present?0:mask;   // one core class: nothing to pick
}
void pin_update(){
    dm_cpu_place_translator(int(gettid()));if(!pin_markers)return;   // [cpu-place]
    if(!pin_mask)return;bool want=true;
    if(pin_ab>0){timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);want=((ts.tv_sec/pin_ab)&1)==0;}
    bool changed=int(want)!=pin_state;if(!changed && ++pin_presents%120)return;
    if(!pin_have_original){CPU_ZERO(&pin_original);if(sched_getaffinity(0,sizeof pin_original,&pin_original))for(int c=0;c<32;c++)CPU_SET(c,&pin_original);pin_have_original=true;}
    cpu_set_t set;CPU_ZERO(&set);if(want){for(int c=0;c<32;c++)if(pin_mask>>c&1)CPU_SET(c,&set);}else set=pin_original;
    if(!changed){cpu_set_t cur;CPU_ZERO(&cur);if(!sched_getaffinity(0,sizeof cur,&cur) && CPU_EQUAL(&cur,&set))return;++pin_drifts;}
    int r=sched_setaffinity(0,sizeof set,&set),err=r?errno:0;pin_state=want;if(r)++pin_failures;
    // Refused while the app's cpuset lacks the cores (Android moves the app between cpusets, at start for one): the kernel
    // keeps the request and applies it again when the cpuset allows, and the next check retries.
    if(changed || pin_drifts<=4 || (r && pin_failures<=4))__android_log_print(ANDROID_LOG_INFO,"dm-vulkan","NVN pin tid=%d %s mask=%x result=%d errno=%d drifts=%u failures=%u",int(gettid()),want?"pinned":"released",want?pin_mask:0u,r,err,pin_drifts,pin_failures);
}
void log_message(void*,const char *text){__android_log_print(ANDROID_LOG_INFO,"dm-vulkan","%s",text);}
[[noreturn]] void fatal(void*,const char *text){__android_log_print(ANDROID_LOG_FATAL,"dm-vulkan","%s",text);abort();}
namespace upscale{bool enabled();void drop_targets();bool targets(const std::vector<VkImage>&,VkFormat,VkExtent2D);void destroy_all();}   // [D3] gear "Upscale" / DM_GFX_UPSCALE, below
void destroy_present_semaphores(){for(auto s:finished)if(s)vkDestroySemaphore(device,s,nullptr);finished.clear();upscale::drop_targets();}   // [D3] + the upscale pass's views of the swapchain images
// 调用者先等队列空闲；基础WSI销毁仍沿用WaitIdle惯例，不声称提供呈现栅栏保证。
// Call only after queue idle. Base WSI teardown follows the WaitIdle convention,
// which is not the explicit presentation-fence guarantee of swapchain_maintenance1.
void destroy_swap(){destroy_present_semaphores();if(swapchain)vkDestroySwapchainKHR(device,swapchain,nullptr);swapchain=VK_NULL_HANDLE;images.clear();if(surface)vkDestroySurfaceKHR(instance,surface,nullptr);surface=VK_NULL_HANDLE;if(active)ANativeWindow_release(active);active=nullptr;}
void cleanup_initialization(){
    if(queue)vkQueueWaitIdle(queue);
    if(device){if(fence)vkDestroyFence(device,fence,nullptr);if(acquired)vkDestroySemaphore(device,acquired,nullptr);if(command_pool)vkDestroyCommandPool(device,command_pool,nullptr);}
    fence=VK_NULL_HANDLE;acquired=VK_NULL_HANDLE;command_pool=VK_NULL_HANDLE;command=VK_NULL_HANDLE;
    if(device){if(overlap_fence)vkDestroyFence(device,overlap_fence,nullptr);if(overlap_acquired)vkDestroySemaphore(device,overlap_acquired,nullptr);if(overlap_pool)vkDestroyCommandPool(device,overlap_pool,nullptr);}
    overlap_fence=VK_NULL_HANDLE;overlap_acquired=VK_NULL_HANDLE;overlap_pool=VK_NULL_HANDLE;overlap_command=VK_NULL_HANDLE;overlap=false;in_flight[0]=in_flight[1]=false;flight=0;
    destroy_swap();if(device)upscale::destroy_all();   // [D3] its pipelines / samplers belong to this device (DRV-07 initializes again with another driver)
    if(device)vkDestroyDevice(device,nullptr);device=VK_NULL_HANDLE;queue=VK_NULL_HANDLE;
    if(instance)vkDestroyInstance(instance,nullptr);instance=VK_NULL_HANDLE;physical=VK_NULL_HANDLE;
}
// 刷新率投票：App 不投票时，安卓（MIUI）让这个表面跑 60 Hz，尽管屏和系统设置都允许 120。投 120 Hz 不改 60 帧的出图节奏（nvn_pace_hz），
// 但赶不上 16.7 ms 的那一帧在屏上停 25 ms 而不是 33.3 ms。标记 gfx-frame-rate = N（Hz；0 = 不投票）。
// Frame-rate vote: without a vote from the app, Android (MIUI) runs this surface at 60 Hz although the panel and the system
// setting allow 120. A 120 Hz vote keeps the 60 fps present pacing (nvn_pace_hz), but a frame that misses 16.7 ms stays on
// screen for 25 ms instead of 33.3. Marker gfx-frame-rate = N (Hz; 0 = no vote).
float frame_rate_vote=120;
// 按游戏输出尺寸出图：窗口缓冲设成游戏输出的尺寸（2000×900），由显示硬件放大到屏幕，呈现端只做 1:1 拷贝，不再每帧清屏、
// 用 GPU 按最近邻放大到 2712×1220。分辨率不变，最后一步放大从 GPU 移到显示控制器（滤波也更平滑）。标记 gfx-present-native = 0 关。
// Present at the game's output size: the window's buffers take the game's output size (2000x900) and the display hardware
// scales them to the screen, so the presenter copies 1:1 instead of clearing and nearest-scaling to 2712x1220 on the GPU
// every frame. Same resolution; the final upscale moves from the GPU to the display controller (with smoother filtering).
// Marker gfx-present-native = 0 turns it off.
#ifdef __OHOS__
bool present_native=false;   // 鸿蒙：缓冲尺寸与合成缩放未在真机核过，先关；标记 gfx-present-native = 1 打开 | HarmonyOS: buffer geometry vs compositor scaling not yet checked on device; off, marker gfx-present-native = 1 enables
#else
bool present_native=true;
#endif
int native_w=-1,native_h=-1;   // the buffer geometry last set on this window (-1 = none yet; 0x0 = the window's own size, the upscale pass)
int create_surface(){
    destroy_swap();native_w=native_h=-1;
    {std::lock_guard<std::mutex> lock(window_mutex);if(!desired)return VK_NOT_READY;active=desired;ANativeWindow_acquire(active);active_generation=generation;}
    if(frame_rate_vote>0){
        // 运行时查询API，系统拒绝投票不会中止Vulkan初始化。 | Resolve the API at runtime; a refused vote does not abort Vulkan initialization.
        static auto set_rate=[]{void *lib=dlopen("libnativewindow.so",RTLD_NOW);return reinterpret_cast<int32_t(*)(ANativeWindow*,float,int8_t)>(lib?dlsym(lib,"ANativeWindow_setFrameRate"):nullptr);}();
        int fr=set_rate?set_rate(active,frame_rate_vote,ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT):-1;
        __android_log_print(ANDROID_LOG_INFO,"dm-vulkan","Frame rate vote %.0f Hz result=%d (marker gfx-frame-rate; display request, not FPS)",double(frame_rate_vote),fr);
    }
    VkAndroidSurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};ci.window=active;
    VkResult r=vkCreateAndroidSurfaceKHR(instance,&ci,nullptr,&surface);if(r)return r;
    if(!device)return 0;VkBool32 supported=VK_FALSE;r=vkGetPhysicalDeviceSurfaceSupportKHR(physical,family,surface,&supported);if(r||!supported)return r?r:VK_ERROR_FEATURE_NOT_PRESENT;return 0;
}
int create_swap(){
    VkSurfaceCapabilitiesKHR cap;VkResult r=vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&cap);if(r)return r;
    if(!(cap.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT))return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if(!(cap.supportedTransforms&VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR))return VK_ERROR_FEATURE_NOT_PRESENT;
    uint32_t n=0;r=vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&n,nullptr);if(r||!n)return r?r:VK_ERROR_FORMAT_NOT_SUPPORTED;
    std::vector<VkSurfaceFormatKHR> formats(n);r=vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&n,formats.data());if(r)return r;
    VkSurfaceFormatKHR sf=formats[0];if(sf.format==VK_FORMAT_UNDEFINED)sf.format=VK_FORMAT_R8G8B8A8_UNORM;
    for(auto f:formats)if(f.format==VK_FORMAT_R8G8B8A8_UNORM||f.format==VK_FORMAT_B8G8R8A8_UNORM){sf=f;break;}
    extent=cap.currentExtent;if(extent.width==UINT32_MAX){extent.width=std::clamp(uint32_t(std::max(1,ANativeWindow_getWidth(active))),cap.minImageExtent.width,cap.maxImageExtent.width);extent.height=std::clamp(uint32_t(std::max(1,ANativeWindow_getHeight(active))),cap.minImageExtent.height,cap.maxImageExtent.height);}if(!extent.width||!extent.height)return VK_NOT_READY;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};ci.surface=surface;
#ifdef __OHOS__
    {unsigned want=4;if(const char *e=getenv("DM_SWAP_IMAGES"))want=unsigned(std::max(2,atoi(e)));ci.minImageCount=std::max(want,cap.minImageCount);}   // 马良最佳实践：重负载 3–4 张；按期望上屏时刻排帧时合成器手里多压一张，取 4 张免得翻译线程等交换链、反压到游戏帧节奏 | Maleoon best practice: 3–4 under heavy load; with desired display times the compositor holds one more buffer, so take 4 to keep the translation thread from waiting on the swapchain and back-pressuring the game cadence
#else
    ci.minImageCount=std::max(2u,cap.minImageCount);
#endif
    if(cap.maxImageCount)ci.minImageCount=std::min(ci.minImageCount,cap.maxImageCount);ci.imageFormat=sf.format;ci.imageColorSpace=sf.colorSpace;ci.imageExtent=extent;ci.imageArrayLayers=1;ci.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;ci.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;ci.preTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;ci.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if(!(cap.supportedCompositeAlpha&ci.compositeAlpha))for(uint32_t b=1;b<=8;b<<=1)if(cap.supportedCompositeAlpha&b){ci.compositeAlpha=VkCompositeAlphaFlagBitsKHR(b);break;}
    // [D3] gear "Upscale": the pass renders into the swapchain images (the blit fallback still needs TRANSFER_DST)
    bool up_usage=false;if(upscale::enabled() && (cap.supportedUsageFlags&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)){ci.imageUsage|=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;up_usage=true;}
    ci.presentMode=VK_PRESENT_MODE_FIFO_KHR;ci.clipped=VK_TRUE;ci.oldSwapchain=swapchain;VkSwapchainKHR replacement=VK_NULL_HANDLE;r=vkCreateSwapchainKHR(device,&ci,nullptr,&replacement);
    // 创建失败也会退休oldSwapchain；重试不得继续传入退休句柄。
    // Even failed creation retires oldSwapchain; never pass that retired handle on retry.
    destroy_present_semaphores();if(swapchain)vkDestroySwapchainKHR(device,swapchain,nullptr);swapchain=r?VK_NULL_HANDLE:replacement;images.clear();if(r)return r;swap_format=sf.format;
    created_transform=cap.currentTransform;created_surface_extent=cap.currentExtent;
    __android_log_print(ANDROID_LOG_INFO,"dm-vulkan","Swapchain: window=%dx%d extent=%ux%u currentTransform=%u supported=%u selected=%u",ANativeWindow_getWidth(active),ANativeWindow_getHeight(active),extent.width,extent.height,unsigned(cap.currentTransform),unsigned(cap.supportedTransforms),unsigned(ci.preTransform));
    r=vkGetSwapchainImagesKHR(device,swapchain,&n,nullptr);if(r)return r;images.resize(n);r=vkGetSwapchainImagesKHR(device,swapchain,&n,images.data());if(r)return r;images.resize(n);
    if(upscale::enabled())upscale::targets(up_usage?images:std::vector<VkImage>{},swap_format,extent);   // [D3] views + framebuffers (no colour-attachment usage / a failure: the pass is off, the blit stays)
    // 呈现完成信号量按交换链图像分配；重新获取该图像后才能再次发信号。
    // Index present-wait semaphores by acquired image, not by CPU frame/fence.
    finished.resize(n,VK_NULL_HANDLE);VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for(auto &s:finished){r=vkCreateSemaphore(device,&sc,nullptr,&s);if(r)return r;}return VK_SUCCESS;
}
// 诊断（标记 gfx-membench）：每种主机可见内存类型的 CPU 写、CPU 读，以及 GPU 填充后的 CPU 读速度，启动时测一次。
// Diagnostic (marker gfx-membench): CPU write, CPU read, and CPU read after a GPU fill, for each host-visible memory type; run once at start.
void membench(){
    VkPhysicalDeviceMemoryProperties mp;vkGetPhysicalDeviceMemoryProperties(physical,&mp);
    const VkDeviceSize n=4u<<20;std::vector<uint8_t> host(n,1),back(n);
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;cp.queueFamilyIndex=family;VkCommandPool pool;if(vkCreateCommandPool(device,&cp,nullptr,&pool))return;
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;VkCommandBuffer cmd;if(vkAllocateCommandBuffers(device,&ca,&cmd)){vkDestroyCommandPool(device,pool,nullptr);return;}
    auto now=[]{timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return double(ts.tv_sec)*1e9+double(ts.tv_nsec);};
    for(uint32_t t=0;t<mp.memoryTypeCount;t++){
        VkMemoryPropertyFlags f=mp.memoryTypes[t].propertyFlags;if(!(f&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))continue;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=n;bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;VkBuffer b;if(vkCreateBuffer(device,&bi,nullptr,&b))continue;
        VkMemoryRequirements r;vkGetBufferMemoryRequirements(device,b,&r);if(!(r.memoryTypeBits&(1u<<t))){vkDestroyBuffer(device,b,nullptr);continue;}
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=r.size;ai.memoryTypeIndex=t;VkDeviceMemory m;if(vkAllocateMemory(device,&ai,nullptr,&m)){vkDestroyBuffer(device,b,nullptr);continue;}
        void *p=nullptr;if(vkBindBufferMemory(device,b,m,0)||vkMapMemory(device,m,0,VK_WHOLE_SIZE,0,&p)){vkFreeMemory(device,m,nullptr);vkDestroyBuffer(device,b,nullptr);continue;}
        bool coherent=(f&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;VkMappedMemoryRange mr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};mr.memory=m;mr.size=VK_WHOLE_SIZE;
        double w=1e30,rc=1e30,rd=1e30,flush=0,inv=0;bool ok=true;
        for(int k=0;k<4;k++){double t0=now();memcpy(p,host.data(),n);double t1=now();if(!coherent)vkFlushMappedMemoryRanges(device,1,&mr);double t2=now();w=std::min(w,t1-t0);flush=std::max(flush,t2-t1);}
        for(int k=0;k<4;k++){double t0=now();memcpy(back.data(),p,n);rc=std::min(rc,now()-t0);}
        for(int k=0;k<4;k++){
            vkResetCommandBuffer(cmd,0);VkCommandBufferBeginInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};vkBeginCommandBuffer(cmd,&cb);
            uint32_t value=0x01020304u+uint32_t(k);vkCmdFillBuffer(cmd,b,0,n,value);
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};mb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&mb,0,nullptr,0,nullptr);
            vkEndCommandBuffer(cmd);VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&cmd;if(vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE)||vkQueueWaitIdle(queue)){ok=false;break;}
            double t0=now();if(!coherent)vkInvalidateMappedMemoryRanges(device,1,&mr);double t1=now();memcpy(back.data(),p,n);double t2=now();
            rd=std::min(rd,t2-t1);inv=std::max(inv,t1-t0);uint32_t got;memcpy(&got,back.data()+n-4,4);ok=ok&&got==value;
        }
        __android_log_print(ANDROID_LOG_INFO,"dm-vulkan","NVN membench type=%u flags=%#x write=%.2f GB/s (flush %.0f us) read=%.2f GB/s read_after_gpu=%.2f GB/s (invalidate %.0f us) ok=%d",
            t,unsigned(f),double(n)/w,flush/1e3,double(n)/rc,double(n)/rd,inv/1e3,int(ok));
        vkUnmapMemory(device,m);vkFreeMemory(device,m,nullptr);vkDestroyBuffer(device,b,nullptr);
    }
    vkDestroyCommandPool(device,pool,nullptr);
}
int has_window(void*){std::lock_guard<std::mutex> l(window_mutex);return desired!=nullptr;}
void wait_window(){std::unique_lock<std::mutex> lock(window_mutex);window_cv.wait(lock,[]{return desired!=nullptr;});}
bool window_changed(){std::lock_guard<std::mutex> lock(window_mutex);return !desired||generation!=active_generation;}
void wait_window_change(uint64_t bad){std::unique_lock<std::mutex> lock(window_mutex);window_cv.wait(lock,[bad]{return generation!=bad;});}
void wait_resize(){std::unique_lock<std::mutex> lock(window_mutex);window_cv.wait_for(lock,std::chrono::milliseconds(16),[]{return generation!=active_generation;});}
// [DOOM] Optional sharpening of the game's frame at present: RCAS, the sharpening pass of AMD FSR 1 (shaders/rcas.comp, offline
// compiled). DM_GFX_SHARPEN=<stops> turns it on (0 = strongest, 0.2 typical, 1 mild); used with DM_DOOM_RES (the game renders
// 3D at a fraction and upscales it itself). Only for 8-bit UNORM RGBA/BGRA frames; anything else is presented unsharpened.
extern "C" { int dm_vk_format_list=0; int dm_vk_custom_border=0; }   // [XC2] custom_border: VK_EXT_custom_border_color enabled (customBorderColorWithoutFormat)   // VK_KHR_image_format_list enabled on the device (read by the Vulkan engine)
extern "C" { int dm_vk_store_none=0; }   // [nonadreno][tile] VK_EXT / VK_KHR_load_store_op_none enabled on the device (read by the engine: DM_GFX_STORE_NONE)
extern "C" { int dm_vk_descriptor_indexing=0; }   // [ETERNAL] VK_EXT_descriptor_indexing enabled: partially bound + sampled-image update-after-bind (bindless pools)
namespace rcas {
const uint32_t code[]={
#include "shaders/rcas.spv.inc"
};
int on=-1;float sharpness=1;VkImage image{};VkDeviceMemory memory{};VkImageView view{};uint32_t w=0,h=0;
VkDescriptorSetLayout set_layout{};VkPipelineLayout layout{};VkPipeline pipeline{};VkDescriptorPool pool{};VkDescriptorSet sets[2]{};VkSampler sampler{};VkImageView source_views[2]{};
bool enabled(){if(on<0){const char *e=getenv("DM_GFX_SHARPEN");on=e && *e && atof(e)>=0;if(on){sharpness=std::exp2(-float(atof(e)));__android_log_print(ANDROID_LOG_INFO,"dm-vulkan","RCAS sharpening at present: stops=%s (DM_GFX_SHARPEN)",e);}}return on>0;}
uint32_t memory_type(uint32_t bits,VkMemoryPropertyFlags want){VkPhysicalDeviceMemoryProperties mp;vkGetPhysicalDeviceMemoryProperties(physical,&mp);for(uint32_t i=0;i<mp.memoryTypeCount;i++)if((bits&(1u<<i)) && (mp.memoryTypes[i].propertyFlags&want)==want)return i;return 0;}
bool setup(){
    if(pipeline)return true;
    VkDescriptorSetLayoutBinding b[2]{};b[0].binding=0;b[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;b[0].descriptorCount=1;b[0].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
    b[1].binding=1;b[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;b[1].descriptorCount=1;b[1].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};dl.bindingCount=2;dl.pBindings=b;if(vkCreateDescriptorSetLayout(device,&dl,nullptr,&set_layout))return false;
    VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT,0,12};VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&set_layout;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&pc;
    if(vkCreatePipelineLayout(device,&pl,nullptr,&layout))return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};mi.codeSize=sizeof code;mi.pCode=code;VkShaderModule module{};if(vkCreateShaderModule(device,&mi,nullptr,&module))return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=module;ci.stage.pName="main";ci.layout=layout;
    VkResult r=vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&ci,nullptr,&pipeline);vkDestroyShaderModule(device,module,nullptr);if(r)return false;
    VkDescriptorPoolSize ps[2]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,2}};VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dp.maxSets=2;dp.poolSizeCount=2;dp.pPoolSizes=ps;
    if(vkCreateDescriptorPool(device,&dp,nullptr,&pool))return false;
    VkDescriptorSetLayout ls[2]={set_layout,set_layout};VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=pool;ai.descriptorSetCount=2;ai.pSetLayouts=ls;if(vkAllocateDescriptorSets(device,&ai,sets))return false;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.magFilter=si.minFilter=VK_FILTER_NEAREST;si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;if(vkCreateSampler(device,&si,nullptr,&sampler))return false;
    return true;
}
bool target(uint32_t width,uint32_t height){
    if(image && w==width && h==height)return true;
    if(view)vkDestroyImageView(device,view,nullptr);if(image)vkDestroyImage(device,image,nullptr);if(memory)vkFreeMemory(device,memory,nullptr);view={};image={};memory={};
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ic.imageType=VK_IMAGE_TYPE_2D;ic.format=VK_FORMAT_R8G8B8A8_UNORM;ic.extent={width,height,1};ic.mipLevels=ic.arrayLayers=1;ic.samples=VK_SAMPLE_COUNT_1_BIT;
    ic.tiling=VK_IMAGE_TILING_OPTIMAL;ic.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;if(vkCreateImage(device,&ic,nullptr,&image))return false;
    VkMemoryRequirements mr;vkGetImageMemoryRequirements(device,image,&mr);VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ma.allocationSize=mr.size;ma.memoryTypeIndex=memory_type(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(vkAllocateMemory(device,&ma,nullptr,&memory) || vkBindImageMemory(device,image,memory,0))return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=ic.format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};if(vkCreateImageView(device,&vi,nullptr,&view))return false;
    w=width;h=height;return true;
}
// Records the pass; on success the caller blits from rcas::image (TRANSFER_SRC_OPTIMAL) instead of the source.
bool record(VkCommandBuffer cmd,unsigned f,VkImage source,VkFormat format,uint32_t width,uint32_t height){
    if(!enabled() || (format!=VK_FORMAT_R8G8B8A8_UNORM && format!=VK_FORMAT_B8G8R8A8_UNORM))return false;
    if(!setup() || !target(width,height)){on=0;__android_log_print(ANDROID_LOG_WARN,"dm-vulkan","RCAS setup failed: sharpening off");return false;}
    if(source_views[f])vkDestroyImageView(device,source_views[f],nullptr);   // this flight's previous frame has retired (its fence was waited)
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=source;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    if(vkCreateImageView(device,&vi,nullptr,&source_views[f])){source_views[f]={};return false;}
    VkDescriptorImageInfo si{sampler,source_views[f],VK_IMAGE_LAYOUT_GENERAL},di{VK_NULL_HANDLE,view,VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr[2]{};for(auto &x:wr){x.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;x.dstSet=sets[f];x.descriptorCount=1;}
    wr[0].dstBinding=0;wr[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;wr[0].pImageInfo=&si;
    wr[1].dstBinding=1;wr[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;wr[1].pImageInfo=&di;vkUpdateDescriptorSets(device,2,wr,0,nullptr);
    VkImageMemoryBarrier b[2]{};
    for(auto &x:b){x.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;x.srcQueueFamilyIndex=x.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;x.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};}
    b[0].image=source;b[0].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b[0].newLayout=VK_IMAGE_LAYOUT_GENERAL;b[0].srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;b[0].dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    b[1].image=image;b[1].oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b[1].newLayout=VK_IMAGE_LAYOUT_GENERAL;b[1].srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;b[1].dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,2,b);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&sets[f],0,nullptr);
    struct{int32_t w,h;float s;}push{int32_t(width),int32_t(height),sharpness};vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof push,&push);
    vkCmdDispatch(cmd,(width+7)/8,(height+7)/8,1);
    b[0].oldLayout=VK_IMAGE_LAYOUT_GENERAL;b[0].newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b[0].srcAccessMask=VK_ACCESS_SHADER_READ_BIT;b[0].dstAccessMask=0;   // back to the layout the engine tracks
    b[1].oldLayout=VK_IMAGE_LAYOUT_GENERAL;b[1].newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b[1].srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;b[1].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,2,b);
    return true;
}
}
// [D3] Upscale to the screen at present (gear "Upscale": Off / Auto, live; DM_GFX_UPSCALE = 0 | auto | gsr | fsr; KB SYNC-17, the DOOM 2016
// DM_GFX_UPSCALE path adapted). Off (default): the window's buffers keep the game's output size (gfx-present-native) and the compositor
// stretches them to the SurfaceView (bilinear). On: the buffers take the SurfaceView's own size (setBuffersGeometry 0x0) and our pass fills
// them from the game's frame: Snapdragon GSR 1 (shaders/gsr.frag, from DOOM 2016, BSD-3; with DM_GFX_SHARPEN RCAS is folded into the same
// pass) or AMD FSR 1 EASU (shaders/fsr_easu.frag, MIT; with DM_GFX_SHARPEN EASU renders an intermediate image and shaders/fsr_rcas.frag
// sharpens it into the swapchain image). Auto = GSR on Adreno (Qualcomm and Turnip: vendor 0x5143), FSR on every other GPU. The SurfaceView
// already has the picture's shape (DmActivity.applyAspectLayout: 16:9 bars, stretched, widescreen, 4:3; hle_d3.c's loading-screen crop is
// stretched too), so the pass fills the whole buffer as the compositor did. Only when it enlarges (the native-resolution option makes the
// game's output the screen size: the plain 1:1 blit then), only 8-bit / 10-bit UNORM frames; a setup failure turns it off for the run (the
// old path, logged). The gear switches it live (dm_present_set_upscale): the next present rebuilds the swapchain. Every ~3 s
// "NVN present upscale <gsr|fsr> WxH -> WxH gpu_us=.." (two timestamps per present).
namespace upscale {
const uint32_t vert_code[]={
#include "shaders/upscale.vert.spv.inc"
};
const uint32_t gsr_code[]={
#include "shaders/gsr.frag.spv.inc"
};
const uint32_t easu_code[]={
#include "shaders/fsr_easu.frag.spv.inc"
};
const uint32_t rcas_code[]={
#include "shaders/fsr_rcas.frag.spv.inc"
};
enum{OFF=0,GSR=1,FSR=2};
std::atomic<int> requested{-1};   // -1 = not read yet; 0 off, 1 auto, 2 gsr, 3 fsr (DM_GFX_UPSCALE, then dm_present_set_upscale)
int mode=OFF;bool broken=false,adreno=false;   // mode: applied on the presenting thread; broken: a setup failure, off for this run
const float edge_sharpness=2.0f;   // SGSR EdgeSharpness
VkSampler sampler{};VkDescriptorSetLayout set_layout{};VkPipelineLayout layout{};VkDescriptorPool pool{};VkDescriptorSet sets[2][2]{};VkImageView src_views[2]{};
VkFormat target_format=VK_FORMAT_UNDEFINED;VkRenderPass pass_swap{},pass_mid{};VkPipeline pipe_gsr{},pipe_easu{},pipe_easu_mid{},pipe_rcas{};
std::vector<VkImageView> views;std::vector<VkFramebuffer> framebuffers;VkExtent2D fb_extent{};
struct Mid{VkImage image{};VkDeviceMemory memory{};VkImageView view{};VkFramebuffer fb{};uint32_t w=0,h=0;}mid[2];   // FSR + RCAS: EASU's output, one per flight
VkQueryPool queries{};bool stamped[2]{};double tick_ns=0,sum_ns=0,max_ns=0;unsigned timed=0;int64_t window_t0=0;int last_sw=0,last_sh=0,last_dw=0,last_dh=0;
int64_t mono_ns(){timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return int64_t(ts.tv_sec)*1000000000+ts.tv_nsec;}
const char *name(int m){return m==GSR?"gsr":m==FSR?"fsr":"off";}
int parse(const char *e){if(!e||!*e||!strcmp(e,"0")||!strcmp(e,"off"))return 0;if(!strcmp(e,"auto")||!strcmp(e,"1"))return 1;if(!strcmp(e,"gsr"))return 2;if(!strcmp(e,"fsr"))return 3;return -2;}
// [D3] 15.11: GSR dropped from the choices (owner, Mi 14 A/B: SGSR 1 "mobile" looked softer than the display hardware's own scaler, ~10 %
// less fine detail at +2.3 ms GPU); auto and the old "gsr" value both mean FSR 1 now. The GSR pipeline stays built for nothing but a manual A/B.
int resolve(int r){return broken?OFF:(r==1||r==2||r==3)?FSR:OFF;}
bool enabled(){return mode!=OFF;}
// Once per initialization: the GPU vendor (for auto) and, the first time, the request from the environment (the gear sets it at start).
void decide(){
    VkPhysicalDeviceProperties pp;vkGetPhysicalDeviceProperties(physical,&pp);adreno=pp.vendorID==0x5143;
    const char *e=getenv("DM_GFX_UPSCALE");int r=parse(e);
    if(r==-2){__android_log_print(ANDROID_LOG_WARN,"dm-vulkan","NVN present upscale: DM_GFX_UPSCALE=%s is not known (0 | auto | gsr | fsr): off",e);r=0;}
    int u=-1;requested.compare_exchange_strong(u,r);r=requested.load();
    __android_log_print(ANDROID_LOG_INFO,"dm-vulkan","NVN present upscale %s (DM_GFX_UPSCALE=%s; %s GPU vendor %#x: auto = %s)%s",r==1?"auto":name(resolve(r)),e&&*e?e:"unset",
        adreno?"Adreno":"non-Adreno",pp.vendorID,"fsr",resolve(r)?"; the window's buffers take the screen size, our pass scales the game's frame into them":"; the compositor stretches the game's output size");
}
void fail(const char *what,int r){if(mode!=OFF)__android_log_print(ANDROID_LOG_WARN,"dm-vulkan","NVN present upscale %s off: %s failed (VkResult %d); the compositor stretch from now on",name(mode),what,r);broken=true;mode=OFF;}
// Frames the pass takes: sampled as UNORM and written to the UNORM swapchain (sRGB / float frames keep the blit).
bool takes(VkFormat f,VkFormatFeatureFlags features){return enabled() && (f==VK_FORMAT_R8G8B8A8_UNORM || f==VK_FORMAT_B8G8R8A8_UNORM || f==VK_FORMAT_A2B10G10R10_UNORM_PACK32) && (features&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);}
void drop_mid(){for(auto &m:mid){if(m.fb)vkDestroyFramebuffer(device,m.fb,nullptr);if(m.view)vkDestroyImageView(device,m.view,nullptr);if(m.image)vkDestroyImage(device,m.image,nullptr);if(m.memory)vkFreeMemory(device,m.memory,nullptr);m=Mid{};}}
// The callers drained the queue (swapchain rebuild, surface loss, teardown).
void drop_targets(){for(auto fb:framebuffers)if(fb)vkDestroyFramebuffer(device,fb,nullptr);for(auto v:views)if(v)vkDestroyImageView(device,v,nullptr);framebuffers.clear();views.clear();fb_extent={};if(device)drop_mid();}
void drop_pipelines(){for(VkPipeline *p:{&pipe_gsr,&pipe_easu,&pipe_easu_mid,&pipe_rcas}){if(*p)vkDestroyPipeline(device,*p,nullptr);*p={};}
    for(VkRenderPass *p:{&pass_swap,&pass_mid}){if(*p)vkDestroyRenderPass(device,*p,nullptr);*p={};}target_format=VK_FORMAT_UNDEFINED;}
// cleanup_initialization, after the queue is idle: everything of this device (a driver fallback initializes again)
void destroy_all(){
    drop_targets();drop_pipelines();
    for(auto &v:src_views){if(v)vkDestroyImageView(device,v,nullptr);v={};}
    if(queries)vkDestroyQueryPool(device,queries,nullptr);if(pool)vkDestroyDescriptorPool(device,pool,nullptr);if(layout)vkDestroyPipelineLayout(device,layout,nullptr);
    if(set_layout)vkDestroyDescriptorSetLayout(device,set_layout,nullptr);if(sampler)vkDestroySampler(device,sampler,nullptr);
    queries={};pool={};layout={};set_layout={};sampler={};for(auto &s:sets)s[0]=s[1]=VK_NULL_HANDLE;stamped[0]=stamped[1]=false;mode=OFF;broken=false;
}
bool setup(){
    if(layout)return true;VkResult r;
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.magFilter=si.minFilter=VK_FILTER_LINEAR;si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if((r=vkCreateSampler(device,&si,nullptr,&sampler))){sampler={};fail("sampler",r);return false;}
    VkDescriptorSetLayoutBinding b{};b.binding=0;b.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;b.descriptorCount=1;b.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};dl.bindingCount=1;dl.pBindings=&b;if((r=vkCreateDescriptorSetLayout(device,&dl,nullptr,&set_layout))){set_layout={};fail("descriptor set layout",r);return false;}
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,4};VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dp.maxSets=4;dp.poolSizeCount=1;dp.pPoolSizes=&ps;
    if((r=vkCreateDescriptorPool(device,&dp,nullptr,&pool))){pool={};fail("descriptor pool",r);return false;}
    VkDescriptorSetLayout ls[4]={set_layout,set_layout,set_layout,set_layout};VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=pool;ai.descriptorSetCount=4;ai.pSetLayouts=ls;
    if((r=vkAllocateDescriptorSets(device,&ai,&sets[0][0]))){fail("descriptor sets",r);return false;}
    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT,0,64};VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&set_layout;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&pc;
    if((r=vkCreatePipelineLayout(device,&pl,nullptr,&layout))){layout={};fail("pipeline layout",r);return false;}
    // GPU cost: timestamps need a queue family with timestamp bits
    uint32_t n=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,nullptr);std::vector<VkQueueFamilyProperties> q(n);vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,q.data());
    VkPhysicalDeviceProperties pp;vkGetPhysicalDeviceProperties(physical,&pp);
    if(family<n && q[family].timestampValidBits && pp.limits.timestampPeriod>0){VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};qi.queryType=VK_QUERY_TYPE_TIMESTAMP;qi.queryCount=4;if(vkCreateQueryPool(device,&qi,nullptr,&queries))queries=VK_NULL_HANDLE;else tick_ns=pp.limits.timestampPeriod;}
    if(!queries)__android_log_print(ANDROID_LOG_INFO,"dm-vulkan","NVN present upscale: no timestamps on this queue, gpu_us is not measured");
    return true;
}
// One render pass: load DONT_CARE (the pass writes every pixel), final layout = present (swapchain) or shader read (the intermediate image).
bool make_pass(VkFormat format,bool swap,VkRenderPass &out){
    VkAttachmentDescription a{};a.format=format;a.samples=VK_SAMPLE_COUNT_1_BIT;a.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;a.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;a.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;a.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;a.finalLayout=swap?VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription sp{};sp.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sp.colorAttachmentCount=1;sp.pColorAttachments=&ref;
    // Swapchain: the layout transition waits for the acquire semaphore (the submit waits it at COLOR_ATTACHMENT_OUTPUT), the present after it.
    // Intermediate: earlier reads of it (RCAS, fragment shader) before the new writes; RCAS reads it after them.
    VkSubpassDependency d[2]{};d[0].srcSubpass=VK_SUBPASS_EXTERNAL;d[0].dstSubpass=0;d[0].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|(swap?0:VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    d[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;d[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    d[1].srcSubpass=0;d[1].dstSubpass=VK_SUBPASS_EXTERNAL;d[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;d[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    if(swap)d[1].dstStageMask=VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;else{d[1].dstStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;d[1].dstAccessMask=VK_ACCESS_SHADER_READ_BIT;}
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=1;rp.pAttachments=&a;rp.subpassCount=1;rp.pSubpasses=&sp;rp.dependencyCount=2;rp.pDependencies=d;
    VkResult r=vkCreateRenderPass(device,&rp,nullptr,&out);if(r){out={};fail("render pass",r);return false;}return true;
}
bool make_pipeline(VkRenderPass pass,const uint32_t *frag,size_t bytes,const char *what,VkPipeline &out){
    if(out)return true;VkResult r;
    VkShaderModule m[2]{};VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize=sizeof vert_code;mi.pCode=vert_code;if((r=vkCreateShaderModule(device,&mi,nullptr,&m[0]))){fail("vertex shader",r);return false;}
    mi.codeSize=bytes;mi.pCode=frag;if((r=vkCreateShaderModule(device,&mi,nullptr,&m[1]))){vkDestroyShaderModule(device,m[0],nullptr);fail(what,r);return false;}
    VkPipelineShaderStageCreateInfo st[2]{};for(int k=0;k<2;k++){st[k].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;st[k].stage=k?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT;st[k].module=m[k];st[k].pName="main";}
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;rs.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba{};ba.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};cb.attachmentCount=1;cb.pAttachments=&ba;
    VkDynamicState dyn[2]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};ds.dynamicStateCount=2;ds.pDynamicStates=dyn;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};gp.stageCount=2;gp.pStages=st;gp.pVertexInputState=&vi;gp.pInputAssemblyState=&ia;gp.pViewportState=&vp;gp.pRasterizationState=&rs;
    gp.pMultisampleState=&ms;gp.pColorBlendState=&cb;gp.pDynamicState=&ds;gp.layout=layout;gp.renderPass=pass;gp.subpass=0;
    r=vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,nullptr,&out);vkDestroyShaderModule(device,m[0],nullptr);vkDestroyShaderModule(device,m[1],nullptr);
    if(r){out={};fail(what,r);return false;}return true;
}
// The passes and pipelines the current mode needs, for the swapchain's format (all made again when the format changes).
bool make_pipelines(VkFormat format){
    if(format!=target_format)drop_pipelines();
    if(!pass_swap && !make_pass(format,true,pass_swap))return false;
    target_format=format;
    if(mode==GSR)return make_pipeline(pass_swap,gsr_code,sizeof gsr_code,"GSR shader / pipeline",pipe_gsr);
    if(!make_pipeline(pass_swap,easu_code,sizeof easu_code,"FSR EASU shader / pipeline",pipe_easu))return false;
    if(!rcas::enabled())return true;
    if(!pass_mid && !make_pass(VK_FORMAT_R8G8B8A8_UNORM,false,pass_mid))return false;
    return make_pipeline(pass_mid,easu_code,sizeof easu_code,"FSR EASU (intermediate) pipeline",pipe_easu_mid) && make_pipeline(pass_swap,rcas_code,sizeof rcas_code,"FSR RCAS shader / pipeline",pipe_rcas);
}
// After a swapchain (re)creation: one view + framebuffer per image (create_swap; the old ones went with destroy_present_semaphores).
bool targets(const std::vector<VkImage> &imgs,VkFormat format,VkExtent2D ext){
    drop_targets();if(!enabled())return false;
    if(imgs.empty()){fail("swapchain colour-attachment usage",0);return false;}
    if(!setup() || !make_pipelines(format))return false;VkResult r;
    for(VkImage im:imgs){
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=im;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkImageView v{};if((r=vkCreateImageView(device,&vi,nullptr,&v))){drop_targets();fail("swapchain image view",r);return false;}views.push_back(v);
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fi.renderPass=pass_swap;fi.attachmentCount=1;fi.pAttachments=&v;fi.width=ext.width;fi.height=ext.height;fi.layers=1;
        VkFramebuffer fb{};if((r=vkCreateFramebuffer(device,&fi,nullptr,&fb))){drop_targets();fail("framebuffer",r);return false;}framebuffers.push_back(fb);
    }
    fb_extent=ext;__android_log_print(ANDROID_LOG_INFO,"dm-vulkan","NVN present upscale %s targets: %zu swapchain images %ux%u format %d%s",name(mode),imgs.size(),ext.width,ext.height,int(format),
        mode==FSR && rcas::enabled()?", EASU into an intermediate image + RCAS (DM_GFX_SHARPEN)":mode==GSR && rcas::enabled()?", RCAS in the same pass (DM_GFX_SHARPEN)":"");
    return true;
}
// FSR + RCAS: EASU's output for flight f, the swapchain's size (RGBA8: colour attachment + sampled, on every Vulkan device).
bool mid_ensure(unsigned f,uint32_t w,uint32_t h){
    Mid &m=mid[f];if(m.image && m.w==w && m.h==h)return true;
    if(m.fb)vkDestroyFramebuffer(device,m.fb,nullptr);if(m.view)vkDestroyImageView(device,m.view,nullptr);if(m.image)vkDestroyImage(device,m.image,nullptr);if(m.memory)vkFreeMemory(device,m.memory,nullptr);m=Mid{};   // flight f's last use has retired (its fence was waited)
    VkResult r;VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ic.imageType=VK_IMAGE_TYPE_2D;ic.format=VK_FORMAT_R8G8B8A8_UNORM;ic.extent={w,h,1};ic.mipLevels=ic.arrayLayers=1;ic.samples=VK_SAMPLE_COUNT_1_BIT;
    ic.tiling=VK_IMAGE_TILING_OPTIMAL;ic.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;if((r=vkCreateImage(device,&ic,nullptr,&m.image))){m.image={};fail("intermediate image",r);return false;}
    VkMemoryRequirements mr;vkGetImageMemoryRequirements(device,m.image,&mr);VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ma.allocationSize=mr.size;ma.memoryTypeIndex=rcas::memory_type(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if((r=vkAllocateMemory(device,&ma,nullptr,&m.memory)) || (r=vkBindImageMemory(device,m.image,m.memory,0))){fail("intermediate memory",r);return false;}
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=m.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=ic.format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    if((r=vkCreateImageView(device,&vi,nullptr,&m.view))){m.view={};fail("intermediate view",r);return false;}
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fi.renderPass=pass_mid;fi.attachmentCount=1;fi.pAttachments=&m.view;fi.width=w;fi.height=h;fi.layers=1;
    if((r=vkCreateFramebuffer(device,&fi,nullptr,&m.fb))){m.fb={};fail("intermediate framebuffer",r);return false;}
    m.w=w;m.h=h;return true;
}
// Once the flight's fence has signalled: its two timestamps; every ~3 s the average / max GPU time of the pass.
void collect(unsigned f){
    if(!stamped[f])return;stamped[f]=false;uint64_t t[2]{};
    if(!queries || vkGetQueryPoolResults(device,queries,f*2,2,sizeof t,t,8,VK_QUERY_RESULT_64_BIT)!=VK_SUCCESS || t[1]<t[0])return;
    const double ns=double(t[1]-t[0])*tick_ns;sum_ns+=ns;max_ns=std::max(max_ns,ns);++timed;
    const int64_t now=mono_ns();if(!window_t0)window_t0=now;
    if(now-window_t0<3000000000ll)return;
    __android_log_print(ANDROID_LOG_INFO,"dm-vulkan","NVN present upscale %s %dx%d -> %dx%d gpu_us=%.0f max_us=%.0f presents=%u rcas=%s",name(mode),last_sw,last_sh,last_dw,last_dh,sum_ns/timed/1e3,max_ns/1e3,timed,rcas::enabled()?"on":"off");
    sum_ns=max_ns=0;timed=0;window_t0=now;
}
void viewport(VkCommandBuffer cmd,uint32_t w,uint32_t h){VkViewport v{0,0,float(w),float(h),0,1};VkRect2D sc{{0,0},{w,h}};vkCmdSetViewport(cmd,0,1,&v);vkCmdSetScissor(cmd,0,1,&sc);}
void source_view(unsigned f,VkImage source,VkFormat format,bool &ok){
    if(src_views[f])vkDestroyImageView(device,src_views[f],nullptr);src_views[f]={};   // this flight's previous frame has retired (its fence was waited)
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=source;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    if(VkResult r=vkCreateImageView(device,&vi,nullptr,&src_views[f])){src_views[f]={};fail("source view",r);ok=false;}
}
void bind_image(VkDescriptorSet set,VkImageView view){
    VkDescriptorImageInfo ii{sampler,view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};wr.dstSet=set;wr.dstBinding=0;wr.descriptorCount=1;wr.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;wr.pImageInfo=&ii;vkUpdateDescriptorSets(device,1,&wr,0,nullptr);
}
uint32_t fbits(float v){uint32_t u;memcpy(&u,&v,4);return u;}
// Records the pass into swapchain image `index` (the whole image: the SurfaceView has the picture's shape). The crop (x,y,w,h) of the
// frame is scaled to it; the source is sampled in SHADER_READ_ONLY_OPTIMAL and returned to TRANSFER_SRC_OPTIMAL (the layout the engine
// tracks). false = nothing recorded (the caller blits).
bool record(VkCommandBuffer cmd,unsigned f,uint32_t index,VkImage source,VkFormat format,uint32_t width,uint32_t height,int x,int y,int w,int h,VkExtent2D ext){
    if(!enabled() || index>=framebuffers.size() || fb_extent.width!=ext.width || fb_extent.height!=ext.height)return false;
    const bool sharpen=mode==FSR && rcas::enabled();
    if(mode==GSR?!pipe_gsr:(!pipe_easu || (sharpen && (!pipe_easu_mid || !pipe_rcas))))return false;
    if(sharpen && !mid_ensure(f,ext.width,ext.height))return false;
    bool ok=true;source_view(f,source,format,ok);if(!ok)return false;
    bind_image(sets[f][0],src_views[f]);if(sharpen)bind_image(sets[f][1],mid[f].view);
    if(queries){vkCmdResetQueryPool(cmd,queries,f*2,2);vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,queries,f*2);}
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=source;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rb.renderArea={{0,0},ext};
    if(mode==GSR){
        rb.renderPass=pass_swap;rb.framebuffer=framebuffers[index];vkCmdBeginRenderPass(cmd,&rb,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe_gsr);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&sets[f][0],0,nullptr);viewport(cmd,ext.width,ext.height);
        // source position p = gl_FragCoord * scale + offset (pixels; gsr.frag)
        const double sx=double(w)/ext.width,sy=double(h)/ext.height;
        struct{float sx,sy,ox,oy,tx,ty,rcas,edge;}pc{float(sx),float(sy),float(x),float(y),1.f/float(width),1.f/float(height),rcas::enabled()?rcas::sharpness:0.f,edge_sharpness};
        vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof pc,&pc);vkCmdDraw(cmd,3,1,0,0);vkCmdEndRenderPass(cmd);
    }else{
        // FsrEasuConOffset (ffx_fsr1.h): viewport w x h at (x,y) of the width x height source, output ext
        const float ivx=float(w),ivy=float(h),isx=float(width),isy=float(height),ox=float(ext.width),oy=float(ext.height);
        const uint32_t con[16]={fbits(ivx/ox),fbits(ivy/oy),fbits(0.5f*ivx/ox-0.5f+float(x)),fbits(0.5f*ivy/oy-0.5f+float(y)),
            fbits(1.f/isx),fbits(1.f/isy),fbits(1.f/isx),fbits(-1.f/isy),fbits(-1.f/isx),fbits(2.f/isy),fbits(1.f/isx),fbits(2.f/isy),fbits(0.f),fbits(4.f/isy),0,0};
        rb.renderPass=sharpen?pass_mid:pass_swap;rb.framebuffer=sharpen?mid[f].fb:framebuffers[index];vkCmdBeginRenderPass(cmd,&rb,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,sharpen?pipe_easu_mid:pipe_easu);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&sets[f][0],0,nullptr);viewport(cmd,ext.width,ext.height);
        vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof con,con);vkCmdDraw(cmd,3,1,0,0);vkCmdEndRenderPass(cmd);
        if(sharpen){   // FsrRcasCon: con0.x = exp2(-stops)
            const uint32_t rc[4]={fbits(rcas::sharpness),0,0,0};
            rb.renderPass=pass_swap;rb.framebuffer=framebuffers[index];vkCmdBeginRenderPass(cmd,&rb,VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe_rcas);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&sets[f][1],0,nullptr);viewport(cmd,ext.width,ext.height);
            vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof rc,rc);vkCmdDraw(cmd,3,1,0,0);vkCmdEndRenderPass(cmd);
        }
    }
    // back to the engine's layout; later writes to the source wait for these reads
    b.oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b.srcAccessMask=0;b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
    if(queries){vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries,f*2+1);stamped[f]=true;}
    last_sw=w;last_sh=h;last_dw=int(ext.width);last_dh=int(ext.height);return true;
}
}
extern "C" void dm_present_set_upscale(int m){upscale::requested.store(m<0||m>3?0:m,std::memory_order_relaxed);}   // [D3] gear "Upscale", live: 0 off, 1 auto, 2 gsr, 3 fsr
#if defined(DM_GAME_D3) || defined(DM_GAME_TL2)
// [D3] / [TL2] hle_d3_icons.c (hle_tl2_icons.c): the HUD skill slots' icon rectangles (source pixels) to copy out of this frame, and the
// copied pixels after the frame's fence. A host-visible buffer per flight; the copy is recorded into the present's own command buffer (the
// source is already in TRANSFER_SRC_OPTIMAL for the blit), so it adds no wait; nothing is recorded unless a batch is due (at most 4 a second).
extern "C" int (*dm_present_icon_want)(int32_t,int32_t,int32_t,int32_t,int32_t(*)[4],int);
extern "C" void (*dm_present_icon_done)(int,const int32_t(*)[4],const uint8_t*,int);
namespace icons {
enum{MAXN=8,MAXPX=160};
VkBuffer buf[2]{};VkDeviceMemory mem[2]{};void *map[2]{};bool coherent[2]{},pend[2]{};int n[2]{},bgra[2]{};int32_t rect[2][MAXN][4];
bool ensure(unsigned f){
    if(buf[f])return true;
    VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bc.size=VkDeviceSize(MAXN)*MAXPX*MAXPX*4;bc.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;if(vkCreateBuffer(device,&bc,nullptr,&buf[f]))return buf[f]=VK_NULL_HANDLE,false;
    VkMemoryRequirements mr;vkGetBufferMemoryRequirements(device,buf[f],&mr);VkPhysicalDeviceMemoryProperties mp;vkGetPhysicalDeviceMemoryProperties(physical,&mp);
    int pick=-1;for(int pass=0;pass<2 && pick<0;pass++)for(uint32_t i=0;i<mp.memoryTypeCount;i++){VkMemoryPropertyFlags fl=mp.memoryTypes[i].propertyFlags;
        VkMemoryPropertyFlags want=pass?VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT:VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        if((mr.memoryTypeBits&(1u<<i)) && (fl&want)==want){pick=int(i);coherent[f]=(fl&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;break;}}
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ma.allocationSize=mr.size;ma.memoryTypeIndex=uint32_t(pick<0?0:pick);
    if(pick<0||vkAllocateMemory(device,&ma,nullptr,&mem[f])||vkBindBufferMemory(device,buf[f],mem[f],0)||vkMapMemory(device,mem[f],0,VK_WHOLE_SIZE,0,&map[f])){
        __android_log_print(ANDROID_LOG_WARN,"dm-vulkan","HUD icon readback buffer failed: no button icons");vkDestroyBuffer(device,buf[f],nullptr);buf[f]=VK_NULL_HANDLE;return false;}
    return true;
}
void collect(unsigned f){
    if(!pend[f])return;pend[f]=false;
    if(!coherent[f]){VkMappedMemoryRange mr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};mr.memory=mem[f];mr.size=VK_WHOLE_SIZE;vkInvalidateMappedMemoryRanges(device,1,&mr);}
    if(dm_present_icon_done)dm_present_icon_done(n[f],rect[f],static_cast<const uint8_t*>(map[f]),bgra[f]);
}
void record(VkCommandBuffer cmd,unsigned f,bool fence_idle,VkImage source,VkFormat format,int32_t x,int32_t y,int32_t w,int32_t h){
    if(!dm_present_icon_want || (format!=VK_FORMAT_R8G8B8A8_UNORM && format!=VK_FORMAT_B8G8R8A8_UNORM && format!=VK_FORMAT_R8G8B8A8_SRGB && format!=VK_FORMAT_B8G8R8A8_SRGB))return;
    if(pend[f] && fence_idle)collect(f);   // a batch whose fence was retired another way (swapchain rebuild drains the queue)
    if(pend[f])return;
    int32_t r[MAXN][4];int k=dm_present_icon_want(x,y,w,h,r,MAXN);if(k<=0 || !ensure(f))return;
    VkBufferImageCopy c[MAXN]{};VkDeviceSize off=0;
    for(int i=0;i<k;i++){c[i].bufferOffset=off;c[i].imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c[i].imageOffset={r[i][0],r[i][1],0};c[i].imageExtent={uint32_t(r[i][2]),uint32_t(r[i][3]),1};
        off+=VkDeviceSize(r[i][2])*r[i][3]*4;memcpy(rect[f][i],r[i],sizeof r[i]);}
    vkCmdCopyImageToBuffer(cmd,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buf[f],uint32_t(k),c);
    VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_HOST_READ_BIT;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.buffer=buf[f];b.size=VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&b,0,nullptr);
    n[f]=k;bgra[f]=format==VK_FORMAT_B8G8R8A8_UNORM||format==VK_FORMAT_B8G8R8A8_SRGB;pend[f]=true;
}
}
#endif
int present_body(VkImage source,VkFormat source_format,uint32_t width,uint32_t height,const int32_t crop[4]);
int present(void*,void*,VkImage source,VkFormat source_format,uint32_t width,uint32_t height,const int32_t crop[4],int32_t){
    std::lock_guard<std::mutex> render_lock(render_mutex);
    pin_update();
    return present_body(source,source_format,width,height,crop);
}
// [present-thread] The present itself; the caller holds render_mutex (present(), the present thread, or DM_GFX_PRESENT_THREAD=2 inline).
int present_body(VkImage source,VkFormat source_format,uint32_t width,uint32_t height,const int32_t crop[4]){
    VkFormatProperties sp;vkGetPhysicalDeviceFormatProperties(physical,source_format,&sp);if(!(sp.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT))return VK_ERROR_FORMAT_NOT_SUPPORTED;
    int x=0,y=0,w=int(width),h=int(height);if(crop&&crop[2]>0&&crop[3]>0){x=std::clamp(crop[0],0,w);y=std::clamp(crop[1],0,h);w=std::min(crop[2],int(width)-x);h=std::min(crop[3],int(height)-y);}if(w<=0||h<=0)return VK_ERROR_INITIALIZATION_FAILED;
    {   // [D3] gear "Upscale" switched (live), or the pass failed: the next swapchain is made for the new mode
        const int m=upscale::resolve(upscale::requested.load(std::memory_order_relaxed));
        if(m!=upscale::mode){__android_log_print(ANDROID_LOG_INFO,"dm-vulkan","NVN present upscale %s -> %s (gear / dm_present_set_upscale)",upscale::name(upscale::mode),upscale::name(m));upscale::mode=m;if(swapchain)swap_dirty=true;}}
    for(;;){
    // [D3] gear "Upscale": the window's own size (0x0) for our pass; otherwise the game's output size, the compositor scales it
    const bool up_want=upscale::takes(source_format,sp.optimalTilingFeatures);const int gw=up_want?0:w,gh=up_want?0:h;
    if(present_native && surface && active && (native_w!=gw || native_h!=gh)){   // the next swapchain takes this size (currentExtent follows the buffer geometry)
        int g=ANativeWindow_setBuffersGeometry(active,gw,gh,0);native_w=gw;native_h=gh;swap_dirty=true;
        if(up_want)__android_log_print(ANDROID_LOG_INFO,"dm-vulkan","Present upscale %s: the game's output %dx%d into window-size buffers result=%d (gear Upscale / DM_GFX_UPSCALE)",upscale::name(upscale::mode),w,h,g);
        else __android_log_print(ANDROID_LOG_INFO,"dm-vulkan","Present at the game's output size %dx%d result=%d (marker gfx-present-native)",w,h,g);}
    // 后台等待释放UI锁；保留同一源帧，恢复并真正呈现后才返回成功。
    // Suspend without the UI lock; keep this source frame until it is presented.
    wait_window();
    VkResult r;
    if(!surface||window_changed()){
        if((r=vkQueueWaitIdle(queue)))return r;
        r=VkResult(create_surface());
        if(r==VK_NOT_READY)continue;
        if(r==VK_ERROR_SURFACE_LOST_KHR){uint64_t bad=active_generation;destroy_swap();wait_window_change(bad);continue;}
        if(r)return r;swap_dirty=true;
    }
    if(swap_dirty){
        if((r=vkQueueWaitIdle(queue)))return r;
        r=VkResult(create_swap());
        if(r==VK_NOT_READY||r==VK_ERROR_OUT_OF_DATE_KHR){wait_resize();continue;}
        if(r==VK_ERROR_SURFACE_LOST_KHR){uint64_t bad=active_generation;destroy_swap();wait_window_change(bad);continue;}
        if(r)return r;swap_dirty=false;
    }
    VkFormatProperties dp;vkGetPhysicalDeviceFormatProperties(physical,swap_format,&dp);if(!(dp.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT))return VK_ERROR_FORMAT_NOT_SUPPORTED;
    // 有限获取超时让渲染线程重新检查Surface代数，UI从不等待驱动。
    // A bounded acquire lets rendering observe lifecycle changes; UI never waits on the driver.
    unsigned f=overlap?flight:0;VkCommandPool pool=f?overlap_pool:command_pool;VkCommandBuffer cmd=f?overlap_command:command;VkSemaphore acq=f?overlap_acquired:acquired;VkFence done=f?overlap_fence:fence;
    if(in_flight[f]){XTRACE_BEGIN("dm-present-fence");r=vkWaitForFences(device,1,&done,VK_TRUE,10000000000ull);XTRACE_END();if(r)return r;if((r=vkResetFences(device,1,&done)))return r;in_flight[f]=false;
#if defined(DM_GAME_D3) || defined(DM_GAME_TL2)
        icons::collect(f);
#endif
        upscale::collect(f);   // [D3] the pass's timestamps of this flight's previous present
        if(present_stamped[f]){uint64_t t[2]{};if(vkGetQueryPoolResults(device,present_queries,f*2,2,sizeof t,t,8,VK_QUERY_RESULT_64_BIT)==VK_SUCCESS && t[1]>=t[0]){present_gpu_ns+=double(t[1]-t[0])*present_tick_ns;
            if(++present_timed==300){__android_log_print(ANDROID_LOG_INFO,"dm-vulkan","NVN PERF present_gpu_us=%.0f extent=%ux%u (marker gfx-present-timing)",present_gpu_ns/present_timed/1e3,extent.width,extent.height);present_gpu_ns=0;present_timed=0;}}present_stamped[f]=false;}}
    uint32_t index=0;XTRACE_BEGIN("dm-acquire");r=vkAcquireNextImageKHR(device,swapchain,50000000ull,acq,VK_NULL_HANDLE,&index);XTRACE_END();
    if(r==VK_TIMEOUT||r==VK_NOT_READY)continue;
    if(r==VK_ERROR_OUT_OF_DATE_KHR){swap_dirty=true;wait_resize();continue;}
    if(r==VK_ERROR_SURFACE_LOST_KHR){if((r=vkQueueWaitIdle(queue)))return r;uint64_t bad=active_generation;destroy_swap();wait_window_change(bad);continue;}
    bool rebuild=r==VK_SUBOPTIMAL_KHR;if(r!=VK_SUCCESS&&!rebuild)return r;
    r=vkResetCommandPool(device,pool,0);if(r)return r;VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;if((r=vkBeginCommandBuffer(cmd,&bi)))return r;
    bool stamp=present_timing && overlap;if(stamp && !present_queries){VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};q.queryType=VK_QUERY_TYPE_TIMESTAMP;q.queryCount=4;if(vkCreateQueryPool(device,&q,nullptr,&present_queries))stamp=false;
        VkPhysicalDeviceProperties pp;vkGetPhysicalDeviceProperties(physical,&pp);present_tick_ns=pp.limits.timestampPeriod;}
    if(stamp){vkCmdResetQueryPool(cmd,present_queries,f*2,2);vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,present_queries,f*2);}
#if defined(DM_GAME_D3) || defined(DM_GAME_TL2)
    icons::record(cmd,f,!in_flight[f],source,source_format,x,y,w,h);   // the source is in TRANSFER_SRC_OPTIMAL (before rcas / the upscale pass, which return it there)
#endif
    // [D3] gear "Upscale": the pass renders the whole swapchain image itself (crop, scale, RCAS); only when it enlarges
    const bool upscaled=up_want && (int(extent.width)>w || int(extent.height)>h) && upscale::record(cmd,f,index,source,source_format,width,height,x,y,w,h,extent);
    if(!upscaled){
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=images[index];barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    // 换布局须在获取信号量的等待阶段（下面提交的 TRANSFER）之后：源阶段写 TOP_OF_PIPE 时，换布局不等信号量，可能在呈现引擎
    // 读完这张图之前就发生（验证层同步检查：SYNC-HAZARD-WRITE-AFTER-READ）。
    // The layout transition must come after the acquire semaphore's wait stage (TRANSFER, in the submit below). With
    // TOP_OF_PIPE as the source stage it does not wait for the semaphore and can happen before the presentation engine has
    // finished reading the image (synchronization validation: SYNC-HAZARD-WRITE-AFTER-READ).
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    double scale=std::min(double(extent.width)/w,double(extent.height)/h);int dw=std::max(1,int(w*scale)),dh=std::max(1,int(h*scale)),dx=(int(extent.width)-dw)/2,dy=(int(extent.height)-dh)/2;
    if(up_want){dx=dy=0;dw=int(extent.width);dh=int(extent.height);}   // [D3] window-size buffers without the pass: fill the SurfaceView as the compositor did
    if(dx || dy || dw!=int(extent.width) || dh!=int(extent.height)){   // letterbox bars only where the blit leaves part of the image
        VkClearColorValue black{};vkCmdClearColorImage(cmd,images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&barrier.subresourceRange);
        barrier.oldLayout=barrier.newLayout;barrier.srcAccessMask=barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);}
    VkImageBlit blit{};blit.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};blit.dstSubresource=blit.srcSubresource;blit.srcOffsets[0]={x,y,0};blit.srcOffsets[1]={x+w,y+h,1};blit.dstOffsets[0]={dx,dy,0};blit.dstOffsets[1]={dx+dw,dy+dh,1};
    // 最近邻不要求额外线性过滤能力。鸿蒙：游戏出图比屏幕小、要放大，格式支持时用线性，不然像素发硬、斜边锯齿。
    // Nearest needs no extra linear-filter support. HarmonyOS: the game image is smaller than the screen and gets upscaled; use linear when the format supports it, otherwise pixels look blocky and diagonals jagged.
    VkFilter filter=VK_FILTER_NEAREST;
#ifdef __OHOS__
    if(sp.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)filter=VK_FILTER_LINEAR;
#endif
    if(up_want && (dw!=w || dh!=h) && (sp.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))filter=VK_FILTER_LINEAR;   // [D3] the pass did not run: scale like the compositor
    const bool sharpened=rcas::record(cmd,f,source,source_format,width,height);
    vkCmdBlitImage(cmd,sharpened?rcas::image:source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,filter);
    // 同队列后续写入必须等源图读取完成；即使下一次图像布局未改变也建立 WAR 执行依赖。
    // Order subsequent same-queue writes after the source read, including reuse without a layout change.
    VkMemoryBarrier source_read{VK_STRUCTURE_TYPE_MEMORY_BARRIER};source_read.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;source_read.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&source_read,0,nullptr,0,nullptr);
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=0;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    }
    if(stamp){vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,present_queries,f*2+1);present_stamped[f]=true;}
    // the acquire semaphore gates the first write to the swapchain image: the blit (TRANSFER) or the upscale pass's attachment
    if((r=vkEndCommandBuffer(cmd)))return r;VkPipelineStageFlags stage=upscaled?VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT:VK_PIPELINE_STAGE_TRANSFER_BIT;VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&acq;submit.pWaitDstStageMask=&stage;submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&finished[index];
    if((r=vkQueueSubmit(queue,1,&submit,done)))return r;in_flight[f]=true;VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};pi.waitSemaphoreCount=1;pi.pWaitSemaphores=&finished[index];pi.swapchainCount=1;pi.pSwapchains=&swapchain;pi.pImageIndices=&index;
#ifdef __OHOS__
    // 期望上屏时刻（鸿蒙 SET_DESIRED_PRESENT_TIMESTAMP）：上屏节奏 = 上次期望时刻 + 一个出图档周期；早到的帧由合成器按时放出，翻译线程不等；
    // 落后时从当前时刻重新起拍。翻译完成时刻的抖动（±5 ms）不再让一帧在屏上停 3 或 5 个刷新。DM_PRESENT_TS=0 关。
    // Desired display time (HarmonyOS SET_DESIRED_PRESENT_TIMESTAMP): cadence = previous desired time + one present-tier period; an early frame is released on time by the compositor
    // without the translation thread waiting; when late, restart the cadence from now. Jitter in translation completion (±5 ms) no longer leaves a frame on screen for 3 or 5 refreshes.
    {static int on=-1;if(on<0){const char *e=getenv("DM_PRESENT_TS");on=!(e&&*e=='0');}static int64_t tgt;double per=dm_present_period();
     if(on&&per>0&&active){timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);int64_t now=int64_t(ts.tv_sec)*1000000000+ts.tv_nsec,p=int64_t(per*1e9),next=tgt?tgt+p:now;
      if(tgt&&now>tgt+p+2000000)dm_present_late.fetch_add(1,std::memory_order_relaxed);if(next<now)next=now;if(next>now+2*p)next=now+p;tgt=next;OH_NativeWindow_NativeWindowHandleOpt(active,SET_DESIRED_PRESENT_TIMESTAMP,next);}}
#endif
#ifdef DM_GAME_DOOM
    dm_aperf_present(active,frame_rate_vote);   // ADPF work report + display rate vote (perf_android.c)
#endif
    dm_gpu_floor_present(physical,device,queue);   // [gpu-floor] PNH-C01: the floor while frames run late (DM_GPU_FLOOR=auto|1|0), at every present
    {int a=0;const char *w=dm_gpu_floor_report(&a);if(w)pc_note("gpu-floor",a,"%s; PNH-C01",w);}   // [pc]
    XTRACE_BEGIN("dm-queue-present");VkResult pr=vkQueuePresentKHR(queue,&pi);XTRACE_END();
    // 呈现失败时先排空，拆除路径要求队列空闲。
    // Drain a failed present before teardown, which requires an idle queue.
    if(overlap){flight^=1;if(pr!=VK_SUCCESS && pr!=VK_SUBOPTIMAL_KHR && (r=vkQueueWaitIdle(queue)))return r;}
    else{if((r=vkWaitForFences(device,1,&done,VK_TRUE,10000000000ull)))return r;if((r=vkResetFences(device,1,&done)))return r;in_flight[0]=false;if((r=vkQueueWaitIdle(queue)))return r;
#if defined(DM_GAME_D3) || defined(DM_GAME_TL2)
        icons::collect(0);
#endif
        upscale::collect(0);
    }
    if(pr==VK_ERROR_SURFACE_LOST_KHR){uint64_t bad=active_generation;destroy_swap();wait_window_change(bad);continue;}
    if(pr==VK_ERROR_OUT_OF_DATE_KHR){swap_dirty=true;wait_resize();continue;}
    if(pr!=VK_SUCCESS&&pr!=VK_SUBOPTIMAL_KHR)return pr;
    // 已成功呈现的帧不因尺寸瞬变报错；下帧再重建或等待可用尺寸。
    // Defer suboptimal recreation to the next frame, which can wait for a usable extent.
    if((rebuild||pr==VK_SUBOPTIMAL_KHR)&&surface_properties_changed())swap_dirty=true;
    return 0;
    }
}
}
// [present-thread] PERF-D07 DM_GFX_PRESENT_THREAD (DOOM 2016 vulkan_present.cpp pt::loop; port-common/modules/present-thread). The engine
// reads the switch and calls dm_present_thread_set. 1: the engine hands each present (source image, format, size, crop) to the present
// thread, which runs the same present_body as before: wait for the flight's fence, acquire, letterbox clear, RCAS, blit, submit, queue
// present, swapchain / surface recreation, the window waits while the app is in the background, the ADPF report and frame-rate vote
// (dm_aperf_present). Order on the one VkQueue: the engine waits until every handed present has its blit submitted before it submits
// anything (dm_present_wait_submitted), and every queue call of both threads takes the queue lock, so the queue sees the same sequence as
// presenting inline. The translation thread keeps its pin (pin_update at the hand-off). 2: the same protocol (queue lock) run inline on
// the caller, no thread. 0 / unset: none of this runs and the queue lock is a no-op.
// The present thread is the ADPF session's first thread (it calls dm_aperf_present; perf_android.c makes the caller a member); the
// translation thread joins it by load like the game's other busy threads. Its cores: every core but the translation thread's, re-checked
// every 128 presents (it is created by the pinned translation thread and inherits that pin: DOOM 2016's DM_GFX_PRESENT_UNPIN lesson).
#if __has_include("../host/cpu_topo.h")
#include "../host/cpu_topo.h"   // [cpu-place] where the module is in: its plan's present mask
extern "C" const dm_cpu_plan_t *dm_cpu_plan(void) __attribute__((weak));
#define PT_HAVE_PLAN 1
#endif
#include <pthread.h>
#include <sys/resource.h>
#include <thread>
namespace pt {
struct Job{VkImage src;VkFormat fmt;uint32_t w,h;int32_t crop[4];bool has_crop;};
dmpt::Handoff<Job,4> &q=*new dmpt::Handoff<Job,4>();   // never destroyed: no exit-time destructor under a thread that waits on it
thread_local uint64_t last_ticket=0;   // the producer's latest hand-off (nvn_vk.c maps its present number to it)
std::atomic<int> translator_tid{0};std::atomic<uint64_t> inline_jobs{0};
unsigned cpus_of(int tid){cpu_set_t s;CPU_ZERO(&s);if(sched_getaffinity(tid,sizeof s,&s))return 0;unsigned m=0;for(int c=0;c<32;c++)if(CPU_ISSET(c,&s))m|=1u<<c;return m;}
// Every 128 presents: the wanted mask from DM_GFX_PRESENT_CPUS (hex), the cpu-place plan, or every core but the translation thread's.
void place(){
    static int ncpu=0;static unsigned all=0,khz[32];static unsigned placed=0,changes=0;
    if(!ncpu){long n=sysconf(_SC_NPROCESSORS_CONF);ncpu=int(n<1?8:n>32?32:n);all=ncpu>=32?~0u:(1u<<ncpu)-1;
        for(int c=0;c<ncpu;c++){char path[96];snprintf(path,sizeof path,"/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq",c);khz[c]=0;if(FILE *f=fopen(path,"rb")){if(fscanf(f,"%u",&khz[c])!=1)khz[c]=0;fclose(f);}}}
    const char *u=getenv("DM_GFX_PRESENT_UNPIN");if(u && *u=='0')return;   // DOOM 2016's switch: =0 keeps the inherited mask
    unsigned want=0,avoid=0;const char *why="";
    if(const char *e=getenv("DM_GFX_PRESENT_CPUS");e && *e){want=unsigned(strtoul(e,nullptr,16))&all;why="DM_GFX_PRESENT_CPUS";}
#ifdef PT_HAVE_PLAN
    if(!want && dm_cpu_plan){const dm_cpu_plan_t *p=dm_cpu_plan();if(p && p->present){want=p->present&all;why="cpu plan";}}
#endif
    if(!want){const int t=translator_tid.load(std::memory_order_relaxed);avoid=pin_mask?pin_mask:t?cpus_of(t):0;want=dmpt::place_mask(all,avoid,khz,ncpu);why=pin_mask?"all but the pin":"all but the translation thread's cores";}
    if(!want)return;
    const unsigned now=cpus_of(0);if(now==want && placed==want)return;
    cpu_set_t s;CPU_ZERO(&s);for(int c=0;c<32;c++)if(want>>c&1)CPU_SET(c,&s);const int r=sched_setaffinity(0,sizeof s,&s),err=r?errno:0;
    if(placed!=want || ++changes<=4)__android_log_print(ANDROID_LOG_INFO,"dm-vulkan","NVN present thread tid=%d cpus %x -> %x (%s; translation thread %d on %x) result=%d errno=%d",
        int(gettid()),now,want,why,translator_tid.load(std::memory_order_relaxed),avoid,r,err);
    placed=want;
}
void loop(){
    pt_on_thread=true;pthread_setname_np(pthread_self(),"dm-present");
    int pr=setpriority(PRIO_PROCESS,0,-8);if(pr)pr=setpriority(PRIO_PROCESS,0,-4);
    __android_log_print(ANDROID_LOG_INFO,"dm-vulkan","NVN present thread started tid=%d nice=%d (setpriority %d) inherited cpus=%x",int(gettid()),getpriority(PRIO_PROCESS,0),pr,cpus_of(0));
    place();
    for(unsigned n=1;;n++){Job j;uint64_t tk;if(!q.take(j,tk))return;
        const int64_t t0=dmpt::now_ns();int r;{std::lock_guard<std::mutex> rl(render_mutex);r=present_body(j.src,j.fmt,j.w,j.h,j.has_crop?j.crop:nullptr);}
        q.finish(tk,r,uint64_t(dmpt::now_ns()-t0));
        if(r){static int told=0;if(told++<4)__android_log_print(ANDROID_LOG_ERROR,"dm-vulkan","NVN present thread: present %llu failed: VkResult %d",(unsigned long long)tk,r);}
        if(n%128==0)place();}
}
}
static void pt_mark_submitted(void){pt::q.mark_submitted();}
static void pt_stamp_present(void){pt::q.stamp(dmpt::now_ns());}
extern "C" void pc_note(const char *id,int applied,const char *fmt,...);   // [pc]
extern "C" void dm_present_thread_set(int mode){pt_cfg.store(mode>0 && mode<=2?mode:0);if(mode>0)__android_log_print(ANDROID_LOG_INFO,"dm-vulkan","NVN present thread mode=%d (DM_GFX_PRESENT_THREAD: 1 own thread, 2 same protocol inline)",pt_cfg.load());
    pc_note("present-thread",pt_cfg.load()==1,"%s",pt_cfg.load()==1?"DM_GFX_PRESENT_THREAD=1: fence / acquire / blit / queue present on dm-present; PERF-D07":pt_cfg.load()==2?"DM_GFX_PRESENT_THREAD=2: the protocol inline, no thread":"DM_GFX_PRESENT_THREAD=0: presents inline");   // [pc]
}
// Returns 0, or the VkResult of an earlier failed present on the thread (the engine stops with it, as it did for an inline failure).
extern "C" int dm_present_async(VkImage source,VkFormat format,uint32_t width,uint32_t height,const int32_t crop[4],int32_t){
    if(pt_cfg.load(std::memory_order_relaxed)!=1){std::lock_guard<std::mutex> render_lock(render_mutex);pin_update();++pt::inline_jobs;pt::last_ticket=0;return present_body(source,format,width,height,crop);}
    pin_update();   // the translation thread (the caller) keeps its pin, as present() pinned it
    {const int t=int(gettid());if(pt::translator_tid.load(std::memory_order_relaxed)!=t)pt::translator_tid.store(t,std::memory_order_relaxed);}
    pt::Job j{source,format,width,height,{0,0,0,0},crop!=nullptr};if(crop)memcpy(j.crop,crop,sizeof j.crop);
    uint64_t tk=0;XTRACE_BEGIN("dm-present-handoff");const int e=pt::q.push(j,[]{std::thread(pt::loop).detach();},&tk);XTRACE_END();
    pt::last_ticket=e?0:tk;return e;
}
// Nanoseconds waited until every handed present has its blit submitted / has finished (0 = nothing pending).
extern "C" uint64_t dm_present_wait_submitted(void){return pt::q.wait_submitted();}
extern "C" uint64_t dm_present_drain(void){return pt::q.drain();}
extern "C" int dm_present_last_error(void){return pt::q.err.load();}
extern "C" void dm_present_queue_lock(void){if(pt_cfg.load(std::memory_order_relaxed))pt_queue_mutex.lock();}
extern "C" void dm_present_queue_unlock(void){if(pt_cfg.load(std::memory_order_relaxed))pt_queue_mutex.unlock();}
// Totals since start: presents run by the thread (or inline with =2), the thread's time in them, the engine's waits on a full hand-off ring
// (ns), and on a not yet submitted blit (ns, dm_present_wait_submitted).
extern "C" void dm_present_thread_stats(uint64_t out[4]){out[0]=pt::q.jobs.load()+pt::inline_jobs.load();out[1]=pt::q.busy_ns.load();out[2]=pt::q.full_ns.load();out[3]=pt::q.wait_ns.load();}
// For nvn_vk.c (mtl_present / mtl_present_wait / mtl_presented_time): the ticket of the present this thread just handed over (0 = presented
// inline), a wait until that present's blit is on the queue (the contract of mtl_present_wait), and when its vkQueuePresentKHR returned
// (CLOCK_MONOTONIC seconds; 0 = not yet).
extern "C" uint64_t dm_present_last_ticket(void){return pt_cfg.load(std::memory_order_relaxed)==1?pt::last_ticket:0;}
extern "C" uint64_t dm_present_wait_ticket(uint64_t tk){return tk && pt_cfg.load(std::memory_order_relaxed)==1?pt::q.wait_ticket(tk):0;}
extern "C" double dm_present_time(uint64_t tk){const int64_t ns=pt::q.presented_ns(tk);return ns?double(ns)*1e-9:0;}
extern "C" void dm_android_vk_set_window(ANativeWindow *window){if(window)ANativeWindow_acquire(window);{std::lock_guard<std::mutex> l(window_mutex);if(desired)ANativeWindow_release(desired);desired=window;++generation;}window_cv.notify_all();}
extern "C" int dm_android_vk_has_window(){return has_window(nullptr);}
// [DOOM] BUGS_AND_FIXES 2.56 (DOOM issue #33, KB DRV-07): the in-process fallback to the system driver (jni_android.c host_thread). A failed
// initialization destroys what it made (fail -> cleanup_initialization), so it may run again with another driver, as long as the engine was
// not initialized yet (dm_vk_initialize below keeps state of its own): dm_android_vk_retry_safe. DM_VK_FAKE_INIT_FAIL (test switch, dm/env):
// 1 = the initialization fails right after the swapchain was made, only while a custom driver is active (the same VkResult as PanVK on the
// Mali-G57 of DOOM issue #33), so the fallback can be checked on a phone where every driver works; 2 = it fails with any driver (the system
// driver too: the start error must be shown, no loop).
extern "C" int dm_vk_driver_custom(void);
static bool engine_touched=false;
extern "C" int dm_android_vk_retry_safe(void){return !engine_touched && !ready;}
static bool fake_init_fail(){const char *e=getenv("DM_VK_FAKE_INIT_FAIL");if(!e||!*e)return false;const int v=atoi(e);return v>=2 || (v==1 && dm_vk_driver_custom());}
extern "C" int dm_android_vk_initialize(const char *path,char *error,size_t bytes){
    if(path){std::string dir=path;size_t cut=dir.find_last_of('/');if(FILE *m=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-present-native").c_str(),"rb")){present_native=fgetc(m)!='0';fclose(m);}
        if(FILE *m=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-present-timing").c_str(),"rb")){present_timing=true;fclose(m);}}
    std::lock_guard<std::mutex> render_lock(render_mutex);
    auto fail=[&](const char *what,VkResult r)->int{if(error&&bytes)snprintf(error,bytes,"%s: VkResult %d",what,int(r));if(!ready)cleanup_initialization();return int(r?r:VK_ERROR_INITIALIZATION_FAILED);};
    if(ready||!path||!*path)return fail("Initialization requires an external shader directory and an uninitialized renderer",VK_ERROR_INITIALIZATION_FAILED);
    // 每次初始化从缺省的 120 Hz 重新读标记。 | Each initialization starts again from the 120 Hz default before the marker.
    frame_rate_vote=120;
    dm_vk_format_list=0;dm_vk_custom_border=0;dm_vk_descriptor_indexing=0;   // [DOOM] 2.56: a repeated initialization (driver fallback) starts over
    {std::string dir=path;size_t cut=dir.find_last_of('/');if(FILE *m=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-frame-rate").c_str(),"rb")){float v=0;frame_rate_vote=fscanf(m,"%f",&v)==1 && v>=0 && v<=240?v:0;fclose(m);}}

    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};ai.pApplicationName="dm-android";ai.apiVersion=VK_API_VERSION_1_1;
#ifdef DM_GAME_ETERNAL
    ai.apiVersion=VK_API_VERSION_1_2;   // [ETERNAL] descriptor indexing as core 1.2 where the device lacks the EXT name (LDPlayer)
#endif

    const char *ext[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,VK_EXT_DEBUG_UTILS_EXTENSION_NAME};VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ic.pApplicationInfo=&ai;ic.enabledExtensionCount=2;ic.ppEnabledExtensionNames=ext;
    // DM_VK_VALIDATION=1 (diagnostics): the Khronos validation layer packaged in the APK (tools/build_apk.ps1 -Validation; a
    // debuggable app's own layers), with synchronization validation and best practices; messages go to logcat "dm-vvl", each
    // message id at most 20 times. =2: core validation only (faster).
    const char *vvl_env=getenv("DM_VK_VALIDATION");int vvl=vvl_env?atoi(vvl_env):0;const char *layer="VK_LAYER_KHRONOS_validation";
    VkValidationFeatureEnableEXT vf_on[]={VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT};
    VkValidationFeaturesEXT vf{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};vf.enabledValidationFeatureCount=2;vf.pEnabledValidationFeatures=vf_on;
    if(vvl){auto enum_layers=(PFN_vkEnumerateInstanceLayerProperties)vkGetInstanceProcAddr(nullptr,"vkEnumerateInstanceLayerProperties");
        uint32_t ln=0;bool have=false;if(enum_layers){enum_layers(&ln,nullptr);std::vector<VkLayerProperties> lp(ln);enum_layers(&ln,lp.data());for(auto &l:lp)if(!strcmp(l.layerName,layer))have=true;}
        __android_log_print(ANDROID_LOG_INFO,"dm-vvl","validation layer %s (%u layers)",have?"found":"NOT FOUND",ln);
        if(have){ic.enabledLayerCount=1;ic.ppEnabledLayerNames=&layer;ic.enabledExtensionCount=3;if(vvl==1)ic.pNext=&vf;}else vvl=0;}
    VkResult r=vkCreateInstance(&ic,nullptr,&instance);if(r)return fail("vkCreateInstance",r);
    if(vvl){static std::mutex vm;static std::unordered_map<int32_t,unsigned> seen;
        VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mi.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
        mi.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mi.pfnUserCallback=[](VkDebugUtilsMessageSeverityFlagBitsEXT sev,VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT *d,void*)->VkBool32{
            unsigned c;{std::lock_guard<std::mutex> g(vm);c=++seen[d->messageIdNumber];}
            if(c<=20 || (c&(c-1))==0)__android_log_print(sev>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT?ANDROID_LOG_ERROR:ANDROID_LOG_WARN,"dm-vvl","#%u %s %s",c,d->pMessageIdName?d->pMessageIdName:"?",d->pMessage?d->pMessage:"");
            return VK_FALSE;};
        auto mk=(PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT");VkDebugUtilsMessengerEXT m;
        if(mk)mk(instance,&mi,nullptr,&m);}
    uint32_t n=0;vkEnumeratePhysicalDevices(instance,&n,nullptr);std::vector<VkPhysicalDevice> devices(n);vkEnumeratePhysicalDevices(instance,&n,devices.data());if(!n)return fail("No Vulkan device",VK_ERROR_INITIALIZATION_FAILED);
    physical=devices[0];
    upscale::decide();   // [D3] gear "Upscale": the GPU vendor for auto, DM_GFX_UPSCALE (logged once)
    // 启动期间也可能旋转/切后台；等待新窗口，不把瞬时撤回误报为启动失败。
    // Rotation/backgrounding can race startup too; wait for a live window instead of failing boot.
    for(;;){wait_window();r=VkResult(create_surface());if(r==VK_NOT_READY)continue;if(r==VK_ERROR_SURFACE_LOST_KHR){uint64_t bad=active_generation;destroy_swap();wait_window_change(bad);continue;}if(r)return fail("Android surface",r);break;}
    vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,nullptr);std::vector<VkQueueFamilyProperties> q(n);vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,q.data());bool found=false;
    for(uint32_t i=0;i<n;i++){VkBool32 p=0;vkGetPhysicalDeviceSurfaceSupportKHR(physical,i,surface,&p);if(p&&(q[i].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)){family=i;found=true;break;}}if(!found)return fail("No graphics/compute/present family",VK_ERROR_FEATURE_NOT_PRESENT);
    VkPhysicalDeviceShaderDrawParametersFeatures draw{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};features.pNext=&draw;vkGetPhysicalDeviceFeatures2(physical,&features);
    vkEnumerateDeviceExtensionProperties(physical,nullptr,&n,nullptr);std::vector<VkExtensionProperties> available(n);vkEnumerateDeviceExtensionProperties(physical,nullptr,&n,available.data());bool mirror=false,minmax=false,indirect_count=false,ahb=false,foreign=false,format_list=false;for(auto e:available){if(std::string(e.extensionName)==VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME)format_list=true;if(std::string(e.extensionName)==VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME)ahb=true;if(std::string(e.extensionName)==VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME)foreign=true;if(std::string(e.extensionName)==VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME)indirect_count=true;if(std::string(e.extensionName)==VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME)mirror=true;if(std::string(e.extensionName)==VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME)minmax=true;}
    {std::string all;for(auto &e:available){all+=e.extensionName;all+=' ';}   // 诊断：设备扩展全表（每次启动一行）| Diagnostics: the full device extension list (one line per launch)
     for(size_t at=0;at<all.size();at+=900)__android_log_print(ANDROID_LOG_INFO,"dm-vulkan","Device extensions [%zu]: %s",at/900,all.substr(at,900).c_str());
     VkPhysicalDeviceExternalMemoryHostPropertiesEXT host{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};VkPhysicalDeviceProperties2 pr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};pr.pNext=&host;vkGetPhysicalDeviceProperties2(physical,&pr);
     __android_log_print(ANDROID_LOG_INFO,"dm-vulkan","Device %s api=%u.%u.%u driver=%#x minImportedHostPointerAlignment=%llu",pr.properties.deviceName,VK_VERSION_MAJOR(pr.properties.apiVersion),VK_VERSION_MINOR(pr.properties.apiVersion),VK_VERSION_PATCH(pr.properties.apiVersion),pr.properties.driverVersion,(unsigned long long)host.minImportedHostPointerAlignment);
#if defined(DM_GAME_XC2) || defined(DM_GAME_D3)
     {   // [XC2] one line per boot that answers the driver questions of docs/ADRENO750.md (driver, XFB, clip/cull, VS stores, SSBO range)
      VkPhysicalDeviceDriverProperties dp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};VkPhysicalDeviceSubgroupProperties sg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};sg.pNext=&dp;
      VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};p2.pNext=&sg;vkGetPhysicalDeviceProperties2(physical,&p2);const VkPhysicalDeviceLimits &L=p2.properties.limits;
      VkPhysicalDeviceTransformFeedbackFeaturesEXT xf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};f2.pNext=&xf;vkGetPhysicalDeviceFeatures2(physical,&f2);
      char nb[128]={0};__system_property_get("ro.dalvik.vm.native.bridge",nb);
      __android_log_print(ANDROID_LOG_INFO,"dm-vulkan","Device caps: driverID=%d name=\"%s\" info=\"%s\" id=%#x subgroup=%u stages=%#x ops=%#x xfb=%u streams=%u vsStores=%u clipCull=%u ssboRange=%u uboRange=%u nativeBridge=%s",
        int(dp.driverID),dp.driverName,dp.driverInfo,p2.properties.deviceID,sg.subgroupSize,unsigned(sg.supportedStages),unsigned(sg.supportedOperations),unsigned(xf.transformFeedback),unsigned(xf.geometryStreams),
        unsigned(f2.features.vertexPipelineStoresAndAtomics),L.maxCombinedClipAndCullDistances,L.maxStorageBufferRange,L.maxUniformBufferRange,nb[0]?nb:"none");}
#endif
    }
    std::vector<const char*> dext={VK_KHR_SWAPCHAIN_EXTENSION_NAME};if(mirror)dext.push_back(VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);
    if(dm_gpu_floor_want_hint(physical,available.data(),uint32_t(available.size()))){dext.push_back("VK_QCOM_queue_perf_hint");draw.pNext=dm_gpu_floor_chain(draw.pNext);}dm_gpu_floor_before_device();   // [gpu-floor] PNH-C01: Qualcomm's queue perf hint where the driver has it, else the KGSL contexts this process owns before the device (DM_GPU_FLOOR=0: off)
    // NVN min/max reduction samplers (depth pyramids): extension only (the instance is Vulkan 1.1); without it reductions sample as average.
    if(minmax)dext.push_back(VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME);
    // [DOOM] GPU-driven culling writes DrawElementsIndirect arguments and counts on the GPU; replayed as vkCmdDraw*IndirectCount.
    if(indirect_count)dext.push_back(VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME);
    // [DOOM] format lists keep UBWC compression on images that need a second view format (xbg_game.cpp upgrade_mutable)
    if(format_list){dext.push_back(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);dm_vk_format_list=1;}
    // [DOOM] zero-copy guest memory imports AHardwareBuffer pages as buffer memory (xbg_game.cpp zc)
    if(ahb && foreign){dext.push_back(VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);dext.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);}
    // [nonadreno][tile] VK_EXT/KHR_load_store_op_none: STORE_OP_NONE for attachment aspects a render pass does not write (DM_GFX_STORE_NONE,
    // from the DOOM 2016 port). Off on Adreno unless DM_GFX_STORE_NONE / DM_GFX_SIM_GPU ask, so the Adreno device is created exactly as before.
    {VkPhysicalDeviceProperties pp{};vkGetPhysicalDeviceProperties(physical,&pp);dm_vk_store_none=0;
     auto env_on=[](const char *n){const char *e=getenv(n);return e && *e && strcmp(e,"0")!=0;};
     const bool other=pp.vendorID!=0x5143 || env_on("DM_GFX_SIM_GPU");
     bool lsn_ext=false,lsn_khr=false;for(const auto &x:available){if(!strcmp(x.extensionName,VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME))lsn_ext=true;if(!strcmp(x.extensionName,"VK_KHR_load_store_op_none"))lsn_khr=true;}
     const char *sn=getenv("DM_GFX_STORE_NONE");
     if((lsn_ext || lsn_khr) && (sn && *sn?atoi(sn)!=0:other)){dext.push_back(lsn_khr?"VK_KHR_load_store_op_none":VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME);dm_vk_store_none=1;}
     __android_log_print(ANDROID_LOG_INFO,"dm-vulkan","NVN store ops: load_store_op_none ext=%d khr=%d enabled=%d",int(lsn_ext),int(lsn_khr),dm_vk_store_none);}
    // Diagnostic (marker gfx-pipe-stats): the driver's per-pipeline statistics (VK_KHR_pipeline_executable_properties).
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pef{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    if(FILE *ps=[&]{std::string dir=path;size_t cut=dir.find_last_of('/');return fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-pipe-stats").c_str(),"rb");}()){fclose(ps);dext.push_back(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);pef.pipelineExecutableInfo=VK_TRUE;pef.pNext=draw.pNext;draw.pNext=&pef;}
#ifdef DM_GAME_ETERNAL
    // [ETERNAL] id Tech 7 shaders index the whole texture / sampler pools: the engine mirrors them in update-after-bind descriptor
    // arrays (xbg_game.cpp namespace bindless). Only the features it uses are enabled; it logs and skips such shaders without them.
    VkPhysicalDeviceDescriptorIndexingFeaturesEXT di_on{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT};
    {bool di_ext=false;for(auto &e:available)if(std::string(e.extensionName)==VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME)di_ext=true;
     // core in Vulkan 1.2: a device can report 1.2+ without listing the EXT (LDPlayer); the same feature struct then needs no extension
     uint32_t dev_api=0;{VkPhysicalDeviceProperties pp;vkGetPhysicalDeviceProperties(physical,&pp);dev_api=pp.apiVersion;}
     const bool di_core=dev_api>=VK_API_VERSION_1_2;
     VkPhysicalDeviceDescriptorIndexingFeaturesEXT di{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT};
     if(di_ext || di_core){VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};f2.pNext=&di;vkGetPhysicalDeviceFeatures2(physical,&f2);}
     const bool ok=(di_ext || di_core) && di.descriptorBindingPartiallyBound && di.descriptorBindingSampledImageUpdateAfterBind;
     if(ok){if(di_ext)dext.push_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);di_on.descriptorBindingPartiallyBound=VK_TRUE;di_on.descriptorBindingSampledImageUpdateAfterBind=VK_TRUE;di_on.pNext=draw.pNext;draw.pNext=&di_on;dm_vk_descriptor_indexing=1;}
     __android_log_print(ANDROID_LOG_INFO,"dm-vulkan","Descriptor indexing ext=%d core=%d partiallyBound=%u sampledImageUpdateAfterBind=%u runtimeArray=%u nonUniformSampled=%u dynamicIndexing=%u -> %s",
        int(di_ext),int(di_core),unsigned(di.descriptorBindingPartiallyBound),unsigned(di.descriptorBindingSampledImageUpdateAfterBind),unsigned(di.runtimeDescriptorArray),unsigned(di.shaderSampledImageArrayNonUniformIndexing),
        unsigned(features.features.shaderSampledImageArrayDynamicIndexing),ok?"enabled":"off (bindless shaders are skipped)");}
#endif
#if defined(DM_GAME_XC2) || defined(DM_GAME_D3)
    // [XC2] NVN samplers with arbitrary border colours (Adreno 750: Qualcomm / Turnip list it); without it the engine uses the nearest canonical colour
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    {bool has=false;for(auto &e:available)if(std::string(e.extensionName)==VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME)has=true;
     if(has && !getenv("DM_GFX_NO_CUSTOM_BORDER")){VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};f2.pNext=&cbc;vkGetPhysicalDeviceFeatures2(physical,&f2);
      if(cbc.customBorderColors && cbc.customBorderColorWithoutFormat){dext.push_back(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);cbc.pNext=draw.pNext;draw.pNext=&cbc;dm_vk_custom_border=1;}}
     __android_log_print(ANDROID_LOG_INFO,"dm-vulkan","Custom border colour ext=%d colors=%u withoutFormat=%u -> %s",int(has),unsigned(cbc.customBorderColors),unsigned(cbc.customBorderColorWithoutFormat),dm_vk_custom_border?"enabled":"off (nearest canonical colour)");}
#endif
    xbg_device_fault_enable(physical,available,dext,&draw.pNext);__android_log_print(ANDROID_LOG_INFO,"dm-vulkan","NVN device fault: %s",dm_vk_device_fault_why);   // [device-fault] FN-64: VK_EXT_device_fault when the driver has it (DM_GFX_DEVICE_FAULT=0: off)
    pc_note("device-fault",dm_vk_device_fault,"%s; FN-64",dm_vk_device_fault_why);   // [pc]
    float priority=1;VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qc.queueFamilyIndex=family;qc.queueCount=1;qc.pQueuePriorities=&priority;VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.pNext=&draw;dc.pEnabledFeatures=&features.features;dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qc;dc.enabledExtensionCount=uint32_t(dext.size());dc.ppEnabledExtensionNames=dext.data();if((r=vkCreateDevice(physical,&dc,nullptr,&device)))return fail("vkCreateDevice",r);vkGetDeviceQueue(device,family,0,&queue);
    for(;;){
        wait_window();
        if(!surface||window_changed()){
            r=VkResult(create_surface());if(r==VK_NOT_READY)continue;
            if(r==VK_ERROR_SURFACE_LOST_KHR){uint64_t bad=active_generation;destroy_swap();wait_window_change(bad);continue;}
            if(r)return fail("Android surface",r);
        }
        r=VkResult(create_swap());
        if(r==VK_NOT_READY||r==VK_ERROR_OUT_OF_DATE_KHR){wait_resize();continue;}
        if(r==VK_ERROR_SURFACE_LOST_KHR){uint64_t bad=active_generation;destroy_swap();wait_window_change(bad);continue;}
        if(r)return fail("Swapchain",r);break;
    }
    if(fake_init_fail()){__android_log_print(ANDROID_LOG_WARN,"dm-vulkan","DM_VK_FAKE_INIT_FAIL=%s: failing the initialization after the swapchain (test of the driver fallback)",getenv("DM_VK_FAKE_INIT_FAIL"));
        return fail("Swapchain (faked by DM_VK_FAKE_INIT_FAIL)",VK_ERROR_INVALID_EXTERNAL_HANDLE);}
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;cp.queueFamilyIndex=family;if((r=vkCreateCommandPool(device,&cp,nullptr,&command_pool)))return fail("Present command pool",r);
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=command_pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;if((r=vkAllocateCommandBuffers(device,&ca,&command)))return fail("Present command",r);
    VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};if((r=vkCreateSemaphore(device,&sc,nullptr,&acquired)))return fail("Acquire semaphore",r);VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};if((r=vkCreateFence(device,&fc,nullptr,&fence)))return fail("Present fence",r);
    {std::string dir=path;size_t cut=dir.find_last_of('/');FILE *m=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-present-overlap").c_str(),"rb");overlap=true;if(m){overlap=fgetc(m)!='0';fclose(m);}
     auto open_marker=[&](const char *name){return fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+name).c_str(),"rb");};
     pin_mask=fastest_cores();pin_ab=0;pin_state=-1;pin_have_original=false;pin_presents=pin_drifts=0;pin_markers=false;
     if(FILE *p=open_marker("/gfx-pin-cpu")){if(fscanf(p,"%x",&pin_mask)!=1)pin_mask=0;fclose(p);pin_markers=true;}
     if(FILE *n=open_marker("/gfx-no-pin")){pin_mask=0;fclose(n);pin_markers=true;}
     if(FILE *t=open_marker("/gfx-ab-target")){char target[16]={0};if(fscanf(t,"%15s",target)==1 && !strcmp(target,"pin"))if(FILE *a=open_marker("/gfx-ab-toggle")){if(fscanf(a,"%d",&pin_ab)!=1)pin_ab=0;fclose(a);pin_markers=true;}fclose(t);}
     if(!pin_markers)pin_mask=0;dm_cpu_place_markers(pin_markers);   // [cpu-place] no marker: the module pins
     __android_log_print(ANDROID_LOG_INFO,"dm-vulkan","NVN pin mask=%x ab=%d (markers gfx-no-pin, gfx-pin-cpu, gfx-ab-target=pin%s)",pin_mask,pin_ab,pin_markers?"":"; none set: the [cpu] plan places this thread");
     if(FILE *b=open_marker("/gfx-membench")){fclose(b);membench();}}
    if(overlap){if((r=vkCreateCommandPool(device,&cp,nullptr,&overlap_pool)))return fail("Present command pool",r);ca.commandPool=overlap_pool;if((r=vkAllocateCommandBuffers(device,&ca,&overlap_command)))return fail("Present command",r);
        if((r=vkCreateSemaphore(device,&sc,nullptr,&overlap_acquired)))return fail("Acquire semaphore",r);if((r=vkCreateFence(device,&fc,nullptr,&overlap_fence)))return fail("Present fence",r);}
    __android_log_print(ANDROID_LOG_INFO,"dm-vulkan","Present overlap=%d (marker gfx-present-overlap)",int(overlap));
    shader_path=path;dm_vk_config config{};config.size=sizeof(config);config.game.size=sizeof(config.game);config.game.physical=physical;config.game.device=device;config.game.queue=queue;config.game.queue_family=family;config.game.enabled_core=features.features;config.game.enabled_shader_draw_parameters=draw.shaderDrawParameters;config.game.fence_timeout_ns=10000000000ull;config.game.shader_directory=shader_path.c_str();config.game.present=present;config.game.log=log_message;config.game.fatal=fatal;config.sampler_mirror_clamp_to_edge=mirror;config.sampler_filter_minmax=minmax;config.has_window=has_window;config.display_hz=0; // Query actual display refresh in frontend; do not guess 60 Hz.
    engine_touched=true;int status=dm_vk_initialize(&config,error,bytes);if(!status)ready=true;else cleanup_initialization();return status;
}
