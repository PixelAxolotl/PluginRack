# PluginRack driver — build and test guide

Separate WDM PortCls WaveRT driver. The user-mode engine in `../core` and
`../app` already speaks this protocol (`VirtualAudioProtocol.h`,
`VirtualRing.h`, `VirtualBridge.h`); this folder holds the kernel side.

## Files

- `PluginRackDriverProtocol.h` — frozen C ABI. Shared with the engine
  tests (`tests/test_driver_protocol.cpp` asserts magic, version, layout,
  clamp behavior). Bump `kProtocolVersion` on any layout change.
- `PluginRackDriver.h` / `.cpp` — DriverEntry, AddDevice, bridge
  interface (`\\.\PluginRackBridge`), IOCTL claim/release/query, and the
  `ExSetTimer` ring bridge. Single client per slot.
- `PluginRackMiniport.cpp` — WaveRT topology contract: 2 render pins
  (speaker node) + 2 capture pins (microphone node), float32 stereo.
  Mechanical PortCls boilerplate follows the SYSVAD WaveRT pattern.
- `PluginRack.inf` — endpoint names, mic/speaker categories, locked-down
  bridge ACL. Replace the placeholder GUID at fork time.

## Prerequisites (not installed in this environment)

- WDK 10/11 matching the target Windows build (needs `portcls.lib`,
  `stdunk.lib`, km headers — the plain Windows SDK is not enough).
- Visual Studio with the WDK extension, x64.
- A Windows 10/11 VM with HVCI (Memory Integrity) + Driver Verifier for
  all testing. Never test on the dev host.

## Build (on a WDK machine)

```powershell
# From a WDK developer prompt, x64 Release:
msbuild driver.vcxproj /p:Configuration=Release /p:Platform=x64
# Produces PluginRack.sys + PluginRack.inf + PluginRack.cat (after signing)
```

## Test-sign and install in the VM

```powershell
# Test certificate (VM only, never ship):
New-SelfSignedCertificate -Type CodeSigning -Subject "CN=PluginRack Test"
# Sign, enable test signing, install:
signtool sign /fd SHA256 /a PluginRack.sys
bcdedit /set testsigning on  # reboot the VM
pnputil /add-driver PluginRack.inf /install
```

Verify: four endpoints in Sound settings, selectable in OBS/Discord,
`\\.\PluginRackBridge` openable by the engine, second claim rejected.

## Ship signing

Partner Center attestation signing with an EV certificate. Bundle the
signed `.inf` + `.cat` + `.sys` in the app installer via `pnputil` /
`DiInstallDriver`. Handle PnP remove/query-remove so upgrades do not
require a reboot unless handles are held.

## What the engine already does

Without the driver installed, selecting a `pluginrack://vaio/...`
endpoint records the assignment and reports "Virtual driver not
installed." Scenes round-trip the stable logical ids with no schema
bump. Once the driver is installed and the bridge claimed, `VirtualCapture`
/ `VirtualPlayback` poll the shared ring with `DriftResampler`
convergence on both paths.
