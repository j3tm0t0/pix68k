// ベアメタル版のネットワークの口の、ホストで試せる部分のテスト:
// platform/net_config.h (アドレス、送り元の制限)、net_http.h (要求の頭、URL、パス)、net_files.h (置き換えの手順、
// 断るファイル、PPM)、key_type.h (文字から USB のキー)
#include "platform/ether_mux.h"
#include "platform/key_type.h"
#include "platform/net_config.h"
#include "platform/net_files.h"
#include "platform/net_http.h"
#include "platform/setting_warning.h"
#include "platform/settings_file.h"
#include "test_check.h"

#include <stdio.h>
#include <string.h>

using namespace platform;

static void testConfig() {
    uint8_t ip[4];
    CHECK_EQ(parseIpv4("192.168.1.10", ip), true);
    CHECK_EQ(ip[0] == 192 && ip[1] == 168 && ip[2] == 1 && ip[3] == 10, true);
    CHECK_EQ(parseIpv4("256.1.1.1", ip), false);
    CHECK_EQ(parseIpv4("1.2.3", ip), false);
    CHECK_EQ(parseIpv4("1.2.3.4x", ip), false);
    CHECK_EQ(parseIpv4("1.2.3.0004", ip), false);
    CHECK_EQ(parseIpv4("", ip), false);
    int prefix = 0;
    CHECK_EQ(parseIpv4Cidr("10.0.2.15/24", ip, prefix), true);
    CHECK_EQ(prefix, 24);
    CHECK_EQ(parseIpv4Cidr("10.0.2.15", ip, prefix), true);
    CHECK_EQ(prefix, -1);
    CHECK_EQ(parseIpv4Cidr("10.0.2.15/33", ip, prefix), false);
    CHECK_EQ(parseIpv4Cidr("10.0.2.15/", ip, prefix), false);
    uint8_t mask[4];
    prefixToMask(20, mask);
    CHECK_EQ(mask[0] == 255 && mask[1] == 255 && mask[2] == 240 && mask[3] == 0, true);
    prefixToMask(0, mask);
    CHECK_EQ(mask[0] | mask[1] | mask[2] | mask[3], 0);
    prefixToMask(32, mask);
    CHECK_EQ(mask[3], 255);
    uint16_t port = 0;
    CHECK_EQ(parseIpv4Port("10.0.2.2:5514", ip, port, 514), true);
    CHECK_EQ(port, 5514);
    CHECK_EQ(parseIpv4Port("10.0.2.2", ip, port, 514), true);
    CHECK_EQ(port, 514);
    CHECK_EQ(parseIpv4Port("10.0.2.2:0", ip, port, 514), false);
    CHECK_EQ(parseIpv4Port("10.0.2.2:70000", ip, port, 514), false);
    CHECK_EQ(parseIpv4Port("router:514", ip, port, 514), false);

    const uint8_t a[4] = {192, 168, 1, 77}, b[4] = {10, 0, 0, 5}, c[4] = {10, 0, 0, 6};
    CHECK_EQ(allowListValid("192.168.1.0/24, 10.0.0.5"), true);
    CHECK_EQ(allowListValid("192.168.1.0/24,router"), false);
    CHECK_EQ(allowListHas("192.168.1.0/24,10.0.0.5", a), true);
    CHECK_EQ(allowListHas("192.168.1.0/24,10.0.0.5", b), true);
    CHECK_EQ(allowListHas("192.168.1.0/24,10.0.0.5", c), false);
    CHECK_EQ(allowListHas("", a), false);
    CHECK_EQ(allowListHas("0.0.0.0/0", c), true);
    CHECK_EQ(allowListHas("bad,10.0.0.6", c), false); // 読めない並びは誰も通さない

    // ログと画面を読むのは net_allow= だけ、書くのはトークンも。どちらもなければ書けない
    CHECK_EQ((int)checkNetAccess("", "", a, "", NetOp::READ), (int)NetAccess::OK);
    CHECK_EQ((int)checkNetAccess("", "", a, "", NetOp::WRITE), (int)NetAccess::NO_WRITE_RULE);
    CHECK_EQ((int)checkNetAccess("10.0.0.5", "", a, "", NetOp::READ), (int)NetAccess::DENIED_SOURCE);
    CHECK_EQ((int)checkNetAccess("10.0.0.5", "", b, "", NetOp::WRITE), (int)NetAccess::OK);
    CHECK_EQ((int)checkNetAccess("", "s3cret", a, "s3cret", NetOp::WRITE), (int)NetAccess::OK);
    CHECK_EQ((int)checkNetAccess("", "s3cret", a, "s3cre", NetOp::WRITE), (int)NetAccess::DENIED_TOKEN);
    CHECK_EQ((int)checkNetAccess("", "s3cret", a, "s3cret!", NetOp::WRITE), (int)NetAccess::DENIED_TOKEN);
    CHECK_EQ((int)checkNetAccess("", "s3cret", a, "", NetOp::READ), (int)NetAccess::OK);
    CHECK_EQ((int)checkNetAccess("10.0.0.5", "s3cret", b, "x", NetOp::WRITE), (int)NetAccess::DENIED_TOKEN);
    // net_token の値はログに出さない
    CHECK_EQ(isSecretSettingKey("net_token"), true);
    CHECK_EQ(isSecretSettingKey("NET_TOKEN"), true);
    CHECK_EQ(isSecretSettingKey("pix68k_net_token"), true);
    CHECK_EQ(isSecretSettingKey("net_tokens"), false);
    CHECK_EQ(isSecretSettingKey("net_allow"), false);
    CHECK_EQ(mentionsSecretSetting("  Net_Token secret"), true);
    CHECK_EQ(mentionsSecretSetting("net token"), false);
    CHECK_EQ(mentionsSecretSetting("fd0 HUMAN.XDF"), false);
    CHECK_EQ(mentionsSecretSetting("net_token s3cret"), true); // 「net_token s3cret=」の、= より前 (不明なキー)
    {
        // ログの文言 (ベアメタル版とホスト版が同じ formatSettingWarning() / formatIgnoredLine() で作る) にトークンが入らない。
        // 不明なキー (「net_token s3cret=」は = より前がキーになる)、読めない値 (長すぎる)、= のない行
        ConfigText text;
        char longToken[300];
        memset(longToken, 'z', sizeof longToken - 1);
        longToken[sizeof longToken - 1] = 0;
        char cfg[600];
        snprintf(cfg, sizeof cfg, "net_token s3cret=\nfd0=A.XDF\nNET_TOKEN=%s\nnet_token s3cret\nfoo=bar\nbaz\n",
                 longToken);
        text.load(cfg, strlen(cfg));
        Settings st;
        int lines = 0, hidden = 0;
        bool leaked = false;
        char msg[384];
        auto check = [&]() {
            lines++;
            if (strstr(msg, "s3cret") || strstr(msg, "zzzz")) leaked = true;
            if (strcmp(msg, "ignored a line about net_token") == 0) hidden++;
        };
        readSettings(
            text, st,
            [&](const char* key, const char* value, SettingResult r) {
                formatSettingWarning(msg, sizeof msg, key, value, r);
                check();
            },
            [&](const char* line) {
                formatIgnoredLine(msg, sizeof msg, line);
                check();
            });
        CHECK_EQ(leaked, false);
        CHECK_EQ(hidden, 3);
        CHECK_EQ(lines, 5); // foo=bar (不明なキー) と baz (= がない) はそのまま出す
        formatSettingWarning(msg, sizeof msg, "foo", "bar", SettingResult::UNKNOWN_KEY);
        CHECK_EQ(strcmp(msg, "unknown key: foo"), 0);
        formatSettingWarning(msg, sizeof msg, "cpu", "1", SettingResult::BAD_VALUE);
        CHECK_EQ(strcmp(msg, "cpu=1: bad value; ignored"), 0);
        formatIgnoredLine(msg, sizeof msg, "baz");
        CHECK_EQ(strcmp(msg, "ignored: baz"), 0);
        char small[8];
        formatIgnoredLine(small, sizeof small, "abcdefghijkl"); // 入りきらない分は捨てる
        CHECK_EQ(strcmp(small, "ignored"), 0);
    }
    // トークンの比べ方 (いつも同じ長さを比べる)
    CHECK_EQ(sameToken("abc", "abc"), true);
    CHECK_EQ(sameToken("abc", "abd"), false);
    CHECK_EQ(sameToken("abc", "ab"), false);
    CHECK_EQ(sameToken("ab", "abc"), false);
    CHECK_EQ(sameToken("", ""), true);
    {
        char a[200], b[200];
        memset(a, 'k', 127), a[127] = 0;
        memcpy(b, a, 128);
        CHECK_EQ(sameToken(a, b), true); // 127 文字までは比べられる
        memset(a, 'k', 150), a[150] = 0;
        memcpy(b, a, 151);
        CHECK_EQ(sameToken(a, b), false); // 長すぎるトークンは一致としない
    }
    // ファイルを読むのは、トークンがあればトークンが要る (pix68k.cfg や cmdline.txt にトークンが書いてあるため)
    CHECK_EQ((int)checkNetAccess("", "s3cret", a, "", NetOp::READ_FILES), (int)NetAccess::DENIED_TOKEN);
    CHECK_EQ((int)checkNetAccess("", "s3cret", a, "s3cret", NetOp::READ_FILES), (int)NetAccess::OK);
    CHECK_EQ((int)checkNetAccess("", "", a, "", NetOp::READ_FILES), (int)NetAccess::OK);
    CHECK_EQ((int)checkNetAccess("10.0.0.5", "", a, "", NetOp::READ_FILES), (int)NetAccess::DENIED_SOURCE);
}

static bool feedAll(HttpHeadReader& r, const char* s, size_t& used) {
    return r.feed(s, strlen(s), used) == HttpHeadReader::DONE;
}

static void testHttp() {
    {
        HttpHeadReader r;
        const char* req = "PUT /files/pix68k/a%20b.cfg?x=1 HTTP/1.1\r\nHost: pix68k\r\ncontent-length: 1234\r\n"
                          "Expect: 100-continue\r\nX-Pix68k-Token:  tok \r\n\r\nBODY";
        size_t used = 0;
        CHECK_EQ(feedAll(r, req, used), true);
        CHECK_EQ(used, strlen(req) - 4); // 本体は残る
        HttpRequest q;
        CHECK_EQ(parseHttpRequest(r.text(), q), true);
        CHECK_EQ((int)q.method, (int)HttpRequest::PUT);
        CHECK_EQ(strcmp(q.path, "/files/pix68k/a%20b.cfg"), 0);
        CHECK_EQ(strcmp(q.query, "x=1"), 0);
        CHECK_EQ(q.contentLength, 1234);
        CHECK_EQ(q.expectContinue, true);
        CHECK_EQ(strcmp(q.token, "tok"), 0);
    }
    {
        // 少しずつ届いても、LF だけの改行でも読める
        HttpHeadReader r;
        const char* req = "GET /log HTTP/1.0\nAuthorization: Bearer abc\nContent-Length: 0\n\n";
        size_t used = 0;
        HttpHeadReader::State st = HttpHeadReader::MORE;
        for (size_t i = 0; req[i] && st == HttpHeadReader::MORE; i++) st = r.feed(req + i, 1, used);
        CHECK_EQ((int)st, (int)HttpHeadReader::DONE);
        HttpRequest q;
        CHECK_EQ(parseHttpRequest(r.text(), q), true);
        CHECK_EQ((int)q.method, (int)HttpRequest::GET);
        CHECK_EQ(q.contentLength, 0);
        CHECK_EQ(strcmp(q.token, "abc"), 0);
    }
    {
        HttpRequest q;
        CHECK_EQ(parseHttpRequest("DELETE /x HTTP/1.1\r\n\r\n", q), true);
        CHECK_EQ((int)q.method, (int)HttpRequest::DELETE);
        CHECK_EQ(parseHttpRequest("PATCH /x HTTP/1.1\r\n\r\n", q), true);
        CHECK_EQ((int)q.method, (int)HttpRequest::OTHER);
        CHECK_EQ(parseHttpRequest("GET x HTTP/1.1\r\n\r\n", q), false);
        CHECK_EQ(parseHttpRequest("GET /x\r\n\r\n", q), false);
        CHECK_EQ(parseHttpRequest("GET /x HTTP/1.1\r\nContent-Length: 12a\r\n\r\n", q), false);
        CHECK_EQ(parseHttpRequest("GET /x HTTP/1.1\r\nbroken header\r\n\r\n", q), false);
    }
    {
        // Content-Length と Transfer-Encoding は厳しく見る (400 にする)
        HttpRequest q;
        // 長い値を切り詰めて読まない (前は 159 文字で切って、後ろを見ていなかった)
        char lng[400] = "PUT /x HTTP/1.1\r\nContent-Length: ";
        size_t n = strlen(lng);
        for (int i = 0; i < 200; i++) lng[n++] = '0';
        memcpy(lng + n, "5\r\n\r\n", 6);
        CHECK_EQ(parseHttpRequest(lng, q), false);
        CHECK_EQ(strcmp(q.error, "bad Content-Length\n"), 0);
        CHECK_EQ(parseHttpRequest("PUT /x HTTP/1.1\r\nContent-Length: 99999999999999\r\n\r\n", q), false);
        CHECK_EQ(parseHttpRequest("PUT /x HTTP/1.1\r\nContent-Length: 2000000000000\r\n\r\n", q), false);
        CHECK_EQ(parseHttpRequest("PUT /x HTTP/1.1\r\nContent-Length: 1099511627776\r\n\r\n", q), true);
        CHECK_EQ(parseHttpRequest("PUT /x HTTP/1.1\r\nContent-Length:\r\n\r\n", q), false);
        CHECK_EQ(parseHttpRequest("PUT /x HTTP/1.1\r\nContent-Length: 1, 1\r\n\r\n", q), false);
        CHECK_EQ(parseHttpRequest("PUT /x HTTP/1.1\r\nContent-Length: -1\r\n\r\n", q), false);
        // 2 回あって値が違えば断る。同じなら受ける
        CHECK_EQ(parseHttpRequest("PUT /x HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n", q), false);
        CHECK_EQ(strcmp(q.error, "conflicting Content-Length\n"), 0);
        CHECK_EQ(parseHttpRequest("PUT /x HTTP/1.1\r\nContent-Length: 5\r\ncontent-length:  5 \r\n\r\n", q), true);
        CHECK_EQ(q.contentLength, 5);
        // Transfer-Encoding はどれも断る (chunked のあとに identity が来ても)
        CHECK_EQ(parseHttpRequest("PUT /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", q), false);
        CHECK_EQ(parseHttpRequest("PUT /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: identity\r\n"
                                  "Content-Length: 5\r\n\r\n", q),
                 false);
        CHECK_EQ(parseHttpRequest("PUT /x HTTP/1.1\r\nContent-Length: 5\r\nTransfer-Encoding: gzip\r\n\r\n", q), false);
        CHECK_EQ(parseHttpRequest("PUT /x HTTP/1.1\r\nTransfer-Encoding: identity\r\n\r\n", q), false);
        // ヘッダ名はトークンの文字だけ。コロンの前の空白や続きの行は、知らないヘッダとして見逃さない
        CHECK_EQ(parseHttpRequest("PUT /x HTTP/1.1\r\nTransfer-Encoding : chunked\r\nContent-Length: 5\r\n\r\n", q), false);
        CHECK_EQ(strcmp(q.error, "bad header name\n"), 0);
        CHECK_EQ(parseHttpRequest("PUT /x HTTP/1.1\r\nContent-Length: 5\r\n chunked: x\r\n\r\n", q), false);
        CHECK_EQ(parseHttpRequest("PUT /x HTTP/1.1\r\nX@Y: 1\r\n\r\n", q), false);
        CHECK_EQ(parseHttpRequest("PUT /x HTTP/1.1\r\n: 1\r\n\r\n", q), false);
        CHECK_EQ(parseHttpRequest("PUT /x HTTP/1.1\r\nX-Ok_1.~: 1\r\n\r\n", q), true);
        // 値の制御文字 (HTAB 以外) は断る
        CHECK_EQ(parseHttpRequest("PUT /x HTTP/1.1\r\nX-A: a\x01b\r\n\r\n", q), false);
        CHECK_EQ(strcmp(q.error, "bad header value\n"), 0);
        CHECK_EQ(parseHttpRequest("PUT /x HTTP/1.1\r\nX-A: a\rb\r\n\r\n", q), false);
        CHECK_EQ(parseHttpRequest("PUT /x HTTP/1.1\r\nX-A: a\tb\r\n\r\n", q), true);
        // トークンも切り詰めない
        char tok[400] = "GET /x HTTP/1.1\r\nX-Pix68k-Token: ";
        n = strlen(tok);
        for (int i = 0; i < 200; i++) tok[n++] = 'a';
        memcpy(tok + n, "\r\n\r\n", 5);
        CHECK_EQ(parseHttpRequest(tok, q), false);
        CHECK_EQ(parseHttpRequest("GET /x HTTP/1.1\r\nAuthorization: Bearer   t1 \r\n\r\n", q), true);
        CHECK_EQ(q.hasOrigin, false);
        CHECK_EQ(strcmp(q.token, "t1"), 0);
        // ブラウザのページからの要求 (Origin: がある) は書き込みを断るので、あるかを覚える
        CHECK_EQ(parseHttpRequest("POST /reboot HTTP/1.1\r\nOrigin: http://example.com\r\n\r\n", q), true);
        CHECK_EQ(q.hasOrigin, true);
        CHECK_EQ(parseHttpRequest("POST /reboot HTTP/1.1\r\norigin: null\r\n\r\n", q), true);
        CHECK_EQ(q.hasOrigin, true);
    }
    {
        // NUL などの制御文字は、頭を読み終える前に断る (NUL で文字列が途切れ、後ろのヘッダを見落とさないように)
        HttpHeadReader r;
        static const char req[] = "PUT /x HTTP/1.1\r\nX-A: a\0\r\nTransfer-Encoding: chunked\r\n\r\n";
        size_t used = 0;
        CHECK_EQ((int)r.feed(req, sizeof req - 1, used), (int)HttpHeadReader::BAD);
        HttpHeadReader r2;
        CHECK_EQ((int)r2.feed("GET / HTTP/1.1\r\n\x1b\r\n\r\n", 20, used), (int)HttpHeadReader::BAD);
        HttpHeadReader r3;
        CHECK_EQ((int)r3.feed("GET / HTTP/1.1\r\nA:\tb\r\n\r\n", 24, used), (int)HttpHeadReader::DONE);
    }
    {
        // CR は LF の直前の 1 つだけ受ける。行頭の CR で空行でないもの、CR の連続、単独の CR は断る
        auto feedState = [](const char* req) {
            HttpHeadReader r;
            size_t used = 0;
            return (int)r.feed(req, strlen(req), used);
        };
        CHECK_EQ(feedState("PUT /x HTTP/1.1\r\nContent-Length: 5\r\n\rTransfer-Encoding: chunked\r\n\r\nabcde"),
                 (int)HttpHeadReader::BAD);
        CHECK_EQ(feedState("PUT /x HTTP/1.1\r\nContent-Length: 5\r\r\n\r\n"), (int)HttpHeadReader::BAD);
        CHECK_EQ(feedState("PUT /x HTTP/1.1\r\nX-A: a\rb\r\n\r\n"), (int)HttpHeadReader::BAD);
        CHECK_EQ(feedState("\r\nGET / HTTP/1.1\r\n\r\n"), (int)HttpHeadReader::BAD);
        CHECK_EQ(feedState("GET / HTTP/1.1\r\nA: b\n\r\n"), (int)HttpHeadReader::DONE); // LF と CR LF が混ざってもよい
        CHECK_EQ(feedState("GET / HTTP/1.1\nA: b\r\n\n"), (int)HttpHeadReader::DONE);
        CHECK_EQ(feedState("GET / HTTP/1.1\r\nA: b\r\n"), (int)HttpHeadReader::MORE);
        {
            // 1 バイトずつ届いても同じ (CR のあとで区切られても)
            HttpHeadReader r;
            const char* req = "GET / HTTP/1.1\r\nA: b\r\r\n\r\n";
            size_t used = 0;
            int st = HttpHeadReader::MORE;
            for (size_t i = 0; req[i] && st == HttpHeadReader::MORE; i++) st = r.feed(req + i, 1, used);
            CHECK_EQ(st, (int)HttpHeadReader::BAD);
        }
        // parseHttpRequest() も同じ規則 (HttpHeadReader を通さずに渡されても)
        HttpRequest q;
        CHECK_EQ(parseHttpRequest("PUT /x HTTP/1.1\r\nContent-Length: 5\r\n\rTransfer-Encoding: chunked\r\n\r\n", q), false);
        CHECK_EQ(parseHttpRequest("PUT /x HTTP/1.1\r\nContent-Length: 5\r\r\n\r\n", q), false);
        CHECK_EQ(strcmp(q.error, "bad line ending (CR not followed by LF)\n"), 0);
        CHECK_EQ(parseHttpRequest("PUT /x HTTP/1.1\r\r\n\r\n", q), false);
        CHECK_EQ(parseHttpRequest("PUT /x HTTP/1.1\r\nContent-Length: 5\r\n", q), false); // 空行がない
        CHECK_EQ(parseHttpRequest("PUT /x HTTP/1.1\nContent-Length: 5\r\n\n", q), true);
        CHECK_EQ(q.contentLength, 5);
    }
    {
        // 頭が大きすぎる
        HttpHeadReader r;
        static char big[HttpHeadReader::MAX + 10];
        memset(big, 'a', sizeof big - 1);
        size_t used;
        CHECK_EQ((int)r.feed(big, sizeof big - 1, used), (int)HttpHeadReader::TOO_LARGE);
    }
    char out[64];
    CHECK_EQ(urlDecode("a%20b%2Fc+d", out, sizeof out), true);
    CHECK_EQ(strcmp(out, "a b/c+d"), 0);
    CHECK_EQ(urlDecode("a%00b", out, sizeof out), false);
    CHECK_EQ(urlDecode("a%4", out, sizeof out), false);
    CHECK_EQ(urlDecode("a%zz", out, sizeof out), false);
    CHECK_EQ(urlDecode("abcdef", out, 4), false);

    CHECK_EQ(queryValue("down=4c&mod=05", "mod", out, sizeof out), true);
    CHECK_EQ(strcmp(out, "05"), 0);
    CHECK_EQ(queryValue("down=4c&mod=05", "down", out, sizeof out), true);
    CHECK_EQ(strcmp(out, "4c"), 0);
    CHECK_EQ(queryValue("up", "up", out, sizeof out), true);
    CHECK_EQ(out[0], 0);
    CHECK_EQ(queryValue("upx=1", "up", out, sizeof out), false);
    CHECK_EQ(queryValue("", "up", out, sizeof out), false);

    CHECK_EQ(normalizeFilePath("pix68k//pix68k.cfg/", out, sizeof out), true);
    CHECK_EQ(strcmp(out, "/pix68k/pix68k.cfg"), 0);
    CHECK_EQ(normalizeFilePath("", out, sizeof out), true);
    CHECK_EQ(strcmp(out, "/"), 0);
    CHECK_EQ(normalizeFilePath("/kernel8.img", out, sizeof out), true);
    CHECK_EQ(strcmp(out, "/kernel8.img"), 0);
    CHECK_EQ(normalizeFilePath("pix68k/../config.txt", out, sizeof out), false);
    CHECK_EQ(normalizeFilePath("./config.txt", out, sizeof out), false);
    CHECK_EQ(normalizeFilePath("SD:/x", out, sizeof out), false);
    CHECK_EQ(normalizeFilePath("a\\b", out, sizeof out), false);
    CHECK_EQ(normalizeFilePath("a*b", out, sizeof out), false);
    CHECK_EQ(normalizeFilePath("..x/y", out, sizeof out), true); // 「..」で始まる名前は段ではない
    CHECK_EQ(normalizeFilePath("abcdefghij", out, 8), false);

    char t[32];
    CHECK_EQ(unescapeTyped("A\\nB\\t\\\\\\q", 10, t, sizeof t), 7u);
    CHECK_EQ(strcmp(t, "A\nB\t\\\\q"), 0);
    CHECK_EQ(unescapeTyped("x\\", 2, t, sizeof t), 2u); // 最後の \ はそのまま
    CHECK_EQ(strcmp(httpReason(409), "Conflict"), 0);
}

// 置き換えの手順を、作り物のファイルシステム (path、part、old の 3 つ) で試す
struct FakeFs {
    struct File {
        bool exists = false;
        int content = 0;
        bool dir = false;
    } f[3];
    const char* names[3] = {"p", "p.part", "p.old"};
    int failRenameAt = -1; // この回の rename を失敗させる (0 から数える)
    int renames = 0;
    bool failRemoveOld = false, failExists = false;
    File* find(const char* n) {
        for (int i = 0; i < 3; i++)
            if (!strcmp(names[i], n)) return &f[i];
        return nullptr;
    }
    FsResult exists(const char* n) {
        if (failExists) return FsResult::ERROR;
        return find(n)->exists ? FsResult::OK : FsResult::NO_FILE;
    }
    bool isDirectory(const char* n) {
        File* x = find(n);
        return x->exists && x->dir;
    }
    FsResult remove(const char* n) {
        File* x = find(n);
        if (x->dir) return FsResult::ERROR; // f_unlink は中身のあるディレクトリを消せない
        if (x == &f[2] && failRemoveOld && x->exists) return FsResult::ERROR;
        if (!x->exists) return FsResult::NO_FILE;
        x->exists = false;
        return FsResult::OK;
    }
    FsResult rename(const char* a, const char* b) {
        if (renames++ == failRenameAt) return FsResult::ERROR;
        File *x = find(a), *y = find(b);
        if (!x->exists || y->exists) return FsResult::ERROR;
        *y = *x;
        x->exists = false;
        return FsResult::OK;
    }
};

// Host: が自分のアドレスか net_name でなければ断る (DNS リバインディング対策)
static void testHost() {
    const uint8_t self[4] = {192, 168, 1, 50};
    auto check = [&](const char* req) {
        HttpRequest q;
        if (!parseHttpRequest(req, q)) return -1;
        return (int)checkHost(q, self, "x68000");
    };
    const int OK = (int)HostCheck::OK, MISSING = (int)HostCheck::MISSING, OTHER = (int)HostCheck::OTHER;
    CHECK_EQ(check("GET /log HTTP/1.1\r\nHost: 192.168.1.50\r\n\r\n"), OK); // curl http://192.168.1.50/log
    CHECK_EQ(check("GET /log HTTP/1.1\r\nHost: 192.168.1.50:80\r\n\r\n"), OK);
    CHECK_EQ(check("GET /log HTTP/1.1\r\nHost: x68000\r\n\r\n"), OK);   // curl http://x68000/log
    CHECK_EQ(check("GET /log HTTP/1.1\r\nhost:  X68000:80 \r\n\r\n"), OK);
    CHECK_EQ(check("GET /log HTTP/1.1\r\nHost: x68000.\r\n\r\n"), OK);
    CHECK_EQ(check("GET /log HTTP/1.0\r\n\r\n"), OK); // HTTP/1.0 は Host: がなくてよい
    CHECK_EQ(check("GET /log HTTP/1.1\r\n\r\n"), MISSING);
    CHECK_EQ(check("GET /log HTTP/1.1\r\nHost: evil.example\r\n\r\n"), OTHER);
    CHECK_EQ(check("GET /log HTTP/1.1\r\nHost: x68000.evil.example\r\n\r\n"), OTHER);
    CHECK_EQ(check("GET /log HTTP/1.1\r\nHost: 192.168.1.5\r\n\r\n"), OTHER);
    CHECK_EQ(check("GET /log HTTP/1.1\r\nHost: 192.168.1.500\r\n\r\n"), OTHER);
    CHECK_EQ(check("GET /log HTTP/1.1\r\nHost: 192.168.1.50:\r\n\r\n"), OTHER);
    CHECK_EQ(check("GET /log HTTP/1.1\r\nHost: \r\n\r\n"), OTHER);
    CHECK_EQ(check("GET /log HTTP/1.1\r\nHost: x6800\r\n\r\n"), OTHER);
    CHECK_EQ(check("GET /log HTTP/1.0\r\nHost: evil.example\r\n\r\n"), OTHER); // HTTP/1.0 でも Host: があれば見る
    // Host: が 2 つあれば読まない (400)
    CHECK_EQ(check("GET /log HTTP/1.1\r\nHost: 192.168.1.50\r\nHost: evil.example\r\n\r\n"), -1);
    // 版の数字でなければ読まない
    CHECK_EQ(check("GET /log HTTP/1.x\r\nHost: 192.168.1.50\r\n\r\n"), -1);
    // 1 桁のアドレス
    const uint8_t self2[4] = {10, 0, 0, 5};
    HttpRequest q;
    CHECK_EQ(parseHttpRequest("GET / HTTP/1.1\r\nHost: 10.0.0.5\r\n\r\n", q), true);
    CHECK_EQ((int)checkHost(q, self2, ""), OK);
    CHECK_EQ(parseHttpRequest("GET / HTTP/1.1\r\nHost: pix68k\r\n\r\n", q), true);
    CHECK_EQ((int)checkHost(q, self2, ""), OTHER); // 名前がなければアドレスだけ
}

static void testCommit() {
    for (int scenario = 0; scenario < 8; scenario++) {
        FakeFs fs;
        fs.f[0] = {true, 1};             // 前の内容
        fs.f[1] = {true, 2};             // 受け取った内容
        if (scenario == 1) fs.f[0].exists = false;          // 新しいファイル
        if (scenario == 2) fs.f[2] = {true, 9};             // 前に残った .old
        if (scenario == 3) fs.failRenameAt = 0;             // path → old に失敗
        if (scenario == 4) fs.failRenameAt = 1;             // part → path に失敗 (old を戻す)
        if (scenario == 5) fs.failExists = true;
        if (scenario == 6) fs.failRemoveOld = true, fs.f[2] = {true, 9}; // 残った .old を消せない
        if (scenario == 7) fs.failRemoveOld = true;         // 置き換えたあとの .old を消せない
        const FsResult r = commitUpload(fs, "p", "p.part", "p.old");
        const bool okExpected = scenario == 0 || scenario == 1 || scenario == 2 || scenario == 7;
        CHECK_EQ((int)r, (int)(okExpected ? FsResult::OK : FsResult::ERROR));
        if (okExpected) {
            CHECK_EQ(fs.f[0].exists && fs.f[0].content == 2, true);
            if (scenario != 7) CHECK_EQ(fs.f[2].exists, false);
        } else if (scenario != 5 || true) {
            // 失敗したときは前の内容が path に残る (scenario 5 は何もしない)
            CHECK_EQ(fs.f[0].exists && fs.f[0].content == 1, true);
        }
        CHECK_EQ(fs.f[1].exists, false); // part は残さない
    }

    {
        // 置き換える先がディレクトリなら、何も変えずに断る (ディレクトリを .old に改名しない)
        FakeFs fs;
        fs.f[0] = {true, 1, true};
        fs.f[1] = {true, 2};
        CHECK_EQ((int)commitUpload(fs, "p", "p.part", "p.old"), (int)FsResult::ERROR);
        CHECK_EQ(fs.f[0].exists && fs.f[0].dir && fs.f[0].content == 1, true);
        CHECK_EQ(fs.f[2].exists, false);
        CHECK_EQ(fs.f[1].exists, false);
        CHECK_EQ(fs.renames, 0);
    }

    // 使っているイメージと、その置き換えの名前は断る
    CHECK_EQ(pathTouchesImage("/pix68k/human.hdf", "SD:/pix68k/HUMAN.HDF"), true);
    CHECK_EQ(pathTouchesImage("/pix68k/HUMAN.HDF.new", "SD:/pix68k/HUMAN.HDF"), true);
    CHECK_EQ(pathTouchesImage("/pix68k/HUMAN.HDF.part", "SD:/pix68k/HUMAN.HDF"), true);
    CHECK_EQ(pathTouchesImage("/pix68k/HUMAN.HDF.old", "NVME:/pix68k/HUMAN.HDF"), true);
    CHECK_EQ(pathTouchesImage("/pix68k/HUMAN.HDF2", "SD:/pix68k/HUMAN.HDF"), false);
    CHECK_EQ(pathTouchesImage("/pix68k/pix68k.cfg", "SD:/pix68k/HUMAN.HDF"), false);
    CHECK_EQ(samePathNoCase("/a/B", "/A/b"), true);
    CHECK_EQ(samePathNoCase("/a/B", "/A/b/"), false);
    // 書き方の違い (続いた /、\、"."、".."、名前の後ろの "." と空白、大文字と小文字) は同じパスとみなす
    CHECK_EQ(pathTouchesImage("/pix68k/HUMAN.XDF", "SD:/pix68k//HUMAN.XDF"), true);
    CHECK_EQ(pathTouchesImage("/pix68k/human.xdf", "SD:pix68k\\.\\HUMAN.XDF"), true);
    CHECK_EQ(pathTouchesImage("/pix68k/HUMAN.XDF", "SD:/x/../pix68k/HUMAN.XDF."), true);
    CHECK_EQ(pathTouchesImage("/pix68k/HUMAN.XDF.part", "SD:/PIX68K//human.xdf"), true);
    CHECK_EQ(pathTouchesImage("/pix68k/HUMAN.XDF", "SD:/pix68k/HUMAN.XDF.part"), false); // 1 つだけなら、逆向きは見ない
    char norm[64];
    CHECK_EQ(normalizePath("SD:/a//b/./c/../d. ", norm, sizeof norm, true), true);
    CHECK_EQ(strcmp(norm, "/A/B/D"), 0);
    CHECK_EQ(normalizePath("SD:", norm, sizeof norm, false), true);
    CHECK_EQ(strcmp(norm, "/"), 0);
    CHECK_EQ(normalizePath("/abcdefgh", norm, 8, false), false);
    {
        // 短い名前 (8.3) でも、実際のディレクトリエントリの長い名前に直してから比べる
        struct FakeNames {
            const char* broken = nullptr; // このパスを引くと読めない (ERROR)
            NameLookup longName(const char* path, char* out, size_t size) {
                if (broken != nullptr && samePathNoCase(path, broken)) return NameLookup::ERROR;
                static const char* const kMap[][2] = {
                    {"/PIX68K", "pix68k"},
                    {"/pix68k/HUMAN6~1.XDF", "Human68k.xdf"},
                    {"/pix68k/HUMAN68K.XDF", "Human68k.xdf"},
                    {"/pix68k/DISK~1.PAR", "disk.part"},
                    {"/pix68k/DISK.PART", "disk.part"},
                };
                for (const auto& m : kMap)
                    if (samePathNoCase(path, m[0])) {
                        if (strlen(m[1]) + 1 > size) return NameLookup::ERROR;
                        strcpy(out, m[1]);
                        return NameLookup::FOUND;
                    }
                return NameLookup::MISSING;
            }
        } fs;
        char c[64];
        CHECK_EQ(canonicalPath(fs, "SD:/PIX68K/HUMAN6~1.XDF", c, sizeof c), true);
        CHECK_EQ(strcmp(c, "/PIX68K/HUMAN68K.XDF"), 0);
        CHECK_EQ(canonicalPath(fs, "/nothere/x", c, sizeof c), true);
        CHECK_EQ(strcmp(c, "/NOTHERE/X"), 0);
        const char* const one[] = {"/pix68k/human68k.xdf"};
        CHECK_EQ(touchesImage(fs, one, 1, "SD:/PIX68K/HUMAN6~1.XDF") == ImageCheck::IN_USE, true);
        // 使っているのが /pix68k/disk.part なら、PUT /pix68k/disk は <名前>.part を作り直すので断る
        const char* const put[] = {"/pix68k/disk", "/pix68k/disk.part", "/pix68k/disk.old"};
        CHECK_EQ(touchesImage(fs, put, 3, "SD:/pix68k/DISK~1.PAR") == ImageCheck::IN_USE, true);
        CHECK_EQ(touchesImage(fs, put, 1, "SD:/pix68k/DISK~1.PAR") == ImageCheck::FREE, true); // 名前だけ見ると漏れる
        // 使っているのが /pix68k/disk.old でも同じ (置き換えで消してしまう)
        CHECK_EQ(touchesImage(fs, put, 3, "SD:/pix68k/disk.old") == ImageCheck::IN_USE, true);
        const char* const other[] = {"/pix68k/disk2", "/pix68k/disk2.part", "/pix68k/disk2.old"};
        CHECK_EQ(touchesImage(fs, other, 3, "SD:/pix68k/DISK~1.PAR") == ImageCheck::FREE, true);
        // 書き戻しは、書いたとおりの名前 (短い名前) に .new を付けた HUMAN6~1.XDF.new に書く。長い名前に直してから
        // .new を付けた HUMAN68K.XDF.NEW とだけ比べると漏れる
        const char* const sfnNew[] = {"/pix68k/HUMAN6~1.XDF.new"};
        CHECK_EQ(touchesImage(fs, sfnNew, 1, "SD:/pix68k/HUMAN6~1.XDF") == ImageCheck::IN_USE, true);
        const char* const lfnNew[] = {"/pix68k/Human68k.xdf.new"};
        CHECK_EQ(touchesImage(fs, lfnNew, 1, "SD:/pix68k/HUMAN6~1.XDF") == ImageCheck::IN_USE, true);
        // 引けなかった (読み出しの失敗など) ときは、文字列で比べずに「決められない」にする
        fs.broken = "/pix68k/X.BIN";
        const char* const x[] = {"/pix68k/x.bin", "/pix68k/x.bin.part", "/pix68k/x.bin.old"};
        CHECK_EQ(touchesImage(fs, x, 3, "SD:/pix68k/HUMAN6~1.XDF") == ImageCheck::UNKNOWN, true);
        fs.broken = "/pix68k/HUMAN6~1.XDF"; // 使っているイメージの側を引けない
        CHECK_EQ(touchesImage(fs, other, 3, "SD:/pix68k/HUMAN6~1.XDF") == ImageCheck::UNKNOWN, true);
        fs.broken = nullptr;
    }
    // 設定メニューでつながない一時ファイルの名前
    CHECK_EQ(isReplaceTempName("GAME.XDF.part"), true);
    CHECK_EQ(isReplaceTempName("game.xdf.NEW"), true);
    CHECK_EQ(isReplaceTempName("game.old"), true);
    CHECK_EQ(isReplaceTempName("GAME.XDF"), false);
    CHECK_EQ(isReplaceTempName("old"), false);
    char name[16];
    CHECK_EQ(withSuffix("/a.img", ".part", name, sizeof name), true);
    CHECK_EQ(strcmp(name, "/a.img.part"), 0);
    CHECK_EQ(withSuffix("/abcdefghijk", ".part", name, sizeof name), false);

    char head[32];
    CHECK_EQ(ppmHeader(head, sizeof head, 768, 512), 15);
    CHECK_EQ(strcmp(head, "P6\n768 512\n255\n"), 0);
    const uint32_t px[2] = {0xff123456u, 0x00abcdefu};
    uint8_t rgb[6];
    xrgbToRgb(px, rgb, 2);
    CHECK_EQ(rgb[0] == 0x12 && rgb[1] == 0x34 && rgb[2] == 0x56 && rgb[3] == 0xab && rgb[5] == 0xef, true);
}

static void testType() {
    // 打てる文字はすべて、USB のキーに直して X68000 のキーに戻すと、JIS 配列での打ち方と同じになる
    int typed = 0;
    for (int c = 0x20; c < 0x7f; c++) {
        bool shiftWant;
        const uint8_t want = x68k::Keyboard::fromAscii((char)c, shiftWant);
        uint8_t usage = 0;
        bool shift = false;
        const bool ok = asciiToUsage((char)c, usage, shift);
        CHECK_EQ(ok, want != 0);
        if (!ok) continue;
        typed++;
        CHECK_EQ(x68k::Keyboard::fromHid(usage), want);
        CHECK_EQ(shift, shiftWant);
    }
    CHECK_EQ(typed, 95);
    uint8_t u;
    bool s;
    CHECK_EQ(asciiToUsage('a', u, s), true);
    CHECK_EQ(u, 0x04);
    CHECK_EQ(s, false);
    CHECK_EQ(asciiToUsage('"', u, s), true); // JIS 配列では SHIFT + 2
    CHECK_EQ(u, 0x1f);
    CHECK_EQ(s, true);
    CHECK_EQ(asciiToUsage('_', u, s), true); // JIS の ろ
    CHECK_EQ(u, 0x87);
    CHECK_EQ(asciiToUsage('\n', u, s), true);
    CHECK_EQ(u, 0x28);
    CHECK_EQ(asciiToUsage((char)0x01, u, s), false);
}

static void testMux() {
    const uint8_t pi[6] = {0xb8, 0x27, 0xeb, 1, 2, 3}, x68[6] = {0x02, 0x68, 0x6b, 0, 0, 1};
    uint8_t f[60] = {};
    memcpy(f, pi, 6);
    CHECK_EQ(routeFrame(f, 60, pi, x68), (unsigned)ROUTE_STACK);
    memcpy(f, x68, 6);
    CHECK_EQ(routeFrame(f, 60, pi, x68), (unsigned)ROUTE_BRIDGE);
    memset(f, 0xff, 6);
    CHECK_EQ(routeFrame(f, 60, pi, x68), (unsigned)ROUTE_BOTH);
    const uint8_t mc[6] = {0x01, 0x00, 0x5e, 0, 0, 0xfb};
    memcpy(f, mc, 6);
    CHECK_EQ(routeFrame(f, 60, pi, x68), (unsigned)ROUTE_BOTH);
    const uint8_t other[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    memcpy(f, other, 6);
    CHECK_EQ(routeFrame(f, 60, pi, x68), (unsigned)ROUTE_DROP);
    CHECK_EQ(routeFrame(f, 10, pi, x68), (unsigned)ROUTE_DROP);

    static FrameQueue<2, 64> q;
    uint8_t out[64];
    CHECK_EQ(q.pop(out), 0u);
    CHECK_EQ(q.push("abc", 3), true);
    CHECK_EQ(q.push("defg", 4), true);
    CHECK_EQ(q.push("x", 1), false); // 満杯
    CHECK_EQ(q.dropped(), 1u);
    CHECK_EQ(q.pop(out), 3u);
    CHECK_EQ(memcmp(out, "abc", 3), 0);
    CHECK_EQ(q.push("hi", 2), true); // 一周する
    CHECK_EQ(q.pop(out), 4u);
    CHECK_EQ(memcmp(out, "defg", 4), 0);
    CHECK_EQ(q.pop(out), 2u);
    CHECK_EQ(q.size(), 0u);
    CHECK_EQ(q.push(out, 65), false); // 大きすぎる
}

int main() {
    testHost();
    testMux();
    testConfig();
    testHttp();
    testCommit();
    testType();
    return finish();
}
