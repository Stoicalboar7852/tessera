// Tessera multi-display: per-stream video statistics for stream 0's overlay and the log (docs/phase-3-design.md,
// "Statistics overlay").
#pragma once

#include "streaming/video/decoder.h"

namespace tessera {

// Called by each stream's decoder about once a second with the last second's statistics. Every 10 reports it logs one
// "Tessera stats:" line for the stream, which tools/tests read.
void reportVideoStats(int stream, const VIDEO_STATS& lastSecond, int width, int height);

// Appends one line per extra stream that reported in the last 3 seconds to stream 0's overlay text
void appendOverlayLines(char* text, int maxLength);

// Starts over, for a new session
void resetVideoStats();

// Forgets a stream that stopped
void clearVideoStats(int stream);

}
