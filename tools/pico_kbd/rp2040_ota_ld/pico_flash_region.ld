/* RP2040 の無線 LAN 越しの更新のビルド: ファームウェアは、ブートローダのうしろの MAIN (otaboot.h の OTAB_MAIN) で動く */
FLASH(rx) : ORIGIN = 0x10010000, LENGTH = 640k
