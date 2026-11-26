/**
 * mac_uid.c — derive every Ethernet address this firmware uses from the chip's
 * factory-programmed 96-bit unique ID.
 *
 * WHY
 * ---
 * The bridge used to ship a fixed set of addresses (02:00:00:00:00:01,
 * ...:00:02, ...:03:00, ...:00:32, ...:03:32).  Every unit therefore looked
 * identical on the wire: two dongles could not be told apart by a Host that
 * keeps ARP/DHCP state across re-plug, and a bench with several units could
 * hand out conflicting addresses.  Provisioning a per-unit MAC at the factory
 * would need a flash page and a production step; the UID is already unique,
 * immutable and free to read, so a hash of it is enough.  The address stays
 * stable across reboots and re-flashes, which is what matters for DHCP leases
 * and for the Host's "new network interface" bookkeeping.
 *
 * SHAPE
 * -----
 *     02 : hh hh hh hh : rr
 *
 *   02      locally administered (bit 1 set) and unicast (bit 0 clear).  We do
 *           not own an OUI, and an LA address is reserved for exactly this, so
 *           it can never collide with a real vendor block.
 *   hh x4   FNV-1a over the twelve UID bytes.
 *   rr      role.  Hard-coded per role so the four addresses of a unit are
 *           distinct by construction (see mac_uid.h for why that matters).
 *
 * Two units collide only if the 32-bit hash collides: 1/2^32 per pair, a
 * birthday problem only at ~77k units.  A collision also has to be observed
 * to matter, i.e. both units on one layer-2 segment, and each bridge link is
 * a point-to-point cable.  The remaining hole is the blank-UID fallback below,
 * which still makes affected units identical.
 *
 * BLANK UID
 * ---------
 * Engineering samples sometimes return all-0x00 or all-0xFF.  Rather than
 * hand a host a degenerate address, fall back to the legacy fixed value, i.e.
 * the unit behaves exactly like the pre-UID firmware.
 */

#include "mac_uid.h"

/* 96-bit unique ID.  Measured on a CH32V208GBU6: A5-33-B4-10-53-5C-AC-A0 (the
 * ISP bootloader exposes the first eight bytes).  Note that this is NOT a MAC:
 * 0xA5 has the multicast bit set.  The SDK's FLASH_GetMACAddress() reads the
 * same offset and would therefore return a multicast address on this part --
 * it is a leftover from the Ethernet-capable siblings, so the bytes have to be
 * hashed rather than copied.  Volatile because it is a read-only system area,
 * not RAM. */
#define MAC_UID_BASE      0x1FFFF7E8u
#define MAC_UID_WORDS     3u
#define MAC_UID_BYTES     (MAC_UID_WORDS * 4u)   /* 12 */

static uint8_t s_mac[MAC_ROLE_COUNT][MAC_ADDR_LEN];
static uint8_t s_ready;

static uint8_t mac_uid_byte(uint8_t index)
{
    uint32_t word = *(volatile const uint32_t *)(MAC_UID_BASE + 4u * (index >> 2));
    return (uint8_t)(word >> (8u * (index & 3u)));
}

void MacUid_Init(void)
{
    /* Role byte, indexed by MacRole.  0x01 / 0x32 / 0x00 / 0x33 keep the
     * distinction the bench work established: NIC != PEER on both links, and
     * the two links differ from each other so a frame that loops through the
     * bridge is not mistaken for our own. */
    static const uint8_t role_byte[MAC_ROLE_COUNT] = {
        0x01,   /* MAC_ROLE_HOST_NIC    */
        0x32,   /* MAC_ROLE_HOST_PEER   */
        0x00,   /* MAC_ROLE_TARGET_NIC  */
        0x33    /* MAC_ROLE_TARGET_PEER */
    };

    uint32_t hash = 2166136261u;    /* FNV-1a offset basis */
    uint8_t  all_zero = 1u;
    uint8_t  all_ones = 1u;
    uint8_t  i;

    for (i = 0; i < MAC_UID_BYTES; i++) {
        uint8_t b = mac_uid_byte(i);
        if (b != 0x00u) all_zero = 0u;
        if (b != 0xFFu) all_ones = 0u;
        hash ^= b;
        hash *= 16777619u;
    }

    if (all_zero || all_ones) {
        hash = 0u;                  /* legacy fixed addresses */
    }

    for (i = 0; i < MAC_ROLE_COUNT; i++) {
        s_mac[i][0] = 0x02u;
        s_mac[i][1] = (uint8_t)(hash >> 24);
        s_mac[i][2] = (uint8_t)(hash >> 16);
        s_mac[i][3] = (uint8_t)(hash >> 8);
        s_mac[i][4] = (uint8_t)(hash);
        s_mac[i][5] = role_byte[i];
    }

    s_ready = 1u;
}

const uint8_t *MacUid_Get(MacRole role)
{
    if (!s_ready) {
        MacUid_Init();
    }
    if ((unsigned)role >= MAC_ROLE_COUNT) {
        role = MAC_ROLE_HOST_NIC;
    }
    return s_mac[role];
}

void MacUid_FormatString(uint8_t *desc, const uint8_t *mac)
{
    static const char hex[] = "0123456789ABCDEF";
    uint8_t i;

    if (desc == 0 || mac == 0) {
        return;
    }

    desc[0] = 26u;                  /* bLength: 2 + 12 * 2 */
    desc[1] = 0x03u;                /* bDescriptorType: STRING */
    for (i = 0; i < 12u; i++) {
        uint8_t value = mac[i >> 1];
        uint8_t nibble = (i & 1u) ? (value & 0x0Fu) : (uint8_t)(value >> 4);
        desc[2u + i * 2u] = (uint8_t)hex[nibble];
        desc[3u + i * 2u] = 0x00u;
    }
}
