// Builds one more copy of upstream's video receive path (VideoStream.c, VideoDepacketizer.c, RtpVideoQueue.c) for
// extra stream TESSERA_VIDEO_INSTANCE (1 .. TESSERA_MAX_STREAMS-1), with its own static state, so the upstream files
// stay unchanged. Each TesseraVideoStream<n>.c / TesseraVideoDepacketizer<n>.c / TesseraRtpVideoQueue<n>.c wrapper
// defines TESSERA_VIDEO_INSTANCE, includes this header, then includes one upstream file.
//
// This header renames the copy's entry points and binds the shared state it uses to the stream's slot. Anything else
// a copy refers to must be safe to share between streams; tests/check_instance_symbols.cmake fails the build when a
// copy starts referring to a symbol that isn't on its list, so an upstream change can't silently bind to stream 0.
#pragma once

#ifndef TESSERA_VIDEO_INSTANCE
#error "Define TESSERA_VIDEO_INSTANCE before including TesseraVideoInstance.h"
#endif

// Upstream declarations, as upstream sees them, before the names below are rebound
#include "Limelight-internal.h"

#define TESSERA_GLUE2(a, b) a##b
#define TESSERA_GLUE(a, b) TESSERA_GLUE2(a, b)
#define TESSERA_INSTANCE_NAME(name) TESSERA_GLUE(name, TESSERA_GLUE(_tessera, TESSERA_VIDEO_INSTANCE))

// Entry points of this copy
#define initializeVideoStream TESSERA_INSTANCE_NAME(initializeVideoStream)
#define destroyVideoStream TESSERA_INSTANCE_NAME(destroyVideoStream)
#define startVideoStream TESSERA_INSTANCE_NAME(startVideoStream)
#define stopVideoStream TESSERA_INSTANCE_NAME(stopVideoStream)
#define notifyKeyFrameReceived TESSERA_INSTANCE_NAME(notifyKeyFrameReceived)
#define readFirstFrame TESSERA_INSTANCE_NAME(readFirstFrame)
#define LiGetRTPVideoStats TESSERA_INSTANCE_NAME(LiGetRTPVideoStats)
#define initializeVideoDepacketizer TESSERA_INSTANCE_NAME(initializeVideoDepacketizer)
#define destroyVideoDepacketizer TESSERA_INSTANCE_NAME(destroyVideoDepacketizer)
#define queueRtpPacket TESSERA_INSTANCE_NAME(queueRtpPacket)
#define stopVideoDepacketizer TESSERA_INSTANCE_NAME(stopVideoDepacketizer)
#define requestDecoderRefresh TESSERA_INSTANCE_NAME(requestDecoderRefresh)
#define notifyFrameLost TESSERA_INSTANCE_NAME(notifyFrameLost)
#define validateDecodeUnitForPlayback TESSERA_INSTANCE_NAME(validateDecodeUnitForPlayback)
#define LiWaitForNextVideoFrame TESSERA_INSTANCE_NAME(LiWaitForNextVideoFrame)
#define LiPollNextVideoFrame TESSERA_INSTANCE_NAME(LiPollNextVideoFrame)
#define LiPeekNextVideoFrame TESSERA_INSTANCE_NAME(LiPeekNextVideoFrame)
#define LiWakeWaitForVideoFrame TESSERA_INSTANCE_NAME(LiWakeWaitForVideoFrame)
#define LiCompleteVideoFrame TESSERA_INSTANCE_NAME(LiCompleteVideoFrame)
#define LiGetPendingVideoFrames TESSERA_INSTANCE_NAME(LiGetPendingVideoFrames)
#define RtpvInitializeQueue TESSERA_INSTANCE_NAME(RtpvInitializeQueue)
#define RtpvCleanupQueue TESSERA_INSTANCE_NAME(RtpvCleanupQueue)
#define RtpvAddPacket TESSERA_INSTANCE_NAME(RtpvAddPacket)
#define RtpvGetCurrentFrameNumber TESSERA_INSTANCE_NAME(RtpvGetCurrentFrameNumber)
#define RtpvSubmitQueuedPackets TESSERA_INSTANCE_NAME(RtpvSubmitQueuedPackets)

// Shared state bound to this stream's slot
#define StreamConfig (TesseraStreams[TESSERA_VIDEO_INSTANCE].config)
#define VideoCallbacks (TesseraStreams[TESSERA_VIDEO_INSTANCE].callbacks)
#define ListenerCallbacks (TesseraStreams[TESSERA_VIDEO_INSTANCE].listener)
#define NegotiatedVideoFormat (TesseraStreams[TESSERA_VIDEO_INSTANCE].videoFormat)
#define VideoPingPayload (TesseraStreams[TESSERA_VIDEO_INSTANCE].pingPayload)

// Extra streams are always encrypted (protocol/multistream.md section 4)
#define EncryptionFeaturesEnabled (SS_ENC_VIDEO)

// Control-stream feedback goes out as per-stream messages
#define connectionDetectedFrameLoss(startFrame, endFrame) tesseraDetectedFrameLoss(TESSERA_VIDEO_INSTANCE, (startFrame), (endFrame))
#define connectionReceivedCompleteFrame(frameIndex, frameIsLTR) tesseraReceivedCompleteFrame(TESSERA_VIDEO_INSTANCE, (frameIndex), (frameIsLTR))
#define connectionSawFrame(frameIndex) tesseraSawFrame(TESSERA_VIDEO_INSTANCE, (frameIndex))
#define connectionSendFrameFecStatus(fecStatus) tesseraSendFrameFecStatus(TESSERA_VIDEO_INSTANCE, (fecStatus))
#define isReferenceFrameInvalidationEnabled() tesseraIsReferenceFrameInvalidationEnabled(TESSERA_VIDEO_INSTANCE)
#define LiGetCurrentHostDisplayHdrMode() tesseraGetHostHdrMode(TESSERA_VIDEO_INSTANCE)
#define LiRequestIdrFrame() tesseraRequestIdrFrame(TESSERA_VIDEO_INSTANCE)

// Prototypes under the new names (the upstream headers declared the old ones)
void initializeVideoStream(void);
void destroyVideoStream(void);
int startVideoStream(void* rendererContext, int drFlags);
void stopVideoStream(void);
void notifyKeyFrameReceived(void);
int readFirstFrame(void);
const RTP_VIDEO_STATS* LiGetRTPVideoStats(void);
void initializeVideoDepacketizer(int pktSize);
void destroyVideoDepacketizer(void);
void queueRtpPacket(PRTPV_QUEUE_ENTRY queueEntry);
void stopVideoDepacketizer(void);
void requestDecoderRefresh(void);
void notifyFrameLost(unsigned int frameNumber, bool speculative);
void validateDecodeUnitForPlayback(PDECODE_UNIT decodeUnit);
bool LiWaitForNextVideoFrame(VIDEO_FRAME_HANDLE* frameHandle, PDECODE_UNIT* decodeUnit);
bool LiPollNextVideoFrame(VIDEO_FRAME_HANDLE* frameHandle, PDECODE_UNIT* decodeUnit);
bool LiPeekNextVideoFrame(PDECODE_UNIT* decodeUnit);
void LiWakeWaitForVideoFrame(void);
void LiCompleteVideoFrame(VIDEO_FRAME_HANDLE handle, int drStatus);
int LiGetPendingVideoFrames(void);
void RtpvInitializeQueue(PRTP_VIDEO_QUEUE queue);
void RtpvCleanupQueue(PRTP_VIDEO_QUEUE queue);
int RtpvAddPacket(PRTP_VIDEO_QUEUE queue, PRTP_PACKET packet, int length, PRTPV_QUEUE_ENTRY packetEntry);
uint32_t RtpvGetCurrentFrameNumber(PRTP_VIDEO_QUEUE queue);
void RtpvSubmitQueuedPackets(PRTP_VIDEO_QUEUE queue);

// Placed after the upstream file in the wrappers: this copy's entry points for TesseraStreams.c
#define TESSERA_DEFINE_VIDEO_STREAM_OPS                                                              \
    const TESSERA_VIDEO_INSTANCE_OPS TESSERA_GLUE(TesseraVideoOps, TESSERA_VIDEO_INSTANCE) = {       \
        initializeVideoStream, destroyVideoStream, startVideoStream, stopVideoStream, LiGetRTPVideoStats \
    };

#define TESSERA_DEFINE_DEPACKETIZER_OPS                                                                    \
    const TESSERA_DEPACKETIZER_INSTANCE_OPS TESSERA_GLUE(TesseraDepacketizerOps, TESSERA_VIDEO_INSTANCE) = { \
        LiWaitForNextVideoFrame, LiPollNextVideoFrame, LiPeekNextVideoFrame,                             \
        LiWakeWaitForVideoFrame, LiCompleteVideoFrame, LiGetPendingVideoFrames                           \
    };
