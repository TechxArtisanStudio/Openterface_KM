/********************************** (C) COPYRIGHT *******************************
 * File Name          : usb_host_cdc.c
 * Author             : Openterface
 * Version            : V1.0.0
 * Date               : 2026/11/18
 * Description        : USB Host CDC-ACM class driver implementation.
 *                      Only available on keymod-plus (BOARD_VERSION=3).
 *********************************************************************************
 * Copyright (c) 2026 Openterface. All rights reserved.
 *******************************************************************************/

#ifdef ENABLE_USB_HOST

#include "include/usb_host_cdc.h"
#include "include/ch32v20x_usbfs_device.h"
#include "usbd_composite_km.h"

/*******************************************************************************/
/* Debug Output */
#define CDC_DEBUG  1

#if CDC_DEBUG
#define CDC_PRINTF(...)  printf(__VA_ARGS__)
#else
#define CDC_PRINTF(...)
#endif

/*******************************************************************************/
/* Global CDC Device Instance */
cdc_acm_device_t g_cdc_acm_device;

/*******************************************************************************/
/* Host transfer state for bulk operations */
static volatile uint8_t g_cdc_bulk_transfer_complete = 0;
static volatile uint8_t g_cdc_bulk_transfer_status = 0;
static volatile uint16_t g_cdc_bulk_rx_len = 0;

/*******************************************************************************/
/* CDC-ACM Initialization */

/*********************************************************************
 * @fn      USB_Host_CDC_Init
 *
 * @brief   Initialize CDC-ACM device state
 *
 * @return  none
 */
void USB_Host_CDC_Init(void)
{
    memset(&g_cdc_acm_device, 0, sizeof(g_cdc_acm_device));

    /* Set default line coding (115200 8N1) */
    g_cdc_acm_device.line_coding.dwDTERate = 115200;
    g_cdc_acm_device.line_coding.bCharFormat = 0;
    g_cdc_acm_device.line_coding.bParityType = 0;
    g_cdc_acm_device.line_coding.bDataBits = 8;

    g_cdc_acm_device.configured = 0;
    g_cdc_acm_device.data_if_active = 0;

    CDC_PRINTF("CDC-ACM initialized\r\n");
}

/*******************************************************************************/
/* Descriptor Parsing */

/*********************************************************************
 * @fn      USB_Host_CDC_ParseDescriptors
 *
 * @brief   Parse CDC descriptors from configuration descriptor
 *
 * @param   config_buf - Configuration descriptor buffer
 * @param   config_len - Length of configuration descriptor
 *
 * @return  STATUS_SUCCESS or error code
 */
uint8_t USB_Host_CDC_ParseDescriptors(const uint8_t *config_buf, uint16_t config_len)
{
    uint16_t offset = 0;
    uint8_t desc_len;
    uint8_t desc_type;
    uint8_t interface_class = 0;
    uint8_t interface_subclass = 0;
    uint8_t current_interface = 0;
    uint8_t found_comm_interface = 0;
    uint8_t found_data_interface = 0;

    CDC_PRINTF("Parsing CDC descriptors (len=%d)\r\n", config_len);

    while (offset < config_len) {
        desc_len = config_buf[offset];
        desc_type = config_buf[offset + 1];

        if (desc_len == 0) {
            CDC_PRINTF("Error: Invalid descriptor length 0\r\n");
            return STATUS_ERR_FRAME;
        }

        switch (desc_type) {
            case USB_DESCR_TYP_INTERF:
                if (desc_len >= 9) {
                    usb_interface_desc_t *if_desc = (usb_interface_desc_t *)&config_buf[offset];
                    current_interface = if_desc->bInterfaceNumber;
                    interface_class = if_desc->bInterfaceClass;
                    interface_subclass = if_desc->bInterfaceSubClass;

                    CDC_PRINTF("  Interface %d: Class=0x%02X SubClass=0x%02X\r\n",
                              current_interface, interface_class, interface_subclass);

                    /* CDC Communication Interface: Class 0x02, SubClass 0x02 (ACM) */
                    if (interface_class == 0x02 && interface_subclass == 0x02) {
                        g_cdc_acm_device.comm_interface = current_interface;
                        found_comm_interface = 1;
                        CDC_PRINTF("    -> CDC Communication Interface\r\n");
                    }
                    /* CDC Data Interface: Class 0x0A */
                    else if (interface_class == 0x0A) {
                        g_cdc_acm_device.data_interface = current_interface;
                        found_data_interface = 1;
                        CDC_PRINTF("    -> CDC Data Interface\r\n");
                    }
                }
                break;

            case USB_DESCR_TYP_ENDP:
                if (desc_len >= 7) {
                    usb_endpoint_desc_t *ep_desc = (usb_endpoint_desc_t *)&config_buf[offset];
                    uint8_t ep_addr = ep_desc->bEndpointAddress;
                    uint8_t ep_attr = ep_desc->bmAttributes & 0x03;  /* Transfer type */

                    CDC_PRINTF("  Endpoint 0x%02X: Attr=0x%02X MaxPkt=%d\r\n",
                              ep_addr, ep_attr, ep_desc->wMaxPacketSize);

                    if (interface_class == 0x02) {
                        /* Notification endpoint (Interrupt IN) on Comm Interface */
                        if ((ep_addr & 0x80) && (ep_attr == 0x03)) {
                            g_cdc_acm_device.notification_ep.ep_addr = ep_addr;
                            g_cdc_acm_device.notification_ep.ep_type = ep_attr;
                            g_cdc_acm_device.notification_ep.max_packet_size = ep_desc->wMaxPacketSize;
                            g_cdc_acm_device.notification_ep.interval = ep_desc->bInterval;
                        }
                    }
                    else if (interface_class == 0x0A) {
                        /* Data endpoints (Bulk) on Data Interface */
                        if (ep_attr == 0x02) {  /* Bulk */
                            if (ep_addr & 0x80) {
                                /* Bulk IN */
                                g_cdc_acm_device.data_in_ep.ep_addr = ep_addr;
                                g_cdc_acm_device.data_in_ep.ep_type = ep_attr;
                                g_cdc_acm_device.data_in_ep.max_packet_size = ep_desc->wMaxPacketSize;
                            } else {
                                /* Bulk OUT */
                                g_cdc_acm_device.data_out_ep.ep_addr = ep_addr;
                                g_cdc_acm_device.data_out_ep.ep_type = ep_attr;
                                g_cdc_acm_device.data_out_ep.max_packet_size = ep_desc->wMaxPacketSize;
                            }
                        }
                    }
                }
                break;

            case CDC_DT_CS_INTERFACE:
                if (desc_len >= 3) {
                    uint8_t subtype = config_buf[offset + 2];
                    CDC_PRINTF("  CDC Functional Descriptor: SubType=0x%02X\r\n", subtype);

                    switch (subtype) {
                        case CDC_SUBTYPE_HEADER:
                            if (desc_len >= 5) {
                                cdc_header_func_desc_t *hdr = (cdc_header_func_desc_t *)&config_buf[offset];
                                CDC_PRINTF("    bcdCDC=0x%04X\r\n", hdr->bcdCDC);
                            }
                            break;

                        case CDC_SUBTYPE_CALL_MGMT:
                            if (desc_len >= 5) {
                                cdc_call_mgmt_func_desc_t *call = (cdc_call_mgmt_func_desc_t *)&config_buf[offset];
                                CDC_PRINTF("    bmCapabilities=0x%02X bDataInterface=%d\r\n",
                                          call->bmCapabilities, call->bDataInterface);
                            }
                            break;

                        case CDC_SUBTYPE_ACM:
                            if (desc_len >= 4) {
                                cdc_acm_func_desc_t *acm = (cdc_acm_func_desc_t *)&config_buf[offset];
                                CDC_PRINTF("    bmCapabilities=0x%02X\r\n", acm->bmCapabilities);
                            }
                            break;

                        case CDC_SUBTYPE_UNION:
                            if (desc_len >= 4) {
                                cdc_union_func_desc_t *union_desc = (cdc_union_func_desc_t *)&config_buf[offset];
                                CDC_PRINTF("    bControlInterface=%d bSubordinate=%d\r\n",
                                          union_desc->bControlInterface, union_desc->bSubordinateInterface0);
                            }
                            break;
                    }
                }
                break;

            default:
                break;
        }

        offset += desc_len;
    }

    /* Verify we found required interfaces */
    if (!found_comm_interface) {
        CDC_PRINTF("Error: CDC Communication Interface not found\r\n");
        return STATUS_ERR_FRAME;
    }
    if (!found_data_interface) {
        CDC_PRINTF("Error: CDC Data Interface not found\r\n");
        return STATUS_ERR_FRAME;
    }

    /* Verify we found required endpoints */
    if (g_cdc_acm_device.data_in_ep.ep_addr == 0) {
        CDC_PRINTF("Error: CDC Data IN endpoint not found\r\n");
        return STATUS_ERR_FRAME;
    }
    if (g_cdc_acm_device.data_out_ep.ep_addr == 0) {
        CDC_PRINTF("Error: CDC Data OUT endpoint not found\r\n");
        return STATUS_ERR_FRAME;
    }

    CDC_PRINTF("CDC descriptor parsing complete\r\n");
    return STATUS_SUCCESS;
}

/*******************************************************************************/
/* CDC Class Requests */

/*********************************************************************
 * @fn      USB_Host_CDC_SetLineCoding
 *
 * @brief   Set line coding (baud rate, data bits, stop bits, parity)
 *
 * @param   line_coding - Pointer to line coding structure
 *
 * @return  STATUS_SUCCESS or error code
 */
uint8_t USB_Host_CDC_SetLineCoding(const cdc_line_coding_t *line_coding)
{
    CDC_PRINTF("CDC: SET_LINE_CODING baud=%d data=%d parity=%d stop=%d\r\n",
              line_coding->dwDTERate, line_coding->bDataBits,
              line_coding->bParityType, line_coding->bCharFormat);

    return USB_Host_ControlTransfer(
        USB_REQ_TYP_OUT | USB_REQ_TYP_CLASS | USB_REQ_RECIP_INTERF,
        CDC_REQ_SET_LINE_CODING,
        0,
        g_cdc_acm_device.comm_interface,
        sizeof(cdc_line_coding_t),
        (uint8_t *)line_coding
    );
}

/*********************************************************************
 * @fn      USB_Host_CDC_GetLineCoding
 *
 * @brief   Get current line coding
 *
 * @param   line_coding - Pointer to store line coding
 *
 * @return  STATUS_SUCCESS or error code
 */
uint8_t USB_Host_CDC_GetLineCoding(cdc_line_coding_t *line_coding)
{
    uint8_t ret;

    ret = USB_Host_ControlTransfer(
        USB_REQ_TYP_IN | USB_REQ_TYP_CLASS | USB_REQ_RECIP_INTERF,
        CDC_REQ_GET_LINE_CODING,
        0,
        g_cdc_acm_device.comm_interface,
        sizeof(cdc_line_coding_t),
        USB_Host_EP0_Buf
    );

    if (ret == STATUS_SUCCESS) {
        memcpy(line_coding, USB_Host_EP0_Buf, sizeof(cdc_line_coding_t));
    }

    return ret;
}

/*********************************************************************
 * @fn      USB_Host_CDC_SetControlLineState
 *
 * @brief   Set control line state (DTR/RTS)
 *
 * @param   state - Control line state (bit 0=DTR, bit 1=RTS)
 *
 * @return  STATUS_SUCCESS or error code
 */
uint8_t USB_Host_CDC_SetControlLineState(uint16_t state)
{
    CDC_PRINTF("CDC: SET_CONTROL_LINE state=0x%04X\r\n", state);

    g_cdc_acm_device.control_line_state = state;

    return USB_Host_ControlTransfer(
        USB_REQ_TYP_OUT | USB_REQ_TYP_CLASS | USB_REQ_RECIP_INTERF,
        CDC_REQ_SET_CONTROL_LINE,
        state,
        g_cdc_acm_device.comm_interface,
        0,
        NULL
    );
}

/*********************************************************************
 * @fn      USB_Host_CDC_SendBreak
 *
 * @brief   Send break signal
 *
 * @param   duration - Break duration in milliseconds (0=release)
 *
 * @return  STATUS_SUCCESS or error code
 */
uint8_t USB_Host_CDC_SendBreak(uint16_t duration)
{
    CDC_PRINTF("CDC: SEND_BREAK duration=%d\r\n", duration);

    return USB_Host_ControlTransfer(
        USB_REQ_TYP_OUT | USB_REQ_TYP_CLASS | USB_REQ_RECIP_INTERF,
        CDC_REQ_SEND_BREAK,
        duration,
        g_cdc_acm_device.comm_interface,
        0,
        NULL
    );
}

/*********************************************************************
 * @fn      USB_Host_CDC_ActivateDataInterface
 *
 * @brief   Activate data interface (SET_INTERFACE)
 *
 * @param   alt_setting - Alternate setting (usually 0 or 1)
 *
 * @return  STATUS_SUCCESS or error code
 */
uint8_t USB_Host_CDC_ActivateDataInterface(uint8_t alt_setting)
{
    uint8_t ret;

    CDC_PRINTF("CDC: SET_INTERFACE data_if=%d alt=%d\r\n",
              g_cdc_acm_device.data_interface, alt_setting);

    ret = USB_Host_ControlTransfer(
        USB_REQ_TYP_OUT | USB_REQ_TYP_STANDARD | USB_REQ_RECIP_INTERF,
        USB_SET_INTERFACE,
        alt_setting,
        g_cdc_acm_device.data_interface,
        0,
        NULL
    );

    if (ret == STATUS_SUCCESS) {
        g_cdc_acm_device.alt_setting = alt_setting;
        g_cdc_acm_device.data_if_active = 1;
    }

    return ret;
}

/*******************************************************************************/
/* Bulk Data Transfer */

/*********************************************************************
 * @fn      USB_Host_CDC_ConfigureEndpoints
 *
 * @brief   Configure host endpoints for CDC data transfer
 *
 * @return  none
 */
void USB_Host_CDC_ConfigureEndpoints(void)
{
    CDC_PRINTF("CDC: Configuring endpoints\r\n");
    CDC_PRINTF("  Data IN: 0x%02X MaxPkt=%d\r\n",
              g_cdc_acm_device.data_in_ep.ep_addr,
              g_cdc_acm_device.data_in_ep.max_packet_size);
    CDC_PRINTF("  Data OUT: 0x%02X MaxPkt=%d\r\n",
              g_cdc_acm_device.data_out_ep.ep_addr,
              g_cdc_acm_device.data_out_ep.max_packet_size);

    /* Endpoints are configured via HOST_EP_MOD which was already set */
    /* Just need to ensure DMA addresses are correct */
}

/*********************************************************************
 * @fn      USB_Host_CDC_SendData
 *
 * @brief   Send data to CDC device (Bulk OUT)
 *
 * @param   data - Data buffer
 * @param   len - Data length
 *
 * @return  STATUS_SUCCESS or error code
 */
uint8_t USB_Host_CDC_SendData(const uint8_t *data, uint16_t len)
{
    uint16_t timeout;
    uint8_t ret;

    if (!g_cdc_acm_device.data_if_active) {
        CDC_PRINTF("Error: Data interface not active\r\n");
        return STATUS_ERR_FRAME;
    }

    if (len > 64) {
        CDC_PRINTF("Error: Data length %d exceeds buffer size\r\n", len);
        return STATUS_ERR_PARAM;
    }

    CDC_PRINTF("CDC: Sending %d bytes to 0x%02X\r\n",
              len, g_cdc_acm_device.data_out_ep.ep_addr);

    /* Copy data to TX buffer */
    memcpy(USB_Host_Tx_Buf, data, len);

    /* Set TX length */
    USBFSH->HOST_TX_LEN = len;

    /* Set endpoint PID */
    uint8_t ep_num = g_cdc_acm_device.data_out_ep.ep_addr & 0x0F;
    USBFSH->HOST_EP_PID = USB_PID_OUT | ep_num;

    /* Reset transfer state */
    g_cdc_bulk_transfer_complete = 0;
    g_cdc_bulk_transfer_status = 0;

    /* Wait for transfer completion */
    timeout = 1000;  /* 100ms timeout */
    while (!g_cdc_bulk_transfer_complete && timeout--) {
        Delay_Us(100);
    }

    if (!g_cdc_bulk_transfer_complete) {
        CDC_PRINTF("CDC: Bulk OUT timeout\r\n");
        return STATUS_ERR_TIMEOUT;
    }

    /* Check response */
    ret = g_cdc_bulk_transfer_status & USBFS_UIS_H_RES_MASK;
    if (ret != USB_PID_ACK) {
        CDC_PRINTF("CDC: Bulk OUT failed, status=0x%02X\r\n", ret);
        return STATUS_ERR_FRAME;
    }

    CDC_PRINTF("CDC: Sent %d bytes\r\n", len);
    return STATUS_SUCCESS;
}

/*********************************************************************
 * @fn      USB_Host_CDC_ReceiveData
 *
 * @brief   Receive data from CDC device (Bulk IN)
 *
 * @param   data - Buffer to receive data
 * @param   max_len - Maximum length to receive
 * @param   actual_len - Pointer to store actual length received
 *
 * @return  STATUS_SUCCESS or error code
 */
uint8_t USB_Host_CDC_ReceiveData(uint8_t *data, uint16_t max_len, uint16_t *actual_len)
{
    uint16_t timeout;
    uint8_t ret;

    if (!g_cdc_acm_device.data_if_active) {
        CDC_PRINTF("Error: Data interface not active\r\n");
        return STATUS_ERR_FRAME;
    }

    CDC_PRINTF("CDC: Receiving up to %d bytes from 0x%02X\r\n",
              max_len, g_cdc_acm_device.data_in_ep.ep_addr);

    /* Set RX length (max to receive) */
    /* Note: For IN transfers, the device sends up to max packet size */

    /* Set endpoint PID */
    uint8_t ep_num = g_cdc_acm_device.data_in_ep.ep_addr & 0x80 ?
                     (g_cdc_acm_device.data_in_ep.ep_addr & 0x0F) | 0x80 :
                     g_cdc_acm_device.data_in_ep.ep_addr & 0x0F;
    USBFSH->HOST_EP_PID = USB_PID_IN | (ep_num & 0x0F);

    /* Reset transfer state */
    g_cdc_bulk_transfer_complete = 0;
    g_cdc_bulk_transfer_status = 0;
    g_cdc_bulk_rx_len = 0;

    /* Wait for transfer completion */
    timeout = 1000;  /* 100ms timeout */
    while (!g_cdc_bulk_transfer_complete && timeout--) {
        Delay_Us(100);
    }

    if (!g_cdc_bulk_transfer_complete) {
        CDC_PRINTF("CDC: Bulk IN timeout\r\n");
        return STATUS_ERR_TIMEOUT;
    }

    /* Check response */
    ret = g_cdc_bulk_transfer_status & USBFS_UIS_H_RES_MASK;
    if (ret != USB_PID_DATA0 && ret != USB_PID_DATA1) {
        /* For DATA0/DATA1, we accept any data toggle */
        if (ret != USB_PID_ACK && ret != 0) {
            CDC_PRINTF("CDC: Bulk IN failed, status=0x%02X\r\n", ret);
            return STATUS_ERR_FRAME;
        }
    }

    /* Get received length */
    *actual_len = g_cdc_bulk_rx_len;

    /* Copy received data */
    if (*actual_len > 0 && data) {
        memcpy(data, USB_Host_Rx_Buf, *actual_len);
    }

    CDC_PRINTF("CDC: Received %d bytes\r\n", *actual_len);
    return STATUS_SUCCESS;
}

/*********************************************************************
 * @fn      USB_Host_CDC_SetRxCallback
 *
 * @brief   Register RX callback for received data
 *
 * @param   callback - Function to call when data is received
 *
 * @return  none
 */
void USB_Host_CDC_SetRxCallback(void (*callback)(const uint8_t *data, uint16_t len))
{
    g_cdc_acm_device.rx_callback = callback;
}

/*******************************************************************************/
/* Bulk Transfer Complete Handler (called from USB_Host_TransferComplete) */

void USB_Host_CDC_BulkTransferComplete(uint8_t status, uint16_t rx_len)
{
    g_cdc_bulk_transfer_status = status;
    g_cdc_bulk_rx_len = rx_len;
    g_cdc_bulk_transfer_complete = 1;

    /* If we received data and have a callback, invoke it */
    if (rx_len > 0 && g_cdc_acm_device.rx_callback) {
        g_cdc_acm_device.rx_callback(USB_Host_Rx_Buf, rx_len);
    }
}

#endif /* ENABLE_USB_HOST */
