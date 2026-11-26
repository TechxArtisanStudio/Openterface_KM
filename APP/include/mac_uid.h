#ifndef MAC_UID_H
#define MAC_UID_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Every Ethernet address the firmware puts on the wire.
 *
 * A unit needs four of them, and they must stay distinct from each other:
 * the address an OS adopts from the descriptor is also the address it filters
 * inbound frames on, so frames we inject with that same source address are
 * dropped as "self" (see the explanation above MyTargetEcmMacInfo in
 * usbd_desc.c).
 *
 *   HOST_NIC     seen by the Host OS as its own interface MAC
 *                (iMACAddress string + CDC NCM GET_NET_ADDRESS)
 *   HOST_PEER    source address on frames we inject toward the Host
 *   TARGET_NIC   seen by the Target OS as its own interface MAC
 *   TARGET_PEER  source address on frames we inject toward the Target
 */
typedef enum {
    MAC_ROLE_HOST_NIC = 0,
    MAC_ROLE_HOST_PEER,
    MAC_ROLE_TARGET_NIC,
    MAC_ROLE_TARGET_PEER,
    MAC_ROLE_COUNT
} MacRole;

#define MAC_ADDR_LEN 6

/* Reads the chip UID and derives all four addresses.  Idempotent; safe to
 * call from MacUid_Get()'s first use, so ordering mistakes cannot leak the
 * old fixed addresses. */
void MacUid_Init(void);

/* 6-byte address for a role.  Never NULL; the buffer lives in this module. */
const uint8_t *MacUid_Get(MacRole role);

/* Fills a 26-byte CDC iMACAddress string descriptor (bLength, 0x03, twelve
 * UTF-16LE hex digits, uppercase and without separators, as the ECM spec
 * requires). */
void MacUid_FormatString(uint8_t *desc, const uint8_t *mac);

#define MAC_HOST_NIC     (MacUid_Get(MAC_ROLE_HOST_NIC))
#define MAC_HOST_PEER    (MacUid_Get(MAC_ROLE_HOST_PEER))
#define MAC_TARGET_NIC   (MacUid_Get(MAC_ROLE_TARGET_NIC))
#define MAC_TARGET_PEER  (MacUid_Get(MAC_ROLE_TARGET_PEER))

#ifdef __cplusplus
}
#endif

#endif /* MAC_UID_H */
