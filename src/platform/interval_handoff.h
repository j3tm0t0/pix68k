// 書き手 (1 つのスレッド / コア) が足していく統計を、読み手 (もう 1 つ) が区間ごとに受け取るための受け渡し。
// 区間の切り替えは書き手が行うので、読み手が受け取ってから区切るまでの間に足したものも落とさない。
//
//   書き手: current() に足し、区間の終わりにしてよいところ (フレームの終わりなど) で endSample() を呼ぶ
//   読み手: take() を呼ぶ。初めて呼ぶと区切りを頼んで false を返し、書き手が次の endSample() で区切ったあとに
//           呼ぶと、その区間を返して true。false が返っている間は同じ頼みのまま待つ (新しく頼まない)
//
// 書き手は頼まれたときだけ shared_ を書き、読み手は書き手が書き終えたと分かってから (servedReq_ の acquire)
// shared_ を読み、読み終えてから次を頼む (request_ の release)。同時に触ることはないのでロックは要らない。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。T はコピーできる構造体
#pragma once

namespace platform {

template <typename T>
class IntervalHandoff {
public:
    // ---- 書き手 ----
    T& current() { return cur_; }
    // 頼まれていれば、今の区間を読み手に渡して新しい区間を始める。渡したら true
    bool endSample() {
        const unsigned req = __atomic_load_n(&request_, __ATOMIC_ACQUIRE);
        if (req == served_) return false;
        shared_ = cur_;
        cur_ = T();
        served_ = req;
        __atomic_store_n(&servedReq_, req, __ATOMIC_RELEASE);
        return true;
    }

    // ---- 読み手 ----
    bool take(T& out) {
        if (!waiting_) {
            requested_++;
            __atomic_store_n(&request_, requested_, __ATOMIC_RELEASE);
            waiting_ = true;
        }
        if (__atomic_load_n(&servedReq_, __ATOMIC_ACQUIRE) != requested_) return false;
        out = shared_;
        waiting_ = false;
        return true;
    }

private:
    T cur_ = T();    // 書き手だけが使う
    T shared_ = T(); // 渡す区間
    unsigned served_ = 0;    // 書き手だけが使う: 最後に応えた頼み
    unsigned request_ = 0;   // 読み手が書く: 頼みの番号
    unsigned servedReq_ = 0; // 書き手が書く: shared_ に入れた区間がどの頼みに応えたものか
    unsigned requested_ = 0; // 読み手だけが使う
    bool waiting_ = false;   // 読み手だけが使う
};

} // namespace platform
