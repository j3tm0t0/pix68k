# ふろっぴU君 (FUK) の偽物の Pico 2 (RP2350) のファーム。README.md を参照
# ビルド: PICO_SDK_PATH を pico-sdk (2.1 以降) に向けて
#   cmake -S tools/pico_fukemu -B build/pico_fukemu -DPICO_BOARD=pico2 && cmake --build build/pico_fukemu
# イメージはファームに入れない (mkimg.py で別の UF2 を作って書く)
# 無線 LAN (mDNS と TCP の状態) を入れるとき: -DPICO_BOARD=pico2_w -DPICO_FUKEMU_WIFI_FILE=... -DPICO_FUKEMU_TOKEN_FILE=...
cmake_minimum_required(VERSION 3.13)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(pico_fukemu C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(pico_fukemu main.c core.c usb_descriptors.c)
target_include_directories(pico_fukemu PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_options(pico_fukemu PRIVATE -Wall -Wextra)
target_link_libraries(pico_fukemu PRIVATE pico_stdlib hardware_flash hardware_sync pico_bootrom tinyusb_device)
# 版 (cmake を走らせたときの git のコミット)
find_package(Git QUIET)
set(fukemu_version unknown)
if(GIT_FOUND)
  execute_process(COMMAND ${GIT_EXECUTABLE} describe --always --dirty --abbrev=7
                  WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR} OUTPUT_VARIABLE fukemu_version
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT fukemu_version)
    set(fukemu_version unknown)
  endif()
endif()
target_compile_definitions(pico_fukemu PRIVATE PICO_FUKEMU_VERSION=\"${fukemu_version}\" PICO_FUKEMU_FIRMWARE=1)

set(kbd_dir ${CMAKE_CURRENT_LIST_DIR}/../pico_kbd)
set(PICO_FUKEMU_WIFI_FILE "" CACHE FILEPATH "SSID とパスワードのファイル (空なら無線 LAN なし)")
set(PICO_FUKEMU_TOKEN_FILE "" CACHE FILEPATH "TCP のトークンのファイル")
set(PICO_FUKEMU_HOSTNAME "pix68k-fukemu" CACHE STRING "DHCP と mDNS で名乗るホスト名")
set(PICO_FUKEMU_TCP_PORT "2323" CACHE STRING "状態を返す TCP のポート")
if(PICO_FUKEMU_WIFI_FILE)
  if(NOT PICO_CYW43_SUPPORTED)
    message(FATAL_ERROR "PICO_FUKEMU_WIFI_FILE は、無線のあるボード (pico2_w) でだけ使える")
  endif()
  if(NOT PICO_FUKEMU_TOKEN_FILE)
    message(FATAL_ERROR "PICO_FUKEMU_WIFI_FILE を渡すときは、PICO_FUKEMU_TOKEN_FILE も渡す")
  endif()
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  set(secrets_dir ${CMAKE_CURRENT_BINARY_DIR}/secrets)
  file(MAKE_DIRECTORY ${secrets_dir})
  # SSID・パスワード・トークンは、ビルド先のヘッダにだけ置く (表示しない、コミットしない)
  add_custom_command(
    OUTPUT ${secrets_dir}/pico_kbd_secrets.h
    COMMAND ${Python3_EXECUTABLE} ${kbd_dir}/gen_secrets.py
            ${PICO_FUKEMU_WIFI_FILE} ${PICO_FUKEMU_TOKEN_FILE} ${secrets_dir}/pico_kbd_secrets.h
    DEPENDS ${kbd_dir}/gen_secrets.py ${PICO_FUKEMU_WIFI_FILE} ${PICO_FUKEMU_TOKEN_FILE}
    VERBATIM)
  # net.c は pico_kbd のものをそのまま使う (行の読み取りとトークンの比べ方は pico_kbd の core.c)
  target_sources(pico_fukemu PRIVATE ${kbd_dir}/net.c ${kbd_dir}/core.c ${secrets_dir}/pico_kbd_secrets.h)
  target_include_directories(pico_fukemu PRIVATE ${secrets_dir})
  set_source_files_properties(${kbd_dir}/net.c ${kbd_dir}/core.c PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter")
  target_compile_definitions(pico_fukemu PRIVATE PICO_KBD_WIFI=1 PICO_KBD_TCP_PORT=${PICO_FUKEMU_TCP_PORT}
                             CYW43_HOST_NAME=\"${PICO_FUKEMU_HOSTNAME}\")
  target_link_libraries(pico_fukemu PRIVATE pico_cyw43_arch_lwip_poll pico_lwip_mdns pico_multicore pico_flash)
  # コア 1 のスタックは main.c の s_core1_stack (8KB) を使う。SCRATCH_X には取らない
  target_compile_definitions(pico_fukemu PRIVATE PICO_CORE1_STACK_SIZE=0)
  # そのとき SDK の multicore_launch_core1 は panic だけになり、引数が使われないと言われるので黙らせる
  set_source_files_properties(${PICO_SDK_PATH}/src/rp2_common/pico_multicore/multicore.c PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter")
endif()

# 無線 LAN 越しの更新 (README.md の「無線 LAN 越しの更新」)。pico_kbd の ota.c と ota_rp2350.c をそのまま使う。
# フラッシュには先に pt.json のパーティション表を書く (A と B は設定とイメージの場所より前に置く)。
# PICO_FUKEMU_OTA_TBYB=1 は O 命令で送るイメージ用 (起動して確定しなければ、ブート ROM が前のイメージに戻す)。
# 版の番号はビルドのたびに大きくする。既定は、2026-01-01 から configure したときまでの分の数
option(PICO_FUKEMU_OTA "無線 LAN 越しの更新を入れる (PICO_FUKEMU_WIFI_FILE が要る)" OFF)
option(PICO_FUKEMU_OTA_TBYB "O 命令で送るイメージにする" OFF)
set(PICO_FUKEMU_OTA_VERSION "" CACHE STRING "イメージの版の番号 (1 から 4294967295。空なら時刻から作る)")
set(fukemu_size_limit 1044480) # 0x0FF000
if(PICO_FUKEMU_OTA)
  if(NOT PICO_FUKEMU_WIFI_FILE OR NOT PICO_RP2350)
    message(FATAL_ERROR "PICO_FUKEMU_OTA は、無線 LAN のビルド (pico2_w、PICO_FUKEMU_WIFI_FILE) でだけ使える")
  endif()
  # ota.c はコア 1 からフラッシュを書く。コア 0 を止めるのは、SDK の rom_flash_op / rom_explicit_buy が中で
  # flash_safe_execute を通るからで、それを確かめたのは pico-sdk 2.3.1。それより古い版では断る
  if(PICO_SDK_VERSION_STRING VERSION_LESS 2.3.1)
    message(FATAL_ERROR "PICO_FUKEMU_OTA には pico-sdk 2.3.1 以降が要る (今は ${PICO_SDK_VERSION_STRING})")
  endif()
  set(ota_version "${PICO_FUKEMU_OTA_VERSION}")
  if(NOT ota_version)
    string(TIMESTAMP ota_now "%s" UTC)
    math(EXPR ota_version "(${ota_now} - 1767225600) / 60")
  endif()
  math(EXPR ota_major "${ota_version} >> 16")
  math(EXPR ota_minor "${ota_version} & 65535")
  message(STATUS "pico_fukemu の OTA の版の番号: ${ota_version}")
  target_sources(pico_fukemu PRIVATE ${kbd_dir}/ota.c ${kbd_dir}/ota_rp2350.c netwrite.c)
  set_source_files_properties(${kbd_dir}/ota.c ${kbd_dir}/ota_rp2350.c PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter")
  # PICO_FLASH_ASSUME_CORE1_SAFE は付けない (コア 1 から書くとき、flash_safe_execute がコア 0 を止める)
  target_compile_definitions(pico_fukemu PRIVATE PICO_FUKEMU_OTA=1 PICO_KBD_OTA=1 PICO_KBD_VERSION=${ota_version}
                             PICO_CRT0_VERSION_MAJOR=${ota_major} PICO_CRT0_VERSION_MINOR=${ota_minor})
  if(PICO_FUKEMU_OTA_TBYB)
    target_compile_definitions(pico_fukemu PRIVATE PICO_CRT0_IMAGE_TYPE_TBYB=1)
  endif()
  target_link_libraries(pico_fukemu PRIVATE boot_picobin_headers boot_picoboot_headers boot_uf2_headers)
  set(fukemu_size_limit 507904) # 496KB
endif()
pico_enable_stdio_usb(pico_fukemu 0)
pico_enable_stdio_uart(pico_fukemu 0)
pico_add_extra_outputs(pico_fukemu)
# ファームが置き場所 (設定の場所の前、OTA ならパーティション) に収まることを確かめる
add_custom_command(TARGET pico_fukemu POST_BUILD
  COMMAND ${CMAKE_COMMAND} -DBIN=$<TARGET_FILE_DIR:pico_fukemu>/pico_fukemu.bin -DLIMIT=${fukemu_size_limit}
          -P ${CMAKE_CURRENT_LIST_DIR}/check_size.cmake)
