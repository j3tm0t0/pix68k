// Munt (external/munt) が使う C ライブラリの関数のうち、Circle にないもの (baremetal/shim_munt の宣言の中身)。
// 数学の関数は Munt が表を作るとき (Synth::open() の中、コア 0) と、標本化周波数の変換の係数を作るときに使う。
// 音を作る間 (コア 3) に呼ぶのは rand() だけ (TVP.cpp)。速さより簡単さを取り、倍精度で 1e-15 ほどの誤差にする。
// ホスト版はホストの libm を使うので、表の値が丸めの境目で違うことがありうる (試した ROM では違わなかった)。
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

namespace {

constexpr double kLn2 = 0.69314718055994530942;
constexpr double kLn10 = 2.30258509299404568402;
// ln 2 を 2 つに分けたもの (上の方は下位のビットが 0 で、整数を掛けても誤差が出ない)
constexpr double kLn2Hi = 6.93147180369123816490e-01;
constexpr double kLn2Lo = 1.90821492927058770002e-10;
// pi / 2 を 2 つに分けたもの (引数を pi / 2 の倍数で引くときの誤差を小さくする)
constexpr double kPiHalfHi = 1.57079632673412561417;
constexpr double kPiHalfLo = 6.07710050650619224932e-11;
constexpr double kTwoOverPi = 0.63661977236758134308;

inline double roundNearest(double x) {
    double r;
    asm("frintn %d0, %d1" : "=w"(r) : "w"(x));
    return r;
}

// 2^i (i は整数)。倍精度の指数の範囲を超えたら 0 か大きな値
double pow2i(int i) {
    if (i < -1022) return 0.0;
    if (i > 1023) return 1e308;
    union {
        double d;
        uint64_t u;
    } v;
    v.u = (uint64_t)(i + 1023) << 52;
    return v.d;
}

// |x| <= pi / 4 の sin と cos (級数)
double sinSmall(double x) {
    const double x2 = x * x;
    double sum = 0.0, term = x;
    for (int k = 1; k <= 25; k += 2) {
        sum += term;
        term *= -x2 / ((k + 1) * (k + 2));
    }
    return sum;
}
double cosSmall(double x) {
    const double x2 = x * x;
    double sum = 0.0, term = 1.0;
    for (int k = 0; k <= 24; k += 2) {
        sum += term;
        term *= -x2 / ((k + 1) * (k + 2));
    }
    return sum;
}

// x = n pi/2 + r (|r| <= pi/4) に分け、n の下位 2 ビットで sin と cos を選ぶ。|x| が 1e9 を超えるものは 0 にする
// (Munt はそこまで大きな値を渡さない)
double sinCos(double x, int shift) {
    if (!(x == x) || x > 1e9 || x < -1e9) return 0.0;
    const double n = roundNearest(x * kTwoOverPi);
    const double r = (x - n * kPiHalfHi) - n * kPiHalfLo;
    const int q = ((int)(long long)n + shift) & 3;
    switch (q) {
    case 0: return sinSmall(r);
    case 1: return cosSmall(r);
    case 2: return -sinSmall(r);
    default: return -cosSmall(r);
    }
}

// 文字列を書く口 (vsnprintf)。入りきらない分は捨て、書こうとした長さを数える
struct Out {
    char* p;
    size_t size;
    size_t n;
    void put(char c) {
        if (n + 1 < size) p[n] = c;
        n++;
    }
};

void putNumber(Out& o, unsigned long long v, unsigned base, bool upper, bool neg, int width, bool zero) {
    char buf[24];
    int len = 0;
    do {
        const unsigned d = (unsigned)(v % base);
        buf[len++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
        v /= base;
    } while (v != 0);
    const int total = len + (neg ? 1 : 0);
    if (!zero)
        for (int i = total; i < width; i++) o.put(' ');
    if (neg) o.put('-');
    if (zero)
        for (int i = total; i < width; i++) o.put('0');
    while (len > 0) o.put(buf[--len]);
}

} // namespace

extern "C" {

double floor(double x) {
    double r;
    asm("frintm %d0, %d1" : "=w"(r) : "w"(x));
    return r;
}

double ceil(double x) {
    double r;
    asm("frintp %d0, %d1" : "=w"(r) : "w"(x));
    return r;
}

double fabs(double x) {
    double r;
    asm("fabs %d0, %d1" : "=w"(r) : "w"(x));
    return r;
}

double sqrt(double x) {
    double r;
    asm("fsqrt %d0, %d1" : "=w"(r) : "w"(x));
    return r;
}

double sin(double x) { return sinCos(x, 0); }
double cos(double x) { return sinCos(x, 1); }

double exp(double x) {
    if (!(x == x)) return x;
    if (x < -746.0) return 0.0;
    if (x > 710.0) return __builtin_inf();
    // x = k ln 2 + r (|r| <= ln 2 / 2)
    const double k = roundNearest(x / kLn2);
    const double r = (x - k * kLn2Hi) - k * kLn2Lo;
    double sum = 1.0, term = 1.0;
    for (int i = 1; i <= 20; i++) {
        term *= r / i;
        sum += term;
    }
    const int ki = (int)k;
    // 指数の範囲の端 (非正規化数の近くと、x が 709.1 を超えて k が 1024 になるとき) は 2 回に分けて掛ける
    // (大きすぎれば無限大になる)
    if (ki < -1000) return sum * pow2i(ki + 500) * pow2i(-500);
    if (ki > 1000) return sum * pow2i(ki - 500) * pow2i(500);
    return sum * pow2i(ki);
}

double log(double x) {
    if (!(x > 0.0)) return x == 0.0 ? -__builtin_inf() : __builtin_nan("");
    union {
        double d;
        uint64_t u;
    } v;
    v.d = x;
    int e = (int)((v.u >> 52) & 0x7ff);
    if (e == 0x7ff) return x; // 無限大
    if (e == 0) {             // 非正規化数
        v.d = x * 4503599627370496.0; // 2^52
        e = (int)((v.u >> 52) & 0x7ff) - 52;
    }
    e -= 1023;
    v.u = (v.u & 0x000fffffffffffffull) | 0x3ff0000000000000ull; // 仮数 1 <= m < 2
    double m = v.d;
    if (m > 1.41421356237309504880) {
        m *= 0.5;
        e++;
    }
    // ln m = 2 (s + s^3/3 + s^5/5 + ...)、s = (m - 1) / (m + 1)、|s| < 0.172
    const double s = (m - 1.0) / (m + 1.0), s2 = s * s;
    double sum = 0.0, p = s;
    for (int k = 1; k <= 41; k += 2) {
        sum += p / k;
        p *= s2;
    }
    return e * kLn2 + 2.0 * sum;
}

double log10(double x) { return log(x) / kLn10; }

double fmod(double x, double y) {
    if (y == 0.0 || !(x == x) || !(y == y)) return 0.0;
    const double ay = fabs(y);
    double r = fabs(x);
    // 大きな商は y を 2 倍ずつ大きくしたもので引く (誤差なしに引ける)
    while (r >= ay) {
        double d = ay;
        while (d * 2.0 <= r) d *= 2.0;
        r -= d;
    }
    return x < 0 ? -r : r;
}

// 線形合同法 (Munt は TVP.cpp で、ピッチの更新の間隔を 0-3 サンプルずらすのに使う)
static uint32_t g_randState = 1;
int rand(void) {
    g_randState = g_randState * 1103515245u + 12345u;
    return (int)((g_randState >> 16) & 0x7fff);
}

int vprintf(const char*, va_list) { return 0; }

int printf(const char*, ...) { return 0; }

// %d %i %u %x %X %c %s %% と、幅 (0 で埋めるものも)、l / ll / h / z を受ける (Munt のパートの名前とデバッグの文)
int vsnprintf(char* out, size_t size, const char* f, va_list args) {
    Out o = {out, size, 0};
    for (; *f; f++) {
        if (*f != '%') {
            o.put(*f);
            continue;
        }
        f++;
        bool zero = false;
        if (*f == '0') {
            zero = true;
            f++;
        }
        int width = 0;
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        int longs = 0;
        while (*f == 'l' || *f == 'h' || *f == 'z') {
            if (*f == 'l') longs++;
            if (*f == 'z') longs = 1;
            f++;
        }
        switch (*f) {
        case 'd':
        case 'i': {
            const long long v = longs >= 2 ? va_arg(args, long long) : longs == 1 ? va_arg(args, long) : va_arg(args, int);
            putNumber(o, v < 0 ? 0ull - (unsigned long long)v : (unsigned long long)v, 10, false, v < 0, width, zero);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            const unsigned long long v = longs >= 2   ? va_arg(args, unsigned long long)
                                         : longs == 1 ? va_arg(args, unsigned long)
                                                      : va_arg(args, unsigned);
            putNumber(o, v, *f == 'u' ? 10 : 16, *f == 'X', false, width, zero);
            break;
        }
        case 'c': o.put((char)va_arg(args, int)); break;
        case 's': {
            const char* s = va_arg(args, const char*);
            if (s == nullptr) s = "(null)";
            int len = 0;
            while (s[len]) len++;
            for (int i = len; i < width; i++) o.put(' ');
            for (int i = 0; i < len; i++) o.put(s[i]);
            break;
        }
        case '%': o.put('%'); break;
        case 0: f--; break; // 書式の終わりで切れた
        default: o.put('%'); o.put(*f); break;
        }
    }
    if (size > 0) out[o.n < size ? o.n : size - 1] = 0;
    return (int)o.n;
}

int snprintf(char* out, size_t size, const char* f, ...) {
    va_list args;
    va_start(args, f);
    const int n = vsnprintf(out, size, f, args);
    va_end(args);
    return n;
}

// Munt はパートの名前 ("Part 1" など、8 バイトの配列) とデバッグの文にだけ使う
int sprintf(char* out, const char* f, ...) {
    va_list args;
    va_start(args, f);
    const int n = vsnprintf(out, 256, f, args);
    va_end(args);
    return n;
}

} // extern "C"
