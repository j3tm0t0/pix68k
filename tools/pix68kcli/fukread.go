package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"errors"
	"flag"
	"fmt"
	"io/fs"
	"net/url"
	"os"
	"time"
)

// XDF (2HD) の大きさ
const xdfBytes = 1261568

// X68000 がフロッピーディスクを使っている途中だと断られたとき、頼み直す回数と、そのときの message (FukReader::describe())
const (
	fukBusyRetries = 5
	fukBusyMessage = "the X68000 is using the floppy drive"
)

var errFukBusy error = &cliError{exitError, "読み出せなかった: " + fukBusyMessage}

// 進み具合を見る間隔と、Ctrl+C のあと中止が終わるのを待つ上限 (試験で短くする)
var (
	fukPoll       = time.Second
	fukCancelWait = 20 * time.Second
)

// FUK のディスクを XDF に読み出す: POST /fuk/read → GET /fuk/status を見る → GET /fuk.xdf。
// Ctrl+C なら POST /fuk/cancel を送る
func (a *app) cmdFukRead(ctx context.Context, c *Client, args []string) error {
	fl := flag.NewFlagSet("fuk-read", flag.ContinueOnError)
	drive := fl.Int("drive", 0, "")
	if err := parseFlags(fl, args); err != nil {
		return err
	}
	if fl.NArg() != 1 || *drive < 0 || *drive > 3 {
		return usageErr("fuk-read: fuk-read [--drive 0-3] ファイル")
	}
	out := fl.Arg(0)
	if _, err := os.Lstat(out); err == nil {
		return &cliError{exitError, out + " がもうある (上書きしない)"}
	} else if !errors.Is(err, fs.ErrNotExist) {
		return err
	}
	if has, err := c.hasEndpoint(ctx, "/fuk/read"); err != nil {
		return err
	} else if !has {
		return &cliError{exitUnsupported, "動いているカーネルに POST /fuk/read がない。新しいカーネルに入れ替える"}
	}
	fmt.Fprintln(a.stderr, "読んでいる間、X68000 は止まる (Ctrl+C でやめる)")
	var id, sum string
	for try := 0; ; try++ {
		var err error
		id, sum, err = a.startFuk(ctx, c, *drive)
		if err == nil {
			break
		}
		if !errors.Is(err, errFukBusy) || try >= fukBusyRetries {
			return err
		}
		// X68000 がディスクを使い終わるのを待って、頼み直す
		fmt.Fprintln(a.stderr, "X68000 がフロッピーディスクを使っている。頼み直す")
		select {
		case <-ctx.Done():
			return &cliError{exitError, "中止した"}
		case <-time.After(fukPoll):
		}
	}
	b, err := c.fetch(ctx, reqSpec{method: "GET", escapedPath: "/fuk.xdf", query: url.Values{"id": {id}}, auth: true})
	if err != nil {
		return err
	}
	if len(b) != xdfBytes {
		return &cliError{exitError, fmt.Sprintf("イメージが %d バイト (%d のはず)", len(b), xdfBytes)}
	}
	if got := fmt.Sprintf("%x", sha256.Sum256(b)); got != sum {
		return &cliError{exitError, "イメージの SHA-256 が pix68k の値と違う (" + got + "、" + sum + ")"}
	}
	if err := writeNewFile(out, b); err != nil {
		return err
	}
	fmt.Fprintf(a.stderr, "%s (%d バイト)\n", out, len(b))
	return nil
}

// 読み出しを頼み、終わるのを待つ。読み出しの番号と、イメージの SHA-256 を返す
func (a *app) startFuk(ctx context.Context, c *Client, drive int) (id, sum string, err error) {
	// 返事を受け取る前に Ctrl+C が来ても、始まった読み出しの番号を知ってからやめる
	q := url.Values{"drive": {fmt.Sprint(drive)}}
	b, err := c.fetch(context.WithoutCancel(ctx), reqSpec{method: "POST", escapedPath: "/fuk/read", query: q, auth: true})
	if err != nil {
		return "", "", err
	}
	if id = parseKV(b)["id"]; id == "" {
		return "", "", &cliError{exitError, "POST /fuk/read の返事に id がない"}
	}
	sum, err = a.waitFuk(ctx, c, id)
	return id, sum, err
}

// ファイルがないときだけ作る。書けなければ消す
func writeNewFile(name string, b []byte) error {
	f, err := os.OpenFile(name, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o666)
	if err != nil {
		return err
	}
	_, err = bytes.NewReader(b).WriteTo(f)
	if err == nil {
		err = f.Sync()
	}
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		os.Remove(name)
	}
	return err
}

// 読み終えるのを待ち、イメージの SHA-256 を返す
func (a *app) waitFuk(ctx context.Context, c *Client, id string) (string, error) {
	last := ""
	fails := 0
	for {
		b, err := c.fetch(ctx, reqSpec{method: "GET", escapedPath: "/fuk/status"})
		if ctx.Err() != nil {
			return "", a.cancelFuk(c, id)
		}
		if err != nil {
			// 一度つながらなかっただけでは、やめない
			if fails++; fails >= 10 {
				return "", err
			}
		} else {
			fails = 0
			m := parseKV(b)
			if m["id"] != id {
				return "", &cliError{exitError, "別の読み出しが始まったか、pix68k が再起動した"}
			}
			if line := fmt.Sprintf("%s/%s トラック、読み直し %s 回", m["tracks"], m["total"], m["retries"]); line != last {
				fmt.Fprintln(a.stderr, line)
				last = line
			}
			switch m["state"] {
			case "done":
				if m["message"] != "done" { // ヘッドを戻せなかったなど
					fmt.Fprintln(a.stderr, m["message"])
				}
				return m["sha256"], nil
			case "reading":
			default:
				if m["message"] == fukBusyMessage {
					return "", errFukBusy
				}
				return "", &cliError{exitError, "読み出せなかった: " + m["message"]}
			}
		}
		select {
		case <-ctx.Done():
			return "", a.cancelFuk(c, id)
		case <-time.After(fukPoll):
		}
	}
}

// 中止を頼み、終わるのを待つ (X68000 が動き出すまで)
func (a *app) cancelFuk(c *Client, id string) error {
	ctx, stop := context.WithTimeout(context.Background(), fukCancelWait)
	defer stop()
	if _, err := c.fetch(ctx, reqSpec{method: "POST", escapedPath: "/fuk/cancel", query: url.Values{"id": {id}}, auth: true}); err != nil {
		return fmt.Errorf("中止を頼めなかった: %w", err)
	}
	for {
		b, err := c.fetch(ctx, reqSpec{method: "GET", escapedPath: "/fuk/status"})
		if m := parseKV(b); err == nil && (m["id"] != id || m["state"] != "reading") {
			return &cliError{exitError, "中止した"}
		}
		select {
		case <-ctx.Done():
			return &cliError{exitError, "中止を頼んだが、終わったことを確かめられない"}
		case <-time.After(fukPoll):
		}
	}
}
