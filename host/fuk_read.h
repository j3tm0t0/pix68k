// FUK につないだ FDD のディスクを、XDF のイメージのファイルに読み出す (--fuk-read。#381)。X68000 は動かさない
#pragma once

// fukSpec は fuk= の値。drive は 0-3。path に読み出す (あるファイルは上書きしない。失敗したら何も残さない)。
// 進み具合と結果を stderr に出し、終了コード (0 成功、1 失敗、2 指定の誤り) を返す。Ctrl+C でやめる
int readFukDisk(const char* fukSpec, int drive, const char* path);
