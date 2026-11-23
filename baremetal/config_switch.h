// config.txt の kernel= の値だけを書き換える (#369)。書き換えるところは、この関数だけ。
// HTTP の POST /config/kernel (host_net.cpp) と、メニューの「Roll back」(update_menu.cpp) が使う
#pragma once

#include <circle/types.h>

#include "platform/config_kernel.h"

// buf は CONFIG_SWITCH_BUF バイト以上の作業用。to のファイルの中身は、呼ぶ側が先に確かめる。途中で Yield() しない。
// 替えたら true。だめなら false で、status に HTTP の状態、error に理由を入れる ("ERROR:" のあるものは、変わったか分からない)
constexpr unsigned CONFIG_SWITCH_BUF = platform::CONFIG_TXT_MAX + 2 * platform::CONFIG_SECTOR;
bool SwitchConfigKernel(const char* drive, const char* const* sections, const char* from, const char* to, u8* buf,
                        int& status, const char*& error);
