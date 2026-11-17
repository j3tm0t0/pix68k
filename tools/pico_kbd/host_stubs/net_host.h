// test_net_host.c が net.c をホストでビルドするための、lwIP と無線のチップと時計の代わり。中身は test_net_host.c にある
#ifndef HOST_STUB_NET_HOST_H
#define HOST_STUB_NET_HOST_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---- 時計 (ミリ秒。試験が進める) ----
typedef uint64_t absolute_time_t;
extern uint64_t g_nowMs;
static inline absolute_time_t get_absolute_time(void) { return g_nowMs; }
static inline absolute_time_t make_timeout_time_ms(uint32_t ms) { return g_nowMs + ms; }
static inline bool time_reached(absolute_time_t t) { return g_nowMs >= t; }
static inline uint32_t to_ms_since_boot(absolute_time_t t) { return (uint32_t)t; }

// ---- lwIP ----
typedef int8_t err_t;
typedef uint16_t u16_t;
enum { ERR_OK = 0, ERR_VAL = -6, ERR_ABRT = -13 };
typedef struct { uint32_t addr; } ip4_addr_t;
typedef ip4_addr_t ip_addr_t;
struct pbuf {
    struct pbuf* next;
    void* payload;
    u16_t tot_len, len;
};
struct tcp_pcb;
typedef err_t (*tcp_recv_fn)(void* arg, struct tcp_pcb* pcb, struct pbuf* p, err_t err);
typedef void (*tcp_err_fn)(void* arg, err_t err);
typedef err_t (*tcp_accept_fn)(void* arg, struct tcp_pcb* newpcb, err_t err);
struct tcp_pcb {
    ip_addr_t remote_ip;
    uint32_t keep_idle, keep_intvl, keep_cnt;
    tcp_recv_fn recv;
    tcp_err_fn errf;
    tcp_accept_fn accept;
    bool closed, aborted;
};
struct netif { ip4_addr_t ip; };
#define IPADDR_TYPE_ANY 0
#define IP_ANY_TYPE NULL
#define SOF_KEEPALIVE 8
#define TCP_WRITE_FLAG_COPY 1
#define ip_set_option(pcb, opt) ((void)(pcb))
#define ip_2_ip4(a) (a)
const char* ip4addr_ntoa(const ip4_addr_t* a);
static inline const ip4_addr_t* netif_ip4_addr(const struct netif* n) { return &n->ip; }
void tcp_arg(struct tcp_pcb* pcb, void* arg);
void tcp_recv(struct tcp_pcb* pcb, tcp_recv_fn fn);
void tcp_err(struct tcp_pcb* pcb, tcp_err_fn fn);
void tcp_accept(struct tcp_pcb* pcb, tcp_accept_fn fn);
err_t tcp_close(struct tcp_pcb* pcb);
void tcp_abort(struct tcp_pcb* pcb);
void tcp_recved(struct tcp_pcb* pcb, u16_t len);
u16_t tcp_sndqueuelen(struct tcp_pcb* pcb);
u16_t tcp_sndbuf(struct tcp_pcb* pcb);
err_t tcp_write(struct tcp_pcb* pcb, const void* data, u16_t len, uint8_t flags);
err_t tcp_output(struct tcp_pcb* pcb);
void tcp_nagle_disable(struct tcp_pcb* pcb);
struct tcp_pcb* tcp_new_ip_type(uint8_t type);
err_t tcp_bind(struct tcp_pcb* pcb, const ip_addr_t* addr, u16_t port);
struct tcp_pcb* tcp_listen_with_backlog(struct tcp_pcb* pcb, uint8_t backlog);
void pbuf_free(struct pbuf* p);
void mdns_resp_init(void);
void mdns_resp_add_netif(struct netif* n, const char* name);
void mdns_resp_announce(struct netif* n);

// ---- 無線のチップ ----
#define CYW43_ITF_STA 0
#define CYW43_COUNTRY_JAPAN 0
#define CYW43_PERFORMANCE_PM 0
#define CYW43_WL_GPIO_LED_PIN 0
#define CYW43_LINK_UP 3
#define CYW43_LINK_FAIL (-1)
#define CYW43_LINK_BADAUTH (-3)
#define CYW43_AUTH_OPEN 0
#define CYW43_AUTH_WPA2_AES_PSK 1
#define CYW43_AUTH_WPA3_WPA2_AES_PSK 2
#define CYW43_AUTH_WPA2_MIXED_PSK 3
#define CYW43_HOST_NAME "pix68k-test"
typedef struct { struct netif netif[1]; } cyw43_t;
extern cyw43_t cyw43_state;
int cyw43_arch_init_with_country(uint32_t country);
void cyw43_arch_enable_sta_mode(void);
void cyw43_wifi_pm(cyw43_t* self, uint32_t pm);
void cyw43_arch_poll(void);
void cyw43_arch_gpio_put(int pin, bool on);
int cyw43_arch_wifi_connect_async(const char* ssid, const char* pw, uint32_t auth);
int cyw43_tcpip_link_status(cyw43_t* self, int itf);
int cyw43_wifi_leave(cyw43_t* self, int itf);

// ---- 試験用の SSID、パスワード、トークン (本物ではない) ----
static const char kWifiSsid[] = "test-ssid";
static const char kWifiPassword[] = "test-password";
static const char kNetToken[] = "0123456789abcdef";
#endif
