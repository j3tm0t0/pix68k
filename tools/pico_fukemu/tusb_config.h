// TinyUSB の設定: CDC を 2 本 (インターフェース 0/1 と 2/3)。本物の FUK と同じ形
#pragma once
#define CFG_TUSB_RHPORT0_MODE OPT_MODE_DEVICE
#define CFG_TUSB_OS OPT_OS_PICO
#define CFG_TUD_ENDPOINT0_SIZE 64

#define CFG_TUD_CDC 2
#define CFG_TUD_MSC 0
#define CFG_TUD_HID 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0
#define CFG_TUD_CDC_RX_BUFSIZE 2048
#define CFG_TUD_CDC_TX_BUFSIZE 4096
#define CFG_TUD_CDC_EP_BUFSIZE 64
