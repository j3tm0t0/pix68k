// USB の記述子: boot プロトコルのキーボード (インターフェース 0。Circle の ukbd が使う形。レポート ID なし、8 バイト) と、
// C 命令で kg を選んだときだけ HID のゲームパッド (インターフェース 1。Circle の upad。TinyUSB の雛形の並びで 11 バイト:
// 8 ビットの軸 6 本 (-127〜127。いつも 0)、ハット 1 つ (論理 1〜8、0 は離した状態)、ボタン 32 個)
#include <stdbool.h>
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
static const uint8_t kPadReport[] = {TUD_HID_REPORT_DESC_GAMEPAD()};

// instance はインターフェースの番号と同じ (0 = キーボード、1 = ゲームパッド)
uint8_t const* tud_hid_descriptor_report_cb(uint8_t instance) { return instance == 1 ? kPadReport : kReport; }

#define EPNUM_HID 0x81
#define EPNUM_PAD 0x82
#define CONFIG_KBD_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)
#define CONFIG_KBD_PAD_LEN (TUD_CONFIG_DESC_LEN + 2 * TUD_HID_DESC_LEN)

static const uint8_t kConfig[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_KBD_LEN, 0, 100),
    // インターフェース 0、文字列 4、boot プロトコルのキーボード、エンドポイント 0x81、8 バイト、10ms ごと
    TUD_HID_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_KEYBOARD, sizeof(kReport), EPNUM_HID, 8, 10),
};

static const uint8_t kConfigPad[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, CONFIG_KBD_PAD_LEN, 0, 100),
    TUD_HID_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_KEYBOARD, sizeof(kReport), EPNUM_HID, 8, 10),
    // インターフェース 1、文字列 5、boot プロトコルでない HID、エンドポイント 0x82、16 バイト、10ms ごと
    TUD_HID_DESCRIPTOR(1, 5, HID_ITF_PROTOCOL_NONE, sizeof(kPadReport), EPNUM_PAD, 16, 10),
};

bool app_pad_enabled(void); // main.c (今のつながりでゲームパッドを見せるか)

uint8_t const* tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return app_pad_enabled() ? kConfigPad : kConfig;
}

// 添字は文字列記述子の番号。0 は言語の一覧、3 はシリアル番号 (どちらも下で別に作る)
static const char* const kStrings[] = {NULL, "pix68k", "pix68k test keyboard", NULL, "Keyboard", "Gamepad"};

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
