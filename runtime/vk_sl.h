// NVIDIA DLSS through Streamline (vk_sl.cpp), for the temporal upscaler (vk_tup.c), the device (vk_device.c) and the
// present (gfx_present.c). Without Streamline (not Windows, or its DLLs missing) every call is a no-op that says no.
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
    uint32_t w, h;
    VkImageUsageFlags usage;
} VkSlImage;   // in VK_IMAGE_LAYOUT_GENERAL

int vk_sl_start(void);                                      // before vkCreateInstance; 1: Streamline runs with DLSS
uint32_t vk_sl_instance_exts(const char **out, uint32_t max);   // instance extensions DLSS asks for
uint32_t vk_sl_device_exts(const char **out, uint32_t max);     // device extensions DLSS asks for
// After vkCreateDevice; exts_ok: the device has every extension vk_sl_device_exts listed.
void vk_sl_device(VkInstance inst, VkPhysicalDevice phys, VkDevice dev, uint32_t family, int exts_ok);
int vk_sl_ready(void);                                      // DLSS can run
PFN_vkVoidFunction vk_sl_proc(VkDevice dev, const char *name);   // Streamline's proxy of a device function, or NULL
// Records DLSS: colour, depth (R32F, 0 near, 1 at infinity), motion (RG16F UV offsets to the previous frame) at the
// render size, out at the output size; jitter in render pixels. dlaa: the render size is the output size.
int vk_sl_evaluate(VkCommandBuffer cb, const VkSlImage *color, const VkSlImage *depth, const VkSlImage *motion,
                   const VkSlImage *out, const float jitter[2], float near_z, float fov_y, int reset, int dlaa);
void vk_sl_shutdown(void);                                  // before the device and instance go away

#ifdef __cplusplus
}
#endif
