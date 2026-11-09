// Frame generation (XC3_FRAME_GEN=dlss|fsr): a frame generated from two drawn ones is shown between them, so the
// window shows twice the frames the game draws.
//
// Both generators work from the inputs the temporal upscaler (vk_tup.c) prepares for its own use: the game's depth
// (0 near, 1 at infinity) and motion vectors (UV offsets to the previous frame) at the render size, with the camera's
// jitter, near plane and field of view. Without a temporal upscaler (upscaler = fsr or bilinear) there are none and
// nothing is generated. Each frame has an id (vk_fg_frame): the replay records frame id, queueing its present closes
// it (vk_backend.c present_image), and the present thread presents it later, while the replay may already record the
// next one; inputs and presents are matched by the id, never by "the newest".
//
// - dlss: NVIDIA DLSS Frame Generation through Streamline (vk_sl.cpp), whose proxy swapchain adds the generated frame
//   at the present.
// - fsr: AMD FSR 3 frame generation from FidelityFX SDK 1.1.4's amd_fidelityfx_vk.dll (the same DLL as the FSR 3
//   upscaler). Its frame generation swapchain replaces ours (vk_fg_fsr_swapchain): it creates the real swapchain,
//   paces the drawn and generated frames and presents them on a queue of its own, and needs a second queue for image
//   acquisition (vk_fg_device; all four of its queues must differ). Each frame records a "prepare" dispatch beside
//   the upscaler (vk_fg_inputs), which copies depth and motion into the context's own images, selected by the frame
//   id (FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT: the next frame's prepare may run before this one is
//   presented), and the present thread configures the frame before presenting it (vk_fg_present).
//
// A frame without inputs (loading screens, movies, a scene the upscaler did not see) and the in-game menu (drawn into
// the back buffer, it would be interpolated) turn generation off until two frames in a row have inputs again. The
// interface the game draws is part of the back buffer either way; neither generator gets a picture without it yet.
//
// The FidelityFX headers are AMD's, unmodified (MIT, third_party/ffx_api), but for ffx_api/vk/ffx_api_vk.h, whose
// inline helpers would pull in more than this needs: the structures used are declared below, same layout and type
// values.
// C++ for the FidelityFX frame generation header, which C cannot compile (it declares an empty enum).
#include "vk_fg.h"

#include <string.h>

extern "C" {
#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "vk.h"
}
#ifndef _WIN32
#define __declspec(x)
#endif
#include "third_party/ffx_api/ffx_api.h"
#include "third_party/ffx_api/ffx_framegeneration.h"
#ifndef _WIN32
#undef __declspec
#endif

// ---- ffx_api/vk/ffx_api_vk.h (FidelityFX SDK 1.1.4)
#define FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK 0x0000003u
#define FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK 0x40001u
#define FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK 0x40005u
typedef VkResult (*PfnQueueSubmitFfx)(uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence);
struct FgQueueInfoVK {
    VkQueue queue;
    uint32_t familyIndex;
    PfnQueueSubmitFfx submitFunc;
};
struct FgBackendVKDesc {
    ffxCreateContextDescHeader header;
    VkDevice vkDevice;
    VkPhysicalDevice vkPhysicalDevice;
    PFN_vkGetDeviceProcAddr vkDeviceProcAddr;
};
struct FgSwapChainVKDesc {
    ffxCreateContextDescHeader header;
    VkPhysicalDevice physicalDevice;
    VkDevice device;
    VkSwapchainKHR *swapchain;
    VkAllocationCallbacks *allocator;
    VkSwapchainCreateInfoKHR createInfo;
    struct FgQueueInfoVK gameQueue, asyncComputeQueue, presentQueue, imageAcquireQueue;
};
typedef VkResult (*PfnCreateSwapchainFfx)(VkDevice, const VkSwapchainCreateInfoKHR *, const VkAllocationCallbacks *,
                                          VkSwapchainKHR *, void *);
typedef void (*PfnDestroySwapchainFfx)(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks *, void *);
struct FgSwapchainFunctionsVK {
    ffxQueryDescHeader header;
    PfnCreateSwapchainFfx create;
    PfnDestroySwapchainFfx destroy;
    PFN_vkGetSwapchainImagesKHR images;
    PFN_vkAcquireNextImageKHR acquire;
    PFN_vkQueuePresentKHR present;
    PFN_vkSetHdrMetadataEXT hdr;
    uint64_t (*last_present_count)(VkSwapchainKHR);
};

#define RING 16

static struct {
    int mode;                       // VK_FG_*; -1 before the first read
    volatile uint64_t build;        // the frame the replay records
    int started;                    // frame 0's start was marked
    // FSR 3
    void *lib;
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxConfigure configure;
    PfnFfxDispatch dispatch;
    PfnFfxQuery query;
    int lib_tried;
    uint32_t first_queue;
    VkQueue present_q, acquire_q;
    ffxContext sc_ctx;              // the frame generation swapchain
    struct FgSwapchainFunctionsVK fns;
    ffxContext ctx;                 // the frame generation context
    uint32_t ctx_w, ctx_h, ctx_rw, ctx_rh, ctx_fmt;
    volatile uint32_t disp_w, disp_h, disp_fmt;   // the swapchain the present thread presents to
    PlatMutex lock;                 // ctx: created and used by the replay, configured by the present thread
    volatile uint64_t prepared[RING];   // id + 1 when the frame's inputs were recorded
    int fsr_on;                     // generation on at the last present
    int fsr_failed;
    uint32_t render_w, render_h;    // the inputs' size (the newest)
    // frames shown per drawn frame, measured every 30 presents from the generators' own counts (vk_fg_shown_ratio)
    uint64_t ratio_drawn, ratio_shown0;
    volatile float ratio;
} F = {.mode = -1};

int vk_fg_mode(void) {
    if (F.mode < 0) {
        const char *v = cfg_value("XC3_FRAME_GEN");
        F.mode = !strcmp(v, "dlss") ? VK_FG_DLSS : !strcmp(v, "fsr") ? VK_FG_FSR : VK_FG_OFF;
        if (F.mode != VK_FG_OFF) {
            const char *u = cfg_value("XC3_UPSCALER");
            int temporal = !strcmp(u, "fsr3") || !strcmp(u, "dlss") || !strcmp(u, "dlaa");
            rt_log("frame generation: %s%s", F.mode == VK_FG_DLSS ? "NVIDIA DLSS Frame Generation" : "AMD FSR 3",
                   temporal ? "" : " asked for, but the upscaler is not temporal (fsr3, dlss or dlaa): no inputs, off");
        }
    }
    return F.mode;
}

static int fsr_wanted(void) { return vk_fg_mode() == VK_FG_FSR && !F.fsr_failed; }

uint32_t vk_fg_extra_queues(void) { return fsr_wanted() ? 2 : 0; }

void vk_fg_device(VkDevice dev, uint32_t family, uint32_t first_queue) {
    if (!fsr_wanted()) return;
    if (first_queue) {
        F.first_queue = first_queue;
        vkGetDeviceQueue(dev, family, first_queue, &F.present_q);
        vkGetDeviceQueue(dev, family, first_queue + 1, &F.acquire_q);
    }
    if (!F.present_q || !F.acquire_q) {
        rt_log("frame generation: FSR 3 off: the graphics queue family has no room for its two queues");
        F.fsr_failed = 1;
    }
}

// ---- frame ids

uint64_t vk_fg_frame(void) {
    uint64_t id = __atomic_load_n(&F.build, __ATOMIC_ACQUIRE);
    if (!F.started && (vk_fg_mode() == VK_FG_DLSS || vk_sl_reflex_ready())) {
        F.started = 1;
        vk_sl_fg_frame_start(id);
    }
    return id;
}

uint64_t vk_fg_frame_close(void) {
    uint64_t id = vk_fg_frame();
    __atomic_store_n(&F.build, id + 1, __ATOMIC_RELEASE);
    if (vk_fg_mode() == VK_FG_DLSS || vk_sl_reflex_ready()) {   // vk_sl.cpp: DLSS-G, or Reflex with DLSS alone
        vk_sl_fg_frame_end(id);
        vk_sl_fg_frame_start(id + 1);   // Reflex sleeps here, before the next frame is recorded
    }
    return id;
}

// ---- FSR 3

static int load_ffx(void) {
    if (F.lib_tried) return F.lib != NULL;
    F.lib_tried = 1;
    char path[1024];
    if (!plat_exe_path(path, sizeof path - 64)) return 0;
    char *slash = plat_last_sep(path);
    if (!slash) return 0;
    strcpy(slash + 1, "amd_fidelityfx_vk.dll");
    void *lib = plat_lib_open(path);
    if (!lib) {
        rt_log("frame generation: FSR 3 off: %s is missing (tools/fetch_upscalers.py puts it there)", path);
        return 0;
    }
    F.create = (PfnFfxCreateContext)plat_lib_sym(lib, "ffxCreateContext");
    F.destroy = (PfnFfxDestroyContext)plat_lib_sym(lib, "ffxDestroyContext");
    F.configure = (PfnFfxConfigure)plat_lib_sym(lib, "ffxConfigure");
    F.dispatch = (PfnFfxDispatch)plat_lib_sym(lib, "ffxDispatch");
    F.query = (PfnFfxQuery)plat_lib_sym(lib, "ffxQuery");
    if (!F.create || !F.destroy || !F.configure || !F.dispatch || !F.query) {
        rt_log("frame generation: FSR 3 off: %s lacks the FidelityFX API entry points", path);
        return 0;
    }
    F.lib = lib;
    return 1;
}

// The game queue is shared with every other submitter: FidelityFX submits to it through this, under the queue lock.
static VkResult game_submit(uint32_t n, const VkSubmitInfo *s, VkFence fence) {
    vk_queue_lock();
    VkResult r = vkQueueSubmit(vk_ctx()->gfx_queue, n, s, fence);
    vk_queue_unlock();
    return r;
}

VkResult vk_fg_fsr_swapchain(const VkSwapchainCreateInfoKHR *ci, VkSwapchainKHR old, VkSwapchainKHR *out,
                             PFN_vkGetSwapchainImagesKHR *images, PFN_vkAcquireNextImageKHR *acquire,
                             PFN_vkQueuePresentKHR *present) {
    VkCtx *c = vk_ctx();
    if (!fsr_wanted() || !load_ffx()) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkSwapchainCreateInfoKHR info = *ci;
    info.oldSwapchain = VK_NULL_HANDLE;   // the frame generation swapchain takes no old one: it goes first
    VkResult r;
    if (!F.sc_ctx) {
        if (old) vkDestroySwapchainKHR(c->device, old, NULL);
        VkSwapchainKHR sc = VK_NULL_HANDLE;
        struct FgSwapChainVKDesc d = {.header = {.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK},
                                      .physicalDevice = c->phys, .device = c->device, .swapchain = &sc,
                                      .createInfo = info,
                                      .gameQueue = {c->gfx_queue, c->gfx_family, game_submit},
                                      .presentQueue = {F.present_q, c->gfx_family, NULL},
                                      .imageAcquireQueue = {F.acquire_q, c->gfx_family, NULL}};
        ffxReturnCode_t rc = F.create(&F.sc_ctx, &d.header, NULL);
        if (rc != FFX_API_RETURN_OK) {
            rt_log("frame generation: FSR 3 off: its swapchain could not be created (%u)", rc);
            F.sc_ctx = NULL;
            F.fsr_failed = 1;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        memset(&F.fns, 0, sizeof F.fns);
        F.fns.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
        rc = F.query(&F.sc_ctx, &F.fns.header);
        if (rc != FFX_API_RETURN_OK || !F.fns.create || !F.fns.destroy || !F.fns.images || !F.fns.acquire ||
            !F.fns.present) {
            rt_log("frame generation: FSR 3 off: its swapchain functions are missing (%u)", rc);
            F.destroy(&F.sc_ctx, NULL);   // destroys its swapchain too
            F.sc_ctx = NULL;
            F.fsr_failed = 1;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        rt_log("frame generation: FSR 3 swapchain %ux%u", info.imageExtent.width, info.imageExtent.height);
        *out = sc;
        r = VK_SUCCESS;
    } else {
        if (old) F.fns.destroy(c->device, old, NULL, F.sc_ctx);
        r = F.fns.create(c->device, &info, NULL, out, F.sc_ctx);
        if (r != VK_SUCCESS) {
            rt_log("frame generation: FSR 3 off: its swapchain could not be recreated (%d)", (int)r);
            F.fsr_failed = 1;
            return r;
        }
    }
    *images = F.fns.images, *acquire = F.fns.acquire, *present = F.fns.present;
    __atomic_store_n(&F.disp_w, info.imageExtent.width, __ATOMIC_RELEASE);
    __atomic_store_n(&F.disp_h, info.imageExtent.height, __ATOMIC_RELEASE);
    __atomic_store_n(&F.disp_fmt, (uint32_t)info.imageFormat, __ATOMIC_RELEASE);
    return r;
}

void vk_fg_fsr_swapchain_destroy(VkSwapchainKHR sc) {
    if (!F.sc_ctx) return;
    if (sc) F.fns.destroy(vk_ctx()->device, sc, NULL, F.sc_ctx);
}

static uint32_t ffx_format(VkFormat f) {
    switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM: return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_R8G8B8A8_SRGB: return FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
    default: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    }
}

// FidelityFX looks up vkGetBufferMemoryRequirements2 by its KHR name, which a Vulkan 1.3 device without
// VK_KHR_get_memory_requirements2 enabled answers with NULL: a KHR name that finds nothing is looked up again as the core
// function it became.
static PFN_vkVoidFunction VKAPI_CALL ffx_device_proc(VkDevice dev, const char *name) {
    PFN_vkVoidFunction f = vkGetDeviceProcAddr(dev, name);
    size_t n = strlen(name);
    if (!f && n > 3 && n < 128 && !strcmp(name + n - 3, "KHR")) {
        char core[128];
        memcpy(core, name, n - 3);
        core[n - 3] = 0;
        f = vkGetDeviceProcAddr(dev, core);
    }
    return f;
}

// The frame generation context for the swapchain's size and the inputs' size; F.lock held. A size change waits for
// the device: queued frames may still use the old context's images.
static int ensure_ctx(uint32_t rw, uint32_t rh) {
    uint32_t w = __atomic_load_n(&F.disp_w, __ATOMIC_ACQUIRE), h = __atomic_load_n(&F.disp_h, __ATOMIC_ACQUIRE),
             fmt = __atomic_load_n(&F.disp_fmt, __ATOMIC_ACQUIRE);
    if (!w || !h || !F.sc_ctx) return 0;
    if (F.ctx && F.ctx_w == w && F.ctx_h == h && F.ctx_fmt == fmt && rw <= F.ctx_rw && rh <= F.ctx_rh) return 1;
    VkCtx *c = vk_ctx();
    if (F.ctx) {
        vk_queue_lock();
        vkDeviceWaitIdle(c->device);
        vk_queue_unlock();
        F.destroy(&F.ctx, NULL);
        F.ctx = NULL;
    }
    struct FgBackendVKDesc be = {.header = {.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK}, .vkDevice = c->device,
                                 .vkPhysicalDevice = c->phys, .vkDeviceProcAddr = ffx_device_proc};
    struct ffxCreateContextDescFrameGeneration d = {
        .header = {.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, .pNext = &be.header},
        .flags = FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT | FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE,
        .displaySize = {w, h}, .maxRenderSize = {rw > w ? rw : w, rh > h ? rh : h},
        .backBufferFormat = ffx_format((VkFormat)fmt)};
    if (cfg_get("XC3_TUP_DEBUG")) d.flags |= FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING;
    ffxReturnCode_t rc = F.create(&F.ctx, &d.header, NULL);
    if (rc != FFX_API_RETURN_OK) {
        static int logged;
        if (!logged++) rt_log("frame generation: FSR 3 context could not be created (%u)", rc);
        F.ctx = NULL;
        return 0;
    }
    F.ctx_w = w, F.ctx_h = h, F.ctx_fmt = fmt, F.ctx_rw = d.maxRenderSize.width, F.ctx_rh = d.maxRenderSize.height;
    rt_log("frame generation: FSR 3 generates at %ux%u from inputs of %ux%u", w, h, rw, rh);
    return 1;
}

static struct FfxApiResource ffx_res(const VkSlImage *i, uint32_t fmt) {
    struct FfxApiResource r = {.resource = (void *)i->image, .state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS};
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = fmt;
    r.description.width = i->iw ? i->iw : i->w, r.description.height = i->ih ? i->ih : i->h, r.description.depth = 1;
    r.description.mipCount = 1;
    r.description.usage = FFX_API_RESOURCE_USAGE_READ_ONLY;
    return r;
}

void vk_fg_inputs(VkCommandBuffer cb, const VkSlImage *depth, const VkSlImage *motion, const float jitter[2],
                  float near_z, float fov_y, int reset, float dt_ms, int dlss_tagged) {
    int mode = vk_fg_mode();
    if (mode == VK_FG_OFF) return;
    uint64_t id = vk_fg_frame();
    F.render_w = depth->iw ? depth->iw : depth->w, F.render_h = depth->ih ? depth->ih : depth->h;   // the largest
    if (mode == VK_FG_DLSS) {
        if (!dlss_tagged) vk_sl_fg_inputs(cb, id, depth, motion, jitter, near_z, fov_y, reset);
        return;
    }
    if (!fsr_wanted() || !F.lib) return;
    plat_mutex_lock(&F.lock);
    if (ensure_ctx(depth->w, depth->h)) {
        struct ffxDispatchDescFrameGenerationPrepare p = {.header = {.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE},
                                                          .frameID = id, .commandList = (void *)cb};
        p.renderSize.width = depth->w, p.renderSize.height = depth->h;
        p.jitterOffset.x = jitter[0], p.jitterOffset.y = jitter[1];
        p.motionVectorScale.x = (float)depth->w, p.motionVectorScale.y = (float)depth->h;
        p.frameTimeDelta = dt_ms;
        p.unused_reset = reset != 0;
        p.cameraNear = near_z;
        p.cameraFar = 3.4e38f;
        p.cameraFovAngleVertical = fov_y;
        p.viewSpaceToMetersFactor = 1.0f;
        p.depth = ffx_res(depth, FFX_API_SURFACE_FORMAT_R32_FLOAT);
        p.motionVectors = ffx_res(motion, FFX_API_SURFACE_FORMAT_R16G16_FLOAT);
        ffxReturnCode_t rc = F.dispatch(&F.ctx, &p.header);
        if (rc == FFX_API_RETURN_OK) __atomic_store_n(&F.prepared[id % RING], id + 1, __ATOMIC_RELEASE);
        else {
            static int logged;
            if (!logged++) rt_log("frame generation: FSR 3 prepare failed (%u)", rc);
        }
    }
    plat_mutex_unlock(&F.lock);
}

static int prepared(uint64_t id) { return __atomic_load_n(&F.prepared[id % RING], __ATOMIC_ACQUIRE) == id + 1; }

// The swapchain calls this from its present to generate the frame: the context's own dispatch.
static ffxReturnCode_t fsr_generate(ffxDispatchDescFrameGeneration *params, void *user) {
    return F.dispatch((ffxContext *)user, &params->header);
}

// The present thread, once per drawn frame: the generator's count of frames it presented (DLSS-G's own count, or how
// many presents the FSR 3 swapchain made) over the last 30 drawn frames gives the shown frames per drawn one.
static void ratio_tick(int mode, VkSwapchainKHR sc) {
    uint64_t shown = mode == VK_FG_DLSS ? vk_sl_fg_shown()
                   : F.sc_ctx && F.fns.last_present_count && sc ? F.fns.last_present_count(sc) : 0;
    if (++F.ratio_drawn < 30) return;
    float r = shown > F.ratio_shown0 ? (float)(shown - F.ratio_shown0) / (float)F.ratio_drawn : 1;
    __atomic_store_n(&F.ratio, r < 1 ? 1 : r > 8 ? 8 : r, __ATOMIC_RELAXED);
    F.ratio_drawn = 0, F.ratio_shown0 = shown;
    static int n;
    if ((rt_trace & RT_TRACE_PERF) && ++n % 20 == 0) rt_log("frame generation: %.2f frames shown per drawn frame", r);
}

float vk_fg_shown_ratio(void) {
    float r = __atomic_load_n(&F.ratio, __ATOMIC_RELAXED);
    return F.mode > VK_FG_OFF && r > 1 ? r : 1;
}

void vk_fg_present(uint64_t id, VkSwapchainKHR sc, uint32_t w, uint32_t h, VkFormat fmt, uint32_t images,
                   const int rect[4], int allow) {
    int mode = vk_fg_mode();
    if (mode != VK_FG_OFF) ratio_tick(mode, sc);
    if (mode == VK_FG_OFF) vk_sl_reflex_present(id);   // Reflex with DLSS alone
    if (mode == VK_FG_DLSS) {
        vk_sl_fg_present(id, w, h, fmt, images, rect, F.render_w, F.render_h, allow);
        return;
    }
    if (mode != VK_FG_FSR || !F.sc_ctx || F.fsr_failed) return;
    plat_mutex_lock(&F.lock);
    if (F.ctx) {
        int on = allow && prepared(id) && id > 0 && prepared(id - 1) && F.ctx_w == w && F.ctx_h == h;
        struct ffxConfigureDescFrameGeneration cf = {.header = {.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION},
                                                     .swapChain = (void *)sc, .frameGenerationCallback = fsr_generate,
                                                     .frameGenerationCallbackUserContext = &F.ctx,
                                                     .frameGenerationEnabled = on != 0, .allowAsyncWorkloads = false,
                                                     .frameID = id};
        if (rect) {
            cf.generationRect.left = rect[0], cf.generationRect.top = rect[1];
            cf.generationRect.width = rect[2], cf.generationRect.height = rect[3];
        } else {
            cf.generationRect.width = (int32_t)w, cf.generationRect.height = (int32_t)h;
        }
        ffxReturnCode_t rc = F.configure(&F.ctx, &cf.header);
        if (rc != FFX_API_RETURN_OK) {
            static int logged;
            if (!logged++) rt_log("frame generation: FSR 3 configure failed (%u)", rc);
            on = 0;
        }
        if (on != F.fsr_on)
            rt_log("frame generation: FSR 3 %s (frame %llu, %ux%u)", on ? "on" : "off", (unsigned long long)id, w, h);
        F.fsr_on = on;
    }
    plat_mutex_unlock(&F.lock);
    (void)images;
}

void vk_fg_present_end(uint64_t id) {
    if (vk_fg_mode() == VK_FG_DLSS || vk_sl_reflex_ready()) vk_sl_fg_present_end(id);
}

void vk_fg_release(void) {
    plat_mutex_lock(&F.lock);
    if (F.ctx) F.destroy(&F.ctx, NULL);
    F.ctx = NULL;
    plat_mutex_unlock(&F.lock);
}

int gfx_frame_gen_factor(void) {
    int m = F.mode;
    return (m == VK_FG_DLSS && vk_sl_fg_active()) || (m == VK_FG_FSR && F.fsr_on) ? 2 : 1;
}

const char *vk_fg_status(void) {
    int m = F.mode;
    if (m == VK_FG_DLSS && vk_sl_fg_active()) return "DLSS FG";
    if (m == VK_FG_FSR && F.fsr_on) return "FSR FG";
    return NULL;
}
