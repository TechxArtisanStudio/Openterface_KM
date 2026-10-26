#include "include/usb_mode_config.h"
#include "ch32v20x_flash.h"
#include "debug.h"

#define USB_MODE_CONFIG_MAGIC             0x4D465355u
#define USB_MODE_CONFIG_VERSION           1u
#define USB_MODE_CONFIG_FLASH_BASE        0x08000000u
#define USB_MODE_CONFIG_FLASH_SIZE        (448u * 1024u)
#define USB_MODE_CONFIG_FLASH_PAGE_SIZE   4096u
#define USB_MODE_CONFIG_FLASH_ADDR        (USB_MODE_CONFIG_FLASH_BASE + USB_MODE_CONFIG_FLASH_SIZE - USB_MODE_CONFIG_FLASH_PAGE_SIZE)

typedef struct {
    uint32_t magic;
    uint8_t record_version;
    uint8_t selected_mode;
    uint16_t flags;
    uint32_t sequence;
    uint32_t crc;
} USB_ModeConfig_Record_t;

static uint8_t g_active_mode = USB_WORK_MODE;
static uint8_t g_stored_mode = USB_WORK_MODE;
static uint16_t g_stored_flags;
static uint8_t g_status;
static volatile uint8_t g_reboot_pending;

static uint32_t config_crc(const USB_ModeConfig_Record_t *record)
{
    const uint8_t *bytes = (const uint8_t *)record;
    uint32_t crc = 2166136261u;
    uint16_t offset;

    for (offset = 0; offset < (sizeof(*record) - sizeof(record->crc)); offset++) {
        crc ^= bytes[offset];
        crc *= 16777619u;
    }
    return crc;
}

static uint8_t mode_is_valid(uint8_t mode)
{
    return (mode == USB_WORK_MODE_HID_ACM ||
            mode == USB_WORK_MODE_HID_UAC_ACM ||
            mode == USB_WORK_MODE_HID_ACM_BRIDGE ||
            mode == USB_WORK_MODE_HID_ECM_BRIDGE ||
            mode == USB_WORK_MODE_HID_UAC_ECM ||
            mode == USB_WORK_MODE_HID_ACM_ECM ||
            mode == USB_WORK_MODE_HID_ACM_ECM_BRIDGE) ? 1u : 0u;
}

static uint8_t mode_is_runtime_supported(uint8_t mode)
{
#if USB_WORK_MODE_RUNTIME_PROFILES
    return (mode == USB_WORK_MODE_HID_ACM ||
            mode == USB_WORK_MODE_HID_UAC_ACM ||
            mode == USB_WORK_MODE_HID_ACM_BRIDGE ||
            mode == USB_WORK_MODE_HID_ECM_BRIDGE ||
            mode == USB_WORK_MODE_HID_UAC_ECM ||
            mode == USB_WORK_MODE_HID_ACM_ECM ||
            mode == USB_WORK_MODE_HID_ACM_ECM_BRIDGE) ? 1u : 0u;
#else
    return (mode == USB_WORK_MODE) ? 1u : 0u;
#endif
}

static const USB_ModeConfig_Record_t *flash_record(void)
{
    return (const USB_ModeConfig_Record_t *)USB_MODE_CONFIG_FLASH_ADDR;
}

void USB_ModeConfig_Init(void)
{
    const USB_ModeConfig_Record_t *record = flash_record();

    g_active_mode = USB_WORK_MODE;
    g_stored_mode = USB_WORK_MODE;
    g_stored_flags = 0;
    g_status = 0;

    if (record->magic == USB_MODE_CONFIG_MAGIC &&
        record->record_version == USB_MODE_CONFIG_VERSION &&
        mode_is_valid(record->selected_mode) &&
        record->crc == config_crc(record)) {
        g_stored_mode = record->selected_mode;
        g_stored_flags = record->flags;
        g_status |= USB_MODE_CONFIG_STATUS_VALID;

        /* Compiled-mode priority: only use the stored mode if it matches
         * the compiled mode.  A mismatch means the firmware was rebuilt
         * with a different USB_WORK_MODE — in that case ignore the stale
         * flash record so the new descriptor/endpoints take effect
         * immediately without requiring a manual flash erase.             */
        if (record->selected_mode == USB_WORK_MODE &&
            mode_is_runtime_supported(record->selected_mode)) {
            g_active_mode = record->selected_mode;
        } else {
            /* Stale record — discard it directly (don't call Clear(),
             * which would recurse back into Init).                        */
            g_status |= USB_MODE_CONFIG_STATUS_MISMATCH;
            FLASH_Unlock();
            FLASH_ErasePage(USB_MODE_CONFIG_FLASH_ADDR);
            FLASH_Lock();
        }
    }
}

uint8_t USB_ModeConfig_GetCompiledMode(void)
{
    return USB_WORK_MODE;
}

uint8_t USB_ModeConfig_GetActiveMode(void)
{
    return g_active_mode;
}

uint8_t USB_ModeConfig_GetStoredMode(void)
{
    return g_stored_mode;
}

uint16_t USB_ModeConfig_GetFlags(void)
{
    return g_stored_flags;
}

uint8_t USB_ModeConfig_GetStatus(void)
{
    return g_status;
}

uint32_t USB_ModeConfig_GetCapabilityMask(void)
{
#if USB_WORK_MODE_RUNTIME_PROFILES
    return ((uint32_t)1u << USB_WORK_MODE_HID_ACM) |
            ((uint32_t)1u << USB_WORK_MODE_HID_UAC_ACM) |
           ((uint32_t)1u << USB_WORK_MODE_HID_ACM_BRIDGE) |
           ((uint32_t)1u << USB_WORK_MODE_HID_ECM_BRIDGE) |
           ((uint32_t)1u << USB_WORK_MODE_HID_UAC_ECM) |
           ((uint32_t)1u << USB_WORK_MODE_HID_ACM_ECM) |
           ((uint32_t)1u << USB_WORK_MODE_HID_ACM_ECM_BRIDGE);
#else
    return (uint32_t)1u << USB_WORK_MODE;
#endif
}

uint8_t USB_ModeConfig_IsModeSupported(uint8_t mode)
{
    return mode_is_runtime_supported(mode);
}

uint8_t USB_ModeConfig_ModeHasTargetUac(uint8_t mode)
{
    return ((mode == USB_WORK_MODE_HID_UAC_ACM) ||
            (mode == USB_WORK_MODE_HID_UAC_ECM)) ? 1u : 0u;
}

uint8_t USB_ModeConfig_ModeHasHostUac(uint8_t mode)
{
    return ((mode == USB_WORK_MODE_HID_UAC_ACM) ||
            (mode == USB_WORK_MODE_HID_UAC_ECM)) ? 1u : 0u;
}

uint8_t USB_ModeConfig_ModeHasTargetCdcAcm(uint8_t mode)
{
    return (mode == USB_WORK_MODE_HID_ACM_BRIDGE) ? 1u : 0u;
}

uint8_t USB_ModeConfig_ModeHasHostCdcAcm(uint8_t mode)
{
    return (mode == USB_WORK_MODE_HID_ACM ||
            mode == USB_WORK_MODE_HID_UAC_ACM ||
            mode == USB_WORK_MODE_HID_ACM_BRIDGE ||
            mode == USB_WORK_MODE_HID_ACM_ECM ||
            mode == USB_WORK_MODE_HID_ACM_ECM_BRIDGE) ? 1u : 0u;
}

uint8_t USB_ModeConfig_ModeHasTargetCdcEcm(uint8_t mode)
{
    return ((mode == USB_WORK_MODE_HID_ECM_BRIDGE) ||
            (mode == USB_WORK_MODE_HID_UAC_ECM) ||
            (mode == USB_WORK_MODE_HID_ACM_ECM) ||
            (mode == USB_WORK_MODE_HID_ACM_ECM_BRIDGE)) ? 1u : 0u;
}

uint8_t USB_ModeConfig_ModeHasHostCdcEcm(uint8_t mode)
{
    return ((mode == USB_WORK_MODE_HID_ECM_BRIDGE) ||
            (mode == USB_WORK_MODE_HID_UAC_ECM) ||
            (mode == USB_WORK_MODE_HID_ACM_ECM) ||
            (mode == USB_WORK_MODE_HID_ACM_ECM_BRIDGE)) ? 1u : 0u;
}

uint8_t USB_ModeConfig_ModeBridgeAcm(uint8_t mode)
{
    return (mode == USB_WORK_MODE_HID_ACM_BRIDGE) ? 1u : 0u;
}

uint8_t USB_ModeConfig_ModeBridgeEcm(uint8_t mode)
{
    return ((mode == USB_WORK_MODE_HID_ECM_BRIDGE) ||
            (mode == USB_WORK_MODE_HID_UAC_ECM) ||
            (mode == USB_WORK_MODE_HID_ACM_ECM) ||
            (mode == USB_WORK_MODE_HID_ACM_ECM_BRIDGE)) ? 1u : 0u;
}

uint8_t USB_ModeConfig_ModeBridgeUac(uint8_t mode)
{
    return ((mode == USB_WORK_MODE_HID_UAC_ACM) ||
            (mode == USB_WORK_MODE_HID_UAC_ECM)) ? 1u : 0u;
}

uint8_t USB_ModeConfig_ActiveModeHasTargetUac(void)
{
    return USB_ModeConfig_ModeHasTargetUac(g_active_mode);
}

uint8_t USB_ModeConfig_ActiveModeHasHostUac(void)
{
    return USB_ModeConfig_ModeHasHostUac(g_active_mode);
}

uint8_t USB_ModeConfig_ActiveModeHasTargetCdcAcm(void)
{
    return USB_ModeConfig_ModeHasTargetCdcAcm(g_active_mode);
}

uint8_t USB_ModeConfig_ActiveModeHasHostCdcAcm(void)
{
    return USB_ModeConfig_ModeHasHostCdcAcm(g_active_mode);
}

uint8_t USB_ModeConfig_ActiveModeHasTargetCdcEcm(void)
{
    return USB_ModeConfig_ModeHasTargetCdcEcm(g_active_mode);
}

uint8_t USB_ModeConfig_ActiveModeHasHostCdcEcm(void)
{
    return USB_ModeConfig_ModeHasHostCdcEcm(g_active_mode);
}

uint8_t USB_ModeConfig_ActiveModeBridgeAcm(void)
{
    return USB_ModeConfig_ModeBridgeAcm(g_active_mode);
}

uint8_t USB_ModeConfig_ActiveModeBridgeEcm(void)
{
    return USB_ModeConfig_ModeBridgeEcm(g_active_mode);
}

uint8_t USB_ModeConfig_ActiveModeBridgeUac(void)
{
    return USB_ModeConfig_ModeBridgeUac(g_active_mode);
}

uint8_t USB_ModeConfig_Write(uint8_t mode, uint16_t flags)
{
    USB_ModeConfig_Record_t record;
    FLASH_Status status;
    uint16_t word_index;
    const uint32_t *words;

    if (!USB_ModeConfig_IsModeSupported(mode)) {
        return 0;
    }

    record.magic = USB_MODE_CONFIG_MAGIC;
    record.record_version = USB_MODE_CONFIG_VERSION;
    record.selected_mode = mode;
    record.flags = flags;
    record.sequence = flash_record()->sequence + 1u;
    record.crc = config_crc(&record);

    FLASH_Unlock();
    status = FLASH_ErasePage(USB_MODE_CONFIG_FLASH_ADDR);
    if (status == FLASH_COMPLETE) {
        words = (const uint32_t *)&record;
        for (word_index = 0; word_index < (sizeof(record) / sizeof(uint32_t)); word_index++) {
            status = FLASH_ProgramWord(USB_MODE_CONFIG_FLASH_ADDR + ((uint32_t)word_index * sizeof(uint32_t)), words[word_index]);
            if (status != FLASH_COMPLETE) {
                break;
            }
        }
    }
    FLASH_Lock();

    if (status == FLASH_COMPLETE) {
        USB_ModeConfig_Init();
        return 1;
    }
    return 0;
}

uint8_t USB_ModeConfig_Clear(void)
{
    FLASH_Status status;

    FLASH_Unlock();
    status = FLASH_ErasePage(USB_MODE_CONFIG_FLASH_ADDR);
    FLASH_Lock();

    if (status == FLASH_COMPLETE) {
        USB_ModeConfig_Init();
        return 1;
    }
    return 0;
}

void USB_ModeConfig_Reboot(void)
{
    NVIC_SystemReset();
}

void USB_ModeConfig_RequestReboot(void)
{
    g_reboot_pending = 1;
}

void USB_ModeConfig_Poll(void)
{
    if (g_reboot_pending != 0) {
        NVIC_SystemReset();
    }
}