// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#if defined(__APPLE__) && defined(USE_SCREEN_SHARING)

#	import <AVFoundation/AVFoundation.h>
#	import <CoreVideo/CoreVideo.h>
#	import <Foundation/Foundation.h>

#	include "AVFCapture.h"

#	include <QtGui/QImage>

static constexpr int AVF_FPS = 30;

@interface AVFCaptureDelegate : NSObject< AVCaptureVideoDataOutputSampleBufferDelegate >

@property(nonatomic, strong) AVCaptureSession *session;
@property(nonatomic, strong) AVCaptureVideoDataOutput *output;
@property(nonatomic, strong) dispatch_queue_t captureQueue;
@property(nonatomic, copy) void (^onStarted)();
@property(nonatomic, copy) void (^onError)(NSString *);
@property(nonatomic, copy) void (^onFrame)(QImage);

- (void)startWithDeviceID:(NSString *)deviceID;
- (void)stop;

@end

@implementation AVFCaptureDelegate

- (void)fail:(NSString *)message {
	if (self.onError) {
		dispatch_async(dispatch_get_main_queue(), ^{
			self.onError(message);
		});
	}
}

- (void)startWithDeviceID:(NSString *)deviceID {
	AVCaptureDevice *device = [AVCaptureDevice deviceWithUniqueID:deviceID];
	if (!device) {
		[self fail:@"The selected camera is no longer available."];
		return;
	}

	NSError *inputError = nil;
	AVCaptureDeviceInput *input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&inputError];
	if (!input) {
		NSString *message = inputError ? inputError.localizedDescription : @"The selected camera could not be opened.";
		[self fail:message];
		return;
	}

	AVCaptureSession *session = [[AVCaptureSession alloc] init];
	[session beginConfiguration];
	if ([session canSetSessionPreset:AVCaptureSessionPreset1280x720])
		session.sessionPreset = AVCaptureSessionPreset1280x720;

	if (![session canAddInput:input]) {
		[session commitConfiguration];
		[self fail:@"The selected camera could not be added to the capture session."];
		return;
	}
	[session addInput:input];

	AVCaptureVideoDataOutput *output = [[AVCaptureVideoDataOutput alloc] init];
	output.alwaysDiscardsLateVideoFrames = YES;
	output.videoSettings = @{ (NSString *) kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA) };
	self.captureQueue = dispatch_queue_create("info.mumble.screenshare.avfoundation", DISPATCH_QUEUE_SERIAL);
	[output setSampleBufferDelegate:self queue:self.captureQueue];
	if (![session canAddOutput:output]) {
		[session commitConfiguration];
		[self fail:@"Camera frames could not be added to the capture session."];
		return;
	}
	[session addOutput:output];

	AVCaptureConnection *connection = [output connectionWithMediaType:AVMediaTypeVideo];
	if (connection.supportsVideoMinFrameDuration)
		connection.videoMinFrameDuration = CMTimeMake(1, AVF_FPS);
	if (connection.supportsVideoMaxFrameDuration)
		connection.videoMaxFrameDuration = CMTimeMake(1, AVF_FPS);

	[session commitConfiguration];
	self.session = session;
	self.output  = output;

	dispatch_async(self.captureQueue, ^{
		[session startRunning];
		if (!session.running) {
			[self fail:@"The camera capture session did not start."];
			return;
		}
		if (self.onStarted)
			dispatch_async(dispatch_get_main_queue(), self.onStarted);
	});
}

- (void)stop {
	AVCaptureSession *session = self.session;
	AVCaptureVideoDataOutput *output = self.output;
	self.session = nil;
	self.output  = nil;
	if (!session)
		return;

	[output setSampleBufferDelegate:nil queue:nullptr];
	dispatch_queue_t queue = self.captureQueue;
	if (queue) {
		dispatch_async(queue, ^{
			[session stopRunning];
		});
	} else {
		[session stopRunning];
	}
}

- (void)captureOutput:(AVCaptureOutput *)output
		didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
			   fromConnection:(AVCaptureConnection *)connection {
	(void) output;
	(void) connection;
	CVPixelBufferRef pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
	if (!pixelBuffer)
		return;

	CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
	const int width        = static_cast< int >(CVPixelBufferGetWidth(pixelBuffer));
	const int height       = static_cast< int >(CVPixelBufferGetHeight(pixelBuffer));
	const int bytesPerLine = static_cast< int >(CVPixelBufferGetBytesPerRow(pixelBuffer));
	const uchar *base      = static_cast< const uchar * >(CVPixelBufferGetBaseAddress(pixelBuffer));

	QImage wrapped(base, width, height, bytesPerLine, QImage::Format_ARGB32);
	QImage frame = wrapped.copy().convertToFormat(QImage::Format_RGBA8888);
	CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);

	dispatch_async(dispatch_get_main_queue(), ^{
		if (self.onFrame)
			self.onFrame(frame);
	});
}

@end

static AVFCaptureDelegate *g_cameraDelegate = nil;
static quint64 g_cameraGeneration = 0;

void avf_stopCamera() {
	++g_cameraGeneration;
	if (g_cameraDelegate) {
		[g_cameraDelegate stop];
		g_cameraDelegate = nil;
	}
}

void avf_startCamera(const QString &uniqueID, std::function< void() > onStarted,
					 std::function< void(QString) > onError, std::function< void(QImage) > onFrame) {
	avf_stopCamera();
	const quint64 generation = g_cameraGeneration;

	AVFCaptureDelegate *delegate = [[AVFCaptureDelegate alloc] init];
	g_cameraDelegate   = delegate;
	delegate.onStarted = [generation, onStarted]() {
		if (g_cameraDelegate && g_cameraGeneration == generation)
			onStarted();
	};
	delegate.onError = [generation, onError](NSString *message) {
		if (g_cameraDelegate && g_cameraGeneration == generation)
			onError(QString::fromNSString(message));
	};
	delegate.onFrame = [generation, onFrame](QImage frame) {
		if (g_cameraDelegate && g_cameraGeneration == generation)
			onFrame(frame);
	};

	NSString *deviceID  = uniqueID.toNSString();
	auto startIfCurrent = ^{
		if (g_cameraDelegate == delegate && g_cameraGeneration == generation)
			[delegate startWithDeviceID:deviceID];
	};

	switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo]) {
		case AVAuthorizationStatusAuthorized:
			startIfCurrent();
			break;
		case AVAuthorizationStatusNotDetermined:
			[AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
								   completionHandler:^(BOOL granted) {
									   dispatch_async(dispatch_get_main_queue(), ^{
										   if (g_cameraDelegate != delegate || g_cameraGeneration != generation)
											   return;
										   if (granted)
											   startIfCurrent();
										   else
											   delegate.onError(@"Camera access was denied. Enable it in System Settings > Privacy & Security > Camera.");
									   });
								   }];
			break;
		case AVAuthorizationStatusDenied:
			delegate.onError(@"Camera access is disabled. Enable it in System Settings > Privacy & Security > Camera.");
			break;
		case AVAuthorizationStatusRestricted:
			delegate.onError(@"Camera access is restricted by the system.");
			break;
	}
}

#endif // __APPLE__ && USE_SCREEN_SHARING
