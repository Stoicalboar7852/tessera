/**
 * @file host-helpers/common/include/tessera/display_modes.h
 * @brief Mode lists, CVT reduced-blanking timings and EDID bytes for Tessera's virtual monitors.
 *
 * Shared by the driver (UMDF, C++17, no exceptions) and the user-mode tools and tests (C++20), so it is header-only
 * and uses only fixed-size arrays and <cstdint>/<cstring>: no STL containers, no allocation, no exceptions.
 * Written from the public specifications (VESA CVT 1.2 reduced blanking v1, VESA E-EDID 1.4, CTA-861-H data
 * blocks); see docs/adr/0005-vdd-driver.md.
 */
#pragma once

#include <cstdint>
#include <cstring>

namespace tessera::modes {

  /// Exact refresh rate as a fraction of hertz (60000/1001 = 59.94 Hz).
  struct rational_t {
    std::uint32_t numerator = 0;
    std::uint32_t denominator = 1;
  };

  /// One display timing. Porches and sync widths are in pixels/lines.
  struct timing_t {
    std::uint32_t width = 0;  ///< Active pixels.
    std::uint32_t height = 0;  ///< Active lines.
    rational_t refresh;  ///< Vertical refresh, exact.
    std::uint32_t h_front = 0, h_sync = 0, h_back = 0;
    std::uint32_t v_front = 0, v_sync = 0, v_back = 0;
    std::uint64_t pixel_clock_hz = 0;  ///< refresh × total pixels, rounded to the nearest hertz.

    std::uint32_t h_total() const {
      return width + h_front + h_sync + h_back;
    }

    std::uint32_t v_total() const {
      return height + v_front + v_sync + v_back;
    }
  };

  /// Largest mode list per monitor (brief F1: the requested modes plus a small safe set).
  constexpr std::uint32_t max_modes = 8;

  struct mode_list_t {
    timing_t modes[max_modes];
    std::uint32_t count = 0;  ///< modes[0] is the preferred (requested) mode.
  };

  /// Limits for a requested mode.
  constexpr std::uint32_t min_dimension = 640;
  constexpr std::uint32_t max_dimension = 8192;
  constexpr std::uint32_t min_refresh_mhz = 23000;
  constexpr std::uint32_t max_refresh_mhz = 500000;

  /// Millihertz value of a rational refresh rate, rounded.
  inline std::uint32_t refresh_mhz(rational_t r) {
    if (r.denominator == 0) {
      return 0;
    }
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(r.numerator) * 1000u + r.denominator / 2) / r.denominator);
  }

  /// true if the requested size and refresh rate are within the supported limits.
  inline bool is_valid_request(std::uint32_t width, std::uint32_t height, rational_t refresh) {
    if (width < min_dimension || height < 480 || width > max_dimension || height > max_dimension || refresh.denominator == 0) {
      return false;
    }
    const std::uint32_t mhz = refresh_mhz(refresh);
    return mhz >= min_refresh_mhz && mhz <= max_refresh_mhz;
  }

  /// Reduces a fraction (so 120/2 and 60/1 compare equal).
  inline rational_t reduce(rational_t r) {
    std::uint32_t a = r.numerator;
    std::uint32_t b = r.denominator;
    while (b != 0) {
      const std::uint32_t t = a % b;
      a = b;
      b = t;
    }
    if (a == 0) {
      return r;
    }
    return {r.numerator / a, r.denominator / a};
  }

  inline bool same_refresh(rational_t a, rational_t b) {
    return static_cast<std::uint64_t>(a.numerator) * b.denominator == static_cast<std::uint64_t>(b.numerator) * a.denominator;
  }

  /**
   * @brief CVT reduced-blanking (version 1) timing for an exact refresh rate.
   *
   * Uses the CVT-RB blanking: 160-pixel horizontal blank (48 front, 32 sync, 80 back), 3-line front porch, a
   * vertical sync width from the aspect ratio, and at least 460 µs of vertical blanking. Unlike the CVT formula,
   * the pixel clock is not rounded to 0.25 MHz: a virtual monitor has no PLL, so the clock is chosen to make the
   * refresh rate exact (59.94 stays 60000/1001).
   */
  inline timing_t cvt_rb(std::uint32_t width, std::uint32_t height, rational_t refresh) {
    timing_t t;
    t.width = width;
    t.height = height;
    t.refresh = reduce(refresh);
    t.h_front = 48;
    t.h_sync = 32;
    t.h_back = 80 + (8 - width % 8) % 8;  // keep the horizontal total a multiple of the 8-pixel cell
    t.v_front = 3;

    // Vertical sync width by aspect ratio (CVT table 3-2); 10 for non-standard ratios.
    const std::uint64_t w = width;
    const std::uint64_t h = height;
    if (w * 3 == h * 4) {
      t.v_sync = 4;
    } else if (w * 9 == h * 16) {
      t.v_sync = 5;
    } else if (w * 10 == h * 16) {
      t.v_sync = 6;
    } else if (w * 4 == h * 5 || w * 9 == h * 15) {
      t.v_sync = 7;
    } else {
      t.v_sync = 10;
    }

    // Lines of vertical blanking needed for 460 µs at the estimated line period:
    //   h_period_us = (1e6 / refresh - 460) / height;  vbi_lines = floor(460 / h_period_us) + 1
    // computed as  vbi = floor(460 * height / (1e6 / refresh - 460)) + 1  in integer microsecond units.
    const double refresh_hz = static_cast<double>(t.refresh.numerator) / static_cast<double>(t.refresh.denominator);
    const double frame_us = 1e6 / refresh_hz;
    std::uint32_t vbi = 0;
    if (frame_us > 460.0) {
      vbi = static_cast<std::uint32_t>(460.0 * static_cast<double>(height) / (frame_us - 460.0)) + 1;
    }
    const std::uint32_t min_vbi = t.v_front + t.v_sync + 6;  // RB_V_FPORCH + V_SYNC + MIN_V_BPORCH
    if (vbi < min_vbi) {
      vbi = min_vbi;
    }
    t.v_back = vbi - t.v_front - t.v_sync;

    const std::uint64_t total = static_cast<std::uint64_t>(t.h_total()) * t.v_total();
    t.pixel_clock_hz = (total * t.refresh.numerator + t.refresh.denominator / 2) / t.refresh.denominator;
    return t;
  }

  /**
   * @brief The modes reported for one monitor: the requested mode first (preferred), then a small safe set
   *        (1280×720, 1920×1080, 2560×1440 and 3840×2160 at 60 Hz, and the requested size at 60 Hz), without
   *        duplicates and never more than max_modes.
   * @return false if the request is outside the supported limits.
   */
  inline bool build_mode_list(std::uint32_t width, std::uint32_t height, rational_t refresh, mode_list_t &out) {
    out.count = 0;
    if (!is_valid_request(width, height, refresh)) {
      return false;
    }
    const auto add = [&out](std::uint32_t w, std::uint32_t h, rational_t r) {
      for (std::uint32_t i = 0; i < out.count; ++i) {
        if (out.modes[i].width == w && out.modes[i].height == h && same_refresh(out.modes[i].refresh, r)) {
          return;
        }
      }
      if (out.count < max_modes) {
        out.modes[out.count++] = cvt_rb(w, h, r);
      }
    };
    const rational_t hz60 {60, 1};
    add(width, height, refresh);
    add(width, height, hz60);
    add(1920, 1080, hz60);
    add(1280, 720, hz60);
    add(2560, 1440, hz60);
    add(3840, 2160, hz60);
    return true;
  }

  // ---------------------------------------------------------------------------------------------------------------
  // EDID
  // ---------------------------------------------------------------------------------------------------------------

  /// The three-letter manufacturer ID in every Tessera EDID ("TSR"). Not a registered PNP ID; Windows shows it in
  /// the monitor's hardware ID (MONITOR\TSRxxxx).
  constexpr char edid_manufacturer[4] = "TSR";

  /// The identity Windows uses to recognise a monitor across sessions (manufacturer + product code + serial).
  struct edid_identity_t {
    std::uint16_t product = 0;
    std::uint32_t serial = 0;

    bool operator==(const edid_identity_t &o) const {
      return product == o.product && serial == o.serial;
    }
  };

  /// Derives the EDID identity from a monitor's 128-bit stable ID (FNV-1a, 48 bits used).
  inline edid_identity_t identity_from_id(const std::uint8_t (&id)[16]) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (const std::uint8_t b : id) {
      h ^= b;
      h *= 0x100000001b3ull;
    }
    return {static_cast<std::uint16_t>(h >> 32), static_cast<std::uint32_t>(h)};
  }

  /// Reads the identity back from EDID bytes; false if the bytes are not a Tessera EDID.
  inline bool read_identity(const std::uint8_t *edid, std::uint32_t size, edid_identity_t &out) {
    static const std::uint8_t header[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    if (edid == nullptr || size < 128 || std::memcmp(edid, header, 8) != 0) {
      return false;
    }
    const std::uint16_t mfg = static_cast<std::uint16_t>((edid[8] << 8) | edid[9]);
    const std::uint16_t ours = static_cast<std::uint16_t>(((edid_manufacturer[0] - '@') << 10) | ((edid_manufacturer[1] - '@') << 5) | (edid_manufacturer[2] - '@'));
    if (mfg != ours) {
      return false;
    }
    out.product = static_cast<std::uint16_t>(edid[10] | (edid[11] << 8));
    out.serial = static_cast<std::uint32_t>(edid[12]) | (static_cast<std::uint32_t>(edid[13]) << 8) | (static_cast<std::uint32_t>(edid[14]) << 16) | (static_cast<std::uint32_t>(edid[15]) << 24);
    return true;
  }

  struct edid_t {
    std::uint8_t bytes[256] = {};
    std::uint32_t size = 0;  ///< 128 (base block) or 256 (with the CTA-861 extension for HDR).
  };

  struct edid_options_t {
    edid_identity_t identity;
    char name[14] = {};  ///< Monitor name, ASCII, up to 13 characters.
    std::uint32_t width_mm = 0;  ///< Physical size; 0 = derive from the preferred mode at 96 DPI.
    std::uint32_t height_mm = 0;
    bool hdr = false;  ///< Adds BT.2020 primaries and a CTA-861 extension with HDR static metadata (PQ, 1000 nits).
  };

  namespace detail {

    inline void checksum(std::uint8_t *block) {
      std::uint32_t sum = 0;
      for (int i = 0; i < 127; ++i) {
        sum += block[i];
      }
      block[127] = static_cast<std::uint8_t>((256 - sum % 256) % 256);
    }

    /// 10-bit CIE coordinate (value × 1024, rounded).
    inline std::uint32_t cie10(double v) {
      return static_cast<std::uint32_t>(v * 1024.0 + 0.5);
    }

    inline void chromaticity(std::uint8_t *p, const double (&xy)[8]) {
      std::uint32_t c[8];
      for (int i = 0; i < 8; ++i) {
        c[i] = cie10(xy[i]);
      }
      p[0] = static_cast<std::uint8_t>(((c[0] & 3) << 6) | ((c[1] & 3) << 4) | ((c[2] & 3) << 2) | (c[3] & 3));
      p[1] = static_cast<std::uint8_t>(((c[4] & 3) << 6) | ((c[5] & 3) << 4) | ((c[6] & 3) << 2) | (c[7] & 3));
      for (int i = 0; i < 8; ++i) {
        p[2 + i] = static_cast<std::uint8_t>(c[i] >> 2);
      }
    }

    /// 18-byte detailed timing descriptor; false if the timing cannot be expressed (pixel clock > 655.35 MHz).
    inline bool detailed_timing(std::uint8_t *p, const timing_t &t, std::uint32_t width_mm, std::uint32_t height_mm) {
      const std::uint64_t clock_10khz = (t.pixel_clock_hz + 5000) / 10000;
      const std::uint32_t h_blank = t.h_front + t.h_sync + t.h_back;
      const std::uint32_t v_blank = t.v_front + t.v_sync + t.v_back;
      if (clock_10khz == 0 || clock_10khz > 0xffff || t.width > 4095 || t.height > 4095 || h_blank > 4095 || v_blank > 4095 || t.h_front > 1023 || t.h_sync > 1023 || t.v_front > 63 || t.v_sync > 63) {
        return false;
      }
      p[0] = static_cast<std::uint8_t>(clock_10khz & 0xff);
      p[1] = static_cast<std::uint8_t>(clock_10khz >> 8);
      p[2] = static_cast<std::uint8_t>(t.width & 0xff);
      p[3] = static_cast<std::uint8_t>(h_blank & 0xff);
      p[4] = static_cast<std::uint8_t>(((t.width >> 8) << 4) | (h_blank >> 8));
      p[5] = static_cast<std::uint8_t>(t.height & 0xff);
      p[6] = static_cast<std::uint8_t>(v_blank & 0xff);
      p[7] = static_cast<std::uint8_t>(((t.height >> 8) << 4) | (v_blank >> 8));
      p[8] = static_cast<std::uint8_t>(t.h_front & 0xff);
      p[9] = static_cast<std::uint8_t>(t.h_sync & 0xff);
      p[10] = static_cast<std::uint8_t>(((t.v_front & 0xf) << 4) | (t.v_sync & 0xf));
      p[11] = static_cast<std::uint8_t>(((t.h_front >> 8) << 6) | ((t.h_sync >> 8) << 4) | ((t.v_front >> 4) << 2) | (t.v_sync >> 4));
      const std::uint32_t wmm = width_mm > 4095 ? 4095 : width_mm;
      const std::uint32_t hmm = height_mm > 4095 ? 4095 : height_mm;
      p[12] = static_cast<std::uint8_t>(wmm & 0xff);
      p[13] = static_cast<std::uint8_t>(hmm & 0xff);
      p[14] = static_cast<std::uint8_t>(((wmm >> 8) << 4) | (hmm >> 8));
      p[15] = 0;
      p[16] = 0;
      p[17] = 0x1e;  // digital separate sync, H+ V+ (CVT-RB uses H+ V-, but a virtual sink does not care)
      return true;
    }

    /// 18-byte display descriptor with a 13-character text payload (tag 0xFC name, 0xFF serial).
    inline void text_descriptor(std::uint8_t *p, std::uint8_t tag, const char *text) {
      p[0] = p[1] = p[2] = 0;
      p[3] = tag;
      p[4] = 0;
      std::size_t n = 0;
      while (n < 13 && text[n] != '\0') {
        const char c = text[n];
        p[5 + n] = static_cast<std::uint8_t>((c >= 0x20 && c < 0x7f) ? c : '?');
        ++n;
      }
      if (n < 13) {
        p[5 + n++] = 0x0a;
      }
      while (n < 13) {
        p[5 + n++] = 0x20;
      }
    }

  }  // namespace detail

  /**
   * @brief Builds the EDID for one monitor. The preferred mode is modes.modes[0] (detailed timing 1, if it fits in
   *        an EDID detailed timing); the full mode list is reported by the driver itself, keyed by the identity.
   */
  inline bool build_edid(const mode_list_t &modes, const edid_options_t &o, edid_t &out) {
    std::memset(out.bytes, 0, sizeof(out.bytes));
    out.size = 0;
    if (modes.count == 0) {
      return false;
    }
    std::uint8_t *b = out.bytes;
    static const std::uint8_t header[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    std::memcpy(b, header, 8);
    const std::uint16_t mfg = static_cast<std::uint16_t>(((edid_manufacturer[0] - '@') << 10) | ((edid_manufacturer[1] - '@') << 5) | (edid_manufacturer[2] - '@'));
    b[8] = static_cast<std::uint8_t>(mfg >> 8);
    b[9] = static_cast<std::uint8_t>(mfg & 0xff);
    b[10] = static_cast<std::uint8_t>(o.identity.product & 0xff);
    b[11] = static_cast<std::uint8_t>(o.identity.product >> 8);
    b[12] = static_cast<std::uint8_t>(o.identity.serial & 0xff);
    b[13] = static_cast<std::uint8_t>((o.identity.serial >> 8) & 0xff);
    b[14] = static_cast<std::uint8_t>((o.identity.serial >> 16) & 0xff);
    b[15] = static_cast<std::uint8_t>(o.identity.serial >> 24);
    b[16] = 0;  // week: unspecified
    b[17] = 2026 - 1990;  // year of manufacture
    b[18] = 1;  // EDID 1.4
    b[19] = 4;
    b[20] = o.hdr ? 0xb5 : 0xa5;  // digital; 10 or 8 bits per colour; DisplayPort

    const timing_t &pref = modes.modes[0];
    std::uint32_t wmm = o.width_mm;
    std::uint32_t hmm = o.height_mm;
    if (wmm == 0 || hmm == 0) {
      wmm = (pref.width * 254 + 480) / 960;  // 96 DPI
      hmm = (pref.height * 254 + 480) / 960;
    }
    const std::uint32_t wcm = (wmm + 5) / 10;
    const std::uint32_t hcm = (hmm + 5) / 10;
    b[21] = static_cast<std::uint8_t>(wcm > 255 ? 255 : wcm);
    b[22] = static_cast<std::uint8_t>(hcm > 255 ? 255 : hcm);
    b[23] = 120;  // gamma 2.2
    b[24] = o.hdr ? 0x02 : 0x06;  // RGB 4:4:4; sRGB default colour space for SDR only; preferred timing is native
    if (o.hdr) {
      const double bt2020[8] = {0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290};
      detail::chromaticity(b + 25, bt2020);
    } else {
      const double srgb[8] = {0.640, 0.330, 0.300, 0.600, 0.150, 0.060, 0.3127, 0.3290};
      detail::chromaticity(b + 25, srgb);
    }
    // 35-37: no established timings. 38-53: standard timings unused.
    for (int i = 38; i < 54; ++i) {
      b[i] = 0x01;
    }
    if (!detail::detailed_timing(b + 54, pref, wmm, hmm)) {
      // Too fast for an EDID detailed timing (e.g. 4K at 144 Hz): describe the 60 Hz variant instead; the exact mode
      // still comes from the driver's mode list.
      const timing_t fallback = cvt_rb(pref.width, pref.height, {60, 1});
      if (!detail::detailed_timing(b + 54, fallback, wmm, hmm)) {
        std::memset(b + 54, 0, 18);
        b[57] = 0x10;  // dummy descriptor
        b[24] = static_cast<std::uint8_t>(b[24] & ~0x02);  // so descriptor 1 is not the preferred timing
      }
    }
    detail::text_descriptor(b + 72, 0xfc, o.name[0] != '\0' ? o.name : "Tessera");
    char serial[14];
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 8; ++i) {
      serial[i] = hex[(o.identity.serial >> (28 - 4 * i)) & 0xf];
    }
    for (int i = 0; i < 4; ++i) {
      serial[8 + i] = hex[(o.identity.product >> (12 - 4 * i)) & 0xf];
    }
    serial[12] = '\0';
    detail::text_descriptor(b + 90, 0xff, serial);
    b[108 + 3] = 0x10;  // descriptor 4: dummy (the block was zeroed above)
    b[126] = o.hdr ? 1 : 0;
    detail::checksum(b);
    out.size = 128;

    if (o.hdr) {
      std::uint8_t *x = out.bytes + 128;
      x[0] = 0x02;  // CTA-861 extension
      x[1] = 0x03;  // revision 3
      std::uint8_t *p = x + 4;
      // Colorimetry data block: BT.2020 RGB.
      *p++ = 0xe3;  // tag 7 (extended), length 3
      *p++ = 0x05;
      *p++ = 0x80;
      *p++ = 0x00;
      // HDR static metadata data block: SDR + SMPTE ST 2084 (PQ), static metadata type 1,
      // luminance code values CV give 50 * 2^(CV/32) nits: max ~1000 nits (CV 138), max frame-average 400 nits
      // (CV 96); the minimum is max * (CV/255)^2 / 100: ~0.05 nits (CV 18).
      *p++ = 0xe6;  // tag 7 (extended), length 6
      *p++ = 0x06;
      *p++ = 0x05;
      *p++ = 0x01;
      *p++ = 138;
      *p++ = 96;
      *p++ = 18;
      x[2] = static_cast<std::uint8_t>(p - x);  // no detailed timings follow
      x[3] = 0x00;
      detail::checksum(x);
      out.size = 256;
    }
    return true;
  }

  /// Copies an ASCII rendering of a UTF-16 name (non-ASCII characters become '?') into a 13-character EDID name.
  inline void ascii_name(const std::uint16_t *utf16, std::uint32_t max_chars, char (&out)[14]) {
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < max_chars && utf16[i] != 0 && n < 13; ++i) {
      const std::uint16_t c = utf16[i];
      out[n++] = (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '?';
    }
    out[n] = '\0';
  }

}  // namespace tessera::modes
