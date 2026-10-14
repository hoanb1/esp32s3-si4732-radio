#ifndef WEBUI_H
#define WEBUI_H

#include <Arduino.h>

const char FT8_WEBUI_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="vi">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>ESP32-S3 Si4732 Multi-Mode WebSDR &amp; FT8/FT4 Receiver</title>
<style>
:root{
  --bg:#060c17;--bg2:#0d1626;--card:#111d32;--card2:#16243d;--border:#223656;
  --cyan:#00e5ff;--green:#00e676;--amber:#ffab00;--red:#ff5252;--purple:#b388ff;
  --text:#e6f1ff;--muted:#88a0bf;
}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI',system-ui,-apple-system,sans-serif;background:radial-gradient(circle at top,#10203a 0%,var(--bg) 70%);color:var(--text);padding:12px;line-height:1.45;min-height:100vh}
.container{max-width:1360px;margin:0 auto;display:flex;flex-direction:column;gap:12px}

/* Header */
header{display:flex;flex-wrap:wrap;justify-content:space-between;align-items:center;background:linear-gradient(135deg,#11213a 0%,#0b1526 100%);border:1px solid var(--border);border-radius:12px;padding:12px 16px;gap:10px;box-shadow:0 6px 20px rgba(0,0,0,.4)}
.title-wrap h1{font-size:1.25rem;color:var(--cyan);letter-spacing:.3px;display:flex;align-items:center;gap:8px;flex-wrap:wrap}
.title-wrap p{font-size:.8rem;color:var(--muted)}
.status-bar{display:flex;flex-wrap:wrap;gap:8px;align-items:center}
.badge{padding:5px 11px;border-radius:999px;font-size:.78rem;font-weight:600;background:#1a2b47;border:1px solid var(--border);display:inline-flex;align-items:center;gap:6px}
.badge.ok{background:rgba(0,230,118,.12);color:var(--green);border-color:rgba(0,230,118,.4)}
.badge.warn{background:rgba(255,171,0,.14);color:var(--amber);border-color:rgba(255,171,0,.4)}
.badge.info{background:rgba(0,229,255,.12);color:var(--cyan);border-color:rgba(0,229,255,.35)}
.dot{width:8px;height:8px;border-radius:50%;background:currentColor;display:inline-block;box-shadow:0 0 6px currentColor}

/* VFO Cockpit Banner */
.vfo-cockpit{display:grid;grid-template-columns:1.35fr 1fr;gap:12px;background:linear-gradient(180deg,#0b1528 0%,#08101e 100%);border:1px solid #28436c;border-radius:12px;padding:14px 16px;box-shadow:inset 0 0 24px rgba(0,229,255,.05),0 6px 18px rgba(0,0,0,.35)}
@media(max-width:900px){.vfo-cockpit{grid-template-columns:1fr}}
.vfo-left{display:flex;flex-direction:column;justify-content:space-between;gap:8px}
.vfo-main-row{display:flex;align-items:baseline;gap:12px;flex-wrap:wrap}
.freq-big{font-family:'Courier New',monospace;font-size:2.75rem;font-weight:800;color:var(--green);text-shadow:0 0 16px rgba(0,230,118,.35);letter-spacing:1px;line-height:1}
.freq-unit{font-size:1.1rem;color:var(--cyan);font-weight:700}
.mode-pill{padding:5px 12px;border-radius:8px;font-weight:800;font-size:.92rem;background:rgba(0,229,255,.16);color:var(--cyan);border:1px solid rgba(0,229,255,.45);letter-spacing:.5px}
.mode-pill.dig{background:rgba(0,230,118,.18);color:var(--green);border-color:rgba(0,230,118,.5)}
.band-pill{padding:5px 11px;border-radius:8px;font-weight:700;font-size:.86rem;background:rgba(255,171,0,.15);color:var(--amber);border:1px solid rgba(255,171,0,.4)}
.vfo-sub-row{display:flex;flex-wrap:wrap;gap:8px;font-size:.8rem;color:var(--muted)}
.vfo-chip{background:#122038;border:1px solid #22385c;padding:3px 9px;border-radius:6px;color:#c9dfff;font-weight:600}
.vfo-chip b{color:var(--cyan)}

/* S-Meter & Signal */
.vfo-right{display:flex;flex-direction:column;justify-content:center;gap:8px;background:#0c172b;border:1px solid #1f3354;border-radius:10px;padding:10px 14px}
.smeter-top{display:flex;justify-content:space-between;align-items:center;font-size:.82rem;font-weight:700}
.smeter-scale{display:flex;justify-content:space-between;font-size:.68rem;color:var(--muted);font-family:monospace;padding:0 2px}
.smeter-scale span.plus{color:var(--red);font-weight:700}
.smeter-bar-bg{height:12px;background:#060c17;border:1px solid #24395c;border-radius:6px;overflow:hidden;position:relative}
.smeter-bar-fill{height:100%;width:0%;background:linear-gradient(90deg,#00e676 0%,#00e5ff 55%,#ffab00 80%,#ff5252 100%);transition:width .2s ease}
.sig-metrics{display:grid;grid-template-columns:repeat(4,1fr);gap:6px;text-align:center}
.sig-box{background:#111f38;border:1px solid #1f3458;border-radius:6px;padding:4px 6px}
.sig-box .lbl{font-size:.68rem;color:var(--muted); display:block}
.sig-box .val{font-size:.88rem;font-weight:700;color:var(--text)}

/* Cards & Grid */
.grid-2{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.grid-3{display:grid;grid-template-columns:1.15fr 1fr .95fr;gap:12px}
@media(max-width:1080px){.grid-3{grid-template-columns:1fr 1fr}}
@media(max-width:780px){.grid-2,.grid-3{grid-template-columns:1fr}}

.card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:13px 15px;box-shadow:0 4px 14px rgba(0,0,0,.28);display:flex;flex-direction:column;gap:10px}
.card-hdr{display:flex;justify-content:space-between;align-items:center;border-bottom:1px solid var(--border);padding-bottom:7px;gap:8px;flex-wrap:wrap}
.card-title{font-size:.9rem;font-weight:700;color:var(--cyan);text-transform:uppercase;letter-spacing:.5px;display:flex;align-items:center;gap:6px}
.lbl-sm{font-size:.76rem;color:var(--muted);font-weight:600}

/* Buttons & Inputs */
.btn-row{display:flex;flex-wrap:wrap;gap:6px}
.btn-grid-6{display:grid;grid-template-columns:repeat(6,1fr);gap:5px}
.btn-grid-7{display:grid;grid-template-columns:repeat(7,1fr);gap:5px}
@media(max-width:560px){.btn-grid-6,.btn-grid-7{grid-template-columns:repeat(3,1fr)}}

button{background:#1b2c4a;color:var(--text);border:1px solid #2d4670;padding:7px 10px;border-radius:7px;cursor:pointer;font-size:.8rem;font-weight:600;transition:all .14s;display:inline-flex;align-items:center;justify-content:center;gap:4px;user-select:none}
button:hover{background:#253c63;border-color:var(--cyan)}
button:active{transform:translateY(1px)}
button.primary{background:#006978;border-color:var(--cyan);color:#fff}
button.primary:hover{background:#008394}
button.success{background:#145a32;border-color:var(--green);color:#fff}
button.warn{background:#6e4700;border-color:var(--amber);color:#fff}
button.danger{background:#6e1c1c;border-color:var(--red);color:#fff}
button.active{background:rgba(0,229,255,.22);border-color:var(--cyan);color:var(--cyan);box-shadow:0 0 10px rgba(0,229,255,.22)}
button.active-dig{background:rgba(0,230,118,.22);border-color:var(--green);color:var(--green);box-shadow:0 0 10px rgba(0,230,118,.25)}

input[type=text],input[type=number],input[type=password],select{background:#091220;border:1px solid #2b446e;color:var(--text);padding:7px 10px;border-radius:7px;font-size:.84rem;outline:none}
input:focus,select:focus{border-color:var(--cyan);box-shadow:0 0 0 2px rgba(0,229,255,.15)}
input[type=range]{accent-color:var(--cyan);cursor:pointer;width:100%}

/* Mode Selector Bar */
.mode-bar{display:grid;grid-template-columns:repeat(7,1fr);gap:6px}
@media(max-width:650px){.mode-bar{grid-template-columns:repeat(4,1fr)}}
.mode-btn{padding:9px 6px;font-size:.84rem;font-weight:800;border-radius:8px;flex-direction:column;gap:2px}
.mode-btn small{font-size:.65rem;font-weight:500;color:var(--muted)}
.mode-btn.active small,.mode-btn.active-dig small{color:inherit}

/* Step tuning buttons */
.tune-grid{display:grid;grid-template-columns:repeat(6,1fr);gap:5px}
.tune-btn{font-family:monospace;font-size:.8rem;padding:7px 4px}
.tune-btn.neg{color:#ff9e80;border-color:#4e2c26}
.tune-btn.pos{color:#80d8ff;border-color:#24455c}

/* Sliders & Controls */
.ctrl-group{background:#0c1628;border:1px solid #1e3150;border-radius:8px;padding:8px 10px;display:flex;flex-direction:column;gap:6px}
.ctrl-hdr{display:flex;justify-content:space-between;align-items:center;font-size:.78rem;color:var(--muted)}
.ctrl-hdr b{color:var(--cyan);font-size:.84rem}

/* Bookmarks & Bands */
.preset-scroll{display:flex;flex-wrap:wrap;gap:6px;max-height:155px;overflow-y:auto;padding-right:2px}
.bm-chip{display:inline-flex;align-items:center;background:#152540;border:1px solid #29436b;border-radius:7px;overflow:hidden;font-size:.77rem}
.bm-chip button.bm-go{background:transparent;border:none;padding:5px 9px;color:var(--text);font-weight:600;border-radius:0}
.bm-chip button.bm-go:hover{background:rgba(0,229,255,.15);color:var(--cyan)}
.bm-chip .bm-freq{color:var(--green);font-family:monospace;margin-left:4px;font-size:.74rem}
.bm-chip .bm-mode{color:var(--amber);font-size:.68rem;margin-left:3px}
.bm-chip button.bm-del{background:rgba(255,82,82,.15);border:none;border-left:1px solid #29436b;color:var(--red);padding:5px 7px;border-radius:0;font-weight:800}
.bm-chip button.bm-del:hover{background:var(--red);color:#fff}

/* Waterfall & LCD */
.wf-wrap{position:relative;background:#040811;border:1px solid #253b61;border-radius:8px;overflow:hidden}
#wfCanvas{width:100%;height:230px;display:block;image-rendering:pixelated;cursor:crosshair}
.wf-axis{display:flex;justify-content:space-between;padding:4px 8px;background:#0a1322;font-size:.72rem;color:var(--muted);font-family:monospace;border-top:1px solid #1c2e4a}
.lcd-wrap{position:relative;background:#040811;border:1px solid #253b61;border-radius:8px;overflow:hidden;display:flex;justify-content:center;align-items:center;min-height:190px}
#lcdImg{width:100%;max-width:480px;height:auto;image-rendering:pixelated;display:block}

/* FT8 / FT4 Conditional Panel */
.ft8-panel{border:1px solid rgba(0,230,118,.45);background:linear-gradient(180deg,#0d1d2f 0%,#0b1626 100%);box-shadow:0 0 22px rgba(0,230,118,.08)}
.ft8-panel.hidden-mode{display:none}
.slot-wrap{display:flex;align-items:center;gap:10px;background:#091322;padding:8px 12px;border-radius:8px;border:1px solid #1e3456}
.slot-bar{flex:1;height:12px;background:#050b14;border-radius:6px;overflow:hidden;border:1px solid #253c61}
.slot-fill{height:100%;width:0%;background:linear-gradient(90deg,var(--cyan),var(--green));transition:width .25s linear}
.slot-fill.decoding{background:linear-gradient(90deg,var(--amber),var(--red))}

/* Traffic Table */
.table-wrap{max-height:290px;overflow-y:auto;border:1px solid #1e3252;border-radius:8px;background:#08111f}
table{width:100%;border-collapse:collapse;font-size:.81rem}
th,td{padding:7px 10px;text-align:left;border-bottom:1px solid #162640;font-family:'Courier New',monospace}
th{background:#12213a;color:var(--cyan);position:sticky;top:0;z-index:1;font-family:sans-serif;font-size:.75rem;text-transform:uppercase}
tr:hover{background:rgba(0,229,255,.06)}
tr.cq-row{background:rgba(0,230,118,.1);color:#9cffc7;font-weight:700}
.call-badge{color:var(--amber);font-weight:700;cursor:pointer;text-decoration:underline dotted}

/* VU meter */
.vu-bar{height:9px;background:#060c17;border:1px solid #243a5e;border-radius:5px;overflow:hidden}
.vu-fill{height:100%;width:0%;background:linear-gradient(90deg,#00e676 0%,#ffab00 75%,#ff5252 100%);transition:width .18s}

/* Collapsible details */
details.card summary{cursor:pointer;font-weight:700;color:var(--cyan);font-size:.9rem;list-style:none;display:flex;justify-content:space-between;align-items:center}
details.card summary::-webkit-details-marker{display:none}
.log-box{background:#060c16;border:1px solid #1d3152;border-radius:8px;padding:8px 10px;height:150px;overflow-y:auto;font-family:monospace;font-size:.75rem;color:#9ec3e6}
</style>
</head>
<body>
<div class="container">

  <!-- HEADER -->
  <header>
    <div class="title-wrap">
      <h1>📻 Máy Thu Vô Tuyến Đa Chế Độ Si4732 <span style="font-size:.82rem;color:var(--amber);font-weight:600">FM • AM • SSB • CW • FT8 • FT4</span></h1>
      <p>Bảng điều khiển thu phát thanh, thông tin đơn biên (SSB) &amp; giải mã tín hiệu số thời gian thực qua Web</p>
    </div>
    <div class="status-bar">
      <span id="wifiBadge" class="badge info"><span class="dot"></span> <span id="wifiText">Wi-Fi...</span></span>
      <span id="syncBadge" class="badge warn"><span class="dot"></span> <span id="syncText">Đồng bộ...</span></span>
      <span class="badge">🕒 UTC: <strong id="utcClock">--:--:--</strong></span>
      <span class="badge" title="Bộ nhớ Heap còn trống">🧠 RAM: <strong id="heapText">--</strong> KB</span>
    </div>
  </header>

  <!-- VFO COCKPIT & S-METER -->
  <section class="vfo-cockpit">
    <div class="vfo-left">
      <div class="vfo-main-row">
        <span class="freq-big" id="freqDisplay">14074.00</span>
        <span class="freq-unit" id="unitDisplay">kHz</span>
        <span class="mode-pill dig" id="modePill">FT8 (USB)</span>
        <span class="band-pill" id="bandPill">20M</span>
      </div>
      <div class="vfo-sub-row">
        <span class="vfo-chip">BFO: <b id="bfoChip">0 Hz</b></span>
        <span class="vfo-chip">Cal Băng: <b id="calChip">0 Hz</b></span>
        <span class="vfo-chip">Băng thông (BW): <b id="bwChip">3.0k</b></span>
        <span class="vfo-chip">Bước dò (Step): <b id="stepChip">1k</b></span>
        <span class="vfo-chip">RF Gain: <b id="agcChip">AGC Auto</b></span>
        <span class="vfo-chip">Audio AVC: <b id="avcChip">48 dB</b></span>
      </div>
    </div>

    <div class="vfo-right">
      <div class="smeter-top">
        <span>📶 Cường độ tín hiệu thu (S-Meter)</span>
        <span id="sLevelText" style="color:var(--green)">S7</span>
      </div>
      <div class="smeter-scale">
        <span>S1</span><span>S3</span><span>S5</span><span>S7</span><span>S9</span><span class="plus">+20</span><span class="plus">+40dB</span>
      </div>
      <div class="smeter-bar-bg">
        <div class="smeter-bar-fill" id="smeterFill"></div>
      </div>
      <div class="sig-metrics">
        <div class="sig-box"><span class="lbl">RSSI</span><span class="val"><span id="rssiVal">0</span> dBµV</span></div>
        <div class="sig-box"><span class="lbl">SNR</span><span class="val"><span id="snrVal">0</span> dB</span></div>
        <div class="sig-box"><span class="lbl">Âm lượng</span><span class="val"><span id="volValTop">35</span>/63</span></div>
        <div class="sig-box"><span class="lbl">Gói FT8/FT4</span><span class="val" id="msgCountTop" style="color:var(--green)">0</span></div>
      </div>
    </div>
  </section>

  <!-- ROW 1: MODE, FREQUENCY TUNING, GAIN & BANDWIDTH -->
  <div class="grid-3">

    <!-- COL 1: MODE & FREQUENCY CONTROL -->
    <div class="card">
      <div class="card-hdr">
        <span class="card-title">🎛️ Chế độ Thu (Mode) &amp; Đặt Tần Số</span>
        <button onclick="sendCtrl('step',{val:1})" title="Đổi bước nhảy tần số trên núm xoay">Bước: <span id="stepBtnTxt">1k</span></button>
      </div>

      <!-- Mode Selector -->
      <div class="lbl-sm">Chọn chế độ giải điều chế (Modulation / Digital Mode):</div>
      <div class="mode-bar">
        <button class="mode-btn" id="mBtnFM" onclick="selectRadioMode('FM')">FM<small>64-108M</small></button>
        <button class="mode-btn" id="mBtnAM" onclick="selectRadioMode('AM')">AM<small>SW/MW</small></button>
        <button class="mode-btn" id="mBtnLSB" onclick="selectRadioMode('LSB')">LSB<small>SSB Dưới</small></button>
        <button class="mode-btn" id="mBtnUSB" onclick="selectRadioMode('USB')">USB<small>SSB Trên</small></button>
        <button class="mode-btn" id="mBtnCW" onclick="selectRadioMode('CW')">CW<small>Morse 0.5k</small></button>
        <button class="mode-btn" id="mBtnFT8" onclick="selectRadioMode('FT8')">FT8<small>Digital 15s</small></button>
        <button class="mode-btn" id="mBtnFT4" onclick="selectRadioMode('FT4')">FT4<small>Digital 7.5s</small></button>
      </div>

      <!-- Direct Frequency Input -->
      <div class="lbl-sm" style="margin-top:2px">Nhập tần số trực tiếp (VD: <code>14074</code>, <code>7074.5</code>, <code>99.9</code>, <code>2155000</code>):</div>
      <div style="display:flex;gap:6px">
        <input type="text" id="freqInput" placeholder="Nhập kHz, MHz..." style="flex:1;font-family:monospace;font-size:.95rem;font-weight:700" onkeydown="if(event.key==='Enter')setDirectFreq()">
        <select id="freqUnitSel" style="width:92px">
          <option value="auto">Tự động</option>
          <option value="khz">kHz</option>
          <option value="mhz">MHz</option>
        </select>
        <button class="primary" onclick="setDirectFreq()">Đặt tần số</button>
      </div>

      <!-- Step +/- Tuning -->
      <div class="lbl-sm">Tăng / Giảm tần số nhanh:</div>
      <div class="tune-grid">
        <button class="tune-btn neg" onclick="tuneStep(-1000)">-1 MHz</button>
        <button class="tune-btn neg" onclick="tuneStep(-100)">-100 kHz</button>
        <button class="tune-btn neg" onclick="tuneStep(-10)">-10 kHz</button>
        <button class="tune-btn neg" onclick="tuneStep(-1)">-1 kHz</button>
        <button class="tune-btn neg" onclick="tuneHz(-100)">-100 Hz</button>
        <button class="tune-btn neg" onclick="tuneHz(-10)">-10 Hz</button>
        <button class="tune-btn pos" onclick="tuneHz(10)">+10 Hz</button>
        <button class="tune-btn pos" onclick="tuneHz(100)">+100 Hz</button>
        <button class="tune-btn pos" onclick="tuneStep(1)">+1 kHz</button>
        <button class="tune-btn pos" onclick="tuneStep(10)">+10 kHz</button>
        <button class="tune-btn pos" onclick="tuneStep(100)">+100 kHz</button>
        <button class="tune-btn pos" onclick="tuneStep(1000)">+1 MHz</button>
      </div>

      <!-- BFO / Clarifier -->
      <div class="ctrl-group" id="bfoGroup">
        <div class="ctrl-hdr">
          <span>Tinh chỉnh BFO / Clarifier (SSB &amp; FT8/FT4):</span>
          <span>BFO: <b id="bfoVal">0 Hz</b> | Cal: <b id="calVal">0 Hz</b></span>
        </div>
        <div class="btn-grid-7">
          <button onclick="sendCtrl('bfo_step',{delta:-100})">-100</button>
          <button onclick="sendCtrl('bfo_step',{delta:-50})">-50</button>
          <button onclick="sendCtrl('bfo_step',{delta:-10})">-10</button>
          <button class="warn" onclick="sendCtrl('bfo_reset')" title="Đặt BFO về 0 Hz">0 Hz</button>
          <button onclick="sendCtrl('bfo_step',{delta:10})">+10</button>
          <button onclick="sendCtrl('bfo_step',{delta:50})">+50</button>
          <button onclick="sendCtrl('bfo_step',{delta:100})">+100</button>
        </div>
      </div>
    </div>

    <!-- COL 2: BANDWIDTH, RF GAIN (AGC/ATTN), AVC & AUDIO -->
    <div class="card">
      <div class="card-hdr">
        <span class="card-title">🎚️ Bandwidth, Gain (AGC/AVC) &amp; Âm Thanh</span>
        <span class="lbl-sm" id="bwActiveLabel">BW: 3.0k</span>
      </div>

      <!-- Filter Bandwidth -->
      <div class="ctrl-group">
        <div class="ctrl-hdr">
          <span>Bộ lọc Băng thông (Filter Bandwidth):</span>
          <button onclick="sendCtrl('bw',{val:1})" style="padding:2px 8px;font-size:.72rem">Đổi BW ⟳</button>
        </div>
        <div class="btn-row" id="bwButtonsRow">
          <!-- Populated dynamically based on FM / AM / SSB mode -->
        </div>
      </div>

      <!-- RF Gain / AGC / Attenuation -->
      <div class="ctrl-group">
        <div class="ctrl-hdr">
          <span>Khuếch đại Cao tần RF (AGC / Suy hao ATTN):</span>
          <b id="agcStatusTxt">AGC Tự động</b>
        </div>
        <div style="display:flex;gap:6px;align-items:center">
          <button id="btnAgcAuto" class="active" onclick="sendCtrl('agc_set',{val:0})">AGC Auto</button>
          <button onclick="sendCtrl('agc',{val:-1})">ATT -</button>
          <input type="range" id="agcSlider" min="0" max="37" value="0" oninput="document.getElementById('agcStatusTxt').textContent=this.value==0?'AGC Tự động':'ATTN '+(this.value-1)+' dB'" onchange="sendCtrl('agc_set',{val:this.value})">
          <button onclick="sendCtrl('agc',{val:1})">ATT +</button>
        </div>
      </div>

      <!-- Audio AVC Gain & Squelch -->
      <div class="ctrl-group">
        <div class="ctrl-hdr">
          <span>Khuếch đại Âm tần (AVC Gain) &amp; Squelch:</span>
          <span>AVC: <b id="avcValTxt">48</b> dB | SQL: <b id="sqlValTxt">0</b></span>
        </div>
        <div style="display:grid;grid-template-columns:1fr 1fr;gap:8px">
          <div style="display:flex;align-items:center;gap:5px">
            <span class="lbl-sm">AVC:</span>
            <input type="range" id="avcSlider" min="12" max="90" step="2" value="48" oninput="document.getElementById('avcValTxt').textContent=this.value" onchange="sendCtrl('avc_set',{val:this.value})">
          </div>
          <div style="display:flex;align-items:center;gap:5px">
            <span class="lbl-sm">SQL:</span>
            <input type="range" id="sqlSlider" min="0" max="60" step="1" value="0" oninput="document.getElementById('sqlValTxt').textContent=this.value" onchange="sendCtrl('squelch',{val:this.value})">
          </div>
        </div>
      </div>

      <!-- Speaker Volume & Live Web Audio -->
      <div class="ctrl-group">
        <div class="ctrl-hdr">
          <span>Âm lượng Loa &amp; Nghe trực tiếp WebSDR:</span>
          <span>Vol: <b id="volText">35</b>/63</span>
        </div>
        <div style="display:flex;gap:6px;align-items:center">
          <button onclick="changeVol(-5)">-</button>
          <input type="range" id="volSlider" min="0" max="63" value="35" oninput="document.getElementById('volText').textContent=this.value" onchange="sendCtrl('volume',{val:this.value})">
          <button onclick="changeVol(5)">+</button>
          <button id="muteBtn" onclick="sendCtrl('mute')">🔊 Loa</button>
          <button id="webAudioBtn" class="primary" onclick="toggleWebAudio()">🎧 Nghe Web: TẮT</button>
        </div>
        <div style="display:flex;align-items:center;gap:8px;margin-top:2px">
          <span class="lbl-sm" style="white-space:nowrap">VU-Meter (GPIO34):</span>
          <div class="vu-bar" style="flex:1"><div class="vu-fill" id="vuFill"></div></div>
          <span id="vuText" style="font-size:.75rem;font-family:monospace;width:36px;text-align:right">0%</span>
        </div>
      </div>
    </div>

    <!-- COL 3: BANDS & BOOKMARKS MANAGER -->
    <div class="card">
      <div class="card-hdr">
        <span class="card-title">🔖 Băng Tần &amp; Kênh Nhớ (Bookmarks)</span>
        <span class="lbl-sm" id="bmCountTxt">Đã lưu: 0</span>
      </div>

      <!-- Quick Band Selector -->
      <div class="lbl-sm">Băng tần Nghiệp dư (Ham HF) &amp; Phát thanh (FM / MW / SW):</div>
      <div class="btn-row">
        <button onclick="selectBandId(22)" id="bBtn22">20M (14.074)</button>
        <button onclick="selectBandId(20)" id="bBtn20">40M (7.074)</button>
        <button onclick="selectBandId(24)" id="bBtn24">15M (21.074)</button>
        <button onclick="selectBandId(26)" id="bBtn26">10M (28.074)</button>
        <button onclick="selectBandId(21)" id="bBtn21">30M (10.136)</button>
        <button onclick="selectBandId(23)" id="bBtn23">17M (18.100)</button>
        <button onclick="selectBandId(25)" id="bBtn25">12M (24.915)</button>
        <button onclick="selectBandId(19)" id="bBtn19">80M (3.573)</button>
        <button onclick="selectBandId(18)" id="bBtn18">160M (1.840)</button>
        <button onclick="selectBandId(0)"  id="bBtn0">FM (64-108M)</button>
        <button onclick="selectBandId(16)" id="bBtn16">MW (531-1701k)</button>
        <button onclick="selectBandId(10)" id="bBtn10">SW 41M</button>
        <button onclick="selectBandId(8)"  id="bBtn8">SW 25M</button>
        <button onclick="selectBandId(6)"  id="bBtn6">SW 19M</button>
        <button onclick="selectBandId(3)"  id="bBtn3">SW 13M</button>
        <button onclick="selectBandId(1)"  id="bBtn1">ALL (0.15-30M)</button>
      </div>

      <!-- Save Bookmark Form -->
      <div class="ctrl-group">
        <div class="ctrl-hdr">
          <span>Lưu tần số đang thu vào bộ nhớ (Bookmark NVS):</span>
        </div>
        <div style="display:flex;gap:6px">
          <input type="text" id="bmNameInput" maxlength="9" placeholder="Tên kênh (tối đa 9 ký tự)" style="flex:1">
          <button class="success" onclick="saveCurrentBookmark()">+ Lưu Bookmark</button>
        </div>
      </div>

      <!-- Saved Bookmarks & Quick Presets List -->
      <div class="lbl-sm">Kênh nhớ đã lưu &amp; Tần số thông dụng (Nhấn để chuyển nhanh):</div>
      <div class="preset-scroll" id="bookmarksContainer">
        <!-- Populated by loadBookmarks() -->
      </div>
    </div>

  </div>

  <!-- ROW 2: REAL-TIME AUDIO WATERFALL -->
  <div class="card">
    <div class="card-hdr">
      <span class="card-title">🌊 Thác Nước Phổ Âm Thanh (Audio Waterfall 200–2600 Hz)</span>
      <div style="display:flex;gap:6px;align-items:center;flex-wrap:wrap">
        <span id="wfHoverFreq" style="font-family:monospace;font-size:.78rem;color:var(--amber)">--- Hz</span>
        <select id="wfPalette" style="padding:3px 6px;font-size:.75rem">
          <option value="0">Màu: WebSDR Classic</option>
          <option value="1">Màu: Emerald / Cyan</option>
          <option value="2">Màu: Lửa Nhiệt (Amber)</option>
        </select>
        <span class="lbl-sm" style="margin-left:4px">Màn hình thiết bị:</span>
        <button id="lcdMode0" onclick="sendCtrl('view',{mode:0})" style="padding:4px 8px;font-size:.74rem">Chia đôi</button>
        <button id="lcdMode1" onclick="sendCtrl('view',{mode:1})" style="padding:4px 8px;font-size:.74rem">Thác nước</button>
        <button id="lcdMode2" onclick="sendCtrl('view',{mode:2})" style="padding:4px 8px;font-size:.74rem">Giải mã</button>
        <button onclick="fetchWaterfall()" style="padding:4px 8px;font-size:.75rem">Tải lại thác nước</button>
      </div>
    </div>
    <div class="wf-wrap">
      <canvas id="wfCanvas" width="720" height="220"></canvas>
      <div class="wf-axis">
        <span>200 Hz</span><span>700 Hz</span><span>1200 Hz</span><span>1700 Hz</span><span>2200 Hz</span><span>2600 Hz</span>
      </div>
    </div>
  </div>

  <!-- ROW 3: CONDITIONAL FT8 / FT4 DIGITAL DECODER PANEL -->
  <div class="card ft8-panel" id="ft8DecoderCard">
    <div class="card-hdr">
      <div style="display:flex;align-items:center;gap:10px;flex-wrap:wrap">
        <span class="card-title" style="color:var(--green);font-size:1rem">🛰️ Bộ Giải Mã Tín Hiệu Số <span id="digTitleMode">FT8 (Chu kỳ 15s)</span></span>
        <span class="badge ok" id="decodeStatusBadge">Đang lắng nghe trên nhịp 15s</span>
      </div>
      <div class="btn-row">
        <button class="primary" onclick="sendCtrl('decode')">⚡ Chạy giải mã test</button>
        <button onclick="sendCtrl('sync')">⏱️ Đồng bộ nhịp :00s</button>
        <button onclick="syncBrowserTime()">🌐 Đồng bộ giờ UTC</button>
        <button onclick="sendCtrl('save_cal')" title="Lưu độ lệch BFO hiện tại vào chuẩn thạch anh của băng tần">💾 Lưu Cal Băng</button>
      </div>
    </div>

    <!-- Slot Progress Bar -->
    <div class="slot-wrap">
      <strong id="slotText" style="font-family:monospace;font-size:.9rem;min-width:135px;color:var(--cyan)">Nhịp FT8: 0s / 15s</strong>
      <div class="slot-bar"><div class="slot-fill" id="slotFill"></div></div>
      <span id="lastDurText" style="font-size:.78rem;color:var(--muted);min-width:145px;text-align:right">Chu kỳ giải mã tự động</span>
    </div>

    <!-- Traffic Filter & Actions -->
    <div style="display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:8px">
      <div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap">
        <span class="lbl-sm">Nhật ký gói tin FT8/FT4 đã giải mã:</span>
        <input type="text" id="callFilter" placeholder="Lọc Callsign / Grid (VD: CQ, XV, 3W...)" style="padding:4px 9px;font-size:.78rem;width:210px" oninput="renderTrafficTable()">
        <label style="font-size:.78rem;color:var(--green);display:inline-flex;align-items:center;gap:4px;cursor:pointer">
          <input type="checkbox" id="cqOnlyChk" onchange="renderTrafficTable()"> Chỉ hiện CQ
        </label>
      </div>
      <div class="btn-row">
        <button onclick="copyTrafficLog()" style="padding:5px 10px;font-size:.76rem">📋 Sao chép nhật ký</button>
        <button class="danger" onclick="sendCtrl('clear_traffic');trafficCache=[];renderTrafficTable()" style="padding:5px 10px;font-size:.76rem">🗑️ Xóa danh sách</button>
      </div>
    </div>

    <!-- Decoded Messages Table -->
    <div class="table-wrap">
      <table>
        <thead>
          <tr>
            <th>UTC</th>
            <th>SNR</th>
            <th>DT</th>
            <th>Tần số Audio</th>
            <th>Hô hiệu (Callsign)</th>
            <th>Nội dung bản tin / Grid Locator</th>
          </tr>
        </thead>
        <tbody id="trafficBody">
          <tr><td colspan="6" style="text-align:center;color:var(--muted);padding:18px">Đang lắng nghe tín hiệu FT8/FT4 (Chưa có bản tin mới)</td></tr>
        </tbody>
      </table>
    </div>
  </div>

  <!-- Banner shown when FT8/FT4 panel is hidden in FM/AM/SSB Voice mode -->
  <div class="card" id="voiceModeBanner" style="display:none;flex-direction:row;justify-content:space-between;align-items:center;padding:10px 15px;background:#0e1b30">
    <span style="font-size:.84rem;color:var(--muted)">🎙️ Máy thu đang ở chế độ thoại / phát thanh (<b id="voiceModeName" style="color:var(--cyan)">FM/AM/SSB</b>) — Bảng giải mã FT8/FT4 đã thu gọn để thác nước chạy liên tục mượt mà.</span>
    <div class="btn-row">
      <button class="success" onclick="selectRadioMode('FT8')">Bật chế độ FT8</button>
      <button onclick="selectRadioMode('FT4')">Bật chế độ FT4</button>
      <button onclick="forceShowFt8=!forceShowFt8;updateFt8Visibility()">👁️ Hiện/Ẩn bảng FT8</button>
    </div>
  </div>

  <!-- ROW 4: WIFI & SYSTEM LOGS (COLLAPSIBLE) -->
  <details class="card">
    <summary>
      <span>📶 Quản Lý Mạng Wi-Fi (8 Mạng Lưu Trữ) &amp; Nhật Ký Hệ Thống</span>
      <span class="lbl-sm">Nhấn để mở rộng ▾</span>
    </summary>
    <div class="grid-2" style="margin-top:10px">
      <div style="display:flex;flex-direction:column;gap:8px">
        <div style="display:flex;justify-content:space-between;align-items:center">
          <span class="lbl-sm">Quét mạng 2.4GHz &amp; Cấu hình 8 khe Wi-Fi ưu tiên:</span>
          <div class="btn-row">
            <button onclick="scanWifi()">🔍 Quét Wi-Fi</button>
            <button onclick="loadWifiList()">🔄 Tải danh sách</button>
          </div>
        </div>
        <div id="scanResults" style="font-size:.76rem;color:var(--cyan);display:none"></div>
        <div id="wifiSlots" style="display:grid;grid-template-columns:1fr 1fr;gap:6px"></div>
        <div class="btn-row">
          <button class="primary" onclick="saveWifiConfig(false)">💾 Lưu Wi-Fi</button>
          <button class="success" onclick="saveWifiConfig(true)">⚡ Lưu &amp; Kết nối ngay</button>
        </div>
      </div>
      <div style="display:flex;flex-direction:column;gap:8px">
        <div style="display:flex;justify-content:space-between;align-items:center">
          <span class="lbl-sm">Nhật ký hệ thống (System Logs):</span>
          <button onclick="fetchLogs()" style="padding:3px 8px;font-size:.74rem">Làm mới</button>
        </div>
        <div class="log-box" id="sysLogBox">Đang tải nhật ký...</div>
      </div>
    </div>
  </details>

</div>

<script>
let curStatus = { mode_id: 2, dig_mode: 0, band_id: 22, bw_idx: 4, freq_khz: "14074.00", unit: "kHz" };
let trafficCache = [];
let bookmarksCache = [];
let forceShowFt8 = false;
let webAudioOn = false;
let audioTimer = null;
let audioCtx = null;

const BUILTIN_PRESETS = [
  { name: "20M FT8", freq: "14074", unit: "khz", mode: "FT8" },
  { name: "40M FT8", freq: "7074",  unit: "khz", mode: "FT8" },
  { name: "15M FT8", freq: "21074", unit: "khz", mode: "FT8" },
  { name: "10M FT8", freq: "28074", unit: "khz", mode: "FT8" },
  { name: "30M FT8", freq: "10136", unit: "khz", mode: "FT8" },
  { name: "17M FT8", freq: "18100", unit: "khz", mode: "FT8" },
  { name: "80M FT8", freq: "3573",  unit: "khz", mode: "FT8" },
  { name: "20M FT4", freq: "14080", unit: "khz", mode: "FT4" },
  { name: "40M FT4", freq: "7047.5",unit: "khz", mode: "FT4" },
  { name: "15M FT4", freq: "21140", unit: "khz", mode: "FT4" },
  { name: "VOV1 FM", freq: "100.0", unit: "mhz", mode: "FM"  },
  { name: "VOV3 FM", freq: "102.7", unit: "mhz", mode: "FM"  },
  { name: "VOV GT",  freq: "91.0",  unit: "mhz", mode: "FM"  },
  { name: "BPM Time",freq: "15000", unit: "khz", mode: "AM"  }
];

const BW_OPTIONS = {
  FM:  ["Auto", "110k", "84k", "60k", "40k"],
  AM:  ["1.0k", "1.8k", "2.0k", "2.5k", "3.0k", "4.0k", "6.0k"],
  SSB: ["0.5k", "1.0k", "1.2k", "2.2k", "3.0k", "4.0k"]
};

function updateUtcClock() {
  const now = new Date();
  const hh = String(now.getUTCHours()).padStart(2,'0');
  const mm = String(now.getUTCMinutes()).padStart(2,'0');
  const ss = String(now.getUTCSeconds()).padStart(2,'0');
  document.getElementById('utcClock').textContent = `${hh}:${mm}:${ss}`;
}
setInterval(updateUtcClock, 500);
updateUtcClock();

async function sendCtrl(action, params = {}) {
  const body = new URLSearchParams({ action, ...params });
  try {
    await fetch('/api/control', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: body.toString()
    });
    setTimeout(fetchStatus, 80);
  } catch (e) { console.error(e); }
}

async function selectRadioMode(target) {
  if (target === 'FT8') {
    await sendCtrl('dig_mode', { val: 0 });
  } else if (target === 'FT4') {
    await sendCtrl('dig_mode', { val: 1 });
  } else if (target === 'FM') {
    await sendCtrl('mode', { mode: 0 });
  } else if (target === 'LSB') {
    await sendCtrl('mode', { mode: 1 });
  } else if (target === 'USB') {
    await sendCtrl('mode', { mode: 2 });
  } else if (target === 'AM') {
    await sendCtrl('mode', { mode: 3 });
  } else if (target === 'CW') {
    await sendCtrl('mode', { mode: 2 });
    await sendCtrl('bw_set', { idx: 0 }); // 0.5k narrow CW filter
  }
}

async function setDirectFreq() {
  const raw = document.getElementById('freqInput').value.trim().replace(',', '.');
  const unit = document.getElementById('freqUnitSel').value;
  if (!raw) return;
  const val = parseFloat(raw);
  if (isNaN(val) || val <= 0) return;
  await sendCtrl('freq', { freq: val, unit: unit === 'auto' ? '' : unit });
  document.getElementById('freqInput').value = '';
}

function tuneStep(deltaKhz) {
  sendCtrl('tune', { delta: deltaKhz });
}

function tuneHz(deltaHz) {
  if (curStatus.mode_id === 1 || curStatus.mode_id === 2) {
    sendCtrl('bfo_step', { delta: deltaHz });
  } else {
    sendCtrl('tune', { delta: deltaHz > 0 ? 1 : -1 });
  }
}

function changeVol(delta) {
  const sl = document.getElementById('volSlider');
  let v = parseInt(sl.value || '35', 10) + delta;
  if (v < 0) v = 0; if (v > 63) v = 63;
  sl.value = v;
  document.getElementById('volText').textContent = v;
  sendCtrl('volume', { val: v });
}

function selectBandId(bId) {
  sendCtrl('band', { id: bId });
}

function renderBandwidthButtons(modeId, activeIdx) {
  const row = document.getElementById('bwButtonsRow');
  const key = (modeId === 0) ? 'FM' : ((modeId === 3) ? 'AM' : 'SSB');
  const list = BW_OPTIONS[key];
  const sig = `${key}_${activeIdx}`;
  if (row.dataset.sig === sig) return;
  row.dataset.sig = sig;
  row.innerHTML = list.map((lbl, idx) =>
    `<button class="${idx === activeIdx ? 'active' : ''}" onclick="sendCtrl('bw_set',{idx:${idx}})">${lbl}</button>`
  ).join('');
}

function updateFt8Visibility() {
  const isDig = (curStatus.mode_id === 2 && (curStatus.dig_mode === 0 || curStatus.dig_mode === 1));
  const card = document.getElementById('ft8DecoderCard');
  const banner = document.getElementById('voiceModeBanner');
  if (isDig || forceShowFt8) {
    card.classList.remove('hidden-mode');
  } else {
    card.classList.add('hidden-mode');
  }
  banner.style.display = isDig ? 'none' : 'flex';
}

async function fetchStatus() {
  try {
    const r = await fetch('/api/status');
    if (!r.ok) return;
    const d = await r.json();
    curStatus = d;

    document.getElementById('freqDisplay').textContent = d.freq_khz;
    document.getElementById('unitDisplay').textContent = d.unit || 'kHz';
    document.getElementById('bandPill').textContent = d.band;

    const isFt8 = (d.mode_id === 2 && d.dig_mode === 0);
    const isFt4 = (d.mode_id === 2 && d.dig_mode === 1);
    const isCw  = (d.mode_id === 2 && d.dig_mode === 2 && d.bw_idx === 0);

    const mp = document.getElementById('modePill');
    if (isFt8) { mp.textContent = 'FT8 (USB)'; mp.className = 'mode-pill dig'; }
    else if (isFt4) { mp.textContent = 'FT4 (USB)'; mp.className = 'mode-pill dig'; }
    else if (isCw) { mp.textContent = 'CW (USB 0.5k)'; mp.className = 'mode-pill'; }
    else { mp.textContent = d.mode; mp.className = 'mode-pill'; }

    document.getElementById('voiceModeName').textContent = mp.textContent;

    // Mode active buttons
    ['FM','AM','LSB','USB','CW','FT8','FT4'].forEach(m => {
      const el = document.getElementById('mBtn' + m);
      if (el) el.className = 'mode-btn';
    });
    if (isFt8) document.getElementById('mBtnFT8').className = 'mode-btn active-dig';
    else if (isFt4) document.getElementById('mBtnFT4').className = 'mode-btn active-dig';
    else if (isCw) document.getElementById('mBtnCW').className = 'mode-btn active';
    else if (d.mode_id === 0) document.getElementById('mBtnFM').className = 'mode-btn active';
    else if (d.mode_id === 1) document.getElementById('mBtnLSB').className = 'mode-btn active';
    else if (d.mode_id === 2) document.getElementById('mBtnUSB').className = 'mode-btn active';
    else if (d.mode_id === 3) document.getElementById('mBtnAM').className = 'mode-btn active';

    // Chips
    document.getElementById('bfoChip').textContent = `${d.bfo} Hz`;
    document.getElementById('calChip').textContent = `${d.cal} Hz`;
    document.getElementById('bwChip').textContent  = d.bw;
    document.getElementById('stepChip').textContent = d.step;
    document.getElementById('stepBtnTxt').textContent = d.step;
    document.getElementById('bwActiveLabel').textContent = `BW: ${d.bw}`;
    document.getElementById('bfoVal').textContent = `${d.bfo} Hz`;
    document.getElementById('calVal').textContent = `${d.cal} Hz`;

    const agcLabel = (d.agc === 0) ? 'AGC Tự động' : `ATTN ${d.att_db} dB`;
    document.getElementById('agcChip').textContent = agcLabel;
    document.getElementById('agcStatusTxt').textContent = agcLabel;
    document.getElementById('btnAgcAuto').className = (d.agc === 0) ? 'active' : '';
    document.getElementById('agcSlider').value = d.agc;

    document.getElementById('avcChip').textContent = `${d.avc} dB`;
    document.getElementById('avcValTxt').textContent = d.avc;
    document.getElementById('avcSlider').value = d.avc;

    document.getElementById('sqlValTxt').textContent = d.squelch;
    document.getElementById('sqlSlider').value = d.squelch;

    // S-Meter
    document.getElementById('rssiVal').textContent = d.rssi;
    document.getElementById('snrVal').textContent = d.snr;
    document.getElementById('volValTop').textContent = d.volume;
    document.getElementById('msgCountTop').textContent = d.msg_count;
    const sLv = d.s_level || 0;
    const sTxt = sLv <= 9 ? `S${sLv}` : `S9+${(sLv - 9) * 10}dB`;
    document.getElementById('sLevelText').textContent = `${sTxt} (${d.rssi} dBµV)`;
    const sPct = Math.min(100, Math.round((sLv / 13) * 100));
    document.getElementById('smeterFill').style.width = `${sPct}%`;

    // Volume & Mute
    document.getElementById('volSlider').value = d.volume;
    document.getElementById('volText').textContent = d.volume;
    const muteBtn = document.getElementById('muteBtn');
    muteBtn.textContent = d.mute ? '🔇 Đã tắt loa' : '🔊 Loa: BẬT';
    muteBtn.className = d.mute ? 'danger' : '';

    // VU Meter
    const vu = Math.min(100, d.audio_level || 0);
    document.getElementById('vuFill').style.width = `${vu}%`;
    document.getElementById('vuText').textContent = `${vu}%`;

    // Wi-Fi & Sync badges
    document.getElementById('wifiText').textContent = `${d.ssid} (${d.ip})`;
    document.getElementById('heapText').textContent = Math.round((d.heap_free || 0) / 1024);
    const sb = document.getElementById('syncBadge');
    sb.className = d.synced ? 'badge ok' : 'badge warn';
    document.getElementById('syncText').textContent = d.synced ? `Đồng bộ ${d.sync_src}` : 'Chưa đồng bộ giờ';

    // LCD view buttons
    [0,1,2].forEach(i => {
      const b = document.getElementById('lcdMode' + i);
      if (b) b.className = (d.lcd_view === i) ? 'active' : '';
    });

    // Highlight active band button
    document.querySelectorAll('[id^="bBtn"]').forEach(b => b.classList.remove('active'));
    const actBandBtn = document.getElementById('bBtn' + d.band_id);
    if (actBandBtn) actBandBtn.classList.add('active');

    renderBandwidthButtons(d.mode_id, d.bw_idx);
    updateFt8Visibility();

    // FT8 / FT4 slot bar
    const slotMax = isFt4 ? 8 : 15;
    const slotCur = d.slot % slotMax;
    const modeLabel = isFt4 ? 'FT4' : 'FT8';
    document.getElementById('digTitleMode').textContent = isFt4 ? 'FT4 (Chu kỳ 7.5s)' : 'FT8 (Chu kỳ 15s)';
    document.getElementById('slotText').textContent = `Nhịp ${modeLabel}: ${slotCur}s / ${slotMax}s`;
    const sf = document.getElementById('slotFill');
    sf.style.width = `${Math.min(100, Math.round((slotCur / slotMax) * 100))}%`;
    sf.className = d.decoding ? 'slot-fill decoding' : 'slot-fill';
    document.getElementById('decodeStatusBadge').textContent = d.decoding
      ? `⚡ Đang giải mã ${modeLabel}...`
      : `Đang thu ${modeLabel} (${d.freq_khz} kHz)`;
    if (d.last_duration_ms > 0) {
      document.getElementById('lastDurText').textContent = `Giải mã xong trong ${d.last_duration_ms} ms`;
    }
  } catch (e) {}
}

// Bookmarks
async function loadBookmarks() {
  try {
    const r = await fetch('/api/bookmarks');
    if (r.ok) bookmarksCache = await r.json();
  } catch (e) {}
  renderBookmarks();
}

function renderBookmarks() {
  const box = document.getElementById('bookmarksContainer');
  document.getElementById('bmCountTxt').textContent = `Đã lưu: ${bookmarksCache.length}/24`;
  let html = '';

  // NVS saved bookmarks first
  bookmarksCache.forEach(bm => {
    const fDisp = bm.mode === 'FM'
      ? (bm.freq_hz / 1000000).toFixed(2) + 'M'
      : (bm.freq_hz / 1000).toFixed(1) + 'k';
    html += `<div class="bm-chip" title="Kênh nhớ NVS #${bm.slot + 1}">
      <button class="bm-go" onclick="sendCtrl('bookmark_tune',{slot:${bm.slot}})">⭐ ${bm.name}<span class="bm-freq">${fDisp}</span><span class="bm-mode">${bm.mode}</span></button>
      <button class="bm-del" onclick="deleteBookmark(${bm.slot})" title="Xóa kênh nhớ">×</button>
    </div>`;
  });

  // Builtin presets
  BUILTIN_PRESETS.forEach(p => {
    html += `<div class="bm-chip">
      <button class="bm-go" onclick="tunePreset('${p.freq}','${p.unit}','${p.mode}')">${p.name}<span class="bm-freq">${p.freq}${p.unit==='mhz'?'M':'k'}</span><span class="bm-mode">${p.mode}</span></button>
    </div>`;
  });

  box.innerHTML = html;
}

async function tunePreset(freq, unit, mode) {
  if (mode === 'FT8') await sendCtrl('dig_mode', { val: 0 });
  else if (mode === 'FT4') await sendCtrl('dig_mode', { val: 1 });
  else if (mode === 'FM') await sendCtrl('mode', { mode: 0 });
  else if (mode === 'AM') await sendCtrl('mode', { mode: 3 });
  else if (mode === 'LSB') await sendCtrl('mode', { mode: 1 });
  else if (mode === 'USB') await sendCtrl('mode', { mode: 2 });
  await sendCtrl('freq', { freq, unit });
}

async function saveCurrentBookmark() {
  const inp = document.getElementById('bmNameInput');
  const name = inp.value.trim() || `${curStatus.band} ${curStatus.freq_khz}`;
  await sendCtrl('bookmark_save', { name: name.substring(0, 9), slot: -1 });
  inp.value = '';
  setTimeout(loadBookmarks, 150);
}

async function deleteBookmark(slot) {
  await sendCtrl('bookmark_del', { slot });
  setTimeout(loadBookmarks, 150);
}

// Waterfall Canvas
const wfCanvas = document.getElementById('wfCanvas');
const wfCtx = wfCanvas.getContext('2d');

wfCanvas.addEventListener('mousemove', e => {
  const rect = wfCanvas.getBoundingClientRect();
  const ratio = Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width));
  const hz = Math.round(200 + ratio * 2400);
  document.getElementById('wfHoverFreq').textContent = `${hz} Hz`;
});

function colorMap(v, pal) {
  if (pal === 1) { // Emerald / Cyan
    return [Math.min(255, v * 0.4), Math.min(255, v * 1.2), Math.min(255, 30 + v)];
  }
  if (pal === 2) { // Amber Fire
    return [Math.min(255, v * 1.4), Math.min(255, Math.max(0, (v - 60) * 1.3)), Math.max(0, (v - 160) * 2)];
  }
  // Classic WebSDR
  if (v < 45) return [4, 10, 20 + v * 2];
  if (v < 110) return [0, (v - 45) * 3.5, 120 + (v - 45) * 2];
  if (v < 185) return [(v - 110) * 3.2, 230, 255 - (v - 110) * 3];
  return [255, 255 - (v - 185) * 2.2, (v - 185) * 3];
}

async function fetchWaterfall() {
  try {
    const r = await fetch('/api/waterfall');
    if (!r.ok) return;
    const buf = new Uint8Array(await r.arrayBuffer());
    if (buf.length < 36 * 120) return;

    const rows = 36, cols = 120;
    const img = wfCtx.createImageData(wfCanvas.width, wfCanvas.height);
    const pal = parseInt(document.getElementById('wfPalette').value || '0', 10);
    const w = wfCanvas.width, h = wfCanvas.height;

    for (let y = 0; y < h; y++) {
      const srcR = Math.min(rows - 1, Math.floor((y / h) * rows));
      for (let x = 0; x < w; x++) {
        const srcC = Math.min(cols - 1, Math.floor((x / w) * cols));
        const val = buf[srcR * cols + srcC];
        const [cr, cg, cb] = colorMap(val, pal);
        const idx = (y * w + x) * 4;
        img.data[idx] = cr;
        img.data[idx + 1] = cg;
        img.data[idx + 2] = cb;
        img.data[idx + 3] = 255;
      }
    }
    wfCtx.putImageData(img, 0, 0);
  } catch (e) {}
}

// Traffic Log
async function fetchTraffic() {
  try {
    const r = await fetch('/api/ft8/traffic');
    if (!r.ok) return;
    trafficCache = await r.json();
    renderTrafficTable();
  } catch (e) {}
}

function renderTrafficTable() {
  const tb = document.getElementById('trafficBody');
  const filter = (document.getElementById('callFilter').value || '').trim().toUpperCase();
  const cqOnly = document.getElementById('cqOnlyChk').checked;

  const filtered = trafficCache.filter(item => {
    const text = `${item.call} ${item.msg}`.toUpperCase();
    if (cqOnly && !text.includes('CQ ')) return false;
    if (filter && !text.includes(filter)) return false;
    return true;
  });

  if (filtered.length === 0) {
    tb.innerHTML = `<tr><td colspan="6" style="text-align:center;color:var(--muted);padding:16px">Đang lắng nghe tín hiệu FT8/FT4 (Chưa có bản tin khớp bộ lọc)</td></tr>`;
    return;
  }

  tb.innerHTML = filtered.map(e => {
    const isCq = (e.msg || '').startsWith('CQ ');
    const snrColor = e.snr >= 0 ? 'var(--green)' : (e.snr >= -12 ? 'var(--cyan)' : 'var(--muted)');
    return `<tr class="${isCq ? 'cq-row' : ''}">
      <td>${e.time}</td>
      <td style="color:${snrColor};font-weight:700">${e.snr > 0 ? '+' + e.snr : e.snr} dB</td>
      <td>${Number(e.dt).toFixed(1)}s</td>
      <td>${e.freq} Hz</td>
      <td><span class="call-badge" onclick="document.getElementById('callFilter').value='${e.call}';renderTrafficTable()">${e.call || '--'}</span></td>
      <td>${e.msg}</td>
    </tr>`;
  }).join('');
}

function copyTrafficLog() {
  const lines = trafficCache.map(e => `${e.time}\t${e.snr}dB\t${e.dt}s\t${e.freq}Hz\t${e.call}\t${e.msg}`).join('\n');
  navigator.clipboard.writeText(lines || 'Chưa có dữ liệu FT8/FT4');
}

async function syncBrowserTime() {
  const ep = Math.floor(Date.now() / 1000);
  try { await fetch('/api/timesync?epoch=' + ep); } catch (e) {}
}

// Live WebSDR Audio Player
function toggleWebAudio() {
  webAudioOn = !webAudioOn;
  const btn = document.getElementById('webAudioBtn');
  if (webAudioOn) {
    btn.textContent = '🎧 Nghe Web: BẬT';
    btn.className = 'success';
    if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    playAudioLoop();
  } else {
    btn.textContent = '🎧 Nghe Web: TẮT';
    btn.className = 'primary';
    if (audioTimer) clearTimeout(audioTimer);
  }
}

async function playAudioLoop() {
  if (!webAudioOn) return;
  try {
    const r = await fetch('/api/audio.wav?t=' + Date.now());
    if (r.ok) {
      const ab = await r.arrayBuffer();
      const audioBuf = await audioCtx.decodeAudioData(ab);
      const src = audioCtx.createBufferSource();
      src.buffer = audioBuf;
      src.connect(audioCtx.destination);
      src.start();
    }
  } catch (e) {}
  if (webAudioOn) audioTimer = setTimeout(playAudioLoop, 180);
}

// Wi-Fi & Logs
async function loadWifiList() {
  try {
    const r = await fetch('/api/wifi/list');
    if (!r.ok) return;
    const d = await r.json();
    const box = document.getElementById('wifiSlots');
    box.innerHTML = d.networks.map((n, i) => `
      <div style="display:flex;gap:4px;align-items:center">
        <span class="lbl-sm" style="width:22px">#${i+1}</span>
        <input type="text" id="wSsid${i}" value="${n.ssid}" placeholder="SSID ${i+1}" style="flex:1;padding:5px">
        <input type="password" id="wPass${i}" value="${n.pass}" placeholder="Mật khẩu" style="width:110px;padding:5px">
      </div>
    `).join('');
  } catch (e) {}
}

async function scanWifi() {
  const el = document.getElementById('scanResults');
  el.style.display = 'block';
  el.textContent = 'Đang quét mạng Wi-Fi 2.4GHz...';
  try {
    const r = await fetch('/api/wifi/scan');
    const list = await r.json();
    el.innerHTML = 'Mạng tìm thấy (nhấn để điền vào ô trống): ' + list.map(a =>
      `<button style="padding:2px 7px;margin:2px;font-size:.73rem" onclick="fillEmptyWifi('${a.ssid}')">${a.ssid} (${a.rssi}dBm)</button>`
    ).join('');
  } catch (e) { el.textContent = 'Lỗi quét Wi-Fi'; }
}

function fillEmptyWifi(ssid) {
  for (let i = 0; i < 8; i++) {
    const el = document.getElementById('wSsid' + i);
    if (el && (!el.value || el.value === ssid)) { el.value = ssid; break; }
  }
}

async function saveWifiConfig(reconnect) {
  const p = new URLSearchParams();
  for (let i = 0; i < 8; i++) {
    p.append(`ssid${i+1}`, document.getElementById(`wSsid${i}`).value.trim());
    p.append(`pass${i+1}`, document.getElementById(`wPass${i}`).value);
  }
  if (reconnect) p.append('reconnect', '1');
  await fetch('/api/wifi/save', { method: 'POST', headers: {'Content-Type':'application/x-www-form-urlencoded'}, body: p.toString() });
  alert(reconnect ? 'Đã lưu và đang kết nối lại Wi-Fi!' : 'Đã lưu danh sách Wi-Fi!');
}

async function fetchLogs() {
  try {
    const r = await fetch('/api/logs');
    if (!r.ok) return;
    const list = await r.json();
    const box = document.getElementById('sysLogBox');
    box.innerHTML = list.map(l => `<div>${l}</div>`).join('');
    box.scrollTop = box.scrollHeight;
  } catch (e) {}
}

// Strictly sequential polling loop (never sends concurrent HTTP requests to ESP32)
let pollTickCount = 0;
async function sequentialPollLoop() {
  try {
    await fetchStatus();
    if (!webAudioOn) {
      await fetchWaterfall();
    }
    if (pollTickCount % 2 === 0) {
      await fetchTraffic();
    }
    pollTickCount++;
  } catch (e) {}
  setTimeout(sequentialPollLoop, 850);
}

window.addEventListener('DOMContentLoaded', async () => {
  await syncBrowserTime();
  await fetchStatus();
  await loadBookmarks();
  await fetchWaterfall();
  await fetchTraffic();
  loadWifiList();
  setTimeout(sequentialPollLoop, 800);
});
</script>
</body>
</html>
)rawliteral";

#endif
