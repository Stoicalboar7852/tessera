# ADR 0009: Second clients share the virtual displays; the host removes them when streaming ends

- **Status:** Accepted (2026-11-05, with the addendum to `docs/phase-3-design.md`)

## Context

The host keeps one set of virtual displays, made for one client's monitors (`display_session`, Phase 2). The first
laptop test (2026-11-05) showed two problems with that:

1. **A second client took over the first one's displays.** When a second client resumed the running app with
   different monitors, the host replaced the displays with that client's modes. Both clients then streamed the same
   pair of displays, and the first client's displays changed under it. Giving each client its own displays and
   arranging them together is feature F3 (Phase 5).
2. **The displays outlived the stream.** After Ctrl+Alt+Shift+Q the app keeps running, and Sunshine keeps its display
   setup for a resume (unless `dd_config_revert_on_disconnect` is set). So the virtual displays stayed until the user
   chose "End session" (`/cancel`) or the app exited.

ENet reports a client's graceful disconnect and a timeout the same way (data 0), so the host can't tell "the user
ended the stream" from "the Wi-Fi dropped" without help from the client.

## Decision

**Sharing (interim, until Phase 5).** A launch or resume with a display list keeps the existing virtual displays
when they were made for the same monitors and modes (as before). It **shares** them, unchanged, while another
session is streaming: display i serves stream i, for at most as many streams as there are displays. The reply carries
`TesseraReason=shared-displays` even when every requested stream runs, and the Qt client says so in its status
overlay. Only when nobody is streaming are the displays replaced. The rules are pure functions in `host-helpers`
(`choose_display_reuse`, `share_displays`) with unit tests.

**Removal when streaming ends.** When the last streaming session ends and the app keeps running:
- **The user ended the stream:** the Tessera client sends `TESSERA_DISCONNECT_QUIT` (`0x54510001`) as its ENet
  disconnect data (protocol §7), and the host removes the displays at once. Every deliberate end goes through
  common-c's graceful disconnect: Ctrl+Alt+Shift+Q, closing the window, or the quit menu.
- **The connection was lost** (ping or ENet timeout, or any other disconnect data): the host keeps the displays for
  60 s, then removes them unless a client is streaming again. A resume within that time reuses them.

Sunshine's own rules still apply first: with no app running, or with `dd_config_revert_on_disconnect`, the displays
go at once as before. `/cancel` and app exit also still remove them.

## Consequences

- The second client sees the first client's displays at their modes, scaled to its own stream size (letterboxed
  when the aspect ratios differ). Its own monitor layout is ignored until Phase 5.
- A client that reconnects with different settings while its old session is still timing out shares its old
  displays once. It gets new ones on the next connection.
- Resuming after a deliberate quit creates the displays again (a short display change on the host, ADR 0006).
- Upstream hosts ignore the disconnect data, and upstream clients send 0, so neither changes behaviour.
- Only the Qt client sends the quit marker today. Android and iOS get it when they move to `/common-c` (ADR 0008).
