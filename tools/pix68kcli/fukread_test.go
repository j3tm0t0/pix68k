package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestFukRead(t *testing.T) {
	defer func(p time.Duration) { fukPoll = p }(fukPoll)
	fukPoll = time.Millisecond
	image := bytes.Repeat([]byte{1, 2, 3, 4}, xdfBytes/4)
	end, id := "done", "77"
	sum := fmt.Sprintf("%x", sha256.Sum256(image))
	n := 0
	cancelled := false
	reads, busy := 0, 0
	var stop context.CancelFunc
	help := helpNew + "POST /fuk/read?drive=N\n"
	f := newFake(t, func(w http.ResponseWriter, r *http.Request, _ []byte) {
		switch r.URL.Path {
		case "/":
			io.WriteString(w, help)
		case "/fuk/read":
			n = 0
			reads++
			w.WriteHeader(202)
			io.WriteString(w, "id=77\nreading\n")
		case "/fuk/cancel":
			cancelled = true
		case "/fuk/status":
			n++
			s := "reading"
			if end != "reading" && n > 3 || cancelled {
				s = end
			}
			if cancelled {
				s = "failed"
			}
			if stop != nil && n == 2 {
				stop()
			}
			if busy > 0 {
				busy--
				io.WriteString(w, "state=failed\nid="+id+"\nmessage=the X68000 is using the floppy drive\n")
				return
			}
			io.WriteString(w, "state="+s+"\nid="+id+"\ntracks=3\ntotal=154\nretries=0\nmessage=a sector cannot be read at cylinder 1\n")
			if s == "done" {
				io.WriteString(w, "sha256="+sum+"\n")
			}
		case "/fuk.xdf":
			w.Write(image)
		}
	})
	out := filepath.Join(t.TempDir(), "a.xdf")
	if code, _, errs := f.run("", nil, "fuk-read", "--drive", "1", out); code != 0 {
		t.Fatal(code, errs)
	}
	got, _ := os.ReadFile(out)
	if !bytes.Equal(got, image) {
		t.Fatal("イメージが違う")
	}
	for _, r := range f.reqs {
		want := strings.HasPrefix(r.uri, "/fuk/read") || r.uri == "/fuk.xdf?id=77"
		if (r.token == testToken) != want {
			t.Errorf("%s の token %q", r.uri, r.token)
		}
	}
	if f.reqs[1].method != "POST" || f.reqs[1].uri != "/fuk/read?drive=1" {
		t.Errorf("%+v", f.reqs[1])
	}
	// あるファイルは上書きせず、要求も送らない
	f.reqs = nil
	if code, _, _ := f.run("", nil, "fuk-read", out); code != exitError || len(f.reqs) != 0 {
		t.Errorf("上書き: %d %d", code, len(f.reqs))
	}
	// 失敗したら、イメージを取らず、ファイルも作らない
	end = "failed"
	out2 := filepath.Join(t.TempDir(), "b.xdf")
	code, _, errs := f.run("", nil, "fuk-read", out2)
	if _, err := os.Stat(out2); code != exitError || !strings.Contains(errs, "cylinder 1") || err == nil {
		t.Errorf("失敗: %d %s", code, errs)
	}
	for _, r := range f.reqs {
		if strings.HasPrefix(r.uri, "/fuk.xdf") {
			t.Error("失敗したのにイメージを取った")
		}
	}
	// ほかの人が別の読み出しを始めた
	end, id = "done", "78"
	f.reqs = nil
	if code, _, _ := f.run("", nil, "fuk-read", out2); code != exitError {
		t.Error("別の読み出しを受け入れた")
	}
	for _, r := range f.reqs {
		if strings.HasPrefix(r.uri, "/fuk.xdf") {
			t.Error("別の読み出しのイメージを取った")
		}
	}
	id = "77"
	// X68000 が使っている途中なら、頼み直す。終わらなければ失敗する
	out3 := filepath.Join(t.TempDir(), "c.xdf")
	reads, busy = 0, 2
	if code, _, errs := f.run("", nil, "fuk-read", out3); code != 0 || reads != 3 {
		t.Errorf("頼み直し: %d %d %s", code, reads, errs)
	}
	reads, busy = 0, 100
	code, _, errs = f.run("", nil, "fuk-read", out2)
	if _, err := os.Stat(out2); code != exitError || reads != fukBusyRetries+1 || !strings.Contains(errs, "using the floppy") || err == nil {
		t.Errorf("頼み直しの上限: %d %d %s", code, reads, errs)
	}
	busy = 0
	// 中身の違うイメージ、SHA-256 のない返事
	good := sum
	for _, bad := range []string{strings.Repeat("0", 64), ""} {
		sum = bad
		code, _, errs := f.run("", nil, "fuk-read", out2)
		if _, err := os.Stat(out2); code != exitError || !strings.Contains(errs, "SHA-256") || err == nil {
			t.Errorf("照合: %d %s", code, errs)
		}
	}
	sum = good
	// 短いイメージ
	end, image = "done", image[:100]
	if code, _, _ := f.run("", nil, "fuk-read", out2); code != exitError {
		t.Error("短いイメージを受け入れた")
	}
	// Ctrl+C
	end = "reading"
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	stop = cancel
	code, _, errs = f.runCtx(ctx, "", nil, "fuk-read", out2)
	if code != exitError || !cancelled || !strings.Contains(errs, "中止した") {
		t.Errorf("中止: %d %v %s", code, cancelled, errs)
	}
	// 使い方と、口のないカーネル
	if code, _, _ := f.run("", nil, "fuk-read", "--drive", "4", out2); code != exitUsage {
		t.Error("drive 4")
	}
	help = helpNew
	if code, _, _ := f.run("", nil, "fuk-read", out2); code != exitUnsupported {
		t.Error("口のないカーネル")
	}
}
