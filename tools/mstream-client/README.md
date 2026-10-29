# mstream-client

A command-line test client for multistream (`protocol/multistream.md`). It is built on `/common-c`, receives every
stream of a session without decoding it, and reports per-stream statistics. The Phase 2 harness
(`tools/tests/Run-MultiStreamTest.ps1`) drives it; it is not a player.

Built by the native CMake tree with MinGW (it needs OpenSSL, linked statically). Configure that tree with the
`D:\Remote streaming\toolchain\msys64\ucrt64\bin` compiler path, not `T:`; otherwise GCC's own include directory is
added a second time and `#include_next` fails.

```
mstream-client pair   --host 127.0.0.1 --pin 1234 [--identity DIR]
mstream-client stream --host 127.0.0.1 --displays 1920x1080@60,1920x1080@60 --seconds 60 --out DIR [--mouse-test]
```

- `pair` runs the GameStream PIN pairing (the host must get the same PIN, through its web UI or `/api/pin`) and pins
  the host's certificate. The identity (certificate, key, unique ID, host certificate) lives in `--identity`
  (default `mstream-identity`).
- `stream` launches `--app` (default `Desktop`) with a display list unless `--legacy`. Display 0 is the primary; the
  displays are placed left to right. `R` may be `59.94`, and a trailing `h` makes a display HDR. `--bitrate` is the
  session total (default 20000 kbit/s), split by common-c across the streams.

`--out` gets:
- `report.json`:
  - the host's capability;
  - the launch reply (`streams`, `reason`);
  - the stream count and connect time;
  - the round-trip time;
  - per stream: size, frames, IDR frames, bitrate, fps, host processing latency and assembly time (median, p95, max),
    RTP/FEC counters and status.
  
  With `--mouse-test`, it also has the host cursor position after moving to each stream's centre (client and host on
  the same PC).
- `stream<n>.h264` / `.h265`: each stream's first `--dump-seconds` (default 3) from an IDR frame, for snapshots
  (`ffmpeg -i stream1.h265 -frames:v 1 stream1.png`).
- `common-c.log`: the library's log.
