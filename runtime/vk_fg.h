// Frame generation (XC3_FRAME_GEN, vk_fg.c): one generated frame between two drawn ones, from NVIDIA DLSS Frame
// Generation (vk_sl.cpp) or AMD FSR 3 frame generation (FidelityFX SDK 1.1.4).
#pragma once

#include <stdint.h>
#include <vulkan/vulkan.h>

#include "vk_sl.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { VK_FG_OFF, VK_FG_DLSS, VK_FG_FSR };
int vk_fg_mode(void);   // XC3_FRAME_GEN as the session runs it (read once; changing it takes a restart)

// Device set-up (vk_device.c): the extra queues of the graphics family FSR 3 asks for (its present and image acquire
// queues) and, once the device exists, where they start in family (0: they did not fit).
uint32_t vk_fg_extra_queues(void);
void vk_fg_device(VkDevice dev, uint32_t family, uint32_t first_queue);

// Frame ids. The replay records frame vk_fg_frame(); queueing its present (vk_backend.c) closes it and returns its id,
// which the present thread hands to vk_fg_present. Both generators name a frame's inputs and present by it.
uint64_t vk_fg_frame(void);
uint64_t vk_fg_frame_close(void);

// The temporal upscaler (vk_tup.c) has the frame's depth (R32F) and motion (RG16F, UV offsets) at the render size in
// cb; dlss_tagged: DLSS upscaled it and tagged them already. dt_ms: time since the frame before.
void vk_fg_inputs(VkCommandBuffer cb, const VkSlImage *depth, const VkSlImage *motion, const float jitter[2],
                  float near_z, float fov_y, int reset, float dt_ms, int dlss_tagged);

// The swapchain with FSR 3 (gfx_present.c, g_pl held, the device idle): created by FidelityFX's frame generation
// swapchain, which then also answers for the image list, the acquire and the present (*images, *acquire, *present).
// old is destroyed first. Returns the Vulkan result.
VkResult vk_fg_fsr_swapchain(const VkSwapchainCreateInfoKHR *ci, VkSwapchainKHR old, VkSwapchainKHR *out,
                             PFN_vkGetSwapchainImagesKHR *images, PFN_vkAcquireNextImageKHR *acquire,
                             PFN_vkQueuePresentKHR *present);
void vk_fg_fsr_swapchain_destroy(VkSwapchainKHR sc);   // the device release

// The present thread around frame id's present into sc (w x h of fmt, images images; the picture in rect {x, y, w,
// h}); allow: no overlay menu covers the picture. hudless: the frame without the interface at the swapchain's size
// and format, placed as the picture is (gfx_present.c hudless_record), or NULL.
void vk_fg_present(uint64_t id, VkSwapchainKHR sc, uint32_t w, uint32_t h, VkFormat fmt, uint32_t images,
                   const int rect[4], int allow, const VkSlImage *hudless);
void vk_fg_present_end(uint64_t id);
void vk_fg_release(void);   // before the device goes away

// Shown frames per drawn frame: 2 while generation runs, else 1. The frame limit (frame_rate.c) divides by it.
int gfx_frame_gen_factor(void);
const char *vk_fg_status(void);   // for the overlay: "DLSS FG", "FSR FG" or NULL while off
// Frames shown per drawn frame lately, from the generators' own counts (1 without generation): the overlay's frame
// rate counts drawn frames and multiplies by it.
float vk_fg_shown_ratio(void);

#ifdef __cplusplus
}
#endif
