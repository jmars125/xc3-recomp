// NVIDIA DLSS through Streamline (XC3_UPSCALER=dlss or dlaa), the second back end of the temporal upscaler (vk_tup.c).
//
// Streamline 2.x (MIT; headers in third_party/streamline, unmodified) is NVIDIA's interface to DLSS: sl.interposer.dll
// and its signed plugins (sl.common.dll with NvLowLatencyVk.dll, sl.dlss.dll and the model, nvngx_dlss.dll) sit next to xc3.exe
// (tools/fetch_upscalers.py puts them there). It is loaded at run time, never linked, and used in its "manual hooking"
// mode: the renderer keeps calling vulkan-1.dll itself and only routes the calls Streamline must see through it.
//
// - vk_sl_start, before the Vulkan instance exists, loads the interposer, starts Streamline with DLSS as its one
//   feature and returns the instance and device extensions DLSS asks for (vk_device.c adds those the driver has).
// - vk_sl_device, once the device exists, hands Streamline the instance, the device and the queue family.
// - The present and the swapchain calls go through Streamline's proxies (vk_sl_proc, gfx_present.c): its common plugin
//   counts frames at the present, and a present that bypasses it leaks memory.
// - vk_sl_evaluate tags the inputs vk_tup.c prepared (colour, depth, motion vectors, output), sets the camera
//   constants and the DLSS mode, and records DLSS into the command buffer. Streamline moves the tagged images between
//   layouts and back itself; it records straight into the buffer, so the recorder thread is drained first (vk_tup.c).
//
// The headers are C++ with MSVC's layout of plain structures, which clang's MinGW target shares for these types; the
// one class with a virtual function (sl::FrameToken) is only passed back by reference, never called.
#include "vk_sl.h"

#include <math.h>
#include <string.h>

extern "C" {
#include "cfg.h"
#include "host.h"
}

#ifdef _WIN32
#include <windows.h>

#include "third_party/streamline/sl.h"
#include "third_party/streamline/sl_dlss.h"
#include "third_party/streamline/sl_helpers_vk.h"

namespace {

using PfnInit = sl::Result (*)(const sl::Preferences &, uint64_t);
using PfnShutdown = sl::Result (*)();
using PfnRequirements = sl::Result (*)(sl::Feature, sl::FeatureRequirements &);
using PfnSetVulkanInfo = sl::Result (*)(const sl::VulkanInfo &);
using PfnFeatureFunction = sl::Result (*)(sl::Feature, const char *, void *&);
using PfnNewFrameToken = sl::Result (*)(sl::FrameToken *&, const uint32_t *);
using PfnSetTag = sl::Result (*)(const sl::FrameToken &, const sl::ViewportHandle &, const sl::ResourceTag *, uint32_t,
                                 sl::CommandBuffer *);
using PfnSetConstants = sl::Result (*)(const sl::Constants &, const sl::FrameToken &, const sl::ViewportHandle &);
using PfnEvaluate = sl::Result (*)(sl::Feature, const sl::FrameToken &, const sl::BaseStructure **, uint32_t,
                                   sl::CommandBuffer *);
using PfnIsSupported = sl::Result (*)(sl::Feature, const sl::AdapterInfo &);
using PfnDlssSetOptions = sl::Result (*)(const sl::ViewportHandle &, const sl::DLSSOptions &);

struct {
    HMODULE lib;
    int started;     // slInit succeeded
    int ready;       // the device is set and DLSS runs on it
    int failed;      // given up for the session
    PfnShutdown shutdown;
    PfnRequirements requirements;
    PfnSetVulkanInfo set_vulkan_info;
    PfnFeatureFunction feature_function;
    PfnNewFrameToken new_frame_token;
    PfnSetTag set_tag;
    PfnSetConstants set_constants;
    PfnEvaluate evaluate;
    PfnIsSupported is_supported;
    PfnDlssSetOptions dlss_set_options;
    PFN_vkGetDeviceProcAddr device_proc;
    PFN_vkGetInstanceProcAddr instance_proc;
    sl::FeatureRequirements req;
    uint32_t frame;
    uint32_t last_mode, last_w, last_h;
} S;

const char *result_name(sl::Result r) {
    switch (r) {
    case sl::Result::eOk: return "ok";
    case sl::Result::eErrorIO: return "a file could not be read";
    case sl::Result::eErrorDriverOutOfDate: return "the driver is too old";
    case sl::Result::eErrorOSOutOfDate: return "Windows is too old";
    case sl::Result::eErrorOSDisabledHWS: return "hardware-accelerated GPU scheduling is off";
    case sl::Result::eErrorDeviceNotCreated: return "no device";
    case sl::Result::eErrorNoSupportedAdapterFound: return "no supported GPU (DLSS needs an NVIDIA RTX card)";
    case sl::Result::eErrorAdapterNotSupported: return "the GPU does not support DLSS";
    case sl::Result::eErrorNoPlugins: return "no Streamline plugins next to xc3.exe";
    case sl::Result::eErrorVulkanAPI: return "a Vulkan call failed";
    case sl::Result::eErrorFeatureMissing: return "the DLSS plugin did not load";
    case sl::Result::eErrorFeatureNotSupported: return "DLSS is not supported here";
    case sl::Result::eErrorMissingInputParameter: return "an input is missing";
    case sl::Result::eErrorNotInitialized: return "Streamline is not started";
    default: return "an error";
    }
}

void log_cb(sl::LogType type, const char *msg) {
    if (type == sl::LogType::eInfo) return;
    char b[512];
    size_t n = strlen(msg);
    if (n >= sizeof b) n = sizeof b - 1;
    memcpy(b, msg, n);
    while (n && (b[n - 1] == '\n' || b[n - 1] == '\r')) n--;
    b[n] = 0;
    rt_log("vk: Streamline %s: %s", type == sl::LogType::eError ? "error" : "warning", b);
}

template <class T> T sym(const char *name) { return reinterpret_cast<T>(reinterpret_cast<void *>(GetProcAddress(S.lib, name))); }

// The folder of xc3.exe as a wide string with a trailing separator.
int exe_dir(wchar_t *out, size_t n) {
    DWORD k = GetModuleFileNameW(NULL, out, (DWORD)n);
    if (!k || k >= n) return 0;
    while (k && out[k - 1] != L'\\' && out[k - 1] != L'/') k--;
    out[k] = 0;
    return k > 0;
}

} // namespace

extern "C" int vk_sl_start(void) {
    if (S.started || S.failed) return S.started;
    S.failed = 1;
    wchar_t dir[MAX_PATH], path[MAX_PATH + 32];
    if (!exe_dir(dir, MAX_PATH)) return 0;
    wcscpy(path, dir);
    wcscat(path, L"sl.interposer.dll");
    // Only the interposer next to the exe: LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR finds its plugins beside it.
    S.lib = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!S.lib) {
        rt_log("vk: DLSS off: sl.interposer.dll is missing next to xc3.exe (tools/fetch_upscalers.py puts it there)");
        return 0;
    }
    PfnInit init = sym<PfnInit>("slInit");
    S.shutdown = sym<PfnShutdown>("slShutdown");
    S.requirements = sym<PfnRequirements>("slGetFeatureRequirements");
    S.set_vulkan_info = sym<PfnSetVulkanInfo>("slSetVulkanInfo");
    S.feature_function = sym<PfnFeatureFunction>("slGetFeatureFunction");
    S.new_frame_token = sym<PfnNewFrameToken>("slGetNewFrameToken");
    S.set_tag = sym<PfnSetTag>("slSetTagForFrame");
    S.set_constants = sym<PfnSetConstants>("slSetConstants");
    S.evaluate = sym<PfnEvaluate>("slEvaluateFeature");
    S.is_supported = sym<PfnIsSupported>("slIsFeatureSupported");
    S.device_proc = sym<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
    S.instance_proc = sym<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    if (!init || !S.shutdown || !S.requirements || !S.set_vulkan_info || !S.feature_function || !S.new_frame_token ||
        !S.set_tag || !S.set_constants || !S.evaluate || !S.device_proc || !S.instance_proc) {
        rt_log("vk: DLSS off: sl.interposer.dll lacks the Streamline entry points");
        return 0;
    }
    static const sl::Feature features[] = {sl::kFeatureDLSS};
    static wchar_t plugin_dir[MAX_PATH];
    static const wchar_t *plugin_dirs[] = {plugin_dir};
    wcscpy(plugin_dir, dir);
    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = cfg_get("XC3_TUP_DEBUG") ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    pref.pathsToPlugins = plugin_dirs;
    pref.numPathsToPlugins = 1;
    pref.logMessageCallback = log_cb;
    // No over-the-air updates: the DLLs next to the exe are the ones that run.
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
                 sl::PreferenceFlags::eDisableDebugText | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = 1;
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "1.0";
    pref.projectId = "7f5f2a3e-8c1d-4b6a-9e2f-3c4d5e6f7a81";   // a fixed id of this project (DLSS asks for one)
    pref.renderAPI = sl::RenderAPI::eVulkan;
    sl::Result r = init(pref, sl::kSDKVersion);
    if (r != sl::Result::eOk) {
        rt_log("vk: DLSS off: Streamline did not start (%s)", result_name(r));
        return 0;
    }
    S.started = 1;
    r = S.requirements(sl::kFeatureDLSS, S.req);
    if (r != sl::Result::eOk) {
        rt_log("vk: DLSS off: no DLSS requirements (%s)", result_name(r));
        S.shutdown();
        S.started = 0;
        return 0;
    }
    S.failed = 0;
    rt_log("vk: Streamline started for DLSS: %u instance and %u device extensions asked for",
           S.req.vkNumInstanceExtensions, S.req.vkNumDeviceExtensions);
    return 1;
}

extern "C" uint32_t vk_sl_instance_exts(const char **out, uint32_t max) {
    uint32_t n = 0;
    for (uint32_t i = 0; S.started && i < S.req.vkNumInstanceExtensions && n < max; i++) out[n++] = S.req.vkInstanceExtensions[i];
    return n;
}

extern "C" uint32_t vk_sl_device_exts(const char **out, uint32_t max) {
    uint32_t n = 0;
    for (uint32_t i = 0; S.started && i < S.req.vkNumDeviceExtensions && n < max; i++) out[n++] = S.req.vkDeviceExtensions[i];
    return n;
}

extern "C" void vk_sl_device(VkInstance inst, VkPhysicalDevice phys, VkDevice dev, uint32_t family, int exts_ok) {
    if (!S.started) return;
    if (!exts_ok) {
        rt_log("vk: DLSS off: the device lacks an extension DLSS needs");
        S.failed = 1;
        return;
    }
    sl::VulkanInfo info{};
    info.instance = inst, info.physicalDevice = phys, info.device = dev;
    info.graphicsQueueFamily = info.computeQueueFamily = family;
    info.graphicsQueueIndex = info.computeQueueIndex = 1;   // DLSS asks for no queues of its own
    sl::Result r = S.set_vulkan_info(info);
    if (r != sl::Result::eOk) {
        rt_log("vk: DLSS off: Streamline refused the device (%s)", result_name(r));
        S.failed = 1;
        return;
    }
    sl::AdapterInfo ai{};
    ai.vkPhysicalDevice = phys;
    if (S.is_supported && (r = S.is_supported(sl::kFeatureDLSS, ai)) != sl::Result::eOk) {
        rt_log("vk: DLSS off: %s", result_name(r));
        S.failed = 1;
        return;
    }
    void *f = NULL;
    if (S.feature_function(sl::kFeatureDLSS, "slDLSSSetOptions", f) != sl::Result::eOk || !f) {
        rt_log("vk: DLSS off: the DLSS plugin lacks slDLSSSetOptions");
        S.failed = 1;
        return;
    }
    S.dlss_set_options = reinterpret_cast<PfnDlssSetOptions>(f);
    S.ready = 1;
    rt_log("vk: DLSS is ready");
}

extern "C" int vk_sl_ready(void) { return S.ready; }

extern "C" PFN_vkVoidFunction vk_sl_proc(VkDevice dev, const char *name) {
    if (!S.ready) return NULL;
    return S.device_proc(dev, name);
}

extern "C" int vk_sl_evaluate(VkCommandBuffer cb, const VkSlImage *color, const VkSlImage *depth, const VkSlImage *motion,
                              const VkSlImage *out, const float jitter[2], float near_z, float fov_y, int reset,
                              int dlaa) {
    if (!S.ready) return 0;
    sl::FrameToken *ft = NULL;
    uint32_t idx = S.frame++;
    if (S.new_frame_token(ft, &idx) != sl::Result::eOk || !ft) return 0;
    sl::ViewportHandle vp(0u);

    sl::DLSSOptions o{};
    // The mode whose render share is nearest the one in use: DLAA 1, quality 2/3, balanced 0.58, performance 1/2,
    // ultra performance 1/3.
    float share = (float)color->w / (float)out->w;
    o.mode = dlaa || share > 0.83f ? sl::DLSSMode::eDLAA
             : share > 0.62f         ? sl::DLSSMode::eMaxQuality
             : share > 0.54f         ? sl::DLSSMode::eBalanced
             : share > 0.42f         ? sl::DLSSMode::eMaxPerformance
                                     : sl::DLSSMode::eUltraPerformance;
    o.outputWidth = out->w, o.outputHeight = out->h;
    o.colorBuffersHDR = sl::Boolean::eFalse;   // tonemapped, gamma-encoded colour
    o.useAutoExposure = sl::Boolean::eFalse;
    if ((uint32_t)o.mode != S.last_mode || out->w != S.last_w || out->h != S.last_h) {
        sl::Result r = S.dlss_set_options(vp, o);
        if (r != sl::Result::eOk) {
            static int logged;
            if (!logged++) rt_log("vk: DLSS: options refused (%s)", result_name(r));
            return 0;
        }
        S.last_mode = (uint32_t)o.mode, S.last_w = out->w, S.last_h = out->h;
        static const char *const names[] = {"off", "performance", "balanced", "quality", "ultra performance",
                                            "ultra quality", "DLAA"};
        uint32_t m = (uint32_t)o.mode;
        rt_log("vk: DLSS (%s) upscales %ux%u to %ux%u", m < 7 ? names[m] : "?", color->w, color->h, out->w, out->h);
    }

    // Projection without the jitter, row-major: the game's depth runs from 0 at the near plane to 1 at infinity.
    float aspect = (float)color->w / (float)color->h, t = 1.0f / tanf(fov_y * 0.5f);
    sl::Constants k{};
    k.cameraViewToClip.setRow(0, sl::float4(t / aspect, 0, 0, 0));
    k.cameraViewToClip.setRow(1, sl::float4(0, t, 0, 0));
    k.cameraViewToClip.setRow(2, sl::float4(0, 0, 1, 1));
    k.cameraViewToClip.setRow(3, sl::float4(0, 0, -near_z, 0));
    k.clipToCameraView.setRow(0, sl::float4(aspect / t, 0, 0, 0));
    k.clipToCameraView.setRow(1, sl::float4(0, 1 / t, 0, 0));
    k.clipToCameraView.setRow(2, sl::float4(0, 0, 0, -1 / near_z));
    k.clipToCameraView.setRow(3, sl::float4(0, 0, 1, 1 / near_z));
    for (uint32_t i = 0; i < 4; i++) {
        sl::float4 e(i == 0, i == 1, i == 2, i == 3);
        k.clipToLensClip.setRow(i, e), k.clipToPrevClip.setRow(i, e), k.prevClipToClip.setRow(i, e);
    }
    k.jitterOffset = {jitter[0], jitter[1]};
    k.mvecScale = {1.0f, 1.0f};   // the prepared motion vectors are offsets in UV
    k.cameraPinholeOffset = {0.0f, 0.0f};
    k.cameraPos = {0, 0, 0}, k.cameraUp = {0, 1, 0}, k.cameraRight = {1, 0, 0}, k.cameraFwd = {0, 0, 1};
    k.cameraNear = near_z;
    k.cameraFar = 100000.0f;
    k.cameraFOV = fov_y;
    k.cameraAspectRatio = aspect;
    k.depthInverted = sl::Boolean::eFalse;
    k.cameraMotionIncluded = sl::Boolean::eTrue;
    k.motionVectors3D = sl::Boolean::eFalse;
    k.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    if (S.set_constants(k, *ft, vp) != sl::Result::eOk) return 0;

    const VkSlImage *img[4] = {color, depth, motion, out};
    const sl::BufferType type[4] = {sl::kBufferTypeScalingInputColor, sl::kBufferTypeDepth, sl::kBufferTypeMotionVectors,
                                    sl::kBufferTypeScalingOutputColor};
    sl::Resource res[4] = {
        {sl::ResourceType::eTex2d, NULL, NULL, NULL}, {sl::ResourceType::eTex2d, NULL, NULL, NULL},
        {sl::ResourceType::eTex2d, NULL, NULL, NULL}, {sl::ResourceType::eTex2d, NULL, NULL, NULL}};
    sl::ResourceTag tag[4] = {{&res[0], 0, sl::ResourceLifecycle::eOnlyValidNow}, {&res[1], 0, sl::ResourceLifecycle::eOnlyValidNow},
                              {&res[2], 0, sl::ResourceLifecycle::eOnlyValidNow}, {&res[3], 0, sl::ResourceLifecycle::eOnlyValidNow}};
    for (int i = 0; i < 4; i++) {
        res[i] = sl::Resource(sl::ResourceType::eTex2d, (void *)img[i]->image, NULL, (void *)img[i]->view,
                              (uint32_t)VK_IMAGE_LAYOUT_GENERAL);
        res[i].width = img[i]->w, res[i].height = img[i]->h;
        res[i].nativeFormat = (uint32_t)img[i]->format;
        res[i].mipLevels = 1, res[i].arrayLayers = 1;
        res[i].usage = (uint32_t)img[i]->usage;
        res[i].flags = 0;
        sl::Extent e{0, 0, img[i]->w, img[i]->h};
        tag[i] = sl::ResourceTag(&res[i], type[i], sl::ResourceLifecycle::eOnlyValidNow, &e);
    }
    sl::CommandBuffer *slcb = reinterpret_cast<sl::CommandBuffer *>(cb);
    sl::Result r = S.set_tag(*ft, vp, tag, 4, slcb);
    if (r == sl::Result::eOk) {
        const sl::BaseStructure *in[] = {&vp};
        r = S.evaluate(sl::kFeatureDLSS, *ft, in, 1, slcb);
    }
    if (r != sl::Result::eOk) {
        static int logged;
        if (!logged++) rt_log("vk: DLSS: evaluation failed (%s)", result_name(r));
        return 0;
    }
    return 1;
}

extern "C" void vk_sl_shutdown(void) {
    if (S.started && S.shutdown) S.shutdown();
    S.started = S.ready = 0;
}

#else   // Streamline is Windows only

extern "C" int vk_sl_start(void) { return 0; }
extern "C" uint32_t vk_sl_instance_exts(const char **out, uint32_t max) { (void)out, (void)max; return 0; }
extern "C" uint32_t vk_sl_device_exts(const char **out, uint32_t max) { (void)out, (void)max; return 0; }
extern "C" void vk_sl_device(VkInstance inst, VkPhysicalDevice phys, VkDevice dev, uint32_t family, int exts_ok) {
    (void)inst, (void)phys, (void)dev, (void)family, (void)exts_ok;
}
extern "C" int vk_sl_ready(void) { return 0; }
extern "C" PFN_vkVoidFunction vk_sl_proc(VkDevice dev, const char *name) { (void)dev, (void)name; return NULL; }
extern "C" int vk_sl_evaluate(VkCommandBuffer cb, const VkSlImage *color, const VkSlImage *depth, const VkSlImage *motion,
                              const VkSlImage *out, const float jitter[2], float near_z, float fov_y, int reset, int dlaa) {
    (void)cb, (void)color, (void)depth, (void)motion, (void)out, (void)jitter, (void)near_z, (void)fov_y, (void)reset, (void)dlaa;
    return 0;
}
extern "C" void vk_sl_shutdown(void) {}

#endif
