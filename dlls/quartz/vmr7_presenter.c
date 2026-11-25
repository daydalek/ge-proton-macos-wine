/* GE macOS Wine PoC modification notice: 2026-11-24; GE media integration and macOS adaptations. Original copyright and license retained. See MODIFICATIONS.json. */
/*
 * Default allocator-presenter for the VMR7
 *
 * Copyright 2023 Zebediah Figura for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "quartz_private.h"

#include "wine/debug.h"
#include "wine/macdrv_video.h"

WINE_DEFAULT_DEBUG_CHANNEL(quartz);

struct vmr7_presenter
{
    IVMRImagePresenter IVMRImagePresenter_iface;
    IVMRSurfaceAllocator IVMRSurfaceAllocator_iface;
    IVMRWindowlessControl IVMRWindowlessControl_iface;
    LONG refcount;

    IVMRSurfaceAllocatorNotify *notify;

    IDirectDraw7 *ddraw;
    IDirectDrawSurface7 *frontbuffer;
    IDirectDrawSurface7 *primary;
    HWND window;
    BOOL yuv_to_rgb;

    /* Native winemac video presentation state.  native_support is 0 while
     * unprobed, 1 when the driver accepted QUERYESCSUPPORT, -1 otherwise.
     * native_root is the root HWND the native surface lives on; it can
     * outlive the video child window, so release goes through it. */
    int native_support;
    HWND native_root;
    BYTE *native_frame;
    SIZE_T native_frame_size;

    SIZE native_size, aspect_ratio;
};

static struct vmr7_presenter *impl_from_IVMRImagePresenter(IVMRImagePresenter *iface)
{
    return CONTAINING_RECORD(iface, struct vmr7_presenter, IVMRImagePresenter_iface);
}

static void vmr7_presenter_release_native(struct vmr7_presenter *presenter);

static HRESULT WINAPI image_presenter_QueryInterface(IVMRImagePresenter *iface, REFIID iid, void **out)
{
    struct vmr7_presenter *presenter = impl_from_IVMRImagePresenter(iface);

    TRACE("presenter %p, iid %s, out %p.\n", presenter, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IVMRImagePresenter))
        *out = iface;
    else if (IsEqualGUID(iid, &IID_IVMRSurfaceAllocator))
        *out = &presenter->IVMRSurfaceAllocator_iface;
    else if (IsEqualGUID(iid, &IID_IVMRWindowlessControl))
        *out = &presenter->IVMRWindowlessControl_iface;
    else
    {
        *out = NULL;
        WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static ULONG WINAPI image_presenter_AddRef(IVMRImagePresenter *iface)
{
    struct vmr7_presenter *presenter = impl_from_IVMRImagePresenter(iface);
    ULONG refcount = InterlockedIncrement(&presenter->refcount);

    TRACE("%p increasing refcount to %lu.\n", presenter, refcount);
    return refcount;
}

static ULONG WINAPI image_presenter_Release(IVMRImagePresenter *iface)
{
    struct vmr7_presenter *presenter = impl_from_IVMRImagePresenter(iface);
    ULONG refcount = InterlockedDecrement(&presenter->refcount);

    TRACE("%p decreasing refcount to %lu.\n", presenter, refcount);
    if (!refcount)
    {
        vmr7_presenter_release_native(presenter);
        free(presenter->native_frame);
        if (presenter->frontbuffer)
            IDirectDrawSurface7_Release(presenter->frontbuffer);
        IDirectDrawSurface7_Release(presenter->primary);
        IDirectDraw7_Release(presenter->ddraw);
        free(presenter);
    }
    return refcount;
}

static BOOL vmr7_presenter_native_supported(struct vmr7_presenter *presenter)
{
    DWORD escape = MACDRV_VIDEO_ESCAPE_ID;
    HDC dc;

    if (!presenter->window) return FALSE;
    if (presenter->native_support) return presenter->native_support > 0;
    if (!(dc = GetDC(presenter->window))) return FALSE;
    presenter->native_support =
            ExtEscape(dc, QUERYESCSUPPORT, sizeof(escape), (const char *)&escape, 0, NULL) > 0 ? 1 : -1;
    ReleaseDC(presenter->window, dc);
    return presenter->native_support > 0;
}

static void vmr7_presenter_release_native(struct vmr7_presenter *presenter)
{
    struct macdrv_video_header header;
    HDC dc;

    if (!presenter->native_root) return;
    memset(&header, 0, sizeof(header));
    header.version = MACDRV_VIDEO_PROTOCOL_VERSION;
    header.command = MACDRV_VIDEO_CMD_RELEASE;
    header.token = (uint64_t)(ULONG_PTR)presenter;
    /* Release through the root HWND: the video child window may already
     * be destroyed while the game window still exists. */
    if ((dc = GetDC(presenter->native_root)))
    {
        ExtEscape(dc, MACDRV_VIDEO_ESCAPE_ID, sizeof(header), (const char *)&header, 0, NULL);
        ReleaseDC(presenter->native_root, dc);
    }
    presenter->native_root = NULL;
}

static BOOL vmr7_presenter_present_native(struct vmr7_presenter *presenter, IDirectDrawSurface7 *surface)
{
    struct macdrv_video_header *header;
    DDSURFACEDESC2 desc = {.dwSize = sizeof(desc)};
    uint64_t payload_size, total_size;
    BOOL ret = FALSE;
    HDC dc;

    if (!vmr7_presenter_native_supported(presenter))
        return FALSE;

    if (FAILED(IDirectDrawSurface7_Lock(surface, NULL, &desc, DDLOCK_READONLY | DDLOCK_WAIT, NULL)))
        return FALSE;

    if ((desc.ddpfPixelFormat.dwFlags & DDPF_RGB)
            && desc.ddpfPixelFormat.dwRGBBitCount == 32
            && desc.ddpfPixelFormat.dwRBitMask == 0x00ff0000
            && desc.ddpfPixelFormat.dwGBitMask == 0x0000ff00
            && desc.ddpfPixelFormat.dwBBitMask == 0x000000ff
            && desc.lPitch != INT_MIN && desc.lPitch != 0
            && desc.dwWidth && desc.dwHeight
            && (uint64_t)desc.dwWidth * 4 <= (uint64_t)INT_MAX
            && (desc.lPitch < 0 ? -(int64_t)desc.lPitch : desc.lPitch) >= (int64_t)desc.dwWidth * 4
            && macdrv_video_present_payload_size(desc.dwWidth, desc.dwHeight,
                                                 desc.dwWidth * 4, &payload_size))
    {
        total_size = sizeof(*header) + payload_size;
        if (presenter->native_frame_size < total_size)
        {
            BYTE *frame = realloc(presenter->native_frame, (size_t)total_size);
            if (frame)
            {
                presenter->native_frame = frame;
                presenter->native_frame_size = (size_t)total_size;
            }
        }
        if (presenter->native_frame_size >= total_size
                && macdrv_video_copy_rows(presenter->native_frame + sizeof(*header),
                        desc.lpSurface, desc.lPitch, desc.dwWidth, desc.dwHeight))
        {
            header = (struct macdrv_video_header *)presenter->native_frame;
            memset(header, 0, sizeof(*header));
            header->version = MACDRV_VIDEO_PROTOCOL_VERSION;
            header->command = MACDRV_VIDEO_CMD_PRESENT;
            header->token = (uint64_t)(ULONG_PTR)presenter;
            header->width = desc.dwWidth;
            header->height = desc.dwHeight;
            header->stride = desc.dwWidth * 4;
            ret = TRUE;
        }
    }

    IDirectDrawSurface7_Unlock(surface, NULL);

    if (!ret) return FALSE;
    ret = FALSE;
    if ((dc = GetDC(presenter->window)))
    {
        HWND root = GetAncestor(presenter->window, GA_ROOT);
        if (root)
        {
            if (presenter->native_root && presenter->native_root != root)
                /* The video window moved to a different root; drop the
                 * surface on the old root before presenting to the new. */
                vmr7_presenter_release_native(presenter);
            ret = ExtEscape(dc, MACDRV_VIDEO_ESCAPE_ID, (INT)total_size,
                    (const char *)presenter->native_frame, 0, NULL) > 0;
            if (ret) presenter->native_root = root;
        }
        /* No root means a native success could never be released; skip the
         * present so the caller releases any stale frame and falls back. */
        ReleaseDC(presenter->window, dc);
    }
    return ret;
}

static BOOL vmr7_presenter_try_native(struct vmr7_presenter *presenter, IDirectDrawSurface7 *surface)
{
    /* Every failed native attempt must drop a stale native frame before
     * the GDI fallback paints underneath it. */
    if (vmr7_presenter_present_native(presenter, surface))
        return TRUE;
    vmr7_presenter_release_native(presenter);
    return FALSE;
}

static HRESULT WINAPI image_presenter_StartPresenting(IVMRImagePresenter *iface, DWORD_PTR cookie)
{
    if (GetModuleHandleW(L"winemac.drv"))
    {
        TRACE("iface %p, cookie %#Ix.\n", iface, cookie);
        return S_OK;
    }
    FIXME("iface %p, cookie %#Ix, stub!\n", iface, cookie);
    return E_NOTIMPL;
}

static HRESULT WINAPI image_presenter_StopPresenting(IVMRImagePresenter *iface, DWORD_PTR cookie)
{
    struct vmr7_presenter *presenter = impl_from_IVMRImagePresenter(iface);

    if (GetModuleHandleW(L"winemac.drv"))
    {
        TRACE("iface %p, cookie %#Ix.\n", iface, cookie);
        vmr7_presenter_release_native(presenter);
        return S_OK;
    }
    FIXME("iface %p, cookie %#Ix, stub!\n", iface, cookie);
    return E_NOTIMPL;
}

static HRESULT WINAPI image_presenter_PresentImage(IVMRImagePresenter *iface,
        DWORD_PTR cookie, VMRPRESENTATIONINFO *info)
{
    struct vmr7_presenter *presenter = impl_from_IVMRImagePresenter(iface);
    POINT point;
    HRESULT hr;
    RECT rect;

    TRACE("iface %p, cookie %#Ix, info %p.\n", iface, cookie, info);

    TRACE("flags %#lx, surface %p, start %s, end %s, aspect ratio %ldx%ld,\n",
            info->dwFlags, info->lpSurf, debugstr_time(info->rtStart),
            debugstr_time(info->rtEnd), info->szAspectRatio.cx, info->szAspectRatio.cy);
    TRACE("src %s, dst %s, type-specific flags %#lx, interlace flags %#lx.\n",
            wine_dbgstr_rect(&info->rcSrc), wine_dbgstr_rect(&info->rcDst),
            info->dwTypeSpecificFlags, info->dwInterlaceFlags);

    if (info->dwFlags & VMRSample_SrcDstRectsValid)
        FIXME("Ignoring src/dst rects.\n");

    /* macdrv presents each window through its client surface. A DirectDraw
     * primary-surface blit does not update the VMR window client surface. */
    if (GetModuleHandleW(L"winemac.drv"))
    {
        HDC source_dc, target_dc;
        DDSURFACEDESC2 desc = {.dwSize = sizeof(desc)};

        if (vmr7_presenter_try_native(presenter, info->lpSurf))
            return S_OK;

        if (SUCCEEDED(hr = IDirectDrawSurface7_GetSurfaceDesc(info->lpSurf, &desc))
                && SUCCEEDED(hr = IDirectDrawSurface7_GetDC(info->lpSurf, &source_dc)))
        {
            BOOL copied;
            GetClientRect(presenter->window, &rect);
            target_dc = GetDC(presenter->window);
            copied = target_dc && StretchBlt(target_dc, 0, 0,
                    rect.right - rect.left, rect.bottom - rect.top,
                    source_dc, 0, 0, desc.dwWidth, desc.dwHeight, SRCCOPY);
            if (target_dc) ReleaseDC(presenter->window, target_dc);
            IDirectDrawSurface7_ReleaseDC(info->lpSurf, source_dc);
            TRACE("macdrv window presentation copied %d, size %lux%lu.\n",
                    copied, desc.dwWidth, desc.dwHeight);
            if (copied) return S_OK;
        }
        WARN("macdrv window presentation failed, hr %#lx.\n", hr);
    }

    if (presenter->yuv_to_rgb)
    {
        DDSURFACEDESC2 surface_desc = {.dwSize = sizeof(surface_desc)};
        BITMAPINFO bitmap_info = {0};
        HDC dc;

        if (SUCCEEDED(IDirectDrawSurface7_Lock(info->lpSurf, NULL, &surface_desc, DDLOCK_WAIT | DDLOCK_READONLY, NULL)))
        {
            GetClientRect(presenter->window, &rect);

            bitmap_info.bmiHeader.biSize = sizeof(bitmap_info.bmiHeader);
            bitmap_info.bmiHeader.biWidth = surface_desc.dwWidth;
            bitmap_info.bmiHeader.biHeight = -surface_desc.dwHeight;
            bitmap_info.bmiHeader.biPlanes = 1;
            bitmap_info.bmiHeader.biBitCount = 32;
            bitmap_info.bmiHeader.biCompression = BI_RGB;

            dc = GetDC(presenter->window);
            StretchDIBits(dc, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                    0, 0, surface_desc.dwWidth, surface_desc.dwHeight, surface_desc.lpSurface,
                    &bitmap_info, DIB_RGB_COLORS, SRCCOPY);
            ReleaseDC(presenter->window, dc);
            IDirectDrawSurface7_Unlock(info->lpSurf, NULL);
            return S_OK;
        }
    }

    GetClientRect(presenter->window, &rect);
    point.x = point.y = 0;
    ClientToScreen(presenter->window, &point);
    OffsetRect(&rect, point.x, point.y);
    if (FAILED(hr = IDirectDrawSurface7_Blt(presenter->primary, &rect, info->lpSurf, NULL, DDBLT_WAIT, NULL)))
        ERR("Failed to blit, hr %#lx.\n", hr);

    return S_OK;
}

static const IVMRImagePresenterVtbl image_presenter_vtbl =
{
    image_presenter_QueryInterface,
    image_presenter_AddRef,
    image_presenter_Release,
    image_presenter_StartPresenting,
    image_presenter_StopPresenting,
    image_presenter_PresentImage,
};

static struct vmr7_presenter *impl_from_IVMRSurfaceAllocator(IVMRSurfaceAllocator *iface)
{
    return CONTAINING_RECORD(iface, struct vmr7_presenter, IVMRSurfaceAllocator_iface);
}

static HRESULT WINAPI surface_allocator_QueryInterface(IVMRSurfaceAllocator *iface, REFIID iid, void **out)
{
    struct vmr7_presenter *presenter = impl_from_IVMRSurfaceAllocator(iface);

    return IVMRImagePresenter_QueryInterface(&presenter->IVMRImagePresenter_iface, iid, out);
}

static ULONG WINAPI surface_allocator_AddRef(IVMRSurfaceAllocator *iface)
{
    struct vmr7_presenter *presenter = impl_from_IVMRSurfaceAllocator(iface);

    return IVMRImagePresenter_AddRef(&presenter->IVMRImagePresenter_iface);
}

static ULONG WINAPI surface_allocator_Release(IVMRSurfaceAllocator *iface)
{
    struct vmr7_presenter *presenter = impl_from_IVMRSurfaceAllocator(iface);

    return IVMRImagePresenter_Release(&presenter->IVMRImagePresenter_iface);
}

static HRESULT WINAPI surface_allocator_AllocateSurface(IVMRSurfaceAllocator *iface,
        DWORD_PTR id, VMRALLOCATIONINFO *info, DWORD *count, IDirectDrawSurface7 **surface)
{
    struct vmr7_presenter *presenter = impl_from_IVMRSurfaceAllocator(iface);
    DDSURFACEDESC2 surface_desc = {.dwSize = sizeof(surface_desc)};
    HRESULT hr;

    TRACE("presenter %p, id %#Ix, info %p, count %p, surface %p.\n", presenter, id, info, count, surface);

    if (info->lpHdr->biSize != sizeof(*info->lpHdr))
    {
        WARN("Invalid BITMAPINFOHEADER size %lu.\n", info->lpHdr->biSize);
        return DDERR_INVALIDPIXELFORMAT;
    }

    surface_desc.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    surface_desc.dwWidth = info->lpHdr->biWidth;
    surface_desc.dwHeight = info->lpHdr->biHeight;
    surface_desc.ddpfPixelFormat.dwSize = sizeof(surface_desc.ddpfPixelFormat);
    surface_desc.ddsCaps.dwCaps = DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_OFFSCREENPLAIN;
    surface_desc.dwBackBufferCount = *count;
    if (GetModuleHandleW(L"winemac.drv")) surface_desc.ddsCaps.dwCaps |= DDSCAPS_SYSTEMMEMORY;

    presenter->yuv_to_rgb = info->lpHdr->biCompression == mmioFOURCC('N','V','1','2')
            || info->lpHdr->biCompression == mmioFOURCC('Y','V','1','2');

    if (presenter->yuv_to_rgb)
    {
        surface_desc.ddpfPixelFormat.dwFlags = DDPF_RGB;
        surface_desc.ddpfPixelFormat.dwRGBBitCount = 32;
        surface_desc.ddpfPixelFormat.dwRBitMask = 0x00ff0000;
        surface_desc.ddpfPixelFormat.dwGBitMask = 0x0000ff00;
        surface_desc.ddpfPixelFormat.dwBBitMask = 0x000000ff;
    }
    else if (info->lpHdr->biCompression == BI_RGB || info->lpHdr->biCompression == BI_BITFIELDS)
    {
        DDSURFACEDESC2 primary_desc;
        DWORD *mask;

        primary_desc.dwSize = sizeof(primary_desc);
        if (FAILED(hr = IDirectDrawSurface7_GetSurfaceDesc(presenter->primary, &primary_desc)))
            return hr;
        if (info->lpHdr->biBitCount != primary_desc.ddpfPixelFormat.dwRGBBitCount)
            return E_FAIL;

        if (info->lpHdr->biCompression == BI_RGB)
        {
            if (info->lpHdr->biBitCount != 32)
            {
                FIXME("Unhandled bit depth %u.\n", info->lpHdr->biBitCount);
                return E_NOTIMPL;
            }

            surface_desc.ddpfPixelFormat.dwFlags = DDPF_RGB;
            surface_desc.ddpfPixelFormat.dwRGBBitCount = 32;
            surface_desc.ddpfPixelFormat.dwRBitMask = 0x00ff0000;
            surface_desc.ddpfPixelFormat.dwGBitMask = 0x0000ff00;
            surface_desc.ddpfPixelFormat.dwBBitMask = 0x000000ff;
        }
        else
        {
            mask = (DWORD *)((BITMAPINFO *)info->lpHdr)->bmiColors;
            surface_desc.ddpfPixelFormat.dwFlags = DDPF_RGB;
            surface_desc.ddpfPixelFormat.dwRGBBitCount = info->lpHdr->biBitCount;
            surface_desc.ddpfPixelFormat.dwRBitMask = mask[0];
            surface_desc.ddpfPixelFormat.dwGBitMask = mask[1];
            surface_desc.ddpfPixelFormat.dwBBitMask = mask[2];
        }
    }
    else
    {
        surface_desc.ddpfPixelFormat.dwFlags = DDPF_FOURCC;
        surface_desc.ddpfPixelFormat.dwFourCC = info->lpHdr->biCompression;
    }

    if (FAILED(hr = IDirectDraw7_CreateSurface(presenter->ddraw,
            &surface_desc, &presenter->frontbuffer, NULL)))
    {
        WARN("Failed to create surface, hr %#lx.\n", hr);
        return hr;
    }
    *surface = presenter->frontbuffer;
    ++*count;

    presenter->native_size = info->szNativeSize;
    presenter->aspect_ratio = info->szAspectRatio;

    return S_OK;
}

static HRESULT WINAPI surface_allocator_FreeSurface(IVMRSurfaceAllocator *iface, DWORD_PTR id)
{
    struct vmr7_presenter *presenter = impl_from_IVMRSurfaceAllocator(iface);
    DDSURFACEDESC2 surface_desc = {.dwSize = sizeof(surface_desc)};

    TRACE("presenter %p, id %#Ix.\n", presenter, id);

    vmr7_presenter_release_native(presenter);
    free(presenter->native_frame);
    presenter->native_frame = NULL;
    presenter->native_frame_size = 0;

    if (presenter->frontbuffer)
    {
        IDirectDrawSurface7_Release(presenter->frontbuffer);
        presenter->frontbuffer = NULL;
    }

    return S_OK;
}

static HRESULT WINAPI surface_allocator_PrepareSurface(IVMRSurfaceAllocator *iface,
        DWORD_PTR id, IDirectDrawSurface7 *surface, DWORD flags)
{
    TRACE("iface %p, id %#Ix, surface %p, flags %#lx.\n", iface, id, surface, flags);
    return S_OK;
}

static HRESULT WINAPI surface_allocator_AdviseNotify(IVMRSurfaceAllocator *iface,
        IVMRSurfaceAllocatorNotify *notify)
{
    struct vmr7_presenter *presenter = impl_from_IVMRSurfaceAllocator(iface);
    RECT rect = {0, 0, 0, 0};

    TRACE("presenter %p, notify %p.\n", presenter, notify);

    presenter->notify = notify;
    IVMRSurfaceAllocatorNotify_SetDDrawDevice(notify, presenter->ddraw,
            MonitorFromRect(&rect, MONITOR_DEFAULTTOPRIMARY));
    return S_OK;
}

static const IVMRSurfaceAllocatorVtbl surface_allocator_vtbl =
{
    surface_allocator_QueryInterface,
    surface_allocator_AddRef,
    surface_allocator_Release,
    surface_allocator_AllocateSurface,
    surface_allocator_FreeSurface,
    surface_allocator_PrepareSurface,
    surface_allocator_AdviseNotify,
};

static struct vmr7_presenter *impl_from_IVMRWindowlessControl(IVMRWindowlessControl *iface)
{
    return CONTAINING_RECORD(iface, struct vmr7_presenter, IVMRWindowlessControl_iface);
}

static HRESULT WINAPI windowless_control_QueryInterface(IVMRWindowlessControl *iface, REFIID iid, void **out)
{
    struct vmr7_presenter *presenter = impl_from_IVMRWindowlessControl(iface);

    return IVMRImagePresenter_QueryInterface(&presenter->IVMRImagePresenter_iface, iid, out);
}

static ULONG WINAPI windowless_control_AddRef(IVMRWindowlessControl *iface)
{
    struct vmr7_presenter *presenter = impl_from_IVMRWindowlessControl(iface);

    return IVMRImagePresenter_AddRef(&presenter->IVMRImagePresenter_iface);
}

static ULONG WINAPI windowless_control_Release(IVMRWindowlessControl *iface)
{
    struct vmr7_presenter *presenter = impl_from_IVMRWindowlessControl(iface);

    return IVMRImagePresenter_Release(&presenter->IVMRImagePresenter_iface);
}

static HRESULT WINAPI windowless_control_GetNativeVideoSize(IVMRWindowlessControl *iface,
        LONG *width, LONG *height, LONG *aspect_width, LONG *aspect_height)
{
    struct vmr7_presenter *presenter = impl_from_IVMRWindowlessControl(iface);

    TRACE("iface %p, width %p, height %p, aspect_width %p, aspect_height %p.\n",
            iface, width, height, aspect_width, aspect_height);

    if (width)
        *width = presenter->native_size.cx;
    if (height)
        *height = presenter->native_size.cy;
    if (aspect_width)
        *aspect_width = presenter->aspect_ratio.cx;
    if (aspect_height)
        *aspect_height = presenter->aspect_ratio.cy;

    TRACE("Returning size (%ld, %ld), aspect ratio (%ld, %ld).\n",
            presenter->native_size.cx, presenter->native_size.cy,
            presenter->aspect_ratio.cx, presenter->aspect_ratio.cy);
    return S_OK;
}

static HRESULT WINAPI windowless_control_GetMinIdealVideoSize(
        IVMRWindowlessControl *iface, LONG *width, LONG *height)
{
    FIXME("iface %p, width %p, height %p, stub!\n", iface, width, height);
    return E_NOTIMPL;
}

static HRESULT WINAPI windowless_control_GetMaxIdealVideoSize(
        IVMRWindowlessControl *iface, LONG *width, LONG *height)
{
    FIXME("iface %p, width %p, height %p, stub!\n", iface, width, height);
    return E_NOTIMPL;
}

static HRESULT WINAPI windowless_control_SetVideoPosition(
        IVMRWindowlessControl *iface, const RECT *source, const RECT *dest)
{
    FIXME("iface %p, source %s, dest %s, stub!.\n", iface, wine_dbgstr_rect(source), wine_dbgstr_rect(dest));
    return E_NOTIMPL;
}

static HRESULT WINAPI windowless_control_GetVideoPosition(
        IVMRWindowlessControl *iface, RECT *source, RECT *dest)
{
    FIXME("iface %p, source %p, dest %p.\n", iface, source, dest);
    return E_NOTIMPL;
}

static HRESULT WINAPI windowless_control_GetAspectRatioMode(
        IVMRWindowlessControl *iface, DWORD *mode)
{
    FIXME("iface %p, mode %p, stub!\n", iface, mode);
    return E_NOTIMPL;
}

static HRESULT WINAPI windowless_control_SetAspectRatioMode(
        IVMRWindowlessControl *iface, DWORD mode)
{
    FIXME("iface %p, mode %#lx, stub!\n", iface, mode);
    return E_NOTIMPL;
}

static HRESULT WINAPI windowless_control_SetVideoClippingWindow(
        IVMRWindowlessControl *iface, HWND window)
{
    struct vmr7_presenter *presenter = impl_from_IVMRWindowlessControl(iface);
    IDirectDrawClipper *clipper;
    HRESULT hr;

    TRACE("iface %p, window %p.\n", iface, window);

    if (GetModuleHandleW(L"winemac.drv"))
    {
        if (window != presenter->window)
            vmr7_presenter_release_native(presenter);
        presenter->window = window;
        return S_OK;
    }

    if (FAILED(hr = IDirectDraw7_CreateClipper(presenter->ddraw, 0, &clipper, NULL)))
        ERR("Failed to create clipper, hr %#lx.\n", hr);
    if (FAILED(hr = IDirectDrawClipper_SetHWnd(clipper, 0, window)))
        ERR("Failed to set clip window, hr %#lx.\n", hr);
    if (FAILED(hr = IDirectDrawSurface7_SetClipper(presenter->primary, clipper)))
        ERR("Failed to set clipper, hr %#lx.\n", hr);
    IDirectDrawClipper_Release(clipper);

    presenter->window = window;

    return S_OK;
}

static HRESULT WINAPI windowless_control_RepaintVideo(
        IVMRWindowlessControl *iface, HWND window, HDC dc)
{
    FIXME("iface %p, window %p, dc %p, stub!\n", iface, window, dc);
    return E_NOTIMPL;
}

static HRESULT WINAPI windowless_control_DisplayModeChanged(IVMRWindowlessControl *iface)
{
    FIXME("iface %p, stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI windowless_control_GetCurrentImage(IVMRWindowlessControl *iface, BYTE **image)
{
    FIXME("iface %p, image %p, stub!\n", iface, image);
    return E_NOTIMPL;
}

static HRESULT WINAPI windowless_control_SetBorderColor(IVMRWindowlessControl *iface, COLORREF color)
{
    FIXME("iface %p, color %#08lx, stub!\n", iface, color);
    return E_NOTIMPL;
}

static HRESULT WINAPI windowless_control_GetBorderColor(IVMRWindowlessControl *iface, COLORREF *color)
{
    FIXME("iface %p, color %p, stub!\n", iface, color);
    return E_NOTIMPL;
}

static HRESULT WINAPI windowless_control_SetColorKey(IVMRWindowlessControl *iface, COLORREF color)
{
    FIXME("iface %p, color %#08lx, stub!\n", iface, color);
    return E_NOTIMPL;
}

static HRESULT WINAPI windowless_control_GetColorKey(IVMRWindowlessControl *iface, COLORREF *color)
{
    FIXME("iface %p, color %p, stub!\n", iface, color);
    return E_NOTIMPL;
}

static const IVMRWindowlessControlVtbl windowless_control_vtbl =
{
    windowless_control_QueryInterface,
    windowless_control_AddRef,
    windowless_control_Release,
    windowless_control_GetNativeVideoSize,
    windowless_control_GetMinIdealVideoSize,
    windowless_control_GetMaxIdealVideoSize,
    windowless_control_SetVideoPosition,
    windowless_control_GetVideoPosition,
    windowless_control_GetAspectRatioMode,
    windowless_control_SetAspectRatioMode,
    windowless_control_SetVideoClippingWindow,
    windowless_control_RepaintVideo,
    windowless_control_DisplayModeChanged,
    windowless_control_GetCurrentImage,
    windowless_control_SetBorderColor,
    windowless_control_GetBorderColor,
    windowless_control_SetColorKey,
    windowless_control_GetColorKey,
};

HRESULT vmr7_presenter_create(IUnknown *outer, IUnknown **out)
{
    struct vmr7_presenter *object;
    HRESULT hr;

    DDSURFACEDESC2 primary_desc =
    {
        .dwSize = sizeof(DDSURFACEDESC2),
        .dwFlags = DDSD_CAPS,
        .ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE,
    };

    TRACE("outer %p, out %p.\n", outer, out);

    if (outer)
        FIXME("Ignoring outer %p.\n", outer);

    if (!(object = calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;
    object->IVMRImagePresenter_iface.lpVtbl = &image_presenter_vtbl;
    object->IVMRSurfaceAllocator_iface.lpVtbl = &surface_allocator_vtbl;
    object->IVMRWindowlessControl_iface.lpVtbl = &windowless_control_vtbl;
    object->refcount = 1;

    if (FAILED(hr = DirectDrawCreateEx(GetModuleHandleW(L"winemac.drv") ? (GUID *)DDCREATE_EMULATIONONLY : NULL,
            (void **)&object->ddraw, &IID_IDirectDraw7, NULL)))
    {
        ERR("Failed to create ddraw object, hr %#lx.\n", hr);
        free(object);
        return hr;
    }

    if (FAILED(hr = IDirectDraw7_SetCooperativeLevel(object->ddraw, NULL, DDSCL_NORMAL)))
        ERR("Failed to set cooperative level, hr %#lx.\n", hr);

    if (GetModuleHandleW(L"winemac.drv"))
    {
        primary_desc.dwFlags |= DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        primary_desc.dwWidth = primary_desc.dwHeight = 1;
        primary_desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
        primary_desc.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
        primary_desc.ddpfPixelFormat.dwFlags = DDPF_RGB;
        primary_desc.ddpfPixelFormat.dwRGBBitCount = 32;
        primary_desc.ddpfPixelFormat.dwRBitMask = 0x00ff0000;
        primary_desc.ddpfPixelFormat.dwGBitMask = 0x0000ff00;
        primary_desc.ddpfPixelFormat.dwBBitMask = 0x000000ff;
    }

    if (FAILED(hr = IDirectDraw7_CreateSurface(object->ddraw, &primary_desc, &object->primary, NULL)))
        ERR("Failed to create primary surface, hr %#lx.\n", hr);

    TRACE("Created VMR7 default presenter %p.\n", object);
    *out = (IUnknown *)&object->IVMRSurfaceAllocator_iface;
    return S_OK;
}
