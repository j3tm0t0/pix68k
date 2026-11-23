// platform/update_rollback.h (更新で残した前の版に戻せるかの判定。#369) のテスト
#include "platform/update_rollback.h"
#include "test_check.h"

#include <string>

using namespace platform;

namespace {
const char* const kPi4[] = {"pi4", nullptr};
const std::string kConfig = "arm_64bit=1\n[pi4]\nkernel=pix68k8-rpi4.img\n";
const std::string kSha(64, 'a');
const std::string kUpdate = "previous=kernel8-rpi4.img\nprevious_version=v0.2.4\nprevious_commit=abc1234\nprevious_sha256=" + kSha +
                            "\ncurrent=pix68k8-rpi4.img\ncurrent_version=v0.3.0\ncurrent_sha256=" + kSha + "\n";

bool plan(const std::string& config, const std::string& update, const char* version, const char* commit, RollbackPlan& p,
          unsigned raspi = 4) {
    return planRollback(config.data(), config.size(), update.data(), update.size(), kPi4, raspi, version, commit, p) == nullptr;
}
std::string replace(std::string s, const std::string& a, const std::string& b) {
    const size_t i = s.find(a);
    CHECK_EQ(i != std::string::npos, true);
    return s.replace(i, a.size(), b);
}
} // namespace

int main() {
    RollbackPlan p;
    CHECK_EQ(plan(kConfig, kUpdate, "v0.3.0", "def5678", p), true);
    CHECK_EQ(strcmp(p.from, "pix68k8-rpi4.img"), 0);
    CHECK_EQ(strcmp(p.to, "kernel8-rpi4.img"), 0);
    CHECK_EQ(strcmp(p.version, "v0.2.4"), 0);
    CHECK_EQ(kSha == p.sha256, true);
    CHECK_EQ(p.running, false);
    // 更新のあと、まだ再起動していない
    CHECK_EQ(plan(kConfig, kUpdate, "v0.2.4", "abc1234", p), true);
    CHECK_EQ(p.running, true);
    // LAN から起動したほかの版、前の版と同じ番号でコミットが違う版
    RollbackPlan q;
    CHECK_EQ(plan(kConfig, kUpdate, "v0.2.5", "abc1234", q), false);
    CHECK_EQ(plan(kConfig, kUpdate, "v0.2.4", "zzz9999", q), false);
    // 戻したあと、ほかの機種、知らない名前
    CHECK_EQ(plan(replace(kConfig, "pix68k8-rpi4.img", "kernel8-rpi4.img"), kUpdate, "v0.3.0", "x", q), false);
    CHECK_EQ(plan(kConfig, kUpdate, "v0.3.0", "x", q, 5), false);
    CHECK_EQ(plan(replace(kConfig, "pix68k8-rpi4.img", "my.img"), kUpdate, "v0.3.0", "x", q), false);
    CHECK_EQ(planRollback(kConfig.data(), kConfig.size(), kUpdate.data(), kUpdate.size(), nullptr, 4, "v0.3.0", "x", q) != nullptr, true);
    // update.txt の誤り
    for (const char* key : {"previous=", "previous_version=", "previous_commit=", "previous_sha256=", "current=", "current_version="}) {
        CHECK_EQ(plan(kConfig, replace(kUpdate, std::string(key), "x" + std::string(key)), "v0.3.0", "x", q), false); // ない
        CHECK_EQ(plan(kConfig, std::string(key) + "v0.3.0\n" + kUpdate, "v0.3.0", "x", q), false);                  // 2 つ
    }
    CHECK_EQ(plan(kConfig, "", "v0.3.0", "x", q), false);
    CHECK_EQ(plan(kConfig, replace(kUpdate, "kernel8-rpi4.img", "../evil.img"), "v0.3.0", "x", q), false);
    CHECK_EQ(plan(kConfig, replace(kUpdate, "previous_sha256=aaaa", "previous_sha256=AAAA"), "v0.3.0", "x", q), false);
    CHECK_EQ(plan(kConfig, replace(kUpdate, "previous_sha256=aaaa", "previous_sha256=aaa"), "v0.3.0", "x", q), false);
    CHECK_EQ(plan(kConfig, kUpdate + std::string(UPDATE_TXT_MAX, '#'), "v0.3.0", "x", q), false);
    for (size_t n = 0; n < kUpdate.size() - 1; n++) // 途中で切れた update.txt でも、戻す先は変わらない
        if (plan(kConfig, kUpdate.substr(0, n), "v0.3.0", "x", q)) CHECK_EQ(strcmp(q.to, "kernel8-rpi4.img"), 0);
    // CRLF
    std::string crlf;
    for (char c : kUpdate) crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
    CHECK_EQ(plan(kConfig, crlf, "v0.3.0", "x", q), true);
    // セクタをまたぐ値
    CHECK_EQ(plan("#" + std::string(494, 'x') + "\n[pi4]\nkernel=pix68k8-rpi4.img\n", kUpdate, "v0.3.0", "x", q), false);
    // 失敗したときは plan を変えない
    CHECK_EQ(strcmp(p.to, "kernel8-rpi4.img"), 0);
    return finish();
}
