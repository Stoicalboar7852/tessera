# Phase 3 acceptance: the Qt client on two client machines

Manual tests for `docs/phase-3-design.md` (acceptance 1–7) on real client hardware, with the dev PC as the host. The
dev PC's unattended run (`tools/tests/Run-QtClientTest.ps1`) covers what one PC can: stream counts, mixed and high
refresh rates, fallbacks and the absolute mouse. These tests cover what it can't: two physical monitors, real
fullscreen, both mouse modes, focus and keyboard, Wi-Fi and Ethernet, the Mac.

| Client | Monitors | Network |
|---|---|---|
| Windows laptop (Ryzen 5 4500U, 8 GB) | built-in + external, both 1920×1080 at 60 Hz | Wi-Fi |
| MacBook Pro (M5, 24 GB) | built-in ProMotion (up to 120 Hz) + 2560×1440 monitor at 144 Hz over HDMI | gigabit Ethernet (USB-C adapter) |

Allow about an hour per client. Results go in the table at the end; the log collector's zip goes to
`D:\Remote streaming\logs\phase3-acceptance\` on the dev PC.

## 1. Host: the dev PC

1. Open PowerShell **as administrator** and run:

   ```
   powershell -ExecutionPolicy Bypass -File "D:\Remote streaming\src\tools\tests\Start-LanTestHost.ps1"
   ```

   It installs the virtual display driver, opens the host's ports to the local network only (the firewall rules are
   named "Tessera LAN test"), starts the Tessera host and shows the PC's network address, the web UI's address with a
   one-time user name and password, and a prompt for pairing PINs. Leave the window open during the tests.
2. To end the tests, type `q` in that window (or press Ctrl+C). It stops the host, removes the driver and the firewall
   rules, and checks that the display layout is as before.

The host is the real thing here (unlike the dev-PC run): it makes stream 0's display its primary, so the taskbar and
new windows appear on the client's main monitor.

## 2. Windows laptop (Wi-Fi)

1. Download the `client-qt-windows-x64` artifact from the newest green run of **client Qt (Windows, macOS)** at
   https://github.com/Stoicalboar7852/tessera/actions/workflows/ci-client-qt.yml (sign in to GitHub; artifacts last 7
   days). Unzip it to a new folder, for example `Desktop\Tessera Moonlight`.
2. If the folder has `portable.dat.inactive`, rename it to `portable.dat`. The client then keeps its settings, pairing
   and logs in that folder and leaves an installed Moonlight alone.
3. Connect the external monitor and extend the desktop (Windows Settings > System > Display > "Extend these
   displays"). Note which one is the main display.
4. Use 5 GHz Wi-Fi if the router has it, with the laptop near the router and on mains power.
5. Start `Moonlight.exe`. The dev PC shows up by name; if it doesn't, use the + button and type the address the host
   window shows. Click it, then type the PIN Moonlight shows into the host window.
6. In Moonlight's settings, under **Displays**, choose **All displays**. Leave everything else at the defaults
   (automatic bitrate, V-Sync on, hardware decoding), with **Display mode: Fullscreen**.

## 3. MacBook Pro (Ethernet)

1. Download the `client-qt-macos` artifact from the same run and open the `.dmg` inside it.
2. Drag Moonlight to the Desktop (not to Applications, so a stock Moonlight there is kept), rename it to
   `Tessera Moonlight`, then move it to Applications.
3. The build isn't signed by a developer, so allow it once in Terminal:

   ```
   xattr -dr com.apple.quarantine "/Applications/Tessera Moonlight.app"
   ```

   ```
   codesign --force --deep --sign - "/Applications/Tessera Moonlight.app"
   ```

   It shares its settings and pairing with stock Moonlight, if that is installed; only the Displays choice is new.
4. Plug in the Ethernet adapter and turn Wi-Fi off for the tests, so the stream can only use the cable. In System
   Settings > Network > Ethernet > Details > Hardware, the speed should read 1000baseT.
5. Built-in display: System Settings > Displays > Refresh rate **ProMotion**. External monitor: 144 Hz. Arrange the
   two side by side.
6. Pair and choose **All displays** as on the laptop (steps 5–6 above).

## 4. Tests

Run each test on both clients unless it says otherwise. Moonlight's shortcuts: Ctrl+Alt+Shift (Ctrl+Option+Shift on
the Mac) with **S** for the statistics overlay, **Z** to release or take the mouse, **X** for fullscreen, **Q** to end
the session.

To see each display's frame rate, open https://www.testufo.com in a browser window on each host display (through the
stream). It shows the rate the host display runs at; the overlay shows what the client receives and draws.

### T1. Two displays for 10 minutes (acceptance 1 and 2)

1. Start **Desktop**. Expect one fullscreen window per monitor, each showing a different host display, with the host's
   taskbar on your main monitor.
2. Turn the overlay on (S). It has a line per extra display ("Display 1: …").
3. Open TestUFO on both host displays and leave it running for 10 minutes.
4. Note for each display: resolution and Hz, the overlay's FPS (network / decoded / rendered), decode time, dropped
   frames, and the overlay's network latency.
5. End the session (Q).

**Pass:** each display runs at its monitor's rate (within 5 %), as TestUFO on that display shows; decoding is in
hardware for both (the overlay's decoder line, and the log); dropped frames don't keep growing; no error messages.

### T2. Mouse and keyboard (acceptance 3)

1. Settings: turn on **Optimize mouse for remote desktop instead of games** (absolute mode). Start a session.
2. Move the cursor slowly across the edge between the two monitors, both ways, near the top, middle and bottom.
   **Pass:** the host cursor follows onto the matching display at the same height, without a jump or a stop at the
   edge.
3. On the host, open Notepad on the second display, click into it and type a sentence; then click back to the first
   display and type there. **Pass:** every key arrives once, nothing sticks, and the session doesn't drop out of
   fullscreen.
4. End the session, turn the option off (relative mode, as for games), start again. **Pass:** the mouse is captured,
   the host cursor moves across both displays by itself and shows in the right window; Z releases and retakes the
   mouse.
5. With **Capture system keyboard shortcuts** set to "in fullscreen": the Windows key and Alt+Tab (Cmd+Tab on the Mac
   is the Mac's own, by design) act on the host, whichever window has focus.

### T3. Choosing displays (acceptance 4)

1. **This display:** one window, on the monitor Moonlight's window was on; looks and behaves as upstream Moonlight.
2. **Chosen displays** with one ticked: the launch warning "Fewer than two of the chosen displays are connected" and one
   window.
3. **Chosen displays** with both ticked: two windows.
4. Unplug the external monitor and open the settings: the Displays section is hidden. Plug it back in.

### T4. Fewer streams than asked for (acceptance 5)

1. In the host's web UI (address and password in the host window), page **Troubleshooting**, client list: turn off
   the multi-display switch for this client.
2. Start a session. **Pass:** one window, and a message in it for about 10 seconds saying multi-display isn't enabled
   for this client.
3. Turn the switch back on.

### T5. Mixed and high refresh rates (acceptance 6; the Mac)

1. With ProMotion and the 144 Hz monitor, start a session and open TestUFO on both host displays.
   **Pass:** about 120 on the built-in display's stream and 144 on the other, in TestUFO and in the overlay.
2. Note the overlay's numbers for both; this is the host's heaviest case (the built-in display streams at its native
   size).
3. Settings > Displays > **Display resolution: Desktop size**, start again and note the numbers again.

### T6. V-Sync off (client-side VRR; the Mac, optional on the laptop)

Settings: V-Sync off. Start a session, play a video or a game whose frame rate varies. **Pass:** no tearing on the
ProMotion display (it adapts its refresh rate), smooth motion on both windows. Note anything odd. Turn V-Sync back on.

### T7. A monitor goes away mid-session

During a session, unplug the external monitor. **Pass:** the session ends with a message that the display was
disconnected; nothing hangs. Plug it back in.

### T8. One-monitor client (acceptance 7)

With the external monitor unplugged and **All displays** chosen, start a session. **Pass:** exactly upstream
Moonlight's behaviour: one window, no extra overlay lines.

## 5. Collecting the logs

After the tests, on each client:

- **Windows:** in PowerShell, `powershell -ExecutionPolicy Bypass -File Collect-ClientLogs.ps1 -MoonlightDir "<the
  folder from step 2.1>"`. It writes `tessera-client-logs-<time>.zip` to the Desktop.
- **Mac:** in Terminal, `bash Collect-ClientLogs.sh`. It writes the same zip to the Desktop.

The scripts are in `tools/tests/` in the repository (https://github.com/Stoicalboar7852/tessera). They collect the
newest Moonlight logs, the monitors, the GPU and the network link; nothing else. Copy the zips and the host window's
run folder (it prints it at the end) into `D:\Remote streaming\logs\phase3-acceptance\` on the dev PC.

## 6. Results

| Test | Client | Display: size @ Hz | FPS network / decoded / rendered | Decode ms | Dropped | Network latency | Pass? | Notes |
|---|---|---|---|---|---|---|---|---|
| T1 | Windows | | | | | | | |
| T1 | Mac | | | | | | | |
| T2–T8 | | | | | | | | |
