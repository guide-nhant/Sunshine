/**
 * @file src/platform/macos/sc_video.h
 * @brief Declarations for ScreenCaptureKit-based video capture on macOS.
 */
#pragma once

// platform includes
#import <AppKit/AppKit.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

// local includes
#import "src/platform/macos/av_video.h"  // FrameCallbackBlock

@class SCVideo;

/**
 * One active capture. Each call to [SCVideo capture:] creates an
 * independent SCStream, so concurrent captures (encoder dummy-frame
 * seeding + the streaming capture thread) never share mutable state —
 * mirroring AVVideo's per-connection NSMapTable design.
 */
@interface SCVideoStream: NSObject <SCStreamOutput, SCStreamDelegate>

@property (nonatomic, retain) SCStream *stream;
@property (nonatomic, copy) FrameCallbackBlock frameCallback;
@property (nonatomic, retain) dispatch_semaphore_t captureStopped;
@property (nonatomic, assign) SCVideo *owner;
/// Set when the stream terminated on its own (start failure or
/// didStopWithError) rather than through a callback-requested stop.
@property (assign) BOOL streamError;

- (void)stopWithError:(BOOL)isError;

@end

@interface SCVideo: NSObject

@property (nonatomic, assign) CGDirectDisplayID displayID;
@property (nonatomic, assign) CMTime minFrameDuration;
@property (nonatomic, assign) OSType pixelFormat;
@property (nonatomic, assign) int frameWidth;
@property (nonatomic, assign) int frameHeight;
@property (nonatomic, assign) int displayPixelWidth;
@property (nonatomic, assign) int displayPixelHeight;

@property (nonatomic, retain) SCDisplay *scDisplay;
@property (nonatomic, retain) SCContentFilter *contentFilter;
@property (nonatomic, retain) dispatch_queue_t sampleQueue;
@property (nonatomic, retain) NSMutableArray<SCVideoStream *> *activeStreams;

- (id)initWithDisplay:(CGDirectDisplayID)displayID frameRate:(int)frameRate;

- (void)setFrameWidth:(int)frameWidth frameHeight:(int)frameHeight;

/// Starts an independent SCStream. Returns a +1 retained SCVideoStream
/// (caller must release) or nil on setup failure. Wait on its
/// captureStopped semaphore; then check streamError.
- (SCVideoStream *)capture:(FrameCallbackBlock)frameCallback;

- (void)removeStream:(SCVideoStream *)stream;
- (void)shutdown;

@end
