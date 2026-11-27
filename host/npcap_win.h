// Windows 版: Npcap の wpcap.dll を実行時に読み込んで、libpcap の関数を使う (host_ether.cpp の neptune_link=pcap:IF)。
// wpcap.dll は Windows の標準ではなく、Npcap (https://npcap.com/) を入れた PC の System32\Npcap にだけある。
// リンクすると Npcap のない PC で exe が起動しなくなるので、pcap:IF を使うときだけ読む。ビルドに Npcap SDK は要らない。
// 使う型と関数だけを、Npcap の pcap.h と同じ形で宣言する (関数は同じ名前の関数ポインタ。呼ぶ側は libpcap と同じに書ける)。
// host_ether.cpp からだけ include する (どれも static)
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// <windows.h> の古いマクロ。x68k のヘッダの引数名 (far) とぶつかる
#undef far
#undef near

#include <stdio.h>
#include <string.h>

typedef unsigned char u_char;
typedef unsigned int bpf_u_int32;
struct pcap_t;
struct pcap_addr;
struct pcap_pkthdr {
    struct {
        long tv_sec; // Windows の long は 32 ビット (Npcap の pcap.h の struct timeval と同じ)
        long tv_usec;
    } ts;
    bpf_u_int32 caplen;
    bpf_u_int32 len;
};
struct bpf_program {
    unsigned int bf_len;
    void* bf_insns;
};
struct pcap_if_t {
    pcap_if_t* next;
    char* name;
    char* description;
    pcap_addr* addresses;
    bpf_u_int32 flags;
};

#define PCAP_ERRBUF_SIZE 256
#define DLT_EN10MB 1
#define PCAP_D_IN 1
#define PCAP_NETMASK_UNKNOWN 0xffffffffu

static pcap_t* (*pcap_create)(const char*, char*);
static int (*pcap_set_snaplen)(pcap_t*, int);
static int (*pcap_set_promisc)(pcap_t*, int);
static int (*pcap_set_timeout)(pcap_t*, int);
static int (*pcap_set_immediate_mode)(pcap_t*, int);
static int (*pcap_activate)(pcap_t*);
static int (*pcap_datalink)(pcap_t*);
static int (*pcap_setdirection)(pcap_t*, int);
static int (*pcap_compile)(pcap_t*, bpf_program*, const char*, int, bpf_u_int32);
static int (*pcap_setfilter)(pcap_t*, bpf_program*);
static void (*pcap_freecode)(bpf_program*);
static char* (*pcap_geterr)(pcap_t*);
static int (*pcap_setnonblock)(pcap_t*, int, char*);
static pcap_t* (*pcap_open_live)(const char*, int, int, int, char*);
static int (*pcap_inject)(pcap_t*, const void*, size_t);
static int (*pcap_next_ex)(pcap_t*, pcap_pkthdr**, const u_char**);
static void (*pcap_close)(pcap_t*);
static HANDLE (*pcap_getevent)(pcap_t*);
static int (*pcap_findalldevs)(pcap_if_t**, char*);
static void (*pcap_freealldevs)(pcap_if_t*);

namespace npcap_detail {
// GetProcAddress の値を関数ポインタに入れる (FARPROC からのキャストの警告を出さないよう、バイトで写す)
template <class F> bool take(HMODULE dll, const char* name, F& f) {
    FARPROC p = GetProcAddress(dll, name);
    static_assert(sizeof f == sizeof p, "function pointer size");
    memcpy(&f, &p, sizeof f);
    return p != nullptr;
}
} // namespace npcap_detail

// wpcap.dll を読み、上の関数をそろえる (2 回目からは結果だけ返す)。できなければ err に理由を入れて false
static bool loadNpcap(char* err, size_t errSize) {
    static int state = 0; // 0 まだ、1 読めた、-1 読めなかった
    static char why[MAX_PATH + 64];
    if (state == 0) {
        // System32\Npcap だけから読む (exe のディレクトリやカレントの wpcap.dll は使わない)。同じ場所の Packet.dll も
        // 見つかるよう、LOAD_WITH_ALTERED_SEARCH_PATH で読む
        char path[MAX_PATH];
        const UINT n = GetSystemDirectoryA(path, MAX_PATH);
        HMODULE dll = nullptr;
        if (n > 0 && n < MAX_PATH - 20) {
            snprintf(path + n, sizeof path - n, "\\Npcap\\wpcap.dll");
            dll = LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        }
        using npcap_detail::take;
        if (!dll) {
            snprintf(why, sizeof why, "Npcap is not installed (get it from https://npcap.com/)");
            state = -1;
        } else if (!take(dll, "pcap_create", pcap_create) || !take(dll, "pcap_set_snaplen", pcap_set_snaplen) ||
                   !take(dll, "pcap_set_promisc", pcap_set_promisc) || !take(dll, "pcap_set_timeout", pcap_set_timeout) ||
                   !take(dll, "pcap_set_immediate_mode", pcap_set_immediate_mode) ||
                   !take(dll, "pcap_activate", pcap_activate) || !take(dll, "pcap_datalink", pcap_datalink) ||
                   !take(dll, "pcap_setdirection", pcap_setdirection) || !take(dll, "pcap_compile", pcap_compile) ||
                   !take(dll, "pcap_setfilter", pcap_setfilter) || !take(dll, "pcap_freecode", pcap_freecode) ||
                   !take(dll, "pcap_geterr", pcap_geterr) || !take(dll, "pcap_setnonblock", pcap_setnonblock) ||
                   !take(dll, "pcap_open_live", pcap_open_live) || !take(dll, "pcap_inject", pcap_inject) ||
                   !take(dll, "pcap_next_ex", pcap_next_ex) || !take(dll, "pcap_close", pcap_close) ||
                   !take(dll, "pcap_getevent", pcap_getevent) || !take(dll, "pcap_findalldevs", pcap_findalldevs) ||
                   !take(dll, "pcap_freealldevs", pcap_freealldevs)) {
            snprintf(why, sizeof why, "%s is too old (update Npcap)", path);
            state = -1;
        } else {
            state = 1;
        }
    }
    if (state < 0) snprintf(err, errSize, "pcap: %s", why);
    return state > 0;
}

// 使えるインタフェースの一覧を out に出す (pcap:IF の IF は \Device\NPF_{...} で、名前からは分かりにくいため)
static void listNpcapDevices(FILE* out) {
    char ebuf[PCAP_ERRBUF_SIZE] = {};
    pcap_if_t* all = nullptr;
    if (pcap_findalldevs(&all, ebuf) != 0) {
        fprintf(out, "pcap: cannot list interfaces: %s\n", ebuf);
        return;
    }
    fprintf(out, "pcap: interfaces (use pcap:NAME):\n");
    for (pcap_if_t* d = all; d; d = d->next) fprintf(out, "  %s  %s\n", d->name, d->description ? d->description : "");
    pcap_freealldevs(all);
}
