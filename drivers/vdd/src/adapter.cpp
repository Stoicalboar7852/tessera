/**
 * @file drivers/vdd/src/adapter.cpp
 * @brief Monitor table, control requests and IddCx monitor callbacks (see adapter.h).
 */
#include "adapter.h"

#include <cstring>

#include "trace.h"

namespace tessera::vdd {

  namespace {

    /// Combined display bandwidth reported to IddCx; RequiredBandwidth of a mode is its pixel clock in Hz.
    /// 2^40 Hz is far above 16 monitors at 8K/240, so the OS never rejects a combination for bandwidth.
    constexpr UINT64 max_pipeline_rate = 1ull << 40;

    IDDCX_ENDPOINT_VERSION g_endpoint_version = {sizeof(IDDCX_ENDPOINT_VERSION), 0, 2, 0, 0};

    void fill_signal(const modes::timing_t &t, DISPLAYCONFIG_VIDEO_SIGNAL_INFO &s, UINT32 divider) {
      s.pixelRate = t.pixel_clock_hz;
      // Line rate = refresh × total lines, kept as an exact fraction.
      UINT64 num = static_cast<UINT64>(t.refresh.numerator) * t.v_total();
      UINT64 den = t.refresh.denominator;
      while (num > 0xffffffffull) {
        num /= 2;
        den = den > 1 ? den / 2 : 1;
      }
      s.hSyncFreq.Numerator = static_cast<UINT32>(num);
      s.hSyncFreq.Denominator = static_cast<UINT32>(den);
      s.vSyncFreq.Numerator = t.refresh.numerator;
      s.vSyncFreq.Denominator = t.refresh.denominator;
      s.activeSize.cx = t.width;
      s.activeSize.cy = t.height;
      s.totalSize.cx = t.h_total();
      s.totalSize.cy = t.v_total();
      s.AdditionalSignalInfo.videoStandard = 255;  // D3DKMDT_VSS_OTHER
      s.AdditionalSignalInfo.vSyncFreqDivider = divider;  // 0 for monitor modes, 1 for target modes
      s.AdditionalSignalInfo.reserved = 0;
      s.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    }

    IDDCX_WIRE_BITS_PER_COMPONENT wire_bits(bool hdr) {
      IDDCX_WIRE_BITS_PER_COMPONENT b = {};
      b.Rgb = hdr ? (IDDCX_BITS_PER_COMPONENT_8 | IDDCX_BITS_PER_COMPONENT_10) : IDDCX_BITS_PER_COMPONENT_8;
      b.YCbCr444 = IDDCX_BITS_PER_COMPONENT_NONE;
      b.YCbCr422 = IDDCX_BITS_PER_COMPONENT_NONE;
      b.YCbCr420 = IDDCX_BITS_PER_COMPONENT_NONE;
      return b;
    }

    /// A container ID derived from the monitor ID (RFC 9562 version 8 "custom" UUID), so Windows keeps grouping
    /// the same client monitor under the same container.
    GUID container_id(const unsigned char (&id)[16]) {
      static const unsigned char ns[16] = {0x7e, 0x55, 0xe2, 0xa0, 0x5d, 0x0b, 0x4c, 0x7e, 0x9a, 0x31, 0x2f, 0x6d, 0x18, 0xc4, 0x90, 0x3b};
      unsigned char b[16];
      for (int i = 0; i < 16; ++i) {
        b[i] = static_cast<unsigned char>(id[i] ^ ns[i]);
      }
      GUID g;
      memcpy(&g, b, sizeof(g));
      g.Data3 = static_cast<USHORT>((g.Data3 & 0x0fff) | 0x8000);
      g.Data4[0] = static_cast<UCHAR>((g.Data4[0] & 0x3f) | 0x80);
      return g;
    }

    bool all_zero(const unsigned char (&id)[16]) {
      for (const unsigned char c : id) {
        if (c != 0) {
          return false;
        }
      }
      return true;
    }

    void evt_monitor_cleanup(WDFOBJECT object) {
      const monitor_context_t *ctx = get_monitor_context(object);
      if (ctx != nullptr && ctx->adapter != nullptr) {
        ctx->adapter->monitor_destroyed(*ctx);
      }
    }

    void evt_orphan_timer(WDFTIMER timer) {
      auto *ctx = get_device_context(WdfTimerGetParentObject(timer));
      if (ctx != nullptr && ctx->adapter != nullptr) {
        ctx->adapter->expire_orphans();
      }
    }

  }  // namespace

  adapter_t::adapter_t(WDFDEVICE device):
      device_(device),
      adapter_(nullptr),
      adapter_ready_(false),
      hdr_capable_(IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxAdapterQueryTargetInfo)),
      iddcx_version_(0),
      orphan_timer_(nullptr),
      op_lock_(SRWLOCK_INIT),
      table_lock_(SRWLOCK_INIT),
      monitors_(),
      render_adapter_set_(false),
      render_adapter_() {
    for (auto &m : monitors_) {
      m.state = state_e::free;
      m.generation = 0;
    }
  }

  adapter_t::~adapter_t() {
    // IddCx unassigns every swap chain before the adapter goes away; stop anything left without touching the
    // (already destroyed) swap chains.
    for (auto &m : monitors_) {
      if (m.state != state_e::free && m.worker != nullptr) {
        m.worker->stop(false);
        m.worker = nullptr;
      }
    }
  }

  NTSTATUS adapter_t::initialize() {
    // IddCxGetVersion is part of the IddCx 1.4 baseline this driver requires.
    IDARG_OUT_GETVERSION v = {};
    if (NT_SUCCESS(IddCxGetVersion(&v))) {
      iddcx_version_ = v.IddCxVersion;
    }
    WDF_TIMER_CONFIG config;
    WDF_TIMER_CONFIG_INIT(&config, evt_orphan_timer);
    config.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device_;
    const NTSTATUS status = WdfTimerCreate(&config, &attributes, &orphan_timer_);
    log_status("WdfTimerCreate", status);
    TraceLoggingWrite(g_trace, "AdapterCreated", TraceLoggingUInt32(iddcx_version_, "IddCxVersion"), TraceLoggingBool(hdr_capable_, "HdrCapable"));
    return status;
  }

  NTSTATUS adapter_t::init_adapter() {
    if (adapter_ != nullptr) {
      return STATUS_SUCCESS;  // back from a low-power state; the adapter already exists
    }
    // IddCxAdapterInitAsync keeps using the caps until AdapterInitFinished, so they must outlive this call.
    static IDDCX_ADAPTER_CAPS caps;
    RtlZeroMemory(&caps, sizeof(caps));
    caps.Size = sizeof(caps);
    caps.Flags = hdr_capable_ ? IDDCX_ADAPTER_FLAGS_CAN_PROCESS_FP16 : IDDCX_ADAPTER_FLAGS_NONE;
    caps.MaxDisplayPipelineRate = max_pipeline_rate;
    caps.MaxMonitorsSupported = max_monitors;
    caps.EndPointDiagnostics.Size = sizeof(caps.EndPointDiagnostics);
    caps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;
    caps.EndPointDiagnostics.pEndPointFriendlyName = L"Tessera Virtual Display";
    caps.EndPointDiagnostics.pEndPointManufacturerName = L"Tessera";
    caps.EndPointDiagnostics.pEndPointModelName = L"Tessera VDD";
    caps.EndPointDiagnostics.pFirmwareVersion = &g_endpoint_version;
    caps.EndPointDiagnostics.pHardwareVersion = &g_endpoint_version;

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, device_context_t);
    IDARG_IN_ADAPTER_INIT in = {};
    in.WdfDevice = device_;
    in.pCaps = &caps;
    in.ObjectAttributes = &attributes;
    IDARG_OUT_ADAPTER_INIT out = {};
    const NTSTATUS status = IddCxAdapterInitAsync(&in, &out);
    log_status("IddCxAdapterInitAsync", status);
    if (NT_SUCCESS(status)) {
      adapter_ = out.AdapterObject;
      get_device_context(adapter_)->adapter = this;
    }
    return status;
  }

  NTSTATUS adapter_t::adapter_init_finished(IDDCX_ADAPTER adapter, NTSTATUS init_status) {
    log_status("AdapterInitFinished", init_status);
    if (!NT_SUCCESS(init_status)) {
      return init_status;
    }
    AcquireSRWLockExclusive(&op_lock_);
    adapter_ready_ = true;
    if (render_adapter_set_ && IDD_IS_FUNCTION_AVAILABLE(IddCxAdapterSetRenderAdapter)) {
      IDARG_IN_ADAPTERSETRENDERADAPTER in = {};
      in.PreferredRenderAdapter = render_adapter_;
      IddCxAdapterSetRenderAdapter(adapter, &in);
    }
    ReleaseSRWLockExclusive(&op_lock_);
    return STATUS_SUCCESS;
  }

  // ---------------------------------------------------------------------------------------------------------------
  // Control requests
  // ---------------------------------------------------------------------------------------------------------------

  void adapter_t::get_version(tessera_vdd_version_t &out) {
    RtlZeroMemory(&out, sizeof(out));
    out.size = sizeof(out);
    out.interface_version = TESSERA_VDD_INTERFACE_VERSION;
    out.driver_major = g_endpoint_version.MajorVer;
    out.driver_minor = g_endpoint_version.MinorVer;
    out.driver_patch = g_endpoint_version.Build;
    out.max_monitors = max_monitors;
    out.iddcx_version = iddcx_version_;
    AcquireSRWLockShared(&table_lock_);
    for (const auto &m : monitors_) {
      out.active_monitors += m.state == state_e::active ? 1 : 0;
    }
    if (render_adapter_set_) {
      out.flags |= TESSERA_VDD_FLAG_RENDER_ADAPTER_SET;
      out.render_adapter_luid_low = render_adapter_.LowPart;
      out.render_adapter_luid_high = render_adapter_.HighPart;
    }
    ReleaseSRWLockShared(&table_lock_);
    if (hdr_capable_) {
      out.flags |= TESSERA_VDD_FLAG_HDR_CAPABLE;
    }
  }

  NTSTATUS adapter_t::set_render_adapter(const LUID &luid) {
    if (!IDD_IS_FUNCTION_AVAILABLE(IddCxAdapterSetRenderAdapter)) {
      return STATUS_NOT_SUPPORTED;
    }
    AcquireSRWLockExclusive(&op_lock_);
    AcquireSRWLockExclusive(&table_lock_);
    render_adapter_ = luid;
    render_adapter_set_ = true;
    // Swap chains on another GPU are dropped so Windows assigns new ones on the requested adapter.
    swapchain_worker_t *moved[max_monitors] = {};
    unsigned n = 0;
    for (auto &m : monitors_) {
      if (m.state != state_e::free && m.worker != nullptr && !same_luid(m.worker->render_adapter(), luid)) {
        moved[n++] = m.worker;
        m.worker = nullptr;
      }
    }
    ReleaseSRWLockExclusive(&table_lock_);
    if (adapter_ready_) {
      IDARG_IN_ADAPTERSETRENDERADAPTER in = {};
      in.PreferredRenderAdapter = luid;
      IddCxAdapterSetRenderAdapter(adapter_, &in);
    }
    ReleaseSRWLockExclusive(&op_lock_);
    for (unsigned i = 0; i < n; ++i) {
      moved[i]->stop(true);
    }
    TraceLoggingWrite(g_trace, "RenderAdapterSet", TraceLoggingUInt32(luid.LowPart, "LuidLow"), TraceLoggingInt32(luid.HighPart, "LuidHigh"), TraceLoggingUInt32(n, "SwapChainsMoved"));
    return STATUS_SUCCESS;
  }

  void adapter_t::add_monitors(WDFFILEOBJECT owner, const tessera_vdd_add_request_t &in, tessera_vdd_add_reply_t &out) {
    RtlZeroMemory(&out, sizeof(out));
    out.size = sizeof(out);
    out.count = in.count;
    AcquireSRWLockExclusive(&op_lock_);
    // All monitors of one request arrive back to back, so Windows re-evaluates the topology once per burst
    // instead of the host paying a full round trip per monitor.
    for (unsigned i = 0; i < in.count; ++i) {
      out.results[i].status = add_one(owner, in.monitors[i], out.results[i]);
    }
    ReleaseSRWLockExclusive(&op_lock_);
  }

  NTSTATUS adapter_t::add_one(WDFFILEOBJECT owner, const tessera_vdd_monitor_t &desc, tessera_vdd_add_result_t &result) {
    if (!adapter_ready_) {
      return STATUS_DEVICE_NOT_READY;
    }
    if (all_zero(desc.id)) {
      return STATUS_INVALID_PARAMETER;
    }
    const bool hdr = (desc.flags & TESSERA_VDD_MONITOR_HDR) != 0;
    if ((desc.flags & ~TESSERA_VDD_MONITOR_HDR) != 0) {
      return STATUS_INVALID_PARAMETER;
    }
    if (hdr && !hdr_capable_) {
      return STATUS_NOT_SUPPORTED;
    }
    modes::mode_list_t list;
    if (!modes::build_mode_list(desc.width, desc.height, {desc.refresh_numerator, desc.refresh_denominator}, list)) {
      return STATUS_INVALID_PARAMETER;
    }

    AcquireSRWLockExclusive(&table_lock_);
    const int existing = find_by_id(desc.id);
    if (existing >= 0) {
      monitor_t &m = monitors_[existing];
      NTSTATUS status = STATUS_DUPLICATE_OBJECTID;  // owned by another open handle, or being created/removed
      if (m.state == state_e::active && (m.owner == owner || m.owner == nullptr)) {
        // Re-claim: an orphan whose host came back, or a repeated add from the same handle. Kept as it is.
        m.owner = owner;
        m.orphaned_at = 0;
        result.target_id = m.target_id;
        result.adapter_luid_low = m.os_adapter.LowPart;
        result.adapter_luid_high = m.os_adapter.HighPart;
        result.flags = TESSERA_VDD_ADDED_RECLAIMED;
        status = STATUS_SUCCESS;
      }
      ReleaseSRWLockExclusive(&table_lock_);
      if (NT_SUCCESS(status)) {
        TraceLoggingWrite(g_trace, "MonitorReclaimed", TraceLoggingUInt32(static_cast<UINT32>(existing), "Slot"));
      }
      return status;
    }
    const modes::edid_identity_t identity = modes::identity_from_id(desc.id);
    unsigned slot = max_monitors;
    bool connector_used[max_monitors] = {};
    for (unsigned i = 0; i < max_monitors; ++i) {
      const monitor_t &m = monitors_[i];
      if (m.state == state_e::free) {
        slot = slot == max_monitors ? i : slot;
        continue;
      }
      connector_used[m.connector] = true;
      if (m.identity == identity) {
        ReleaseSRWLockExclusive(&table_lock_);
        return STATUS_DUPLICATE_OBJECTID;  // two IDs hash to the same EDID identity (48 bits): refuse the second
      }
    }
    if (slot == max_monitors) {
      ReleaseSRWLockExclusive(&table_lock_);
      return STATUS_TOO_MANY_NODES;
    }
    // A connector derived from the ID, so the same client monitor tends to come back on the same connector.
    unsigned connector = identity.serial % max_monitors;
    for (unsigned i = 0; connector_used[connector] && i < max_monitors; ++i) {
      connector = (connector + 1) % max_monitors;
    }
    monitor_t &m = monitors_[slot];
    m.state = state_e::creating;
    ++m.generation;
    m.desc = desc;
    m.modes = list;
    m.identity = identity;
    m.connector = connector;
    m.hdr = hdr;
    m.object = nullptr;
    m.owner = owner;
    m.orphaned_at = 0;
    m.target_id = 0;
    m.os_adapter = {};
    m.worker = nullptr;
    modes::edid_options_t options;
    options.identity = identity;
    modes::ascii_name(desc.name, TESSERA_VDD_NAME_CHARS, options.name);
    options.width_mm = desc.width_mm;
    options.height_mm = desc.height_mm;
    options.hdr = hdr;
    modes::build_edid(list, options, m.edid);
    const unsigned generation = m.generation;
    ReleaseSRWLockExclusive(&table_lock_);

    const ULONGLONG t0 = GetTickCount64();
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, monitor_context_t);
    attributes.EvtCleanupCallback = evt_monitor_cleanup;
    IDDCX_MONITOR_INFO info = {};
    info.Size = sizeof(info);
    info.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL;
    info.ConnectorIndex = connector;
    info.MonitorDescription.Size = sizeof(info.MonitorDescription);
    info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    info.MonitorDescription.DataSize = m.edid.size;
    info.MonitorDescription.pData = m.edid.bytes;
    info.MonitorContainerId = container_id(desc.id);
    IDARG_IN_MONITORCREATE create_in = {};
    create_in.ObjectAttributes = &attributes;
    create_in.pMonitorInfo = &info;
    IDARG_OUT_MONITORCREATE create_out = {};
    NTSTATUS status = IddCxMonitorCreate(adapter_, &create_in, &create_out);
    if (!NT_SUCCESS(status)) {
      log_status("IddCxMonitorCreate", status);
      AcquireSRWLockExclusive(&table_lock_);
      m.state = state_e::free;
      ReleaseSRWLockExclusive(&table_lock_);
      return status;
    }
    monitor_context_t *ctx = get_monitor_context(create_out.MonitorObject);
    ctx->adapter = this;
    ctx->slot = slot;
    ctx->generation = generation;
    AcquireSRWLockExclusive(&table_lock_);
    m.object = create_out.MonitorObject;
    ReleaseSRWLockExclusive(&table_lock_);

    IDARG_OUT_MONITORARRIVAL arrival = {};
    status = IddCxMonitorArrival(create_out.MonitorObject, &arrival);
    if (!NT_SUCCESS(status)) {
      log_status("IddCxMonitorArrival", status);
      AcquireSRWLockExclusive(&table_lock_);
      m.state = state_e::free;
      m.object = nullptr;
      ReleaseSRWLockExclusive(&table_lock_);
      WdfObjectDelete(create_out.MonitorObject);
      return status;
    }
    AcquireSRWLockExclusive(&table_lock_);
    m.state = state_e::active;
    m.target_id = arrival.OsTargetId;
    m.os_adapter = arrival.OsAdapterLuid;
    ReleaseSRWLockExclusive(&table_lock_);
    result.target_id = arrival.OsTargetId;
    result.adapter_luid_low = arrival.OsAdapterLuid.LowPart;
    result.adapter_luid_high = arrival.OsAdapterLuid.HighPart;
    result.flags = 0;
    TraceLoggingWrite(g_trace, "MonitorAdded", TraceLoggingUInt32(slot, "Slot"), TraceLoggingUInt32(connector, "Connector"), TraceLoggingUInt32(arrival.OsTargetId, "TargetId"), TraceLoggingUInt32(desc.width, "Width"), TraceLoggingUInt32(desc.height, "Height"), TraceLoggingUInt32(modes::refresh_mhz({desc.refresh_numerator, desc.refresh_denominator}), "RefreshMilliHz"), TraceLoggingBool(hdr, "Hdr"), TraceLoggingUInt64(GetTickCount64() - t0, "Milliseconds"));
    return STATUS_SUCCESS;
  }

  void adapter_t::remove_monitors(WDFFILEOBJECT owner, const tessera_vdd_remove_request_t &in, tessera_vdd_remove_reply_t &out) {
    RtlZeroMemory(&out, sizeof(out));
    out.size = sizeof(out);
    out.count = in.count;
    AcquireSRWLockExclusive(&op_lock_);
    for (unsigned i = 0; i < in.count; ++i) {
      AcquireSRWLockShared(&table_lock_);
      const int slot = find_by_id(in.ids[i]);
      NTSTATUS status = STATUS_SUCCESS;
      if (slot < 0) {
        status = STATUS_NOT_FOUND;
      } else if (monitors_[slot].owner != owner) {
        status = STATUS_ACCESS_DENIED;  // another handle's monitor, or an orphan waiting for its host
      } else if (monitors_[slot].state != state_e::active) {
        status = STATUS_DEVICE_BUSY;
      }
      ReleaseSRWLockShared(&table_lock_);
      if (NT_SUCCESS(status)) {
        status = remove_slot(static_cast<unsigned>(slot));
      }
      out.status[i] = status;
    }
    ReleaseSRWLockExclusive(&op_lock_);
  }

  NTSTATUS adapter_t::remove_slot(unsigned slot) {
    monitor_t &m = monitors_[slot];
    AcquireSRWLockExclusive(&table_lock_);
    IDDCX_MONITOR object = m.object;
    m.state = state_e::departing;
    ReleaseSRWLockExclusive(&table_lock_);

    const ULONGLONG t0 = GetTickCount64();
    const NTSTATUS status = IddCxMonitorDeparture(object);  // Windows unassigns the swap chain first
    log_status("IddCxMonitorDeparture", status);

    AcquireSRWLockExclusive(&table_lock_);
    swapchain_worker_t *worker = m.worker;
    m.worker = nullptr;
    m.state = state_e::free;
    m.object = nullptr;
    m.owner = nullptr;
    ReleaseSRWLockExclusive(&table_lock_);
    if (worker != nullptr) {
      // Not expected: the swap chain went away with the monitor, so it must not be deleted again.
      TraceLoggingWrite(g_trace, "WorkerSurvivedDeparture", TraceLoggingLevel(WINEVENT_LEVEL_WARNING), TraceLoggingUInt32(slot, "Slot"));
      worker->stop(false);
    }
    TraceLoggingWrite(g_trace, "MonitorRemoved", TraceLoggingUInt32(slot, "Slot"), TraceLoggingNTStatus(status, "Status"), TraceLoggingUInt64(GetTickCount64() - t0, "Milliseconds"));
    return status;
  }

  void adapter_t::list_monitors(WDFFILEOBJECT caller, tessera_vdd_list_reply_t &out) {
    RtlZeroMemory(&out, sizeof(out));
    out.size = sizeof(out);
    AcquireSRWLockShared(&table_lock_);
    for (const auto &m : monitors_) {
      if (m.state == state_e::free) {
        continue;
      }
      tessera_vdd_list_entry_t &e = out.entries[out.count++];
      e.monitor = m.desc;
      e.target_id = m.target_id;
      e.connector_index = m.connector;
      e.state = (m.owner != nullptr && m.owner == caller ? TESSERA_VDD_STATE_OWNED_BY_CALLER : 0u) | (m.owner == nullptr ? TESSERA_VDD_STATE_ORPHANED : 0u) | (m.worker != nullptr ? TESSERA_VDD_STATE_SWAPCHAIN : 0u);
    }
    ReleaseSRWLockShared(&table_lock_);
  }

  void adapter_t::file_cleanup(WDFFILEOBJECT file) {
    unsigned orphaned = 0;
    AcquireSRWLockExclusive(&table_lock_);
    const ULONGLONG now = GetTickCount64();
    for (auto &m : monitors_) {
      if (m.state != state_e::free && m.owner == file) {
        m.owner = nullptr;
        m.orphaned_at = now;
        ++orphaned;
      }
    }
    ReleaseSRWLockExclusive(&table_lock_);
    if (orphaned != 0) {
      TraceLoggingWrite(g_trace, "MonitorsOrphaned", TraceLoggingUInt32(orphaned, "Count"));
      schedule_orphan_timer();
    }
  }

  void adapter_t::expire_orphans() {
    AcquireSRWLockExclusive(&op_lock_);
    unsigned expired[max_monitors];
    unsigned n = 0;
    AcquireSRWLockShared(&table_lock_);
    const ULONGLONG now = GetTickCount64();
    for (unsigned i = 0; i < max_monitors; ++i) {
      const monitor_t &m = monitors_[i];
      if (m.state == state_e::active && m.owner == nullptr && m.orphaned_at != 0 && now - m.orphaned_at >= TESSERA_VDD_ORPHAN_GRACE_MS) {
        expired[n++] = i;
      }
    }
    ReleaseSRWLockShared(&table_lock_);
    for (unsigned i = 0; i < n; ++i) {
      remove_slot(expired[i]);
    }
    ReleaseSRWLockExclusive(&op_lock_);
    schedule_orphan_timer();  // for orphans that are not due yet
  }

  void adapter_t::schedule_orphan_timer() {
    if (orphan_timer_ == nullptr) {
      return;
    }
    ULONGLONG due = 0;
    bool any = false;
    AcquireSRWLockShared(&table_lock_);
    const ULONGLONG now = GetTickCount64();
    for (const auto &m : monitors_) {
      if (m.state == state_e::active && m.owner == nullptr && m.orphaned_at != 0) {
        const ULONGLONG deadline = m.orphaned_at + TESSERA_VDD_ORPHAN_GRACE_MS;
        const ULONGLONG left = deadline > now ? deadline - now : 0;
        due = any ? (left < due ? left : due) : left;
        any = true;
      }
    }
    ReleaseSRWLockShared(&table_lock_);
    if (any) {
      WdfTimerStart(orphan_timer_, WDF_REL_TIMEOUT_IN_MS(due + 10));
    }
  }

  int adapter_t::find_by_id(const unsigned char (&id)[16]) const {
    for (unsigned i = 0; i < max_monitors; ++i) {
      if (monitors_[i].state != state_e::free && memcmp(monitors_[i].desc.id, id, 16) == 0) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  adapter_t::monitor_t *adapter_t::slot_of(IDDCX_MONITOR monitor) {
    const monitor_context_t *ctx = get_monitor_context(monitor);
    if (ctx == nullptr || ctx->slot >= max_monitors) {
      return nullptr;
    }
    monitor_t &m = monitors_[ctx->slot];
    return (m.state != state_e::free && m.generation == ctx->generation) ? &m : nullptr;
  }

  // ---------------------------------------------------------------------------------------------------------------
  // IddCx callbacks
  // ---------------------------------------------------------------------------------------------------------------

  NTSTATUS adapter_t::parse_monitor_description(const IDDCX_MONITOR_DESCRIPTION &description, UINT capacity, IDDCX_MONITOR_MODE *modes_out, IDDCX_MONITOR_MODE2 *modes2_out, IDARG_OUT_PARSEMONITORDESCRIPTION &out) {
    modes::edid_identity_t identity;
    if (description.Type != IDDCX_MONITOR_DESCRIPTION_TYPE_EDID || !modes::read_identity(static_cast<const std::uint8_t *>(description.pData), description.DataSize, identity)) {
      return STATUS_INVALID_PARAMETER;
    }
    modes::mode_list_t list;
    bool hdr = false;
    bool found = false;
    AcquireSRWLockShared(&table_lock_);
    for (const auto &m : monitors_) {
      if (m.state != state_e::free && m.identity == identity) {
        list = m.modes;
        hdr = m.hdr;
        found = true;
        break;
      }
    }
    ReleaseSRWLockShared(&table_lock_);
    if (!found) {
      return STATUS_NOT_FOUND;
    }
    out.MonitorModeBufferOutputCount = list.count;
    out.PreferredMonitorModeIdx = 0;
    if (capacity == 0 || (modes_out == nullptr && modes2_out == nullptr)) {
      return STATUS_SUCCESS;
    }
    if (capacity < list.count) {
      return STATUS_BUFFER_TOO_SMALL;
    }
    for (unsigned i = 0; i < list.count; ++i) {
      if (modes2_out != nullptr) {
        IDDCX_MONITOR_MODE2 &mode = modes2_out[i];
        RtlZeroMemory(&mode, sizeof(mode));
        mode.Size = sizeof(mode);
        mode.Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
        fill_signal(list.modes[i], mode.MonitorVideoSignalInfo, 0);
        mode.BitsPerComponent = wire_bits(hdr);
      } else {
        IDDCX_MONITOR_MODE &mode = modes_out[i];
        RtlZeroMemory(&mode, sizeof(mode));
        mode.Size = sizeof(mode);
        mode.Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
        fill_signal(list.modes[i], mode.MonitorVideoSignalInfo, 0);
      }
    }
    return STATUS_SUCCESS;
  }

  NTSTATUS adapter_t::default_modes(IDDCX_MONITOR monitor, UINT capacity, IDDCX_MONITOR_MODE *modes_out, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES &out) {
    // Only called for monitors without a description; every Tessera monitor has an EDID, but answer anyway.
    AcquireSRWLockShared(&table_lock_);
    const monitor_t *m = slot_of(monitor);
    modes::mode_list_t list;
    if (m != nullptr) {
      list = m->modes;
    }
    ReleaseSRWLockShared(&table_lock_);
    if (m == nullptr) {
      return STATUS_NOT_FOUND;
    }
    out.DefaultMonitorModeBufferOutputCount = list.count;
    out.PreferredMonitorModeIdx = 0;
    if (capacity == 0 || modes_out == nullptr) {
      return STATUS_SUCCESS;
    }
    if (capacity < list.count) {
      return STATUS_BUFFER_TOO_SMALL;
    }
    for (unsigned i = 0; i < list.count; ++i) {
      RtlZeroMemory(&modes_out[i], sizeof(modes_out[i]));
      modes_out[i].Size = sizeof(modes_out[i]);
      modes_out[i].Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER;
      fill_signal(list.modes[i], modes_out[i].MonitorVideoSignalInfo, 0);
    }
    return STATUS_SUCCESS;
  }

  NTSTATUS adapter_t::query_target_modes(IDDCX_MONITOR monitor, UINT capacity, IDDCX_TARGET_MODE *modes_out, IDDCX_TARGET_MODE2 *modes2_out, IDARG_OUT_QUERYTARGETMODES &out) {
    AcquireSRWLockShared(&table_lock_);
    const monitor_t *m = slot_of(monitor);
    modes::mode_list_t list;
    bool hdr = false;
    if (m != nullptr) {
      list = m->modes;
      hdr = m->hdr;
    }
    ReleaseSRWLockShared(&table_lock_);
    if (m == nullptr) {
      return STATUS_NOT_FOUND;
    }
    // Target modes are exactly the monitor modes, so the OS's intersection is the same short list.
    out.TargetModeBufferOutputCount = list.count;
    if (capacity == 0 || (modes_out == nullptr && modes2_out == nullptr)) {
      return STATUS_SUCCESS;
    }
    if (capacity < list.count) {
      return STATUS_BUFFER_TOO_SMALL;
    }
    for (unsigned i = 0; i < list.count; ++i) {
      if (modes2_out != nullptr) {
        IDDCX_TARGET_MODE2 &mode = modes2_out[i];
        RtlZeroMemory(&mode, sizeof(mode));
        mode.Size = sizeof(mode);
        fill_signal(list.modes[i], mode.TargetVideoSignalInfo.targetVideoSignalInfo, 1);
        mode.RequiredBandwidth = list.modes[i].pixel_clock_hz;
        mode.BitsPerComponent = wire_bits(hdr);
      } else {
        IDDCX_TARGET_MODE &mode = modes_out[i];
        RtlZeroMemory(&mode, sizeof(mode));
        mode.Size = sizeof(mode);
        fill_signal(list.modes[i], mode.TargetVideoSignalInfo.targetVideoSignalInfo, 1);
        mode.RequiredBandwidth = list.modes[i].pixel_clock_hz;
      }
    }
    return STATUS_SUCCESS;
  }

  NTSTATUS adapter_t::query_target_info(UINT connector, IDARG_OUT_QUERYTARGET_INFO &out) {
    bool hdr = false;
    AcquireSRWLockShared(&table_lock_);
    for (const auto &m : monitors_) {
      if (m.state != state_e::free && m.connector == connector) {
        hdr = m.hdr;
        break;
      }
    }
    ReleaseSRWLockShared(&table_lock_);
    // Only HDR monitors advertise wide/high colour (SudoVDA advertises it on every target).
    out.TargetCaps = hdr ? (IDDCX_TARGET_CAPS_WIDE_COLOR_SPACE | IDDCX_TARGET_CAPS_HIGH_COLOR_SPACE) : IDDCX_TARGET_CAPS_NONE;
    out.DitheringSupport.Rgb = IDDCX_BITS_PER_COMPONENT_NONE;
    out.DitheringSupport.YCbCr444 = IDDCX_BITS_PER_COMPONENT_NONE;
    out.DitheringSupport.YCbCr422 = IDDCX_BITS_PER_COMPONENT_NONE;
    out.DitheringSupport.YCbCr420 = IDDCX_BITS_PER_COMPONENT_NONE;
    return STATUS_SUCCESS;
  }

  NTSTATUS adapter_t::assign_swapchain(IDDCX_MONITOR monitor, const IDARG_IN_SETSWAPCHAIN &in) {
    AcquireSRWLockExclusive(&table_lock_);
    monitor_t *m = slot_of(monitor);
    swapchain_worker_t *previous = nullptr;
    unsigned generation = 0;
    if (m != nullptr) {
      previous = m->worker;
      m->worker = nullptr;
      generation = m->generation;
    }
    const bool on_requested_adapter = !render_adapter_set_ || same_luid(in.RenderAdapterLuid, render_adapter_);
    ReleaseSRWLockExclusive(&table_lock_);
    if (m == nullptr) {
      WdfObjectDelete(in.hSwapChain);
      return STATUS_INVALID_PARAMETER;
    }
    if (previous != nullptr) {
      previous->stop(true);  // not expected: Windows unassigns before it assigns again
    }
    if (!on_requested_adapter) {
      TraceLoggingWrite(g_trace, "SwapChainOnOtherAdapter", TraceLoggingLevel(WINEVENT_LEVEL_WARNING), TraceLoggingUInt32(in.RenderAdapterLuid.LowPart, "LuidLow"), TraceLoggingInt32(in.RenderAdapterLuid.HighPart, "LuidHigh"));
    }
    const bool buffer2 = hdr_capable_ && IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2);
    swapchain_worker_t *worker = swapchain_worker_t::start(in.hSwapChain, in.RenderAdapterLuid, in.hNextSurfaceAvailable, devices_, buffer2);
    if (worker == nullptr) {
      return STATUS_INSUFFICIENT_RESOURCES;
    }
    AcquireSRWLockExclusive(&table_lock_);
    const bool still_ours = m->generation == generation && m->state != state_e::free;
    if (still_ours) {
      m->worker = worker;
    }
    ReleaseSRWLockExclusive(&table_lock_);
    if (!still_ours) {
      worker->stop(true);
    }
    return STATUS_SUCCESS;
  }

  NTSTATUS adapter_t::unassign_swapchain(IDDCX_MONITOR monitor) {
    AcquireSRWLockExclusive(&table_lock_);
    monitor_t *m = slot_of(monitor);
    swapchain_worker_t *worker = nullptr;
    if (m != nullptr) {
      worker = m->worker;
      m->worker = nullptr;
    }
    ReleaseSRWLockExclusive(&table_lock_);
    if (worker != nullptr) {
      worker->stop(true);
    }
    return STATUS_SUCCESS;
  }

  void adapter_t::monitor_destroyed(const monitor_context_t &ctx) {
    if (ctx.slot >= max_monitors) {
      return;
    }
    swapchain_worker_t *worker = nullptr;
    AcquireSRWLockExclusive(&table_lock_);
    monitor_t &m = monitors_[ctx.slot];
    if (m.generation == ctx.generation && m.state != state_e::free) {
      worker = m.worker;
      m.worker = nullptr;
      m.object = nullptr;
      if (m.state == state_e::active) {
        m.state = state_e::free;  // destroyed by Windows (e.g. adapter teardown), not by a remove request
      }
    }
    ReleaseSRWLockExclusive(&table_lock_);
    if (worker != nullptr) {
      worker->stop(false);
    }
  }

}  // namespace tessera::vdd
