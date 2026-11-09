# Chaster ESP32

An ESP32-hosted Chaster dashboard with a two-stage setup and explicit wearer-to-keyholder handover.

## How the handover works

1. **Wearer setup:** connect to the device setup network `Chaster-ESP32` (default password: `chaster32`) and open `http://192.168.4.1`.
2. Enter the Wi-Fi network, the lock ID, a wearer Chaster developer token with read access, and a unique dashboard password (at least 10 characters).
3. The device joins the Wi-Fi network and displays a read-only wearer dashboard. Freeze/unfreeze, time adjustment, lock selection and keyholder configuration are hidden and also rejected by the firmware.
4. **Give the device to the keyholder.** The keyholder opens the device dashboard and enters their own Chaster developer token in the handover panel.
5. The device restarts in keyholder mode. The wearer token is erased from ESP32 NVS and RAM; the keyholder token is stored and used for all subsequent Chaster API requests. The keyholder dashboard restores lock controls, lock selection, history and token replacement.

The wearer token is never promoted to a keyholder token. The keyholder must supply a token belonging to their own Chaster account. The Wi-Fi settings, selected lock ID and dashboard password remain in place during handover.

## Features

- Wearer-first Wi-Fi and read-only Chaster setup.
- Explicit keyholder token handover; wearer token is removed when handover completes.
- Keyholder dashboard with live elapsed/remaining timers, lock progress, status/frozen indicators, selected-lock metadata, history and raw API diagnostics.
- Keyholder-only freeze/unfreeze and add/remove-time actions, enforced in both the UI and firmware API.
- Keyholder lock selection and token replacement.
- 30-second refresh interval with request throttling.
- HTTP Basic Authentication for dashboard routes.
- Authenticated Wi-Fi settings and recovery if the configured network changes or drops.
- Full credential reset from keyholder configuration.
- PlatformIO build targets for ESP32, ESP32-C3, ESP32-S2 and ESP32-S3.

## Build and flash

1. Install Visual Studio Code and PlatformIO.
2. Open this repository.
3. Build for your board:

   ```sh
   pio run -e esp32dev
   pio run -e esp32c3
   pio run -e esp32s2
   pio run -e esp32s3
   ```

4. Flash the matching environment using PlatformIO.
5. Follow the wearer setup and handover steps above.

**Preserving saved data when updating:** Use PlatformIO's normal Upload action (`pio run -e <environment> -t upload`). Do not use `pio run -t erase`, `esptool erase-flash`, Erase All Flash, or a full-chip erase: these erase NVS and remove saved settings and tokens. Avoid changing the partition layout between firmware versions unless a migration plan is provided.

## Chaster API permissions

Use separate developer tokens for the two roles:

- Wearer token: read-only lock access (the Chaster token must have the applicable lock-read permission).
- Keyholder token: `keyholder:read` and `keyholder:write` scopes (or the legacy `keyholder` scope where supported).

Chaster also checks the permissions on the selected lock; a valid token does not guarantee every action will be accepted. Test endpoint payloads, scopes and lock permissions on a non-critical lock before relying on the firmware.

## Security limitations — read before use

- Change the default setup AP password in `src/main.cpp` before deployment.
- Set a unique dashboard password. Dashboard routes use HTTP Basic Authentication with username `admin`.
- Local dashboard traffic is plain HTTP; use only on a trusted private network.
- Tokens are stored in ESP32 NVS. Physically protect the device. Use the full credential reset before disposal or transfer to a different owner.
- **TLS certificate validation is not enabled yet.** The firmware currently uses `WiFiClientSecure::setInsecure()`, so it does not verify Chaster's HTTPS certificate and is vulnerable to interception. Do not treat this as a secure credential appliance until certificate validation is implemented.
- The firmware has no OTA updater.
- This project is not a safety, emergency-unlock or certified lock-control device. Keep access to Chaster's official controls.

## Verification status

The handover flow is implemented in the `wearer-keyholder-handover` branch. It has not yet been compiled with PlatformIO or tested on physical ESP32 hardware. Confirm all four PlatformIO targets, Chaster endpoint permissions, token handover, reset behavior and lock actions on a non-critical test lock before relying on the firmware.
