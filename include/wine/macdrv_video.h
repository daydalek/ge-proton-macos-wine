/*
 * Private ExtEscape protocol for native macOS video presentation
 *
 * Shared between the PE side (dlls/quartz VMR7 presenter, which produces
 * frames) and the Unix side of winemac.drv (which displays them in a
 * native child window above OpenGL/Metal surfaces).  The header is plain
 * C so it can also be compiled standalone by headless tests.
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

#ifndef __WINE_WINE_MACDRV_VIDEO_H
#define __WINE_WINE_MACDRV_VIDEO_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Private ExtEscape() escape code, fits a signed INT.  ASCII "WMVR". */
#define MACDRV_VIDEO_ESCAPE_ID 0x574d5652

#define MACDRV_VIDEO_PROTOCOL_VERSION 1

enum macdrv_video_command
{
    MACDRV_VIDEO_CMD_PRESENT = 1,
    MACDRV_VIDEO_CMD_RELEASE = 2,
};

/* Fixed-layout, pointer-free wire header.  For PRESENT it is immediately
 * followed by stride*height bytes of tightly packed top-down BGRX rows.
 * For RELEASE the buffer contains only this header. */
struct macdrv_video_header
{
    uint32_t version;   /* MACDRV_VIDEO_PROTOCOL_VERSION */
    uint32_t command;   /* enum macdrv_video_command */
    uint64_t token;     /* opaque producer identity, never dereferenced */
    uint32_t width;     /* payload width in pixels */
    uint32_t height;    /* payload height in pixels */
    uint32_t stride;    /* payload bytes per row */
    uint32_t reserved;  /* must be zero */
};

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(struct macdrv_video_header) == 32, "header must be exactly 32 bytes");
_Static_assert(offsetof(struct macdrv_video_header, token) == 8, "token must be at offset 8");
#else
typedef char macdrv_video_header_size_assert[(sizeof(struct macdrv_video_header) == 32) ? 1 : -1];
typedef char macdrv_video_header_token_assert[(offsetof(struct macdrv_video_header, token) == 8) ? 1 : -1];
#endif

/* Computes the pixel payload size for a PRESENT message, or returns false
 * when the geometry is invalid or the total message size would not fit an
 * ExtEscape() INT count. */
static inline bool macdrv_video_present_payload_size(uint32_t width, uint32_t height,
        uint32_t stride, uint64_t *payload_size)
{
    uint64_t row, payload;

    if (!width || !height) return false;
    row = (uint64_t)width * 4;
    if (row > UINT32_MAX || stride < row) return false;
    payload = (uint64_t)stride * height;
    if (payload + sizeof(struct macdrv_video_header) > (uint64_t)INT_MAX) return false;
#if SIZE_MAX < UINT64_MAX
    if (payload + sizeof(struct macdrv_video_header) > (uint64_t)SIZE_MAX) return false;
#endif
    if (payload_size) *payload_size = payload;
    return true;
}

/* Validates a complete escape input buffer.  Returns MACDRV_VIDEO_CMD_PRESENT
 * or MACDRV_VIDEO_CMD_RELEASE on success, 0 on malformed input.  For a valid
 * PRESENT, *payload and *payload_size locate the pixel data in the buffer. */
static inline uint32_t macdrv_video_message_parse(const void *data, uint32_t count,
        const void **payload, uint64_t *payload_size)
{
    const struct macdrv_video_header *header = data;
    uint64_t size;

    if (!data || count < sizeof(*header)) return 0;
    if (header->version != MACDRV_VIDEO_PROTOCOL_VERSION) return 0;
    if (header->reserved) return 0;

    switch (header->command)
    {
    case MACDRV_VIDEO_CMD_PRESENT:
        if (!macdrv_video_present_payload_size(header->width, header->height,
                                             header->stride, &size)) return 0;
        if ((uint64_t)count < sizeof(*header) + size) return 0;
        if (payload) *payload = header + 1;
        if (payload_size) *payload_size = size;
        return MACDRV_VIDEO_CMD_PRESENT;
    case MACDRV_VIDEO_CMD_RELEASE:
        if (count != sizeof(*header)) return 0;
        return MACDRV_VIDEO_CMD_RELEASE;
    default:
        return 0;
    }
}

/* Validates a signed DirectDraw lPitch and copies the frame into a tightly
 * packed top-down destination buffer of width*4 bytes per row (height*width*4
 * bytes total).  src is the base pointer reported by Lock(); successive
 * scanlines are src + i*src_pitch, so a negative pitch walks to lower
 * addresses.  Returns false without copying when the arguments are invalid. */
static inline bool macdrv_video_copy_rows(void *dst, const void *src,
        int32_t src_pitch, uint32_t width, uint32_t height)
{
    uint64_t row = (uint64_t)width * 4;
    uint64_t abs_pitch;
    uint32_t i;

    if (!dst || !src || !width || !height) return false;
    if (row > UINT32_MAX) return false;
    if (src_pitch == 0 || src_pitch == INT32_MIN) return false;
    abs_pitch = (uint64_t)(src_pitch < 0 ? -(int64_t)src_pitch : src_pitch);
    if (abs_pitch < row) return false;
    /* Bound the tightly packed destination like a PRESENT payload and the
     * signed source span for this platform's pointer range (PE32 too). */
    if (!macdrv_video_present_payload_size(width, height, (uint32_t)row, NULL)) return false;
    if ((uint64_t)(height - 1) * abs_pitch > (uint64_t)PTRDIFF_MAX) return false;

    for (i = 0; i < height; ++i)
        memcpy((char *)dst + (uint64_t)i * row,
               (const char *)src + (int64_t)i * src_pitch, (size_t)row);
    return true;
}

#endif  /* __WINE_WINE_MACDRV_VIDEO_H */
