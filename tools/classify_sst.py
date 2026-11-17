"""SingleStepTests の失敗を、既知の不一致の規則に当てはめて分類する。

SingleStepTests (CLK で生成) と食い違う箇所では、WinUAE の 68000 の動作か実機の計測値に合わせている。
合わせた結果テストと合わなくなるものと、テストデータの誤り (upstream-issue-4) を、規則ごとに理由を付けて一覧にする。

  python3 -I tools/classify_sst.py <JSON のディレクトリ> <失敗一覧 (ファイル名<TAB>テスト名)> > tests/sst/expected_failures.txt

どの規則にも当てはまらない失敗があれば標準エラーに出し、終了コード 1 を返す。
"""
import gzip
import json
import re
import sys

UPSTREAM_BAD = {
    ("ASL.b.sst", "e502 [ASL.b Q, D2] 1583"),
    ("ASL.b.sst", "e502 [ASL.b Q, D2] 1761"),
}


def rule(fname, t):
    i, f = t["initial"], t["final"]
    name = t["name"]
    if (fname, name) in UPSTREAM_BAD:
        return "upstream-issue-4: テストデータの誤り (SingleStepTests/680x0 issue #4)"
    if fname.startswith("ASR."):
        m = re.search(r"\[ASR\.(.) D(\d), D(\d)\]", name)
        if m:
            bits = {"b": 8, "w": 16, "l": 32}[m.group(1)]
            cnt = i["d" + m.group(2)] & 63
            v = i["d" + m.group(3)] & ((1 << bits) - 1)
            if cnt > bits and v >> (bits - 1):
                return "asr-count-gt-size: ASR でシフト数 > ビット数、負の値のとき C=X=符号 (WinUAE)。テストは C=X=0"
    if fname in ("ADD.l.sst", "SUB.l.sst") and re.search(r"\[(ADD|SUB)\.l Q, A\d\]", name):
        return "addq-l-an-cycles: ADDQ/SUBQ.l #,An は 8 サイクル (WinUAE)。テストは 6"
    if re.search(r"\(d(16|8), PC", name) and i["ssp"] != f["ssp"]:
        # PC 相対オペランドの奇数アドレス: フレームの FC は WinUAE ではプログラム空間、テストはデータ空間
        frame = dict(f["ram"])
        sp = f["ssp"]
        status = (frame.get(sp, 0) << 8) | frame.get(sp + 1, 0)
        if status & 7 in (1, 5):
            return "pcrel-fc: PC 相対オペランドのアドレスエラーの FC はプログラム空間 (WinUAE)。テストはデータ空間"
    if fname in ("DIVU.sst", "DIVS.sst"):
        # 除数 0 またはオーバーフローなら、WinUAE の未定義フラグに合わせたことによる不一致
        vec5 = any(x[0] == "r" and x[3] in (0x14, 0x16) for x in t["transactions"])
        if vec5:
            return "div-zero-flags: ゼロ除算の N/Z (WinUAE: DIVU は上位語で決まる、DIVS は Z=1)"
        dn = int(re.search(r", D(\d)\]", name).group(1))
        if f["d%d" % dn] == i["d%d" % dn] and (f["sr"] & 2):
            return "div-overflow: オーバーフロー時の N=1 Z=0 (WinUAE) / DIVS のオーバーフローのサイクル数 (Cwik の計測値)"
    return None


def main(jsondir, failfile):
    fails = [l.rstrip("\n").split("\t") for l in open(failfile) if "\t" in l]
    byfile = {}
    for fname, name in fails:
        byfile.setdefault(fname, set()).add(name)
    out = ["# SingleStepTests の既知の不一致 (tools/classify_sst.py で生成)", "# ファイル名\tテスト名\t理由"]
    unknown = 0
    for fname in sorted(byfile):
        tests = json.load(gzip.open("%s/%s.json.gz" % (jsondir, fname[:-4])))
        for t in tests:
            if t["name"] not in byfile[fname]:
                continue
            r = rule(fname, t)
            if r is None:
                print("unclassified: %s\t%s" % (fname, t["name"]), file=sys.stderr)
                unknown += 1
            else:
                out.append("%s\t%s\t%s" % (fname, t["name"], r.split(":")[0]))
    print("\n".join(out))
    return 1 if unknown else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
