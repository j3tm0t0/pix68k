// キーのログを出すかどうかの判定 (#360)。
// 前と同じ内容のレポートが届くことがある。
// 押しているキーか修飾キーが前と変わったときだけ true を返す
#pragma once
#include <stdint.h>

namespace platform {

class KeyLogFilter {
public:
    // 1 つのキーボードのレポート。並び順が違っても、キーの組が同じなら同じと見なす。最初は何も押していないものとする
    bool changed(uint8_t modifiers, const uint8_t keys[6]) {
        uint8_t s[6];
        for (int i = 0; i < 6; i++) { // 並べ替えて比べる
            int j = i;
            for (; j > 0 && s[j - 1] > keys[i]; j--) s[j] = s[j - 1];
            s[j] = keys[i];
        }
        bool diff = modifiers != m_Modifiers;
        for (int i = 0; i < 6; i++) diff |= s[i] != m_Keys[i];
        m_Modifiers = modifiers;
        for (int i = 0; i < 6; i++) m_Keys[i] = s[i];
        return diff;
    }
    void reset() { *this = KeyLogFilter(); } // キーボードを外したとき

private:
    uint8_t m_Modifiers = 0;
    uint8_t m_Keys[6] = {};
};

} // namespace platform
