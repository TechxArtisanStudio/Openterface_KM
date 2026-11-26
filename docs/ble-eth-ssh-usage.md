# BLE-Eth SSH Usage

This guide shows how to:

- start the local BLE-Eth TCP proxy
- open an interactive SSH session through that proxy
- run a non-interactive SSH command from another program or script

The examples below use the target-side SSH endpoint:

- target IP: `192.168.20.2` (factory default; configurable, see below)
- target port: `22`
- BLE device name: `KVM-GO`

The target IP is stored in the device's ECM IP configuration and is used by the
BLE-Eth tunnel as both the ARP destination and the TCP destination. Read it with
BLE-Eth command `0x23` (`GET_ECM_IP`) and change it with `0x22` (`SET_ECM_IP`);
the tunnel picks the new identity up immediately. `tools/ssh_through_ble_eth.py`
reads the ECM target host from the device when `--target` is omitted, so it
follows the new address without extra flags.

Use `0x23` with sub-command `0x02` to read the current target host/device
addresses before running the commands below.

## Prerequisites

Install the Python dependencies used by the BLE tools:

```bash
python3 -m pip install -r tools/requirements.txt
```

If you use the project virtual environment:

```bash
source .venv_ecm/bin/activate
python3 -m pip install -r tools/requirements.txt
```

Make sure:

- the firmware is running in BLE-Eth / ECM bridge mode
- the device is advertising over BLE
- the remote host reachable from the firmware is serving SSH on port `22`

## Reachability Check: Ping vs. TCP

The BLE-Eth tunnel carries **TCP only**. It does not forward ICMP, so
`ping <target>` through the BLE proxy cannot work — there is no ICMP path in
the protocol and no ICMP relay in the firmware.

`BLE_ETH_CMD_PING` (`0x1E`) is a protocol-level echo used to measure BLE
round-trip time; it is not a network ping.

To check reachability through the tunnel, probe the TCP port instead:

```bash
# Through the local proxy
nc -vz 127.0.0.1 18048

# Direct from the host OS on the ECM side (ICMP is answered by the bridge there)
ping 192.168.20.5
```

If the ECM interface itself must be validated, ping works on that interface
because the bridge answers ICMP Echo on both ECM links. Only the BLE proxy path
is TCP-only.

## Quick Interactive Login

The wrapper can start the BLE connection, run the local proxy, and launch `ssh` for you:

```bash
python3 tools/ssh_through_ble_eth.py --user pengtianyu
```

Useful options:

- `--ble-addr <mac-or-uuid>`: connect to a specific BLE address instead of scanning by name
- `--target <ip>`: override the default target IP
- `--target-port <port>`: override the default SSH port
- `--data-ack-policy feature`: enable strict DATA ACK mode only when the firmware advertises support
- `--trace-frames`: print BLE-Eth frame traces for debugging

The wrapper already enables the current safe SSH profile by default:

- keyboard-interactive/password auth only
- constrained cipher/KEX/MAC set
- host key checking disabled for local forwarded sessions

## Start Proxy Only

Use proxy-only mode when you want another program to connect to the SSH tunnel:

```bash
python3 tools/ssh_through_ble_eth.py --proxy-only --local-port 18048
```

When the proxy is ready it listens on:

- local address: `127.0.0.1:18048`
- remote address: `<configured target host>:22` (`192.168.20.2` by default)

Leave this command running. In another terminal, connect to the local forwarded port.

## SSH Login From Another Terminal

Interactive login through the proxy:

```bash
ssh -p 18048 pengtianyu@127.0.0.1
```

For the current transport, this more conservative SSH profile is recommended:

```bash
ssh -p 18048 \
  -o StrictHostKeyChecking=no \
  -o UserKnownHostsFile=/dev/null \
  -o ObscureKeystrokeTiming=no \
  -o PreferredAuthentications=keyboard-interactive,password \
  -o PubkeyAuthentication=no \
  -o Compression=no \
  -o KexAlgorithms=curve25519-sha256 \
  -o HostKeyAlgorithms=ssh-ed25519,rsa-sha2-512,rsa-sha2-256 \
  -o Ciphers=aes128-ctr \
  -o MACs=hmac-sha2-256 \
  pengtianyu@127.0.0.1
```

## Run a Remote SSH Command

This is the simplest way to let another program verify that the tunnel works.

Example with normal `ssh`:

```bash
ssh -p 18048 \
  -o StrictHostKeyChecking=no \
  -o UserKnownHostsFile=/dev/null \
  -o PreferredAuthentications=keyboard-interactive,password \
  -o PubkeyAuthentication=no \
  -o Compression=no \
  -o KexAlgorithms=curve25519-sha256 \
  -o HostKeyAlgorithms=ssh-ed25519,rsa-sha2-512,rsa-sha2-256 \
  -o Ciphers=aes128-ctr \
  -o MACs=hmac-sha2-256 \
  pengtianyu@127.0.0.1 \
  'echo LOGIN_OK && whoami && hostname'
```

Example with `sshpass` for scripted password login:

```bash
sshpass -p '<YOUR_PASSWORD>' ssh -p 18048 \
  -o ConnectTimeout=20 \
  -o NumberOfPasswordPrompts=1 \
  -o StrictHostKeyChecking=no \
  -o UserKnownHostsFile=/dev/null \
  -o PreferredAuthentications=keyboard-interactive,password \
  -o PubkeyAuthentication=no \
  -o Compression=no \
  -o KexAlgorithms=curve25519-sha256 \
  -o HostKeyAlgorithms=ssh-ed25519,rsa-sha2-512,rsa-sha2-256 \
  -o Ciphers=aes128-ctr \
  -o MACs=hmac-sha2-256 \
  pengtianyu@127.0.0.1 \
  'echo LOGIN_OK && whoami && hostname'
```

Expected output looks like:

```text
LOGIN_OK
pengtianyu
pengzhijiandeMac-mini.local
```

## Wrapper-Driven Banner Test

If you only want a quick transport check without opening a shell:

```bash
python3 tools/ssh_through_ble_eth.py --test-banner
```

That verifies the local proxy can open the tunnel and read the remote SSH banner.

Expected output looks like:

```text
SSH banner: SSH-2.0-OpenSSH_10.2
```

## Test-Harness Banner Test

For a repeatable “connect + banner + exit” check (useful for transport validation loops):

```bash
python3 tests/test_ble_eth_ssh.py --target 192.168.20.2 --test-only --ack-window 6
```

Notes:

- `--ack-window` requests a strict in-flight fragment window when metadata ACKs are enabled.
- The firmware may clamp the effective window via `rx_window` in the DATA ACK payload.
- `--test-only` exits with a non-zero status if the SSH banner is missing/invalid (for example when the tunnel disconnects before the banner).

## What Changed in the 2026-06-03 Fix

The previous failure mode for large terminal redraws (for example a large `top` window) was:

- missing/dropped bytes under load → SSH integrity errors (MAC/bad packet)
- occasional BLE disconnects during bursty traffic

The current stable behavior comes from a combination of firmware and host changes:

- **Cap BLE notification size for stability**: the firmware intentionally keeps BLE-Eth notifications under a “safe” length (currently 180 bytes) even if the negotiated MTU is larger.
- **Don’t lose queued bytes on notify failure**: firmware TX data is only consumed after a notification is accepted by the BLE stack.
- **Per-fragment ACK metadata + flow control**: the firmware can ACK each fragment with `[status, conn_id, seq, expected_seq, (rx_window)]`, and the host uses that to retry or throttle instead of blindly blasting fragments.
- **Deterministic CONNECT timeout**: CONNECT returns `status=0xE1` on SYN timeout/retry exhaustion instead of hanging.
- **SRAM headroom for BLE stability**: if BLE advertising disappears after disconnect, treat it as an SRAM pressure signal; the current build frees SRAM by reducing `KB_QUEUE_SIZE`.

Validation note (example stress run): a 1-second loop of `top -l 1 -n 120 -stats pid,command,cpu,mem` ran hundreds of iterations with no SSH corruption strings; one BLE disconnect occurred and the proxy resumed after reconnect.

## Safe Window Size for High-Flow Sessions

When running high-refresh terminal apps (for example `top`), larger terminal windows can push more text than the BLE tunnel can reliably carry.

Observed behavior:

- Interactive full-screen redraw apps (interactive `top`, `htop`, etc.) are worst-case because they redraw the whole screen every refresh.
- Snapshot-style commands (for example `top -l 1 ...`) are much lower risk and are recommended for stress/monitor loops.

Important: any “safe window size” is not a strict protocol limit. It is an empirical stability point that can shift with BLE conditions and host load.

### Recommended Defaults

For interactive high-flow workloads:

- start at `75x15` or smaller
- prefer lower refresh rate (`top -s 2` or slower)
- increase size gradually only after stable runs

Note: `top -s` alone may still disconnect. It reduces refresh frequency, but interactive `top` still redraws the full screen each refresh.

### If `top -s` Still Fails

Use lower-volume `top` modes that cap rows/fields per sample.

Recommended commands on the remote host:

```bash
# Single snapshot (low risk)
top -l 1 -n 15 -stats pid,command,cpu,mem
```

```bash
# Repeated snapshots every 3s (safer than interactive top)
while true; do
  top -l 1 -n 15 -stats pid,command,cpu,mem
  sleep 3
done
```

If you want a repeatable transport stress loop without interactive redraw, use the project script (run with the proxy already active):

```bash
bash tools/ble_top_poll.sh --port 18048 --interval 1 --top-n 120 --top-stats pid,command,cpu,mem
```

If you still want interactive `top`, apply all three controls together:

- small window (start from `75x15`)
- slower refresh (`top -s 3`)
- reduced fields/process count when possible (`-stats`, `-n`)

For normal shell work (non-high-refresh):

- standard terminal sizes are often fine
- if instability appears, reduce window size first

### Why Window Size Matters

Larger windows produce more bytes per redraw (text + terminal control sequences). During high refresh, this can exceed the BLE tunnel's effective stable throughput and cause retransmit pressure, timeout, or disconnect.

### Quick Calibration Workflow

1. Start with `60x12`.
2. Run your workload (for example `top`) for 3 to 5 minutes.
3. Increase size in small steps (for example +10 columns, +3 rows).
4. Stop increasing at the first disconnect/MAC error.
5. Use the previous passing size as your safe window.

Example candidate ladder:

- `60x12`
- `75x15`
- `90x20`
- `100x25`

Use at least 3 repeated runs per size before calling it stable.

## Load Behaviour With BLE Enabled

Measured with the Mac as host and a Windows machine as target, one direction
saturated at a time. Bulk throughput does not care whether the radio is on; the
cost lands on small packets:

| Measurement | BLE on | BLE off |
|---|---|---|
| TCP 8 MiB target -> host | 0.50–0.56 MiB/s | 0.55 MiB/s |
| TCP 8 MiB host -> target | 0.39–0.40 MiB/s | 0.37 MiB/s |
| ICMP while host -> target runs flat out | **37.5–50% loss**, RTT 2.7–7.8 ms | 0% loss, RTT 117–214 ms |
| ICMP while target -> host runs flat out | **10% loss, up to 361 duplicate replies** | 0% loss, no duplicates |

The duplicate replies are not a `ping` artifact: 30 packets on an idle link gave
0 loss and 0 duplicates, and the same saturation with BLE off produced neither.
Two separate runs reproduced them (+85 and +361), which points at the bridge
queuing and re-sending rather than at ICMP. The suspected mechanism is
`NCM_Bridge_Poll()` being sliced by `TMOS_SystemProcess()` in the main loop, with
the NCM endpoint dropping and retransmitting at the queue tail. **This is not
root-caused** — treat the loss as a known cost of running the radio during bulk
transfers, not as a network misconfiguration to debug.

One observation that has not been reproduced: after a long target -> host 8 MiB
run followed by an echo test, host -> target fell to 0.07–0.09 MiB/s and stayed
there across three fresh connections; a power cycle (reflash) restored 0.22–0.40
MiB/s. If one direction suddenly crawls for no reason, power-cycle the unit
before investigating anything else.

## Common Failures

`ERROR: bleak library not installed`

Install the Python requirements first:

```bash
python3 -m pip install -r tools/requirements.txt
```

`BLE device 'KVM-GO' not found`

- confirm the device is powered and advertising
- pass `--ble-addr` if scanning by name is unreliable

`failed to connect: Peer removed pairing information (CBErrorDomain Code=14)`

The host still holds a bond, but the device no longer has the matching keys.
`wchisp flash` erases and rewrites the whole code flash, and the unit comes back
without the bond it had before, so it advertises normally yet refuses the
connection from the machine that flashed it. Re-pairing does not work until the
stale bond is deleted on the host:

- macOS: System Settings -> Bluetooth -> the device -> *Forget This Device*,
  then run the client again; the next `BleEthClient.connect()` pairs from scratch
- Windows: Settings -> Bluetooth & devices -> the device -> *Remove device*
- Linux: `bluetoothctl remove <MAC>`

Do not rely on `blueutil --unpair` on macOS. It is marked EXPERIMENTAL, and on
macOS 26 it was seen exiting 0 without removing the bond: the device stays in the
paired list and the next connection fails with the same code. Use the System
Settings panel, and re-check `blueutil --paired` afterwards to confirm the entry
is actually gone.

Scanning still lists the device while the bond is stale, so a successful scan is
not proof that the connection will succeed. If several units are in range, also
check that `--ble-addr` points at the one under test before deleting anything.

`the first command after flashing gets no reply at all`

The unit has just been written by `wchisp`, so neither side holds a bond. The
first connection spends its opening seconds on SMP pairing; ATT writes issued in
that window are acknowledged by the stack but never reach the application, so
`0x1E` (`PING`) and `0x25` both come back empty while `connect()` reports
success and the services resolve normally. Pair once, drop the link, connect
again — from the second connection every command answers, and
`blueutil --paired` now lists the unit under its own address:

- symptom is silent, not an error: `write_gatt_char()` returns, no notification
  ever arrives, and a `wait_for` timeout is all you see
- a wrong unit in range looks different — it answers `57 AB 00 D0 00 D2`
  (`CMD_ERROR`) because that image predates the command, so "silence" and
  "error frame" point at different causes

`the link connects but nothing answers, and mtu_size reads 60`

Check `client.mtu_size` before blaming the firmware. A healthy link on this unit
negotiates **247**; if it reports **60**, every application write is being dropped
and no notification will ever arrive — `INFO`, `PING` and `0x25` all time out
together, with an empty frame list, while `connect()` still succeeds and the
services (and all characteristic properties) resolve normally.

This is a host-side stale identity, not a device fault. Right after a previous
connection is torn down, macOS can surface the same unit under a *second*
`CBPeripheral` UUID, without the bond association and therefore without the MTU
exchange. Scanning again a few seconds later returns the original UUID and MTU
247, and everything works:

- rule of thumb: **MTU 60 means retry, not reflash** — reconnect (or rescan) and
  the same commands answer
- `blueutil --paired` keeps showing the correct address and `paired` throughout,
  and `blueutil --info <address>` can simultaneously claim `not paired`, so
  neither output is a reliable verdict here; trust `mtu_size` instead
- the two silent failures differ in what preceded them: this one follows a
  reconnect on a bonded unit, while the first-connection-after-flash case above
  follows an erase
- cheap guard in a test loop:

  ```python
  client = BleakClient(dev, timeout=25)
  await client.connect()
  if client.mtu_size < 100:
      await client.disconnect()
      await asyncio.sleep(5)
      continue  # retry, do not raise
  ```

`the reply never arrives because the client filters on the wrong opcode`

Every positive response is sent as `cmd | 0x80` (see
[ble-eth-protocol.md](ble-eth-protocol.md)), so `PING` (`0x1E`) comes back as
`0x9E` and `INFO` (`0x1F`) as `0x9F`. A client that matches notifications against
the *request* opcode throws the replies away and reports a timeout with an empty
buffer — the same silence as the three causes above, with no way to tell them
apart until you log the raw frames:

```
57 ab 00 9e 03 01 02 03 a9     PING, payload 01 02 03, answered as 0x9E
57 ab 00 9f 08 01 02 00 f6 1f 00 00 00 c1   INFO, answered as 0x9F
```

`0x25` is the trap here: it answers `0xA5`, which *is* `0x25 | 0x80`, so a filter
derived from the portal example looks correct while `PING` and `INFO` time out.

`ERROR: BLE link disconnected while proxy was running`

- the SSH path is working, but repeated sessions can still hit intermittent BLE disconnects
- retry the session or start the proxy again
- reduce terminal window size for high-flow workloads (`top`, rapidly updating output)
- use `--trace-frames` when collecting a failure trace

`CONNECT failed: status=0xE1`

- the tunnel open timed out before the TCP path established
- retry the session
- check the target-side SSH service is reachable on `<configured target host>:22` (`192.168.20.2` by default)

## Recommended Workflow

For manual use:

1. Start proxy-only mode.
2. Open SSH from another terminal.
3. If the BLE link drops, restart the proxy and retry.

For automation:

1. Start `tools/ssh_through_ble_eth.py --proxy-only --local-port <port> --target <configured target>`.
   Omitting `--target` uses the device's configured target host.
2. Wait until the script reports the proxy is active.
3. Run your `ssh` or `sshpass ssh` command against `127.0.0.1:<port>`.
4. Stop the proxy when the command finishes.