# SudoVDA per-session virtual display — porting notes (US-076 Phase 2/3)

`virtual_display.{h,cpp}` (here) + `third-party/sudovda/{sudovda-ioctl.h,
sudovda.h}` are **vendored verbatim from Apollo** (ClassicOldSong/Apollo @
`adc5c5a`, GPLv3; the SudoVDA headers are MIT/CC0). They give the host the
per-session virtual-display control layer decided in
[ADR 0025](../../../../../docs/decisions/0025-sudovda-per-session-virtual-display.md).

**Status: vendored AND wired — BUILD-UNVERIFIED.** The wiring below was
added but has NOT been compiled (no Windows MSYS2 host-build env in the
session that wrote it). Deps are only standard Win SDK + mingw `ddk/`
headers + setupapi/cfgmgr32/dxva2/ole32/dxgi (no WDK), so it *should* build
on the host's mingw toolchain — **verify with a local host-engine build
before trusting it.**

## What was wired

1. **CMake** (`cmake/compile_definitions/windows.cmake`):
   - `virtual_display.cpp/.h` + `third-party/sudovda/*.h` added to
     `PLATFORM_TARGET_FILES`.
   - `include_directories(SYSTEM third-party)` so `<sudovda/sudovda.h>`
     resolves.
   - link libs `cfgmgr32 dxva2 ole32` added (dxgi/setupapi were already
     present). If the mingw link fails on a missing symbol, check these
     first (`GetPhysicalMonitors` → dxva2, `CoCreateGuid` → ole32,
     `CM_*`/`SetupDi*` → cfgmgr32/setupapi).

2. **Per-session hook** (`src/display_device.cpp`, all `#ifdef _WIN32`):
   - `VDD_STATE` holder + `lumen_create_virtual_display_unlocked(session)` /
     `lumen_remove_virtual_display_unlocked()` (open device + ping thread on
     first use, `createVirtualDisplay` at `session.width/height/fps`).
   - `configure_display(video_config, session)`: when
     `output_name == "virtual"`, create the monitor and return (skip the
     physical libdisplaydevice reconfig).
   - `revert_configuration()`: remove the monitor.
   - `map_output_name("virtual")`: returns `VDD_STATE.gdi_name` (the created
     `\\.\DISPLAYx`), or `""` (→ physical primary) if none active. The old
     ADR-0024 friendly-name resolver branch was deleted.

## Verify / caveats to check on the local build

- **Ordering**: `map_output_name("virtual")` returns "" until
  `configure_display` has created the monitor. Confirm on hardware that
  `configure_display` runs before capture binds the display (Sunshine's
  stream-start sequence). If capture inits first, the create may need to
  move earlier (e.g. into the rtsp launch path, as Apollo does).
- **Watchdog**: `startPingThread` keeps the monitor alive; the fail
  callback only logs — decide whether a ping failure should end the session.
- **Protocol version**: `openVDisplayDevice()` checks
  `sudovda-ioctl.h`'s `VDAProtocolVersion` against the installed driver;
  a skew returns `VERSION_INCOMPATIBLE`. Keep this header + the fetched
  driver (`scripts/fetch-windows-vdd.sh`) in sync.
- **`config.m_device_id = "virtual"`** never reaches libdisplaydevice now
  (we return early), but double-check no other path applies it.

## Rebase note

Record these files in `engine/host/UPSTREAM.md` as LumeN deviations so a
rebase onto upstream Sunshine preserves them (upstream has no virtual
display).
