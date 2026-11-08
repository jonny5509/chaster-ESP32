# Chaster ESP32

An ESP32-hosted dashboard for managing Chaster locks through the **Chaster Public API**. The setup and handover experience is modelled on the EmlaLock ESP32 project, but the firmware does not call EmlaLock's API.

## Features

- Wearer-first setup with Wi-Fi and a Chaster Public API token.
- **Hand Over** action: retains the wearer token privately in ESP32 NVS, clears the saved Wi-Fi and any previous keyholder token, and restarts the setup access point.
- Keyholder setup: the keyholder connects to the setup AP and enters their own Wi-Fi details and Chaster API token. The wearer token is not displayed or requested again.
- Role-locked keyholder dashboard after handover.
- Chaster Public API Bearer-token integration, using the wearer token before handover and the keyholder token after handover.
- Wearer lock list and keyholder lock search, lock selection and lock details.
- Freeze/unfreeze, add/remove time, recent lock history, and elapsed/remaining timers.
- 30-second refresh interval with request throttling.
- HTTP Basic Authentication for dashboard routes.
- Authenticated Wi-Fi settings and recovery if the configured network changes or drops.
- Wearer-mode credential reset (hidden in keyholder mode to avoid erasing the retained wearer token).
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
**Preserving saved data when updating:** Use PlatformIO's normal **Upload** action (`pio run -e <environment> -t upload`) to flash a firmware update. The firmware image is written to the application partition; normal upload does not intentionally erase the NVS partition where Wi-Fi credentials, both Chaster tokens, dashboard password, role/handover state and selected lock ID are stored. Do **not** use `pio run -t erase`, `esptool erase-flash`, an IDE option such as **Erase All Flash**, or a full-chip erase: those erase NVS and permanently remove the saved configuration. Avoid changing the partition layout between firmware versions unless a migration plan is provided. Back up the credentials/configuration securely before any maintenance; there is currently no export/restore tool. A normal upload is not a guarantee against data loss if the flash chip/partition layout is changed or the device is factory-erased.

5. Connect to the setup network `Chaster-ESP32` (default password: `chaster32`) and open `http://192.168.4.1`.
6. First setup: enter the wearer's Wi-Fi, Chaster API token and a strong local dashboard password (at least 10 characters).
7. Open the device's LAN address and sign in as `admin` using the dashboard password. Existing installations without a dashboard password will be prompted to set one before lock details are shown.
8. When ready, press **Hand Over to Keyholder**. The device restarts its setup AP. The existing dashboard password is retained; share it with the keyholder so they can authenticate to the setup page and dashboard.
9. The keyholder connects to `Chaster-ESP32` and enters their own Wi-Fi and Chaster API token. The wearer token remains stored privately and is not shown to the keyholder.

The keyholder role is locked after handover. Use the device's credential reset flow before reconfiguring it from scratch.

## Chaster API permissions

Use Chaster developer/access tokens with the required scopes for the account and actions being performed. Chaster also checks the lock's own permissions; a valid token does not guarantee every action will be accepted.

The firmware uses these Chaster-native routes:

- `GET /locks` and `GET /locks/{lockId}` for wearer lock list/details.
- `POST /keyholder/locks/search` for keyholder lock search.
- `POST /locks/{lockId}/freeze` for freeze/unfreeze.
- `POST /locks/{lockId}/update-time` with signed `duration` seconds for adding/removing time.
- `POST /locks/{lockId}/history` for recent history.

Time payload examples:

- Add one hour: `{"duration":3600}`
- Remove 30 minutes: `{"duration":-1800}`

The API request formats and scopes should be checked against the current Chaster documentation and tested on a non-critical lock before relying on them. In particular, verify that the selected lock permits the requested action.

## Security limitations — read before use

- Change the default setup AP password in `src/main.cpp` before deployment.
- Set a unique dashboard password. Dashboard pages and API routes use HTTP Basic Authentication with username `admin`. Local dashboard traffic is plain HTTP; use only on a trusted private network. The password is retained across handover. There is no in-firmware password recovery; if it is lost, an NVS erase/reflash may be required. Keep a secure copy.
- Tokens are stored in ESP32 NVS. The wearer token is retained during handover, so physically protect the device and use credential reset before disposal or transfer to a different owner.
- **TLS certificate validation is not enabled yet.** The firmware currently uses `WiFiClientSecure::setInsecure()`, so it does not verify Chaster's HTTPS certificate and is vulnerable to interception. Do not treat this as a secure credential appliance until certificate validation is implemented.
- The firmware has no OTA updater.
- This project is not a safety, emergency-unlock or certified lock-control device. Keep access to Chaster's official controls.

## Verification status

The code has been updated on a feature branch. It has not yet been compiled with PlatformIO or tested on physical ESP32 hardware in this environment. Confirm all four PlatformIO targets, Chaster endpoint payloads/scopes, lock permissions, and the handover sequence before relying on the firmware.
