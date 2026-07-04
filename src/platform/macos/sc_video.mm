/**
 * @file src/platform/macos/sc_video.mm
 * @brief Definitions for ScreenCaptureKit-based video capture on macOS.
 */
// local includes
#import "sc_video.h"

#include "src/logging.h"

using namespace std::literals;

@implementation SCVideoStream {
  BOOL _stopped;
}

- (id)init {
  self = [super init];
  _stopped = NO;
  self.streamError = NO;
  return self;
}

- (void)dealloc {
  self.stream = nil;
  self.frameCallback = nil;
  self.captureStopped = nil;
  [super dealloc];
}

- (void)stopWithError:(BOOL)isError {
  @synchronized(self) {
    if (_stopped) {
      return;
    }
    _stopped = YES;

    if (isError) {
      self.streamError = YES;
    }

    self.frameCallback = nil;

    SCStream *stream = [self.stream retain];
    self.stream = nil;
    if (stream != nil) {
      [stream stopCaptureWithCompletionHandler:^(NSError *err) {
        // "Already stopped" is fine here; retain keeps the stream alive
        // until the async stop settles.
        [stream release];
      }];
    }

    [self.owner removeStream:self];
    dispatch_semaphore_signal(self.captureStopped);
  }
}

- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type {
  if (type != SCStreamOutputTypeScreen) {
    return;
  }

  FrameCallbackBlock callback;
  @synchronized(self) {
    if (_stopped || self.frameCallback == nil) {
      return;
    }
    callback = [[self.frameCallback retain] autorelease];
  }

  if (!CMSampleBufferIsValid(sampleBuffer) || CMSampleBufferGetImageBuffer(sampleBuffer) == nil) {
    return;
  }

  // SCK delivers idle/suspended notifications as sample buffers too;
  // only complete frames carry displayable content.
  NSArray *attachments = (NSArray *) CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, NO);
  NSDictionary *frame_info = attachments.firstObject;
  NSNumber *status = frame_info[SCStreamFrameInfoStatus];
  if (status != nil && status.intValue != SCFrameStatusComplete) {
    return;
  }

  if (!callback(sampleBuffer)) {
    // Consumer requested stop.
    [self stopWithError:NO];
  }
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)err {
  BOOST_LOG(error) << "ScreenCaptureKit stream stopped unexpectedly: "sv
                   << (err ? err.localizedDescription.UTF8String : "unknown");
  [self stopWithError:YES];
}

@end

@implementation SCVideo

- (id)initWithDisplay:(CGDirectDisplayID)displayID frameRate:(int)frameRate {
  self = [super init];

  self.displayID = displayID;
  self.pixelFormat = kCVPixelFormatType_32BGRA;
  self.minFrameDuration = CMTimeMake(1, frameRate);

  CGDisplayModeRef mode = CGDisplayCopyDisplayMode(displayID);
  self.frameWidth = (int) CGDisplayModeGetPixelWidth(mode);
  self.frameHeight = (int) CGDisplayModeGetPixelHeight(mode);
  self.displayPixelWidth = self.frameWidth;
  self.displayPixelHeight = self.frameHeight;
  CFRelease(mode);

  // relative_priority must be in [QOS_MIN_RELATIVE_PRIORITY, 0]; the old
  // DISPATCH_QUEUE_PRIORITY_HIGH (=2) made this attr NULL, silently dropping
  // the QoS to default.
  dispatch_queue_attr_t qos = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
  dispatch_queue_t queue = dispatch_queue_create("lumenScreenCaptureQueue", qos);
  self.sampleQueue = queue;
  [queue release];

  NSMutableArray *streams = [[NSMutableArray alloc] init];
  self.activeStreams = streams;
  [streams release];

  // SCShareableContent enumeration is async-only; block briefly so this
  // initializer keeps AVVideo's synchronous contract for display.mm.
  __block SCDisplay *matched_display = nil;
  dispatch_semaphore_t content_ready = dispatch_semaphore_create(0);

  [SCShareableContent getShareableContentExcludingDesktopWindows:NO
                                              onScreenWindowsOnly:NO
                                                completionHandler:^(SCShareableContent *content, NSError *err) {
    if (err != nil) {
      BOOST_LOG(error) << "SCShareableContent enumeration failed: "sv << err.localizedDescription.UTF8String;
    } else {
      for (SCDisplay *display in content.displays) {
        if (display.displayID == displayID) {
          matched_display = [display retain];
          break;
        }
      }
    }
    dispatch_semaphore_signal(content_ready);
  }];

  if (dispatch_semaphore_wait(content_ready, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) != 0) {
    BOOST_LOG(error) << "Timed out enumerating shareable content for ScreenCaptureKit"sv;
    [content_ready release];
    [self release];
    return nil;
  }
  [content_ready release];

  if (matched_display == nil) {
    BOOST_LOG(error) << "ScreenCaptureKit could not find display "sv << displayID;
    [self release];
    return nil;
  }

  self.scDisplay = matched_display;
  [matched_display release];

  SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:self.scDisplay excludingWindows:@[]];
  self.contentFilter = filter;
  [filter release];

  return self;
}

- (void)dealloc {
  [self shutdown];
  self.scDisplay = nil;
  self.contentFilter = nil;
  self.sampleQueue = nil;
  self.activeStreams = nil;
  [super dealloc];
}

- (void)setFrameWidth:(int)frameWidth frameHeight:(int)frameHeight {
  self.frameWidth = frameWidth;
  self.frameHeight = frameHeight;
}

- (SCStreamConfiguration *)buildConfiguration {
  SCStreamConfiguration *config = [[[SCStreamConfiguration alloc] init] autorelease];

  config.width = self.frameWidth;
  config.height = self.frameHeight;
  config.minimumFrameInterval = self.minFrameDuration;
  config.pixelFormat = self.pixelFormat;
  config.showsCursor = YES;
  config.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
  // Default queueDepth is 3; the encode pipeline retains CVPixelBuffers by
  // reference (av_img_t), so a 3-surface pool starves and drops frames at
  // 60 fps+. 8 is the documented maximum useful depth (WWDC22).
  config.queueDepth = 8;

  if (self.pixelFormat == kCVPixelFormatType_32BGRA) {
    config.colorSpaceName = kCGColorSpaceSRGB;
  } else {
    // NV12 / P010: match the BT.709 matrix the encoder negotiates and the
    // AVFoundation path produced — see US-062 (don't downgrade color).
    config.colorMatrix = kCGDisplayStreamYCbCrMatrix_ITU_R_709_2;
  }

  // Aspect-fit the display into the requested canvas (parity with the
  // AVFoundation path's AVVideoScalingModeResizeAspect). Rects are kept
  // integral and even-aligned for the 4:2:0 pixel formats.
  double scale = MIN((double) self.frameWidth / self.displayPixelWidth,
                     (double) self.frameHeight / self.displayPixelHeight);
  long dest_width = lround(self.displayPixelWidth * scale) & ~1L;
  long dest_height = lround(self.displayPixelHeight * scale) & ~1L;
  long dest_x = ((self.frameWidth - dest_width) / 2) & ~1L;
  long dest_y = ((self.frameHeight - dest_height) / 2) & ~1L;
  config.destinationRect = CGRectMake(dest_x, dest_y, dest_width, dest_height);

  return config;
}

- (SCVideoStream *)capture:(FrameCallbackBlock)frameCallback {
  SCVideoStream *capture_stream = [[SCVideoStream alloc] init];
  capture_stream.owner = self;
  capture_stream.frameCallback = frameCallback;

  dispatch_semaphore_t signal = dispatch_semaphore_create(0);
  capture_stream.captureStopped = signal;
  [signal release];

  SCStream *stream = [[SCStream alloc] initWithFilter:self.contentFilter
                                        configuration:[self buildConfiguration]
                                             delegate:capture_stream];
  capture_stream.stream = stream;
  [stream release];

  NSError *output_error = nil;
  if (![capture_stream.stream addStreamOutput:capture_stream
                                         type:SCStreamOutputTypeScreen
                           sampleHandlerQueue:self.sampleQueue
                                        error:&output_error]) {
    BOOST_LOG(error) << "SCStream addStreamOutput failed: "sv
                     << (output_error ? output_error.localizedDescription.UTF8String : "unknown");
    [capture_stream release];
    return nil;
  }

  @synchronized(self.activeStreams) {
    [self.activeStreams addObject:capture_stream];
  }

  [capture_stream.stream startCaptureWithCompletionHandler:^(NSError *err) {
    if (err != nil) {
      BOOST_LOG(error) << "SCStream startCapture failed: "sv << err.localizedDescription.UTF8String;
      [capture_stream stopWithError:YES];
    } else {
      BOOST_LOG(info) << "ScreenCaptureKit stream started ("sv << self.frameWidth << 'x' << self.frameHeight << ')';
    }
  }];

  return capture_stream;
}

- (void)removeStream:(SCVideoStream *)stream {
  @synchronized(self.activeStreams) {
    [self.activeStreams removeObject:stream];
  }
}

- (void)shutdown {
  NSArray *streams;
  @synchronized(self.activeStreams) {
    streams = [[self.activeStreams copy] autorelease];
  }
  for (SCVideoStream *stream in streams) {
    [stream stopWithError:NO];
  }
}

@end
