# Pairing Code and Matching Mechanism Guide

## Overview

This document describes the complete BLE pairing code system and device matching mechanism for the Openterface KM project. It covers how pairing works, how matching codes are configured, and how devices are identified and matched.

---

## 1. BLE Pairing Configuration (Flash-Persistent)

### Location
- ** Firmware**: `APP/ble_pair_config.c` and `APP/include/ble_pair_config.h`
- **Flash address**: `0x0806A000` (4 KB page)

### Record Layout
The pairing configuration stores these values in a single Flash record:

| Field | Size | Description |
|-------|------|-------------|
| magic | 4 bytes | `0x50454C42` ("BLEP") |
| record_version | 1 byte | Version number (currently 1) |
| max_bonds | 1 byte | Maximum bond slots (1-4) |
| has_pin_override | 1 byte | Whether a PIN override is set (0 or 1) |
| padding | 1 byte | Alignment padding |
| pin_override | 4 bytes | Custom PIN code (6 digits max, 0 if no override) |
| sequence | 4 bytes | Write sequence number (starts at 1) |
| crc | 4 bytes | FNV-1a CRC over all fields except crc |

### Default Values (Factory)
- **Default PIN**: `666666` (`BLE_PAIR_DEFAULT_PIN`)
- **Default max_bonds**: `1` (`BLE_PAIR_MIN_BONDS`)
- **No PIN override** by default

### PIN Override Operations

The PIN override is set via CH9329 command `0x22` sub-function `0x04`:

```c
// Set PIN override (exactly 6 ASCII digits)
case 0x04:
    if (data_len != 7) { /* error */ }
    pin = pdata[1]..pdata[6];  // 6 digits
    BLE_PairConfig_SetPin(pin);  // Persists to flash
    // Also applied to GAPBOND_PERI_DEFAULT_PASSCODE for new pairings
```

The effective PIN is obtained via:
```c
case 0x07:  /* Read the effective PIN as six ASCII digits */
    pin = BLE_PairConfig_GetEffectivePin();  // Returns override or 666666
    // Format as 6 ASCII digits
```

### Bond Slot Limit Operations

Configure maximum number of bonded devices (1-4):

```c
// Set slot limit
case 0x05:  /* Set slot limit — USB only */
    max_bonds = pdata[1];
    BLE_PairConfig_SetMaxBonds(max_bonds);
    Peripheral_RefreshPairingMode();  // Updates pairing mode state

// Read slot limit
case 0x06:  /* Read slot limit */
    response[0] = STATUS_SUCCESS;
    response[1] = BLE_PairConfig_GetMaxBonds();
```

### Factory Reset
CH9329 command `0x22` sub-function `0x02` erases:
- All bonds
- PIN override
- Resets max_bonds to 1

```c
case 0x02:  /* Erase all bonds + PIN override + slot limit */
    Peripheral_FactoryResetSecurity();  // Erases flash record
    Peripheral_RefreshPairingMode();
    Peripheral_ScheduleSystemReset(500);
```

---

## 2. BLE Device Name Configuration

### Location
- **Firmware**: `APP/ble_name_config.c` and `APP/peripheral.c`
- **Flash address**: `0x0806D000` (third-to-last 4 KB page)

### Default Names (by Board Version)
| Board Version | Default BLE Name |
|--------------|------------------|
| v1 (kvmgo) | `KVM-GO` |
| v2 (minikvm) | `Mini-KVM` |
| v3 (keymod-plus, keymod-plus-beta, keymod-mini) | `KeyMod` |

### Name Change Operations
Device name can be changed via CH9329 command `0x23`:

```c
case CMD_BLE_SET_NAME:  /* 0x23 - Set BLE device name */
    // pdata[1..n] = new name bytes
    // Persists to flash at 0x0806D000
```

The runtime device name is constructed in `APP/peripheral.c::Peripheral_Init()`:
- Reads from `g_ble_name.name` (initialized from flash or compile-time default)
- Writes to both advertisement data and scan response data
- Limited to 20 characters

---

## 3. Device Matching Mechanism

### Host-Side Matching (Python)

The device matching logic resides in `tools/ble_eth_tunnel.py::_device_matches()`:

```python
def _device_matches(discovered, name_hint: str) -> bool:
    """Return True when a discovered BLE device likely is an Openterface target."""
    aliases = {"kvm-go", "keymod", "openterface"}
    hint = (name_hint or "").strip().lower()
    if hint:
        aliases.add(hint)

    name = (getattr(discovered, "name", "") or "").lower()
    if any(alias in name for alias in aliases):
        return True

    metadata = getattr(discovered, "metadata", {}) or {}
    service_uuids = metadata.get("uuids") or metadata.get("service_uuids") or []
    normalized = {str(uuid).lower() for uuid in service_uuids}
    if SERVICE_UUID.lower() in normalized or "0000fff0" in " ".join(normalized):
        return True

    return False
```

**Matching Criteria**:
1. **Name substring match**: Device name (lowercased) contains any of: `kvm-go`, `keymod`, `openterface`, or the optional `name_hint`
2. **Service UUID match**: Advertised BLE service UUIDs contain `0000fff0-0000-1000-8000-00805f9b34fb`

### Windows Test Matching

`tests/test_usb_mode_ble_windows.py::matches_device()`:

```python
def matches_device(device, advertisement, name_matches: list[str], service_uuid: str) -> bool:
    name = (device.name or advertisement.local_name or "").lower()
    services = [item.lower() for item in (advertisement.service_uuids or [])]
    return any(match in name for match in name_matches) or service_uuid.lower() in services
```

**Parameters**:
- `name_matches`: List of case-insensitive name substrings (default: `["kvm-go", "keymod"]`)
- `service_uuid`: Expected service UUID string

### Bleak Scanner Integration

The matching function is used during BLE device discovery:

```python
# In BleEthClient.connect():
devices = await BleakScanner.discover(timeout=5)
device = None
for discovered in devices:
    if _device_matches(discovered, self.device_name):
        device = discovered
        break
```

---

## 4. How Matching Code is Modified

The matching mechanism can be modified through these pathways:

### 4.1 Firmware: BLE Device Name

**File**: `APP/peripheral.c` / `APP/ble_name_config.c`

The advertised BLE name determines which devices the host-side matcher identifies:

```c
// peripheral.c: Peripheral_Init()
// Advertises the device name in both advertisement and scan response data

uint8_t nameLen = strlen(g_ble_name.name);
uint8_t deviceNameLen = sizeof(attDeviceName) - 1;

// Add device name to advertisement data
advertData[advertDataLen++] = nameLen + 1;
advertData[advertDataLen++] = GAP_ADTYPE_LOCAL_NAME_COMPLETE;
tmos_memcpy(&advertData[advertDataLen], g_ble_name.name, nameLen);
advertDataLen += nameLen;
```

**How to change**: 
- Flash a new BLE name via CH9329 command `0x23`
- Or rebuild firmware with a different `BLE_DEVICE_NAME` compile-time define
- Board-specific defaults in `build/makefile`:

```makefile
# BLE name per board version
ifeq ($(BOARD_VERSION),1)
BLE_NAME ?= KVM-GO
else ifeq ($(BOARD_VERSION),2)
BLE_NAME ?= Mini-KVM
else
BLE_NAME ?= KeyMod
endif
```

### 4.2 Host-Side: Matching Aliases

**File**: `tools/ble_eth_tunnel.py`

Add or modify the alias set:

```python
def _device_matches(discovered, name_hint: str) -> bool:
    aliases = {"kvm-go", "keymod", "openterface"}  # Modify this set
    ...
```

### 4.3 Host-Side: Service UUID

The service UUID `0000fff0-0000-1000-8000-00805f9b34fb` is hardcoded in `tools/ble_eth_tunnel.py`. If the firmware's advertised service UUID changes, this constant must be updated.

### 4.4 Windows Test: Name/UUID Parameters

**File**: `tests/test_usb_mode_ble_windows.py`

The `matches_device()` function accepts `name_matches` and `service_uuid` as configurable parameters, allowing test-specific matching rules.

---

## 5. Complete Pairing Flow

```
1. Device powers up → advertises BLE name (KVM-GO/Mini-KVM/KeyMod)
2. Host discovers device via _device_matches() 
   - Checks name contains alias (kvm-go/keymod/openterface)
   - Checks service UUID 0000fff0
3. Host initiates connection
4. Firmware handles security request (0x22)
5. PIN entry: 666666 default, or overridden PIN if configured
6. Bonding: 1-4 bonds possible, configured via 0x05/0x06
7. Connection encrypted with paired LTK
```

---

## 6. Configuration Summary

| Aspect | Default | Configurable Via |
|--------|---------|------------------|
| **Default PIN** | 666666 | Flash (ble_pair_config.c) or CH9329 0x22/0x04 |
| **Max Bonds** | 1 | Flash (ble_pair_config.c) or CH9329 0x22/0x05 |
| **BLE Device Name** | Board-dependent | Flash (ble_name_config.c) or CH9329 0x23 |
| **Matching Aliases** | kvm-go, keymod, openterface | tools/ble_eth_tunnel.py |
| **Service UUID** | 0000fff0-0000-1000-8000-00805f9b34fb | tools/ble_eth_tunnel.py (must match firmware) |

---

## 7. Key Code References

### Firmware
- `APP/ble_pair_config.c` - Flash-persistent pairing config (PIN, bonds)
- `APP/ble_name_config.c` - Flash-persistent BLE name
- `APP/peripheral.c` - BLE initialization, security commands (0x22)
- `APP/include/ble_pair_config.h` - Pairing config headers/defines
- `APP/include/ble_name_config.h` - BLE name config headers/defines

### Host/Tools
- `tools/ble_eth_tunnel.py::_device_matches()` - Device matching logic
- `tools/ble_eth_tunnel.py` - BLE-Eth tunnel client with device discovery
- `tests/test_usb_mode_ble_windows.py::matches_device()` - Windows test matching
- `tests/conftest.py::KNOWN_BLE_NAMES` - Test known BLE names