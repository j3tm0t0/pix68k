# リリースの説明

公開リポジトリ (j3tm0t0/pix68k) の Release の説明の本文を、版ごとに `<タグ>.md` (例: `v0.2.0.md`) に書く。

- タグを push すると、`.github/workflows/release.yml` がこのファイルを読み、ソースの場所と SHA256SUMS の文を後ろに足して Release の説明にする。
- タグを付けるコミットにこのファイルがない (空か、空白だけのときも同じ) と、workflow は zip のビルドの前に、Release を作らずに失敗する。公開リポジトリは immutable releases なので、
  公開したあとは添付のファイルを変えられない。説明はタグを付ける前に書いてコミットしておく。
- 添付のファイル (zip、pix68kcli の 7 つ、SHA256SUMS) の一覧と SHA-256 は workflow が作るので、ここには書かない。
