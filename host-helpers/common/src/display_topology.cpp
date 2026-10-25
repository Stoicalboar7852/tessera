/**
 * @file host-helpers/common/src/display_topology.cpp
 * @brief CCD snapshot / restore implementation.
 */
// local includes
#include "tessera/display_topology.h"
#include "tessera/win32.h"

// standard includes
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <sstream>
#include <thread>

using nlohmann::json;

namespace tessera::display {

  namespace {

    // Windows 11 24H2 split HDR from "advanced color". These definitions mirror the Windows SDK
    // (wingdi.h, 10.0.26100+) under our own names, because older SDKs and MinGW headers lack them.
    constexpr auto info_get_advanced_color_info_2 = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(15);
    constexpr auto info_set_hdr_state = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(16);

    struct advanced_color_info_2_t {
      DISPLAYCONFIG_DEVICE_INFO_HEADER header;
      UINT32 value;  // bit 4: highDynamicRangeSupported, bit 5: highDynamicRangeUserEnabled
      UINT32 color_encoding;
      UINT32 bits_per_color_channel;
      UINT32 active_color_mode;  // 0 SDR, 1 WCG, 2 HDR
    };

    struct set_hdr_state_t {
      DISPLAYCONFIG_DEVICE_INFO_HEADER header;
      UINT32 value;  // bit 0: enableHdr
    };

    struct raw_config_t {
      std::vector<DISPLAYCONFIG_PATH_INFO> paths;
      std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    };

    result_t<raw_config_t> query(UINT32 flags) {
      raw_config_t cfg;
      for (int attempt = 0; attempt < 8; ++attempt) {
        UINT32 np = 0;
        UINT32 nm = 0;
        LONG rc = GetDisplayConfigBufferSizes(flags, &np, &nm);
        if (rc != ERROR_SUCCESS) {
          return win32::win32_error(static_cast<DWORD>(rc), "GetDisplayConfigBufferSizes");
        }
        cfg.paths.resize(np);
        cfg.modes.resize(nm);
        rc = QueryDisplayConfig(flags, &np, cfg.paths.data(), &nm, cfg.modes.data(), nullptr);
        if (rc == ERROR_INSUFFICIENT_BUFFER) {
          continue;  // topology changed between the two calls
        }
        if (rc != ERROR_SUCCESS) {
          return win32::win32_error(static_cast<DWORD>(rc), "QueryDisplayConfig");
        }
        cfg.paths.resize(np);
        cfg.modes.resize(nm);
        return cfg;
      }
      return make_error(tessera_code_e::timeout, "QueryDisplayConfig", "topology kept changing while querying");
    }

    std::string luid_key(const LUID &l) {
      return std::to_string(l.HighPart) + ":" + std::to_string(l.LowPart);
    }

    /// Caches LUID -> adapter device path lookups.
    class adapter_names_t {
    public:
      std::string get(const LUID &luid) {
        const auto key = luid_key(luid);
        if (const auto it = cache_.find(key); it != cache_.end()) {
          return it->second;
        }
        DISPLAYCONFIG_ADAPTER_NAME name {};
        name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME;
        name.header.size = sizeof(name);
        name.header.adapterId = luid;
        std::string path;
        if (DisplayConfigGetDeviceInfo(&name.header) == ERROR_SUCCESS) {
          path = win32::to_utf8(name.adapterDevicePath);
        }
        cache_.emplace(key, path);
        return path;
      }

    private:
      std::map<std::string, std::string> cache_;
    };

    void read_target_name(const DISPLAYCONFIG_PATH_INFO &p, path_t &out) {
      DISPLAYCONFIG_TARGET_DEVICE_NAME name {};
      name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
      name.header.size = sizeof(name);
      name.header.adapterId = p.targetInfo.adapterId;
      name.header.id = p.targetInfo.id;
      if (DisplayConfigGetDeviceInfo(&name.header) == ERROR_SUCCESS) {
        out.monitor_path = win32::to_utf8(name.monitorDevicePath);
        out.monitor_name = win32::to_utf8(name.monitorFriendlyDeviceName);
        out.output_technology = static_cast<std::uint32_t>(name.outputTechnology);
      }
    }

    void read_source_name(const DISPLAYCONFIG_PATH_INFO &p, path_t &out) {
      DISPLAYCONFIG_SOURCE_DEVICE_NAME name {};
      name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
      name.header.size = sizeof(name);
      name.header.adapterId = p.sourceInfo.adapterId;
      name.header.id = p.sourceInfo.id;
      if (DisplayConfigGetDeviceInfo(&name.header) == ERROR_SUCCESS) {
        out.gdi_name = win32::to_utf8(name.viewGdiDeviceName);
      }
    }

    struct hdr_state_t {
      bool supported = false;
      bool enabled = false;
    };

    hdr_state_t read_hdr(const LUID &adapter, UINT32 target_id) {
      advanced_color_info_2_t info2 {};
      info2.header.type = info_get_advanced_color_info_2;
      info2.header.size = sizeof(info2);
      info2.header.adapterId = adapter;
      info2.header.id = target_id;
      if (DisplayConfigGetDeviceInfo(&info2.header) == ERROR_SUCCESS) {
        return {(info2.value & (1u << 4)) != 0, (info2.value & (1u << 5)) != 0};
      }
      DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO info {};
      info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
      info.header.size = sizeof(info);
      info.header.adapterId = adapter;
      info.header.id = target_id;
      if (DisplayConfigGetDeviceInfo(&info.header) == ERROR_SUCCESS) {
        return {(info.value & 0x1u) != 0, (info.value & 0x2u) != 0};
      }
      return {};
    }

    status_t write_hdr(const LUID &adapter, UINT32 target_id, bool enable, std::string_view context) {
      set_hdr_state_t s {};
      s.header.type = info_set_hdr_state;
      s.header.size = sizeof(s);
      s.header.adapterId = adapter;
      s.header.id = target_id;
      s.value = enable ? 1u : 0u;
      LONG rc = DisplayConfigSetDeviceInfo(&s.header);
      if (rc == ERROR_SUCCESS) {
        return status_t::ok();
      }
      // Before Windows 11 24H2: the advanced colour switch is the HDR switch.
      DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE legacy {};
      legacy.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
      legacy.header.size = sizeof(legacy);
      legacy.header.adapterId = adapter;
      legacy.header.id = target_id;
      legacy.value = enable ? 1u : 0u;
      rc = DisplayConfigSetDeviceInfo(&legacy.header);
      if (rc != ERROR_SUCCESS) {
        return win32::win32_error(static_cast<DWORD>(rc), "DisplayConfigSetDeviceInfo(HDR)", context);
      }
      return status_t::ok();
    }

    double hz(const rational_t &r) {
      return r.denominator == 0 ? 0.0 : static_cast<double>(r.numerator) / static_cast<double>(r.denominator);
    }

    std::string path_label(const path_t &p) {
      std::string label = p.monitor_name.empty() ? std::string("target ") + std::to_string(p.target_id) : p.monitor_name;
      if (!p.gdi_name.empty()) {
        label += " (" + p.gdi_name + ")";
      }
      return label;
    }

    bool same_identity(const path_t &a, const path_t &b) {
      return a.adapter_path == b.adapter_path && a.target_id == b.target_id &&
             a.source_adapter_path == b.source_adapter_path && a.source_id == b.source_id;
    }

    json rational_json(const rational_t &r) {
      return json::array({r.numerator, r.denominator});
    }

    bool read_rational(const json &j, rational_t &out) {
      if (!j.is_array() || j.size() != 2 || !j[0].is_number_unsigned() || !j[1].is_number_unsigned()) {
        return false;
      }
      out.numerator = j[0].get<std::uint32_t>();
      out.denominator = j[1].get<std::uint32_t>();
      return true;
    }

  }  // namespace

  result_t<topology_t> capture() {
    auto cfg = query(QDC_ONLY_ACTIVE_PATHS);
    if (!cfg) {
      return cfg.error();
    }
    adapter_names_t names;
    topology_t topo;
    for (const auto &p : cfg->paths) {
      if ((p.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0) {
        continue;
      }
      path_t out;
      out.adapter_path = names.get(p.targetInfo.adapterId);
      out.source_adapter_path = names.get(p.sourceInfo.adapterId);
      out.source_id = p.sourceInfo.id;
      out.target_id = p.targetInfo.id;
      read_target_name(p, out);
      read_source_name(p, out);

      if (p.sourceInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && p.sourceInfo.modeInfoIdx < cfg->modes.size()) {
        const auto &m = cfg->modes[p.sourceInfo.modeInfoIdx];
        if (m.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
          out.width = m.sourceMode.width;
          out.height = m.sourceMode.height;
          out.position_x = m.sourceMode.position.x;
          out.position_y = m.sourceMode.position.y;
          out.pixel_format = static_cast<std::uint32_t>(m.sourceMode.pixelFormat);
        }
      }
      if (p.targetInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && p.targetInfo.modeInfoIdx < cfg->modes.size()) {
        const auto &m = cfg->modes[p.targetInfo.modeInfoIdx];
        if (m.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) {
          const auto &s = m.targetMode.targetVideoSignalInfo;
          target_timing_t t;
          t.pixel_rate = s.pixelRate;
          t.h_sync = {s.hSyncFreq.Numerator, s.hSyncFreq.Denominator};
          t.v_sync = {s.vSyncFreq.Numerator, s.vSyncFreq.Denominator};
          t.active_width = s.activeSize.cx;
          t.active_height = s.activeSize.cy;
          t.total_width = s.totalSize.cx;
          t.total_height = s.totalSize.cy;
          t.video_standard = s.videoStandard;
          t.scanline_ordering = static_cast<std::uint32_t>(s.scanLineOrdering);
          out.timing = t;
        }
      }
      out.refresh_rate = {p.targetInfo.refreshRate.Numerator, p.targetInfo.refreshRate.Denominator};
      out.rotation = static_cast<std::uint32_t>(p.targetInfo.rotation);
      out.scaling = static_cast<std::uint32_t>(p.targetInfo.scaling);
      out.scanline_ordering = static_cast<std::uint32_t>(p.targetInfo.scanLineOrdering);

      const auto hdr = read_hdr(p.targetInfo.adapterId, p.targetInfo.id);
      out.hdr_supported = hdr.supported;
      out.hdr_enabled = hdr.enabled;
      topo.paths.push_back(std::move(out));
    }
    return topo;
  }

  result_t<std::size_t> active_path_count() {
    auto cfg = query(QDC_ONLY_ACTIVE_PATHS);
    if (!cfg) {
      return cfg.error();
    }
    return static_cast<std::size_t>(std::count_if(cfg->paths.begin(), cfg->paths.end(), [](const auto &p) {
      return (p.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
    }));
  }

  std::vector<std::string> compare(const topology_t &expected, const topology_t &actual) {
    std::vector<std::string> diffs;
    for (const auto &e : expected.paths) {
      const auto it = std::find_if(actual.paths.begin(), actual.paths.end(), [&](const path_t &a) {
        return same_identity(e, a);
      });
      const std::string label = path_label(e);
      if (it == actual.paths.end()) {
        diffs.push_back(label + ": expected active, is inactive");
        continue;
      }
      const auto &a = *it;
      if (e.width != a.width || e.height != a.height) {
        diffs.push_back(label + ": resolution " + std::to_string(a.width) + "x" + std::to_string(a.height) + ", expected " + std::to_string(e.width) + "x" + std::to_string(e.height));
      }
      if (e.position_x != a.position_x || e.position_y != a.position_y) {
        diffs.push_back(label + ": position (" + std::to_string(a.position_x) + "," + std::to_string(a.position_y) + "), expected (" + std::to_string(e.position_x) + "," + std::to_string(e.position_y) + ")");
      }
      if (std::fabs(hz(e.refresh_rate) - hz(a.refresh_rate)) > 0.001) {
        std::ostringstream s;
        s.precision(6);
        s << label << ": refresh " << hz(a.refresh_rate) << " Hz, expected " << hz(e.refresh_rate) << " Hz";
        diffs.push_back(s.str());
      }
      if (e.rotation != a.rotation) {
        diffs.push_back(label + ": rotation " + std::to_string(a.rotation) + ", expected " + std::to_string(e.rotation));
      }
      if (e.scaling != a.scaling) {
        diffs.push_back(label + ": scaling " + std::to_string(a.scaling) + ", expected " + std::to_string(e.scaling));
      }
      if (e.hdr_supported && a.hdr_supported && e.hdr_enabled != a.hdr_enabled) {
        diffs.push_back(label + ": HDR " + (a.hdr_enabled ? "on" : "off") + ", expected " + (e.hdr_enabled ? "on" : "off"));
      }
    }
    for (const auto &a : actual.paths) {
      const bool expected_active = std::any_of(expected.paths.begin(), expected.paths.end(), [&](const path_t &e) {
        return same_identity(e, a);
      });
      if (!expected_active) {
        diffs.push_back(path_label(a) + ": expected inactive, is active");
      }
    }
    return diffs;
  }

  namespace {

    struct match_t {
      DISPLAYCONFIG_PATH_INFO path;
      const path_t *wanted;
    };

    status_t set_config(std::vector<DISPLAYCONFIG_PATH_INFO> &paths, std::vector<DISPLAYCONFIG_MODE_INFO> &modes, UINT32 flags, const char *where) {
      const LONG rc = SetDisplayConfig(static_cast<UINT32>(paths.size()), paths.data(), static_cast<UINT32>(modes.size()), modes.data(), flags);
      if (rc != ERROR_SUCCESS) {
        return win32::win32_error(static_cast<DWORD>(rc), where);
      }
      return status_t::ok();
    }

    /// Builds the SetDisplayConfig arrays for `matches`; with_timing=false leaves target timing to Windows.
    void build_arrays(const std::vector<match_t> &matches, bool with_timing, std::vector<DISPLAYCONFIG_PATH_INFO> &paths, std::vector<DISPLAYCONFIG_MODE_INFO> &modes) {
      paths.clear();
      modes.clear();
      std::map<std::string, UINT32> source_mode_index;  // clones share one source mode
      for (const auto &m : matches) {
        DISPLAYCONFIG_PATH_INFO p = m.path;
        const path_t &w = *m.wanted;
        p.flags = DISPLAYCONFIG_PATH_ACTIVE;
        p.targetInfo.rotation = static_cast<DISPLAYCONFIG_ROTATION>(w.rotation);
        p.targetInfo.scaling = static_cast<DISPLAYCONFIG_SCALING>(w.scaling);
        p.targetInfo.refreshRate = {w.refresh_rate.numerator, w.refresh_rate.denominator};
        p.targetInfo.scanLineOrdering = static_cast<DISPLAYCONFIG_SCANLINE_ORDERING>(w.scanline_ordering);

        const std::string skey = luid_key(p.sourceInfo.adapterId) + "/" + std::to_string(p.sourceInfo.id);
        if (const auto it = source_mode_index.find(skey); it != source_mode_index.end()) {
          p.sourceInfo.modeInfoIdx = it->second;
        } else {
          DISPLAYCONFIG_MODE_INFO sm {};
          sm.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
          sm.id = p.sourceInfo.id;
          sm.adapterId = p.sourceInfo.adapterId;
          sm.sourceMode.width = w.width;
          sm.sourceMode.height = w.height;
          sm.sourceMode.pixelFormat = static_cast<DISPLAYCONFIG_PIXELFORMAT>(w.pixel_format);
          sm.sourceMode.position.x = w.position_x;
          sm.sourceMode.position.y = w.position_y;
          const auto idx = static_cast<UINT32>(modes.size());
          modes.push_back(sm);
          source_mode_index.emplace(skey, idx);
          p.sourceInfo.modeInfoIdx = idx;
        }

        if (with_timing && w.timing) {
          const auto &t = *w.timing;
          DISPLAYCONFIG_MODE_INFO tm {};
          tm.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;
          tm.id = p.targetInfo.id;
          tm.adapterId = p.targetInfo.adapterId;
          auto &s = tm.targetMode.targetVideoSignalInfo;
          s.pixelRate = t.pixel_rate;
          s.hSyncFreq = {t.h_sync.numerator, t.h_sync.denominator};
          s.vSyncFreq = {t.v_sync.numerator, t.v_sync.denominator};
          s.activeSize = {t.active_width, t.active_height};
          s.totalSize = {t.total_width, t.total_height};
          s.videoStandard = t.video_standard;
          s.scanLineOrdering = static_cast<DISPLAYCONFIG_SCANLINE_ORDERING>(t.scanline_ordering);
          p.targetInfo.modeInfoIdx = static_cast<UINT32>(modes.size());
          modes.push_back(tm);
        } else {
          p.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        }
        paths.push_back(p);
      }
    }

  }  // namespace

  status_t apply(const topology_t &wanted, const restore_options_t &options) {
    if (wanted.paths.empty()) {
      return make_error(tessera_code_e::unsafe, "display::apply", "refusing to apply a topology with no active display");
    }
    auto all = query(QDC_ALL_PATHS);
    if (!all) {
      return all.error();
    }
    adapter_names_t names;
    std::vector<match_t> matches;
    std::vector<std::string> missing;
    for (const auto &w : wanted.paths) {
      const DISPLAYCONFIG_PATH_INFO *found = nullptr;
      for (const auto &p : all->paths) {
        if (p.sourceInfo.id == w.source_id && p.targetInfo.id == w.target_id &&
            names.get(p.targetInfo.adapterId) == w.adapter_path && names.get(p.sourceInfo.adapterId) == w.source_adapter_path) {
          found = &p;
          break;
        }
      }
      if (found == nullptr) {
        missing.push_back(path_label(w) + " is not connected (adapter " + w.adapter_path + ", target " + std::to_string(w.target_id) + ")");
        continue;
      }
      if (!found->targetInfo.targetAvailable) {
        missing.push_back(path_label(w) + " is not available (monitor unplugged or off?)");
        continue;
      }
      path_t probe;
      read_target_name(*found, probe);
      if (!w.monitor_path.empty() && !probe.monitor_path.empty() && probe.monitor_path != w.monitor_path) {
        missing.push_back(path_label(w) + ": a different monitor is connected there now (" + probe.monitor_name + ")");
        continue;
      }
      matches.push_back({*found, &w});
    }
    if (!missing.empty()) {
      std::string msg = "cannot restore exactly:";
      for (const auto &m : missing) {
        msg += " " + m + ";";
      }
      return make_error(tessera_code_e::not_found, "display::apply", msg);
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    constexpr UINT32 apply_flags = SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE | SDC_ALLOW_CHANGES;

    // 1) exact timing, 2) Windows picks the timing for the saved resolution/refresh.
    status_t st;
    for (const bool with_timing : {true, false}) {
      build_arrays(matches, with_timing, paths, modes);
      st = set_config(paths, modes, SDC_VALIDATE | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES, "SetDisplayConfig(validate)");
      if (st) {
        st = set_config(paths, modes, apply_flags, "SetDisplayConfig(apply)");
      }
      if (st) {
        break;
      }
    }
    if (!st) {
      return st;
    }

    if (options.restore_hdr) {
      // Re-query: LUIDs are the same, but HDR can only be toggled on active targets.
      for (const auto &m : matches) {
        if (!m.wanted->hdr_supported) {
          continue;
        }
        const auto now = read_hdr(m.path.targetInfo.adapterId, m.path.targetInfo.id);
        if (now.supported && now.enabled != m.wanted->hdr_enabled) {
          if (auto hst = write_hdr(m.path.targetInfo.adapterId, m.path.targetInfo.id, m.wanted->hdr_enabled, path_label(*m.wanted)); !hst) {
            return hst;
          }
        }
      }
    }

    // Windows applies asynchronously in places (HDR, mode database); verify for up to ~2 s.
    std::vector<std::string> diffs;
    for (int i = 0; i < 10; ++i) {
      auto now = capture();
      if (!now) {
        return now.error();
      }
      diffs = compare(wanted, now.value());
      if (diffs.empty()) {
        return status_t::ok();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::string msg = "applied, but the result differs:";
    for (const auto &d : diffs) {
      msg += " " + d + ";";
    }
    return make_error(tessera_code_e::invalid_data, "display::apply(verify)", msg);
  }

  status_t apply_database_current() {
    const LONG rc = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_USE_DATABASE_CURRENT);
    if (rc != ERROR_SUCCESS) {
      return win32::win32_error(static_cast<DWORD>(rc), "SetDisplayConfig(SDC_USE_DATABASE_CURRENT)");
    }
    return status_t::ok();
  }

  json to_json(const topology_t &t) {
    json paths = json::array();
    for (const auto &p : t.paths) {
      json jp {
        {"adapter_path", p.adapter_path},
        {"source_adapter_path", p.source_adapter_path},
        {"source_id", p.source_id},
        {"target_id", p.target_id},
        {"monitor_path", p.monitor_path},
        {"monitor_name", p.monitor_name},
        {"gdi_name", p.gdi_name},
        {"width", p.width},
        {"height", p.height},
        {"position", json::array({p.position_x, p.position_y})},
        {"pixel_format", p.pixel_format},
        {"refresh_rate", rational_json(p.refresh_rate)},
        {"rotation", p.rotation},
        {"scaling", p.scaling},
        {"scanline_ordering", p.scanline_ordering},
        {"output_technology", p.output_technology},
        {"hdr_supported", p.hdr_supported},
        {"hdr_enabled", p.hdr_enabled},
      };
      if (p.timing) {
        const auto &tm = *p.timing;
        jp["timing"] = json {
          {"pixel_rate", tm.pixel_rate},
          {"h_sync", rational_json(tm.h_sync)},
          {"v_sync", rational_json(tm.v_sync)},
          {"active", json::array({tm.active_width, tm.active_height})},
          {"total", json::array({tm.total_width, tm.total_height})},
          {"video_standard", tm.video_standard},
          {"scanline_ordering", tm.scanline_ordering},
        };
      }
      paths.push_back(std::move(jp));
    }
    return json {{"paths", std::move(paths)}};
  }

  result_t<topology_t> from_json(const json &j) {
    auto bad = [](std::string what) {
      return make_error(tessera_code_e::invalid_data, "display::from_json", std::move(what));
    };
    try {
      if (!j.is_object() || !j.contains("paths") || !j["paths"].is_array()) {
        return bad("missing paths array");
      }
      if (j["paths"].empty() || j["paths"].size() > 64) {
        return bad("paths must hold 1 to 64 entries");
      }
      topology_t t;
      for (const auto &jp : j["paths"]) {
        if (!jp.is_object()) {
          return bad("path is not an object");
        }
        auto str = [&](const char *k, std::string &out) {
          if (!jp.contains(k) || !jp[k].is_string() || jp[k].get_ref<const std::string &>().size() > 512) {
            return false;
          }
          out = jp[k].get<std::string>();
          return true;
        };
        auto u32 = [&](const char *k, std::uint32_t &out) {
          if (!jp.contains(k) || !jp[k].is_number_unsigned() || jp[k].get<std::uint64_t>() > 0xffffffffull) {
            return false;
          }
          out = jp[k].get<std::uint32_t>();
          return true;
        };
        path_t p;
        bool ok = str("adapter_path", p.adapter_path) && str("source_adapter_path", p.source_adapter_path) &&
                  str("monitor_path", p.monitor_path) && str("monitor_name", p.monitor_name) && str("gdi_name", p.gdi_name) &&
                  u32("source_id", p.source_id) && u32("target_id", p.target_id) && u32("width", p.width) &&
                  u32("height", p.height) && u32("pixel_format", p.pixel_format) && u32("rotation", p.rotation) &&
                  u32("scaling", p.scaling) && u32("scanline_ordering", p.scanline_ordering) &&
                  u32("output_technology", p.output_technology);
        if (!ok || p.adapter_path.empty()) {
          return bad("path has a missing or invalid field");
        }
        if (p.width == 0 || p.height == 0 || p.width > 32768 || p.height > 32768) {
          return bad("path resolution out of range");
        }
        const auto &pos = jp.value("position", json());
        if (!pos.is_array() || pos.size() != 2 || !pos[0].is_number_integer() || !pos[1].is_number_integer()) {
          return bad("path position invalid");
        }
        const auto px = pos[0].get<std::int64_t>();
        const auto py = pos[1].get<std::int64_t>();
        if (px < -65536 || px > 65536 || py < -65536 || py > 65536) {
          return bad("path position out of range");
        }
        p.position_x = static_cast<std::int32_t>(px);
        p.position_y = static_cast<std::int32_t>(py);
        if (!read_rational(jp.value("refresh_rate", json()), p.refresh_rate)) {
          return bad("path refresh_rate invalid");
        }
        if (!jp.contains("hdr_supported") || !jp["hdr_supported"].is_boolean() || !jp.contains("hdr_enabled") || !jp["hdr_enabled"].is_boolean()) {
          return bad("path hdr flags invalid");
        }
        p.hdr_supported = jp["hdr_supported"].get<bool>();
        p.hdr_enabled = jp["hdr_enabled"].get<bool>();
        if (jp.contains("timing") && !jp["timing"].is_null()) {
          const auto &jt = jp["timing"];
          target_timing_t tm;
          const auto &act = jt.value("active", json());
          const auto &tot = jt.value("total", json());
          if (!jt.is_object() || !jt.contains("pixel_rate") || !jt["pixel_rate"].is_number_unsigned() ||
              !read_rational(jt.value("h_sync", json()), tm.h_sync) || !read_rational(jt.value("v_sync", json()), tm.v_sync) ||
              !act.is_array() || act.size() != 2 || !tot.is_array() || tot.size() != 2 ||
              !jt.contains("video_standard") || !jt["video_standard"].is_number_unsigned() ||
              !jt.contains("scanline_ordering") || !jt["scanline_ordering"].is_number_unsigned()) {
            return bad("path timing invalid");
          }
          tm.pixel_rate = jt["pixel_rate"].get<std::uint64_t>();
          tm.active_width = act[0].get<std::uint32_t>();
          tm.active_height = act[1].get<std::uint32_t>();
          tm.total_width = tot[0].get<std::uint32_t>();
          tm.total_height = tot[1].get<std::uint32_t>();
          tm.video_standard = jt["video_standard"].get<std::uint32_t>();
          tm.scanline_ordering = jt["scanline_ordering"].get<std::uint32_t>();
          p.timing = tm;
        }
        t.paths.push_back(std::move(p));
      }
      const bool has_primary = std::any_of(t.paths.begin(), t.paths.end(), [](const path_t &p) {
        return p.is_primary();
      });
      if (!has_primary) {
        return bad("no path at desktop position (0,0), so there would be no primary display");
      }
      return t;
    } catch (const std::exception &ex) {
      return bad(std::string("exception: ") + ex.what());
    }
  }

  std::string describe(const topology_t &t) {
    std::ostringstream s;
    s.precision(6);
    for (const auto &p : t.paths) {
      s << path_label(p) << ": " << p.width << "x" << p.height << " @ " << hz(p.refresh_rate) << " Hz at (" << p.position_x << "," << p.position_y << ")"
        << (p.is_primary() ? " primary" : "") << " rot=" << p.rotation << " scale=" << p.scaling
        << " hdr=" << (p.hdr_supported ? (p.hdr_enabled ? "on" : "off") : "n/a") << " [src " << p.source_id << ", tgt " << p.target_id << "]\n";
    }
    return s.str();
  }

}  // namespace tessera::display
