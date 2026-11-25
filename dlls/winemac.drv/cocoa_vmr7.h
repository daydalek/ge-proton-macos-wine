/*
 * VMR7 native video presentation surfaces
 *
 * Implements macdrv_window_present_video() / macdrv_window_clear_video().
 * A private ExtEscape from the quartz VMR7 presenter hands BGRX frames to
 * the graphics driver; each presenter token gets a borderless child
 * NSWindow of the top-level WineWindow, composited above the window's
 * in-front OpenGL surfaces and Metal content where ordinary GDI layer
 * updates never appear.  Included exactly once at the end of
 * cocoa_window.m, so it can see the WineWindow class extension.
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

#ifndef __WINE_MACDRV_COCOA_VMR7_H
#define __WINE_MACDRV_COCOA_VMR7_H

/* AppKit/QuartzCore come from the including cocoa_window.m. */

@interface WineVMR7VideoWindow : NSWindow
@end

@implementation WineVMR7VideoWindow
    - (BOOL) canBecomeKeyWindow { return NO; }
    - (BOOL) canBecomeMainWindow { return NO; }
@end


@interface WineVMR7VideoSurface : NSObject
{
    WineWindow* parent;         /* not retained; the parent owns us */
    NSWindow* child;
    NSView* content;            /* not retained; owned by child */
    CGImageRef image;           /* latest frame, retained */
    CGRect target;              /* in parent contentView coordinates */
    NSRect appliedFrame;        /* last applied screen frame */
    uint64_t token;
    BOOL ordered;               /* child is currently ordered in */
    BOOL updatingLayout;        /* re-entrancy guard */
}

    - (id) initWithParent:(WineWindow*)parentWindow token:(uint64_t)presenterToken;
    - (void) setImage:(CGImageRef)frame target:(CGRect)rect;
    - (void) updateLayout;
    - (void) detach;

@end

@implementation WineVMR7VideoSurface

    - (id) initWithParent:(WineWindow*)parentWindow token:(uint64_t)presenterToken
    {
        self = [super init];
        if (self)
        {
            parent = parentWindow;
            token = presenterToken;
        }
        return self;
    }

    - (void) dealloc
    {
        [self detach];
        [super dealloc];
    }

    - (void) detach
    {
        if (child)
        {
            if ([child parentWindow] == parent)
                [parent removeChildWindow:child];
            [child orderOut:nil];
            [child release];
            child = nil;
            content = nil;
            ordered = NO;
            appliedFrame = NSZeroRect;
        }
        if (image)
        {
            CGImageRelease(image);
            image = nil;
        }
        parent = nil;
    }

    - (void) setImage:(CGImageRef)frame target:(CGRect)rect
    {
        if (frame != image)
        {
            CGImageRetain(frame);
            CGImageRelease(image);
            image = frame;
        }
        target = rect;
        [self updateLayout];
    }

    - (void) createChild
    {
        NSRect frame = NSMakeRect(0, 0, MAX(1, target.size.width), MAX(1, target.size.height));

        child = [[WineVMR7VideoWindow alloc] initWithContentRect:frame
                                                       styleMask:NSWindowStyleMaskBorderless
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        [child setReleasedWhenClosed:NO];
        [child setOpaque:YES];
        [child setBackgroundColor:[NSColor blackColor]];
        [child setHasShadow:NO];
        [child setIgnoresMouseEvents:YES];
        child.collectionBehavior = (child.collectionBehavior & ~NSWindowCollectionBehaviorFullScreenPrimary)
                | NSWindowCollectionBehaviorFullScreenAuxiliary
                | NSWindowCollectionBehaviorIgnoresCycle;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [child setExcludedFromWindowsMenu:YES];
#pragma clang diagnostic pop

        content = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, frame.size.width, frame.size.height)];
        [content setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        [content setWantsLayer:YES];
        [content setLayerContentsRedrawPolicy:NSViewLayerContentsRedrawNever];
        content.layer.actions = @{ @"contents": [NSNull null], @"bounds": [NSNull null],
                @"position": [NSNull null], @"frame": [NSNull null] };
        content.layer.contentsGravity = kCAGravityResize;
        content.layer.opaque = YES;
        [child setContentView:content];
        [content release];   /* child owns it now; content is a weak alias */
    }

    - (void) updateLayout
    {
        /* Ordering or resizing the child can synchronously deliver parent
         * notifications that call us back; drop the nested update. */
        if (updatingLayout || !parent) return;
        updatingLayout = YES;
        @try
        {
            [self performLayout];
        }
        @finally
        {
            updatingLayout = NO;
        }
    }

    - (void) performLayout
    {
        NSView* parentView = [parent contentView];
        CGRect bounds, visible;

        if (!image || !parentView || parent.closing ||
                ![parent isVisible] || [parent isMiniaturized])
        {
            if (ordered) { [child orderOut:nil]; ordered = NO; }
            return;
        }

        bounds = NSRectToCGRect([parentView bounds]);
        visible = CGRectIntersection(target, bounds);
        if (CGRectIsEmpty(visible) || CGRectIsNull(visible))
        {
            if (ordered) { [child orderOut:nil]; ordered = NO; }
            return;
        }

        if (!child) [self createChild];
        if (!child) return;

        /* Crop the frame to the part inside the parent content bounds; the
         * cropped pixels cover only the still-visible part of the target. */
        {
            CGFloat sx = (CGFloat)CGImageGetWidth(image) / target.size.width;
            CGFloat sy = (CGFloat)CGImageGetHeight(image) / target.size.height;
            CGRect imageBounds = CGRectMake(0, 0, CGImageGetWidth(image), CGImageGetHeight(image));
            CGRect crop = CGRectIntersection(imageBounds, CGRectIntegral(CGRectMake(
                    (CGRectGetMinX(visible) - CGRectGetMinX(target)) * sx,
                    (CGRectGetMinY(visible) - CGRectGetMinY(target)) * sy,
                    CGRectGetWidth(visible) * sx, CGRectGetHeight(visible) * sy)));
            CGImageRef cropped = CGRectEqualToRect(crop, imageBounds) ? NULL
                    : CGImageCreateWithImageInRect(image, crop);
            NSRect winRect, screenRect;

            content.layer.contentsScale = [child backingScaleFactor];
            content.layer.contents = (id)(cropped ? cropped : image);
            if (cropped) CGImageRelease(cropped);

            /* The parent content view is flipped; go through window/screen
             * coordinates rather than flipping Y by hand. */
            winRect = [parentView convertRect:NSRectFromCGRect(visible) toView:nil];
            screenRect = [parent convertRectToScreen:winRect];
            if ([child level] != [parent level])
                [child setLevel:[parent level]];
            if (!NSEqualRects(screenRect, appliedFrame))
                [child setFrame:screenRect display:NO];
            appliedFrame = screenRect;
        }

        /* Order above this parent (and its in-front GL/Metal content) when
         * first attaching or reappearing, not on every frame. */
        if (!ordered)
        {
            if ([child parentWindow] != parent)
                [parent addChildWindow:child ordered:NSWindowAbove];
            else
                [child orderWindow:NSWindowAbove relativeTo:[parent windowNumber]];
            ordered = YES;
        }
    }

@end


@implementation WineWindow (WineVMR7VideoSurfaces)

    - (BOOL) wine_presentVideoWithToken:(uint64_t)token image:(CGImageRef)image target:(CGRect)rect
    {
        NSNumber* key = [NSNumber numberWithUnsignedLongLong:token];
        WineVMR7VideoSurface* surface;

        if (!image || CGRectIsEmpty(rect)) return NO;
        if (!videoSurfaces)
            videoSurfaces = [[NSMutableDictionary alloc] init];
        if (!(surface = [videoSurfaces objectForKey:key]) && self.closing) return NO;
        if (!surface)
        {
            surface = [[WineVMR7VideoSurface alloc] initWithParent:self token:token];
            if (!surface) return NO;
            [videoSurfaces setObject:surface forKey:key];
            [surface release];
            NSLog(@"Wine VMR7 native video surface: token %llu, image %zux%zu, target %@",
                    (unsigned long long)token, CGImageGetWidth(image), CGImageGetHeight(image),
                    NSStringFromRect(NSRectFromCGRect(rect)));
        }
        [surface setImage:image target:rect];
        return YES;
    }

    - (void) wine_clearVideoWithToken:(uint64_t)token
    {
        WineVMR7VideoSurface* surface;

        if (!videoSurfaces) return;
        /* The dictionary owns the surface: keep it alive across removal so
         * the entry is gone before detach runs. */
        if ((surface = [[videoSurfaces objectForKey:[NSNumber numberWithUnsignedLongLong:token]] retain]))
        {
            NSLog(@"Wine VMR7 native video surface released: token %llu",
                    (unsigned long long)token);
            [videoSurfaces removeObjectForKey:[NSNumber numberWithUnsignedLongLong:token]];
            [surface detach];
            [surface release];
        }
    }

    - (void) wine_updateVideoSurfaces
    {
        NSArray* surfaces;

        if (!videoSurfaces || !videoSurfaces.count) return;
        surfaces = [videoSurfaces allValues];
        for (WineVMR7VideoSurface* surface in surfaces)
            [surface updateLayout];
    }

    - (void) wine_clearVideoSurfaces
    {
        NSArray* surfaces;

        if (!videoSurfaces) return;
        surfaces = [videoSurfaces allValues];
        [videoSurfaces removeAllObjects];
        for (WineVMR7VideoSurface* surface in surfaces)
            [surface detach];
    }

@end


/* Display a video frame for a presenter token inside the given rect
 * (top-down Wine coordinates relative to the window's content area).
 * Runs synchronously on the main thread. */
bool macdrv_window_present_video(macdrv_window w, uint64_t token, CGImageRef image, CGRect rect)
{
    __block BOOL presented = FALSE;

    OnMainThread(^{
        @autoreleasepool
        {
            WineWindow* window = (WineWindow*)w;
            if ([window isKindOfClass:[WineWindow class]])
                presented = [window wine_presentVideoWithToken:token image:image
                        target:cgrect_mac_from_win(rect)];
        }
    });
    return presented;
}

/* Remove the native video surface for the given presenter token, if any. */
void macdrv_window_clear_video(macdrv_window w, uint64_t token)
{
    OnMainThread(^{
        @autoreleasepool
        {
            WineWindow* window = (WineWindow*)w;
            if ([window isKindOfClass:[WineWindow class]])
                [window wine_clearVideoWithToken:token];
        }
    });
}

#endif  /* __WINE_MACDRV_COCOA_VMR7_H */
