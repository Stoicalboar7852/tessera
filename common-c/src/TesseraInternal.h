// Tessera multistream internals (protocol/multistream.md): the per-stream state and the hooks that connect the
// upstream files (marked "Tessera:" where they call in) to TesseraStreams.c and TesseraControl.c.
// Included at the end of Limelight-internal.h.
#pragma once

#include "Tessera.h"

// State of one stream. Stream 0 is upstream's own stream and only uses display and bitrateKbps here; streams
// 1 .. TESSERA_MAX_STREAMS-1 each run a copy of the upstream video path bound to their slot (TesseraVideoInstance.h).
typedef struct _TESSERA_STREAM_SLOT {
    // From LiTesseraConfigure()
    TESSERA_DISPLAY display;
    DECODER_RENDERER_CALLBACKS callbacks; // VideoCallbacks of the copy
    void* rendererContext;

    // Per connection
    STREAM_CONFIGURATION config; // StreamConfig of the copy
    CONNECTION_LISTENER_CALLBACKS listener; // ListenerCallbacks of the copy
    int videoFormat; // NegotiatedVideoFormat of the copy
    SS_PING pingPayload; // VideoPingPayload of the copy
    int bitrateKbps; // this stream's share of the configured bitrate
    bool initialized;
    bool started;
    volatile bool stopped;
    volatile bool hostHdrMode;
} TESSERA_STREAM_SLOT, *PTESSERA_STREAM_SLOT;

extern TESSERA_STREAM_SLOT TesseraStreams[TESSERA_MAX_STREAMS];

// Entry points of one copy of the video path (defined by the TesseraVideoStream<n>.c and
// TesseraVideoDepacketizer<n>.c wrappers).
typedef struct _TESSERA_VIDEO_INSTANCE_OPS {
    void (*initialize)(void);
    void (*destroy)(void);
    int (*start)(void* rendererContext, int drFlags);
    void (*stop)(void);
    const RTP_VIDEO_STATS* (*getRtpStats)(void);
} TESSERA_VIDEO_INSTANCE_OPS;

typedef struct _TESSERA_DEPACKETIZER_INSTANCE_OPS {
    bool (*waitForNextFrame)(VIDEO_FRAME_HANDLE* frameHandle, PDECODE_UNIT* decodeUnit);
    bool (*pollNextFrame)(VIDEO_FRAME_HANDLE* frameHandle, PDECODE_UNIT* decodeUnit);
    bool (*peekNextFrame)(PDECODE_UNIT* decodeUnit);
    void (*wakeWaitForFrame)(void);
    void (*completeFrame)(VIDEO_FRAME_HANDLE handle, int drStatus);
    int (*pendingFrames)(void);
} TESSERA_DEPACKETIZER_INSTANCE_OPS;

extern const TESSERA_VIDEO_INSTANCE_OPS TesseraVideoOps1, TesseraVideoOps2, TesseraVideoOps3;
extern const TESSERA_DEPACKETIZER_INSTANCE_OPS TesseraDepacketizerOps1, TesseraDepacketizerOps2, TesseraDepacketizerOps3;

// Called from upstream files
void tesseraParseDescribe(const char* payload); // RtspConnection.c, after DESCRIBE
int tesseraSetupVideoStreams(void); // RtspConnection.c, after SETUP streamid=video
int tesseraAddSdpAttributes(int (*addAttribute)(void* head, char* name, const char* payload), void* head); // SdpGenerator.c
int tesseraStreamBitrateKbps(int stream); // SdpGenerator.c: the bitrate stream 0 asks for
void tesseraInitializeVideoStreams(void); // Connection.c
void tesseraStartVideoStreams(void* rendererContext, int drFlags);
void tesseraStopVideoStreams(void);
void tesseraDestroyVideoStreams(void);
void tesseraHandleControlMessage(unsigned short type, const uint8_t* payload, int length); // ControlStream.c
void tesseraFillMousePosition(NV_INPUT_HEADER* header); // InputStream.c, input thread, before sending

// Provided by upstream files
bool tesseraSendControlMessage(unsigned short type, const void* payload, int length, uint8_t channelId); // ControlStream.c
int tesseraQueueInputPacket(const void* packet, int length, uint8_t channelId); // InputStream.c

// Between TesseraControl.c and TesseraStreams.c
void tesseraReportHostStreamStatus(int stream, int state, int reason);
void tesseraSetHostHdrMode(int stream, bool enabled);
int tesseraGetStreamCountInternal(void);

// What the copies of the video path call instead of the stream 0 functions (TesseraVideoInstance.h)
void tesseraDetectedFrameLoss(int stream, uint32_t startFrame, uint32_t endFrame);
void tesseraReceivedCompleteFrame(int stream, uint32_t frameIndex, bool frameIsLTR);
void tesseraSawFrame(int stream, uint32_t frameIndex);
void tesseraSendFrameFecStatus(int stream, PSS_FRAME_FEC_STATUS fecStatus);
bool tesseraIsReferenceFrameInvalidationEnabled(int stream);
bool tesseraGetHostHdrMode(int stream);
void tesseraRequestIdrFrame(int stream);
