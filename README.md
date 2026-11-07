# Chaster ESP32

An ESP32 web dashboard starter for reading Chaster lock information, based on the workflow of the EmlaLock ESP32 project.

## Supported boards

PlatformIO environments are included for ESP32 DevKit, ESP32-C3, ESP32-S2 and ESP32-S3. The default environment is esp32dev.

## Current features

- Wi-Fi setup access point named Chaster-ESP32-Setup
- Stores Wi-Fi details and Chaster token in ESP32 NVS
- Bearer-token authentication with the Chaster Public API
- Reads the user's locks using GET /locks, or one lock using GET /locks/{lockId}
- Refreshes data every 30 seconds
- Handover action clears saved credentials and restarts setup mode
- Mobile-friendly local dashboard

## Build and flash

Install Visual Studio Code and PlatformIO, open this repository, connect the board and select its matching environment.

Example commands:

    pio run -e esp32dev
    pio run -e esp32dev -t upload

Use esp32c3, esp32s2 or esp32s3 for the other boards.

## Chaster API setup

Generate a developer token in your Chaster developer interface. The token needs the lock read scope (locks:read, or legacy locks scope). On first boot, connect to the Chaster-ESP32-Setup Wi-Fi network and open http://192.168.4.1. Enter your home Wi-Fi credentials, Chaster token and optionally a lock ID. Leave the lock ID blank to request the account lock list.

## Time controls

The current firmware implements authenticated lock reads only. Chaster documents POST /locks/{lockId}/update-time, but its request DTO and permission model must be checked against the live OpenAPI schema before add/remove time controls are implemented. These controls are intentionally omitted rather than sending an unverified payload that could alter a live session.

## Security

- Treat your Chaster token like a password. Never commit it or share it in screenshots.
- The token is stored in ESP32 NVS. Use the handover action before giving the device to another person.
- The local dashboard has no login/password. Use only on a trusted private network.
- HTTPS certificate validation is a release TODO in the firmware; insecure TLS mode must be replaced before exposing the device to untrusted networks.
- This is not a safety or emergency-unlock device. Keep access to Chaster's official controls.

## References

- https://docs.chaster.app/api/basics/introduction
- https://docs.chaster.app/api/public-api/endpoints/
- https://api.chaster.app/api
