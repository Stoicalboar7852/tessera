/**
 * @file protocol/include/tessera/protocol.h
 * @brief Tessera protocol extensions: names, limits and wire structures shared by the host and common-c.
 *
 * Plain C (C11 and C++), no dependencies, so the host (C++20), common-c (C11) and the tools can all include it.
 * The specification is protocol/multistream.md; the decision record is docs/adr/0007-multistream-protocol.md.
 *
 * Byte order follows the channel each structure travels on: control-message payloads are little-endian (as
 * Sunshine's own control extensions), input packets use the GameStream input convention (size big-endian, magic
 * little-endian, coordinates big-endian).
 */
#pragma once

#include <stdint.h>

/* ---------------------------------------------------------------------------------------------------------------
 * multistream (v1): one video stream per client monitor in one session
 * ------------------------------------------------------------------------------------------------------------- */

/** Extension version advertised by the host and requested by the client. */
#define TESSERA_MULTISTREAM_VERSION 1

/** Most streams in one session, stream 0 included. */
#define TESSERA_MAX_STREAMS 4

/** Most entries a client may send in its display list (the host accepts at most TESSERA_MAX_STREAMS of them). */
#define TESSERA_MAX_REQUESTED_DISPLAYS 16

/** Longest client monitor key, in bytes (sent as twice as many hex digits). */
#define TESSERA_MONITOR_KEY_MAX_BYTES 32

/** Longest display list value in the launch query, in characters. */
#define TESSERA_DISPLAY_LIST_MAX_CHARS 2048

/** Client DPI limits; 0 means unknown. */
#define TESSERA_MIN_DPI 48
#define TESSERA_MAX_DPI 1200

/** Largest absolute client layout coordinate. */
#define TESSERA_MAX_LAYOUT_COORDINATE 32767

/** Lowest bitrate the client split gives a stream, in kbit/s. */
#define TESSERA_MIN_STREAM_BITRATE_KBPS 1000

/* /serverinfo elements (host -> client). */
#define TESSERA_SERVERINFO_MULTISTREAM "TesseraMultiStream"
#define TESSERA_SERVERINFO_MAX_STREAMS "TesseraMaxStreams"

/* /launch and /resume query arguments (client -> host). */
#define TESSERA_LAUNCH_ARG_VERSION "tmsv"
#define TESSERA_LAUNCH_ARG_DISPLAYS "tmsd"

/* /launch and /resume reply elements (host -> client). */
#define TESSERA_LAUNCH_STREAMS "TesseraStreams"
#define TESSERA_LAUNCH_REASON "TesseraReason"

/* RTSP. */
#define TESSERA_SDP_STREAMS "x-tessera.streams"

/* Display list syntax: entries joined by '~', fields joined by '.' (all URL-unreserved characters). */
#define TESSERA_DISPLAY_ENTRY_SEPARATOR '~'
#define TESSERA_DISPLAY_FIELD_SEPARATOR '.'
#define TESSERA_DISPLAY_FIELD_COUNT 9

/* Display flags (field 9). */
#define TESSERA_DISPLAY_FLAG_HDR 0x1u

/**
 * Why a session got fewer streams than requested, or why a stream stopped. The launch reply carries the token
 * (TESSERA_REASON_TOKENS), stream status messages carry the number.
 */
enum tessera_reason_e {
  TESSERA_REASON_NONE = 0,
  TESSERA_REASON_HOST_LIMIT = 1,  /**< The host allows fewer streams (configuration or build limit). */
  TESSERA_REASON_ENCODER_LIMIT = 2,  /**< The GPU allows fewer concurrent encoder sessions. */
  TESSERA_REASON_NOT_PERMITTED = 3,  /**< Multi-display streaming is turned off for this client. */
  TESSERA_REASON_NO_DISPLAY_DRIVER = 4,  /**< The virtual display driver is missing or failed to open. */
  TESSERA_REASON_DISPLAY_FAILED = 5,  /**< Creating or arranging the displays failed. */
  TESSERA_REASON_ENCODER_FAILED = 6,  /**< The stream's encoder failed. */
  TESSERA_REASON_CAPTURE_FAILED = 7,  /**< Capturing the stream's display failed. */
  TESSERA_REASON_INVALID_REQUEST = 8,  /**< The display list or stream parameters failed validation. */
  TESSERA_REASON_UNSUPPORTED = 9,  /**< The host's encoder or capture method can't run several streams. */
  TESSERA_REASON_DECODER_FAILED = 10,  /**< Client side only: the stream's decoder couldn't start. */
  TESSERA_REASON_NO_VIDEO = 11,  /**< Client side only: the stream received no (complete) video. */
};

#define TESSERA_REASON_TOKENS \
  {"none", "host-limit", "encoder-limit", "not-permitted", "no-display-driver", "display-failed", "encoder-failed", \
   "capture-failed", "invalid-request", "unsupported", "decoder-failed", "no-video"}

/* Control messages, 0x5400-0x54FF (Sunshine's extensions use 0x55xx). */
#define TESSERA_CTRL_IDR_REQUEST 0x5400  /**< client -> host: tessera_ctrl_stream_t */
#define TESSERA_CTRL_INVALIDATE_REF_FRAMES 0x5401  /**< client -> host: tessera_ctrl_invalidate_t */
#define TESSERA_CTRL_HDR_MODE 0x5410  /**< host -> client: tessera_ctrl_stream_t, then the legacy HDR mode payload */
#define TESSERA_CTRL_STREAM_STATUS 0x5411  /**< host -> client: tessera_ctrl_stream_status_t */

/* Stream states in TESSERA_CTRL_STREAM_STATUS. */
#define TESSERA_STREAM_RUNNING 0
#define TESSERA_STREAM_STOPPED 1

/* Input packets. */
#define TESSERA_MOUSE_POSITION_MAGIC 0x54000001u

/*
 * Video encryption of extra streams: AES-GCM with the session's video key. The 12-byte IV is the stream's 64-bit
 * packet counter (little-endian, bytes 0-7), then 0x00 0x00, the stream index, and 'V' (bytes 8-11). For stream 0
 * this equals Sunshine's existing video IV. The IV travels in each packet's encryption prefix, so only the host
 * builds it.
 */
#define TESSERA_VIDEO_IV_STREAM_BYTE 10

#pragma pack(push, 1)

/** Control payload that names a stream (index 1 .. streams-1; legacy messages cover stream 0). */
typedef struct tessera_ctrl_stream_t {
  uint8_t stream;
  uint8_t reserved[3];  /**< zero */
} tessera_ctrl_stream_t;

/** TESSERA_CTRL_INVALIDATE_REF_FRAMES: frames first..last (inclusive) of the stream were lost. */
typedef struct tessera_ctrl_invalidate_t {
  tessera_ctrl_stream_t header;
  int64_t first_frame;  /**< little-endian */
  int64_t last_frame;  /**< little-endian */
} tessera_ctrl_invalidate_t;

/** TESSERA_CTRL_STREAM_STATUS. */
typedef struct tessera_ctrl_stream_status_t {
  tessera_ctrl_stream_t header;
  uint8_t state;  /**< TESSERA_STREAM_RUNNING or TESSERA_STREAM_STOPPED */
  uint8_t reason;  /**< enum tessera_reason_e */
  uint16_t reserved;  /**< zero */
} tessera_ctrl_stream_status_t;

/** TESSERA_MOUSE_POSITION_MAGIC: absolute mouse position on one stream's display. */
typedef struct tessera_mouse_position_t {
  uint32_t size;  /**< packet size excluding this field, big-endian */
  uint32_t magic;  /**< TESSERA_MOUSE_POSITION_MAGIC, little-endian */
  uint8_t stream;  /**< 0 .. streams-1 */
  uint8_t reserved;  /**< zero */
  int16_t x;  /**< big-endian, 0 .. width-1 */
  int16_t y;  /**< big-endian, 0 .. height-1 */
  int16_t width;  /**< big-endian reference width (the client's view of the stream) */
  int16_t height;  /**< big-endian reference height */
} tessera_mouse_position_t;

#pragma pack(pop)
