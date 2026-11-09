// Temporal upscaling in place of the game's own: XC3_UPSCALER=fsr3 runs AMD FidelityFX Super Resolution 3.1, dlss and
// dlaa run NVIDIA DLSS (vk_sl.cpp), where the game reconstructs its scene.
//
// The game draws the scene and its post effects at half the game resolution (960x544 at 1920x1080; with the patch set
// "scene", resolution.c, at the render scale instead), tonemaps it, and brings it to the game resolution with a 2x2
// temporal reconstruction of its own: five full-screen draws (fragment shaders 8a739994fe8f27fd, 3860d1fb663cc4c7,
// c7ccb6fe83468b61, f1c308ed7843326d and 9a7f297f8b22180b, named by the FNV-1a hash of their SPIR-V as vk_dump.c prints
// it) that read the tonemapped colour, the depth, the previous depth, the motion vector target and the previous output,
// and write the output, which a copy (fragment shader de3f095d826afe46) then brings into the frame the interface is drawn
// over. The camera alternates a quarter-pixel jitter each frame: the projection's [0][2] and [1][2] in the camera block
// (c3 rows 3 and 4) are -+1/(2 w) and +-1/(2 h) of the render size w x h, and the first draw's block c4 holds the phase
// at [2].z (0: -1/(2 w), +1/(2 h)), the render size at [0].xy and the output size at [1].xy.
//
// vk_exec_draw.c prepare_draw hands each fragment shader to vk_tup_role. The first draw's textures (slot 0 colour,
// slot 2 motion, slot 3 depth) and constants are captured and the five draws are skipped; at the last one the replay
// records vk_tup_record in their place: a compute pass (shaders/tup_prep.comp) copies the colour and the depth into
// images of ours and decodes the motion vectors, FSR or DLSS runs on those, and its result is copied into the output
// target. A frame whose first draw cannot be captured keeps the game's reconstruction.
//
// With the scaled scene (render_scale, and upscaler = dlaa at the full size) the reconstruction writes twice the scene
// size and the game's copy halves that into a target of the scene size, which nothing brings into the frame. There
// the upscaler writes the frame size into an image of its own, and vk_exec_draw.c tup_fixup stretches it (or, without
// a temporal upscaler, the copy's target) into the frame before the interface is drawn: DLAA renders at 1:1, and
// render_scale 67 is DLSS's quality mode.
//
// FSR comes from AMD's signed amd_fidelityfx_vk.dll (FidelityFX SDK 1.1.4) next to xc3.exe, loaded on first use; the
// API headers in third_party/ffx_api are AMD's, unmodified (MIT), but for the Vulkan one, declared below. The DLL
// records its dispatches straight into the command buffer, so the recorder thread (vk_rec.h) is drained first: the
// calls stay in order. DLSS does the same through Streamline.
#include "vk.h"

#include <float.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
// ffx_api.h marks its entry points __declspec(dllexport) on every target; only the function pointer types are used
// here (the DLL is loaded at run time), so elsewhere the attribute is dropped for the include.
#ifndef _WIN32
#define __declspec(x)
#endif
#include "third_party/ffx_api/ffx_api.h"
#include "third_party/ffx_api/ffx_upscale.h"
#ifndef _WIN32
#undef __declspec
#endif
#include "tup_prep_spv.h"
#include "vk_fg.h"
#include "vk_rec.h"
#include "vk_sl.h"

#define SLOTS 8

// The Vulkan back-end description of ffx_api/vk/ffx_api_vk.h (FidelityFX SDK 1.1.4), which C cannot include: it names
// its structures without the struct tag. Same layout and type value.
#define FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK 0x0000003u
struct ffxCreateBackendVKDesc {
    ffxCreateContextDescHeader header;
    VkDevice vkDevice;
    VkPhysicalDevice vkPhysicalDevice;
    PFN_vkGetDeviceProcAddr vkDeviceProcAddr;
};

static const uint64_t ROLE_HASH[VK_TUP_ROLES] = {0, 0x8a739994fe8f27fdull, 0x3860d1fb663cc4c7ull,
                                                 0xc7ccb6fe83468b61ull, 0xf1c308ed7843326dull, 0x9a7f297f8b22180bull,
                                                 0xde3f095d826afe46ull};

typedef struct {
    VkImage image;
    VmaAllocation alloc;
    VkImageView view;
} Img;

// Frame generation's picture without the interface: the upscaler's output is the frame before the game draws its
// interface over it, so a copy of it per frame id (HL_RING of them, so the present of frame id reads its own while
// later frames are recorded) is what the generators compare the back buffer with to keep the HUD out of the frames
// they make (vk_tup_hudless; gfx_present.c scales it as it scales the picture).
#define HL_RING 4

static struct {
    Img img[HL_RING];
    uint64_t id[HL_RING];   // frame id + 1 whose picture img holds
    uint32_t w, h;
} HL;

static struct {
    int tried, ok;                      // ok: the input pass exists
    int ffx_tried;                      // amd_fidelityfx_vk.dll looked for (lib: found)
    int mode, logged_mode;              // VK_TUP_* of the frame before; the last one logged
    void *lib;
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxDispatch dispatch;
    PfnFfxQuery query;
    ffxContext ctx;
    uint32_t rw, rh, ow, oh;            // our images' sizes: the largest render size seen, the output size
    uint32_t crw, crh, cow, coh;        // the FSR context's sizes
    int reset;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline prep;
    VkSampler sampler;
    Img color, depth, motion, out;      // ours: render-size inputs, output-size result
    VkImageView src_view[SLOTS][3];     // the game images' views each slot's last frame read
    unsigned slot;
    uint64_t last_ns;
    uint64_t frames;
} T;

// ---- roles

// DLSS and DLAA need Streamline from the start (vk_device.c); without it they fall back to FSR 3.
int vk_tup_mode(void) {
    const char *u = cfg_value("XC3_UPSCALER");
    int dlss = !strcmp(u, "dlss"), dlaa = !strcmp(u, "dlaa");
    if ((dlss || dlaa) && vk_sl_ready()) return dlaa ? VK_TUP_DLAA : VK_TUP_DLSS;
    return dlss || dlaa || !strcmp(u, "fsr3") ? VK_TUP_FSR3 : VK_TUP_OFF;
}

static const char *mode_name(int m) { return m == VK_TUP_DLSS ? "DLSS" : m == VK_TUP_DLAA ? "DLAA" : "FSR 3"; }

static int knob(const char *key) {   // test keys, unset by default
    const char *v = cfg_get(key);
    return v && *v && strcmp(v, "0");
}

static uint64_t spirv_hash(const GfxShader *s) {
    uint64_t words = 0, h = 0xcbf29ce484222325ull;
    const uint8_t *b = (const uint8_t *)gfx_shader_spirv(s, &words);
    for (uint64_t i = 0; b && i < words * 4; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

int vk_tup_role(const GfxShader *fs) {
    static struct { const GfxShader *s; int role; } memo[256];
    if (!fs) return VK_TUP_NONE;
    unsigned i = (unsigned)(((uintptr_t)fs * 0x9E3779B97F4A7C15ull) >> 56);
    if (memo[i].s == fs) return memo[i].role;
    uint64_t h = spirv_hash(fs);
    int role = VK_TUP_NONE;
    for (int r = 1; r < VK_TUP_ROLES; r++)
        if (h == ROLE_HASH[r]) role = r;
    // The title screen's copy (and some cutscenes'): another shader doing the same job, the reconstruction's output
    // halved into the frame (on the scaled path, into a target of the scene size).
    if (h == 0xe98d22f95e30dbc6ull) role = VK_TUP_COPY;
    static unsigned logged;
    if (role && !(logged & 1u << role)) {
        logged |= 1u << role;
        if (role == VK_TUP_COPY && h == ROLE_HASH[VK_TUP_COPY])
            rt_log("vk: temporal upscale: the game's copy of its reconstruction found (fragment shader %016llx)",
                   (unsigned long long)h);
        else
            rt_log("vk: temporal upscale: the game's reconstruction draw %d found (fragment shader %016llx)", role,
                   (unsigned long long)h);
    }
    memo[i].s = fs, memo[i].role = role;
    return role;
}

// ---- set-up

static void ffx_message(uint32_t type, const wchar_t *msg) {
    char b[512];
    size_t n = 0;
    for (; msg && msg[n] && n < sizeof b - 1; n++) b[n] = msg[n] < 128 ? (char)msg[n] : '?';
    b[n] = 0;
    rt_log("vk: FSR %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", b);
}

static int load_ffx(void) {
    if (T.ffx_tried) return T.lib != NULL;
    T.ffx_tried = 1;
    char path[1024];
    if (!plat_exe_path(path, sizeof path - 64)) return 0;
    char *slash = strrchr(path, '\\');
    if (!slash) slash = strrchr(path, '/');
    if (!slash) return 0;
    strcpy(slash + 1, "amd_fidelityfx_vk.dll");
    T.lib = plat_lib_open(path);
    if (!T.lib) {
        rt_log("vk: FSR 3 off: %s is missing (tools/fetch_upscalers.py puts it there)", path);
        return 0;
    }
    T.create = (PfnFfxCreateContext)plat_lib_sym(T.lib, "ffxCreateContext");
    T.destroy = (PfnFfxDestroyContext)plat_lib_sym(T.lib, "ffxDestroyContext");
    T.dispatch = (PfnFfxDispatch)plat_lib_sym(T.lib, "ffxDispatch");
    T.query = (PfnFfxQuery)plat_lib_sym(T.lib, "ffxQuery");
    if (!T.create || !T.destroy || !T.dispatch || !T.query) {
        rt_log("vk: FSR 3 off: %s lacks the FidelityFX API entry points", path);
        T.lib = NULL;
        return 0;
    }
    return 1;
}

static int init(VkCtx *c) {
    T.tried = 1;
    T.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    if (!T.push) return 0;
    VkDescriptorSetLayoutBinding b[6];
    for (uint32_t i = 0; i < 6; i++)
        b[i] = (VkDescriptorSetLayoutBinding){.binding = i, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                              .descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                                      : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 6, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &T.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof tup_prep_spv,
                                   .pCode = tup_prep_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &T.dsl) != VK_SUCCESS ||
        vkCreatePipelineLayout(c->device, &li, NULL, &T.layout) != VK_SUCCESS ||
        vkCreateSampler(c->device, &si, NULL, &T.sampler) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS) {
        rt_log("vk: temporal upscale off: its input pass could not be created");
        return 0;
    }
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = T.layout};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &T.prep);
    vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) {
        rt_log("vk: temporal upscale off: its input pass could not be created");
        return 0;
    }
    return T.ok = 1;
}

static void img_free(VkCtx *c, Img *i) {
    if (i->view) vkDestroyImageView(c->device, i->view, NULL);
    if (i->image) vmaDestroyImage(c->vma, i->image, i->alloc);
    memset(i, 0, sizeof *i);
}

static int img_make(VkCtx *c, Img *i, VkFormat fmt, uint32_t w, uint32_t h) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ci, &ai, &i->image, &i->alloc, NULL) != VK_SUCCESS) return i->image = VK_NULL_HANDLE, 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = i->image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    return vkCreateImageView(c->device, &vi, NULL, &i->view) == VK_SUCCESS;
}

static void ffx_free(void) {
    if (T.ctx) T.destroy(&T.ctx, NULL);
    T.ctx = NULL;
    T.crw = T.crh = T.cow = T.coh = 0;
}

static void ctx_free(VkCtx *c) {
    ffx_free();
    img_free(c, &T.color), img_free(c, &T.depth), img_free(c, &T.motion), img_free(c, &T.out);
    T.rw = T.rh = T.ow = T.oh = 0;
}

// The FidelityFX Vulkan backend loads its device functions through this and calls some without a NULL check: the
// KHR aliases of functions core since Vulkan 1.1 (vkGetBufferMemoryRequirements2KHR), which the device has only when
// their extension is enabled, and the debug-utils names and labels, which exist only with the validation layer. The
// aliases fall back to the core name; the debug functions become no-ops.
static VkResult VKAPI_PTR ffx_no_name(VkDevice d, const VkDebugUtilsObjectNameInfoEXT *i) {
    (void)d, (void)i;
    return VK_SUCCESS;
}
static void VKAPI_PTR ffx_no_label(VkCommandBuffer cb, const VkDebugUtilsLabelEXT *l) { (void)cb, (void)l; }
static void VKAPI_PTR ffx_no_label_end(VkCommandBuffer cb) { (void)cb; }

static PFN_vkVoidFunction VKAPI_PTR ffx_proc(VkDevice d, const char *name) {
    PFN_vkVoidFunction f = vkGetDeviceProcAddr(d, name);
    if (f) return f;
    size_t n = strlen(name);
    if (n > 3 && n < 128 && !strcmp(name + n - 3, "KHR")) {
        char core[128];
        memcpy(core, name, n - 3), core[n - 3] = 0;
        if ((f = vkGetDeviceProcAddr(d, core))) return f;
    }
    if (!strcmp(name, "vkSetDebugUtilsObjectNameEXT")) return (PFN_vkVoidFunction)ffx_no_name;
    if (!strcmp(name, "vkCmdBeginDebugUtilsLabelEXT")) return (PFN_vkVoidFunction)ffx_no_label;
    if (!strcmp(name, "vkCmdEndDebugUtilsLabelEXT")) return (PFN_vkVoidFunction)ffx_no_label_end;
    if (knob("XC3_TUP_DEBUG")) rt_log("vk: FSR 3: the device has no %s", name);
    return NULL;
}

// Our images for a render size of rw x rh and an output of ow x oh. The render images only grow: a smaller render size
// (a scene drawn smaller than before) uses their top left part, so the upscaler keeps its context and its history. A new
// size waits for the device: earlier frames may still read the old images and the old FSR context's resources.
static int ensure_images(VkCtx *c, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh) {
    if (T.color.image && rw <= T.rw && rh <= T.rh && T.ow == ow && T.oh == oh) return 1;
    if (T.color.image && T.ow == ow && T.oh == oh) rw = rw > T.rw ? rw : T.rw, rh = rh > T.rh ? rh : T.rh;
    vk_queue_lock();
    vkDeviceWaitIdle(c->device);
    vk_queue_unlock();
    ctx_free(c);
    if (!img_make(c, &T.color, VK_FORMAT_R8G8B8A8_UNORM, rw, rh) || !img_make(c, &T.depth, VK_FORMAT_R32_SFLOAT, rw, rh) ||
        !img_make(c, &T.motion, VK_FORMAT_R16G16_SFLOAT, rw, rh) || !img_make(c, &T.out, VK_FORMAT_R8G8B8A8_UNORM, ow, oh)) {
        rt_log("vk: temporal upscale: its images could not be created");
        ctx_free(c);
        return 0;
    }
    T.rw = rw, T.rh = rh, T.ow = ow, T.oh = oh;
    T.reset = 1;
    return 1;
}

static int ensure_ffx(VkCtx *c) {
    if (T.ctx && T.crw == T.rw && T.crh == T.rh && T.cow == T.ow && T.coh == T.oh) return 1;
    if (!load_ffx()) return 0;
    ffx_free();
    struct ffxCreateBackendVKDesc be = {.header = {.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK},
                                        .vkDevice = c->device, .vkPhysicalDevice = c->phys,
                                        .vkDeviceProcAddr = ffx_proc};
    // The colour is tonemapped and gamma encoded; the depth grows with distance (0 near), toward a far plane the
    // projection puts at infinity.
    struct ffxCreateContextDescUpscale cd = {.header = {.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE,
                                                        .pNext = &be.header},
                                             .flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE | FFX_UPSCALE_ENABLE_DEPTH_INFINITE,
                                             .maxRenderSize = {T.rw, T.rh}, .maxUpscaleSize = {T.ow, T.oh},
                                             .fpMessage = ffx_message};
    if (knob("XC3_TUP_DEBUG")) cd.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
    ffxReturnCode_t rc = T.create(&T.ctx, &cd.header, NULL);
    if (rc != FFX_API_RETURN_OK) {
        rt_log("vk: FSR 3: ffxCreateContext failed (%u)", rc);
        T.ctx = NULL;
        return 0;
    }
    T.crw = T.rw, T.crh = T.rh, T.cow = T.ow, T.coh = T.oh;
    T.reset = 1;
    return 1;
}

// ---- per frame

static void barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                    VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa,
                               .dstStageMask = ds, .dstAccessMask = da, .oldLayout = from, .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                               .subresourceRange = {aspect, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                          .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

static VkImageView src_view(VkCtx *c, const VkTupImage *i, VkFormat fmt, VkImageAspectFlags aspect) {
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = i->image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                .subresourceRange = {aspect, 0, 1, 0, 1}};
    VkImageView v = VK_NULL_HANDLE;
    return vkCreateImageView(c->device, &vi, NULL, &v) == VK_SUCCESS ? v : VK_NULL_HANDLE;
}

static struct FfxApiResource res(const Img *i, uint32_t fmt, uint32_t w, uint32_t h, int uav) {
    struct FfxApiResource r = {.resource = (void *)i->image, .state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS};
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = fmt;
    r.description.width = w, r.description.height = h, r.description.depth = 1;
    r.description.mipCount = 1;
    r.description.usage = uav ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
    return r;
}

// T.out (ow x oh, just written by the upscaler, GENERAL) copied into the ring slot of frame id.
static void hudless_keep(VkCtx *c, VkCommandBuffer cb, uint64_t id, uint32_t ow, uint32_t oh) {
    if (HL.w != ow || HL.h != oh) {   // a new frame size: earlier presents may still read the old images
        vk_queue_lock();
        vkDeviceWaitIdle(c->device);
        vk_queue_unlock();
        for (int i = 0; i < HL_RING; i++) img_free(c, &HL.img[i]), HL.id[i] = 0;
        HL.w = HL.h = 0;
        for (int i = 0; i < HL_RING; i++)
            if (!img_make(c, &HL.img[i], VK_FORMAT_R8G8B8A8_UNORM, ow, oh)) {
                rt_log("vk: frame generation: the HUD-less picture's images could not be created");
                for (int k = 0; k < HL_RING; k++) img_free(c, &HL.img[k]);
                return;
            }
        HL.w = ow, HL.h = oh;
        rt_log("vk: the picture without the HUD is kept for frame generation and HDR (%ux%u)", ow, oh);
    }
    unsigned s = (unsigned)(id % HL_RING);
    const VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL;
    const VkPipelineStageFlags2 ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, XF = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier(cb, T.out.image, VK_IMAGE_ASPECT_COLOR_BIT, G, G, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, XF,
            VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier(cb, HL.img[s].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, G, ALL,
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, XF, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy cp = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {ow, oh, 1}};
    vkCmdCopyImage(cb, T.out.image, G, HL.img[s].image, G, 1, &cp);
    barrier(cb, HL.img[s].image, VK_IMAGE_ASPECT_COLOR_BIT, G, G, XF, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL,
            VK_ACCESS_2_MEMORY_READ_BIT);
    barrier(cb, T.out.image, VK_IMAGE_ASPECT_COLOR_BIT, G, G, XF, 0, ALL, 0);   // the next frame's writes wait for the read
    HL.id[s] = id + 1;
}

int vk_tup_record(VkCommandBuffer cb, const VkTupFrame *f) {
    VkCtx *c = vk_ctx();
    if (!T.tried) init(c);
    int mode = vk_tup_mode();
    if (!T.ok || mode == VK_TUP_OFF || f->color.format != VK_FORMAT_R8G8B8A8_UNORM ||
        (!f->own_out && f->out.format != VK_FORMAT_R8G8B8A8_UNORM))
        return 0;
    uint32_t rw = f->color.w, rh = f->color.h, ow = f->out.w, oh = f->out.h;
    if (f->rw && f->rh) {   // the game's dynamic resolution drew a smaller scene into the same targets
        static uint32_t lw, lh;
        if ((f->rw != lw || f->rh != lh) && knob("XC3_TUP_DEBUG"))
            rt_log("vk: temporal upscale: the scene drew %ux%u of %ux%u", f->rw, f->rh, rw, rh);
        lw = f->rw, lh = f->rh;
        rw = f->rw, rh = f->rh;
    }
    if (f->depth.w < rw || f->depth.h < rh || f->motion.w < rw || f->motion.h < rh || !ensure_images(c, rw, rh, ow, oh))
        return 0;
    if (mode == VK_TUP_FSR3 && !ensure_ffx(c)) return 0;
    if (mode != T.mode) T.reset = 1, T.mode = mode;   // the other upscaler's history is not this one's
    unsigned s = T.slot++ % SLOTS;
    for (int k = 0; k < 3; k++)
        if (T.src_view[s][k]) vkDestroyImageView(c->device, T.src_view[s][k], NULL), T.src_view[s][k] = VK_NULL_HANDLE;
    VkImageView *sv = T.src_view[s];
    sv[0] = src_view(c, &f->color, f->color.format, VK_IMAGE_ASPECT_COLOR_BIT);
    sv[1] = src_view(c, &f->depth, f->depth.format, VK_IMAGE_ASPECT_DEPTH_BIT);
    sv[2] = src_view(c, &f->motion, f->motion.format, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!sv[0] || !sv[1] || !sv[2]) return 0;

    const VkPipelineStageFlags2 ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                XF = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    const VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL;
    // the game's writes to the inputs become visible to the input pass; the previous frame's use of our images ends
    barrier(cb, f->color.image, VK_IMAGE_ASPECT_COLOR_BIT, G, G, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, CS,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    barrier(cb, f->depth.image, f->depth.aspect, G, G, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, CS,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    barrier(cb, f->motion.image, VK_IMAGE_ASPECT_COLOR_BIT, G, G, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, CS,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    const Img *ours[3] = {&T.color, &T.depth, &T.motion};
    for (int k = 0; k < 3; k++)
        barrier(cb, ours[k]->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, G, ALL,
                VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    VkDescriptorImageInfo ii[6] = {{T.sampler, sv[0], G}, {T.sampler, sv[1], G}, {T.sampler, sv[2], G},
                                   {VK_NULL_HANDLE, T.color.view, G}, {VK_NULL_HANDLE, T.depth.view, G},
                                   {VK_NULL_HANDLE, T.motion.view, G}};
    VkWriteDescriptorSet w[6];
    for (uint32_t i = 0; i < 6; i++)
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = i,
                                      .descriptorCount = 1, .pImageInfo = &ii[i],
                                      .descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                              : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    int32_t size[2] = {(int32_t)rw, (int32_t)rh};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, T.prep);
    rec_push(T.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, T.layout, 0, 6, w);
    vkCmdPushConstants(cb, T.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof size, size);
    vkCmdDispatch(cb, (rw + 7) / 8, (rh + 7) / 8, 1);
    for (int k = 0; k < 3; k++)
        barrier(cb, ours[k]->image, VK_IMAGE_ASPECT_COLOR_BIT, G, G, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS,
                VK_ACCESS_2_SHADER_READ_BIT);
    barrier(cb, T.out.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, G, ALL,
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    uint64_t now = kern_now_ns();
    float dt = T.last_ns && now > T.last_ns ? (float)((now - T.last_ns) / 1e6) : 16.7f;
    if (dt > 100) dt = 100;
    T.last_ns = now;
    int sharp = cfg_int("XC3_SHARPNESS");
    if (sharp < 0) sharp = 0;
    if (sharp > 100) sharp = 100;
    struct ffxDispatchDescUpscale d = {.header = {.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE}, .commandList = (void *)cb};
    d.color = res(&T.color, FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM, T.rw, T.rh, 0);   // renderSize: the part in use
    d.depth = res(&T.depth, FFX_API_SURFACE_FORMAT_R32_FLOAT, T.rw, T.rh, 0);
    d.motionVectors = res(&T.motion, FFX_API_SURFACE_FORMAT_R16G16_FLOAT, T.rw, T.rh, 0);
    d.output = res(&T.out, FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM, ow, oh, 1);
    d.jitterOffset.x = f->jitter[0], d.jitterOffset.y = f->jitter[1];
    d.motionVectorScale.x = (float)rw, d.motionVectorScale.y = (float)rh;
    d.renderSize.width = rw, d.renderSize.height = rh;
    d.upscaleSize.width = ow, d.upscaleSize.height = oh;
    d.enableSharpening = sharp > 0;
    d.sharpness = sharp / 100.0f;
    d.frameTimeDelta = dt;
    d.preExposure = 1.0f;
    d.reset = T.reset;
    d.cameraNear = f->near_z > 0 ? f->near_z : 0.1f;
    d.cameraFar = FLT_MAX; // what the backend expects with FFX_UPSCALE_ENABLE_DEPTH_INFINITE
    d.cameraFovAngleVertical = f->fov_y > 0 ? f->fov_y : 1.2f;
    d.viewSpaceToMetersFactor = 1.0f;
    if (knob("XC3_TUP_DEBUG_VIEW")) d.flags |= FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW;
    ffxReturnCode_t rc = FFX_API_RETURN_OK;
    int ok = 1, dlss_tagged = 0;
    if (knob("XC3_TUP_BYPASS")) {
        // test: our prepared colour, stretched, in place of FSR's output
        VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0}, {(int32_t)rw, (int32_t)rh, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0}, {(int32_t)ow, (int32_t)oh, 1}}};
        barrier(cb, T.color.image, VK_IMAGE_ASPECT_COLOR_BIT, G, G, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, XF,
                VK_ACCESS_2_TRANSFER_READ_BIT);
        barrier(cb, T.out.image, VK_IMAGE_ASPECT_COLOR_BIT, G, G, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, XF,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdBlitImage(cb, T.color.image, G, T.out.image, G, 1, &bl, VK_FILTER_LINEAR);
        barrier(cb, T.out.image, VK_IMAGE_ASPECT_COLOR_BIT, G, G, XF, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    } else if (mode == VK_TUP_FSR3) {
        rec_drain();   // the input pass is in the command buffer before the DLL records into it
        rc = T.dispatch(&T.ctx, &d.header);
        if (rc != FFX_API_RETURN_OK) {
            static int logged;
            if (!logged++) rt_log("vk: FSR 3: ffxDispatch failed (%u)", rc);
            ok = 0;
        }
    } else {
        const VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkSlImage ci = {T.color.image, T.color.view, VK_FORMAT_R8G8B8A8_UNORM, rw, rh, u, T.rw, T.rh},
                  di = {T.depth.image, T.depth.view, VK_FORMAT_R32_SFLOAT, rw, rh, u, T.rw, T.rh},
                  mi = {T.motion.image, T.motion.view, VK_FORMAT_R16G16_SFLOAT, rw, rh, u, T.rw, T.rh},
                  oi = {T.out.image, T.out.view, VK_FORMAT_R8G8B8A8_UNORM, ow, oh, u, 0, 0};
        rec_drain();   // as for FSR: Streamline records straight into the command buffer
        ok = dlss_tagged = vk_sl_evaluate(cb, vk_fg_frame(), &ci, &di, &mi, &oi, f->jitter, d.cameraNear,
                                          d.cameraFovAngleVertical, T.reset, mode == VK_TUP_DLAA);
    }
    if (ok && vk_fg_mode() != VK_FG_OFF) {
        // Frame generation reads the same prepared depth and motion (vk_fg.c), recorded after the upscaler's reads.
        const VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkSlImage di = {T.depth.image, T.depth.view, VK_FORMAT_R32_SFLOAT, rw, rh, u, T.rw, T.rh},
                  mi = {T.motion.image, T.motion.view, VK_FORMAT_R16G16_SFLOAT, rw, rh, u, T.rw, T.rh};
        rec_drain();
        vk_fg_inputs(cb, &di, &mi, f->jitter, d.cameraNear, d.cameraFovAngleVertical, T.reset, dt, dlss_tagged);
    }
    T.reset = 0;
    if (!ok) return 0;
    if (f->own_out) {   // the caller brings T.out into the frame (vk_tup_result)
        barrier(cb, T.out.image, VK_IMAGE_ASPECT_COLOR_BIT, G, G, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, ALL,
                VK_ACCESS_2_MEMORY_READ_BIT);
    } else {
        barrier(cb, T.out.image, VK_IMAGE_ASPECT_COLOR_BIT, G, G, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, XF,
                VK_ACCESS_2_TRANSFER_READ_BIT);
        barrier(cb, f->out.image, VK_IMAGE_ASPECT_COLOR_BIT, G, G, ALL, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                XF, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageCopy cp = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .extent = {ow, oh, 1}};
        vkCmdCopyImage(cb, T.out.image, G, f->out.image, G, 1, &cp);
        barrier(cb, f->out.image, VK_IMAGE_ASPECT_COLOR_BIT, G, G, XF, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL,
                VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    }
    // later passes that write the game's inputs wait for the input pass
    barrier(cb, f->color.image, VK_IMAGE_ASPECT_COLOR_BIT, G, G, CS, 0, ALL, 0);
    if (vk_fg_mode() != VK_FG_OFF || vk_hdr_active()) hudless_keep(c, cb, vk_fg_frame(), ow, oh);
    T.frames++;
    if (T.logged_mode != mode) {
        T.logged_mode = mode;
        rt_log("vk: %s runs in place of the game's reconstruction, %ux%u to %ux%u", mode_name(mode), rw, rh, ow, oh);
    }
    return 1;
}

int vk_tup_hudless(uint64_t id, VkImage *img, uint32_t *w, uint32_t *h) {
    unsigned s = (unsigned)(id % HL_RING);
    if (HL.id[s] != id + 1 || !HL.img[s].image) return 0;
    *img = HL.img[s].image, *w = HL.w, *h = HL.h;
    return 1;
}

int vk_tup_result(VkTupImage *o) {
    if (!T.out.image) return 0;
    *o = (VkTupImage){T.out.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, T.ow, T.oh};
    return 1;
}

// src into dst, stretched with a linear filter when the sizes differ.
void vk_tup_copy(VkCommandBuffer cb, const VkTupImage *src, const VkTupImage *dst) {
    const VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL;
    const VkPipelineStageFlags2 ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, XF = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    const VkAccessFlags2 RW = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier(cb, src->image, VK_IMAGE_ASPECT_COLOR_BIT, G, G, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, XF, VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier(cb, dst->image, VK_IMAGE_ASPECT_COLOR_BIT, G, G, ALL, RW, XF, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0}, {(int32_t)src->w, (int32_t)src->h, 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0}, {(int32_t)dst->w, (int32_t)dst->h, 1}}};
    vkCmdBlitImage(cb, src->image, G, dst->image, G, 1, &bl, VK_FILTER_LINEAR);
    barrier(cb, dst->image, VK_IMAGE_ASPECT_COLOR_BIT, G, G, XF, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL, RW);
    barrier(cb, src->image, VK_IMAGE_ASPECT_COLOR_BIT, G, G, XF, VK_ACCESS_2_TRANSFER_READ_BIT, ALL, RW);
}

void vk_tup_reset(void) { T.reset = 1; }

void vk_tup_release(void) {
    VkCtx *c = vk_ctx();
    if (!c || !T.tried) return;
    ctx_free(c);
    for (int i = 0; i < HL_RING; i++) img_free(c, &HL.img[i]);
    memset(&HL, 0, sizeof HL);
    for (int s = 0; s < SLOTS; s++)
        for (int k = 0; k < 3; k++)
            if (T.src_view[s][k]) vkDestroyImageView(c->device, T.src_view[s][k], NULL);
    if (T.prep) vkDestroyPipeline(c->device, T.prep, NULL);
    if (T.sampler) vkDestroySampler(c->device, T.sampler, NULL);
    if (T.layout) vkDestroyPipelineLayout(c->device, T.layout, NULL);
    if (T.dsl) vkDestroyDescriptorSetLayout(c->device, T.dsl, NULL);
    memset(&T, 0, sizeof T);
}
