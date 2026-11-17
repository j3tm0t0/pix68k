#!/usr/bin/env python3
"""無線 LAN の SSID とパスワード、TCP のトークンを、ビルド先に置くヘッダに変える。

使い方: gen_secrets.py <wifi のファイル> <トークンのファイル> <出力するヘッダ>
  wifi のファイル: 1 行目が SSID、2 行目がパスワード (なければ暗号なし)
  トークンのファイル: 1 行目がトークン (空白を含まない ASCII で 8 文字以上)
中身は表示しない。出力したヘッダは、リポジトリにもログにも残さないこと。
"""
import sys


def first_lines(path, count):
    with open(path, "rb") as f:
        lines = f.read().split(b"\n")
    lines = [line.rstrip(b"\r") for line in lines]
    return (lines + [b""] * count)[:count]


def c_array(name, data):
    body = ", ".join(str(b) for b in data + b"\0")
    return "static const char %s[] = {%s};\n" % (name, body)


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: gen_secrets.py <wifi> <token> <out.h>")
    ssid, password = first_lines(sys.argv[1], 2)
    (token,) = first_lines(sys.argv[2], 1)
    # エラーの文には中身を入れない
    if not 1 <= len(ssid) <= 32 or b"\0" in ssid:
        sys.exit("gen_secrets.py: SSID は 1 から 32 バイト")
    if len(password) > 63 or b"\0" in password:
        sys.exit("gen_secrets.py: パスワードは 63 バイトまで")
    if len(token) < 8 or len(token) > 64 or any(b <= 0x20 or b >= 0x7F for b in token):
        sys.exit("gen_secrets.py: トークンは空白を含まない ASCII で 8 から 64 文字")
    with open(sys.argv[3], "w") as f:
        f.write("// gen_secrets.py が作った。コミットしないこと\n")
        f.write(c_array("kWifiSsid", ssid))
        f.write(c_array("kWifiPassword", password))
        f.write(c_array("kNetToken", token))


if __name__ == "__main__":
    main()
