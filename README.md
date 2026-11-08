# Chaster ESP32

An ESP32-hosted, mobile-friendly dashboard for viewing and managing Chaster locks. This is a Chaster-native port; it does not call the old EmlaLock API.

## Included

- Wi-Fi setup access point: `Chaster-ESP32` (default password: `chaster32`); it is enabled only when the saved Wi-Fi connection cannot be established
- Stores Wi-Fi details, Chaster token, optional lock ID, account mode and dashboard password in ESP32 NVS
- Chaster Public API Bearer-token authentication
- Wearer lock list and keyholder lock-search mode
- Lock selection, return-to-list control and lock detail display
- Elapsed and remaining time display from lock start/end timestamps
- Freeze and unfreeze controls
- Add/remove time controls using `POST /locks/{lockId}/update-time` with signed `duration` seconds
- Recent history request
- 30-second data refresh, with failed API attempts throttled to avoid rapid retry loops
- HTTP Basic Authentication for dashboard routes
- Clear-credentials action
- PlatformIO environments for ESP32, ESP32-C3, ESP32-S2 and ESP32-S3
- GitHub Actions build matrix

## Build and flash

1. Install Visual Studio Code and PlatformIO.
2. Open this repository.
3. Build for your board, e.g.:

   ```sh
   pio run -e esp32dev
   pio run -e esp32dev -t upload
   ```

   Other environments: `esp32c3`, `esp32s2`, `esp32s3`.

4. Connect to Wi-Fi `Chaster-ESP32` (default password `chaster32`).
5. Open `http://192.168.4.1`.
6. Enter your Wi-Fi SSID, Chaster developer/access token, optional lock ID, account view and a dashboard password of at least 10 characters.
7. After restart, open the ESP32's LAN IP address. Sign in as username `admin` with the dashboard password.

Leave Lock ID blank to show an account lock list. For keyholder mode, use a token belonging to the keyholder account.

## Chaster scopes and permissions

The Chaster Public API requires a developer or access token. The relevant scopes include:

- `locks:read` for wearer lock reads
- `locks:write` for wearer lock actions
- `keyholder:read` for keyholder lock search
- `keyholder:write` for keyholder actions

Legacy aggregate scopes may continue to work. Chaster also checks the permissions granted on each individual lock. The API can reject an action even when the token itself is valid.

### Add/remove time payload

The firmware sends a signed duration in seconds:

- Add one hour: `{"duration":3600}`
- Remove 30 minutes: `{"duration":-1800}`

This follows the request format supplied for this integration. Note that the currently published Swagger page lists `UpdateTimeDto` without showing its properties, so the exact request body is not independently confirmed by the visible OpenAPI schema. The UI reports Chaster's API response; test on a non-critical lock first and check Chaster's lock history before relying on it.

## Security limitations — read before use

- Change the default setup access-point password in `src/main.cpp` before deployment.
- Set a unique, strong dashboard password. The dashboard uses HTTP Basic Authentication, but local dashboard traffic is plain HTTP. Use only on a trusted private network.
- The Chaster token is stored in ESP32 NVS. Do not commit it to GitHub or share it. Use the reset action before handing the board to someone else.
- **TLS certificate validation is not enabled yet.** The firmware currently calls `WiFiClientSecure::setInsecure()`. This means the ESP32 does not verify Chaster's HTTPS certificate and is vulnerable to interception. Do not use this as a secure credential appliance or on untrusted networks until certificate validation is implemented.
- No OTA firmware updater is included.
- This project is not a safety, emergency-unlock or certified lock-control device. Keep access to Chaster's official controls.

## Verification status

The dashboard JavaScript has been syntax-checked. The firmware has not yet been compiled with PlatformIO or tested on physical ESP32 hardware. A recent commit was not associated with a GitHub Actions run or status result when checked, so no successful build is claimed. Verify the workflow result, board target, API scopes, API request bodies and lock permissions before relying on it.

## References

- [Chaster Public API getting started](https://docs.chaster.app/api/basics/getting-started/)
- [Chaster Public API endpoints and OpenAPI/Swagger](https://docs.chaster.app/api/public-api/endpoints/)
- [Chaster OAuth scopes changelog](https://docs.chaster.app/api/changelog/)
