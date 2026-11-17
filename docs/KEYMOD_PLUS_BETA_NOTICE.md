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

## Firmware Differences

The `keymod-plus-beta` firmware is built with:
- `CONFIG_KEYMOD_PLUS_BETA=y` (enables internal RC oscillator config for Bluetooth)
- Same USB/HID/CH9329 functionality as production
- Debug features may be enabled for testing

## For Beta Testers

### Expected Behavior
- **USB/HID/CH9329/ECM**: Fully functional — keyboard, mouse, media keys all work normally
- **Bluetooth/BLE**: Functional but with reduced range and reliability
- **BLE-Eth tunnel**: Works but may require closer proximity

### Testing Focus Areas
Please prioritize testing:
1. USB HID functionality (keyboard, mouse, media keys)
2. CH9329 protocol over CDC-ECM
3. USB work mode switching
4. BLE connectivity (noting any range/connection issues)

### Reporting Issues
When reporting Bluetooth-related issues, please mention:
- Distance from host
- Obstructions/walls between devices
- Other 2.4 GHz devices nearby (WiFi, microwaves, etc.)
- Whether the issue persists when devices are < 1 meter apart

## Production Release

Production Keymod-Plus units will:
- Include the external Bluetooth crystal oscillator
- Meet full Bluetooth specification for range and signal strength
- Use the standard `keymod-plus` or `main` branch firmware
- **Not** have the 5–10 dB signal penalty

---

**Branch:** `keymod-plus-beta`  
**Target Audience:** Official Keymod-Plus Beta Testers Only  
**Hardware:** Early R&D Prototype Units (No External BT Crystal)  
**Date:** November 2026