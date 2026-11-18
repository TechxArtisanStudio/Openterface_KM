/********************************** (C) COPYRIGHT *******************************
 * File Name          : usb_otg_mode.c
 * Author             : Openterface
 * Version            : V1.0.0
 * Date               : 2026/11/18
 * Description        : USB OTG mode switching implementation.
 *                      Enables runtime switching between Device and Host modes.
 *                      Only available on keymod-plus (BOARD_VERSION=3).
 *********************************************************************************
 * Copyright (c) 2026 Openterface. All rights reserved.
 *******************************************************************************/

#ifdef ENABLE_USB_HOST

#include "include/usb_otg_mode.h"
#include "include/usb_host_cdc.h"
#include "include/ch32v20x_usbfs_device.h"
#include "usbd_composite_km.h"

/*******************************************************************************/
/* Debug Output */
#define USB_HOST_DEBUG  1

#if USB_HOST_DEBUG
#define HOST_PRINTF(...)  printf(__VA_ARGS__)
#else
#define HOST_PRINTF(...)
#endif

/*******************************************************************************/
/* Global Variables */
volatile usb_otg_mode_t g_usb_otg_mode = USB_OTG_MODE_DEVICE;
usb_host_device_t g_usb_host_device;

/* Host endpoint buffers */
__attribute__((aligned(4))) uint8_t USB_Host_Tx_Buf[64];
__attribute__((aligned(4))) uint8_t USB_Host_Rx_Buf[64];
__attribute__((aligned(4))) uint8_t USB_Host_EP0_Buf[64];

/* Host transfer state */
static volatile uint8_t g_host_transfer_complete = 0;
static volatile uint8_t g_host_transfer_status = 0;
static volatile uint16_t g_host_rx_len = 0;

/* Device attach state */
static volatile uint8_t g_host_device_attached = 0;

/*******************************************************************************/
/* USB Host Mode Initialization */

/*********************************************************************
 * @fn      USB_OTG_ModeInit
 *
 * @brief   Initialize USB OTG mode (starts in Device mode)
 *
 * @return  none
 */
void USB_OTG_ModeInit(void)
{
    g_usb_otg_mode = USB_OTG_MODE_DEVICE;
    memset(&g_usb_host_device, 0, sizeof(g_usb_host_device));
    HOST_PRINTF("USB OTG Mode initialized (Device mode)\r\n");
}

/*********************************************************************
 * @fn      USB_OTG_GetMode
 *
 * @brief   Get current USB OTG mode
 *
 * @return  Current mode
 */
usb_otg_mode_t USB_OTG_GetMode(void)
{
    return g_usb_otg_mode;
}

/*********************************************************************
 * @fn      USB_Host_Init
 *
 * @brief   Initialize USB Host mode registers
 *
 * @return  none
 */
void USB_Host_Init(void)
{
    /* Reset USB SIE and enable Host mode */
    USBFSH->BASE_CTRL = USBFS_UC_RESET_SIE | USBFS_UC_CLR_ALL | USBFS_UC_HOST_MODE;
    Delay_Us(10);

    /* Clear reset bit, keep Host mode */
    USBFSH->BASE_CTRL = USBFS_UC_HOST_MODE | USBFS_UC_INT_BUSY | USBFS_UC_DMA_EN;

    /* Configure Host control: enable port, disable pulldown */
    USBFSH->HOST_CTRL = USBFS_UH_PORT_EN | USBFS_UH_PD_DIS;

    /* Enable TX and RX endpoints */
    USBFSH->HOST_EP_MOD = USBFS_UH_EP_TX_EN | USBFS_UH_EP_RX_EN;

    /* Set DMA addresses for host buffers */
    USBFSH->HOST_TX_DMA = (uint32_t)USB_Host_Tx_Buf;
    USBFSH->HOST_RX_DMA = (uint32_t)USB_Host_Rx_Buf;

    /* Clear device address */
    USBFSH->DEV_ADDR = 0;

    /* Enable Host interrupts: transfer, detect, SOF */
    USBFSH->INT_EN = USBFS_UIE_TRANSFER | USBFS_UIE_DETECT | USBFS_UIE_HST_SOF;

    /* Enable automatic SOF generation */
    USBFSH->HOST_SETUP = USBFS_UH_SOF_EN;

    /* Initialize transfer state */
    g_host_transfer_complete = 0;
    g_host_transfer_status = 0;
    g_host_device_attached = 0;

    /* Initialize CDC-ACM driver */
    USB_Host_CDC_Init();

    HOST_PRINTF("USB Host mode initialized\r\n");
}

/*********************************************************************
 * @fn      USB_OTG_SwitchToHost
 *
 * @brief   Switch from Device mode to Host mode
 *
 * @return  STATUS_SUCCESS or error code
 */
uint8_t USB_OTG_SwitchToHost(void)
{
    if (g_usb_otg_mode == USB_OTG_MODE_HOST) {
        HOST_PRINTF("Already in Host mode\r\n");
        return STATUS_SUCCESS;
    }

    HOST_PRINTF("Switching to Host mode...\r\n");

    /* Enter switching state */
    g_usb_otg_mode = USB_OTG_MODE_SWITCHING;

    /* Disable Device mode */
    USBFSD->INT_EN = 0;
    USBFSD->BASE_CTRL = 0;
    USBFSD->UDEV_CTRL = 0;
    NVIC_DisableIRQ(USBFS_IRQn);

    Delay_Ms(10);

    /* Initialize Host mode */
    USB_Host_Init();

    /* Re-enable USB interrupt */
    NVIC_EnableIRQ(USBFS_IRQn);

    /* Switch to Host mode */
    g_usb_otg_mode = USB_OTG_MODE_HOST;

    HOST_PRINTF("Switched to Host mode\r\n");
    return STATUS_SUCCESS;
}

/*********************************************************************
 * @fn      USB_OTG_SwitchToDevice
 *
 * @brief   Switch from Host mode back to Device mode
 *
 * @return  STATUS_SUCCESS or error code
 */
uint8_t USB_OTG_SwitchToDevice(void)
{
    if (g_usb_otg_mode == USB_OTG_MODE_DEVICE) {
        HOST_PRINTF("Already in Device mode\r\n");
        return STATUS_SUCCESS;
    }

    HOST_PRINTF("Switching to Device mode...\r\n");

    /* Enter switching state */
    g_usb_otg_mode = USB_OTG_MODE_SWITCHING;

    /* Disable Host mode */
    USBFSH->INT_EN = 0;
    USBFSH->BASE_CTRL = 0;
    USBFSH->HOST_CTRL = 0;
    NVIC_DisableIRQ(USBFS_IRQn);

    Delay_Ms(10);

    /* Re-initialize Device mode */
    USBFS_Device_Init(ENABLE);

    /* Switch to Device mode */
    g_usb_otg_mode = USB_OTG_MODE_DEVICE;

    /* Clear host device info */
    memset(&g_usb_host_device, 0, sizeof(g_usb_host_device));

    HOST_PRINTF("Switched to Device mode\r\n");
    return STATUS_SUCCESS;
}

/*******************************************************************************/
/* USB Host Transfer Functions */

/*********************************************************************
 * @fn      USB_Host_BusReset
 *
 * @brief   Perform USB bus reset
 *
 * @return  none
 */
void USB_Host_BusReset(void)
{
    HOST_PRINTF("USB Host: Bus reset...\r\n");

    /* Assert bus reset */
    USBFSH->HOST_CTRL |= USBFS_UH_BUS_RESET;
    Delay_Ms(100);

    /* Release bus reset */
    USBFSH->HOST_CTRL &= ~USBFS_UH_BUS_RESET;
    Delay_Ms(100);

    HOST_PRINTF("USB Host: Bus reset complete\r\n");
}

/*********************************************************************
 * @fn      USB_Host_ControlTransfer
 *
 * @brief   Perform control transfer on EP0
 *
 * @param   bmRequestType - Request type
 * @param   bRequest - Request code
 * @param   wValue - Value field
 * @param   wIndex - Index field
 * @param   wLength - Length field
 * @param   data - Data buffer (for IN/OUT)
 *
 * @return  STATUS_SUCCESS or error code
 */
uint8_t USB_Host_ControlTransfer(uint8_t bmRequestType, uint8_t bRequest,
                                  uint16_t wValue, uint16_t wIndex,
                                  uint16_t wLength, uint8_t *data)
{
    uint8_t ret;
    uint8_t token;
    uint16_t timeout;

    /* Prepare SETUP packet in EP0 buffer */
    USB_SETUP_REQ *setup = (USB_SETUP_REQ *)USB_Host_EP0_Buf;
    setup->bRequestType = bmRequestType;
    setup->bRequest = bRequest;
    setup->wValue = wValue;
    setup->wIndex = wIndex;
    setup->wLength = wLength;

    /* Copy OUT data if needed */
    if ((wLength > 0) && (bmRequestType & USB_REQ_TYP_OUT) && data) {
        memcpy(&USB_Host_EP0_Buf[8], data, wLength);
    }

    /* Reset toggle */
    USBFSH->HOST_TX_CTRL = USBFS_UH_T_TOG;  /* DATA0 for SETUP */
    USBFSH->HOST_RX_CTRL = USBFS_UH_R_TOG;

    /* Send SETUP token */
    USBFSH->HOST_TX_LEN = 8;  /* SETUP packet is always 8 bytes */
    USBFSH->HOST_EP_PID = USB_PID_SETUP | 0;  /* SETUP to EP0 */

    g_host_transfer_complete = 0;
    timeout = 1000;  /* 100ms timeout */

    while (!g_host_transfer_complete && timeout--) {
        Delay_Us(100);
    }

    if (!g_host_transfer_complete) {
        HOST_PRINTF("USB Host: SETUP timeout\r\n");
        return STATUS_ERR_TIMEOUT;
    }

    /* Check response */
    ret = g_host_transfer_status & USBFS_UIS_H_RES_MASK;
    if (ret != USB_PID_ACK) {
        HOST_PRINTF("USB Host: SETUP failed, status=0x%02X\r\n", ret);
        return STATUS_ERR_FRAME;
    }

    /* DATA stage */
    if (wLength > 0) {
        if (bmRequestType & USB_REQ_TYP_IN) {
            /* IN data stage */
            USBFSH->HOST_TX_CTRL = USBFS_UH_T_TOG;  /* DATA1 */
            USBFSH->HOST_RX_CTRL = USBFS_UH_R_TOG;  /* DATA1 */
            USBFSH->HOST_TX_LEN = 0;
            USBFSH->HOST_EP_PID = USB_PID_IN | 0;  /* IN to EP0 */

            g_host_transfer_complete = 0;
            timeout = 1000;

            while (!g_host_transfer_complete && timeout--) {
                Delay_Us(100);
            }

            if (!g_host_transfer_complete) {
                return STATUS_ERR_TIMEOUT;
            }

            /* Copy received data */
            if (data && g_host_rx_len > 0) {
                memcpy(data, USB_Host_Rx_Buf, g_host_rx_len);
            }

            /* Send STATUS (zero-length OUT) */
            USBFSH->HOST_TX_CTRL = 0;  /* DATA0 */
            USBFSH->HOST_TX_LEN = 0;
            USBFSH->HOST_EP_PID = USB_PID_OUT | 0;

            g_host_transfer_complete = 0;
            timeout = 500;

            while (!g_host_transfer_complete && timeout--) {
                Delay_Us(100);
            }
        } else {
            /* OUT data stage */
            USBFSH->HOST_TX_CTRL = USBFS_UH_T_TOG;  /* DATA1 */
            USBFSH->HOST_TX_LEN = wLength;
            USBFSH->HOST_EP_PID = USB_PID_OUT | 0;  /* OUT to EP0 */

            g_host_transfer_complete = 0;
            timeout = 1000;

            while (!g_host_transfer_complete && timeout--) {
                Delay_Us(100);
            }

            if (!g_host_transfer_complete) {
                return STATUS_ERR_TIMEOUT;
            }

            /* Get STATUS (zero-length IN) */
            USBFSH->HOST_RX_CTRL = USBFS_UH_R_TOG;  /* DATA1 */
            USBFSH->HOST_TX_LEN = 0;
            USBFSH->HOST_EP_PID = USB_PID_IN | 0;

            g_host_transfer_complete = 0;
            timeout = 500;

            while (!g_host_transfer_complete && timeout--) {
                Delay_Us(100);
            }
        }
    }

    return STATUS_SUCCESS;
}

/*********************************************************************
 * @fn      USB_Host_GetDescriptor
 *
 * @brief   Send GET_DESCRIPTOR request
 *
 * @param   type - Descriptor type
 * @param   index - Descriptor index
 * @param   len - Requested length
 *
 * @return  STATUS_SUCCESS or error code
 */
uint8_t USB_Host_GetDescriptor(uint8_t type, uint8_t index, uint16_t len)
{
    uint16_t wValue = (type << 8) | index;

    HOST_PRINTF("USB Host: GET_DESCRIPTOR type=0x%02X index=%d len=%d\r\n",
                type, index, len);

    return USB_Host_ControlTransfer(
        USB_REQ_TYP_IN | USB_REQ_TYP_STANDARD | USB_REQ_RECIP_DEVICE,
        USB_GET_DESCRIPTOR,
        wValue,
        0,
        len,
        USB_Host_EP0_Buf
    );
}

/*********************************************************************
 * @fn      USB_Host_SetAddress
 *
 * @brief   Send SET_ADDRESS request
 *
 * @param   addr - Device address (1-127)
 *
 * @return  STATUS_SUCCESS or error code
 */
uint8_t USB_Host_SetAddress(uint8_t addr)
{
    uint8_t ret;

    HOST_PRINTF("USB Host: SET_ADDRESS addr=%d\r\n", addr);

    ret = USB_Host_ControlTransfer(
        USB_REQ_TYP_OUT | USB_REQ_TYP_STANDARD | USB_REQ_RECIP_DEVICE,
        USB_SET_ADDRESS,
        addr,
        0,
        0,
        NULL
    );

    if (ret == STATUS_SUCCESS) {
        /* Set device address in hardware */
        USBFSH->DEV_ADDR = addr;
        g_usb_host_device.address = addr;
        Delay_Ms(10);  /* Allow device to process address */
    }

    return ret;
}

/*********************************************************************
 * @fn      USB_Host_SetConfiguration
 *
 * @brief   Send SET_CONFIGURATION request
 *
 * @param   config - Configuration value
 *
 * @return  STATUS_SUCCESS or error code
 */
uint8_t USB_Host_SetConfiguration(uint8_t config)
{
    HOST_PRINTF("USB Host: SET_CONFIGURATION config=%d\r\n", config);

    return USB_Host_ControlTransfer(
        USB_REQ_TYP_OUT | USB_REQ_TYP_STANDARD | USB_REQ_RECIP_DEVICE,
        USB_SET_CONFIGURATION,
        config,
        0,
        0,
        NULL
    );
}

/*********************************************************************
 * @fn      USB_Host_EnumerateDevice
 *
 * @brief   Enumerate connected USB device
 *
 * @return  STATUS_SUCCESS or error code
 */
uint8_t USB_Host_EnumerateDevice(void)
{
    uint8_t ret;
    uint16_t total_len;

    HOST_PRINTF("USB Host: Starting enumeration...\r\n");

    /* Step 1: Bus reset */
    USB_Host_BusReset();

    /* Step 2: Get device descriptor (8 bytes to get max packet size) */
    ret = USB_Host_GetDescriptor(USB_DESCR_TYP_DEVICE, 0, 8);
    if (ret != STATUS_SUCCESS) {
        HOST_PRINTF("USB Host: Failed to get device descriptor\r\n");
        return ret;
    }

    /* Copy partial device descriptor */
    memcpy(&g_usb_host_device.device_desc, USB_Host_EP0_Buf, 8);
    HOST_PRINTF("USB Host: VID=0x%04X PID=0x%04X\r\n",
                g_usb_host_device.device_desc.idVendor,
                g_usb_host_device.device_desc.idProduct);

    /* Step 3: Set address */
    ret = USB_Host_SetAddress(1);
    if (ret != STATUS_SUCCESS) {
        HOST_PRINTF("USB Host: Failed to set address\r\n");
        return ret;
    }

    /* Step 4: Get full device descriptor */
    ret = USB_Host_GetDescriptor(USB_DESCR_TYP_DEVICE, 0, 18);
    if (ret != STATUS_SUCCESS) {
        return ret;
    }
    memcpy(&g_usb_host_device.device_desc, USB_Host_EP0_Buf, 18);

    /* Step 5: Get configuration descriptor (header first) */
    ret = USB_Host_GetDescriptor(USB_DESCR_TYP_CONFIG, 0, 9);
    if (ret != STATUS_SUCCESS) {
        return ret;
    }
    memcpy(&g_usb_host_device.config_desc, USB_Host_EP0_Buf, 9);
    total_len = g_usb_host_device.config_desc.wTotalLength;

    HOST_PRINTF("USB Host: Config total length=%d\r\n", total_len);

    /* Get full configuration descriptor */
    if (total_len > 64) total_len = 64;  /* Limit to buffer size */
    ret = USB_Host_GetDescriptor(USB_DESCR_TYP_CONFIG, 0, total_len);
    if (ret != STATUS_SUCCESS) {
        return ret;
    }

    /* Parse CDC descriptors if this is a CDC device */
    /* Check if VID:PID matches ESP32 (303A:xxxx) or if we have CDC class */
    ret = USB_Host_CDC_ParseDescriptors(USB_Host_EP0_Buf, total_len);
    if (ret == STATUS_SUCCESS) {
        g_usb_host_device.cdc_comm_interface = g_cdc_acm_device.comm_interface;
        g_usb_host_device.cdc_data_interface = g_cdc_acm_device.data_interface;
        g_usb_host_device.cdc_notification_ep = g_cdc_acm_device.notification_ep.ep_addr;
        g_usb_host_device.cdc_data_in_ep = g_cdc_acm_device.data_in_ep.ep_addr;
        g_usb_host_device.cdc_data_out_ep = g_cdc_acm_device.data_out_ep.ep_addr;
        g_usb_host_device.cdc_data_in_maxpkt = g_cdc_acm_device.data_in_ep.max_packet_size;
        g_usb_host_device.cdc_data_out_maxpkt = g_cdc_acm_device.data_out_ep.max_packet_size;
        HOST_PRINTF("USB Host: CDC device detected\r\n");
    }

    /* Step 6: Set configuration */
    ret = USB_Host_SetConfiguration(g_usb_host_device.config_desc.bConfigurationValue);
    if (ret != STATUS_SUCCESS) {
        return ret;
    }

    /* Step 7: Initialize CDC if detected */
    if (g_cdc_acm_device.comm_interface != 0 || g_cdc_acm_device.data_interface != 0) {
        USB_Host_CDC_ConfigureEndpoints();
        USB_Host_CDC_SetControlLineState(CDC_CTRL_DTE_PRESENT | CDC_CTRL_CARRIER_ON);
        USB_Host_CDC_SetLineCoding(&g_cdc_acm_device.line_coding);
        USB_Host_CDC_ActivateDataInterface(0);
        g_cdc_acm_device.configured = 1;
        HOST_PRINTF("USB Host: CDC configured\r\n");
    }

    g_usb_host_device.enumerated = 1;
    HOST_PRINTF("USB Host: Enumeration complete!\r\n");

    return STATUS_SUCCESS;
}

/*******************************************************************************/
/* USB Host Interrupt Handlers */

/*********************************************************************
 * @fn      USB_Host_TransferComplete
 *
 * @brief   Handle transfer complete interrupt
 *
 * @return  none
 */
void USB_Host_TransferComplete(void)
{
    uint8_t status;
    uint16_t rx_len = 0;
    uint8_t token;

    /* Get handshake response */
    status = USBFSH->INT_ST & USBFS_UIS_H_RES_MASK;

    /* Get current token */
    token = USBFSH->HOST_EP_PID & USBFS_UH_TOKEN_MASK;

    /* Get RX length for IN transfers */
    if (token == (USB_PID_IN << 4)) {
        rx_len = USBFSH->RX_LEN;
        g_host_rx_len = rx_len;
    }

    /* Store status and signal completion */
    g_host_transfer_status = status;
    g_host_transfer_complete = 1;

    /* Toggle for next transfer */
    USBFSH->HOST_TX_CTRL ^= USBFS_UH_T_TOG;
    USBFSH->HOST_RX_CTRL ^= USBFS_UH_R_TOG;

    /* Notify CDC driver if this is a bulk transfer (not EP0) */
    /* Check if the endpoint is not 0 (EP0 is for control transfers) */
    uint8_t ep_num = USBFSH->HOST_EP_PID & USBFS_UH_ENDP_MASK;
    if (ep_num != 0 && g_usb_host_device.enumerated && g_cdc_acm_device.configured) {
        /* This is a bulk transfer on CDC data endpoints */
        USB_Host_CDC_BulkTransferComplete(status, rx_len);
    }
}

/*********************************************************************
 * @fn      USB_Host_DetectDevice
 *
 * @brief   Handle device detect interrupt (attach/detach)
 *
 * @return  none
 */
void USB_Host_DetectDevice(void)
{
    uint8_t mis_st;

    mis_st = USBFSH->MIS_ST;

    if (mis_st & USBFS_UMS_DEV_ATTACH) {
        /* Device attached */
        if (!g_host_device_attached) {
            g_host_device_attached = 1;
            HOST_PRINTF("USB Host: Device attached\r\n");

            /* Start enumeration */
            USB_Host_EnumerateDevice();
        }
    } else {
        /* Device detached */
        if (g_host_device_attached) {
            g_host_device_attached = 0;
            g_usb_host_device.enumerated = 0;
            HOST_PRINTF("USB Host: Device detached\r\n");
        }
    }
}

/*********************************************************************
 * @fn      USB_Host_SOFHandler
 *
 * @brief   Handle SOF interrupt
 *
 * @return  none
 */
void USB_Host_SOFHandler(void)
{
    /* SOF interrupt occurs every 1ms in host mode */
    /* Currently no action needed */
}

#endif /* ENABLE_USB_HOST */
