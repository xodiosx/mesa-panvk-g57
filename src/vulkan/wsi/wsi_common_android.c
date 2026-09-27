/*
 * Copyright © 2026 PanVK WinlatorMali contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * VK_KHR_android_surface for the Android/kbase build.
 *
 * Upstream Mesa does not ship an Android WSI at all (there is no
 * wsi_common_android.c in any release branch), so this is written against the
 * public ANativeWindow interface instead of ported from upstream.  The
 * presentation model is the classic lock/unlockAndPost one:
 *
 *   - the swapchain asks the window for buffers of the swapchain geometry with
 *     ANativeWindow_setBuffersGeometry();
 *   - every swapchain image owns one buffer locked through
 *     ANativeWindow_lock(), taken in image order, which is the order a
 *     BufferQueue wants them back in;
 *   - the WSI renders into a host-visible buffer (the same buffer-blit path the
 *     X11 SHM path uses) and the present copies that into the compositor buffer
 *     before ANativeWindow_unlockAndPost() queues the frame.
 *
 * No dma-buf is exchanged with the compositor, so this works on a kbase device
 * that can neither import host pointers nor share dma-bufs with the window
 * system.
 */

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <android/native_window.h>

#include "util/log.h"
#include "util/macros.h"
#include "util/u_math.h"
#include "util/u_thread.h"
#include "vk_enum_to_str.h"
#include "vk_format.h"
#include "vk_instance.h"
#include "vk_physical_device.h"
#include "vk_util.h"
#include "wsi_common_entrypoints.h"
#include "wsi_common_private.h"
#include "wsi_common_queue.h"

#define WSI_ANDROID_MIN_IMAGES 2
#define WSI_ANDROID_MAX_IMAGES 8

static const VkFormat wsi_android_formats[] = {
   VK_FORMAT_R8G8B8A8_UNORM,
   VK_FORMAT_R8G8B8A8_SRGB,
};

static const VkPresentModeKHR wsi_android_present_modes[] = {
   VK_PRESENT_MODE_FIFO_KHR,
   VK_PRESENT_MODE_IMMEDIATE_KHR,
};

struct wsi_android_surface {
   VkIcdSurfaceAndroid base;
};

struct wsi_android_image {
   struct wsi_image base;

   struct ANativeWindow *window;
   /* Buffer the compositor handed us through ANativeWindow_lock().  It stays
    * locked until the image is presented. */
   uint8_t *buffer_ptr;
   uint32_t buffer_stride;
   bool locked;
};

struct wsi_android_swapchain {
   struct wsi_swapchain base;

   struct ANativeWindow *window;
   struct wsi_android_image *images;
   VkExtent2D extent;

   struct wsi_queue present_queue;
   struct wsi_queue acquire_queue;

   /* Latches the first error, like the x11 backend does, so a failed present
    * is not hidden by a later successful one. */
   VkResult status;

   /* Serializes ANativeWindow_lock()/unlockAndPost() against destruction. */
   mtx_t lock;
};

static struct ANativeWindow *
wsi_android_surface_get_window(VkIcdSurfaceBase *icd_surface)
{
   return ((struct wsi_android_surface *)icd_surface)->base.window;
}

static bool
wsi_android_window_geometry(struct ANativeWindow *window, uint32_t *width,
                            uint32_t *height)
{
   int32_t w = ANativeWindow_getWidth(window);
   int32_t h = ANativeWindow_getHeight(window);

   if (w <= 0 || h <= 0)
      return false;

   *width = (uint32_t)w;
   *height = (uint32_t)h;

   return true;
}

static VkResult
wsi_android_surface_get_support(VkIcdSurfaceBase *icd_surface,
                                struct wsi_device *wsi_device,
                                uint32_t queueFamilyIndex,
                                VkBool32 *pSupported)
{
   uint32_t width, height;

   *pSupported = wsi_android_window_geometry(
                    wsi_android_surface_get_window(icd_surface), &width,
                    &height)
                    ? VK_TRUE
                    : VK_FALSE;

   return VK_SUCCESS;
}

static VkResult
wsi_android_surface_get_capabilities2(
   VkIcdSurfaceBase *icd_surface, struct wsi_device *wsi_device,
   const void *info_next, VkSurfaceCapabilities2KHR *pSurfaceCapabilities)
{
   uint32_t width, height;

   assert(pSurfaceCapabilities->sType ==
          VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR);

   if (!wsi_android_window_geometry(
          wsi_android_surface_get_window(icd_surface), &width, &height))
      return VK_ERROR_SURFACE_LOST_KHR;

   pSurfaceCapabilities->minImageCount = WSI_ANDROID_MIN_IMAGES;
   pSurfaceCapabilities->maxImageCount = WSI_ANDROID_MAX_IMAGES;
   pSurfaceCapabilities->currentExtent = (VkExtent2D){width, height};
   pSurfaceCapabilities->minImageExtent = (VkExtent2D){1, 1};
   pSurfaceCapabilities->maxImageExtent = (VkExtent2D){width, height};
   pSurfaceCapabilities->maxImageArrayLayers = 1;
   pSurfaceCapabilities->supportedTransforms =
      VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   pSurfaceCapabilities->currentTransform =
      VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   pSurfaceCapabilities->supportedCompositeAlpha =
      VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

   vk_foreach_struct(ext, pSurfaceCapabilities->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_SURFACE_PROTECTED_CAPABILITIES_KHR: {
         VkSurfaceProtectedCapabilitiesKHR *protected = (void *)ext;
         protected->supportsProtected = VK_FALSE;
         break;
      }

      case VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_KHR: {
         /* Unsupported */
         VkSurfacePresentScalingCapabilitiesKHR *scaling = (void *)ext;
         scaling->supportedPresentScaling = 0;
         scaling->supportedPresentGravityX = 0;
         scaling->supportedPresentGravityY = 0;
         scaling->minScaledImageExtent =
            pSurfaceCapabilities->minImageExtent;
         scaling->maxScaledImageExtent =
            pSurfaceCapabilities->maxImageExtent;
         break;
      }

      case VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_KHR: {
         /* FIFO is always compatible with every other mode. */
         VkSurfacePresentModeCompatibilityKHR *compat = (void *)ext;
         const VkSurfacePresentModeKHR *present_mode =
            vk_find_struct_const(info_next, SURFACE_PRESENT_MODE_KHR);

         if (compat->pPresentModes == NULL) {
            compat->presentModeCount = 1;
         } else if (compat->presentModeCount) {
            compat->presentModeCount = 1;
            compat->pPresentModes[0] =
               present_mode ? present_mode->presentMode
                            : VK_PRESENT_MODE_FIFO_KHR;
         }
         break;
      }

      case VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT: {
         VkPresentTimingSurfaceCapabilitiesEXT *timing = (void *)ext;
         timing->presentStageQueries = 0;
         timing->presentTimingSupported = VK_FALSE;
         timing->presentAtAbsoluteTimeSupported = VK_FALSE;
         timing->presentAtRelativeTimeSupported = VK_FALSE;
         break;
      }

      default:
         /* Ignored */
         break;
      }
   }

   return VK_SUCCESS;
}

static VkResult
wsi_android_surface_get_formats(VkIcdSurfaceBase *icd_surface,
                                struct wsi_device *wsi_device,
                                uint32_t *pSurfaceFormatCount,
                                VkSurfaceFormatKHR *pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormatKHR, out, pSurfaceFormats,
                          pSurfaceFormatCount);

   for (unsigned i = 0; i < ARRAY_SIZE(wsi_android_formats); i++) {
      vk_outarray_append_typed(VkSurfaceFormatKHR, &out, f) {
         f->format = wsi_android_formats[i];
         f->colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      }
   }

   return vk_outarray_status(&out);
}

static VkResult
wsi_android_surface_get_formats2(VkIcdSurfaceBase *icd_surface,
                                 struct wsi_device *wsi_device,
                                 const void *info_next,
                                 uint32_t *pSurfaceFormatCount,
                                 VkSurfaceFormat2KHR *pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormat2KHR, out, pSurfaceFormats,
                          pSurfaceFormatCount);

   for (unsigned i = 0; i < ARRAY_SIZE(wsi_android_formats); i++) {
      vk_outarray_append_typed(VkSurfaceFormat2KHR, &out, f) {
         f->surfaceFormat.format = wsi_android_formats[i];
         f->surfaceFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      }
   }

   return vk_outarray_status(&out);
}

static VkResult
wsi_android_surface_get_present_modes(VkIcdSurfaceBase *icd_surface,
                                      struct wsi_device *wsi_device,
                                      uint32_t *pPresentModeCount,
                                      VkPresentModeKHR *pPresentModes)
{
   if (pPresentModes == NULL) {
      *pPresentModeCount = ARRAY_SIZE(wsi_android_present_modes);
      return VK_SUCCESS;
   }

   *pPresentModeCount =
      MIN2(*pPresentModeCount, ARRAY_SIZE(wsi_android_present_modes));
   typed_memcpy(pPresentModes, wsi_android_present_modes, *pPresentModeCount);

   return *pPresentModeCount < ARRAY_SIZE(wsi_android_present_modes) ?
             VK_INCOMPLETE :
             VK_SUCCESS;
}

static VkResult
wsi_android_surface_get_present_rectangles(VkIcdSurfaceBase *icd_surface,
                                           struct wsi_device *wsi_device,
                                           uint32_t *pRectCount,
                                           VkRect2D *pRects)
{
   uint32_t width, height;

   if (!wsi_android_window_geometry(
          wsi_android_surface_get_window(icd_surface), &width, &height))
      return VK_ERROR_SURFACE_LOST_KHR;

   if (pRects == NULL) {
      *pRectCount = 1;
      return VK_SUCCESS;
   }

   *pRectCount = MIN2(*pRectCount, 1u);
   pRects[0] = (VkRect2D){
      .offset = {0, 0},
      .extent = {width, height},
   };

   return *pRectCount < 1 ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
wsi_CreateAndroidSurfaceKHR(VkInstance _instance,
                            const VkAndroidSurfaceCreateInfoKHR *pCreateInfo,
                            const VkAllocationCallbacks *pAllocator,
                            VkSurfaceKHR *pSurface)
{
   VK_FROM_HANDLE(vk_instance, instance, _instance);
   struct wsi_android_surface *surface;

   assert(pCreateInfo->sType ==
          VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR);
   assert(pCreateInfo->window);

   uint32_t width, height;
   if (!wsi_android_window_geometry(pCreateInfo->window, &width, &height))
      return VK_ERROR_INITIALIZATION_FAILED;

   surface = vk_zalloc2(&instance->alloc, pAllocator,
                        sizeof(struct wsi_android_surface), 8,
                        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (surface == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   /* The surface keeps the window alive for as long as it exists. */
   ANativeWindow_acquire(pCreateInfo->window);

   surface->base.base.platform = VK_ICD_WSI_PLATFORM_ANDROID;
   surface->base.window = pCreateInfo->window;

   *pSurface = VkIcdSurfaceBase_to_handle(&surface->base.base);

   if (wsi_panvk_trace())
      fprintf(stderr,
              "PANVKDBG SURFACE create_android window=%p %ux%u surface=%p\n",
              (void *)pCreateInfo->window, width, height, (void *)*pSurface);

   return VK_SUCCESS;
}

void
wsi_android_surface_destroy(VkIcdSurfaceBase *icd_surface, VkInstance _instance,
                            const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(vk_instance, instance, _instance);
   struct wsi_android_surface *surface =
      (struct wsi_android_surface *)icd_surface;

   ANativeWindow_release(surface->base.window);
   vk_free2(&instance->alloc, pAllocator, surface);
}

/*
 * Takes one buffer from the window for the given image.  The buffer stays
 * locked until the image is presented, which is the only way to hand it back.
 */
static bool
wsi_android_lock_image(struct wsi_android_image *image, unsigned size)
{
   struct ANativeWindow *window = image->window;
   int32_t width, height;
   ARect in_rect, out_rect;
   uint8_t *ptr;

   if (!window || image->locked)
      return false;

   width = ANativeWindow_getWidth(window);
   height = ANativeWindow_getHeight(window);
   if (width <= 0 || height <= 0)
      return false;

   /* Ask for the whole buffer so the compositor never does a partial update. */
   in_rect.left = 0;
   in_rect.top = 0;
   in_rect.right = width;
   in_rect.bottom = height;
   memset(&out_rect, 0, sizeof(out_rect));

   ptr = ANativeWindow_lock(window, &in_rect, &out_rect);
   if (!ptr)
      return false;

   if (out_rect.width <= 0 || out_rect.height <= 0) {
      /* No buffer was available.  ANativeWindow_lock() still has to be paired
       * with an unlock. */
      ANativeWindow_unlockAndPost(window);
      return false;
   }

   image->buffer_ptr = ptr;
   image->buffer_stride = (uint32_t)out_rect.stride * 4;
   image->locked = true;

   if (wsi_panvk_trace())
      fprintf(stderr,
              "PANVKDBG ANDROID_LOCK image=%p ptr=%p stride=%u out=%dx%d "
              "want=%u\n",
              (void *)image, (void *)ptr, image->buffer_stride, out_rect.width,
              out_rect.height, size);

   return true;
}

/*
 * Called by the generic buffer-blit context to obtain the compositor buffer the
 * finished frame is copied into.  Images are initialized in index order, which
 * is the order a BufferQueue expects buffers to be queued back in.
 */
static uint8_t *
wsi_android_alloc_buffer(struct wsi_image *imagew, unsigned size)
{
   struct wsi_android_image *image = (struct wsi_android_image *)imagew;

   if (!wsi_android_lock_image(image, size))
      return NULL;

   return image->buffer_ptr;
}

static VkResult
wsi_android_image_init(struct wsi_android_swapchain *chain,
                       const struct wsi_image_info *info,
                       struct wsi_android_image *image)
{
   image->window = chain->window;

   return wsi_create_image(&chain->base, info, &image->base);
}

static void
wsi_android_image_finish(struct wsi_android_image *image)
{
   if (image->locked) {
      ANativeWindow_unlockAndPost(image->window);
      image->locked = false;
      image->buffer_ptr = NULL;
   }
}

static struct wsi_image *
wsi_android_get_wsi_image(struct wsi_swapchain *wsi_chain, uint32_t image_index)
{
   struct wsi_android_swapchain *chain =
      (struct wsi_android_swapchain *)wsi_chain;

   assert(image_index < chain->base.image_count);

   return &chain->images[image_index].base;
}

static VkResult
wsi_android_swapchain_result(struct wsi_android_swapchain *chain,
                             VkResult result)
{
   /* Prioritise returning existing errors for consistency. */
   if (chain->status < 0)
      return chain->status;

   /* Return temporary errors, but don't persist them. */
   if (result == VK_TIMEOUT || result == VK_NOT_READY)
      return result;

   if (result < 0 || result == VK_SUBOPTIMAL_KHR) {
#ifndef NDEBUG
      if (chain->status != result)
         mesa_logd("%s:%d: Swapchain status changed to %s\n", __FILE__,
                   __LINE__, vk_Result_to_str(result));
#endif
      chain->status = result;
   }

   return chain->status;
}

static VkResult
wsi_android_acquire_next_image(struct wsi_swapchain *wsi_chain,
                               const VkAcquireNextImageInfoKHR *info,
                               uint32_t *image_index)
{
   struct wsi_android_swapchain *chain =
      (struct wsi_android_swapchain *)wsi_chain;
   VkResult result;

   if (chain->status < 0)
      return chain->status;

   if (wsi_panvk_trace())
      fprintf(stderr,
              "PANVKDBG PRESENT ANDROID_ACQUIRE_ENTER chain=%p timeout=%" PRIu64
              " images=%u mode=%d\n",
              (void *)chain, info->timeout, chain->base.image_count,
              chain->base.present_mode);

   result = wsi_queue_pull(&chain->acquire_queue, image_index, info->timeout);
   if (result == VK_TIMEOUT && info->timeout == 0)
      result = VK_NOT_READY;

   if (result == VK_TIMEOUT || result == VK_NOT_READY)
      return result;

   if (result < 0) {
      mtx_lock(&chain->lock);
      result = wsi_android_swapchain_result(chain, result);
      mtx_unlock(&chain->lock);
      if (result < 0)
         return result;
   }

   assert(*image_index < chain->base.image_count);

   /* A buffer only stays locked for one frame, so every acquire after the first
    * one has to take a fresh buffer from the window.  This is what throttles
    * the swapchain to what the compositor can actually consume. */
   mtx_lock(&chain->lock);
   if (!chain->images[*image_index].locked &&
       !wsi_android_lock_image(
          &chain->images[*image_index],
          (unsigned)chain->base.image_info.linear_size)) {
      /* Give the image back so it is not lost, and let the caller rebuild. */
      wsi_queue_push(&chain->acquire_queue, *image_index);
      result = wsi_android_swapchain_result(chain, VK_ERROR_OUT_OF_DATE_KHR);
      mtx_unlock(&chain->lock);
      return result;
   }
   mtx_unlock(&chain->lock);

   return chain->status < 0 ? chain->status : VK_SUCCESS;
}

static void
wsi_android_copy_blit_to_buffer(struct wsi_android_swapchain *chain,
                                uint32_t image_index)
{
   struct wsi_android_image *image = &chain->images[image_index];
   const uint32_t blit_stride = image->base.row_pitches[0];
   const uint8_t *src = image->base.cpu_map;

   if (!image->locked || !image->buffer_ptr || !src || !blit_stride)
      return;

   /* The blit target and the compositor buffer have the same layout by
    * construction, so this is one straight copy of the frame. */
   assert(blit_stride * chain->extent.height <= image->base.sizes[0]);

   for (uint32_t y = 0; y < chain->extent.height; y++) {
      memcpy(image->buffer_ptr + (size_t)y * image->buffer_stride,
             src + (size_t)y * blit_stride,
             MIN2(blit_stride, image->buffer_stride));
   }

   if (wsi_panvk_trace())
      fprintf(stderr,
              "PANVKDBG ANDROID_COPY image=%u blit_stride=%u "
              "buffer_stride=%u height=%u\n",
              image_index, blit_stride, image->buffer_stride,
              chain->extent.height);
}

static VkResult
wsi_android_queue_present(struct wsi_swapchain *wsi_chain,
                          uint32_t image_index, uint64_t present_id,
                          const VkPresentRegionKHR *damage)
{
   struct wsi_android_swapchain *chain =
      (struct wsi_android_swapchain *)wsi_chain;
   VkResult result;

   if (wsi_panvk_trace())
      fprintf(stderr,
              "PANVKDBG PRESENT ANDROID_QUEUE_ENTER chain=%p image=%u "
              "present_id=%" PRIu64 " blit=%d\n",
              (void *)chain, image_index, present_id, chain->base.blit.type);

   if (chain->status < 0)
      return chain->status;

   assert(image_index < chain->base.image_count);

   /* The generic queue-present path already waited on the present fence for a
    * software device, so the blit result in cpu_map is complete here. */
   wsi_android_copy_blit_to_buffer(chain, image_index);

   mtx_lock(&chain->lock);

   if (chain->images[image_index].locked) {
      /* unlockAndPost is the only way to hand a locked buffer back, and it is
       * also what queues the frame for the compositor. */
      if (ANativeWindow_unlockAndPost(chain->window) != 0) {
         result =
            wsi_android_swapchain_result(chain, VK_ERROR_SURFACE_LOST_KHR);
      } else {
         chain->images[image_index].locked = false;
         chain->images[image_index].buffer_ptr = NULL;
         wsi_queue_push(&chain->acquire_queue, image_index);
         result = chain->status;
      }
   } else {
      result = wsi_android_swapchain_result(chain, VK_ERROR_SURFACE_LOST_KHR);
   }

   mtx_unlock(&chain->lock);

   if (wsi_panvk_trace())
      fprintf(stderr,
              "PANVKDBG ANDROID_POST image=%u result=%d present_id=%" PRIu64
              "\n",
              image_index, result, present_id);

   return result;
}

static VkResult
wsi_android_release_images(struct wsi_swapchain *wsi_chain,
                           uint32_t count, const uint32_t *indices)
{
   struct wsi_android_swapchain *chain =
      (struct wsi_android_swapchain *)wsi_chain;

   for (uint32_t i = 0; i < count; i++) {
      uint32_t index = indices[i];
      assert(index < chain->base.image_count);
      wsi_queue_push(&chain->acquire_queue, index);
   }

   return VK_SUCCESS;
}

static void
wsi_android_set_present_mode(struct wsi_swapchain *wsi_chain,
                             VkPresentModeKHR mode)
{
   struct wsi_android_swapchain *chain =
      (struct wsi_android_swapchain *)wsi_chain;

   chain->base.present_mode = mode;
}

static VkResult
wsi_android_swapchain_destroy(struct wsi_swapchain *wsi_chain,
                              const VkAllocationCallbacks *pAllocator)
{
   struct wsi_android_swapchain *chain =
      (struct wsi_android_swapchain *)wsi_chain;

   for (unsigned i = 0; i < wsi_chain->image_count; i++) {
      /* Hand any buffer we still hold back to the compositor before dropping
       * the swapchain, otherwise the BufferQueue would never release it. */
      wsi_android_image_finish(&chain->images[i]);
      wsi_destroy_image(&chain->base, &chain->images[i].base);
   }

   wsi_queue_destroy(&chain->present_queue);
   wsi_queue_destroy(&chain->acquire_queue);
   mtx_destroy(&chain->lock);
   wsi_swapchain_finish(&chain->base);

   vk_free(pAllocator, chain);

   return VK_SUCCESS;
}

static VkResult
wsi_android_surface_create_swapchain(VkIcdSurfaceBase *icd_surface,
                                     VkDevice device,
                                     struct wsi_device *wsi_device,
                                     const VkSwapchainCreateInfoKHR
                                        *pCreateInfo,
                                     const VkAllocationCallbacks *pAllocator,
                                     struct wsi_swapchain **swapchain_out)
{
   struct wsi_android_swapchain *chain;
   struct wsi_cpu_image_params cpu_image_params = {
      .base.image_type = WSI_IMAGE_TYPE_CPU,
      .alloc_shm = wsi_android_alloc_buffer,
   };
   VkPresentModeKHR present_mode =
      wsi_swapchain_get_present_mode(wsi_device, pCreateInfo);
   VkResult result;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);

   struct ANativeWindow *window = wsi_android_surface_get_window(icd_surface);
   if (!window)
      return VK_ERROR_SURFACE_LOST_KHR;

   if (wsi_panvk_trace())
      fprintf(stderr,
              "PANVKDBG PRESENT ANDROID_CREATE_ENTER window=%p format=%d "
              "extent=%ux%u minImages=%u mode=%d\n",
              (void *)window, pCreateInfo->imageFormat,
              pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height,
              pCreateInfo->minImageCount, present_mode);

   /* An app that ignores the advertised formats and asks for BGRA still gets
    * what it asked for, by giving the compositor the same layout.  The blit
    * buffer has the swapchain format, so the copy stays a straight memcpy. */
   int32_t window_format = WINDOW_FORMAT_RGBA_8888;
   if (pCreateInfo->imageFormat == VK_FORMAT_B8G8R8A8_UNORM ||
       pCreateInfo->imageFormat == VK_FORMAT_B8G8R8A8_SRGB)
      window_format = WINDOW_FORMAT_BGRA_8888;

   /* A zero extent means "whatever the surface currently is". */
   VkExtent2D extent = pCreateInfo->imageExtent;
   uint32_t width, height;

   if (extent.width == 0 || extent.height == 0) {
      if (!wsi_android_window_geometry(window, &width, &height))
         return VK_ERROR_SURFACE_LOST_KHR;
      extent = (VkExtent2D){width, height};
   }

   unsigned num_images =
      CLAMP(pCreateInfo->minImageCount, WSI_ANDROID_MIN_IMAGES,
            WSI_ANDROID_MAX_IMAGES);

   size_t size = sizeof(*chain) + num_images * sizeof(chain->images[0]);
   chain = vk_zalloc(pAllocator, size, 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (chain == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   chain->images = (struct wsi_android_image *)(chain + 1);
   chain->window = window;
   chain->extent = extent;

   int ret = mtx_init(&chain->lock, mtx_plain);
   if (ret != thrd_success) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail_alloc;
   }

   if (ANativeWindow_setBuffersGeometry(
          window, (int32_t)extent.width, (int32_t)extent.height,
          window_format) != 0) {
      result = VK_ERROR_SURFACE_LOST_KHR;
      goto fail_mutex;
   }
   ANativeWindow_setBuffersTransform(window, NATIVE_WINDOW_TRANSFORM_IDENTITY);
   /* One extra buffer so the compositor always has a front buffer while the
    * swapchain holds the rest locked. */
   ANativeWindow_setBufferCount(window, (int32_t)num_images + 1);

   /* ANativeWindow_lock() blocks until the compositor frees a buffer, so the
    * number of images has to fit the number of buffers the window really gave
    * us, otherwise creating the swapchain would hang. */
   int32_t buffer_count = ANativeWindow_getBuffersCount(window);
   if (buffer_count > 0 && (unsigned)buffer_count < num_images)
      num_images = (unsigned)buffer_count;

   result = wsi_swapchain_init(wsi_device, &chain->base, device, pCreateInfo,
                               &cpu_image_params.base, pAllocator);
   if (result != VK_SUCCESS)
      goto fail_mutex;

   chain->base.destroy = wsi_android_swapchain_destroy;
   chain->base.get_wsi_image = wsi_android_get_wsi_image;
   chain->base.acquire_next_image = wsi_android_acquire_next_image;
   chain->base.queue_present = wsi_android_queue_present;
   chain->base.release_images = wsi_android_release_images;
   chain->base.set_present_mode = wsi_android_set_present_mode;
   chain->base.present_mode = present_mode;
   chain->base.image_count = num_images;
   chain->base.present_timing.time_domain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;

   unsigned i;
   for (i = 0; i < chain->base.image_count; i++) {
      result = wsi_android_image_init(chain, &chain->base.image_info,
                                      &chain->images[i]);
      if (result != VK_SUCCESS)
         goto fail_init_images;
   }

   result = wsi_queue_init(&chain->present_queue, chain->base.image_count + 1);
   if (result != VK_SUCCESS)
      goto fail_init_images;

   result = wsi_queue_init(&chain->acquire_queue, chain->base.image_count + 1);
   if (result != VK_SUCCESS)
      goto fail_init_present_queue;

   for (i = 0; i < chain->base.image_count; i++)
      wsi_queue_push(&chain->acquire_queue, i);

   *swapchain_out = &chain->base;

   if (wsi_panvk_trace())
      fprintf(stderr,
              "PANVKDBG PRESENT ANDROID_CREATE_OK images=%u blit=%d "
              "window=%p\n",
              chain->base.image_count, chain->base.blit.type, (void *)window);

   return VK_SUCCESS;

fail_init_present_queue:
   wsi_queue_destroy(&chain->present_queue);
fail_init_images:
   /* wsi_create_image() tears down the image it failed on, so only the images
    * before it still need wsi_destroy_image(); every buffer that did get locked
    * does have to be handed back to the compositor either way. */
   for (unsigned j = 0; j <= i && j < chain->base.image_count; j++) {
      wsi_android_image_finish(&chain->images[j]);
      if (j < i)
         wsi_destroy_image(&chain->base, &chain->images[j].base);
   }
   wsi_swapchain_finish(&chain->base);
fail_mutex:
   mtx_destroy(&chain->lock);
fail_alloc:
   vk_free(pAllocator, chain);

   return result;
}

VkResult
wsi_android_init_wsi(struct wsi_device *wsi_device,
                     const VkAllocationCallbacks *alloc,
                     VkPhysicalDevice physical_device)
{
   struct wsi_interface *wsi = wsi_device->wsi[VK_ICD_WSI_PLATFORM_ANDROID];

   if (!wsi) {
      wsi = vk_zalloc(alloc, sizeof(*wsi), 8,
                      VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
      if (!wsi)
         return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   wsi->get_support = wsi_android_surface_get_support;
   wsi->get_capabilities2 = wsi_android_surface_get_capabilities2;
   wsi->get_formats = wsi_android_surface_get_formats;
   wsi->get_formats2 = wsi_android_surface_get_formats2;
   wsi->get_present_modes = wsi_android_surface_get_present_modes;
   wsi->get_present_rectangles = wsi_android_surface_get_present_rectangles;
   wsi->create_swapchain = wsi_android_surface_create_swapchain;

   wsi_device->wsi[VK_ICD_WSI_PLATFORM_ANDROID] = wsi;

   return VK_SUCCESS;
}

void
wsi_android_finish_wsi(struct wsi_device *wsi_device,
                       const VkAllocationCallbacks *alloc)
{
   struct wsi_interface *wsi =
      wsi_device->wsi[VK_ICD_WSI_PLATFORM_ANDROID];

   if (wsi) {
      wsi_device->wsi[VK_ICD_WSI_PLATFORM_ANDROID] = NULL;
      vk_free(alloc, wsi);
   }
}
