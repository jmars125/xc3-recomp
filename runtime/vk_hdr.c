// HDR output (XC3_HDR=1, read at start): an HDR10 swapchain (Rec. 2020 primaries, PQ) and the game's SDR picture
// brought into it on the present, with the 3D scene's highlights expanded towards the display's peak and the
// interface kept at its own white. Settings, read when they change: XC3_HDR_PEAK_NITS (the display's peak),
// XC3_HDR_GAME_NITS (the scene's white, "paper white"), XC3_HDR_UI_NITS (the interface's and this port's menu's white),
// XC3_HDR_HIGHLIGHTS (0..100, how far the scene's brightest parts reach towards the peak), XC3_HDR_SATURATION (0..200,
// 100 unchanged) and XC3_HDR_GAMMA (srgb, 2.2 or 2.4: the decode of the game's 8-bit values). With XC3_HDR=0 none of
// this runs and the present is what it was.
//
// XC3 renders, tonemaps and composes its frame in 8-bit targets: what reaches the present is SDR. NX2navi
// (runtime/vk_hdr.c) gets real HDR out of its game by splicing an HDR tonemapper into the game's tonemap shader and
// keeping the targets after it in float; XC3's targets after its tonemap are RGBA8, so here the SDR picture is
// expanded instead, the way Windows' Auto HDR does, but with the interface told apart (shaders/hdr_encode.comp):
// the temporal upscaler keeps the frame before the interface is drawn (vk_tup.c vk_tup_hudless), and the pixels where
// the presented picture differs from it are the interface's.
//
// On the present (gfx_present.c present_image): vk_hdr_encode turns the crop rectangle into PQ code values in an
// RGBA16F image of the crop's size, which FSR 1 or the blit scale into vk_hdr_full, an RGBA16F image of the window's
// size; the menu overlay draws into an RGBA8 image (vk_hdr_overlay_target) that vk_hdr_overlay blends onto it at the
// interface's white (shaders/hdr_ui.comp); a blit then converts it into the A2B10G10R10 swapchain image. Frame
// generation's picture without the interface goes through the same encode (vk_hdr_encode with slot 1), so it differs
// from the back buffer only where the interface is.
#include "vk.h"

#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "hdr_encode_spv.h"
#include "hdr_ui_spv.h"
#include "host.h"

#define SLOTS 8   // source views per slot: present_image waits for the slot's fence before it records again

typedef struct {
    VkImage image;
    VmaAllocation alloc;
    VkImageView view;
    uint32_t w, h;
    VkFormat format;
} Img;

static struct {
    int wanted;        // XC3_HDR at start: -1 not read yet
    int active;        // the swapchain is HDR10
    int tried, ok;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline encode, ui;
    VkSampler sampler;
    Img enc[2];        // the encoded picture and the encoded picture without the interface, crop size
    Img full;          // the window-size PQ picture
    Img over;          // the overlay's RGBA8 image
    VkImageView src_view[SLOTS][2][2];   // [slot][which][source, picture without the interface]
    unsigned cfg_seen;
    float p0[4], p1[4];
} H = {.wanted = -1, .cfg_seen = ~0u};

int vk_hdr_wanted(void) {
    if (H.wanted < 0) H.wanted = cfg_bool("XC3_HDR");
    return H.wanted;
}

void vk_hdr_set_active(int on) {
    if (on != H.active) rt_log("gfx: HDR output %s", on ? "on (HDR10, PQ)" : "off: the display has no HDR10 swapchain (Windows' HDR switched on?)");
    H.active = on;
}

int vk_hdr_active(void) { return H.active; }

// The settings, again when one changed (the menu sets them while the game runs).
static void read_settings(void) {
    unsigned v = cfg_version();
    if (v == H.cfg_seen) return;
    H.cfg_seen = v;
    double peak = cfg_double("XC3_HDR_PEAK_NITS"), game = cfg_double("XC3_HDR_GAME_NITS"), ui = cfg_double("XC3_HDR_UI_NITS");
    peak = peak < 80 ? 80 : peak > 10000 ? 10000 : peak;
    game = game < 80 ? 80 : game > peak ? peak : game;
    ui = ui < 80 ? 80 : ui > peak ? peak : ui;
    const char *g = cfg_value("XC3_HDR_GAMMA");
    H.p0[0] = (float)game, H.p0[1] = (float)ui, H.p0[2] = (float)peak;
    H.p0[3] = g && !strcmp(g, "2.2") ? 1.0f : g && !strcmp(g, "2.4") ? 2.0f : 0.0f;
    int hi = cfg_int("XC3_HDR_HIGHLIGHTS"), sat = cfg_int("XC3_HDR_SATURATION");
    H.p1[0] = (hi < 0 ? 0 : hi > 100 ? 100 : hi) / 100.0f;
    H.p1[2] = (sat < 0 ? 0 : sat > 200 ? 200 : sat) / 100.0f;
}

static VkPipeline make_pipeline(VkCtx *c, const uint32_t *code, size_t size) {
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = H.layout};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    vkDestroyShaderModule(c->device, m, NULL);
    return p;
}

// One layout for both passes: two sampled images and a storage image (the overlay pass uses the first and the third).
static int init(VkCtx *c) {
    H.tried = 1;
    H.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    if (!H.push) {
        rt_log("gfx: HDR off: no VK_KHR_push_descriptor");
        return 0;
    }
    VkDescriptorSetLayoutBinding b[3] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 3, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 48};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &H.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &H.dsl) != VK_SUCCESS ||
        vkCreatePipelineLayout(c->device, &li, NULL, &H.layout) != VK_SUCCESS ||
        vkCreateSampler(c->device, &si, NULL, &H.sampler) != VK_SUCCESS ||
        !(H.encode = make_pipeline(c, hdr_encode_spv, sizeof hdr_encode_spv)) ||
        !(H.ui = make_pipeline(c, hdr_ui_spv, sizeof hdr_ui_spv))) {
        rt_log("gfx: HDR off: its pipelines could not be created");
        return 0;
    }
    return H.ok = 1;
}

static void img_free(VkCtx *c, Img *i) {
    if (i->view) vkDestroyImageView(c->device, i->view, NULL);
    if (i->image) vmaDestroyImage(c->vma, i->image, i->alloc);
    memset(i, 0, sizeof *i);
}

// An image of w x h and fmt; a size change waits for the device (earlier presents may still read the old one).
static int img_ensure(VkCtx *c, Img *i, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage) {
    if (i->image && i->w == w && i->h == h && i->format == fmt) return 1;
    if (i->image) {
        vk_queue_lock();
        vkDeviceWaitIdle(c->device);
        vk_queue_unlock();
        img_free(c, i);
    }
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ci, &ai, &i->image, &i->alloc, NULL) != VK_SUCCESS) return i->image = VK_NULL_HANDLE, 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = i->image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, &i->view) != VK_SUCCESS) {
        img_free(c, i);
        return 0;
    }
    i->w = w, i->h = h, i->format = fmt;
    return 1;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 ss,
                    VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa,
                               .dstStageMask = ds, .dstAccessMask = da, .oldLayout = from, .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                          .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

static VkImageView src_view(VkCtx *c, VkImage img, VkFormat fmt) {
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView v = VK_NULL_HANDLE;
    return vkCreateImageView(c->device, &vi, NULL, &v) == VK_SUCCESS ? v : VK_NULL_HANDLE;
}

VkImage vk_hdr_encode(VkCommandBuffer cb, unsigned slot, int which, VkImage src, VkFormat fmt, VkImage bare,
                      const int rect[4], VkFormat *out_fmt) {
    VkCtx *c = vk_ctx();
    if (!H.active) return VK_NULL_HANDLE;
    if (!H.tried) init(c);
    if (!H.ok) return VK_NULL_HANDLE;
    read_settings();
    Img *e = &H.enc[which & 1];
    if (!img_ensure(c, e, (uint32_t)rect[2], (uint32_t)rect[3], VK_FORMAT_R16G16B16A16_SFLOAT,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        return VK_NULL_HANDLE;
    slot %= SLOTS;
    VkImageView *v = H.src_view[slot][which & 1];
    for (int k = 0; k < 2; k++)
        if (v[k]) vkDestroyImageView(c->device, v[k], NULL), v[k] = VK_NULL_HANDLE;
    // the game's images hold sRGB-encoded values in UNORM: a UNORM view, decoded in the shader
    v[0] = src_view(c, src, fmt);
    v[1] = bare ? src_view(c, bare, VK_FORMAT_R8G8B8A8_UNORM) : VK_NULL_HANDLE;
    if (!v[0]) return VK_NULL_HANDLE;
    const VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL;
    const VkPipelineStageFlags2 ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier(cb, src, G, G, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    if (bare) barrier(cb, bare, G, G, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    barrier(cb, e->image, VK_IMAGE_LAYOUT_UNDEFINED, G, ALL, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, CS,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    VkDescriptorImageInfo ii[3] = {{H.sampler, v[0], G}, {H.sampler, v[1] ? v[1] : v[0], G}, {VK_NULL_HANDLE, e->view, G}};
    VkWriteDescriptorSet w[3];
    for (uint32_t i = 0; i < 3; i++)
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = i, .descriptorCount = 1,
                                      .pImageInfo = &ii[i],
                                      .descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                              : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    struct { int32_t offset[2], size[2]; float p0[4], p1[4]; } pc = {{rect[0], rect[1]}, {rect[2], rect[3]},
                                                                    {H.p0[0], H.p0[1], H.p0[2], H.p0[3]},
                                                                    {H.p1[0], v[1] ? 1.0f : 0.0f, H.p1[2], 0}};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, H.encode);
    rec_push(H.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, H.layout, 0, 3, w);
    vkCmdPushConstants(cb, H.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, ((uint32_t)rect[2] + 7) / 8, ((uint32_t)rect[3] + 7) / 8, 1);
    barrier(cb, e->image, G, G, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, ALL, VK_ACCESS_2_MEMORY_READ_BIT);
    // later submissions that write the sources wait for the reads
    barrier(cb, src, G, G, CS, 0, ALL, 0);
    if (bare) barrier(cb, bare, G, G, CS, 0, ALL, 0);
    static int said = -1;   // the state last logged (the picture alone)
    if (!which && said != (v[1] != 0) && (said = v[1] != 0, 1)) rt_log("gfx: HDR encode: %dx%d, %s", rect[2], rect[3], v[1] ? "the scene told apart from the interface"
                                                                          : "no picture without the interface (no temporal upscaler yet): nothing expanded");
    *out_fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    return e->image;
}

VkImage vk_hdr_full(uint32_t w, uint32_t h) {
    VkCtx *c = vk_ctx();
    if (!img_ensure(c, &H.full, w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        return VK_NULL_HANDLE;
    return H.full.image;
}

int vk_hdr_overlay_target(uint32_t w, uint32_t h, VkImage *img, VkImageView *view) {
    VkCtx *c = vk_ctx();
    if (!img_ensure(c, &H.over, w, h, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return 0;
    *img = H.over.image, *view = H.over.view;
    return 1;
}

void vk_hdr_overlay(VkCommandBuffer cb) {
    if (!H.ok || !H.over.image || !H.full.image) return;
    read_settings();
    const VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL;
    const VkPipelineStageFlags2 ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier(cb, H.over.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, G, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    VkDescriptorImageInfo ii[3] = {{H.sampler, H.over.view, G}, {H.sampler, H.over.view, G}, {VK_NULL_HANDLE, H.full.view, G}};
    VkWriteDescriptorSet w[3];
    for (uint32_t i = 0; i < 3; i++)
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = i, .descriptorCount = 1,
                                      .pImageInfo = &ii[i],
                                      .descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                              : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    struct { int32_t offset[2], size[2]; float p[4], unused[4]; } pc = {{0, 0}, {(int32_t)H.full.w, (int32_t)H.full.h},
                                                                       {H.p0[1], H.p0[2], 0, 0}, {0}};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, H.ui);
    rec_push(H.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, H.layout, 0, 3, w);
    vkCmdPushConstants(cb, H.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, (H.full.w + 7) / 8, (H.full.h + 7) / 8, 1);
    barrier(cb, H.full.image, G, G, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, ALL, VK_ACCESS_2_MEMORY_READ_BIT);
}

void vk_hdr_release(void) {
    VkCtx *c = vk_ctx();
    if (!c || !H.tried) return;
    for (int i = 0; i < 2; i++) img_free(c, &H.enc[i]);
    img_free(c, &H.full), img_free(c, &H.over);
    for (int s = 0; s < SLOTS; s++)
        for (int k = 0; k < 2; k++)
            for (int j = 0; j < 2; j++)
                if (H.src_view[s][k][j]) vkDestroyImageView(c->device, H.src_view[s][k][j], NULL);
    if (H.encode) vkDestroyPipeline(c->device, H.encode, NULL);
    if (H.ui) vkDestroyPipeline(c->device, H.ui, NULL);
    if (H.sampler) vkDestroySampler(c->device, H.sampler, NULL);
    if (H.layout) vkDestroyPipelineLayout(c->device, H.layout, NULL);
    if (H.dsl) vkDestroyDescriptorSetLayout(c->device, H.dsl, NULL);
    int wanted = H.wanted, active = H.active;
    memset(&H, 0, sizeof H);
    H.wanted = wanted, H.active = active, H.cfg_seen = ~0u;
}
