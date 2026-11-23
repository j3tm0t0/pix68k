// platform/sha256.h のテスト (FIPS 180-4 の例と、Python の hashlib で求めた境目の長さの値)
#include "platform/sha256.h"
#include "test_check.h"

#include <stdio.h>
#include <string.h>

static bool is(const char* data, size_t n, const char* want, size_t step) {
    platform::Sha256 h;
    for (size_t i = 0; i < n; i += step) h.update(data + i, n - i < step ? n - i : step);
    char out[65];
    h.finishHex(out);
    return strcmp(out, want) == 0;
}

int main() {
    const size_t kSteps[] = {1, 3, 63, 64, 65, 4096};
    static char a[1000000];
    memset(a, 'a', sizeof a);
    const char* two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    static const struct { size_t n; const char* want; } kA[] = {
        {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
        {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
        {63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
        {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
        {65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"},
        {119, "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb"},
        {120, "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c"},
        {128, "6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e"},
    };
    for (size_t s : kSteps) {
        CHECK_EQ(is("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", s), true);
        CHECK_EQ(is("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", s), true);
        CHECK_EQ(is(two, strlen(two), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", s), true);
        CHECK_EQ(is(a, sizeof a, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", s), true);
        for (const auto& t : kA) CHECK_EQ(is(a, t.n, t.want, s), true);
    }
    return finish();
}
