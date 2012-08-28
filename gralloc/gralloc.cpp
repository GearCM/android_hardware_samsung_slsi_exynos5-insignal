/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef HAVE_ANDROID_OS      // just want PAGE_SIZE define
# include <asm/page.h>
#else
# include <sys/user.h>
#endif
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>

#include <ion/ion.h>
#include <linux/ion.h>
#include <cutils/log.h>
#include <cutils/atomic.h>

#include <hardware/hardware.h>
#include <hardware/gralloc.h>

#include "gralloc_priv.h"
#include "exynos_format.h"
#include "gr.h"

#define ION_HEAP_EXYNOS_CONTIG_MASK     (1 << 4)
#define ION_EXYNOS_VIDEO_MASK           (1 << 29)

/*****************************************************************************/

struct gralloc_context_t {
    alloc_device_t  device;
    /* our private data here */
};

static int gralloc_alloc_buffer(alloc_device_t* dev,
                                size_t size, int usage, buffer_handle_t* pHandle);

/*****************************************************************************/

int fb_device_open(const hw_module_t* module, const char* name,
                   hw_device_t** device);

static int gralloc_device_open(const hw_module_t* module, const char* name,
                               hw_device_t** device);

extern int gralloc_lock(gralloc_module_t const* module,
                        buffer_handle_t handle, int usage,
                        int l, int t, int w, int h,
                        void** vaddr);

extern int gralloc_unlock(gralloc_module_t const* module, 
                          buffer_handle_t handle);

extern int gralloc_register_buffer(gralloc_module_t const* module,
                                   buffer_handle_t handle);

extern int gralloc_unregister_buffer(gralloc_module_t const* module,
                                     buffer_handle_t handle);

/*****************************************************************************/

static struct hw_module_methods_t gralloc_module_methods = {
open: gralloc_device_open
};

struct private_module_t HAL_MODULE_INFO_SYM = {
base: {
common: {
tag: HARDWARE_MODULE_TAG,
     version_major: 1,
     version_minor: 0,
     id: GRALLOC_HARDWARE_MODULE_ID,
     name: "Graphics Memory Allocator Module",
     author: "The Android Open Source Project",
     methods: &gralloc_module_methods
        },
    registerBuffer: gralloc_register_buffer,
    unregisterBuffer: gralloc_unregister_buffer,
    lock: gralloc_lock,
    unlock: gralloc_unlock,
      },
    framebuffer: 0,
    flags: 0,
    numBuffers: 0,
    bufferMask: 0,
    lock: PTHREAD_MUTEX_INITIALIZER,
    currentBuffer: 0,
    ionfd: -1,
};

/*****************************************************************************/

#define ALIGN(x, a) (((x) + (a - 1)) & ~(a - 1))

static unsigned int _select_heap(int usage)
{
    unsigned int heap_mask;

    if (usage & GRALLOC_USAGE_PROTECTED)
        heap_mask = ION_HEAP_EXYNOS_CONTIG_MASK;
    else
        heap_mask = ION_HEAP_SYSTEM_MASK;

    return heap_mask;
}

static int gralloc_alloc_rgb(int ionfd, int w, int h, int format, int usage,
                             unsigned int ion_flags, private_handle_t **hnd, int *stride)
{
    size_t size, bpr;
    int bpp = 0, vstride, fd, err;
    unsigned int heap_mask = _select_heap(usage);

    switch (format) {
        case HAL_PIXEL_FORMAT_RGBA_8888:
        case HAL_PIXEL_FORMAT_RGBX_8888:
        case HAL_PIXEL_FORMAT_BGRA_8888:
            bpp = 4;
            break;
        case HAL_PIXEL_FORMAT_RGB_888:
            bpp = 3;
            break;
        case HAL_PIXEL_FORMAT_RGB_565:
        case HAL_PIXEL_FORMAT_RGBA_5551:
        case HAL_PIXEL_FORMAT_RGBA_4444:
        case HAL_PIXEL_FORMAT_RAW_SENSOR:
            bpp = 2;
            break;
        case HAL_PIXEL_FORMAT_BLOB:
            bpp = 1;
            break;
        default:
            return -EINVAL;
    }
    bpr = ALIGN(w*bpp, 16);
    vstride = ALIGN(h, 16);
    size = bpr * vstride;
    *stride = bpr / bpp;
    size = ALIGN(size, PAGE_SIZE);

    err = ion_alloc_fd(ionfd, size, 0, heap_mask, ion_flags,
                       &fd);
    *hnd = new private_handle_t(fd, size, usage, w, h, format, *stride,
                                vstride);

    return err;
}

static int gralloc_alloc_framework_yuv(int ionfd, int w, int h, int format,
                                       int usage, unsigned int ion_flags,
                                       private_handle_t **hnd, int *stride)
{
    size_t size;
    int err, fd;

    switch (format) {
        case HAL_PIXEL_FORMAT_YV12:
            *stride = ALIGN(w, 16);
            break;
        case HAL_PIXEL_FORMAT_YCrCb_420_SP:
            *stride = w;
            break;
        default:
            ALOGE("invalid yuv format %d\n", format);
            return -EINVAL;
    }

    size = *stride * h * 3 / 2;
    err = ion_alloc_fd(ionfd, size, 0, 1 << ION_HEAP_TYPE_SYSTEM,
                       ion_flags, &fd);
    if (err)
        return err;

    *hnd = new private_handle_t(fd, size, usage, w, h, format, *stride, h);
    return err;
}

static int gralloc_alloc_yuv(int ionfd, int w, int h, int format,
                             int usage, unsigned int ion_flags,
                             private_handle_t **hnd, int *stride)
{
    size_t luma_size, chroma_size;
    int err, planes, fd, fd1, fd2 = 0;
    size_t luma_vstride;
    unsigned int heap_mask = _select_heap(usage);

    *stride = ALIGN(w, 16);

    switch (format) {
        case HAL_PIXEL_FORMAT_EXYNOS_YV12:
            {
                luma_vstride = ALIGN(h, 16);
                luma_size = luma_vstride * *stride;
                chroma_size = (luma_vstride / 2) * ALIGN(*stride / 2, 16);
                planes = 3;
                break;
            }
        case HAL_PIXEL_FORMAT_EXYNOS_YCrCb_420_SP:
        case HAL_PIXEL_FORMAT_YCbCr_420_SP:
        case HAL_PIXEL_FORMAT_YCbCr_420_SP_TILED:
            {
                size_t chroma_vstride = ALIGN(h / 2, 32);
                luma_vstride = ALIGN(h, 32);
                luma_size = luma_vstride * *stride;
                chroma_size = chroma_vstride * *stride;
                planes = 2;
                break;
            }
        case HAL_PIXEL_FORMAT_YV12:
        case HAL_PIXEL_FORMAT_YCrCb_420_SP:
            return gralloc_alloc_framework_yuv(ionfd, w, h, format, usage,
                                               ion_flags, hnd, stride);
        default:
            ALOGE("invalid yuv format %d\n", format);
            return -EINVAL;
    }

    err = ion_alloc_fd(ionfd, luma_size, 0, heap_mask, ion_flags, &fd);
    if (err)
        return err;
    err = ion_alloc_fd(ionfd, chroma_size, 0, heap_mask, ion_flags, &fd1);
    if (err)
        goto err1;
    if (planes == 3) {
        err = ion_alloc_fd(ionfd, chroma_size, 0, heap_mask, ion_flags, &fd2);
        if (err)
            goto err2;

        *hnd = new private_handle_t(fd, fd1, fd2, luma_size, usage, w, h,
                                    format, *stride, luma_vstride);
    } else {
        *hnd = new private_handle_t(fd, fd1, luma_size, usage, w, h, format,
                                    *stride, luma_vstride);
    }
    return err;

err2:
    close(fd1);
err1:
    close(fd);
    return err;
}

static int gralloc_alloc(alloc_device_t* dev,
                         int w, int h, int format, int usage,
                         buffer_handle_t* pHandle, int* pStride)
{
    int stride;
    int err;
    unsigned int ion_flags = 0;
    private_handle_t *hnd;

    if (!pHandle || !pStride)
        return -EINVAL;

    if( (usage & GRALLOC_USAGE_SW_READ_MASK) == GRALLOC_USAGE_SW_READ_OFTEN )
        ion_flags = ION_FLAG_CACHED;
    if (usage & GRALLOC_USAGE_PROTECTED)
        ion_flags |= ION_EXYNOS_VIDEO_MASK;

    private_module_t* m = reinterpret_cast<private_module_t*>
        (dev->common.module);
    gralloc_module_t* module = reinterpret_cast<gralloc_module_t*>
        (dev->common.module);

    err = gralloc_alloc_rgb(m->ionfd, w, h, format, usage, ion_flags, &hnd,
                            &stride);
    if (err)
        err = gralloc_alloc_yuv(m->ionfd, w, h, format, usage, ion_flags,
                                &hnd, &stride);
    if (err)
        return err;

    err = grallocMap(module, hnd);

    if (err != 0)
        goto err;

    *pHandle = hnd;
    *pStride = stride;
    return 0;
err:
    close(hnd->fd);
    if (hnd->fd1 >= 0)
        close(hnd->fd1);
    if (hnd->fd2 >= 0)
        close(hnd->fd2);
    return err;
}

static int gralloc_free(alloc_device_t* dev,
                        buffer_handle_t handle)
{
    if (private_handle_t::validate(handle) < 0)
        return -EINVAL;

    private_handle_t const* hnd = reinterpret_cast<private_handle_t const*>(handle);
    gralloc_module_t* module = reinterpret_cast<gralloc_module_t*>(
                                                                   dev->common.module);

    grallocUnmap(module, const_cast<private_handle_t*>(hnd));
    close(hnd->fd);
    if (hnd->fd1 >= 0)
        close(hnd->fd1);
    if (hnd->fd2 >= 0)
        close(hnd->fd2);

    delete hnd;
    return 0;
}

/*****************************************************************************/

static int gralloc_close(struct hw_device_t *dev)
{
    gralloc_context_t* ctx = reinterpret_cast<gralloc_context_t*>(dev);
    if (ctx) {
        /* TODO: keep a list of all buffer_handle_t created, and free them
         * all here.
         */
        free(ctx);
    }
    return 0;
}

int gralloc_device_open(const hw_module_t* module, const char* name,
                        hw_device_t** device)
{
    int status = -EINVAL;
    if (!strcmp(name, GRALLOC_HARDWARE_GPU0)) {
        gralloc_context_t *dev;
        dev = (gralloc_context_t*)malloc(sizeof(*dev));

        /* initialize our state here */
        memset(dev, 0, sizeof(*dev));

        /* initialize the procs */
        dev->device.common.tag = HARDWARE_DEVICE_TAG;
        dev->device.common.version = 0;
        dev->device.common.module = const_cast<hw_module_t*>(module);
        dev->device.common.close = gralloc_close;

        dev->device.alloc = gralloc_alloc;
        dev->device.free = gralloc_free;

        private_module_t *p = reinterpret_cast<private_module_t*>(dev->device.common.module);
        p->ionfd = ion_open();

        *device = &dev->device.common;
        status = 0;
    } else {
        status = fb_device_open(module, name, device);
    }
    return status;
}