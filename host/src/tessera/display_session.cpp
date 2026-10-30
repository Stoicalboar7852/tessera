/**
 * @file src/tessera/display_session.cpp
 * @brief [fork] Tessera: the virtual displays of a multistream session.
 */
// platform includes
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi1_6.h>

// standard includes
#include <algorithm>
#include <chrono>
#include <climits>
#include <optional>
#include <thread>
#include <tuple>

// local includes
#include "display_session.h"
#include "src/logging.h"
#include "tessera/display_topology.h"
#include "tessera/state_journal.h"
#include "tessera/vdd_client.h"
#include "tessera/win32.h"

using namespace std::literals;

namespace tessera::host {

  namespace {
    constexpr auto wait_timeout = 10s;
    constexpr auto settle_time = 1s;  ///< ADR 0006: let Windows finish before anything else changes the displays

    /// The GPU the displays render on: Sunshine's adapter_name if set, else the high-performance GPU.
    std::optional<LUID> render_adapter(const std::string &name) {
      IDXGIFactory6 *factory = nullptr;
      if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory6), reinterpret_cast<void **>(&factory)))) {
        return std::nullopt;
      }
      std::optional<LUID> luid;
      IDXGIAdapter1 *adapter = nullptr;
      if (name.empty()) {
        if (SUCCEEDED(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1), reinterpret_cast<void **>(&adapter)))) {
          DXGI_ADAPTER_DESC1 desc {};
          if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            luid = desc.AdapterLuid;
          }
          adapter->Release();
        }
      } else {
        const auto wanted = win32::to_wide(name);
        for (UINT i = 0; !luid && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
          DXGI_ADAPTER_DESC1 desc {};
          if (SUCCEEDED(adapter->GetDesc1(&desc)) && wanted == desc.Description) {
            luid = desc.AdapterLuid;
          }
          adapter->Release();
        }
      }
      factory->Release();
      return luid;
    }

    /// \\.\DISPLAYn of an active target, or nullopt if it isn't active.
    std::optional<std::string> active_gdi_name(const LUID &adapter, std::uint32_t target_id) {
      std::vector<DISPLAYCONFIG_PATH_INFO> paths;
      std::vector<DISPLAYCONFIG_MODE_INFO> modes;
      LONG status;
      do {
        UINT32 path_count = 0;
        UINT32 mode_count = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) {
          return std::nullopt;
        }
        paths.resize(path_count);
        modes.resize(mode_count);
        status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr);
        paths.resize(path_count);
      } while (status == ERROR_INSUFFICIENT_BUFFER);
      if (status != ERROR_SUCCESS) {
        return std::nullopt;
      }
      for (const auto &path : paths) {
        if (path.targetInfo.adapterId.LowPart != adapter.LowPart || path.targetInfo.adapterId.HighPart != adapter.HighPart || path.targetInfo.id != target_id) {
          continue;
        }
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source {};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS) {
          return win32::to_utf8(source.viewGdiDeviceName);
        }
      }
      return std::nullopt;
    }

    display::path_t *find_by_gdi(display::topology_t &topology, const std::string &gdi_name) {
      const auto it = std::ranges::find(topology.paths, gdi_name, &display::path_t::gdi_name);
      return it == topology.paths.end() ? nullptr : &*it;
    }
  }  // namespace

  struct display_session_t::impl_t {
    std::optional<vdd::vdd_client_t> vdd;
    std::vector<vdd::added_monitor_t> added;
    display::topology_t original;
    std::optional<state::state_journal_t> journal;
    std::string journal_id;
    bool reverted = false;
  };

  display_session_t::display_session_t():
      impl_ {std::make_unique<impl_t>()} {
  }

  display_session_t::~display_session_t() {
    revert();
  }

  result_t<std::shared_ptr<display_session_t>> display_session_t::create(const std::vector<multistream::client_display_t> &clients, const options_t &options) {
    std::shared_ptr<display_session_t> session {new display_session_t()};
    auto &impl = *session->impl_;
    session->clients_ = clients;

    auto vdd = vdd::vdd_client_t::open();
    if (!vdd) {
      return vdd.error();
    }
    impl.vdd.emplace(std::move(vdd).value());

    if (const auto adapter = render_adapter(options.adapter_name)) {
      if (auto st = impl.vdd->set_render_adapter(*adapter); !st) {
        BOOST_LOG(warning) << "Multistream: couldn't set the render adapter: "sv << st.error().to_string();
      }
    } else {
      BOOST_LOG(warning) << "Multistream: render adapter not found; Windows chooses"sv;
    }

    // Journal the topology without the new displays: that is what a crash restore must bring back
    auto original = display::capture();
    if (!original) {
      return original.error();
    }
    impl.original = std::move(original).value();
    auto journal_path = state::state_journal_t::default_path();
    if (!journal_path) {
      return journal_path.error();
    }
    impl.journal.emplace(state::state_journal_t::options_t {journal_path.value()});
    auto id = impl.journal->begin("display_topology", "tessera-host multistream", display::to_json(impl.original));
    if (!id) {
      return id.error();
    }
    impl.journal_id = id.value();

    // All displays in one request (ADR 0006)
    std::vector<vdd::monitor_spec_t> specs;
    for (std::size_t i = 0; i < clients.size(); ++i) {
      const auto &client = clients[i];
      auto monitor_id = multistream::monitor_id(options.client_cert, client.key);
      if (!monitor_id) {
        return monitor_id.error();
      }
      vdd::monitor_spec_t spec;
      std::copy(monitor_id->begin(), monitor_id->end(), spec.id.begin());
      spec.width = client.width;
      spec.height = client.height;
      spec.refresh_numerator = client.refresh.numerator;
      spec.refresh_denominator = client.refresh.denominator;
      spec.hdr = client.hdr;
      std::tie(spec.width_mm, spec.height_mm) = multistream::physical_size_mm(client.width, client.height, client.dpi);
      spec.name = win32::to_wide((options.client_name.empty() ? "Client"s : options.client_name).substr(0, 24) + '-' + std::to_string(i + 1));
      specs.push_back(std::move(spec));
    }
    auto added = impl.vdd->add(specs);
    if (!added) {
      return added.error();
    }
    for (auto &result : added.value()) {
      if (!result) {
        return result.error();
      }
      impl.added.push_back(result.value());
    }

    // Wait until Windows has activated them all
    const auto deadline = std::chrono::steady_clock::now() + wait_timeout;
    session->displays_.assign(clients.size(), {});
    while (true) {
      bool all = true;
      for (std::size_t i = 0; i < impl.added.size(); ++i) {
        if (auto name = active_gdi_name(impl.added[i].adapter, impl.added[i].target_id)) {
          session->displays_[i] = std::move(*name);
        } else {
          all = false;
        }
      }
      if (all) {
        break;
      }
      if (std::chrono::steady_clock::now() > deadline) {
        return make_error(tessera_code_e::timeout, "display_session_t::create", "the virtual displays didn't become active");
      }
      std::this_thread::sleep_for(50ms);
    }

    // The client's layout, to the right of the existing desktop, attached to its rightmost display
    auto current = display::capture();
    if (!current) {
      return current.error();
    }
    auto wanted = std::move(current).value();
    const auto rightmost = std::ranges::max_element(impl.original.paths, {}, [](const display::path_t &p) {
      return static_cast<std::int64_t>(p.position_x) + p.width;
    });
    const auto rects = multistream::layout(clients);
    const auto leftmost_rect = std::ranges::min_element(rects, {}, &multistream::rect_t::x);
    const std::int32_t block_x = rightmost == impl.original.paths.end() ? 0 : rightmost->position_x + static_cast<std::int32_t>(rightmost->width);
    const std::int32_t block_y = rightmost == impl.original.paths.end() ? 0 : rightmost->position_y - leftmost_rect->y;

    for (auto &path : wanted.paths) {
      // Physical displays keep their place
      const auto it = std::ranges::find_if(impl.original.paths, [&path](const display::path_t &p) {
        return p.adapter_path == path.adapter_path && p.target_id == path.target_id;
      });
      if (it != impl.original.paths.end()) {
        path.position_x = it->position_x;
        path.position_y = it->position_y;
      }
    }
    for (std::size_t i = 0; i < rects.size(); ++i) {
      auto *path = find_by_gdi(wanted, session->displays_[i]);
      if (!path) {
        return make_error(tessera_code_e::not_found, "display_session_t::create", "virtual display " + session->displays_[i] + " not in the topology");
      }
      path->position_x = block_x + rects[i].x;
      path->position_y = block_y + rects[i].y;
    }
    if (options.make_primary) {
      // Windows' primary is the display at (0, 0)
      const auto *first = find_by_gdi(wanted, session->displays_[0]);
      const auto dx = first->position_x;
      const auto dy = first->position_y;
      for (auto &path : wanted.paths) {
        path.position_x -= dx;
        path.position_y -= dy;
      }
    }

    if (auto st = display::apply(wanted); !st) {
      return st.error();  // the destructor removes the displays and restores the topology
    }
    if (auto st = impl.journal->mark_applied(impl.journal_id, display::to_json(wanted)); !st) {
      BOOST_LOG(warning) << "Multistream: journal: "sv << st.error().to_string();
    }
    BOOST_LOG(info) << "Multistream: virtual displays ready:\n"sv << display::describe(wanted);

    std::this_thread::sleep_for(settle_time);
    return session;
  }

  void display_session_t::revert() {
    auto &impl = *impl_;
    if (impl.reverted) {
      return;
    }
    impl.reverted = true;

    if (impl.vdd && !impl.added.empty()) {
      std::vector<vdd::monitor_id_t> ids;
      for (const auto &added : impl.added) {
        ids.push_back(added.id);
      }
      if (auto removed = impl.vdd->remove(ids); !removed) {
        BOOST_LOG(error) << "Multistream: removing the virtual displays failed: "sv << removed.error().to_string();
      }
      const auto deadline = std::chrono::steady_clock::now() + wait_timeout;
      while (std::chrono::steady_clock::now() < deadline && std::ranges::any_of(impl.added, [](const vdd::added_monitor_t &a) {
               return active_gdi_name(a.adapter, a.target_id).has_value();
             })) {
        std::this_thread::sleep_for(50ms);
      }
    }

    // Windows usually brings back the saved layout for the remaining monitors by itself; only change it if not
    if (!impl.original.paths.empty()) {
      auto current = display::capture();
      if (current && !display::compare(impl.original, current.value()).empty()) {
        std::this_thread::sleep_for(settle_time);
        if (auto st = display::apply(impl.original); !st) {
          BOOST_LOG(error) << "Multistream: restoring the display topology failed: "sv << st.error().to_string();
          if (auto fallback = display::apply_database_current(); !fallback) {
            BOOST_LOG(error) << "Multistream: fallback failed: "sv << fallback.error().to_string();
          }
        }
      }
    }
    if (impl.journal && !impl.journal_id.empty()) {
      if (auto st = impl.journal->remove(impl.journal_id); !st) {
        BOOST_LOG(warning) << "Multistream: journal entry kept: "sv << st.error().to_string();
      }
    }
    impl.vdd.reset();
    BOOST_LOG(info) << "Multistream: virtual displays removed"sv;
  }

}  // namespace tessera::host
