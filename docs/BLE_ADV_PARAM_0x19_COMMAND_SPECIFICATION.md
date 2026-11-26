# BLE advertising parameter command (0x19) full specification

## 1. Overview

Command code `0x19` controls and queries BLE advertising parameters, with the following functions:

| Parameter type | Function | Status |
|---------|------|------|
| 0x01 | Advertising channel control | ✅ implemented |
| 0x02 | Transmit power control | ✅ implemented |
| 0x03 | BLE test mode | ✅ implemented |

---

## 2. Command frame format

### 2.1 Basic frame structure

```
[header1] [header2] [address] [command] [length] [data...] [checksum]
 57        AB        00        19        N         ...       XX
```

### 2.2 Checksum calculation

```
checksum = (header1 + header2 + address + command + length + all data bytes) & 0xFF
```

---

## 3. Function details

### 3.1 Advertising channel control (param_type = 0x01)

#### Description
Controls which BLE advertising channels (CH37/CH38/CH39) are used.

#### BLE advertising channels

| Channel | Frequency | Bit mask |
|------|------|--------|
| CH37 | 2402 MHz | bit0 = 0x01 |
| CH38 | 2426 MHz | bit1 = 0x02 |
| CH39 | 2480 MHz | bit2 = 0x04 |

#### Command format

| Sub-command | Function | Data format |
|--------|------|----------|
| 0x00 | Get the current channel map | `57 AB 00 19 02 00 01 [checksum]` |
| 0x01 | Set the channel map | `57 AB 00 19 03 01 01 [value] [checksum]` |

#### Channel map values

| Value | Meaning | Channels used |
|----|------|----------|
| 0x01 | CH37 only | 2402 MHz |
| 0x02 | CH38 only | 2426 MHz |
| 0x04 | CH39 only | 2480 MHz |
| 0x03 | CH37 + CH38 | 2402, 2426 MHz |
| 0x05 | CH37 + CH39 | 2402, 2480 MHz |
| 0x06 | CH38 + CH39 | 2426, 2480 MHz |
| 0x07 | all channels | 2402, 2426, 2480 MHz |

#### Response format

```
success: 57 AB 00 D9 02 [param_type] [value] [checksum]
failure: 57 AB 00 D9 01 [error_code] [checksum]
```

#### Checksum formulas

```
get command: checksum = (0x57 + 0xAB + 0x00 + 0x19 + 0x02 + 0x00 + 0x01) & 0xFF = 0x1E
set command: checksum = (0x57 + 0xAB + 0x00 + 0x19 + 0x03 + 0x01 + 0x01 + value) & 0xFF = (0x20 + value) & 0xFF
```

#### Complete command codes

| Function | Command code |
|------|--------|
| Get the channel map | `57 AB 00 19 02 00 01 1E` |
| Set CH37 only | `57 AB 00 19 03 01 01 01 21` |
| Set CH38 only | `57 AB 00 19 03 01 01 02 22` |
| Set CH39 only | `57 AB 00 19 03 01 01 04 24` |
| Set all channels | `57 AB 00 19 03 01 01 07 27` |

#### Constraints
- Cannot be changed while the device is connected (returns the error `STATUS_ERR_FRAME`)
- A value of 0 is automatically changed to 0x07 (all channels)

---

### 3.2 Transmit power control (param_type = 0x02)

#### Description
Controls BLE transmit power, ranging from -18 dBm to +7 dBm.

#### Power level table

| Power value | Macro | Hex |
|--------|--------|----------|
| -18 dBm | LL_TX_POWEER_MINUS_18_DBM | 0x01 |
| -10 dBm | LL_TX_POWEER_MINUS_10_DBM | 0x03 |
| -5 dBm | LL_TX_POWEER_MINUS_5_DBM | 0x05 |
| -3 dBm | LL_TX_POWEER_MINUS_3_DBM | 0x07 |
| 0 dBm | LL_TX_POWEER_0_DBM | 0x09 |
| +1 dBm | LL_TX_POWEER_1_DBM | 0x0B |
| +2 dBm | LL_TX_POWEER_2_DBM | 0x0D |
| +3 dBm | LL_TX_POWEER_3_DBM | 0x11 |
| +4 dBm | LL_TX_POWEER_4_DBM | 0x15 |
| +5 dBm | LL_TX_POWEER_5_DBM | 0x1B |
| +6 dBm | LL_TX_POWEER_6_DBM | 0x25 |
| +7 dBm | LL_TX_POWEER_7_DBM | 0x3F |

#### Command format

| Sub-command | Function | Data format |
|--------|------|----------|
| 0x00 | Get the current power | `57 AB 00 19 02 00 02 [checksum]` |
| 0x01 | Set the power | `57 AB 00 19 03 01 02 [power] [checksum]` |

#### Checksum formulas

```
get command: checksum = (0x57 + 0xAB + 0x00 + 0x19 + 0x02 + 0x00 + 0x02) & 0xFF = 0x1F
set command: checksum = (0x57 + 0xAB + 0x00 + 0x19 + 0x03 + 0x01 + 0x02 + power) & 0xFF = (0x21 + power) & 0xFF
```

#### Complete command codes

| Function | Command code |
|------|--------|
| Get the current power | `57 AB 00 19 02 00 02 1F` |
| Set -18 dBm | `57 AB 00 19 03 01 02 01 22` |
| Set -10 dBm | `57 AB 00 19 03 01 02 03 24` |
| Set -5 dBm | `57 AB 00 19 03 01 02 05 26` |
| Set -3 dBm | `57 AB 00 19 03 01 02 07 28` |
| Set 0 dBm | `57 AB 00 19 03 01 02 09 2A` |
| Set +1 dBm | `57 AB 00 19 03 01 02 0B 2C` |
| Set +2 dBm | `57 AB 00 19 03 01 02 0D 2E` |
| Set +3 dBm | `57 AB 00 19 03 01 02 11 32` |
| Set +4 dBm | `57 AB 00 19 03 01 02 15 36` |
| Set +5 dBm | `57 AB 00 19 03 01 02 1B 3C` |
| Set +6 dBm | `57 AB 00 19 03 01 02 25 46` |
| Set +7 dBm | `57 AB 00 19 03 01 02 3F 60` |

#### Response format

```
success: 57 AB 00 D9 02 [param_type] [power_value] [checksum]
failure: 57 AB 00 D9 01 [error_code] [checksum]
```

#### Constraints
- Cannot be changed while the device is connected (returns the error `STATUS_ERR_FRAME`)
- The power value must be one of the valid values above, otherwise `STATUS_ERR_PARAM` is returned

---

### 3.3 BLE test mode (param_type = 0x03)

#### Description
Starts BLE test mode, transmitting a continuous RF signal at the given frequency for spectrum analysis.

#### Test bands

| Band selection | Frequency | Test channel | Notes |
|----------|------|----------|------|
| 0 | 2402 MHz | 0 | low band |
| 1 | 2440 MHz | 19 | mid band |
| 2 | 2480 MHz | 39 | high band |

#### Frequency formula

```
frequency = 2402 + 2 × test channel (MHz)
```

#### Command format

| Sub-command | Function | Data format |
|--------|------|----------|
| 0x00 | Start test mode | `57 AB 00 19 04 00 03 [band] [power] [checksum]` |
| 0x01 | Stop test mode | `57 AB 00 19 02 01 03 [checksum]` |

#### Checksum formula (test mode start command)

```
checksum = (0x22 + band + power) & 0xFF
```

where `0x22` is the low 8 bits of the sum of the fixed prefix `57 AB 00 19 04 00 03`.

---

## 4. Complete command code table for test mode

### 4.1 Low band 2402 MHz (band=0)

| Power | Power value | Complete command code |
|------|--------|------------|
| -18 dBm | 0x01 | `57 AB 00 19 04 00 03 00 01 23` |
| -10 dBm | 0x03 | `57 AB 00 19 04 00 03 00 03 25` |
| -5 dBm | 0x05 | `57 AB 00 19 04 00 03 00 05 27` |
| -3 dBm | 0x07 | `57 AB 00 19 04 00 03 00 07 29` |
| 0 dBm | 0x09 | `57 AB 00 19 04 00 03 00 09 2B` |
| +1 dBm | 0x0B | `57 AB 00 19 04 00 03 00 0B 2D` |
| +2 dBm | 0x0D | `57 AB 00 19 04 00 03 00 0D 2F` |
| +3 dBm | 0x11 | `57 AB 00 19 04 00 03 00 11 33` |
| +4 dBm | 0x15 | `57 AB 00 19 04 00 03 00 15 37` |
| +5 dBm | 0x1B | `57 AB 00 19 04 00 03 00 1B 3D` |
| +6 dBm | 0x25 | `57 AB 00 19 04 00 03 00 25 47` |
| +7 dBm | 0x3F | `57 AB 00 19 04 00 03 00 3F 61` |

### 4.2 Mid band 2440 MHz (band=1)

| Power | Power value | Complete command code |
|------|--------|------------|
| -18 dBm | 0x01 | `57 AB 00 19 04 00 03 01 01 24` |
| -10 dBm | 0x03 | `57 AB 00 19 04 00 03 01 03 26` |
| -5 dBm | 0x05 | `57 AB 00 19 04 00 03 01 05 28` |
| -3 dBm | 0x07 | `57 AB 00 19 04 00 03 01 07 2A` |
| 0 dBm | 0x09 | `57 AB 00 19 04 00 03 01 09 2C` |
| +1 dBm | 0x0B | `57 AB 00 19 04 00 03 01 0B 2E` |
| +2 dBm | 0x0D | `57 AB 00 19 04 00 03 01 0D 30` |
| +3 dBm | 0x11 | `57 AB 00 19 04 00 03 01 11 34` |
| +4 dBm | 0x15 | `57 AB 00 19 04 00 03 01 15 38` |
| +5 dBm | 0x1B | `57 AB 00 19 04 00 03 01 1B 3E` |
| +6 dBm | 0x25 | `57 AB 00 19 04 00 03 01 25 48` |
| +7 dBm | 0x3F | `57 AB 00 19 04 00 03 01 3F 62` |

### 4.3 High band 2480 MHz (band=2)

| Power | Power value | Complete command code |
|------|--------|------------|
| -18 dBm | 0x01 | `57 AB 00 19 04 00 03 02 01 25` |
| -10 dBm | 0x03 | `57 AB 00 19 04 00 03 02 03 27` |
| -5 dBm | 0x05 | `57 AB 00 19 04 00 03 02 05 29` |
| -3 dBm | 0x07 | `57 AB 00 19 04 00 03 02 07 2B` |
| 0 dBm | 0x09 | `57 AB 00 19 04 00 03 02 09 2D` |
| +1 dBm | 0x0B | `57 AB 00 19 04 00 03 02 0B 2F` |
| +2 dBm | 0x0D | `57 AB 00 19 04 00 03 02 0D 31` |
| +3 dBm | 0x11 | `57 AB 00 19 04 00 03 02 11 35` |
| +4 dBm | 0x15 | `57 AB 00 19 04 00 03 02 15 39` |
| +5 dBm | 0x1B | `57 AB 00 19 04 00 03 02 1B 3F` |
| +6 dBm | 0x25 | `57 AB 00 19 04 00 03 02 25 49` |
| +7 dBm | 0x3F | `57 AB 00 19 04 00 03 02 3F 63` |

### 4.4 Stop test mode

| Function | Complete command code |
|------|------------|
| Stop test mode | `57 AB 00 19 02 01 03 21` |

---

## 5. Response format

### 5.1 Channel control response (param_type=0x01)

```
success: 57 AB 00 D9 02 01 [channel_map] [checksum]
```

### 5.2 Power control response (param_type=0x02)

```
success: 57 AB 00 D9 02 02 [power_value] [checksum]
```

### 5.3 Test mode response (param_type=0x03)

**Start response (4 bytes of data):**
```
57 AB 00 D9 04 03 [status] [ret_code] [actual_power] [checksum]
```
- status: 0x01 = success, 0x02 = fallback succeeded
- ret_code: BLE API return value
- actual_power: the power value actually used

**Stop response (3 bytes of data):**
```
57 AB 00 D9 03 03 [status] [ret_code] [checksum]
```

---

## 6. Error codes

| Error code | Value | Notes |
|--------|-----|------|
| STATUS_SUCCESS | 0x00 | success |
| STATUS_ERR_PARAM | 0xE5 | bad parameter |
| STATUS_ERR_FRAME | 0xE6 | frame error (cannot change while the device is connected) |

---

## 7. Implementation details

### 7.1 Code location

- **File**: `APP/peripheral.c`
- **Function**: `Peripheral_BleAdvParamCommand()`
- **Call site**: `CH9329_DataParser()` handling CMD_BLE_ADV_PARAM

### 7.2 Key variables

```c
// current transmit power (tracked at runtime)
static uint8_t g_current_tx_power = BLE_TX_POWER;  // default 0 dBm

// test mode flag
static uint8_t g_test_mode_active = 0;
```

### 7.3 Test mode flow

```
start test mode:
1. check whether the device is connected (refuse if it is)
2. validate the band parameter (0/1/2)
3. validate the power parameter (must be a valid value)
4. stop advertising
5. apply the requested power
6. call API_LE_TransmitterTestCmd() to start the test
7. set g_test_mode_active = 1

stop test mode:
1. call API_LE_TestEndCmd() to end the test
2. restore the default power
3. restart advertising
4. clear g_test_mode_active = 0
```

### 7.4 BLE test API

```c
// BLE 5.x test command
uint8_t test_params[4];
test_params[0] = tx_channel;      // TX channel
test_params[1] = 255;             // data length (maximum)
test_params[2] = 4;               // payload type: continuous carrier
test_params[3] = 1;               // PHY: LE 1M
ret = API_LE_TransmitterTestCmd(test_params, 0x2034);

// BLE 4.x fallback
if (ret != 0) {
    uint8_t test_params_v4[3];
    test_params_v4[0] = tx_channel;
    test_params_v4[1] = 37;       // BLE 4.x maximum length
    test_params_v4[2] = 4;        // continuous carrier
    ret = API_LE_TransmitterTestCmd(test_params_v4, 0x201E);
}
```

---

## 8. Usage notes

### 8.1 Channel control
- Disconnect first before changing the channel map
- At least one channel must be enabled
- Advertising restarts automatically after the change

### 8.2 Power control
- Disconnect first before changing the power
- The power value must be one of the predefined valid values
- In test mode the power setting may not take effect (hardware limitation)

### 8.3 Test mode
- The device stops advertising in test mode
- Advertising resumes automatically after the test stops
- Test mode uses a continuous carrier, which appears as a single-frequency signal on a spectrum analyser
- High-power tests may produce harmonic spurs

---

## 9. Testing recommendations

### 9.1 Basic test order

1. **Confirm the link works**
   ```
   57 AB 00 19 02 00 02 1F    # get the current power
   ```

2. **Test each band**
   ```
   57 AB 00 19 04 00 03 00 09 2B  # 2402MHz, 0dBm
   57 AB 00 19 04 00 03 01 09 2C  # 2440MHz, 0dBm
   57 AB 00 19 04 00 03 02 09 2D  # 2480MHz, 0dBm
   ```

3. **Test power adjustment**
   ```
   57 AB 00 19 04 00 03 00 01 23  # 2402MHz, -18dBm
   57 AB 00 19 04 00 03 00 3F 61  # 2402MHz, +7dBm
   ```

4. **Stop the test**
   ```
   57 AB 00 19 02 01 03 21
   ```

### 9.2 Suggested spectrum analyser settings

| Parameter | Suggested value |
|------|--------|
| Centre frequency | 2440 MHz |
| Frequency span | 80 MHz |
| Reference level | -20 dBm |
| Resolution bandwidth | 100 kHz |

---

## 10. Version history

| Version | Date | Change |
|------|------|----------|
| 1.0 | 2024-09 | initial version, channel control and power control |
| 1.1 | 2024-09 | added test mode support |
| 1.2 | 2024-09 | test mode gained a power parameter |
| 1.3 | 2024-09 | corrected the mid band to 2440 MHz |
| 1.4 | 2024-09 | fixed checksum errors in every command code |

---

## 11. Related files

- `APP/peripheral.c` - command handling implementation
- `APP/include/usbd_composite_km.h` - command code definitions
- `LIB/wchble.h` - BLE API and power macros
- `HAL/include/config.h` - default configuration

---

*Document date: 2024-09-21*
