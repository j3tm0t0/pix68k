//go:build unix

package main

import (
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
)

// 新しく作るファイルは umask に従う (umask 077 なら 600)
func TestFilesGetNewFileFollowsUmask(t *testing.T) {
	old := syscall.Umask(0o077)
	defer syscall.Umask(old)
	f := newFake(t, ok("net_token=x\n"))
	out := filepath.Join(t.TempDir(), "pix68k.cfg")
	if code, _, errs := f.run("", nil, "files", "get", "/pix68k/pix68k.cfg", out); code != 0 {
		t.Fatal(code, errs)
	}
	st, _ := os.Stat(out)
	if st.Mode().Perm() != 0o600 {
		t.Errorf("権限 %o、600 のはず", st.Mode().Perm())
	}
	syscall.Umask(0o022)
	out2 := filepath.Join(t.TempDir(), "a.cfg")
	f.run("", nil, "files", "get", "/a.cfg", out2)
	if st, _ := os.Stat(out2); st.Mode().Perm() != 0o644 {
		t.Errorf("umask 022: 権限 %o", st.Mode().Perm())
	}
}

// 元のファイルが 0600 なら、一時ファイルも作ったときから 0600 (umask 022 でも 0644 の時がない)
func TestWriteFileAtomicCreatesWithOriginalMode(t *testing.T) {
	old := syscall.Umask(0o022)
	defer syscall.Umask(old)
	dir := t.TempDir()
	out := filepath.Join(dir, "pix68k.cfg")
	os.WriteFile(out, []byte("old\n"), 0o600)
	os.Chmod(out, 0o600)
	checked := false
	afterTempCreate = func(name string) {
		checked = true
		st, err := os.Stat(name)
		if err != nil || st.Mode().Perm() != 0o600 {
			t.Errorf("作った直後の一時ファイルの権限 %v (%v)、600 のはず", st.Mode().Perm(), err)
		}
	}
	defer func() { afterTempCreate = nil }()
	if err := writeFileAtomic(out, strings.NewReader("new\n")); err != nil {
		t.Fatal(err)
	}
	if !checked {
		t.Fatal("一時ファイルを見ていない")
	}
	if st, _ := os.Stat(out); st.Mode().Perm() != 0o600 {
		t.Errorf("置き換えたあとの権限 %o", st.Mode().Perm())
	}
}
