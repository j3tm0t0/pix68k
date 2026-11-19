package main

import (
	"bytes"
	"errors"
	"fmt"
	"image"
	"strconv"
)

// PPM (P6、最大値 255) を画像にする。口は "P6\n<幅> <高さ>\n255\n" の頭を付ける (src/platform/net_files.h の ppmHeader)。
// 一般の PPM のコメント (#) と空白の並びも読む
func decodePPM(b []byte) (*image.RGBA, error) {
	pos := 0
	skipSpace := func() {
		for pos < len(b) {
			c := b[pos]
			if c == '#' {
				for pos < len(b) && b[pos] != '\n' {
					pos++
				}
				continue
			}
			if c == ' ' || c == '\t' || c == '\r' || c == '\n' {
				pos++
				continue
			}
			break
		}
	}
	token := func() (string, error) {
		skipSpace()
		start := pos
		for pos < len(b) && !bytes.ContainsRune([]byte(" \t\r\n#"), rune(b[pos])) {
			pos++
		}
		if start == pos {
			return "", errors.New("PPM の頭が短い")
		}
		return string(b[start:pos]), nil
	}
	magic, err := token()
	if err != nil {
		return nil, err
	}
	if magic != "P6" {
		return nil, fmt.Errorf("PPM (P6) ではない (頭が %q)", magic)
	}
	var v [3]int
	for i := range v {
		s, err := token()
		if err != nil {
			return nil, err
		}
		n, err := strconv.Atoi(s)
		if err != nil || n <= 0 || n > 65535 {
			return nil, fmt.Errorf("PPM の頭の数が読めない (%q)", s)
		}
		v[i] = n
	}
	w, h, maxv := v[0], v[1], v[2]
	if maxv != 255 {
		return nil, fmt.Errorf("PPM の最大値 %d には対応していない (255 だけ)", maxv)
	}
	// 32 ビットの環境 (linux/arm/7) で幅 x 高さ x 4 が int を超えないよう、先に大きさを決めておく
	const maxSide = 8192
	if w > maxSide || h > maxSide {
		return nil, fmt.Errorf("PPM が大きすぎる (%dx%d。%d まで)", w, h, maxSide)
	}
	pos++ // 最大値の後ろの空白 1 つ
	need := w * h * 3
	if pos > len(b) || len(b)-pos < need {
		return nil, fmt.Errorf("PPM の画素が足りない (%d バイトのはずが %d)", need, max(len(b)-pos, 0))
	}
	img := image.NewRGBA(image.Rect(0, 0, w, h))
	px := b[pos : pos+need]
	for i, j := 0, 0; i < need; i, j = i+3, j+4 {
		img.Pix[j] = px[i]
		img.Pix[j+1] = px[i+1]
		img.Pix[j+2] = px[i+2]
		img.Pix[j+3] = 0xff
	}
	return img, nil
}
