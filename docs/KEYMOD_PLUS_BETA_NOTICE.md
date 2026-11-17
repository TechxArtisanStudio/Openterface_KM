# Keymod-Plus Beta Firmware Notice

## Overview

The **`keymod-plus-beta`** branch and its associated firmware builds are **exclusively for Keymod-Plus beta testers** participating in the official beta testing program.

## Hardware Notice: Early R&D Units

⚠️ **Important:** The beta test hardware units are **early R&D (Research & Development) prototypes**, not production units.

### Bluetooth Crystal Oscillator Limitation

These early prototypes **do not have an external crystal oscillator for the Bluetooth subsystem**. Instead, they rely on the internal RC oscillator, which has significantly lower frequency stability.

**Impact on Bluetooth Performance:**
- **Signal strength reduction: approximately 5–10 dB weaker** compared to production units
- Reduced Bluetooth range and reliability
- Potential connection instability in noisy RF environments
- Longer pairing/connection times

This is a **known hardware limitation** of the beta test units and **will be resolved in production hardware** (which includes the proper external crystal oscillator).

## What This Means for Beta Testing

| Aspect | Beta Units (keymod-plus-beta) | Production Units (keymod-plus) |
|--------|------------------------------|-------------------------------|
| Bluetooth Crystal | Internal RC oscillator | External crystal oscillator |
| Bluetooth Signal | 5–10 dB weaker | Full specification |
| BLE Range | Reduced | Full specification |
| Firmware Branch | `keymod-plus-beta` | `keymod-plus` / `main` |
| USB/HID Function | Full functionality | Full functionality |
| USB-Eth Tunnel | Full functionality | Full functionality |
| BLE-Eth Tunnel | Works (reduced range) | Full functionality |

## Firmware Differences

The `keymod-plus-beta` firmware is built with:
- `CONFIG_KEYMOD_PLUS_BETA=y` (enables internal RC oscillator config for Bluetooth)
- Same USB/HID/CH9329 functionality as production
- Debug features may be enabled for testing

## For Beta Testers

### Expected Behavior
- **USB/HID/CH9329**: Fully functional — keyboard, mouse, media keys all work normally
- **USB-Eth Tunnel (CDC-ECM)**: Fully functional — CH9329 commands over USB Ethernet work normally, no performance impact
- **Bluetooth/BLE**: Functional but with reduced range and reliability
- **BLE-Eth Tunnel**: Works but may require closer proximity due to reduced BLE signal

### USB-Eth Tunnel Testing

The **USB-Eth tunnel** (also known as CDC-ECM) is a wired Ethernet-over-USB interface that provides reliable CH9329 command transmission. This tunnel is **not affected by the Bluetooth crystal limitation** and works at full specification on beta units.

**Verified functionality:**
- Keyboard HID reports (cmd 0x02)
- Relative mouse HID reports (cmd 0x05)
- Absolute mouse HID reports (cmd 0x04)
- Media key HID reports (cmd 0x03)
- USB work mode switching (cmd 0x30)

**Test command example:**
```bash
# Send keyboard 'A' via USB-Eth tunnel
python3 tests/test_ch9329_ecm.py --iface <cdc-ecm-interface> --keyboard a

# Or via SSH to remote machine
python3 tests/test_ch9329_ecm.py --ssh --ssh-host <host> --ssh-user <user> --ssh-iface <interface> --ssh-sudo --keyboard a
```

### Testing Focus Areas
Please prioritize testing:
1. USB HID functionality (keyboard, mouse, media keys)
2. **USB-Eth tunnel** (CDC-ECM) — fully functional, recommended for reliable testing
3. CH9329 protocol over CDC-ECM
4. USB work mode switching
5. BLE connectivity (noting any range/connection issues)

### Reporting Issues
When reporting Bluetooth-related issues, please mention:
- Distance from host
- Obstructions/walls between devices
- Other 2.4 GHz devices nearby (WiFi, microwaves, etc.)
- Whether the issue persists when devices are < 1 meter apart

## Beta Firmware Release Strategy

### Important: No Official Release

The `keymod-plus-beta` firmware is **NOT officially released** through the standard release channels. This is intentional to **avoid confusion** with production unit users who should use the standard `keymod-plus` or `main` branch firmware.

### How to Get Beta Firmware

Beta firmware builds are available exclusively through **GitHub Actions** build artifacts. Every commit to the `keymod-plus-beta` branch triggers an automated build.

**Steps to download the latest beta firmware:**

1. Go to the GitHub Actions page for the `keymod-plus-beta` branch:
   ```
   https://github.com/TechxArtisanStudio/Openterface_KM/actions?query=branch%3Akeymod-plus-beta
   ```

2. Click on the latest workflow run (the most recent build at the top).

3. Scroll down to the **Artifacts** section at the bottom of the page.

4. Download the firmware artifact (usually named something like `firmware-beta` or similar).

5. Extract the downloaded archive and flash the firmware to your beta unit.

**Example build:**
- https://github.com/TechxArtisanStudio/Openterface_KM/actions/runs/44035133047

### Why GitHub Actions Only?

- **Keeps beta separate from production**: Production users won't accidentally download beta firmware
- **Always up-to-date**: Each commit triggers a new build with the latest features and fixes
- **Traceable**: Each build is linked to a specific commit hash for debugging
- **Easy access**: Beta testers can always find the latest build without waiting for official releases

### Staying Updated

To get the latest beta features and fixes:

1. **Watch the repository** on GitHub to receive notifications
2. **Check GitHub Actions** periodically for new builds on the `keymod-plus-beta` branch
3. **Report issues** when you find them — this helps us improve both beta and production firmware

---

## Production Release

Production Keymod-Plus units will:
- Include the external Bluetooth crystal oscillator
- Meet full Bluetooth specification for range and signal strength
- Use the standard `keymod-plus` or `main` branch firmware
- **Not** have the 5–10 dB signal penalty
- Be available through official release channels

---

**Branch:** `keymod-plus-beta`  
**Target Audience:** Official Keymod-Plus Beta Testers Only  
**Hardware:** Early R&D Prototype Units (No External BT Crystal)  
**Firmware Access:** GitHub Actions builds only (no official release)  
**Date:** November 2026