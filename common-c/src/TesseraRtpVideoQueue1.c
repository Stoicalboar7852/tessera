// Extra video stream 1: upstream's RtpVideoQueue.c compiled again with its own state (see TesseraVideoInstance.h).
#define TESSERA_VIDEO_INSTANCE 1
// Named through a macro: qmake leaves out of the build any source file that another one #includes.
#define TESSERA_UPSTREAM_SOURCE "RtpVideoQueue.c"
#include "TesseraVideoInstance.h"
#include TESSERA_UPSTREAM_SOURCE
