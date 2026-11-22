// TinyUSB の設定: USB デバイスで、HID のインターフェースを 1 つだけ持つ
#ifndef PICO_KBD_TUSB_CONFIG_H
#define PICO_KBD_TUSB_CONFIG_H

#define CFG_TUSB_RHPORT0_MODE OPT_MODE_DEVICE
#define CFG_TUSB_OS OPT_OS_PICO
#define CFG_TUD_ENDPOINT0_SIZE 64

#define CFG_TUD_HID 2 // 0 = キーボード、1 = ゲームパッド (C 命令で kg を選んだときだけ見せる)
#define CFG_TUD_CDC 0
#define CFG_TUD_MSC 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0
#define CFG_TUD_HID_EP_BUFSIZE 16

#endif
