// usb_descriptors.c -- 本物の FUK と同じ VID:PID (04D8:E6B1)、製造者名、製品名で、CDC を 2 本出す。シリアル番号は持たない
// (pix68k は VID:PID で探し、Linux では /dev/serial/by-id/usb-TNB_Factory._FDC_emulator-if00 と -if02 を開く)
#include "tusb.h"

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x04D8,
    .idProduct = 0xE6B1,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 0,
    .bNumConfigurations = 1,
};

const uint8_t *tud_descriptor_device_cb(void) { return (const uint8_t *)&desc_device; }

enum { ITF_CDC0 = 0, ITF_CDC0_DATA, ITF_CDC1, ITF_CDC1_DATA, ITF_TOTAL };
#define CONFIG_LEN (TUD_CONFIG_DESC_LEN + 2 * TUD_CDC_DESC_LEN)

static const uint8_t desc_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_TOTAL, 0, CONFIG_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_CDC0, 0, 0x81, 8, 0x02, 0x82, 64),
    TUD_CDC_DESCRIPTOR(ITF_CDC1, 0, 0x83, 8, 0x04, 0x84, 64),
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_config;
}

static const char *const strings[] = {NULL, "TNB Factory.", "FDC emulator"};

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t buf[32];
    int n;
    if (index == 0) {
        buf[1] = 0x0409;
        n = 1;
    } else {
        if (index >= sizeof strings / sizeof strings[0]) return NULL;
        const char *s = strings[index];
        for (n = 0; s[n] && n < 31; n++) buf[1 + n] = (uint8_t)s[n];
    }
    buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return buf;
}
