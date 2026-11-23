package main

import (
	"archive/zip"
	"bytes"
	"context"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

// tools/make_release.sh が作るものと同じ
const releaseConfig = `# pix68k の config.txt (Circle の boot/config64.txt をもとにしたもの)
arm_64bit=1
kernel_address=0x80000
initial_turbo=0

[pi2]
arm_64bit=0
kernel_address=0x8000
kernel=kernel7.img

[pi3]
kernel=kernel8.img

[pi3+]
kernel=kernel8.img

[pi4]
kernel=kernel8-rpi4.img
max_framebuffers=2

[pi5]
kernel=kernel_2712.img

[all]
`

func TestFindKernelRelease(t *testing.T) {
	for _, c := range []struct{ raspi, model, want string }{
		{"2", "Raspberry Pi 2 Model B", "kernel7.img"}, {"3", "Raspberry Pi 3 Model B", "kernel8.img"},
		{"3", "Raspberry Pi 3 Model B+", "kernel8.img"}, {"4", "Raspberry Pi 4 Model B", "kernel8-rpi4.img"},
		{"4", "Raspberry Pi 400", "kernel8-rpi4.img"}, {"5", "Raspberry Pi 5", "kernel_2712.img"},
	} {
		s := modelSections(c.raspi, c.model)
		if s == nil {
			t.Errorf("%s: 知らない機種", c.model)
			continue
		}
		for _, cfg := range []string{releaseConfig, strings.ReplaceAll(releaseConfig, "\n", "\r\n")} {
			got, _, _, err := findKernel(cfg, s)
			if err != nil || got != c.want {
				t.Errorf("%s: %q %v", c.model, got, err)
			}
			// 替えても、その値のほかは 1 バイトも変わらない
			out, err := setKernel(cfg, s, "new.img")
			if err != nil {
				t.Errorf("%s: %v", c.model, err)
			} else if n := strings.Count(out, "kernel=new.img"); n != 1 || strings.Count(strings.Replace(out, "new.img", "", 1), "\r") != strings.Count(cfg, "\r") || len(out) != len(cfg)-len(c.want)+len("new.img") {
				t.Errorf("%s: %q", c.model, out)
			} else if got, _, _, _ := findKernel(out, s); got != "new.img" {
				t.Errorf("%s: 替えたあと %q", c.model, got)
			}
		}
	}
	if modelSections("3", "Raspberry Pi Zero 2 W") != nil {
		t.Error("知らない機種")
	}
}

func TestFindKernelCases(t *testing.T) {
	pi400 := modelSections("4", "Raspberry Pi 400")
	for _, c := range []struct{ cfg, want string }{
		{"kernel=a.img", "a.img"},
		{"kernel=a.img\nkernel=b.img\n", "b.img"},                 // 後のものが勝つ
		{"[pi4]\nkernel=a.img\n[pi400]\nkernel=b.img\n", "b.img"}, // 両方当てはまる
		{"[pi400]\nkernel=b.img\n[pi5]\nkernel=c.img\n", "b.img"},
		{"#kernel=x.img\nkernel=a.img\n", "a.img"},
		{"[none]\nkernel=x.img\ninclude y.txt\n[all]\nkernel=a.img\n", "a.img"},
		{"kernel=a.img\n[none]\n[pi4]\nkernel=x.img\n", ""},
		{"kernel=a.img\n[none]\n[pi3]\nkernel=x.img\n", "a.img"},
		{"kernel a.img\n", ""},
		{"[none]\n[all]\n[pi4]\nkernel=b.img\n", "b.img"},
		{"[gpio4=1]\ngpu_mem=64\n[all]\nkernel=a.img\n", "a.img"}, // 関係のない項目
		{"[pi5]\nkernel=x.img\n", ""},
		{"[tryboot]\nkernel=x.img\n[all]\nkernel=a.img\n", ""},
		{"[gpio4=1]\n[pi4]\nkernel=x.img\n", ""}, // 条件が重なる
		{"include x.txt\nkernel=a.img\n", ""},
		{"os_prefix=v1/\nkernel=a.img\n", ""},
		{"kernel = a.img\n", ""},
		{"kernel=sub/a.img\n", ""},
		{"kernel=\n", ""},
		{"[pi4\nkernel=a.img\n", ""},
		{"", ""},
	} {
		got, _, _, err := findKernel(c.cfg, pi400)
		if got != c.want || (err == nil) != (c.want != "") {
			t.Errorf("%q: %q %v", c.cfg, got, err)
		}
	}
	if _, err := setKernel("kernel=a.img\n", pi400, "x\ny"); err == nil {
		t.Error("改行のある名前を受けた")
	}
}

func makeZip(t *testing.T, files map[string][]byte) []byte {
	t.Helper()
	var b bytes.Buffer
	z := zip.NewWriter(&b)
	for n, d := range files {
		w, err := z.Create(n)
		if err != nil {
			t.Fatal(err)
		}
		w.Write(d)
	}
	z.Close()
	return b.Bytes()
}

func fakeRelease(t *testing.T, zipData []byte, sum string) *releaseSource {
	s := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("X-Pix68k-Token") != "" {
			t.Error("配布元に token を送った")
		}
		switch r.URL.Path {
		case "/api/latest":
			io.WriteString(w, `{"tag_name":"v0.3.0"}`)
		case "/dl/v0.3.0/SHA256SUMS":
			io.WriteString(w, strings.Repeat("0", 64)+"  other\n"+sum+"  pix68k-v0.3.0.zip\n")
		case "/dl/v0.3.0/pix68k-v0.3.0.zip":
			w.Write(zipData)
		default:
			http.NotFound(w, r)
		}
	}))
	t.Cleanup(s.Close)
	return &releaseSource{s.URL + "/api", s.URL + "/dl", s.Client()}
}

func TestUpdateCheck(t *testing.T) {
	oldK := []byte("xx version v0.2.4, commit abc\x00 old")
	newK := []byte("xx version v0.3.0, commit def\x00 new")
	z := makeZip(t, map[string][]byte{"kernel8-rpi4.img": newK, "kernel8.img": []byte("other")})
	for _, c := range []struct {
		name, drive, commit, badSum, tag string
		sd                               []byte
		code                             int
		want                             string
	}{
		{"違う", "SD:", "abc", "", "", oldK, 0, "結果: v0.3.0 のカーネルと違う"},
		{"版の指定", "SD:", "abc", "", "v0.3.0", oldK, 0, "結果: v0.3.0 のカーネルと違う"},
		{"同じ", "SD:", "def", "", "", newK, 0, "結果: 配布している版と同じ"},
		{"LAN 起動", "SD:", "zzz", "", "", oldK, 0, "更新できない: 動いているカーネル"},
		{"USB", "USB:", "abc", "", "", oldK, 0, "更新できない: USB"},
		{"NVMe", "NVME:", "abc", "", "", oldK, 0, "更新できない: SD カードのほか"},
		{"なし", "none", "abc", "", "", oldK, exitBusy, "ドライブがない"},
		{"SHA が違う", "SD:", "abc", strings.Repeat("1", 64), "", oldK, exitError, "SHA256SUMS と違う"},
		{"ない版", "SD:", "abc", "", "v9.9.9", oldK, exitNotFound, "404"},
		{"config.txt がない", "SD:", "noconfig", "", "", oldK, 0, "更新できない: ドライブに config.txt がない"},
		{"カーネルがない", "SD:", "nokernel", "", "", oldK, 0, "kernel8-rpi4.img がない"},
		{"config.txt を判断できない", "SD:", "badconfig", "", "", oldK, 0, "更新できない: config.txt: include"},
		{"SHA-256 の不一致", "SD:", "badsum", "", "", oldK, exitError, "読んだファイルの SHA-256"},
		{"悪い版", "SD:", "abc", "", "../x", oldK, exitUsage, "正しくない"},
	} {
		t.Run(c.name, func(t *testing.T) {
			f := newFake(t, func(w http.ResponseWriter, r *http.Request, _ []byte) {
				switch r.URL.Path {
				case "/version":
					ver := "v0.2.4"
					if c.commit == "def" {
						ver = "v0.3.0"
					}
					io.WriteString(w, "version="+ver+"\ncommit="+c.commit+"\nraspi=4\nmodel=Raspberry Pi 400\ndrive="+c.drive+"\n")
				case "/files/config.txt":
					if c.commit == "noconfig" {
						http.NotFound(w, r)
						return
					}
					if c.commit == "badconfig" {
						io.WriteString(w, "include x.txt\n")
						return
					}
					io.WriteString(w, releaseConfig)
				case "/files/kernel8-rpi4.img":
					w.Write(c.sd)
				case "/sha256/kernel8-rpi4.img":
					if c.commit == "nokernel" {
						http.NotFound(w, r)
						return
					}
					if c.commit == "badsum" {
						io.WriteString(w, "sha256="+strings.Repeat("2", 64)+"\n")
						return
					}
					io.WriteString(w, "sha256="+sha256Hex(c.sd)+"\n")
				default:
					http.NotFound(w, r)
				}
			})
			sum := sha256Hex(z)
			if c.badSum != "" {
				sum = c.badSum
			}
			var out, errb bytes.Buffer
			env := map[string]string{"PIX68K_TOKEN": testToken, "XDG_CONFIG_HOME": t.TempDir()}
			a := &app{stdout: &out, stderr: &errb, stdin: strings.NewReader(""), getenv: func(k string) string { return env[k] },
				releases: fakeRelease(t, z, sum)}
			args := []string{"--host", f.host(), "update", "--check"}
			if c.tag != "" {
				args = append(args, "--tag", c.tag)
			}
			code := a.run(context.Background(), args)
			if code != c.code || !strings.Contains(out.String()+errb.String(), c.want) {
				t.Errorf("code %d\n%s%s", code, out.String(), errb.String())
			}
			for _, r := range f.reqs {
				if r.method != "GET" {
					t.Errorf("書き込みを送った: %s %s", r.method, r.uri)
				}
			}
		})
	}
}

func TestUpdateOldKernelAndUsage(t *testing.T) {
	f := newFake(t, func(w http.ResponseWriter, r *http.Request, _ []byte) { http.NotFound(w, r) })
	if code, _, _ := f.run("", nil, "update", "--check"); code != exitUnsupported {
		t.Errorf("code %d", code)
	}
	if code, _, _ := f.run("", nil, "update", "x"); code != exitUsage {
		t.Errorf("code %d", code)
	}
}

func TestSumFor(t *testing.T) {
	h := strings.Repeat("a", 64)
	if s, err := sumFor([]byte(h+" *x.zip\n"), "x.zip"); err != nil || s != h {
		t.Error(s, err)
	}
	for _, b := range []string{"", "zz  x.zip\n", h + "  x.zip\n" + h + "  x.zip\n", h + "  y/x.zip\n"} {
		if _, err := sumFor([]byte(b), "x.zip"); err == nil {
			t.Errorf("%q を受けた", b)
		}
	}
}

func TestReleaseErrors(t *testing.T) {
	for _, st := range []int{403, 429, 404, 409, 500, 503} {
		s := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(st) }))
		src := &releaseSource{s.URL + "/api", s.URL + "/dl", s.Client()}
		_, err := src.resolve(context.Background(), "")
		want := exitError
		if st == 404 {
			want = exitNotFound
		}
		limited := st == 403 || st == 429
		if err == nil || exitCode(err) != want || strings.Contains(err.Error(), "--tag") != limited {
			t.Errorf("%d: %v", st, err)
		}
		_, err = src.kernel(context.Background(), "v0.3.0", "kernel8.img")
		if err == nil || exitCode(err) != want || strings.Contains(err.Error(), "--tag") {
			t.Errorf("%d: %v", st, err)
		}
		s.Close()
		if _, err := src.resolve(context.Background(), ""); exitCode(err) != exitConnect {
			t.Errorf("つながらない: %v", err)
		}
	}
	// https でない転送先
	plain := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { t.Error("http に転送した") }))
	defer plain.Close()
	tls := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/ok" {
			io.WriteString(w, "x")
			return
		}
		to := plain.URL
		if r.URL.Path == "/tls" {
			to = "/ok"
		}
		http.Redirect(w, r, to, http.StatusFound)
	}))
	defer tls.Close()
	hc := tls.Client()
	hc.CheckRedirect = httpsOnly
	src := &releaseSource{tls.URL, tls.URL, hc}
	if _, err := src.get(context.Background(), tls.URL+"/plain", 100); err == nil {
		t.Error("http への転送を受けた")
	}
	if b, err := src.get(context.Background(), tls.URL+"/tls", 100); err != nil || string(b) != "x" {
		t.Error(string(b), err)
	}
}

func TestModel(t *testing.T) {
	for _, m := range []string{"Raspberry Pi 500", "Compute Module 5"} {
		if modelSections("5", m) != nil {
			t.Error(m)
		}
	}
}

func TestKernelZip(t *testing.T) {
	for _, files := range []map[string][]byte{{"other": []byte("x")}, {"kernel8.img": {}}} {
		z := makeZip(t, files)
		if _, err := fakeRelease(t, z, sha256Hex(z)).kernel(context.Background(), "v0.3.0", "kernel8.img"); err == nil {
			t.Error(files)
		}
	}
}
