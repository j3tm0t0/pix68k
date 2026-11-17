// USB の記述子: boot プロトコルのキーボード 1 つ (Circle の ukbd が使う形。レポート ID なし、8 バイト)
#include <string.h>
#include "pico/unique_id.h"
#include "tusb.h"

// VID:PID は TinyUSB の例と同じ試験用の値 (販売しない機器のため)
static const tusb_desc_device_t kDevice = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0xCafe,
    .idProduct = 0x4068,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

uint8_t const* tud_descriptor_device_cb(void) { return (uint8_t const*)&kDevice; }

static const uint8_t kReport[] = {TUD_HID_REPORT_DESC_KEYBOARD()};

uint8_t const* tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return kReport;
}

#define EPNUM_HID 0x81
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

static const uint8_t kConfig[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_TOTAL_LEN, 0, 100),
    // インターフェース 0、文字列 4、boot プロトコルのキーボード、エンドポイント 0x81、8 バイト、10ms ごと
    TUD_HID_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_KEYBOARD, sizeof(kReport), EPNUM_HID, 8, 10),
};

uint8_t const* tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return kConfig;
}

// 添字は文字列記述子の番号。0 は言語の一覧、3 はシリアル番号 (どちらも下で別に作る)
static const char* const kStrings[] = {NULL, "pix68k", "pix68k test keyboard", NULL, "Keyboard"};

static uint16_t s_str[32];

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    const char* s;
    if (index == 0) {
        s_str[1] = 0x0409;
        s_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | 4);
        return s_str;
    }
    if (index == 3) {
        pico_get_unique_board_id_string(serial, sizeof serial);
        s = serial;
    } else if (index < sizeof kStrings / sizeof kStrings[0] && kStrings[index] != NULL) {
        s = kStrings[index];
    } else {
        return NULL;
    }
    size_t n = strlen(s);
    if (n > 31) n = 31;
    for (size_t i = 0; i < n; i++) s_str[1 + i] = (uint8_t)s[i];
    s_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return s_str;
}
