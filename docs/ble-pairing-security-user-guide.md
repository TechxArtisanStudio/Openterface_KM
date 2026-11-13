# BLE Pairing Code Security: User Guide

**Applies to:** KeyMod and any other Openterface KVM model whose current release
includes Bluetooth LE. KVM-GO builds currently ship without Bluetooth, so this
guide does not apply to them.

This guide explains what the Bluetooth pairing code is, how to change it, how to
reset it, and what it does and does not protect. It is written for end users.
No special tools are required for normal use.

## At a glance

| Item | Default | Notes |
| --- | --- | --- |
| Pairing code (PIN) | `666666` | Exactly six digits, `000000` to `999999` |
| Phones that can be paired | 1 | One phone, exclusive |
| Read the current code back | Paired phone or USB | Explicit query only, on an encrypted link |
| Change the code | Paired phone or USB | The current code is not required |
| Factory reset | BOOT button or command | Removes all phones and restores `666666` |

## 1. What the pairing code is

The pairing code is a six-digit Bluetooth PIN. Your phone asks for it only when
it pairs with the KVM for the first time. After that, the phone and the KVM
remember each other, and reconnecting does not ask for the code again.

The device has no screen or keypad. The code is entered on the phone, and the
KVM checks it during pairing.

## 2. Why it matters

Everything the KVM does over Bluetooth is protected by pairing:

- keyboard and mouse input sent to the target computer;
- control commands sent to the KVM;
- the optional network tunnel between the phone and the target.

A device that has not completed pairing cannot send any of these. The pairing
code is therefore the main credential that decides who may control the KVM over
Bluetooth.

Every unit ships with the same initial code, `666666`. That value is public and
is not a secret. Change it after the first pairing.

## 3. Pairing a phone for the first time

1. Make sure no other phone is currently paired. If one is, remove it first
   (see section 5) or perform a factory reset (see section 6).
2. Open your phone's Bluetooth settings and select the KVM. The default name is
   `KeyMod`. A custom name may have been set.
3. When the phone asks for a PIN, enter the current code. Out of the box this is
   `666666`.
4. Confirm the pairing. The LED confirms success (see section 9).

**Pair in a private place and change the code immediately.** Until a phone is
paired, anyone nearby who knows `666666` can take the single pairing slot. Once
a phone is paired, new pairing requests are rejected until the pairing is
removed on the KVM side or the KVM is reset.

## 4. Changing the pairing code

You can change the code in either of these ways:

- **From a phone that is already paired and connected**, using the Openterface
  companion app's security or pairing settings. The Bluetooth link must be
  encrypted, which it is for a paired phone. If your app version does not offer
  this option, use the USB command instead.
- **From a computer connected by USB**, using the command in Appendix A. This is
  an advanced option intended for integrators and recovery, and it works
  independently of the app version. It requires a model or USB mode that
  exposes the USB serial port.

Rules for the new code:

- It must be exactly six digits, `000000` to `999999`.
- The current code is not required. Access to a paired phone or to USB is the
  authorization.
- The change takes effect at the next pairing. Phones that are already paired
  keep working and do not need to re-enter anything.
- The KVM does not echo the new code when you set it. Reading it back is a
  separate, explicit action (see below).

Because a paired phone or a USB host can change the code without knowing the old
one, treat both as trusted: keep your paired phone locked, and do not leave the
KVM connected to an untrusted computer.

**Reading the current code back.** The KVM never sends the code on its own, but
it answers an explicit request from a paired phone on an encrypted Bluetooth
link, or from a computer connected by USB (Appendix A). An unpaired device, or
any unencrypted link, gets nothing. This is the same trust boundary as changing
the code: whoever holds the paired phone, or has USB access, can read it.
Firmware older than this release does not support the read command.

## 5. Adding or replacing a phone

By default the KVM accepts one phone at a time. To replace it:

1. On the old phone, use the companion app to remove or unpair the KVM. This
   erases the pairing record on the KVM. If the app does not offer this, perform
   a factory reset instead (section 6).
2. In the old phone's Bluetooth settings, forget or unpair the KVM. This step is
   important on iOS; skipping it can make the next pairing fail.
3. On the new phone, pair using the current code.

If the old phone is lost, perform a factory reset (section 6).

If the pairing limit has been raised above one (an advanced USB-only option),
lowering it again does not remove phones that are already paired. It only
prevents new pairings. Use the remove/reset function to clear existing phones.

## 6. Factory reset

A factory reset removes every paired phone, clears any custom code, and restores
the default code `666666` and the single-phone limit.

**Models with a BOOT button (KeyMod):**

1. With the device already running, hold the BOOT button for about 5 seconds.
2. The LED starts fast-blinking. Release the button.
3. The device reboots. All phones must be forgotten and paired again.

Releasing the button before 5 seconds cancels the reset. Do not hold BOOT while
powering on; that enters firmware-update mode instead of normal operation.

**App or USB:** the companion app and the USB command in Appendix A can also
erase all pairings and reset the security settings. The device replies first,
then reboots.

After a reset, remember to forget the KVM in each old phone's Bluetooth
settings before pairing a new phone.

## 7. What the pairing code protects

| Protected by pairing | Not protected by pairing |
| --- | --- |
| Bluetooth keyboard and mouse input | USB access: plugging in a cable is full control |
| Bluetooth control commands | The device name and Bluetooth address, which are public |
| The optional Bluetooth network tunnel | A paired phone itself: whoever holds it can control the KVM |
| New phones trying to pair while the slot is occupied | Someone who knows the current code before the first pairing |

The USB port is intentionally not restricted. It is the recovery path if
Bluetooth access is lost, and physical access to the KVM is already equivalent
to control of the target.

## 8. Security recommendations

- Change `666666` immediately after pairing your phone for the first time.
- Use a six-digit code that is not easy to guess.
- Pair in a private place, especially after a factory reset.
- Lock your paired phone. Anyone who can use it can control the KVM, read the
  code, and change it.
- Forget the KVM on phones you no longer use.
- If you think the code is known by someone else, change it from your paired
  phone or over USB. If you no longer trust the paired phone, perform a factory
  reset.
- Keep the KVM firmware and the companion app up to date.

## 9. LED feedback

The KeyMod LED shows the Bluetooth state.

| Event | LED behavior |
| --- | --- |
| Ready to pair / advertising | Slow blink |
| Phone connected | Solid |
| Phone disconnected | Fast blink |
| Pairing succeeded | Solid for about 1 second |
| Pairing failed or wrong code | Blinking for about 1 second |
| BOOT hold reached 5 seconds | Fast blink until release |

## 10. Troubleshooting

**A new phone cannot pair.**
Another phone is probably still paired. Remove it in the app, forget it in the
old phone's Bluetooth settings, and try again. If the old phone is lost, perform
a factory reset.

**I changed the code, but my phone still connects without asking.**
That is expected. The phone uses the key stored during its original pairing. The
code is only requested when a phone pairs for the first time.

**I forgot the current code.**
Read it back from the paired phone, or over USB with the command in Appendix A.
If neither is available to you, set a new code over USB — knowing the old one is
not required — or perform a factory reset, which restores `666666` and removes
all paired phones.

**How can I tell whether the code is still the default?**
Read it back from the paired phone or over USB (Appendix A) and compare it with
`666666`. On older firmware that cannot answer the read command, the only test
is to try pairing with `666666` while no phone is paired: success means the code
is still the default, failure means it was changed.

**The phone says pairing failed, or the old phone keeps interfering.**
Forget the KVM in the phone's Bluetooth settings, then pair again. On iOS this
step is required after removing a pairing on the KVM side.

**Can I pair more than one phone?**
By default, no: one phone, exclusive. An advanced USB-only setting can raise the
limit to up to four phones. A higher limit reduces exclusivity, so use it only
if you really need it.

---

## Appendix A: Advanced USB command reference

This section is for integrators and advanced users. Normal users should change
the code from the companion app.

Commands use the standard CH9329 frame on the KVM's USB serial (CDC ACM) port.
This requires a model or USB mode that exposes the serial port.

```text
57 AB 00 <command> <length> <payload...> <checksum>
```

The checksum is the 8-bit sum of every byte before it. Responses use
`command | 0x80` for success or data, and `command | 0xC0` for an error.

| Action | Request frame | Response |
| --- | --- | --- |
| Set the code to `123456` | `57 AB 00 22 07 04 31 32 33 34 35 36 64` | status |
| Read the current code | `57 AB 00 22 01 07 2C` | `status` + six ASCII digits |
| Read pairing status | `57 AB 00 22 01 03 28` | `status, phone count, limit` |
| Read the phone limit | `57 AB 00 22 01 06 2B` | `status, limit` |
| Set the phone limit to 4 | `57 AB 00 22 02 05 04 2F` | status |
| Erase all phones and reset | `57 AB 00 22 01 02 27` | status, then reboot |

Notes:

- The code payload is always the sub-command `04` followed by six ASCII digits.
  For example, `123456` is `31 32 33 34 35 36`.
- Setting the phone limit is accepted only over USB. Other security commands are
  also accepted from a paired phone over an encrypted Bluetooth link.
- Erasing all phones also clears a custom code and restores the limit to one.
- Setting the code does not echo it; the reply only confirms that the new code
  was stored. Use the read command to retrieve it. The read command answers with
  the code currently in effect — your custom code, or `666666` if none was set.

Status values:

| Value | Meaning |
| --- | --- |
| `00` | Success |
| `E3` | Unsupported command |
| `E5` | Invalid parameter |
| `E9` | Flash erase failed |
| `EA` | Flash program failed |
| `EB` | Flash verification failed |
