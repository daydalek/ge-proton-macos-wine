/*
 * Optional MetalFX Spatial presentation for opaque GDI surfaces.
 * Copyright 2026 GE-Proton macOS Wine PoC contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>

/* Instances belong to one WineContentView and are used on the main thread.
 * copyScaledImage returns an immutable, owned image or NULL for fallback. */
@interface WineSpatialScaler : NSObject
{
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id scaler;
    id<MTLTexture> inputTexture, outputTexture;
    id<MTLBuffer> upload, readback;
    CGContextRef bitmap;
    NSUInteger inputWidth, inputHeight, outputWidth, outputHeight;
    NSUInteger uploadStride, readbackStride;
    BOOL failed;
}
+ (BOOL) enabled;
- (CGImageRef) copyScaledImage:(CGImageRef)image width:(NSUInteger)width height:(NSUInteger)height;
@end
