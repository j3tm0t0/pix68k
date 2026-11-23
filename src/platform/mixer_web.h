// 音を鳴らしたまま変えるミキサー (platform/mixer.h。#364) の、Web の口 (GET /mixer、POST /mixer、POST /mixer/save、
// GET /mixer.html) の要求の読み方、応答の作り方、ページ。HTTP のタスクは主ループと同じコア 0 で動く (baremetal/host_net.cpp)。
// ベアメタルでも使えるよう、例外、RTTI、STL、stdio は使わない (ヘッダだけ)。
#pragma once

#include "platform/mixer.h"
#include "platform/net_http.h"

namespace platform {

// Web に出すレベル。1 秒ごとに締めた最大を、何度読んでも同じ値で返す (読んでも 0 に戻さないので、ページを 2 つ開いても欠けない)
class WebLevels {
public:
    static constexpr uint32_t PERIOD_MS = 1000, IDLE_MS = 5000;
    // 取ったレベルを足す (TakeSoundLevels() が、画面のミキサーなどの分と一緒に足す)
    void add(const x68k::Sound::Levels& lv) {
        if (active_) x68k::Sound::mergeLevels(acc_, lv);
    }
    // 読むとき。しばらく読まれていなければ、溜めていない状態から始める (始めたら true)
    bool touch(uint32_t nowMs) {
        lastRead_ = nowMs;
        if (active_) return false;
        active_ = true;
        acc_ = shown_ = x68k::Sound::Levels();
        lastTick_ = nowMs;
        return true;
    }
    // 締める時刻が来たか (主ループから毎回呼ぶ)。true なら、レベルを取って足してから close() を呼ぶ
    bool due(uint32_t nowMs) {
        if (!active_) return false;
        if (nowMs - lastRead_ >= IDLE_MS) {
            active_ = false;
            return false;
        }
        return nowMs - lastTick_ >= PERIOD_MS;
    }
    void close(uint32_t nowMs) {
        lastTick_ = nowMs;
        shown_ = acc_;
        acc_ = x68k::Sound::Levels();
    }
    const x68k::Sound::Levels& recent() const { return shown_; }

private:
    x68k::Sound::Levels acc_ = {}, shown_ = {};
    uint32_t lastRead_ = 0, lastTick_ = 0;
    bool active_ = false;
};

// Web の保存。書くのは主ループ (HTTP のタスクのスタックは小さい)。頼む側は done() になるまで待つ
class WebSave {
public:
    void request() { state_ = WANT; }
    void cancel() { state_ = NONE; }
    bool wanted() const { return state_ == WANT; }
    void finish(bool ok) { state_ = ok ? OK : FAILED; }
    void refuse() { state_ = BUSY; } // 待つ間に設定メニューが開いたか、再起動のために X68000 を止めている
    bool done() const { return state_ == OK || state_ == FAILED || state_ == BUSY; }
    bool busy() const { return state_ == BUSY; }
    bool ok() const { return state_ == OK; }

private:
    enum { NONE, WANT, OK, FAILED, BUSY } state_ = NONE;
};

inline const char* mixerChannelName(int group) {
    static const char* const kNames[MIXER_GROUPS] = {"opm", "adpcm", "mercury", "ymf288", "midi", "cd", "master"};
    return kNames[group];
}

// POST /mixer?ch=midi&db=-3 (db=off も)。読めなければ false (error に理由)
struct MixerWebRequest {
    int group = -1, dB = 0;
    const char* error = "";
};
inline bool parseMixerRequest(const char* query, MixerWebRequest& r) {
    r = MixerWebRequest();
    char v[16];
    if (queryValue(query, "ch", v, sizeof v))
        for (int i = 0; i < MIXER_GROUPS; i++)
            if (strcmp(v, mixerChannelName(i)) == 0) r.group = i;
    if (r.group < 0) {
        r.error = "ch= is opm, adpcm, mercury, ymf288, midi, cd or master\n";
        return false;
    }
    r.error = "db= is -40 to 12, or off\n";
    if (!queryValue(query, "db", v, sizeof v)) return false;
    if (strcmp(v, "off") == 0) {
        r.dB = Settings::VOLUME_OFF;
        return true;
    }
    const char* p = v;
    const bool neg = *p == '-';
    if (neg || *p == '+') p++;
    int n = 0, digits = 0;
    for (; *p >= '0' && *p <= '9' && digits < 3; p++, digits++) n = n * 10 + (*p - '0');
    if (digits == 0 || *p != 0) return false;
    if (neg) n = -n;
    if (n < VOLUME_MIN_DB || n > VOLUME_MAX_DB) return false;
    r.dB = n;
    return true;
}

static constexpr size_t MIXER_JSON_SIZE = 1024;
// GET /mixer の応答 (JSON)。level は 0 から MIXER_STEPS (画面の棒と同じ段)。db の off は null
inline void formatMixerJson(char* out, size_t size, const MixerControl& c, const x68k::Sound::Levels& lv, bool unsaved) {
    const SoundSources src = c.mixerSources();
    uint32_t peak[MIXER_GROUPS];
    mixerPeaks(lv, peak);
    MenuText t(out, size);
    t.add("{\"unsaved\":").add(unsaved ? "true" : "false").add(",\"steps\":").add((long)MIXER_STEPS).add(",\"ch\":[");
    for (int i = 0; i < MIXER_GROUPS; i++) {
        const bool present = mixerPresent(src, i);
        const char* note = i == x68k::Sound::VOL_MIDI && src.midi ? osdMidiFaultText(src.midiOut, src.midiFault) : nullptr;
        t.add(i ? ",{\"name\":\"" : "{\"name\":\"").add(mixerChannelName(i)).add("\",\"db\":");
        if (c.mixerVolume(i) == Settings::VOLUME_OFF) t.add("null");
        else t.add((long)c.mixerVolume(i));
        t.add(",\"level\":").add((long)(present ? mixerStep(peak[i]) : 0));
        t.add(",\"clip\":").add(present && mixerClipped(lv, peak, i) ? "true" : "false");
        t.add(",\"present\":").add(present ? "true" : "false");
        if (note != nullptr) t.add(",\"note\":\"").add(note).add("\"");
        t.add("}");
    }
    t.add("]}\n");
}

// ブラウザのページから送られた書き込み (Origin: が付く) を、ミキサーの口だけで受けるか。同じ機械のページ (Origin: が
// http:// + Host:) で、net_token= が設定してあるときだけ。Host: は checkHost() で確かめたもの。
// トークンが合うかは、ほかの書き込みと同じく checkNetAccess() で見る
inline bool mixerOriginAllowed(const HttpRequest& r, const char* token) {
    static const char kScheme[] = "http://";
    const size_t n = sizeof kScheme - 1, len = strlen(r.origin);
    return token[0] != 0 && r.hasHost && r.host[0] != 0 && len > n && http_detail::equalNoCase(r.origin, n, kScheme) &&
           http_detail::equalNoCase(r.origin + n, len - n, r.host);
}

// GET /mixer.html。1 秒ごとに GET /mixer を読む (隠れている間は読まない)。トークンはページの変数にだけ置く
inline const char* mixerPage() {
    return R"PAGE(<!doctype html><html lang="ja"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>pix68k mixer</title><style>
:root{color-scheme:light dark}body{font:15px system-ui,sans-serif;max-width:34rem;margin:1rem auto;padding:0 16px}
.r{display:grid;grid-template-columns:4.5rem 1fr 4rem;gap:.2rem .6rem;align-items:center;margin:.5rem 0}
.r input{width:100%}.r output{text-align:right;font-variant-numeric:tabular-nums}
meter{grid-column:2;width:100%;height:.7rem}.off{opacity:.5}input[type=password]{width:12rem}
</style><h1>Mixer</h1><div id="rows"></div>
<p><button id="save">Save</button> <span id="st" role="status"></span></p>
<p><label>net_token <input id="tok" type="password" autocomplete="off"></label></p>
<script>
const $=i=>document.getElementById(i),R={};let busy=0,failed=0;
const label=v=>v===null?'off':(v>0?'+':'')+v+' dB';
function say(s){$('st').textContent=s;failed=0}
function lost(s){say(s);failed=1}
async function post(u){try{const r=await fetch(u,{method:'POST',headers:{'X-Pix68k-Token':$('tok').value}});
const t=(await r.text()).trim();say(r.ok?'':t);return r.ok}catch(e){lost('cannot connect');return false}}
function row(c){const d=document.createElement('div');d.className='r';
const n=document.createElement('label'),i=document.createElement('input'),o=document.createElement('output'),m=document.createElement('meter'),w=document.createElement('small');
n.textContent=c.name;i.type='range';i.min=-41;i.max=12;i.id=n.htmlFor='v_'+c.name;m.min=0;
d.append(n,i,o,m,w);$('rows').append(d);const x=R[c.name]={d,i,o,m,w,drag:0};
const val=()=>i.value<-40?null:+i.value;
let t;i.onpointerdown=()=>{clearTimeout(t);x.drag=1};
i.onpointerup=i.onpointercancel=i.onblur=()=>{clearTimeout(t);t=setTimeout(()=>x.drag=0,100)};
i.oninput=()=>{o.textContent=label(val())};
i.onchange=async()=>{const v=val();x.drag=0;busy++;await post('/mixer?ch='+c.name+'&db='+(v===null?'off':v));busy--};
return x}
async function poll(){if(!document.hidden&&!busy){try{const r=await fetch('/mixer',{cache:'no-store'});
if(!r.ok)lost((await r.text()).trim());else{if(failed)say('');const j=await r.json();if(!busy)for(const c of j.ch){const x=R[c.name]||row(c);
if(!x.drag){x.i.value=c.db===null?-41:c.db;x.o.textContent=label(c.db)}
x.m.max=j.steps;x.m.value=c.level;x.d.classList.toggle('off',!c.present);
x.w.textContent=c.note||(c.present?(c.clip?'clip':''):'-')}
$('save').textContent=j.unsaved?'Save *':'Save'}}catch(e){lost('cannot connect')}}setTimeout(poll,1000)}
$('save').onclick=async()=>{busy++;if(await post('/mixer/save'))say('saved');busy--};poll();
</script></html>
)PAGE";
}

} // namespace platform
