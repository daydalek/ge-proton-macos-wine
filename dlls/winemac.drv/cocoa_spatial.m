/* GE macOS Wine PoC modification notice: 2026-11-24; GE media integration and macOS adaptations. Original copyright and license retained. See MODIFICATIONS.json. */
/*
 * Optional MetalFX Spatial presentation for opaque GDI surfaces.
 * Copyright 2026 GE-Proton macOS Wine PoC contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#import "cocoa_spatial.h"
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

#if __has_include(<MetalFX/MetalFX.h>)
#import <MetalFX/MetalFX.h>
#define HAVE_METALFX_SPATIAL 1
#endif

/* Keep MetalFX optional at load time so the Wine driver retains its macOS 11
 * deployment target. The descriptor is resolved after the runtime OS check. */
static Class spatial_descriptor_class(void)
{
    static Class descriptor;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
#ifdef HAVE_METALFX_SPATIAL
        if (@available(macOS 13.0, *))
        {
            if (dlopen("/System/Library/Frameworks/MetalFX.framework/MetalFX", RTLD_LAZY | RTLD_LOCAL))
                descriptor = NSClassFromString(@"MTLFXSpatialScalerDescriptor");
        }
#endif
    });
    return descriptor;
}

static void release_image_bytes(void *info, const void *data, size_t size)
{
    (void)info;
    (void)size;
    free((void *)data);
}

@implementation WineSpatialScaler

+ (BOOL) enabled
{
    static BOOL enabled;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *mode = getenv("WINE_METALFX");
        if (mode && !strcmp(mode, "spatial"))
        {
            enabled = !!spatial_descriptor_class();
            if (!enabled) NSLog(@"Wine MetalFX: Spatial unavailable; using standard GDI presentation");
        }
    });
    return enabled;
}

- (id) init
{
    self = [super init];
    if (!self) return nil;
#ifdef HAVE_METALFX_SPATIAL
    if (@available(macOS 13.0, *))
    {
        device = MTLCreateSystemDefaultDevice();
        if (device && [spatial_descriptor_class() supportsDevice:device])
            queue = [device newCommandQueue];
    }
#endif
    if (!queue)
    {
        NSLog(@"Wine MetalFX: GPU unavailable; using standard GDI presentation");
        [self release];
        return nil;
    }
    return self;
}

- (void) clearResources
{
    CGContextRelease(bitmap);
    bitmap = NULL;
    [scaler release];
    scaler = nil;
    [inputTexture release];
    inputTexture = nil;
    [outputTexture release];
    outputTexture = nil;
    [upload release];
    upload = nil;
    [readback release];
    readback = nil;
}

- (void) dealloc
{
    [self clearResources];
    [queue release];
    [device release];
    [super dealloc];
}

- (BOOL) configureInputWidth:(NSUInteger)iw height:(NSUInteger)ih
                outputWidth:(NSUInteger)ow height:(NSUInteger)oh
{
    [self clearResources];
#ifdef HAVE_METALFX_SPATIAL
    if (@available(macOS 13.0, *))
    {
        MTLFXSpatialScalerDescriptor *descriptor = [[spatial_descriptor_class() alloc] init];
        descriptor.inputWidth = iw;
        descriptor.inputHeight = ih;
        descriptor.outputWidth = ow;
        descriptor.outputHeight = oh;
        /* GDI CGImages carry sRGB encoded values. Unorm plus Perceptual avoids
         * an extra sRGB texture decode before MetalFX's color processing. */
        descriptor.colorTextureFormat = MTLPixelFormatBGRA8Unorm;
        descriptor.outputTextureFormat = MTLPixelFormatBGRA8Unorm;
        descriptor.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
        scaler = [descriptor newSpatialScalerWithDevice:device];
        [descriptor release];
        if (!scaler) return NO;

        MTLTextureDescriptor *texture = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
            width:iw height:ih mipmapped:NO];
        texture.storageMode = MTLStorageModePrivate;
        texture.usage = [scaler colorTextureUsage];
        inputTexture = [device newTextureWithDescriptor:texture];
        texture.width = ow;
        texture.height = oh;
        texture.usage = [scaler outputTextureUsage];
        outputTexture = [device newTextureWithDescriptor:texture];

        uploadStride = (iw * 4 + 255) & ~(NSUInteger)255;
        readbackStride = (ow * 4 + 255) & ~(NSUInteger)255;
        upload = [device newBufferWithLength:uploadStride * ih options:MTLResourceStorageModeShared];
        readback = [device newBufferWithLength:readbackStride * oh options:MTLResourceStorageModeShared];
        if (!inputTexture || !outputTexture || !upload || !readback) return NO;

        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        bitmap = CGBitmapContextCreate(upload.contents, iw, ih, 8, uploadStride, space,
                                      kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst);
        CGColorSpaceRelease(space);
        if (!bitmap) return NO;
        CGContextSetBlendMode(bitmap, kCGBlendModeCopy);
        inputWidth = iw;
        inputHeight = ih;
        outputWidth = ow;
        outputHeight = oh;
        NSLog(@"Wine MetalFX: GDI Spatial %lux%lu -> %lux%lu on %@",
              (unsigned long)iw, (unsigned long)ih, (unsigned long)ow, (unsigned long)oh, device.name);
        return YES;
    }
#endif
    return NO;
}

- (CGImageRef) copyScaledImage:(CGImageRef)image width:(NSUInteger)width height:(NSUInteger)height
{
    if (failed || !image) return NULL;
    NSUInteger iw = CGImageGetWidth(image), ih = CGImageGetHeight(image);
    /* Bound memory and arithmetic before allocating. A 1:1 or shrinking axis
     * uses standard presentation; this path only performs genuine upscaling. */
    if (!iw || !ih || width <= iw || height <= ih || width > 8192 || height > 8192 ||
        width * height > 32 * 1024 * 1024) return NULL;

    if (iw != inputWidth || ih != inputHeight || width != outputWidth || height != outputHeight)
    {
        if (![self configureInputWidth:iw height:ih outputWidth:width height:height])
        {
            failed = YES;
            [self clearResources];
            NSLog(@"Wine MetalFX: allocation failed; using standard GDI presentation");
            return NULL;
        }
    }

#ifdef HAVE_METALFX_SPATIAL
    if (@available(macOS 13.0, *))
    {
        CGContextDrawImage(bitmap, CGRectMake(0, 0, iw, ih), image);
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        if (!commands || !blit) return NULL;
        [blit copyFromBuffer:upload sourceOffset:0 sourceBytesPerRow:uploadStride
              sourceBytesPerImage:uploadStride * ih sourceSize:MTLSizeMake(iw, ih, 1)
              toTexture:inputTexture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        [scaler setColorTexture:inputTexture];
        [scaler setOutputTexture:outputTexture];
        [scaler setInputContentWidth:iw];
        [scaler setInputContentHeight:ih];
        [scaler encodeToCommandBuffer:commands];
        blit = [commands blitCommandEncoder];
        if (!blit) return NULL;
        [blit copyFromTexture:outputTexture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
              sourceSize:MTLSizeMake(width, height, 1) toBuffer:readback destinationOffset:0
              destinationBytesPerRow:readbackStride destinationBytesPerImage:readbackStride * height];
        [blit endEncoding];
        /* Reuse textures only after completion, and give Core Animation its own
         * immutable copy. This intentionally favors correctness for GDI games. */
        [commands commit];
        [commands waitUntilCompleted];
        if (commands.status != MTLCommandBufferStatusCompleted)
        {
            failed = YES;
            NSLog(@"Wine MetalFX: GPU failure %@; using standard GDI presentation", commands.error);
            return NULL;
        }

        size_t size = readbackStride * height;
        void *bytes = malloc(size);
        if (!bytes) return NULL;
        memcpy(bytes, readback.contents, size);
        CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, bytes, size, release_image_bytes);
        if (!provider) { free(bytes); return NULL; }
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGImageRef result = CGImageCreate(width, height, 8, 32, readbackStride, space,
            kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst, provider, NULL, FALSE, kCGRenderingIntentDefault);
        CGColorSpaceRelease(space);
        CGDataProviderRelease(provider);
        return result;
    }
#endif
    return NULL;
}
@end
