<!--
EspAppLoader
File: README.md
Purpose: Describes EspAppLoader and how to build and test this release.

Copyright (c) 2026 Darryl Cornish

SPDX-License-Identifier: MPL-2.0
-->

# EspAppLoader 2026-11-23-2

The loader implementation lives in the C++ namespace `esp_apploader`.
The controller is `AppLoaderMgr`, model components use `Mdl`, and the
presentation classes are `AppLoaderSerialVw` and `AppLoaderWebVw`.
Arduino's global `setup()` and `loop()` delegate to the namespaced
`appLoaderSetup()` and `appLoaderLoop()`. Build macros use
`ESP_APPLOADER_`. The included hardware profile and board helpers
retain the WTg pin assignments and names.

The Wi-Fi upload SSID and advertised BLE name now use
`EspAppLoader-<unit>`. Existing SD paths, including `/RECOVERY.WTG`,
and flash partition labels remain compatible with the previous build.
The included test app is version 0.1.1 with command echo.

## Build and test

Use ESP32 Dev Module, Espressif core 3.3.12, Flash Size 8 MB, and Partition
Scheme Custom. Open `EspAppLoader/EspAppLoader.ino` directly from this package so
Arduino sees a single set of source files. AppLoader uses RTClib and Adafruit
BusIO. The supplied partition table places AppLoader in app1 at `0x3D0000` and
the operational app in app0 at `0x10000`. For a board with this layout already
provisioned, export the application-only binary and write it to app1 to
preserve app0.

With AppLoader running in app1, startup falls back to a validated app0 image if
the SD card is unavailable or `/apps` has no top-level `.bin` files.
GPIO4 held low at startup or the SD marker `/RECOVERY.WTG` suppresses fallback.
An invalid app0 image leaves the loader available for diagnostics. A successful
handoff keeps app0 selected across resets. Automatic rollback remains open.

On a bench board, the included `AppLoader_Test_App` can occupy app0. It stays
running so you can verify the handoff. At 115200 baud, send `help?` to list commands,
`status?` for partition status or `r` to select app1 and restart. Hold GPIO4 low before sending `r`
when you want the loader to stay available for diagnostics.

All five host tests passed. ESP32 compile/link and physical board checks are
pending. Versioned documents describe their respective historical builds.

## Provision AppLoader from app0

This revision adds `appLdr2App1 <0|1>`. Upload the updated sketch into app0
with the existing custom partition table and core 3.3.12. The command is
available over serial and authorized BLE. It requires AppLoader running in
app0 with app0 currently selected for boot, an inactive app1, and test mode
disabled. Ordinary installation remains blocked while running in app0.
This operation replaces the contents of app1. Before any app1 erase/write,
the source must pass ESP image validation and contain the complete embedded
EspAppLoader product/version/build record matching this running loader.
An ordinary operational app, missing identity, or mismatched identity is rejected.
The verified version and build read from app0 are reported in a
`PROVISION state=identity_verified source=app0 product=EspAppLoader version=... build=...`
line before copying. Copying rechecks identity before erasing app1. The record
is a product identity check, not a cryptographic publisher signature.

- `appLdr2App1 1`: copy and verify AppLoader, select app1, and restart there.
  No SD card is required. Automatic fallback is suppressed until an explicit
  successful `bootapp` or `bootapp existing` clears the provisioning hold.
- `appLdr2App1 0`: first run `startup <filename>` to select a suitable SD
  application. The command captures that candidate and its SHA-256, copies
  AppLoader, and restarts into app1. AppLoader then rechecks that same SD
  candidate, installs it into app0, verifies it, selects app0, and restarts
  again. This requires two restarts; app0 is never overwritten while running.

Missing or invalid arguments are rejected. The explicit command authorizes
provisioning, so there is no subsequent YES prompt. Upload mode must be closed
first. GPIO4/recovery marker at the intermediate startup cancels automatic SD
installation and leaves AppLoader available. Failure or interruption during
SD installation leaves a persistent loader hold and does not automatically
retry destructive writes. Inspect the error, then use the existing manual
install/boot commands. If an interruption occurred before the first handoff
and the device still runs AppLoader in app0, repeat the provisioning command.

The handoff is stored as one NVS blob. Corrupt/unreadable state suppresses
automatic fallback. A failed copy does not select the destination. The copier
validates the source, uses OTA writing/validation, and compares every copied
byte with flash read-back. Selected SD contents are checked again immediately
before installation. This is integrity checking, not application health testing.
This revision targets the existing unsigned Arduino profile; signed-image
provisioning is not validated. Bootloader rollback-enabled profiles are rejected
because their first-boot health handshake is not implemented by this loader.

Automation can match complete lines beginning `PROVISION`,
`OK appLdr2App1`, or `ERR appLdr2App1 code=`. `stage=copy_verified next_boot=app1`
is the intermediate success; option 0 completes with
`stage=install_verified next_boot=app0`. After reset, verify the actual running
partition with `partitions?` in the loader or the operational app's interface.
General diagnostic output remains present. No complete automation protocol for
all legacy commands is claimed by this change.

Host tests cover copy guards, argument parsing, chunked copying, injected
read/write/validation errors, read-back corruption, and persisted handoff
records. Compile/link with ESP32 core 3.3.12 and physical power-interruption,
serial/BLE, SD, and boot tests are still required. This patch was based on
`EspAppLoader_0.5.4-test1-2026-11-18-MPL-2.0.zip`.

## License

EspAppLoader source and documentation are distributed under the
[Mozilla Public License 2.0](LICENSE). The historical Word design document
has an adjacent license notice.
