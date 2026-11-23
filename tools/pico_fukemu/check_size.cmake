# ファームの大きさが、置き場所に収まることを確かめる。LIMIT は、OTA なしなら設定の場所 (layout.h の FUKEMU_FW_MAX = 0x0FF000)、
# OTA のビルドなら pt.json の A / B のパーティションの大きさ (496KB)
file(SIZE ${BIN} size)
if(size GREATER ${LIMIT})
  message(FATAL_ERROR "pico_fukemu.bin が ${size} バイトで、置き場所 (${LIMIT} バイト) に収まらない")
endif()
