// NVIDIA DLSS and DLSS Frame Generation through Streamline: DLSS (XC3_UPSCALER=dlss or dlaa) is the second back end of
// the temporal upscaler (vk_tup.c); DLSS-G (XC3_FRAME_GEN=dlss) is one of the two frame generators (vk_fg.c).
//
// Streamline 2.x (MIT; headers in third_party/streamline, unmodified) is NVIDIA's interface to DLSS: sl.interposer.dll
// and its signed plugins (sl.common.dll with NvLowLatencyVk.dll, sl.dlss.dll and the model, nvngx_dlss.dll; for frame
// generation sl.dlss_g.dll with nvngx_dlssg.dll, sl.reflex.dll and sl.pcl.dll) sit next to xc3.exe
// (tools/fetch_upscalers.py puts them there). It is loaded at run time, never linked, and used in its "manual hooking"
// mode: the renderer keeps calling vulkan-1.dll itself and only routes the calls Streamline must see through it.
//
// - vk_sl_start, before the Vulkan instance exists, loads the interposer, starts Streamline with the features asked for
//   and returns the instance and device extensions and the queues they ask for (vk_device.c adds those the driver
//   has, and the queues after its own one in the graphics family).
// - vk_sl_device, once the device exists, hands Streamline the instance, the device and the queues.
// - The present and the swapchain calls go through Streamline's proxies (vk_sl_proc, gfx_present.c): its common plugin
//   counts frames at the present, a present that bypasses it leaks memory, and DLSS-G's proxy swapchain adds the
//   generated frame there.
// - vk_sl_evaluate tags the inputs vk_tup.c prepared (colour, depth, motion vectors, output), sets the camera
//   constants and the DLSS mode, and records DLSS into the command buffer. Streamline moves the tagged images between
//   layouts and back itself; it records straight into the buffer, so the recorder thread is drained first (vk_tup.c).
//
// Frame generation. Every Streamline call of a frame names it by a frame token, here the id vk_fg.c gives the frame
// the replay records and its present (vk_fg_frame). DLSS-G reads the depth and the motion vectors tagged for that id
// (vk_sl_evaluate, or vk_sl_fg_inputs when FSR upscales), copied at once (eOnlyValidNow: our images are rewritten by
// the next frame before this one is presented), the camera constants of the id, and the back buffer at the present.
// The latency markers it requires (sl.pcl) go at the replay's frame boundaries and around the present, and Reflex,
// which it requires on, sleeps at the start of each frame (vk_sl_fg_frame_start).
//
// Reflex. Its low-latency mode (XC3_REFLEX: 0 off, 1 on, 2 on with boost; live) also runs with DLSS alone, when frame
// generation is off: the same sleep and markers at the same places, so the game starts a frame only as early as the GPU
// can take it. DLSS-G keeps it at least on. Streamline sets the mode on the swapchain it saw last
// (VK_NV_low_latency2) and drops it with that swapchain, so it is set again after each one (vk_sl_swapchain_made), as
// NX2navi does (runtime/dlssg/nx2_sl.cpp, nx2sl_reflex).
//
// The DLSS model (XC3_DLSS_PRESET, live): default leaves NVIDIA's model per mode, a letter takes that one in every
// mode (vk_sl_evaluate sets the options again when it changes). A frame without tagged inputs
// (loading screens, movies, the scene not drawn) and the in-game menu turn generation off (sl::DLSSGMode::eOff, its
// resources kept) until two frames in a row have them again.
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
#include "third_party/streamline/sl_dlss_g.h"
#include "third_party/streamline/sl_helpers_vk.h"
#include "third_party/streamline/sl_pcl.h"
#include "third_party/streamline/sl_reflex.h"

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
using PfnDlssgSetOptions = sl::Result (*)(const sl::ViewportHandle &, const sl::DLSSGOptions &);
using PfnDlssgGetState = sl::Result (*)(const sl::ViewportHandle &, sl::DLSSGState &, const sl::DLSSGOptions *);
using PfnPclSetMarker = sl::Result (*)(sl::PCLMarker, const sl::FrameToken &);
using PfnReflexSleep = sl::Result (*)(const sl::FrameToken &);
using PfnReflexSetOptions = sl::Result (*)(const sl::ReflexOptions &);

#define MAX_EXTS 32
#define RING 16   // frames whose tagging state the present can still look up

struct {
    HMODULE lib;
    int started;     // slInit succeeded
    int ready;       // the device is set: the proxies work
    int dlss;        // DLSS runs on the device
    int fg;          // DLSS-G runs on the device
    int want_dlss, want_fg, want_reflex;
    int reflex;            // Reflex's functions are there: the sleep and the markers run while reflex_mode or fg
    int reflex_mode;       // the mode last set: 0 off, 1 on, 2 boost; -1 none yet
    unsigned reflex_cfg;   // cfg_version when XC3_REFLEX was read
    int preset;            // the DLSS model in every mode (sl::DLSSPreset), 0 NVIDIA's per mode
    unsigned preset_cfg;   // cfg_version when XC3_DLSS_PRESET was read
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
    PfnDlssgSetOptions dlssg_set_options;
    PfnDlssgGetState dlssg_get_state;
    PfnPclSetMarker pcl_marker;
    PfnReflexSleep reflex_sleep;
    PfnReflexSetOptions reflex_set_options;
    PFN_vkGetDeviceProcAddr device_proc;
    PFN_vkGetInstanceProcAddr instance_proc;
    const char *inst_exts[MAX_EXTS], *dev_exts[MAX_EXTS];
    uint32_t n_inst_exts, n_dev_exts;
    uint32_t gfx_queues, compute_queues;   // the extra queues the features ask for
    uint32_t last_mode, last_w, last_h;
    int last_preset;
    // frame generation, by frame id
    volatile uint64_t tagged[RING];        // id + 1 when the frame's depth and motion were tagged
    volatile uint64_t constants[RING];     // id + 1 when the frame's camera constants were set
    sl::Constants last_k;                  // the newest constants, for a frame that set none
    volatile int32_t have_last_k;
    int fg_on;                             // the mode last set: 1 on
    uint32_t fg_w, fg_h, fg_rw, fg_rh;     // the sizes the options were last set for
    int fg_set;                            // options set once
    uint64_t fg_last_status_check;
    uint64_t fg_shown;   // frames DLSS-G presented, drawn and generated (vk_sl_fg_present)
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
    case sl::Result::eErrorAdapterNotSupported: return "the GPU does not support it";
    case sl::Result::eErrorNoPlugins: return "no Streamline plugins next to xc3.exe";
    case sl::Result::eErrorVulkanAPI: return "a Vulkan call failed";
    case sl::Result::eErrorFeatureMissing: return "its plugin did not load";
    case sl::Result::eErrorFeatureNotSupported: return "it is not supported here";
    case sl::Result::eErrorMissingInputParameter: return "an input is missing";
    case sl::Result::eErrorNotInitialized: return "Streamline is not started";
    default: return "an error";
    }
}

void log_cb(sl::LogType type, const char *msg) {
    static int debug = -1;   // XC3_TUP_DEBUG: every line, info too
    if (debug < 0) debug = cfg_get("XC3_TUP_DEBUG") != NULL;
    if (type == sl::LogType::eInfo && !debug) return;
    char b[512];
    size_t n = strlen(msg);
    if (n >= sizeof b) n = sizeof b - 1;
    memcpy(b, msg, n);
    while (n && (b[n - 1] == '\n' || b[n - 1] == '\r')) n--;
    b[n] = 0;
    // A warning that repeats every frame would flood the log: after 200 lines only errors are written.
    static volatile LONG lines;
    if (type != sl::LogType::eError && !debug && InterlockedIncrement(&lines) > 200) return;
    rt_log("vk: Streamline %s: %s", type == sl::LogType::eError ? "error" : "warning", b);
}

template <class T> T sym(const char *name) { return reinterpret_cast<T>(reinterpret_cast<void *>(GetProcAddress(S.lib, name))); }

template <class T> T feature_fn(sl::Feature f, const char *name) {
    void *p = NULL;
    if (S.feature_function(f, name, p) != sl::Result::eOk) return NULL;
    return reinterpret_cast<T>(p);
}

// The folder of xc3.exe as a wide string with a trailing separator.
int exe_dir(wchar_t *out, size_t n) {
    DWORD k = GetModuleFileNameW(NULL, out, (DWORD)n);
    if (!k || k >= n) return 0;
    while (k && out[k - 1] != L'\\' && out[k - 1] != L'/') k--;
    out[k] = 0;
    return k > 0;
}

void add_exts(const char **list, uint32_t *n, const char *const *add, uint32_t m) {
    for (uint32_t i = 0; i < m; i++) {
        int dup = 0;
        for (uint32_t j = 0; j < *n; j++) dup |= !strcmp(list[j], add[i]);
        if (!dup && *n < MAX_EXTS) list[(*n)++] = add[i];
    }
}

// The requirements of one feature merged into ours. 0: it cannot run.
int require(sl::Feature f, const char *name) {
    sl::FeatureRequirements req{};
    sl::Result r = S.requirements(f, req);
    if (r != sl::Result::eOk) {
        rt_log("vk: %s off: no requirements (%s)", name, result_name(r));
        return 0;
    }
    add_exts(S.inst_exts, &S.n_inst_exts, req.vkInstanceExtensions, req.vkNumInstanceExtensions);
    add_exts(S.dev_exts, &S.n_dev_exts, req.vkDeviceExtensions, req.vkNumDeviceExtensions);
    if (cfg_get("XC3_TUP_DEBUG")) {
        for (uint32_t i = 0; i < req.vkNumFeatures12; i++)
            rt_log("vk: %s asks for the Vulkan 1.2 feature %s", name, req.vkFeatures12[i]);
        for (uint32_t i = 0; i < req.vkNumFeatures13; i++)
            rt_log("vk: %s asks for the Vulkan 1.3 feature %s", name, req.vkFeatures13[i]);
        for (uint32_t i = 0; i < req.vkNumDeviceExtensions; i++)
            rt_log("vk: %s asks for the device extension %s", name, req.vkDeviceExtensions[i]);
    }
    S.gfx_queues += req.vkNumGraphicsQueuesRequired;
    S.compute_queues += req.vkNumComputeQueuesRequired;
    return 1;
}

sl::FrameToken *token(uint64_t id) {
    sl::FrameToken *ft = NULL;
    uint32_t idx = (uint32_t)id;
    if (S.new_frame_token(ft, &idx) != sl::Result::eOk) return NULL;
    return ft;
}

// Reflex paces this frame: DLSS-G runs, or the low-latency mode is on.
int reflex_runs() { return S.reflex && (S.fg || S.reflex_mode > 0); }

void marker(sl::PCLMarker m, uint64_t id) {
    if (!reflex_runs()) return;
    if (sl::FrameToken *ft = token(id)) S.pcl_marker(m, *ft);
}

// Camera constants for a w x h render with the game's projection: depth from 0 at the near plane to 1 at infinity,
// no jitter in the matrices (row-major), motion vectors as UV offsets to the previous frame.
void make_constants(sl::Constants &k, uint32_t w, uint32_t h, const float jitter[2], float near_z, float fov_y, int reset) {
    float aspect = (float)w / (float)h, t = 1.0f / tanf(fov_y * 0.5f);
    k = sl::Constants{};
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
}

int set_constants(const sl::FrameToken &ft, uint64_t id, const sl::Constants &k) {
    sl::ViewportHandle vp(0u);
    if (S.set_constants(k, ft, vp) != sl::Result::eOk) return 0;
    S.last_k = k;
    InterlockedExchange((volatile LONG *)&S.have_last_k, 1);
    InterlockedExchange64((volatile LONG64 *)&S.constants[id % RING], (LONG64)(id + 1));
    return 1;
}

sl::Resource resource(const VkSlImage *i) {
    sl::Resource r(sl::ResourceType::eTex2d, (void *)i->image, NULL, (void *)i->view, (uint32_t)VK_IMAGE_LAYOUT_GENERAL);
    r.width = i->iw ? i->iw : i->w, r.height = i->ih ? i->ih : i->h;   // the extent (tag) says the part in use
    r.nativeFormat = (uint32_t)i->format;
    r.mipLevels = 1, r.arrayLayers = 1;
    r.usage = (uint32_t)i->usage;
    r.flags = 0;
    return r;
}

// Tags n images (types[i]) of frame id, copied at once by Streamline where a later present needs them.
sl::Result tag(const sl::FrameToken &ft, VkCommandBuffer cb, const VkSlImage *const *img, const sl::BufferType *type, int n) {
    sl::Resource res[4];
    sl::Extent ext[4];
    sl::ResourceTag tags[4];
    for (int i = 0; i < n; i++) {
        res[i] = resource(img[i]);
        ext[i] = sl::Extent{0, 0, img[i]->w, img[i]->h};
        tags[i] = sl::ResourceTag(&res[i], type[i], sl::ResourceLifecycle::eOnlyValidNow, &ext[i]);
    }
    sl::ViewportHandle vp(0u);
    return S.set_tag(ft, vp, tags, (uint32_t)n, reinterpret_cast<sl::CommandBuffer *>(cb));
}

void mark_tagged(uint64_t id) { InterlockedExchange64((volatile LONG64 *)&S.tagged[id % RING], (LONG64)(id + 1)); }
int was_tagged(uint64_t id) { return (uint64_t)InterlockedCompareExchange64((volatile LONG64 *)&S.tagged[id % RING], 0, 0) == id + 1; }
int had_constants(uint64_t id) {
    return (uint64_t)InterlockedCompareExchange64((volatile LONG64 *)&S.constants[id % RING], 0, 0) == id + 1;
}

// XC3_REFLEX set as Streamline's Reflex mode: when the setting changed, and with force (a new swapchain) always.
// DLSS-G requires it on, so 0 means on while generation runs.
void reflex_apply(int force) {
    if (!S.reflex) return;
    unsigned v = cfg_version();
    if (!force && v == S.reflex_cfg) return;
    S.reflex_cfg = v;
    int m = cfg_int("XC3_REFLEX");
    m = m < 0 ? 0 : m > 2 ? 2 : m;
    if (S.fg && !m) m = 1;
    if (!force && m == S.reflex_mode) return;
    sl::ReflexOptions o{};
    o.mode = m == 2 ? sl::ReflexMode::eLowLatencyWithBoost : m ? sl::ReflexMode::eLowLatency : sl::ReflexMode::eOff;
    sl::Result r = S.reflex_set_options(o);
    static int n;
    if (m != S.reflex_mode && n++ < 8)
        rt_log("vk: Reflex low-latency mode %s (%s)", m == 2 ? "on with boost" : m ? "on" : "off", result_name(r));
    S.reflex_mode = m;
}

// XC3_DLSS_PRESET as Streamline's preset (sl::DLSSPreset: J 10, K 11, L 12, M 13), read again when a setting changed.
int dlss_preset() {
    unsigned v = cfg_version();
    if (v != S.preset_cfg) {
        S.preset_cfg = v;
        const char *p = cfg_value("XC3_DLSS_PRESET");
        S.preset = !p || !p[0] || p[1] ? 0 : p[0] == 'j' ? 10 : p[0] == 'k' ? 11 : p[0] == 'l' ? 12 : p[0] == 'm' ? 13 : 0;
    }
    return S.preset;
}

} // namespace

extern "C" int vk_sl_start(int dlss, int fg, int reflex) {
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
    // DLSS-G needs Reflex (its latency markers come from sl.pcl, which Streamline always loads); DLSS alone takes it
    // when reflex is asked for (frame generation off: FSR 3's swapchain does not go through Streamline).
    static sl::Feature features[3];
    uint32_t nf = 0;
    reflex = fg || (dlss && reflex);
    if (dlss) features[nf++] = sl::kFeatureDLSS;
    if (fg) features[nf++] = sl::kFeatureDLSS_G;
    if (reflex) features[nf++] = sl::kFeatureReflex;
    if (!nf) return 0;
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
    pref.numFeaturesToLoad = nf;
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
    S.want_dlss = dlss && require(sl::kFeatureDLSS, "DLSS");
    S.want_reflex = reflex && require(sl::kFeatureReflex, "Reflex");
    S.want_fg = fg && S.want_reflex && require(sl::kFeatureDLSS_G, "DLSS Frame Generation");
    S.reflex_mode = -1;
    if (!S.want_dlss && !S.want_fg) {
        S.shutdown();
        S.started = 0;
        return 0;
    }
    S.failed = 0;
    rt_log("vk: Streamline started for%s%s: %u instance and %u device extensions, %u graphics and %u compute queues "
           "asked for", S.want_dlss ? " DLSS" : "", S.want_fg ? " DLSS Frame Generation" : "", S.n_inst_exts,
           S.n_dev_exts, S.gfx_queues, S.compute_queues);
    return 1;
}

extern "C" uint32_t vk_sl_instance_exts(const char **out, uint32_t max) {
    uint32_t n = 0;
    for (uint32_t i = 0; S.started && i < S.n_inst_exts && n < max; i++) out[n++] = S.inst_exts[i];
    return n;
}

extern "C" uint32_t vk_sl_device_exts(const char **out, uint32_t max) {
    uint32_t n = 0;
    for (uint32_t i = 0; S.started && i < S.n_dev_exts && n < max; i++) out[n++] = S.dev_exts[i];
    return n;
}

// With DLSS-G the instance and the device are made through Streamline's proxies: they add its extensions, features,
// queues and the native optical flow queue DLSS-G wants (its own families), which our manual set-up lacks. DLSS
// alone keeps the manual set-up (vk_sl_device's slSetVulkanInfo).
extern "C" PFN_vkCreateInstance vk_sl_create_instance(void) {
    PFN_vkCreateInstance f = S.started && S.want_fg ? sym<PFN_vkCreateInstance>("vkCreateInstance") : NULL;
    if (S.started && S.want_fg && !f) rt_log("vk: DLSS Frame Generation: sl.interposer.dll lacks vkCreateInstance");
    return f;
}

extern "C" PFN_vkEnumeratePhysicalDevices vk_sl_enumerate_physical_devices(void) {
    return S.started && S.want_fg ? sym<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices") : NULL;
}

extern "C" PFN_vkCreateDevice vk_sl_create_device(VkInstance inst) {
    (void)inst;
    return S.started && S.want_fg ? sym<PFN_vkCreateDevice>("vkCreateDevice") : NULL;
}

extern "C" uint32_t vk_sl_extra_queues(void) { return S.started && !S.want_fg ? S.gfx_queues + S.compute_queues : 0; }

extern "C" void vk_sl_device(VkInstance inst, VkPhysicalDevice phys, VkDevice dev, uint32_t family, uint32_t first_queue,
                             int exts_ok) {
    if (!S.started) return;
    if (!exts_ok) {
        rt_log("vk: DLSS off: the device lacks an extension Streamline needs");
        S.failed = 1;
        return;
    }
    sl::VulkanInfo info{};
    info.instance = inst, info.physicalDevice = phys, info.device = dev;
    info.graphicsQueueFamily = info.computeQueueFamily = family;
    // Streamline's queues follow ours in the graphics family: its graphics queues first, then its compute ones.
    info.graphicsQueueIndex = first_queue;
    info.computeQueueIndex = first_queue + S.gfx_queues;
    sl::Result r = S.want_fg ? sl::Result::eOk : S.set_vulkan_info(info);   // the device proxy knows it all
    if (r != sl::Result::eOk) {
        rt_log("vk: DLSS off: Streamline refused the device (%s)", result_name(r));
        S.failed = 1;
        return;
    }
    S.ready = 1;
    sl::AdapterInfo ai{};
    ai.vkPhysicalDevice = phys;
    if (S.want_reflex) {
        if (S.is_supported && (r = S.is_supported(sl::kFeatureReflex, ai)) != sl::Result::eOk)
            rt_log("vk: Reflex off: %s", result_name(r));
        else if (!(S.pcl_marker = feature_fn<PfnPclSetMarker>(sl::kFeaturePCL, "slPCLSetMarker")) ||
                 !(S.reflex_sleep = feature_fn<PfnReflexSleep>(sl::kFeatureReflex, "slReflexSleep")) ||
                 !(S.reflex_set_options = feature_fn<PfnReflexSetOptions>(sl::kFeatureReflex, "slReflexSetOptions")))
            rt_log("vk: Reflex off: a plugin lacks its entry points");
        else
            S.reflex = 1;
    }
    if (S.want_dlss) {
        if (S.is_supported && (r = S.is_supported(sl::kFeatureDLSS, ai)) != sl::Result::eOk)
            rt_log("vk: DLSS off: %s", result_name(r));
        else if (!(S.dlss_set_options = feature_fn<PfnDlssSetOptions>(sl::kFeatureDLSS, "slDLSSSetOptions")))
            rt_log("vk: DLSS off: the DLSS plugin lacks slDLSSSetOptions");
        else {
            S.dlss = 1;
            rt_log("vk: DLSS is ready");
        }
    }
    if (S.want_fg) {
        if (S.is_supported && (r = S.is_supported(sl::kFeatureDLSS_G, ai)) != sl::Result::eOk)
            rt_log("vk: DLSS Frame Generation off: %s", result_name(r));
        else if (!S.reflex)
            rt_log("vk: DLSS Frame Generation off: it needs Reflex");
        else if (!(S.dlssg_set_options = feature_fn<PfnDlssgSetOptions>(sl::kFeatureDLSS_G, "slDLSSGSetOptions")))
            rt_log("vk: DLSS Frame Generation off: a plugin lacks its entry points");
        else {
            S.dlssg_get_state = feature_fn<PfnDlssgGetState>(sl::kFeatureDLSS_G, "slDLSSGGetState");
            S.fg = 1;
            rt_log("vk: DLSS Frame Generation is ready");
        }
    }
    reflex_apply(1);
    if (!S.dlss && !S.fg) {   // the proxies stay: the swapchain may already go through them
        rt_log("vk: Streamline runs with no feature");
    }
}

extern "C" int vk_sl_ready(void) { return S.dlss; }
extern "C" int vk_sl_fg_ready(void) { return S.fg; }

extern "C" PFN_vkVoidFunction vk_sl_proc(VkDevice dev, const char *name) {
    if (!S.ready) return NULL;
    return S.device_proc(dev, name);
}

// DLSS-G's swapchain hook finds the window of a swapchain by its surface, which it records when the surface is created
// through its proxy: the window's surface goes through this (gfx_window_win.c wsys_surface).
extern "C" PFN_vkVoidFunction vk_sl_instance_proc(VkInstance inst, const char *name) {
    if (!S.started || !S.want_fg) return NULL;
    return S.instance_proc(inst, name);
}

extern "C" int vk_sl_evaluate(VkCommandBuffer cb, uint64_t id, const VkSlImage *color, const VkSlImage *depth,
                              const VkSlImage *motion, const VkSlImage *out, const float jitter[2], float near_z,
                              float fov_y, int reset, int dlaa) {
    if (!S.dlss) return 0;
    sl::FrameToken *ft = token(id);
    if (!ft) return 0;
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
    int preset = dlss_preset();
    sl::DLSSPreset pr = (sl::DLSSPreset)preset;   // 0 is eDefault: NVIDIA's model per mode
    o.dlaaPreset = o.qualityPreset = o.balancedPreset = o.performancePreset = o.ultraPerformancePreset =
        o.ultraQualityPreset = pr;
    if ((uint32_t)o.mode != S.last_mode || out->w != S.last_w || out->h != S.last_h || preset != S.last_preset) {
        sl::Result r = S.dlss_set_options(vp, o);
        if (r != sl::Result::eOk) {
            static int logged;
            if (!logged++) rt_log("vk: DLSS: options refused (%s)", result_name(r));
            return 0;
        }
        S.last_mode = (uint32_t)o.mode, S.last_w = out->w, S.last_h = out->h, S.last_preset = preset;
        static const char *const names[] = {"off", "performance", "balanced", "quality", "ultra performance",
                                            "ultra quality", "DLAA"};
        uint32_t m = (uint32_t)o.mode;
        rt_log("vk: DLSS (%s, model %c) upscales %ux%u to %ux%u", m < 7 ? names[m] : "?",
               preset ? (char)('A' + preset - 1) : '-', color->w, color->h, out->w, out->h);
    }

    sl::Constants k;
    make_constants(k, color->w, color->h, jitter, near_z, fov_y, reset);
    if (!set_constants(*ft, id, k)) return 0;
    const VkSlImage *img[4] = {color, depth, motion, out};
    const sl::BufferType type[4] = {sl::kBufferTypeScalingInputColor, sl::kBufferTypeDepth, sl::kBufferTypeMotionVectors,
                                    sl::kBufferTypeScalingOutputColor};
    sl::Result r = tag(*ft, cb, img, type, 4);
    if (r == sl::Result::eOk) {
        mark_tagged(id);   // DLSS-G reads the same depth and motion tags
        const sl::BaseStructure *in[] = {&vp};
        r = S.evaluate(sl::kFeatureDLSS, *ft, in, 1, reinterpret_cast<sl::CommandBuffer *>(cb));
    }
    if (r != sl::Result::eOk) {
        static int logged;
        if (!logged++) rt_log("vk: DLSS: evaluation failed (%s)", result_name(r));
        return 0;
    }
    return 1;
}

extern "C" int vk_sl_fg_inputs(VkCommandBuffer cb, uint64_t id, const VkSlImage *depth, const VkSlImage *motion,
                               const float jitter[2], float near_z, float fov_y, int reset) {
    if (!S.fg) return 0;
    sl::FrameToken *ft = token(id);
    if (!ft) return 0;
    sl::Constants k;
    make_constants(k, depth->w, depth->h, jitter, near_z, fov_y, reset);
    if (!set_constants(*ft, id, k)) return 0;
    const VkSlImage *img[2] = {depth, motion};
    const sl::BufferType type[2] = {sl::kBufferTypeDepth, sl::kBufferTypeMotionVectors};
    sl::Result r = tag(*ft, cb, img, type, 2);
    if (r != sl::Result::eOk) {
        static int logged;
        if (!logged++) rt_log("vk: DLSS Frame Generation: tagging failed (%s)", result_name(r));
        return 0;
    }
    mark_tagged(id);
    return 1;
}

// The replay thread at the frame boundaries: the frame id's recording begins (Reflex sleeps here) and ends.
extern "C" void vk_sl_fg_frame_start(uint64_t id) {
    if (!reflex_runs()) return;
    if (sl::FrameToken *ft = token(id)) {
        S.reflex_sleep(*ft);
        S.pcl_marker(sl::PCLMarker::eSimulationStart, *ft);
        S.pcl_marker(sl::PCLMarker::eSimulationEnd, *ft);
        S.pcl_marker(sl::PCLMarker::eRenderSubmitStart, *ft);
    }
}

extern "C" void vk_sl_fg_frame_end(uint64_t id) { marker(sl::PCLMarker::eRenderSubmitEnd, id); }

// The present thread, before frame id goes to the proxy present: the options (generation on while this frame and the
// one before have their inputs and allow is set), constants for a frame that set none, the picture's rectangle in the
// w x h back buffer, and the present marker.
extern "C" void vk_sl_fg_present(uint64_t id, uint32_t w, uint32_t h, VkFormat fmt, uint32_t images, const int rect[4],
                                 uint32_t render_w, uint32_t render_h, int allow, const VkSlImage *hudless) {
    if (!S.fg) return;
    sl::FrameToken *ft = token(id);
    if (!ft) return;
    int on = allow && was_tagged(id) && id > 0 && was_tagged(id - 1);
    if (!had_constants(id) && InterlockedCompareExchange((volatile LONG *)&S.have_last_k, 0, 0)) {
        sl::Constants k = S.last_k;
        k.reset = sl::Boolean::eTrue;
        sl::ViewportHandle vp(0u);
        S.set_constants(k, *ft, vp);
    }
    if (on != S.fg_on || !S.fg_set || w != S.fg_w || h != S.fg_h || (on && (render_w != S.fg_rw || render_h != S.fg_rh))) {
        sl::DLSSGOptions o{};
        o.mode = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
        o.numFramesToGenerate = 1;
        o.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;   // short pauses (menus, loading) do not reallocate
        o.numBackBuffers = images;
        o.colorWidth = w, o.colorHeight = h;
        o.colorBufferFormat = (uint32_t)fmt;
        if (render_w && render_h) {
            o.mvecDepthWidth = render_w, o.mvecDepthHeight = render_h;
            o.mvecBufferFormat = (uint32_t)VK_FORMAT_R16G16_SFLOAT;
            o.depthBufferFormat = (uint32_t)VK_FORMAT_R32_SFLOAT;
        }
        sl::ViewportHandle vp(0u);
        sl::Result r = S.dlssg_set_options(vp, o);
        if (r != sl::Result::eOk) {
            static int logged;
            if (!logged++) rt_log("vk: DLSS Frame Generation: options refused (%s)", result_name(r));
        } else {
            if (on != S.fg_on || !S.fg_set)
                rt_log("vk: DLSS Frame Generation %s (frame %llu, %ux%u, inputs %ux%u)", on ? "on" : "off",
                       (unsigned long long)id, w, h, render_w, render_h);
            S.fg_on = on, S.fg_set = 1, S.fg_w = w, S.fg_h = h, S.fg_rw = render_w, S.fg_rh = render_h;
        }
    }
    if (on && hudless) {
        // The frame without the interface, placed as the back buffer's picture: DLSS-G keeps what differs (the HUD)
        // out of the frames it generates. It is read at this present (eValidUntilPresent).
        sl::Resource res = resource(hudless);
        sl::Extent e{0, 0, hudless->w, hudless->h};
        if (rect) e = sl::Extent{(uint32_t)rect[1], (uint32_t)rect[0], (uint32_t)rect[2], (uint32_t)rect[3]};
        sl::ResourceTag t(&res, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &e);
        sl::ViewportHandle vp(0u);
        sl::Result r = S.set_tag(*ft, vp, &t, 1, NULL);
        static int n;
        if (r != sl::Result::eOk && n++ < 4) rt_log("vk: DLSS Frame Generation: the HUD-less tag failed (%s)", result_name(r));
    }
    if (rect && (rect[0] || rect[1] || (uint32_t)rect[2] != w || (uint32_t)rect[3] != h)) {
        // the picture between black bars: generation runs on its rectangle only
        sl::Extent e{(uint32_t)rect[1], (uint32_t)rect[0], (uint32_t)rect[2], (uint32_t)rect[3]};
        sl::ResourceTag t(NULL, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent, &e);
        sl::ViewportHandle vp(0u);
        S.set_tag(*ft, vp, &t, 1, NULL);
    }
    // Every present: the frames DLSS-G put on screen since the last one (drawn and generated), for the overlay; while
    // on, a status other than ok (the reasons DLSS-G gives for not running) goes to the log, once a second at most.
    if (S.dlssg_get_state && S.fg_set) {
        sl::DLSSGState st{};
        sl::ViewportHandle vp(0u);
        if (S.dlssg_get_state(vp, st, NULL) == sl::Result::eOk) {
            __atomic_add_fetch(&S.fg_shown, st.numFramesActuallyPresented, __ATOMIC_RELAXED);
            if (on && st.status != sl::DLSSGStatus::eOk && id - S.fg_last_status_check >= 120) {
                S.fg_last_status_check = id;
                static int n;
                if (n++ < 8) rt_log("vk: DLSS Frame Generation status %#x", (unsigned)st.status);
            }
        }
    }
    reflex_apply(0);
    S.pcl_marker(sl::PCLMarker::ePresentStart, *ft);
}

extern "C" int vk_sl_reflex_ready(void) { return S.reflex && !S.fg; }

extern "C" void vk_sl_reflex_present(uint64_t id) {
    if (!S.reflex || S.fg) return;
    reflex_apply(0);
    marker(sl::PCLMarker::ePresentStart, id);
}

extern "C" void vk_sl_swapchain_made(void) { reflex_apply(1); }

extern "C" int vk_sl_reflex_mode(void) { return reflex_runs() ? (S.reflex_mode < 1 ? 1 : S.reflex_mode) : 0; }

extern "C" void vk_sl_fg_present_end(uint64_t id) { marker(sl::PCLMarker::ePresentEnd, id); }

extern "C" int vk_sl_fg_active(void) { return S.fg && S.fg_on; }

extern "C" uint64_t vk_sl_fg_shown(void) { return __atomic_load_n(&S.fg_shown, __ATOMIC_RELAXED); }

extern "C" void vk_sl_fg_release(VkDevice dev) {
    if (!S.fg) return;
    if (S.fg_set) {
        sl::DLSSGOptions o{};
        o.mode = sl::DLSSGMode::eOff;   // no eRetainResourcesWhenOff: its resources go now, ahead of the swapchain
        sl::ViewportHandle vp(0u);
        S.dlssg_set_options(vp, o);
        S.fg_on = S.fg_set = 0;
    }
    PFN_vkDeviceWaitIdle idle = (PFN_vkDeviceWaitIdle)S.device_proc(dev, "vkDeviceWaitIdle");
    (idle ? idle : vkDeviceWaitIdle)(dev);
}

extern "C" void vk_sl_shutdown(void) {
    if (S.started && S.shutdown) S.shutdown();
    S.started = S.ready = S.dlss = S.fg = S.reflex = 0;
}

#else   // Streamline is Windows only

extern "C" int vk_sl_start(int dlss, int fg, int reflex) { (void)dlss, (void)fg, (void)reflex; return 0; }
extern "C" int vk_sl_reflex_ready(void) { return 0; }
extern "C" void vk_sl_reflex_present(uint64_t id) { (void)id; }
extern "C" void vk_sl_swapchain_made(void) {}
extern "C" int vk_sl_reflex_mode(void) { return 0; }
extern "C" uint32_t vk_sl_instance_exts(const char **out, uint32_t max) { (void)out, (void)max; return 0; }
extern "C" uint32_t vk_sl_device_exts(const char **out, uint32_t max) { (void)out, (void)max; return 0; }
extern "C" uint32_t vk_sl_extra_queues(void) { return 0; }
extern "C" PFN_vkCreateInstance vk_sl_create_instance(void) { return NULL; }
extern "C" PFN_vkEnumeratePhysicalDevices vk_sl_enumerate_physical_devices(void) { return NULL; }
extern "C" PFN_vkCreateDevice vk_sl_create_device(VkInstance inst) { (void)inst; return NULL; }
extern "C" void vk_sl_device(VkInstance inst, VkPhysicalDevice phys, VkDevice dev, uint32_t family, uint32_t first_queue,
                             int exts_ok) {
    (void)inst, (void)phys, (void)dev, (void)family, (void)first_queue, (void)exts_ok;
}
extern "C" int vk_sl_ready(void) { return 0; }
extern "C" int vk_sl_fg_ready(void) { return 0; }
extern "C" PFN_vkVoidFunction vk_sl_proc(VkDevice dev, const char *name) { (void)dev, (void)name; return NULL; }
extern "C" PFN_vkVoidFunction vk_sl_instance_proc(VkInstance inst, const char *name) { (void)inst, (void)name; return NULL; }
extern "C" int vk_sl_evaluate(VkCommandBuffer cb, uint64_t id, const VkSlImage *color, const VkSlImage *depth,
                              const VkSlImage *motion, const VkSlImage *out, const float jitter[2], float near_z,
                              float fov_y, int reset, int dlaa) {
    (void)cb, (void)id, (void)color, (void)depth, (void)motion, (void)out, (void)jitter, (void)near_z, (void)fov_y,
        (void)reset, (void)dlaa;
    return 0;
}
extern "C" int vk_sl_fg_inputs(VkCommandBuffer cb, uint64_t id, const VkSlImage *depth, const VkSlImage *motion,
                               const float jitter[2], float near_z, float fov_y, int reset) {
    (void)cb, (void)id, (void)depth, (void)motion, (void)jitter, (void)near_z, (void)fov_y, (void)reset;
    return 0;
}
extern "C" void vk_sl_fg_frame_start(uint64_t id) { (void)id; }
extern "C" void vk_sl_fg_frame_end(uint64_t id) { (void)id; }
extern "C" void vk_sl_fg_present(uint64_t id, uint32_t w, uint32_t h, VkFormat fmt, uint32_t images, const int rect[4],
                                 uint32_t render_w, uint32_t render_h, int allow, const VkSlImage *hudless) {
    (void)id, (void)w, (void)h, (void)fmt, (void)images, (void)rect, (void)render_w, (void)render_h, (void)allow,
        (void)hudless;
}
extern "C" void vk_sl_fg_present_end(uint64_t id) { (void)id; }
extern "C" int vk_sl_fg_active(void) { return 0; }
extern "C" uint64_t vk_sl_fg_shown(void) { return 0; }
extern "C" void vk_sl_fg_release(VkDevice dev) { (void)dev; }
extern "C" void vk_sl_shutdown(void) {}

#endif
