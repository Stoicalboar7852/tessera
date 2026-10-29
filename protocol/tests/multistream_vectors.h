/**
 * @file protocol/tests/multistream_vectors.h
 * @brief Test vectors shared by the host's and common-c's multistream tests, so both sides agree on the wire format.
 */
#pragma once

/* Two displays: 2560x1440@60 primary at the origin, 1920x1080@59.94 HDR to its right, 180 px lower. */
#define TESSERA_TV_DISPLAY_LIST_2 "a1b2c3d4.2560.1440.60.1.0.0.109.0~e5f60718.1920.1080.60000.1001.2560.180.92.1"

/* Bitrate split of 50000 kbit/s for 2560x1440@60 and 1920x1080@60: pixel rates 221184000 and 124416000. */
#define TESSERA_TV_SPLIT_TOTAL_KBPS 50000
#define TESSERA_TV_SPLIT_KBPS_0 32000
#define TESSERA_TV_SPLIT_KBPS_1 18000
