package main

import (
	"bytes"
	"regexp"
)

// /log は起動してからのログの最新の 64KB を毎回まるごと返す。続けて読んだ 2 回の本文から、新しく増えた分を取り出す。
//
// リングは頭から捨てていくので、前の本文の終わり (anchor) は、新しい本文の中では前の長さより後ろには来ない。
// その範囲で最後に現れたところの後ろを新しい分とする。見つからなければ (再起動した、または 2 回の間に 64KB 以上
// 増えた)、restarted を true にして新しい本文をまるごと返す。
const anchorLen = 512

func logDelta(prev, cur []byte) (delta []byte, restarted bool) {
	if len(prev) == 0 {
		return cur, false
	}
	a := prev
	if len(a) > anchorLen {
		a = a[len(a)-anchorLen:]
	}
	limit := len(prev)
	if limit > len(cur) {
		limit = len(cur)
	}
	if i := bytes.LastIndex(cur[:limit], a); i >= 0 {
		return cur[i+len(a):], false
	}
	return cur, true
}

// リングが一杯になってからの本文は、頭の 1 行が途中から始まる。一杯 (64KB) なら頭の 1 行を捨てる
const logRingSize = 64 * 1024

func trimPartialHead(b []byte) []byte {
	if len(b) < logRingSize {
		return b
	}
	if i := bytes.IndexByte(b, '\n'); i >= 0 {
		return b[i+1:]
	}
	return b
}

// 最後の n 行
func lastLines(b []byte, n int) []byte {
	if n <= 0 {
		return b
	}
	end := len(b)
	if end > 0 && b[end-1] == '\n' {
		end--
	}
	i := end
	for k := 0; k < n; k++ {
		j := bytes.LastIndexByte(b[:i], '\n')
		if j < 0 {
			return b
		}
		i = j
	}
	return b[i+1:]
}

// 追いかけている間、口は読むたびに「httpd: <アドレス> GET /log: 200 (...)」の 1 行をログに足す。その行は出さない
var reLogPoll = regexp.MustCompile(`^\S+ httpd: \S+ GET /log: 200 `)

func dropLogPolls(b []byte) []byte {
	if !bytes.Contains(b, []byte("GET /log: 200")) {
		return b
	}
	var out []byte
	for len(b) > 0 {
		i := bytes.IndexByte(b, '\n')
		line := b
		if i >= 0 {
			line = b[:i+1]
		}
		b = b[len(line):]
		if !reLogPoll.Match(line) {
			out = append(out, line...)
		}
	}
	return out
}
