/**
 * @file tools/mstream-client/src/main.cpp
 * @brief mstream-client: a command-line test client for multistream (see tools/mstream-client/README.md).
 */
// standard includes
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// local includes
#include "pairing.h"
#include "session.h"

namespace {

  void usage() {
    std::cerr <<
      "usage:\n"
      "  mstream-client pair   --host H --pin NNNN [--identity DIR] [--name NAME]\n"
      "  mstream-client stream --host H [--identity DIR] [--app Desktop] [--displays WxH@R[h],...] [--legacy]\n"
      "                        [--h264] [--bitrate KBPS] [--seconds N] [--dump-seconds N] [--mouse-test] --out DIR\n"
      "R is the refresh rate in Hz (59.94 allowed); a trailing 'h' makes the display HDR. Display 0 is the primary.\n"
      "stream writes report.json, common-c.log and stream<n>.h264/.h265 (the first seconds of each stream) to DIR.\n";
  }

  /// "1920x1080@60", "2560x1440@59.94h"
  bool parse_display(const std::string &text, mstream::display_request_t &d) {
    int width = 0;
    int height = 0;
    char rest[32] = {};
    if (std::sscanf(text.c_str(), "%dx%d@%31s", &width, &height, rest) != 3) {
      return false;
    }
    std::string rate {rest};
    d.hdr = !rate.empty() && rate.back() == 'h';
    if (d.hdr) {
      rate.pop_back();
    }
    d.width = width;
    d.height = height;
    if (rate == "59.94") {
      d.refresh_numerator = 60000;
      d.refresh_denominator = 1001;
    } else if (rate == "119.88") {
      d.refresh_numerator = 120000;
      d.refresh_denominator = 1001;
    } else {
      d.refresh_numerator = std::stoi(rate);
      d.refresh_denominator = 1;
    }
    return width > 0 && height > 0 && d.refresh_numerator > 0;
  }

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const std::string command = argv[1];
  std::string identity_dir = "mstream-identity";
  std::string pin;
  std::string name = "mstream-client";
  mstream::session_options_t options;
  options.displays = {mstream::display_request_t {}};

  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        usage();
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--host") {
      options.host = next();
    } else if (arg == "--identity") {
      identity_dir = next();
    } else if (arg == "--pin") {
      pin = next();
    } else if (arg == "--name") {
      name = next();
    } else if (arg == "--app") {
      options.app = next();
    } else if (arg == "--displays") {
      options.displays.clear();
      std::string list = next();
      std::size_t start = 0;
      while (start <= list.size()) {
        const auto end = list.find(',', start);
        mstream::display_request_t d;
        if (!parse_display(list.substr(start, end == std::string::npos ? std::string::npos : end - start), d)) {
          std::cerr << "bad display: " << list << '\n';
          return 2;
        }
        options.displays.push_back(d);
        if (end == std::string::npos) {
          break;
        }
        start = end + 1;
      }
    } else if (arg == "--legacy") {
      options.legacy = true;
    } else if (arg == "--h264") {
      options.hevc = false;
    } else if (arg == "--bitrate") {
      options.bitrate_kbps = std::stoi(next());
    } else if (arg == "--seconds") {
      options.seconds = std::stoi(next());
    } else if (arg == "--dump-seconds") {
      options.dump_seconds = std::stoi(next());
    } else if (arg == "--mouse-test") {
      options.mouse_test = true;
    } else if (arg == "--out") {
      options.out_dir = next();
    } else {
      usage();
      return 2;
    }
  }
  if (options.host.empty()) {
    usage();
    return 2;
  }

  auto identity = mstream::load_or_create_identity(identity_dir);
  if (!identity) {
    std::cerr << "can't load or create the client identity in " << identity_dir << '\n';
    return 1;
  }

  if (command == "pair") {
    if (pin.empty()) {
      usage();
      return 2;
    }
    const auto error = mstream::pair(options.host, identity_dir, *identity, pin, name);
    if (!error.empty()) {
      std::cerr << "pairing failed: " << error << '\n';
      return 1;
    }
    std::cout << "paired\n";
    return 0;
  }

  if (command == "stream") {
    if (options.out_dir.empty()) {
      usage();
      return 2;
    }
    const auto report = mstream::run_session(options, *identity);
    std::ofstream(options.out_dir + "/report.json") << report.dump(2) << '\n';
    std::cout << report.dump(2) << '\n';
    return report.contains("error") ? 1 : 0;
  }

  usage();
  return 2;
}
