// experimental_usb_fdd_write= で、どの USB-FDD に書いてよいかを決める (#386)。-1 なら書かない、-2 なら値が不正 (書かない)。
// pix68k.cfg そのものを読めて、そこに書いてあるときだけ。次のときは、書いてあっても -1 にする:
//   - pix68k.cfg を読めない (readOk が false)
//   - 書き直しの残りの pix68k.new を読んだ (readPath が cfgPath と違う)
//   - 起動の組み合わせのファイルに書いてある (base には pix68k.cfg の中身だけを渡す)
// value には、読んだ値を入れる (知らせるため)。ベアメタルでも使えるよう、例外、RTTI、STL は使わない。
#pragma once

#include <stddef.h>
#include <string.h>

#include "platform/config_text.h"
#include "platform/real_floppy.h"

namespace platform {

inline int usbFddWriteUnitFrom(bool readOk, const char* readPath, const char* cfgPath, const ConfigText& base, char* value,
                               size_t size) {
    value[0] = 0;
    if (!readOk || readPath == nullptr || cfgPath == nullptr || strcmp(readPath, cfgPath) != 0) return -1;
    if (!base.get("experimental_usb_fdd_write", value, size)) return -1;
    return usbFddWriteUnit(value);
}

} // namespace platform
