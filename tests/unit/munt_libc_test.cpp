// ベアメタル版で Munt が使う C ライブラリの関数 (baremetal/munt_libc.cpp) を、ホストの libc / libm と比べる。
// - 数学の関数の誤差 (sin、cos、exp、log、log10 は倍精度で 1e-14 ほど。fmod、floor、ceil、fabs、sqrt は同じ値)
// - Munt の表 (Tables.cpp) を munt_libc.cpp の関数で作ったものと、ホストの libm で作ったものが同じになること
// - vsnprintf の書式
// munt_libc.cpp は AArch64 の命令 (frintm など) を使うので、AArch64 のホストだけでビルドする (CMakeLists.txt)
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "Tables.h"
#include "test_check.h"

extern "C" {
double pix68k_sin(double);
double pix68k_cos(double);
double pix68k_exp(double);
double pix68k_log(double);
double pix68k_log10(double);
double pix68k_fmod(double, double);
double pix68k_sqrt(double);
double pix68k_floor(double);
double pix68k_ceil(double);
double pix68k_fabs(double);
int pix68k_snprintf(char*, size_t, const char*, ...);
int pix68k_sprintf(char*, const char*, ...);
const void* pix68kShimTables();
}

namespace {

double worst = 0;
const char* worstName = "";

// 誤差が tol (相対。|want| が 1 より小さいときは絶対) を超えたら数える
void near(const char* name, double x, double got, double want, double tol) {
    const double scale = fabs(want) > 1 ? fabs(want) : 1;
    const double err = fabs(got - want) / scale;
    if (err > worst) {
        worst = err;
        worstName = name;
    }
    if (!(err <= tol)) {
        printf("  %s(%.17g): got %.17g, want %.17g\n", name, x, got, want);
        failures++;
    }
}

uint64_t g_seed = 1;
double uniform(double lo, double hi) {
    g_seed = g_seed * 6364136223846793005ull + 1442695040888963407ull;
    return lo + (hi - lo) * (double)(g_seed >> 11) / 9007199254740992.0;
}

void testMath() {
    printf("munt libc: math functions against the host libm\n");
    for (int i = 0; i < 200000; i++) {
        const double x = i < 100000 ? uniform(-40, 40) : uniform(-1e4, 1e4);
        const double tol = i < 100000 ? 4e-15 : 1e-12; // 大きな引数は pi / 2 で引くときの誤差が増える
        near("sin", x, pix68k_sin(x), sin(x), tol);
        near("cos", x, pix68k_cos(x), cos(x), tol);
    }
    for (int i = 0; i < 200000; i++) {
        const double x = uniform(-700, 700);
        near("exp", x, pix68k_exp(x), exp(x), 1e-14);
        const double y = exp(uniform(-690, 690));
        near("log", y, pix68k_log(y), log(y), 1e-14);
        near("log10", y, pix68k_log10(y), log10(y), 1e-14);
        const double z = uniform(0.5, 2.0);
        near("log", z, pix68k_log(z), log(z), 1e-15);
    }
    CHECK_EQ(pix68k_exp(0.0) == 1.0, true);
    CHECK_EQ(pix68k_log(1.0) == 0.0, true);
    CHECK_EQ(pix68k_log(0.0) < -1e308, true);
    CHECK_EQ(pix68k_exp(-800.0) == 0.0, true);
    // 指数の範囲の端 (k = 1024 になるところと、無限大になるところ)
    for (double x = 709.0; x < 709.782; x += 0.001) near("exp", x, pix68k_exp(x), exp(x), 1e-14);
    CHECK_EQ(pix68k_exp(709.9) > 1e308, true);
    CHECK_EQ(pix68k_exp(709.9) == exp(709.9), true); // どちらも無限大
    for (double x = -745.0; x < -700.0; x += 0.01) {
        const double got = pix68k_exp(x), want = exp(x);
        // 非正規化数は精度が落ちるので、差を最小の正規化数で測る
        if (fabs(got - want) > 1e-15 * fabs(want) + 4.9e-324 * 2) {
            printf("  exp(%.17g): got %.17g, want %.17g\n", x, got, want);
            failures++;
        }
    }
    // 同じ値になるもの
    int diff = 0;
    for (int i = 0; i < 200000; i++) {
        const double x = uniform(-1e6, 1e6), y = uniform(0.001, 5000) * (i & 1 ? 1 : -1);
        diff += pix68k_fmod(x, y) != fmod(x, y);
        diff += pix68k_floor(x) != floor(x);
        diff += pix68k_ceil(x) != ceil(x);
        diff += pix68k_fabs(x) != fabs(x);
        diff += pix68k_sqrt(fabs(x)) != sqrt(fabs(x));
        // Munt の使い方 (float の PCM の位置を float の長さで割った余り)
        const float fx = (float)uniform(0, 1e6), fy = (float)(int)uniform(1, 65536);
        diff += (float)pix68k_fmod(fx, fy) != fmodf(fx, fy);
    }
    CHECK_EQ(diff, 0);
    printf("  worst relative error %.3g (%s)\n", worst, worstName);
}

void testTables() {
    printf("munt libc: Munt's tables are the same with munt_libc and with the host libm\n");
    const MT32Emu::Tables& host = MT32Emu::Tables::getInstance();
    const uint8_t* shim = (const uint8_t*)pix68kShimTables();
    const size_t n = offsetof(MT32Emu::Tables, resAmpDecayFactors); // ポインタの前まで (表の配列)
    size_t bad = 0;
    for (size_t i = 0; i < n; i++) bad += ((const uint8_t*)&host)[i] != shim[i];
    CHECK_EQ(bad, 0u);
}

void testFormat() {
    printf("munt libc: vsnprintf\n");
    char buf[64];
    CHECK_EQ(pix68k_sprintf(buf, "Part %d", 3), 6);
    CHECK_EQ(strcmp(buf, "Part 3"), 0);
    pix68k_snprintf(buf, sizeof buf, "%s=%04X %u %d %c %x%%", "pcm", 0x2fu, 7u, -12, 'z', 255u);
    CHECK_EQ(strcmp(buf, "pcm=002F 7 -12 z ff%"), 0);
    pix68k_snprintf(buf, sizeof buf, "[%5d][%3s][%ld][%lld]", 42, "x", -5L, 1LL << 40);
    CHECK_EQ(strcmp(buf, "[   42][  x][-5][1099511627776]"), 0);
    CHECK_EQ(pix68k_snprintf(buf, 5, "abcdefgh"), 8); // 入りきらない分は捨て、書こうとした長さを返す
    CHECK_EQ(strcmp(buf, "abcd"), 0);
}

} // namespace

int main() {
    testMath();
    testTables();
    testFormat();
    return finish();
}
