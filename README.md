# Chaster ESP32

An ESP32-hosted, **keyholder-only** dashboard for managing Chaster locks through the Chaster Public API. The device is configured directly with the keyholder's account; there is no wearer setup or handover step.

## Features

- Keyholder-first setup with Wi-Fi, a Chaster Public API developer token and a selected lock ID.
- Keyholder lock search and selected-lock details.
- Responsive dark control-centre dashboard with live elapsed/remaining timers, lock progress, status/frozen indicators, selected-lock metadata, history and raw API diagnostics.
- Freeze/unfreeze, add/remove time, recent lock history, and elapsed/remaining timers.
- All Chaster API requests use the configured keyholder token. The firmware never uses a wearer token.
- 30-second refresh interval with request throttling.
- HTTP Basic Authentication for dashboard routes.
- Authenticated Wi-Fi settings and recovery if the configured network changes or drops.
- Keyholder token replacement and full credential reset from the dashboard.
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

**Preserving saved data when updating:** Use PlatformIO's normal **Upload** action (`pio run -e <environment> -t upload`) to flash a firmware update. The firmware image is written to the application partition; normal upload does not intentionally erase the NVS partition where Wi-Fi credentials, the keyholder token, dashboard password and selected lock ID are stored. Do **not** use `pio run -t erase`, `esptool erase-flash`, an IDE option such as **Erase All Flash**, or a full-chip erase: those erase NVS and permanently remove the saved configuration. Avoid changing the partition layout between firmware versions unless a migration plan is provided. Back up credentials/configuration securely before maintenance; there is currently no export/restore tool.

5. Connect to the setup network `Chaster-ESP32` (default password: `chaster32`) and open `http://192.168.4.1`.
6. Enter the keyholder account's Wi-Fi details, Chaster developer token, lock ID and a strong local dashboard password (at least 10 characters).
7. Open the device's LAN address and sign in as `admin` using the dashboard password.

**Upgrading an older wearer-first installation:** The new firmware does not promote a saved wearer token into a keyholder token. If no keyholder token is present, it opens keyholder setup and requires the keyholder to enter their own token. The previous wearer token is not used for API calls and is cleared when the new keyholder setup is saved. Existing keyholder installations can retain their keyholder token and configuration when updated normally.

## Chaster API permissions

Use a Chaster developer/access token belonging to the actual keyholder account. It should have `keyholder:read` and `keyholder:write` scopes (or the legacy `keyholder` scope, where supported). Chaster also checks the lock's own permissions; a valid token does not guarantee every action will be accepted.

The firmware uses these Chaster-native routes:

- `POST /keyholder/locks/search` to search locks the keyholder can access.
- `GET /locks/{lockId}` for selected lock details.
- `POST /locks/{lockId}/freeze` for freeze/unfreeze.
- `POST /locks/{lockId}/update-time` with signed `duration` seconds for adding/removing time.
- `POST /locks/{lockId}/history` for recent history.

Time payload examples:

- Add one hour: `{"duration":3600}`
- Remove 30 minutes: `{"duration":-1800}`

Test endpoint payloads, scopes and lock permissions on a non-critical lock before relying on the firmware.

## Security limitations — read before use

- Change the default setup AP password in `src/main.cpp` before deployment.
- Set a unique dashboard password. Dashboard pages and API routes use HTTP Basic Authentication with username `admin`. Local dashboard traffic is plain HTTP; use only on a trusted private network. There is no in-firmware password recovery; if it is lost, an NVS erase/reflash may be required. Keep a secure copy.
- The keyholder token is stored in ESP32 NVS. Physically protect the device and use the full credential reset before disposal or transfer to a different owner.
- **TLS certificate validation is not enabled yet.** The firmware currently uses `WiFiClientSecure::setInsecure()`, so it does not verify Chaster's HTTPS certificate and is vulnerable to interception. Do not treat this as a secure credential appliance until certificate validation is implemented.
- The firmware has no OTA updater.
- This project is not a safety, emergency-unlock or certified lock-control device. Keep access to Chaster's official controls.

## Verification status

The keyholder-only changes have been committed to the feature branch but have not yet been compiled with PlatformIO or tested on physical ESP32 hardware. Confirm all four PlatformIO targets, Chaster endpoint payloads/scopes, lock permissions and setup/reset behavior before relying on the firmware.
