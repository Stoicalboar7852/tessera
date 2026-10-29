/**
 * @file host-helpers/common/src/multistream.cpp
 * @brief Host side of the multistream extension (protocol/multistream.md).
 */
// standard includes
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <optional>

// local includes
#include "tessera/crypto.h"
#include "tessera/multistream.h"

namespace tessera::multistream {

  namespace {
    constexpr std::string_view where = "multistream::parse_display_list";

    error_t invalid(std::string message) {
      return make_error(tessera_code_e::invalid_data, std::string(where), std::move(message));
    }

    std::vector<std::string_view> split(std::string_view text, char separator) {
      std::vector<std::string_view> parts;
      std::size_t start = 0;
      while (true) {
        const auto end = text.find(separator, start);
        parts.push_back(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos) {
          return parts;
        }
        start = end + 1;
      }
    }

    /// Decimal digits only, at most 10 of them.
    std::optional<std::uint64_t> parse_unsigned(std::string_view field) {
      if (field.empty() || field.size() > 10 || !std::all_of(field.begin(), field.end(), [](char c) {
            return c >= '0' && c <= '9';
          })) {
        return std::nullopt;
      }
      std::uint64_t value = 0;
      std::from_chars(field.data(), field.data() + field.size(), value);
      return value;
    }

    /// An optional '-', then decimal digits.
    std::optional<std::int64_t> parse_signed(std::string_view field) {
      const bool negative = !field.empty() && field.front() == '-';
      const auto magnitude = parse_unsigned(negative ? field.substr(1) : field);
      if (!magnitude) {
        return std::nullopt;
      }
      return negative ? -static_cast<std::int64_t>(*magnitude) : static_cast<std::int64_t>(*magnitude);
    }

    int hex_value(char c) {
      if (c >= '0' && c <= '9') {
        return c - '0';
      }
      if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
      }
      if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
      }
      return -1;
    }

    std::optional<std::vector<std::uint8_t>> parse_key(std::string_view field) {
      if (field.size() < 2 || field.size() > 2 * TESSERA_MONITOR_KEY_MAX_BYTES || field.size() % 2 != 0) {
        return std::nullopt;
      }
      std::vector<std::uint8_t> key;
      for (std::size_t i = 0; i < field.size(); i += 2) {
        const int hi = hex_value(field[i]);
        const int lo = hex_value(field[i + 1]);
        if (hi < 0 || lo < 0) {
          return std::nullopt;
        }
        key.push_back(static_cast<std::uint8_t>(hi << 4 | lo));
      }
      return key;
    }

    result_t<client_display_t> parse_entry(std::string_view entry, std::size_t index) {
      const auto at = [index](std::string_view what) {
        return "display " + std::to_string(index) + ": " + std::string(what);
      };
      const auto fields = split(entry, TESSERA_DISPLAY_FIELD_SEPARATOR);
      if (fields.size() != TESSERA_DISPLAY_FIELD_COUNT) {
        return invalid(at("expected " + std::to_string(TESSERA_DISPLAY_FIELD_COUNT) + " fields, got " + std::to_string(fields.size())));
      }

      client_display_t d;
      auto key = parse_key(fields[0]);
      if (!key) {
        return invalid(at("bad monitor key"));
      }
      d.key = std::move(*key);

      const auto width = parse_unsigned(fields[1]);
      const auto height = parse_unsigned(fields[2]);
      const auto numerator = parse_unsigned(fields[3]);
      const auto denominator = parse_unsigned(fields[4]);
      if (!width || !height || !numerator || !denominator || *width > modes::max_dimension || *height > modes::max_dimension ||
          *numerator > std::numeric_limits<std::uint32_t>::max() || *denominator > std::numeric_limits<std::uint32_t>::max()) {
        return invalid(at("bad size or refresh rate"));
      }
      d.width = static_cast<std::uint32_t>(*width);
      d.height = static_cast<std::uint32_t>(*height);
      d.refresh = {static_cast<std::uint32_t>(*numerator), static_cast<std::uint32_t>(*denominator)};
      if (!modes::is_valid_request(d.width, d.height, d.refresh)) {
        return invalid(at("size or refresh rate out of range"));
      }

      const auto x = parse_signed(fields[5]);
      const auto y = parse_signed(fields[6]);
      if (!x || !y || std::abs(*x) > TESSERA_MAX_LAYOUT_COORDINATE || std::abs(*y) > TESSERA_MAX_LAYOUT_COORDINATE) {
        return invalid(at("bad position"));
      }
      d.x = static_cast<std::int32_t>(*x);
      d.y = static_cast<std::int32_t>(*y);

      const auto dpi = parse_unsigned(fields[7]);
      if (!dpi || (*dpi != 0 && (*dpi < TESSERA_MIN_DPI || *dpi > TESSERA_MAX_DPI))) {
        return invalid(at("bad DPI"));
      }
      d.dpi = static_cast<std::uint32_t>(*dpi);

      // Unknown flags are ignored, so later versions can add flags that older hosts may skip.
      const auto flags = parse_unsigned(fields[8]);
      if (!flags || *flags > 0xFFFF) {
        return invalid(at("bad flags"));
      }
      d.hdr = (*flags & TESSERA_DISPLAY_FLAG_HDR) != 0;
      return d;
    }

    bool overlap(const rect_t &a, const rect_t &b) {
      return std::int64_t {a.x} < std::int64_t {b.x} + b.width && std::int64_t {b.x} < std::int64_t {a.x} + a.width &&
             std::int64_t {a.y} < std::int64_t {b.y} + b.height && std::int64_t {b.y} < std::int64_t {a.y} + a.height;
    }

    /// Positive-length overlap of [a0, a1) and [b0, b1).
    bool spans_overlap(std::int64_t a0, std::int64_t a1, std::int64_t b0, std::int64_t b1) {
      return std::min(a1, b1) > std::max(a0, b0);
    }

    bool share_edge(const rect_t &a, const rect_t &b) {
      const std::int64_t ar = std::int64_t {a.x} + a.width, ab = std::int64_t {a.y} + a.height;
      const std::int64_t br = std::int64_t {b.x} + b.width, bb = std::int64_t {b.y} + b.height;
      if ((ar == b.x || br == a.x) && spans_overlap(a.y, ab, b.y, bb)) {
        return true;
      }
      return (ab == b.y || bb == a.y) && spans_overlap(a.x, ar, b.x, br);
    }

    bool usable_as_is(const std::vector<rect_t> &rects) {
      for (std::size_t i = 0; i < rects.size(); ++i) {
        for (std::size_t j = i + 1; j < rects.size(); ++j) {
          if (overlap(rects[i], rects[j])) {
            return false;
          }
        }
      }
      // Connected through shared edges.
      std::vector<bool> reached(rects.size(), false);
      std::vector<std::size_t> pending {0};
      reached[0] = true;
      while (!pending.empty()) {
        const auto i = pending.back();
        pending.pop_back();
        for (std::size_t j = 0; j < rects.size(); ++j) {
          if (!reached[j] && share_edge(rects[i], rects[j])) {
            reached[j] = true;
            pending.push_back(j);
          }
        }
      }
      return std::all_of(reached.begin(), reached.end(), [](bool r) {
        return r;
      });
    }
  }  // namespace

  result_t<std::vector<client_display_t>> parse_display_list(std::string_view text) {
    if (text.empty() || text.size() > TESSERA_DISPLAY_LIST_MAX_CHARS) {
      return invalid("display list is empty or longer than " + std::to_string(TESSERA_DISPLAY_LIST_MAX_CHARS) + " characters");
    }
    const auto entries = split(text, TESSERA_DISPLAY_ENTRY_SEPARATOR);
    if (entries.size() > TESSERA_MAX_REQUESTED_DISPLAYS) {
      return invalid("more than " + std::to_string(TESSERA_MAX_REQUESTED_DISPLAYS) + " displays");
    }

    std::vector<client_display_t> displays;
    for (std::size_t i = 0; i < entries.size(); ++i) {
      auto display = parse_entry(entries[i], i);
      if (!display) {
        return display.error();
      }
      for (const auto &earlier : displays) {
        if (earlier.key == display->key) {
          return invalid("display " + std::to_string(i) + ": monitor key repeated");
        }
      }
      displays.push_back(std::move(display).value());
    }
    return displays;
  }

  std::string format_display_list(const std::vector<client_display_t> &displays) {
    std::string out;
    for (const auto &d : displays) {
      if (!out.empty()) {
        out += TESSERA_DISPLAY_ENTRY_SEPARATOR;
      }
      out += crypto::to_hex(d.key.data(), d.key.size());
      for (const auto value : {std::int64_t {d.width}, std::int64_t {d.height}, std::int64_t {d.refresh.numerator}, std::int64_t {d.refresh.denominator}, std::int64_t {d.x}, std::int64_t {d.y}, std::int64_t {d.dpi}, std::int64_t {d.hdr ? TESSERA_DISPLAY_FLAG_HDR : 0}}) {
        out += TESSERA_DISPLAY_FIELD_SEPARATOR;
        out += std::to_string(value);
      }
    }
    return out;
  }

  bool matches_mode(const client_display_t &display, int width, int height, int fps) {
    if (display.width != static_cast<std::uint32_t>(width) || display.height != static_cast<std::uint32_t>(height)) {
      return false;
    }
    // Moonlight sends whole frames per second (59.94 Hz as 60).
    const auto mhz = static_cast<std::int64_t>(modes::refresh_mhz(display.refresh));
    return std::abs(mhz - std::int64_t {fps} * 1000) < 1000;
  }

  plan_t plan_streams(std::size_t requested, const limits_t &limits) {
    if (requested <= 1) {
      return {1, TESSERA_REASON_NONE};
    }
    if (!limits.permitted) {
      return {1, TESSERA_REASON_NOT_PERMITTED};
    }
    if (!limits.driver_available) {
      return {1, TESSERA_REASON_NO_DISPLAY_DRIVER};
    }
    const auto host_max = std::clamp<std::uint32_t>(limits.host_max, 1, TESSERA_MAX_STREAMS);
    const auto allowed = std::max<std::uint32_t>(1, std::min(host_max, limits.encoder_max));
    if (requested <= allowed) {
      return {static_cast<std::uint32_t>(requested), TESSERA_REASON_NONE};
    }
    return {allowed, limits.encoder_max < host_max ? TESSERA_REASON_ENCODER_LIMIT : TESSERA_REASON_HOST_LIMIT};
  }

  std::string_view reason_token(tessera_reason_e reason) {
    static constexpr const char *tokens[] = TESSERA_REASON_TOKENS;
    const auto index = static_cast<std::size_t>(reason);
    return index < std::size(tokens) ? tokens[index] : "unknown";
  }

  std::vector<rect_t> layout(const std::vector<client_display_t> &displays) {
    std::vector<rect_t> rects;
    for (const auto &d : displays) {
      rects.push_back({d.x, d.y, d.width, d.height});
    }
    if (rects.empty()) {
      return rects;
    }

    if (!usable_as_is(rects)) {
      std::vector<std::size_t> order(rects.size());
      std::iota(order.begin(), order.end(), 0);
      std::stable_sort(order.begin(), order.end(), [&rects](std::size_t a, std::size_t b) {
        return rects[a].x < rects[b].x;
      });
      std::int32_t x = 0;
      for (const auto i : order) {
        rects[i].x = x;
        rects[i].y = 0;
        x += static_cast<std::int32_t>(rects[i].width);
      }
    }

    const auto min_x = std::min_element(rects.begin(), rects.end(), [](const rect_t &a, const rect_t &b) {
                         return a.x < b.x;
                       })->x;
    const auto min_y = std::min_element(rects.begin(), rects.end(), [](const rect_t &a, const rect_t &b) {
                         return a.y < b.y;
                       })->y;
    for (auto &r : rects) {
      r.x -= min_x;
      r.y -= min_y;
    }
    return rects;
  }

  result_t<monitor_id_t> monitor_id(std::string_view client_cert_pem, const std::vector<std::uint8_t> &key) {
    std::string input = "tessera-monitor-v1";
    input.append(client_cert_pem);
    input.append(reinterpret_cast<const char *>(key.data()), key.size());
    const auto digest = crypto::sha256_hex(input);
    if (!digest) {
      return digest.error();
    }
    const std::string &hex = digest.value();
    monitor_id_t id {};
    for (std::size_t i = 0; i < id.size(); ++i) {
      id[i] = static_cast<std::uint8_t>(hex_value(hex[2 * i]) << 4 | hex_value(hex[2 * i + 1]));
    }
    return id;
  }

  std::pair<std::uint32_t, std::uint32_t> physical_size_mm(std::uint32_t width, std::uint32_t height, std::uint32_t dpi) {
    if (dpi == 0) {
      return {0, 0};
    }
    const auto mm = [dpi](std::uint32_t pixels) {
      return static_cast<std::uint32_t>((std::uint64_t {pixels} * 254 + dpi * 5) / (std::uint64_t {dpi} * 10));
    };
    return {mm(width), mm(height)};
  }

  std::vector<std::uint32_t> cap_bitrates(std::vector<std::uint32_t> kbps, std::uint32_t max_total_kbps) {
    const auto total = std::accumulate(kbps.begin(), kbps.end(), std::uint64_t {0});
    if (max_total_kbps == 0 || total <= max_total_kbps) {
      return kbps;
    }
    for (auto &k : kbps) {
      k = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::uint64_t {k} * max_total_kbps / total));
    }
    return kbps;
  }

}  // namespace tessera::multistream
