// fukemu_net.py の試験用: netwrite.c (W 命令) を TCP で受ける。フラッシュはメモリの上で、終わると out に書き出す
// 使い方: nwsim PORT TOKEN OUT    (R 命令を受けたら、フラッシュを OUT に書いて終わる)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "netwrite.h"

#define FLASH (4u << 20)
static uint8_t flash[FLASH];
static int fd;
static bool wr(uint32_t off, const uint8_t *p)
{
    if (off % 4096 || off + 4096 > FLASH) return false;
    memcpy(flash + off, p, 4096);
    return true;
}
static bool er(uint32_t off)
{
    if (off % 4096 || off + 4096 > FLASH) return false;
    memset(flash + off, 0xff, 4096);
    return true;
}
static const uint8_t *rd(uint32_t off) { return flash + off; }
static void rep(const char *s)
{
    char b[256];
    int n = snprintf(b, sizeof b, "#pico %s\n", s);
    if (write(fd, b, (size_t)n) != n) exit(1);
}
static bool ej(int unit) { (void)unit; return true; }
static const nw_io_t io = {wr, er, rd, rep, ej};

int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    memset(flash, 0xff, sizeof flash);
    nw_init(&io);
    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET, a.sin_port = htons((uint16_t)atoi(argv[1])), a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 1)) return 1;
    printf("ready\n");
    fflush(stdout);
    fd = accept(ls, NULL, NULL);
    char line[1024];
    size_t n = 0;
    bool authed = false;
    for (;;) {
        char c;
        if (read(fd, &c, 1) != 1) return 1;
        if (c != '\n') {
            if (n < sizeof line - 1) line[n++] = c;
            continue;
        }
        line[n] = 0, n = 0;
        if (!authed) {
            if (strcmp(line, argv[2]) != 0) return 1;
            authed = true;
            rep("auth ok");
        } else if (line[0] == 'W') {
            nw_command(line[1] == ' ' ? line + 2 : line + 1);
        } else if (line[0] == 'R') {
            rep("reboot");
            FILE *f = fopen(argv[3], "wb");
            fwrite(flash, 1, FLASH, f);
            fclose(f);
            return 0;
        }
    }
}
