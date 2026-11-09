// NVIDIA DLSS and DLSS Frame Generation through Streamline (vk_sl.cpp), for the temporal upscaler (vk_tup.c), frame
// generation (vk_fg.c), the device (vk_device.c) and the present (gfx_present.c). Without Streamline (not Windows, or
// its DLLs missing) every call is a no-op that says no.
#pragma once

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    VkImage image;
    VkImageView view;
    VkFormat format;
    uint32_t w, h;          // the part in use, from the top left
    VkImageUsageFlags usage;
    uint32_t iw, ih;        // the image's own size when larger (dynamic resolution); 0: w x h
} VkSlImage;   // in VK_IMAGE_LAYOUT_GENERAL

// Before vkCreateInstance, with the features wanted (dlss: DLSS for the upscaler, fg: DLSS Frame Generation);
// 1: Streamline runs with at least one of them.
int vk_sl_start(int dlss, int fg, int reflex);   // reflex: Reflex with DLSS alone (frame generation off)
uint32_t vk_sl_instance_exts(const char **out, uint32_t max);   // instance extensions the features ask for
uint32_t vk_sl_device_exts(const char **out, uint32_t max);     // device extensions the features ask for
uint32_t vk_sl_extra_queues(void);                              // queues of the graphics family Streamline asks for
// Streamline's vkCreateInstance, vkEnumeratePhysicalDevices and vkCreateDevice proxies, which DLSS Frame Generation
// needs (they set up its queues and features themselves, and its vkCreateDevice finds the instance of a physical
// device only through its vkEnumeratePhysicalDevices); NULL without it.
PFN_vkCreateInstance vk_sl_create_instance(void);
PFN_vkEnumeratePhysicalDevices vk_sl_enumerate_physical_devices(void);
PFN_vkCreateDevice vk_sl_create_device(VkInstance inst);
// After vkCreateDevice; first_queue: the index of the first queue of the family made for Streamline; exts_ok: the
// device has every extension vk_sl_device_exts listed.
void vk_sl_device(VkInstance inst, VkPhysicalDevice phys, VkDevice dev, uint32_t family, uint32_t first_queue, int exts_ok);
int vk_sl_ready(void);                                      // DLSS can run
int vk_sl_fg_ready(void);                                   // DLSS Frame Generation can run
PFN_vkVoidFunction vk_sl_proc(VkDevice dev, const char *name);   // Streamline's proxy of a device function, or NULL
// Streamline's proxy of an instance function while DLSS Frame Generation is wanted (the window's surface), or NULL.
PFN_vkVoidFunction vk_sl_instance_proc(VkInstance inst, const char *name);
// Records DLSS for frame id (vk_fg_frame): colour, depth (R32F, 0 near, 1 at infinity), motion (RG16F UV offsets to
// the previous frame) at the render size, out at the output size; jitter in render pixels. dlaa: the render size is
// the output size. The depth and motion tags serve DLSS Frame Generation too.
int vk_sl_evaluate(VkCommandBuffer cb, uint64_t id, const VkSlImage *color, const VkSlImage *depth,
                   const VkSlImage *motion, const VkSlImage *out, const float jitter[2], float near_z, float fov_y,
                   int reset, int dlaa);
// DLSS Frame Generation's inputs of frame id when DLSS does not upscale it (the same images as vk_sl_evaluate's).
int vk_sl_fg_inputs(VkCommandBuffer cb, uint64_t id, const VkSlImage *depth, const VkSlImage *motion,
                    const float jitter[2], float near_z, float fov_y, int reset);
void vk_sl_fg_frame_start(uint64_t id);   // replay: frame id's recording begins (Reflex sleeps)
void vk_sl_fg_frame_end(uint64_t id);     // replay: frame id's recording is done, its present queued
// Present thread, right before frame id's present: the w x h back buffer of fmt and its image count, the picture's
// rectangle {x, y, w, h} in it, the render size of the inputs; allow: generation may run (no menu over the picture).
void vk_sl_fg_present(uint64_t id, uint32_t w, uint32_t h, VkFormat fmt, uint32_t images, const int rect[4],
                      uint32_t render_w, uint32_t render_h, int allow);
void vk_sl_fg_present_end(uint64_t id);   // right after it
int vk_sl_reflex_ready(void);              // Reflex runs without DLSS-G: the frame and present calls are for it
void vk_sl_reflex_present(uint64_t id);    // the present thread, before frame id's present, without DLSS-G
void vk_sl_swapchain_made(void);           // a swapchain was made: Reflex's mode is set on it again
int vk_sl_reflex_mode(void);               // the Reflex mode pacing frames: 0 none, 1 on, 2 with boost
int vk_sl_fg_active(void);                // generation was on at the last present
uint64_t vk_sl_fg_shown(void);            // frames DLSS-G presented so far, drawn and generated
// Before the swapchain goes away: generation off with its resources freed, and the device idle through Streamline's
// vkDeviceWaitIdle, which also waits for the frames its present thread still holds.
void vk_sl_fg_release(VkDevice dev);
void vk_sl_shutdown(void);                                  // before the device and instance go away

#ifdef __cplusplus
}
#endif
