# 実機の試験用: Raspberry Pi Pico (RP2040) を USB キーボード兼 UART の中継にするファームウェア。
# Pico W は -DPICO_BOARD=pico_w、Pico 2 W は -DPICO_BOARD=pico2_w を付ける (README.md を参照)。
# ビルド: PICO_SDK_PATH を pico-sdk (2.1 以降) に向けて
#   cmake -S tools/pico_kbd -B build/pico_kbd && cmake --build build/pico_kbd
cmake_minimum_required(VERSION 3.13)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(pico_kbd C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(pico_kbd main.c core.c usb_descriptors.c)
target_include_directories(pico_kbd PRIVATE ${CMAKE_CURRENT_LIST_DIR})
# panic で止まらずに再起動する (main.c の app_panic)
target_compile_definitions(pico_kbd PRIVATE PICO_PANIC_FUNCTION=app_panic)
target_link_libraries(pico_kbd PRIVATE pico_stdlib pico_unique_id hardware_irq hardware_uart hardware_watchdog tinyusb_device tinyusb_board)
# GP3 と Pi 5 の J2 の間に NPN トランジスタを挟む配線では 1 にする (J 命令が High を出す)
if(PICO_KBD_PWR_ACTIVE_HIGH)
  target_compile_definitions(pico_kbd PRIVATE PICO_KBD_PWR_ACTIVE_HIGH=1)
endif()
# 無線 LAN で命令を受けるビルド (Pico W / Pico 2 W)。PICO_KBD_WIFI_FILE に、1 行目が SSID、2 行目がパスワードのファイルを、
# PICO_KBD_TOKEN_FILE に、1 行目が TCP のトークンのファイルを渡す。中身は cmake の変数にもログにも入れず、
# gen_secrets.py がビルド先のヘッダ (pico_kbd_secrets.h) に変える。できた elf / uf2 には中身が入るので、配らないこと。
# SDK に lib/cyw43-driver と lib/lwip のサブモジュールが要る
set(PICO_KBD_WIFI_FILE "" CACHE FILEPATH "SSID とパスワードのファイル (空なら無線 LAN なし)")
set(PICO_KBD_TOKEN_FILE "" CACHE FILEPATH "TCP のトークンのファイル")
set(PICO_KBD_HOSTNAME "pix68k-pico" CACHE STRING "DHCP で名乗るホスト名")
set(PICO_KBD_TCP_PORT "2323" CACHE STRING "命令を受ける TCP のポート")
set(PICO_KBD_WIFI_REBOOT_MIN "10" CACHE STRING "無線 LAN につながらないままこの分数がたったら再起動する (0 なら、しない)")
if(PICO_KBD_WIFI_FILE)
  if(NOT PICO_CYW43_SUPPORTED)
    message(FATAL_ERROR "PICO_KBD_WIFI_FILE は、無線のあるボード (pico_w、pico2_w) でだけ使える")
  endif()
  if(NOT PICO_KBD_TOKEN_FILE)
    message(FATAL_ERROR "PICO_KBD_WIFI_FILE を渡すときは、PICO_KBD_TOKEN_FILE も渡す")
  endif()
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(secrets_dir ${CMAKE_CURRENT_BINARY_DIR}/secrets)
  file(MAKE_DIRECTORY ${secrets_dir})
  add_custom_command(
    OUTPUT ${secrets_dir}/pico_kbd_secrets.h
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/gen_secrets.py
            ${PICO_KBD_WIFI_FILE} ${PICO_KBD_TOKEN_FILE} ${secrets_dir}/pico_kbd_secrets.h
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/gen_secrets.py ${PICO_KBD_WIFI_FILE} ${PICO_KBD_TOKEN_FILE}
    VERBATIM)
  target_sources(pico_kbd PRIVATE net.c ${secrets_dir}/pico_kbd_secrets.h)
  target_include_directories(pico_kbd PRIVATE ${secrets_dir})
  target_compile_definitions(pico_kbd PRIVATE PICO_KBD_WIFI=1 PICO_KBD_TCP_PORT=${PICO_KBD_TCP_PORT}
                             PICO_KBD_WIFI_REBOOT_MIN=${PICO_KBD_WIFI_REBOOT_MIN}
                             CYW43_HOST_NAME=\"${PICO_KBD_HOSTNAME}\")
  target_link_libraries(pico_kbd PRIVATE pico_cyw43_arch_lwip_poll pico_lwip_mdns)
endif()
# 無線 LAN 越しの更新 (ota.c と README.md を参照)。RP2350 は、フラッシュに pt.json のパーティション表を書いてから使う。
# PICO_KBD_OTA_TBYB=1 は、O 命令で送るイメージ用 (起動して確定しなければ、ブート ROM が前のイメージに戻す)。
# UF2 で最初に書くイメージには付けない。
# 版の番号はビルドのたびに大きくする。既定は、2026-01-01 から configure したときまでの分の数
option(PICO_KBD_OTA "無線 LAN 越しの更新を入れる" OFF)
option(PICO_KBD_OTA_TBYB "O 命令で送るイメージにする" OFF)
set(PICO_KBD_VERSION "" CACHE STRING "イメージの版の番号 (1 から 4294967295。空なら時刻から作る)")
if(PICO_KBD_OTA)
  set(kbd_version "${PICO_KBD_VERSION}")
  if(NOT kbd_version)
    string(TIMESTAMP kbd_now "%s" UTC)
    math(EXPR kbd_version "(${kbd_now} - 1767225600) / 60")
  endif()
  math(EXPR kbd_version_major "${kbd_version} >> 16")
  math(EXPR kbd_version_minor "${kbd_version} & 65535")
  message(STATUS "pico_kbd の版の番号: ${kbd_version} (${kbd_version_major}.${kbd_version_minor})")
  target_sources(pico_kbd PRIVATE ota.c)
  if(PICO_RP2350)
    target_sources(pico_kbd PRIVATE ota_rp2350.c)
  else()
    # RP2040: ブートローダ (boot2040/) のうしろの MAIN で動くようにリンクする (otaboot.h)。
    # pico_add_linker_script_override_path のある pico-sdk (2.3 以降) が要る (2.2.0 では止まる)
    target_sources(pico_kbd PRIVATE ota_rp2040.c otaboot.c)
    pico_add_linker_script_override_path(pico_kbd ${CMAKE_CURRENT_LIST_DIR}/rp2040_ota_ld FILES pico_flash_region.ld)
    target_link_libraries(pico_kbd PRIVATE hardware_flash hardware_sync)
  endif()
  # コア 1 は使わないので、フラッシュを書く間は割り込みを止めるだけでよい (SDK の flash_safe_execute が、
  # pico_multicore の入ったビルドでコア 1 を止めようとして失敗しないようにする)
  target_compile_definitions(pico_kbd PRIVATE PICO_KBD_OTA=1 PICO_KBD_VERSION=${kbd_version} PICO_FLASH_ASSUME_CORE1_SAFE=1
                             PICO_CRT0_VERSION_MAJOR=${kbd_version_major} PICO_CRT0_VERSION_MINOR=${kbd_version_minor})
  option(PICO_KBD_OTA_NEVER_BUY "確定しない版にする (前の版に戻る動きを確かめるため)" OFF)
  if(PICO_KBD_OTA_NEVER_BUY)
    target_compile_definitions(pico_kbd PRIVATE PICO_KBD_OTA_NEVER_BUY=1)
  endif()
  if(PICO_KBD_OTA_TBYB)
    target_compile_definitions(pico_kbd PRIVATE PICO_CRT0_IMAGE_TYPE_TBYB=1)
  endif()
  if(PICO_RP2350)
    target_link_libraries(pico_kbd PRIVATE pico_flash boot_picobin_headers boot_picoboot_headers boot_uf2_headers)
  endif()
endif()
pico_add_extra_outputs(pico_kbd)
