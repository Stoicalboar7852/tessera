/**
 * @file tools/mstream-client/src/session.h
 * @brief One streaming session: launch with a display list, receive every stream, report per-stream statistics.
 */
#pragma once

// standard includes
#include <string>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "http_client.h"

namespace mstream {

  struct display_request_t {
    int width = 1920;
    int height = 1080;
    int refresh_numerator = 60;
    int refresh_denominator = 1;
    bool hdr = false;
  };

  struct session_options_t {
    std::string host;
    std::string app = "Desktop";
    std::vector<display_request_t> displays;  ///< Display 0 is the primary; placed left to right.
    bool legacy = false;  ///< No display list: an ordinary single-stream session.
    bool hevc = true;
    int bitrate_kbps = 20000;  ///< Total for the session.
    int seconds = 30;
    int dump_seconds = 3;  ///< Raw video (Annex B) written per stream for the first seconds, for snapshots.
    bool mouse_test = false;  ///< Move the mouse to the centre of each stream and read the host cursor back.
    std::string out_dir;
  };

  /// Runs the session and returns the report (see tools/mstream-client/README.md).
  nlohmann::json run_session(const session_options_t &options, const identity_t &identity);

}  // namespace mstream
