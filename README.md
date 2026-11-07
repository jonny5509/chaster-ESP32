# Chaster ESP32

An ESP32-hosted, mobile-friendly dashboard for reading Chaster lock data and using the documented lock freeze control. This is a Chaster-native replacement foundation for the EmlaLock ESP32 workflow; it does not call the old EmlaLock API.

## Features in this version

- Wi-Fi setup access point: `Chaster-ESP32` (password: `chaster32`)
- Stores Wi-Fi details, Chaster token, optional lock ID and local dashboard password in ESP32 NVS
- Uses Chaster Public API Bearer-token authentication
- Reads `GET /locks` when no lock ID is set, or `GET /locks/{lockId}` for a selected lock
- Refreshes the Chaster data every 30 seconds
- Freeze/unfreeze controls using `POST /locks/{lockId}/freeze` with the documented `{ "isFrozen": true|false }` body
- HTTP Basic Authentication for the local dashboard and its API routes
- Clear-credentials action for device handover/reset
- PlatformIO targets for ESP32, ESP32-C3, ESP32-S2 and ESP32-S3

## Setup

1. Install Visual Studio Code and PlatformIO.
2. Open this repository and select the environment matching your board.
3. Build and flash, for example:

   ```sh
   pio run -e esp32dev
   pio run -e esp32dev -t upload
   ```

4. Power the board. Connect your phone/computer to Wi-Fi `Chaster-ESP32` using password `chaster32`.
5. Open `http://192.168.4.1`.
6. Enter your home Wi-Fi SSID, Chaster developer/access token, an optional lock ID, and a local dashboard password of at least 10 characters.
7. After the ESP32 restarts and joins your home Wi-Fi, open its IP address shown in the serial monitor and sign in with username `admin` and the dashboard password you set.

Leave the lock ID blank to display the lock list available to the Chaster account associated with the token. For keyholder use, configure a token belonging to the keyholder account with the required Chaster API scopes and permissions.

## Chaster token scopes and permissions

For reading locks, use `locks:read` (or the legacy `locks` scope). Freeze/unfreeze requires `locks:write` plus the corresponding per-lock permission. Chaster checks authorization server-side; a successful firmware request cannot bypass lock permissions.

Chaster's Public API is in beta and may change. See:
- https://docs.chaster.app/api/basics/getting-started/
- https://docs.chaster.app/api/public-api/endpoints/
- https://docs.chaster.app/api/reference/oauth-scopes-update/

## Add/remove time: API schema limitation

Chaster documents `POST /locks/{lockId}/update-time` and states that it adds or removes duration when the caller has the matching `lock.time.add` or `lock.time.remove` permission. However, the current published OpenAPI specification defines `UpdateTimeDto` as an empty object with no duration, direction, or reason fields. This firmware therefore does **not** send a guessed request or expose misleading add/subtract controls. The time-change controls can be added when Chaster publishes the required request semantics or confirms the correct payload.

## Security notes

- Change the default setup AP password in `src/main.cpp` before deployment.
- Set a strong dashboard password. HTTP Basic Authentication protects the dashboard, but local traffic is plain HTTP; use only on a trusted private Wi-Fi network.
- The Chaster token is stored in ESP32 NVS. Do not share it or commit it to GitHub. Use the clear-credentials action before transferring the board.
- **HTTPS certificate validation is not yet production-ready.** The current firmware uses `WiFiClientSecure::setInsecure()` for the Chaster API connection because the ESP32 trust anchor has not yet been verified against Chaster's live certificate chain. Do not expose this device to untrusted networks or treat it as a secure credential appliance until certificate validation is implemented and tested.
- No firmware OTA updater is included in this Chaster port yet.
- This is not a safety, emergency-unlock, or certified lock-control device. Keep access to Chaster's official controls.

## Verification status

The source has been committed to GitHub, but it has **not** yet been compiled with PlatformIO or tested on physical ESP32 hardware. Verify the correct board environment, TLS connectivity, API scopes and lock permissions before relying on it.
