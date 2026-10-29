# Tessera protocol extensions

Tessera stays fully compatible with stock Moonlight clients and stock Sunshine hosts (brief §4.3).
Every new feature is negotiated, and a missing feature on either side never breaks the basic single-stream session.

## Rules

1. **Negotiation.** The host advertises capabilities in `/serverinfo`; the client requests them in the RTSP
   exchange. A feature is used only if both sides advertised it **and** the paired client has permission for it
   (per-client toggles in the host web UI).
2. **Versioning.** Each extension has a name and an integer version. A change that breaks compatibility increments
   the version; both sides may support several versions and use the highest common one.
3. **Encryption.** Every new channel (extra video streams, mic, camera, USB/IP, storage) is encrypted with the
   session's negotiated keys (AES-GCM), as Sunshine already does for control and input.
4. **Untrusted input.** Every length and field from the wire is validated, and every allocation is bounded.
   Parsers get fuzz targets (Phase 12).

## Capability registry

| Name | Version | Phase | Summary | Status |
|---|---|---|---|---|
| `multistream` | 1 | 2 | One video stream per client monitor in one session (`multistream.md`, ADR 0007). Per-client permission: "Multi-display streaming", on by default. | In progress |
