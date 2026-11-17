// ホスト版の Neptune-X のフレームの送受信の口 (x68k::EtherLink)。--neptune-link / neptune_link= の値で選ぶ:
//   echo        ARP と ping に答える試験用の口 (platform/ether_peer.h の EchoPeer、既定)
//   loopback    送ったフレームをそのまま返す
//   pcap:IF     libpcap でホストのネットワークインタフェース IF に生のフレームを出し入れする (ビルドに libpcap が要る)。
//               IF をプロミスキャスにし、X68000 の MAC アドレス宛て、ブロードキャスト、マルチキャストだけを受け取る。
//               BPF (/dev/bpf*) を読み書きする権限が要る。有線のインタフェースで使う (Wi-Fi では、ほかの MAC アドレスの
//               フレームをアクセスポイントが通さないことが多い)
//   tap:NAME    Linux の tap デバイス NAME に出し入れする (あらかじめ作り、使う人に読み書きを許しておく)
// pcap と tap は受け取るスレッドを動かし、platform::FrameRing で X68000 のスレッドに渡す。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "x68k/ether_link.h"

// spec は neptune_link= の値 (空なら echo)。mac は X68000 の MAC アドレス (pcap のフィルタに使う)。
// 開けなければ nullptr を返し、err に理由を入れる。返した口は delete で閉じる
x68k::EtherLink* openHostEtherLink(const char* spec, const uint8_t mac[6], char* err, size_t errSize);
