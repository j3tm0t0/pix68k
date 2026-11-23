// FUK (TNB製作所の「ふろっぴU君」。USB で FDC と FDD をエミュレータにつなぐ装置) を、内蔵の FDC の代わりに使う (#295)。
//
// FUK は USB のシリアルポートを 2 本持つ。ポート 1 でレジスタを読み書きし、ポート 2 で割り込みの通知と
// Read / Write のデータをやり取りする (docs/fuk.md に形式をまとめた)。
//   ポート 1 (こちら → FUK): 0x80|reg, 値 = 書く / reg = 読む (1 バイト返る) / 0x40 = リセット
//   ポート 2 (FUK → こちら): 'C' 'c' = FDC の割り込みが立つ / 下りる、'D' 'd' = FDD の割り込み、
//                            'R' 長さ (2 バイト、下位が先) データ リザルト (7 バイト) = Read の結果、
//                            'W' = 書くデータの要求、'w' = 書き込みの終わり、't' = 中止
//   ポート 2 (こちら → FUK): 'M' データ = 書くデータ
//
// ポート 1 の読み出しは、返事が届くまで USB の往復で 1ms ほどかかる。X68000 を動かすスレッド (コア) を
// その間止めないよう、X68000 が読む値を先に読んでおく:
//   - リザルト ($E94003): コマンドを送ったあと (READ ID と Write は、用意ができたと分かったあと) に、こちらから
//     読み出しを順に送る。そろうまで X68000 にはステータス 0x10 (ビジー、RQM なし) を返し、そろったら 0xD0 にする。
//     ソフトウェアは RQM を待ってから読むので、応答の遅い FDC に見えるだけ
//   - ステータス ($E94001): コマンドの途中や転送の途中は、状態から作る。コマンドを待っているときは、最後に
//     読んだ値を返す。古くなっていたら (MSR_FRESH_US)、返しつつ問い合わせを 1 つ送って更新する。
//     コマンドを送った直後など、最後に読んだ値が使えない間は 0x10 を返す
//   - ドライブの状態 ($E94005): ドライブ制御を書いたときに読み出しも送る。値は選んだドライブの組ごとに覚え、
//     古くなっていたら (DRIVE_FRESH_US)、返しつつ読み直す。ディスクを出し入れした ('D') あとは、覚えた値を捨てる
// X68000 を待たせるのは、先に読んでいない値を読んだとき (WAIT_US で打ち切る) と、書き込みがたまったとき、返事を
// 待っている間にステータスを読んだとき (yieldForReply()) だけ。
//
// Read / Write のデータは、内蔵の FDC と同じく DMAC のチャネル 0 が $E94003 を読み書きして運ぶ
// (dataReady() / readData() / terminalCount())。DMA の Read は、'R' のデータが届いた分から渡す (DMAC の転送バイト数は
// 届くのに合わせて減る)。DMA を使わない Read は、全部届いてから渡す。
// DMA を使わない Read (CPU が $E94003 を 1 バイトずつ読む) はできるが、DMA を使わない Write は受け付けない
// (書くバイト数を先に FUK に知らせる決まりで、その数を DMAC から取るため。こちらで異常終了にする)。
//
// FUK がつながっていない間 (抜いた、見分けに失敗した、時間切れのあとつなぎ直している間) は、ドライブが 1 台も
// レディにならない FDC として答える (コマンドは異常終了、ノットレディ)。X68000 は止まらない。
//
// 時間はすべて実時間 (FukLink::nowUs()) で数える。相手が実物の装置で、X68000 の速さ (max_speed など) とは関係なく動くため。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない。
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace x68k {

// FUK の 2 本のポート。どちらがポート 1 かは Fuk が見分けるので、ここでは 0 と 1 で呼ぶ。
// X68000 を動かすスレッド (コア) から呼ぶ。waitRx() のほかは待たずに戻ること
class FukLink {
public:
    virtual ~FukLink() {}
    // 装置がつながっている
    virtual bool online() = 0;
    // 装置をつなぎ直すたびに変わる番号 (抜いてすぐ挿したことが分かるように)。同じ装置との通信が一度切れて
    // 戻っただけ (USB の転送のエラーなど) なら変えないこと。変えなければ、send() で受け付けたバイトも捨てないこと。
    // 変えるのは、装置が USB からいったん消えて (切り離されて) 戻ったのを見たときだけにすること。
    // 番号が変わると、Fuk は 'M' とデータの送り残しを捨てて見分けの 0x00 を送る。装置の電源が保たれたまま
    // USB だけが切れて戻った (ハブの瞬断、ホストのコントローラのリセット、スリープからの復帰) ときは、装置が
    // まだ 'M' のデータを待っていることがあり、そうなら 0x00 がデータとして読まれ、0 で埋めたセクタが書かれうる。
    // 口からはこれを見分けられない (docs/fuk.md の「既知の制限」)
    virtual unsigned epoch() = 0;
    // port (0 / 1) に送る。受け付けたバイト数を返す
    virtual unsigned send(int port, const uint8_t* data, unsigned n) = 0;
    // port に届いたバイトを max まで取り出す。なければ 0
    virtual unsigned receive(int port, uint8_t* dst, unsigned max) = 0;
    // 直前の receive(port) で取り出した最後のバイトが、口に届いた実時間 (nowUs() と同じ元)。receive() は、届いた時刻の
    // 違うバイトを 1 回にまとめないこと (まとめると、前のバイトもあとの時刻になる)。Read のデータの届き方を
    // X68000 の DMA の速さに写すのに使う (#320)。取り出す側の都合 (X68000 を 1 フレームずつまとめて動かす) で遅れた
    // 時刻ではなく、届いた時刻を返すこと。分からなければ nowUs() でよい (そのときは届き方を写せない)
    virtual uint64_t receivedAtUs(int port) {
        (void)port;
        return nowUs();
    }
    // 届いた時刻を覚えるリングが一杯で、覚えられなかったまとまりの数 (調べるとき用。そのバイトは、あとのまとまりの
    // 時刻になる)
    virtual unsigned droppedStamps() { return 0; }
    // 実時間 (us)。始まりはいつでもよい
    virtual uint64_t nowUs() = 0;
    // 受信を待つ: us たつか、何か届いたら戻る (届かなくても us より早く戻ってよい。OS の都合で us より少し長く待つことがある)
    virtual void waitRx(unsigned us) = 0;
    // port に send() で渡したバイトが、すべて装置へ出ていったか (口の中に残っていない)。send() は受け付けた時点で
    // 戻るので、順番を守って別のポートに送るときに確かめる。確かめられない口は true を返してよい (ホスト版は
    // OS が送信のバッファの残りを答えないとき)
    virtual bool sendIdle(int port) = 0;
};

class Fuk {
public:
    // DMAC のチャネル 0 の転送バイト数 (MTC)。Write のコマンドの前に FUK へ知らせる
    class Host {
    public:
        virtual ~Host() {}
        // チャネル 0 が動いていなければ (開始していない、異常終了したあとなど) 0 を返すこと。MTC には前の転送の
        // 残りの値が残っていることがある
        virtual uint32_t fukDmaCount() = 0;
        // チャネル 0 の転送が、fukDmaCount() のバイト数で終わる 1 ブロックだけか (チェーンやコンティニューでない)。
        // そうでなければ、Write で FUK に知らせるバイト数が分からない
        virtual bool fukDmaSingleBlock() = 0;
        // X68000 の時間 (ns、0 は「分からない」) と、1 フレームの長さ (ns)。DMA の Read のデータを、FUK から届いた
        // 間隔のまま X68000 の時間に写すのに使う (#320、docs/fuk.md の「Read のデータを渡す時刻」)。fukEmuNs() が 0 を
        // 返すと、届いたデータをすぐに渡す。fukFrameNs() が 0 なら、60 Hz の 1 フレーム (DEFAULT_FRAME_NS) とみなす
        virtual uint64_t fukEmuNs() { return 0; }
        virtual uint64_t fukFrameNs() { return 0; }
    };

    // ---- 時間 (us、実時間)。理由は docs/fuk.md の「時間」に書いた ----
    static constexpr uint32_t MSR_FRESH_US = 20000;      // コマンドを待っているときのステータスを、問い合わせ直さずに返す時間
    static constexpr uint32_t DRIVE_FRESH_US = 50000;    // ドライブの状態を、読み直さずに返す時間
    static constexpr uint32_t WAIT_US = 30000;           // 先に読んでいない値を読まれたときに、返事を待つ上限
    static constexpr uint32_t REPLY_TIMEOUT_US = 500000; // ポート 1 の返事が来ないと決める時間
    static constexpr uint32_t READ_TIMEOUT_US = 3000000; // 'R' が来ないと決める時間
    static constexpr uint32_t WRITE_TIMEOUT_US = 10000000; // 'W' や 'w' が来ないと決める時間
    static constexpr uint32_t DMA_TIMEOUT_US = 1000000;  // 'W' のあと、DMAC がデータを渡してこないと決める時間 (書き込みをやめる)
    static constexpr uint32_t RESULT_TIMEOUT_US = 3000000; // READ ID などのリザルトが用意されないと決める時間
    static constexpr uint32_t RESULT_POLL_US = 2000;     // リザルトを待つ間に、ステータスを問い合わせる間隔
    static constexpr uint32_t DRAIN_QUIET_US = 30000;    // 見分ける前に、これだけ何も届かなければ受信が空になったとみなす
    static constexpr uint32_t DRAIN_MAX_US = 500000;     // 受信が止まらないときに、捨てるのをやめる時間
    static constexpr uint32_t SETTLE_QUIET_US = 10000;   // リセットのあと、ポート 2 にこれだけ何も届かなければ使い始める
    static constexpr uint32_t PROBE_TIMEOUT_US = 300000; // 見分けの返事 ('f' / 'F') を待つ時間
    static constexpr uint32_t RETRY_US = 1000000;        // 見分けに失敗したあと、やり直すまでの時間
    static constexpr uint32_t YIELD_STEP_US = 200;       // ステータスを読まれたときに、返事を待つ 1 回の長さ
    static constexpr uint32_t YIELD_WINDOW_US = 16000;   // この時間 (ほぼ 1 フレーム) あたり、
    static constexpr uint32_t YIELD_BUDGET_US = 2000;    // ここまで待つ
    static constexpr uint32_t PAUSE_GAP_US = 100000;     // poll() の間がこれだけ空いたら、X68000 が止められていたとみなす

    struct Stats {
        uint32_t reads = 0;        // ポート 1 に送った読み出し
        uint32_t replies = 0;      // 返事を受け取った読み出し
        uint64_t replyUs = 0;      // 読み出しを送ってから返事を受け取るまでの時間の合計
        uint32_t replyMaxUs = 0;   // その最大
        uint32_t waits = 0;        // 返事を待った回数 (X68000 を止めた回数)
        uint32_t waitTimeouts = 0; // そのうち、上限まで待っても届かなかった回数
        uint64_t waitUs = 0;       // 待った時間の合計
        uint32_t yields = 0;       // ステータスを読まれたときに、返事を少し待った回数 (yieldForReply())
        uint64_t yieldUs = 0;      // その時間の合計
        uint32_t timeouts = 0;     // 返事や 'R' 'W' 'w' が来なかった回数
        uint32_t links = 0;        // 見分けに成功した回数
        uint32_t linkFailures = 0; // 見分けに失敗した回数
        uint32_t readFrames = 0;   // 受け取った 'R'
        uint32_t writeFrames = 0;  // 送った 'M'
        uint32_t stray = 0;        // 捨てたバイト (待っていない返事、分からない通知)
        uint32_t queueWaits = 0;   // 書き込みがたまって、空くのを待った回数
        uint32_t cancelledWrites = 0; // データがそろわず、FUK に送る前にやめた Write
        uint32_t refusedWrites = 0; // 受け付けなかった、DMA を使わない Write
        uint32_t streamedReads = 0; // 'R' のデータが届いている途中から DMAC に渡し始めた Read
        uint32_t timedReads = 0;    // データを届いた時刻の間隔で X68000 の時間に写した Read
        uint64_t leadUs = 0;        // その遅れの合計 (X68000 の時間で、DMA の Read ごとに 1 回)
        uint64_t spanUs = 0;        // その Read のデータが届くのにかかった時間 (最初のバイトから最後のバイトまで) の合計
        uint32_t lateReads = 0;     // 渡す時刻を過ぎてから届いたバイトがあった Read (遅れが足りなかった)
        uint32_t lateMaxUs = 0;     // そのうち、いちばん遅れたもの (X68000 の時間)
        uint32_t tcReads = 0;       // TC で終えた、写した Read
        uint32_t tcUs[6] = {};      // 最近の 6 回の、最初のバイトを渡してから TC までの X68000 の時間 (tcUs[tcReads % 6] が次)
    };

    explicit Fuk(FukLink& link, Host* host = nullptr);
    ~Fuk();
    void setHost(Host* host) { (reader_ ? readerHost_ : host_) = host; }
    // DMA の Read のデータを、最初のバイトを受け取ってから us だけ遅らせて渡し始める (fuk_lead=)。0 なら自動で、
    // 「1 フレーム + その 1/8 (Host::fukFrameNs() から決める) - 最初のバイトが届いてからの時間」を上限に、'R' が全部
    // 届いた時点で決める。遅れなしは指定できない (1 で代用)
    void setLeadUs(uint32_t us) { leadUs_ = us; }
    static constexpr uint64_t DEFAULT_FRAME_NS = 16683000; // fukFrameNs() が 0 のときの 1 フレーム

    // FUK を見分けて使えるようになるまで、長くて waitMs 待つ (起動のときに使う。X68000 を動かす前に呼ぶ)。
    // 使えるようになったら true。false でも、あとからつなげば poll() の中で見分ける
    bool start(unsigned waitMs);
    // 使える (見分けが済んでいる)
    bool ready() const { return ls_ >= L_SYNC; }
    // 状態が変わるたびに増える (環境がログに出すために見る)
    const Stats& stats() const { return stats_; } // X68000 を動かすスレッド (コア) から読む
    // ---- ほかのスレッド (コア) から読んでよいもの (ベアメタル版のコア 0 がログに出す)。poll() が 4 ms に 1 回ほど更新する ----
    unsigned linkChanges() const { return __atomic_load_n(&linkChanges_, __ATOMIC_ACQUIRE); }
    bool readyShared() const { return __atomic_load_n(&sharedReady_, __ATOMIC_ACQUIRE); }
    Stats snapshot();

    // ---- Machine から (内蔵の FDC と同じ使い方) ----
    void reset();                            // X68000 のリセット
    uint8_t read(uint32_t offset);           // $E94001-$E94007
    void write(uint32_t offset, uint8_t v);
    void writeIoc(uint32_t offset, uint8_t v); // $E9C001-$E9C00F への書き込み (FUK にも伝える)
    void setForceReady(bool on);             // FDD の強制レディ (OPM の CT)
    bool irq() const;                        // FDC の割り込み
    bool fddIrq() const { return fddIrq_; }  // FDD の割り込み (ディスクの出し入れ)

    // ---- ディスクの読み出し (x68k/fuk_reader.h の FukReader) から。X68000 を動かしていない間だけ使う (#381) ----
    // X68000 が FDC を使っている途中 (コマンド、リザルト、SEEK の割り込み、DMAC のチャネル 0、強制レディ) でなく、
    // FUK とのやり取りも残っていない
    bool idleForReader();
    // X68000 が FDC を使っている途中 (X68000 が動かないと終わらない)。Fuk のやり取りは進めない。false でも、FUK の返事や
    // 送り残しを待っていれば idleForReader() は false (待てば true になる)
    bool busyForReader();
    // ドライブ unit (0-3) のモーターを回し、DMAC とのつながりを外す。外している間、Fuk は書き込みのコマンドを
    // FUK に送らない (転送バイト数が 0 なので、DMA を使わない書き込みとして断る)。idleForReader() のときだけ呼ぶ
    void beginReader(int unit);
    // ドライブの選択とモーターを、X68000 が最後に書いた値に戻す (値が変わるなら true)
    bool restoreReaderMotor();
    // 外したものを戻す。head は、ヘッドをどうしたか (RESTORED は readerCylinder()、分からなければ 0 に戻した)。
    // LOST なら、X68000 がそのドライブの SEEK / RECALIBRATE をするまで WRITE ID を断る (READ / WRITE DATA は ID が
    // 合わなければエラーになるが、WRITE ID はヘッドのいるトラックに書くため)
    enum class ReaderHead : uint8_t { UNMOVED, RESTORED, LOST };
    void endReader(ReaderHead head);
    // X68000 が最後に頼んだシーク先のシリンダ (分からなければ -1)。beginReader() のときの値
    int readerCylinder() const { return readerCyl_[readerUnit_]; }
    // つながりが切れるか、FDC をリセットするたびに増える
    unsigned resetCount() const { return resetCount_; }
    // 'D' (ディスクの出し入れ) が届くたびに増える
    unsigned fddEvents() const { return fddEvents_; }
    uint64_t nowUs() { return link_.nowUs(); }

    // 届いたものを処理し、送るものを送る。ラスタごとと、レジスタを読み書きしたときに呼ぶ
    void poll();
    // DMAC から
    bool dataReady();
    int readData(uint8_t* dst, int n);
    void terminalCount();
    void abortTransfer();

private:
    // L_SYNC: リセットを送り、ステータスの返事を待っている。L_SETTLE: 返事が届き、ポート 2 が静かになるのを待っている
    // (L_SETTLE の間は、X68000 のコマンドを FUK に送らずにためておく)
    enum LinkState : uint8_t { L_OFFLINE, L_DRAIN, L_PROBE, L_SYNC, L_SETTLE, L_READY };
    enum Phase : uint8_t {
        P_IDLE,         // コマンドを待っている / コマンドのバイトを受け取っている
        P_READ_WAIT,    // 'R' を待っている
        P_READ_DATA,    // 'R' のデータを DMAC (か CPU) に渡している
        P_WRITE_WAIT,   // 'W' か 'w' を待っている
        P_WRITE_DATA,   // DMAC (か CPU) からデータを受け取っている
        P_WRITE_BUSY,   // 'M' を送って 'w' を待っている
        P_RESULT_WAIT,  // リザルトが用意されるのを、ステータスを問い合わせながら待っている
        P_RESULT_FETCH, // リザルトを決まった数だけ読み出している
        P_RESULT,       // リザルトがそろい、X68000 が読んでいる
    };
    enum : uint8_t { OP_WRITE, OP_READ, OP_RESET };
    // 読み出しの返事の使い道
    enum : uint8_t { T_MSR, T_RESULT, T_SYNC, T_DIRECT, T_STALE /* リセットの前に送った読み出し (捨てる) */, T_DRIVE = 0x10 /* 下位 4 ビットは選んだドライブの組 */ };
    struct Op {
        uint8_t kind, reg, val;
    };
    static constexpr unsigned QUEUE = 256;       // 2 のべき乗
    static constexpr unsigned QUEUE_RESERVE = 32; // そのうち、こちらが送る読み出しのために空けておく数
    static constexpr unsigned BUF = 65536 + 16;  // 'R' の長さは 16 ビット

    uint64_t now() { return link_.nowUs(); }
    bool push(uint8_t kind, uint8_t reg, uint8_t val);
    void pushWrite(uint8_t reg, uint8_t v);
    void publish();
    void pushRead(uint8_t reg, uint8_t tag);
    void pump();
    void service();
    void serviceLink();
    void receivePort1();
    void receivePort2();
    void port2Byte(uint8_t b);
    void reply(uint8_t tag, uint8_t v);
    void checkTimeouts();
    template <typename F> bool waitFor(F done);
    void goOffline();
    void relink();
    void linkChanged() {
        __atomic_store_n(&sharedReady_, ready(), __ATOMIC_RELEASE);
        __atomic_add_fetch(&linkChanges_, 1u, __ATOMIC_ACQ_REL);
    }
    void clearCommand();
    void setPhase(Phase p);
    void requestMsr();
    void yieldForReply();
    void invalidateMsr();
    void dropQueue();
    void frameDone();
    void dispatch();
    void startFetch(int want, bool irq);
    void enterResult(bool irq);
    void failSeeks();
    void resetDevice();
    void cancelWrite();
    bool tailPending();       // 'M' とデータを、まだ装置が書き終えていない ('w' も 't' も来ておらず、時間切れでもない)
    void scanDone(const uint8_t* p, unsigned n); // 捨てるポート 2 のバイトの中の 'w' と 't' (書き込みの終わり) を拾う
    bool localSeekPending() const;
    void failCommand();       // 実行中のコマンドを、こちらで作った異常終了にする
    void sendWriteData();
    void localWrite(uint8_t v); // つながっていない間の FDC
    void localExecute();
    void setResult7(uint8_t st0, uint8_t st1);
    uint8_t unit() const { return cmd_[1] & 3; }

    FukLink& link_;
    Host* host_;
    Stats stats_;
    unsigned linkChanges_ = 0;  // ほかのスレッドも読む (atomic)
    bool sharedReady_ = false;  // 同じく
    bool sharedLock_ = false;   // sharedStats_ を読み書きしている間は true (__atomic_test_and_set)
    Stats sharedStats_;
    unsigned publishCount_ = 0;
    bool queueOverflow_ = false;

    // つながり
    LinkState ls_ = L_OFFLINE;
    unsigned epoch_ = 0;
    bool wasOnline_ = false, probeFailed_ = false;
    uint64_t yieldWindow_ = 0;
    uint32_t yieldUsed_ = 0;
    uint64_t lsSince_ = 0, lastRx_ = 0, retryAt_ = 0, lastService_ = 0;
    int p1_ = 0, p2_ = 1;
    int probe_[2] = {-1, -1};
    bool probeSent_[2] = {false, false};

    // ポート 1 に送るもの (順番どおりに送る。読み出しは、返事が届くまで次を送らない)
    Op q_[QUEUE];
    unsigned qHead_ = 0, qTail_ = 0;
    uint8_t out1_[128];     // 口に渡す途中のバイト
    unsigned out1Len_ = 0, out1Pos_ = 0;
    bool awaiting_ = false;
    uint8_t awaitTag_ = 0;
    uint64_t awaitSince_ = 0, awaitSent_ = 0;

    // ポート 2 の受信
    uint8_t rx2State_ = 0; // 0 通知、1 長さの下位、2 長さの上位、3 データ
    uint32_t frameLen_ = 0, frameGot_ = 0;
    uint8_t* data_;        // 'R' のデータとリザルト
    uint32_t dataLen_ = 0, dataPos_ = 0;
    // DMA の Read で、'R' のデータが届いている途中から DMAC に渡している (#320)。readTc_ / readAbort_ は、その間に
    // TC か DMA の異常終了が来た (残りとリザルトが届いてからリザルトにする)
    bool streaming_ = false, readTc_ = false, readAbort_ = false;
    void startStream();
    uint32_t dataAvail();
    // データを X68000 の時間に写す (#320)。dataAt_[i] は i バイト目が口に届いた時刻 ('R' から us)。timed_ のとき、
    // i バイト目は X68000 の時間で anchorEmu_ + leadNs_ + (dataAt_[i] - dataAt_[0]) を過ぎてから渡す
    uint32_t* dataAt_;
    uint64_t frameAt_ = 0;
    bool timed_ = false;
    uint64_t anchorEmu_ = 0, leadNs_ = 0, leadMax_ = 0;
    bool leadFixed_ = false;  // leadNs_ を決めた (それまでは渡さない)
    bool late_ = false;       // この Read で、渡す時刻を過ぎてから届いたバイトがあった
    uint64_t firstEmu_ = 0;   // 最初のバイトを渡した X68000 の時間 (0 はまだ)
    uint32_t availPos_ = 0;
    uint32_t leadUs_ = 0;
    void beginRead();

    // Write
    uint8_t* wbuf_;        // 'M' とデータ
    uint32_t wNeed_ = 0, wGot_ = 0, wSent_ = 0, wTotal_ = 0;
    int8_t lastCyl_[4] = {-1, -1, -1, -1};   // 最後に送った SEEK / RECALIBRATE の行き先 (分からなければ -1)
    uint8_t headLost_ = 0;   // ヘッドが X68000 の思うシリンダにいるか分からないドライブ (ビットごと)
    bool cmdHold_ = false;   // WRITE ID の 1 バイト目を、ドライブが分かるまで送らずに持っている
    // FukReader が使っている間の、戻すもの
    bool reader_ = false, readerMotor_ = false;
    Host* readerHost_ = nullptr;
    int readerUnit_ = 0;
    int8_t readerCyl_[4] = {-1, -1, -1, -1};
    bool readerSis_ = false, readerNoDma_ = false, readerNonDma_ = false;
    unsigned readerReset_ = 0;
    unsigned resetCount_ = 0, fddEvents_ = 0;
    uint8_t lastSelect_ = 0, lastControl_ = 0; // X68000 が最後に書いたドライブ選択とドライブ制御 (リセットのあとに送り直す)
    bool dmaAborted_ = false; // FUK の答え ('R' / 'W') を待っている間に、DMA が異常終了した
    bool wFlushing_ = false;  // 'M' とデータを口に渡し終えたが、装置へ出きったかをまだ確かめていない
    bool wAwaitDone_ = false; // 'M' を送り始めたが、まだ 'w' (書き込みの終わり) を受け取っていない
    uint64_t wSince_ = 0;     // 'M' を送り始めた時刻。データが口から出きったときに付け直す ('w' の時間切れはそこから数える)

    // コマンド
    Phase phase_ = P_IDLE;
    uint64_t phaseSince_ = 0, resultPollAt_ = 0;
    uint8_t cmd_[9] = {};
    int cmdLen_ = 0, cmdNeed_ = 0;
    bool cmdLocal_ = false;   // 受け取っているコマンドは FUK に送っていない (つながっていない、受け付けない)
    bool cmdRefused_ = false; // DMA を使わない書き込みなので、受け付けない
    uint8_t res_[7] = {};
    int resLen_ = 0, resPos_ = 0, resWant_ = 0;
    bool resIrq_ = false;
    bool resFetching_ = false; // リザルトの読み出しを送って、返事を待っている
    bool maskDevIrq_ = false;  // READ ID の間は、FUK の 'C' を X68000 に見せない
    bool nonDma_ = false;     // SPECIFY の ND
    bool noDma_ = false;      // コマンドを受け取ったときに DMAC の転送バイト数が 0 だった (CPU が読み書きする)
    bool sisLocal_ = false;   // Read のあとの SENSE INTERRUPT STATUS は FUK に送らない
    uint8_t seekBusy_ = 0;    // SEEK / RECALIBRATE を送り、まだ SENSE INTERRUPT STATUS で引き取っていないドライブ
    uint8_t localSeek_[4] = {}; // つながっていない間の SEEK / RECALIBRATE の結果 (0 = なし)

    // ステータス
    uint8_t msr_ = 0x80;
    bool msrKnown_ = false, msrPending_ = false, msrRequery_ = false;
    uint64_t msrAt_ = 0;

    // ドライブ
    uint8_t driveSel_ = 0;
    uint8_t driveStat_[16] = {};
    uint64_t driveAt_[16] = {};
    bool driveKnown_[16] = {};
    uint8_t drivePending_[16] = {};
    bool directDone_ = false;
    uint8_t directValue_ = 0;

    // 割り込み
    bool devIrq_ = false, localIrq_ = false, fddIrq_ = false;
    bool forceReady_ = false, forceReadySent_ = false;
};

} // namespace x68k
