/*
 * File: WiFiSkullController.cpp
 * Purpose: Implements Wi-Fi/AP startup, embedded web pages, HTTP endpoints, uploads, and manual eye commands.
 *
 * MIT License
 *
 * Copyright (c) 2026 Doug Brann https://github.com/TheDougMiester
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "WiFiSkullController.h"
#include "ConfigManager.h"
#include "PinDefinitions.h"
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <SD_MMC.h>
#include "SDMutex.h"
#include <esp_task_wdt.h>

uint32_t WiFiSkullController::nReceivedValue = 0;
bool WiFiSkullController::hasNewCommand = false;
uint8_t WiFiSkullController::lastTrack = 0;
bool WiFiSkullController::isPlaying = false;
bool WiFiSkullController::isLiveMic = false;
uint8_t WiFiSkullController::lastVolume = 25;
char WiFiSkullController::lastFileName[64] = "0000.wav";
char WiFiSkullController::lastStatusMsg[128] = "Ready - Select a track";
WebServer* WiFiSkullController::server = nullptr;
static bool sdMounted = false;
int WiFiSkullController::eyeManualX = 0;
int WiFiSkullController::eyeManualY = 0;
int WiFiSkullController::eyeManualLid = -1;
int WiFiSkullController::eyeManualPupil = -1;
uint32_t WiFiSkullController::eyeManualUntil = 0;
bool WiFiSkullController::eyeManualActive = false;


const char HTML_MAIN[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no">
<title>Talking Skull</title>
<style>
*{box-sizing:border-box;margin:0;padding:0;font-family:-apple-system,system-ui,Segoe UI,Roboto,Arial}
body{background:#0f0f10;color:#e8e8e8;display:flex;justify-content:center;min-height:100vh;padding:8px}
.container{width:100%;max-width:440px;background:#1c1c1e;border-radius:24px;padding:12px 12px 16px;box-shadow:0 10px 40px rgba(0,0,0,.6);border:1px solid #2c2c2e;display:flex;flex-direction:column;max-height:95vh}
h1{font-size:22px;text-align:center;margin-bottom:4px}
.sub{font-size:11px;text-align:center;color:#8e8e93;margin-bottom:8px}
.msg{width:100%;min-height:44px;background:#000;border-radius:12px;border:1px solid #333;padding:8px 12px;font-size:13px;font-weight:600;display:flex;align-items:center;justify-content:center;text-align:center;word-break:break-word;transition:all 0.1s}
.msg.ok{color:#ffd60a;border-color:#5a4a00;background:#1a1600;transform:scale(1.02)}
.msg.err{color:#ff453a;border-color:#7a1f1a;background:#1e0a09;animation:shake 0.2s}
.msg.idle{color:#8e8e93}
@keyframes shake{0%,100%{transform:translateX(0)}25%{transform:translateX(-4px)}75%{transform:translateX(4px)}}
.fileList{width:100%;flex:1;min-height:120px;max-height:45vh;overflow-y:auto;background:#0a0a0a;border-radius:12px;border:1px solid #2c2c2e;margin:8px 0;padding:4px;-webkit-overflow-scrolling:touch}
.fileItem{padding:8px 10px;border-radius:8px;margin:3px 0;background:#1c1c1e;cursor:pointer;display:flex;justify-content:space-between;align-items:flex-start;gap:8px;transition:transform 0.05s, background 0.1s, box-shadow 0.1s}
.fileItem:hover{background:#2c2c2e}
.fileItem:active{transform:scale(0.97);background:#3a3a3c;box-shadow:inset 0 2px 4px rgba(0,0,0,0.6)}
.fileItem.active{background:#ff9f0a;color:#000;font-weight:700;transform:scale(1.01);box-shadow:0 2px 8px rgba(255,159,10,0.4)}
.fileItem.active:active{transform:scale(0.97)}
.fileItem .left{flex:1;min-width:0;word-break:break-word;display:flex;flex-direction:column}
.fileItem .right{flex-shrink:0;display:flex;align-items:center;gap:6px;margin-left:8px}
.fileItem span.sz{font-size:10px;color:#8e8e93;white-space:nowrap}
.fileItem span.src{font-size:8px;color:#30d158}
.btn{width:100%;height:48px;border-radius:14px;border:none;font-size:16px;font-weight:700;cursor:pointer;margin-top:6px;transition:transform 0.08s, box-shadow 0.08s, background 0.1s, opacity 0.1s;box-shadow:0 3px 0 rgba(0,0,0,0.4), 0 4px 8px rgba(0,0,0,0.3);user-select:none;-webkit-tap-highlight-color:transparent;touch-action:manipulation}
.btn:active{transform:translateY(2px) scale(0.97);box-shadow:0 1px 0 rgba(0,0,0,0.4), 0 1px 3px rgba(0,0,0,0.3);opacity:0.9}
#playBtn{background:#ffd60a;color:#000}
#playBtn:active{background:#ffcc00}
#stopBtn{background:#ff453a;color:#fff}
#stopBtn:active{background:#cc0000}
.micRow{display:flex;gap:8px;margin-top:8px}
.micBtn{flex:1;height:52px;border-radius:14px;border:none;font-size:14px;font-weight:700;cursor:pointer;transition:transform 0.08s, box-shadow 0.08s, background 0.1s;box-shadow:0 3px 0 rgba(0,0,0,0.4);user-select:none;-webkit-tap-highlight-color:transparent;touch-action:manipulation}
.micBtn:active{transform:translateY(2px) scale(0.96);box-shadow:0 1px 0 rgba(0,0,0,0.4)}
#micBtn{background:#30d158;color:#000}
#micBtn.rec{background:#ff453a;animation:pulse 1s infinite}
#muteBtn{background:#3a3a3c;color:#ffd60a}
#muteBtn:active{background:#2c2c2e}
.volRow{display:flex;align-items:center;gap:10px;margin-top:8px;background:#0a0a0a;padding:8px 10px;border-radius:12px;border:1px solid #2c2c2e}
.volRow input{flex:1;accent-color:#ffd60a;height:6px}
.footer{margin-top:6px;text-align:center;font-size:10px;color:#636366;flex-shrink:0}
.linkBtn{display:block;text-align:center;margin-top:8px;color:#0a84ff;text-decoration:none;font-size:12px;flex-shrink:0}
@keyframes pulse{0%{opacity:1}50%{opacity:.6}100%{opacity:1}}
.delBtn{background:#ff3b30;color:#fff;border:none;border-radius:6px;padding:4px 8px;cursor:pointer;transition:transform 0.05s, background 0.1s;flex-shrink:0;box-shadow:0 2px 0 rgba(0,0,0,0.3)}
.delBtn:active{transform:scale(0.85) translateY(1px);background:#cc0000;box-shadow:0 1px 0 rgba(0,0,0,0.3)}
</style></head><body>
<div class="container">
<h1>💀 Skull - I2S</h1><div class="sub">WAV • Live Mic • FFT Dual-Band • SD+LittleFS • MAX98357A</div>
<div id="msg" class="msg idle">Ready - Select a track</div>
<div id="fileList" class="fileList">Loading...</div>
<button id="playBtn" class="btn" onclick="playSelected()">▶ PLAY SELECTED</button>
<div class="micRow">
<button id="stopBtn" class="micBtn" style="background:#ff453a;color:#fff" onclick="doStop()">■ STOP</button>
<button id="pauseBtn" class="micBtn" style="background:#ff9f0a;color:#000" onclick="doPause()">⏸ PAUSE</button>
</div>
<div class="micRow">
<button id="micBtn" class="micBtn" onpointerdown="beginMicHold(event)" onpointerup="endMicHold(event)" onpointercancel="endMicHold(event)">🎤 HOLD TO TALK (Live Mic)</button>
<button id="muteBtn" class="micBtn" onclick="toggleMute()">🔇 MUTE MIC</button>
</div>
<div class="volRow" title="These go to eleven">🔊 <input type="range" id="vol" min="0" max="11" value="9" oninput="onVol(this.value)" onchange="doVol(this.value)"><span id="volVal">9</span></div>
<a class="linkBtn" href="/upload">📁 Manage Files</a>
<a class="linkBtn" href="/eyes">👁️ Eyes Control</a>
<div class="footer"><span id="ip"></span> • <span id="status"></span></div>
</div>
<script>
let files=[]; let selected=null; let isMuted=false; let ws=null; let audioCtx=null; let processor=null; let micStream=null;
let micHeld=false; let liveMicRequested=false;
function showMsg(t,c){let m=document.getElementById('msg');m.textContent=t;m.className='msg '+(c||'idle');}
function clearSelection(){document.querySelectorAll('.fileItem').forEach(x=>x.classList.remove('active'));selected=null;}
function loadList(){fetch('/list').then(r=>r.json()).then(j=>{files=j.files||[];let el=document.getElementById('fileList');el.innerHTML='';if(files.length==0){el.innerHTML='<div style="padding:20px;text-align:center;color:#636366">No WAVs - put .wav in SD root or /wav + go to Upload page</div>';return;}files.forEach(f=>{let d=document.createElement('div');d.className='fileItem';d.innerHTML=`<div class="left"><span>${f.name}</span><span><span class="sz">${(f.size/1024).toFixed(0)}KB</span> <span class="src">${f.src}</span></span></div>`;d.onclick=()=>{document.querySelectorAll('.fileItem').forEach(x=>x.classList.remove('active'));d.classList.add('active');selected=f.name;showMsg('Selected: '+f.name+' ('+f.src+')','idle');};el.appendChild(d);});}).catch(()=>{document.getElementById('fileList').innerHTML='List error';});}

function playSelected(){
  if(!selected){showMsg('Select a file first','err');return;}
  let b=document.getElementById('playBtn');
  b.style.transform='translateY(2px) scale(0.97)';setTimeout(()=>b.style.transform='',150);
  fetch('/play?file='+encodeURIComponent(selected)).then(r=>r.json()).then(j=>{
    if(j.ok)showMsg('▶ Playing '+j.file,'ok');
    else showMsg('Error: '+j.error,'err');
  }).catch(()=>showMsg('Network error','err'));
}
let isPaused=false;
function doStop(){
  let b=document.getElementById('stopBtn');
  b.style.transform='translateY(2px) scale(0.97)';setTimeout(()=>b.style.transform='',150);
  micHeld=false;
  stopMic(false); // /stop below is the one authoritative stop command
  fetch('/stop').then(r=>r.json()).then(j=>{showMsg('■ Stopped','idle'); clearSelection(); isPaused=false; document.getElementById('pauseBtn').textContent='⏸ PAUSE';}).catch(()=>showMsg('Network error','err'));
}
function doPause(){
  let b=document.getElementById('pauseBtn');
  b.style.transform='translateY(2px) scale(0.97)';setTimeout(()=>b.style.transform='',150);
  if(!isPaused){
    fetch('/pause').then(r=>r.json()).then(j=>{if(j.ok){showMsg('⏸ Paused '+j.file,'idle'); isPaused=true; b.textContent='▶ RESUME';}});
  } else {
    fetch('/resume').then(r=>r.json()).then(j=>{if(j.ok){showMsg('▶ Resumed '+j.file,'ok'); isPaused=false; b.textContent='⏸ PAUSE';}});
  }
}
// Spinal Tap display scale: UI 0..11 maps onto the unchanged firmware 0..30 scale.
function volToInternal(v){return Math.round(Number(v)*30/11);}
function volToEleven(v){return Math.round(Number(v)*11/30);}
function onVol(v){document.getElementById('volVal').textContent=v;}
function doVol(v){
  const shown=Math.max(0,Math.min(11,Number(v)));
  const internal=volToInternal(shown);
  fetch('/volume?vol='+internal).then(r=>r.json()).then(j=>{
    if(j.ok)showMsg('Volume '+shown+(shown===11?' — These go to eleven.':''),'idle');
  });
}
function toggleMute(){isMuted=!isMuted;let b=document.getElementById('muteBtn');b.style.transform='translateY(2px) scale(0.96)';setTimeout(()=>b.style.transform='',120);document.getElementById('muteBtn').textContent=isMuted?'🔊 UNMUTE MIC':'🔇 MUTE MIC'; if(isMuted&&processor) processor.port.postMessage({mute:true}); if(!isMuted&&processor) processor.port.postMessage({mute:false}); fetch('/volume?micmute='+(isMuted?1:0));}
function beginMicHold(e){
  if(e.pointerType==='mouse' && e.button!==0) return;
  if(micHeld) return;
  micHeld=true;
  try{e.currentTarget.setPointerCapture(e.pointerId);}catch{}
  startMic();
}
function endMicHold(e){
  // A pointerup caused by merely moving across this button must be a no-op.
  if(!micHeld) return;
  micHeld=false;
  stopMic(true);
}
async function startMic(){
  if(!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia){
    micHeld=false;
    showMsg('Mic error: getUserMedia requires HTTPS or localhost. Use http://192.168.1.90 with Chrome flag --unsafely-treat-insecure-origin-as-secure=http://192.168.1.90,http://talkingskull.local or Firefox about:config media.devices.insecure.enabled=true','err');
    return;
  }
  if(isMuted){micHeld=false;showMsg('Mic muted - unmute first','err');return;}
  try{
    let stream=await navigator.mediaDevices.getUserMedia({audio:{sampleRate:16000,channelCount:1,echoCancellation:true,noiseSuppression:true}});
    if(!micHeld){stream.getTracks().forEach(t=>t.stop());return;}
    let context=new (window.AudioContext||window.webkitAudioContext)({sampleRate:16000});
    await context.audioWorklet.addModule('/mic-processor.js');
    if(!micHeld){stream.getTracks().forEach(t=>t.stop());context.close();return;}
    micStream=stream;
    audioCtx=context;
    let src=audioCtx.createMediaStreamSource(stream);
    processor=new AudioWorkletNode(audioCtx,'mic-processor');
    processor.port.onmessage=e=>{ if(ws && ws.readyState===1 && !isMuted){ ws.send(e.data); } };
    src.connect(processor); processor.connect(audioCtx.destination);
    const socket=new WebSocket('ws://'+location.hostname+':81/');
    ws=socket;
    socket.binaryType='arraybuffer';
    socket.onopen=()=>{
      if(!micHeld){try{socket.close();}catch{} return;}
      liveMicRequested=true;
      document.getElementById('micBtn').classList.add('rec');
      showMsg('🎤 Live Mic - Talking...','ok');
      fetch('/play?live=1');
    };
    socket.onclose=()=>{
      if(ws===socket) ws=null;
      document.getElementById('micBtn').classList.remove('rec');
      if(liveMicRequested){liveMicRequested=false;fetch('/play?live=0').catch(()=>{});}
      showMsg('Live Mic ended','idle');
    };
    socket.onerror=()=>showMsg('WS error - check WebSocket :81','err');
  }catch(e){
    micHeld=false;
    stopMic(false);
    if(e.name==='NotAllowedError') showMsg('Mic blocked - allow mic permission in browser','err');
    else if(e.name==='NotFoundError') showMsg('No mic found on device','err');
    else if(location.protocol!=='https:' && location.hostname!=='localhost' && location.hostname!=='127.0.0.1') showMsg('Mic requires HTTPS or localhost. For HTTP use Chrome flag --unsafely-treat-insecure-origin-as-secure='+location.origin+' or Firefox media.devices.insecure.enabled=true. Error: '+e.message,'err');
    else showMsg('Mic error: '+e.name+': '+e.message,'err');
  }
}
function stopMic(notifyServer=true){
  const shouldNotify=notifyServer && liveMicRequested;
  liveMicRequested=false;
  if(processor){try{processor.disconnect();}catch{} processor=null;}
  if(audioCtx){try{audioCtx.close();}catch{} audioCtx=null;}
  if(micStream){micStream.getTracks().forEach(t=>t.stop()); micStream=null;}
  const socket=ws; ws=null;
  if(socket){try{socket.close();}catch{}}
  document.getElementById('micBtn').classList.remove('rec');
  if(shouldNotify) fetch('/play?live=0').catch(()=>{});
}
window.addEventListener('pointerup',()=>{if(micHeld){micHeld=false;stopMic(true);}});
window.addEventListener('pointercancel',()=>{if(micHeld){micHeld=false;stopMic(true);}});
window.addEventListener('blur',()=>{if(micHeld){micHeld=false;stopMic(true);}});
function pollStatus(){
  fetch('/status').then(r=>r.json()).then(j=>{
    document.getElementById('ip').textContent=j.ip||'';
    const shownVol=volToEleven(j.vol ?? 25);
    document.getElementById('vol').value=shownVol;
    document.getElementById('volVal').textContent=shownVol;
    if(j.playing){
      document.getElementById('status').textContent=j.msg||'Playing '+j.file;
      if(j.msg && !j.msg.includes('Ready')) showMsg(j.msg,j.msg.includes('error')?'err':'ok');
    } else {
      // Show finished message even if file selected (bug fix)
      if(j.msg && j.msg.toLowerCase().includes('finished')){
        document.getElementById('status').textContent=j.msg;
        showMsg(j.msg,'idle'); // always show finished, keep selection yellow so user can replay
      } else if(j.msg && j.msg.toLowerCase().includes('stopped')){
        clearSelection();
        showMsg(j.msg,'idle');
        document.getElementById('status').textContent=j.msg;
      } else if(j.msg && j.msg.includes('error')){
        showMsg(j.msg,'err');
        document.getElementById('status').textContent=j.msg;
      } else {
        document.getElementById('status').textContent=j.msg||'Stopped';
        if(j.msg && j.msg.includes('Ready')) showMsg(j.msg,'idle');
      }
    }
  }).catch(()=>{});
}
loadList(); pollStatus(); setInterval(pollStatus,1500);
</script></body></html>
)rawliteral";

const char HTML_UPLOAD[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Upload WAVs</title><style>
body{background:#0f0f10;color:#e8e8e8;font-family:system-ui;padding:8px;display:flex;justify-content:center}
.container{width:100%;max-width:460px;background:#1c1c1e;border-radius:20px;padding:16px;border:1px solid #2c2c2e;display:flex;flex-direction:column;max-height:95vh}
h2{text-align:center;margin-bottom:8px}
.sub{font-size:11px;color:#8e8e93;text-align:center;margin-bottom:10px}
.msg{width:100%;min-height:44px;background:#000;border-radius:10px;border:1px solid #333;padding:10px;font-size:13px;font-weight:600;display:flex;align-items:center;justify-content:center;text-align:center;word-break:break-word;margin-bottom:8px;transition:transform 0.1s}
.msg.ok{color:#30d158;border-color:#1a4a2a;background:#0a1a0f}
.msg.err{color:#ff453a;border-color:#7a1f1a;background:#1e0a09}
.msg.idle{color:#8e8e93}
.drop{border:2px dashed #3a3a3c;border-radius:12px;padding:20px;text-align:center;background:#0a0a0a;margin:10px 0;transition:all 0.15s}
.drop.dragover{border-color:#ffd60a;background:#1a1600;transform:scale(1.02)}
.fileList{width:100%;flex:1;min-height:100px;max-height:40vh;overflow-y:auto;background:#0a0a0a;border-radius:12px;padding:6px;margin:10px 0;border:1px solid #2c2c2e}
.fileItem{display:flex;justify-content:space-between;align-items:flex-start;gap:8px;padding:10px 12px;margin:4px 0;background:#1c1c1e;border-radius:8px;font-size:13px;transition:transform 0.05s, background 0.1s}
.fileItem:active{transform:scale(0.97);background:#2c2c2e}
.btn{width:100%;height:44px;border-radius:12px;border:none;font-weight:700;margin-top:8px;cursor:pointer;transition:transform 0.08s, box-shadow 0.08s, background 0.1s, opacity 0.1s;box-shadow:0 3px 0 rgba(0,0,0,0.4), 0 4px 8px rgba(0,0,0,0.3)}
.btn:active{transform:translateY(2px) scale(0.95);box-shadow:0 1px 0 rgba(0,0,0,0.4), 0 1px 3px rgba(0,0,0,0.3);opacity:0.9}
#browseLabel{display:inline-block;background:#2c2c2e;color:#fff;padding:10px 18px;border-radius:10px;cursor:pointer;border:1px solid #3a3a3c;transition:all 0.1s;box-shadow:0 3px 0 rgba(0,0,0,0.4)}
#browseLabel:active{background:#ffd60a;color:#000;transform:translateY(2px) scale(0.95);box-shadow:0 1px 0 rgba(0,0,0,0.4)}
.delBtn{background:#ff3b30;color:#fff;border:none;border-radius:6px;padding:4px 8px;cursor:pointer;transition:transform 0.05s, background 0.1s;flex-shrink:0;box-shadow:0 2px 0 rgba(0,0,0,0.3)}
.delBtn:active{transform:scale(0.85) translateY(1px);background:#cc0000;box-shadow:0 1px 0 rgba(0,0,0,0.3)}
.info{font-size:11px;color:#636366;text-align:center;margin-top:6px}
a{color:#0a84ff;text-decoration:none;display:block;text-align:center;margin-top:12px;font-size:13px;flex-shrink:0}
input[type=file]{display:none}
</style></head><body>
<div class="container"><h2 style="font-size:15px">📁 Upload .wav files to SD Card</h2>
<p class="sub">WAV only, mono/stereo 16kHz-48kHz 16-bit. Stereo: LEFT=voice for FFT jaw, RIGHT=music mix.</p>
<div id="msg" class="msg idle">Ready - pick WAVs</div>
<div class="drop" id="dropZone">
<label id="browseLabel" for="f">📂 Browse WAVs</label>
<input type="file" id="f" accept=".wav" multiple>
<div class="info"><span id="free"></span></div></div>
<div id="list" class="fileList"></div>
<div style="margin-top:14px;padding-top:10px;border-top:1px solid #2c2c2e">
<h3 style="font-size:14px;text-align:center;margin-bottom:6px">⚙️ Config Upload (skull.conf)</h3>
<p class="sub">Upload skull.conf to SD card - edit on PC, no IDE - used after reboot</p>
<div class="drop" id="dropConf">
<label id="browseConfLabel" for="fc">📄 Browse skull.conf</label>
<input type="file" id="fc" accept=".conf">
<div class="info">Will save to SD /skull.conf and LittleFS /skull.conf</div>
</div>
</div>
<a href="/">← Back to Player</a></div>
<script>
let uploadQueue=[]; let uploading=false;
function showMsg(t,c){let m=document.getElementById('msg');m.textContent=t;m.className='msg '+(c||'idle');}
function loadList(){
  fetch('/list').then(r=>r.json()).then(j=>{
    let el=document.getElementById('list');el.innerHTML='';
    document.getElementById('free').textContent=`Free LittleFS: ${(j.free/1024).toFixed(0)}KB / ${(j.total/1024).toFixed(0)}KB | SD: ${j.sd ? (j.sd.free/1024/1024).toFixed(1)+'GB free' : 'no SD'}`;
    (j.files||[]).forEach(f=>{
      let d=document.createElement('div');d.className='fileItem';
      d.innerHTML=`<div style="flex:1;min-width:0;word-break:break-word"><span>${f.name}</span> <small>(${(f.size/1024).toFixed(0)}KB)</small></div><div style="flex-shrink:0"><button class="delBtn" onclick="delFile('${encodeURIComponent(f.name)}')">🗑</button></div>`;
      el.appendChild(d);
    });
    if((j.files||[]).length==0) el.innerHTML='<div style="padding:12px;text-align:center;color:#636366">No WAVs yet - SD in root or pick files above</div>';
  }).catch(e=>showMsg('List error: '+e,'err'));
}
async function handleFiles(files){
  if(!files || files.length==0){showMsg('Pick canceled','idle');return;}
  uploadQueue = Array.from(files);
  showMsg(`Picked ${uploadQueue.length} file(s) - starting upload...`,'idle');
  for(let file of uploadQueue){
    if(!file.name.toLowerCase().endsWith('.wav')){showMsg('Skip '+file.name+' - Only .wav allowed','err'); continue;}
    if(file.size>8*1024*1024){showMsg('Skip '+file.name+' >8MB - convert to mono 16k','err'); continue;}
    let fd=new FormData(); fd.append('file',file);
    showMsg(`Uploading ${file.name} ${(file.size/1024).toFixed(0)}KB...`,'idle');
    try{
      let r=await fetch('/upload',{method:'POST',body:fd});
      let text=await r.text();
      let j; try{ j=JSON.parse(text);}catch{ throw new Error(text.substring(0,200));}
      if(j.ok){showMsg(`✓ Uploaded ${j.file} (${(j.size/1024).toFixed(0)}KB) to ${j.dest}`,'ok');}
      else {showMsg(`Error ${file.name}: ${j.error}`,'err');}
    } catch(e){showMsg(`Network/Error ${file.name}: ${e} - Try mono 16k <2MB`,'err');}
    await new Promise(r=>setTimeout(r,300));
  }
  loadList();
  document.getElementById('f').value='';
}
document.getElementById('f').addEventListener('change', (e)=>{ handleFiles(e.target.files); });
let dropZone=document.getElementById('dropZone');
dropZone.addEventListener('dragover', e=>{ e.preventDefault(); dropZone.classList.add('dragover'); });
dropZone.addEventListener('dragleave', e=>{ dropZone.classList.remove('dragover'); });
dropZone.addEventListener('drop', e=>{
  e.preventDefault(); dropZone.classList.remove('dragover');
  if(e.dataTransfer.files) handleFiles(e.dataTransfer.files);
});
function delFile(encName){let name=decodeURIComponent(encName);if(!confirm('Delete '+name+' from LittleFS? SD delete on PC'))return;fetch('/delete?file='+encName).then(r=>r.json()).then(j=>{if(j.ok){showMsg('Deleted '+name,'ok'); loadList();}else showMsg('Error: '+(j.error||'delete failed'),'err');}).catch(e=>showMsg('Error: '+e,'err'));}

async function handleConfFile(file){
  if(!file) return;
  if(file.name.toLowerCase()!=='skull.conf'){ showMsg('Only skull.conf allowed for config','err'); return; }
  let fd=new FormData(); fd.append('file',file);
  showMsg('Uploading skull.conf...','idle');
  try{
    let r=await fetch('/upload?conf=1',{method:'POST',body:fd});
    let text=await r.text();
    let j; try{ j=JSON.parse(text);}catch{ throw new Error(text.substring(0,300));}
    if(j.ok) showMsg('✓ Config uploaded '+j.file+' -> '+j.dest+' (reboot to use)','ok');
    else showMsg('Error '+file.name+': '+j.error,'err');
  }catch(e){ showMsg('Network/Error '+e,'err'); }
}
document.getElementById('fc').addEventListener('change', e=>{ if(e.target.files[0]) handleConfFile(e.target.files[0]); e.target.value=''; });
let dropConf=document.getElementById('dropConf');
if(dropConf){
  dropConf.addEventListener('dragover', e=>{ e.preventDefault(); dropConf.classList.add('dragover'); });
  dropConf.addEventListener('dragleave', e=>{ dropConf.classList.remove('dragover'); });
  dropConf.addEventListener('drop', e=>{ e.preventDefault(); dropConf.classList.remove('dragover'); if(e.dataTransfer.files[0]) handleConfFile(e.dataTransfer.files[0]); });
}
loadList();

</script></body></html>
)rawliteral";


const char HTML_EYES[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no">
<title>Eyes Control</title><style>
body{background:#0a0a0a;color:#e8e8e8;font-family:system-ui;display:flex;justify-content:center;min-height:100vh;padding:10px;touch-action:none}
.container{width:100%;max-width:460px;background:#1c1c1e;border-radius:20px;padding:14px;border:1px solid #2c2c2e}
h2{text-align:center;margin-bottom:6px}
.sub{font-size:11px;color:#8e8e93;text-align:center;margin-bottom:8px}
.row{display:flex;gap:10px;align-items:stretch}
.pad{flex:1;aspect-ratio:1/1;max-width:320px;margin:0 auto;background:radial-gradient(circle at center,#222,#000);border-radius:20px;border:2px solid #333;position:relative;touch-action:none;user-select:none}
.knob{width:40px;height:40px;background:#ff3b30;border-radius:50%;position:absolute;left:50%;top:50%;transform:translate(-50%,-50%);box-shadow:0 4px 10px rgba(0,0,0,.6);pointer-events:none;transition:left 0.05s,top 0.05s}
.vcol{width:70px;display:flex;flex-direction:column;align-items:center;gap:6px;background:#0a0a0a;border-radius:12px;border:1px solid #2c2c2e;padding:8px 6px}
.vcol label{font-size:10px;color:#8e8e93}
.vcol input{writing-mode:vertical-lr;direction:rtl;width:6px;flex:1;accent-color:#ff3b30}
.palette{width:96px;justify-content:center;align-items:flex-start;padding:10px}
.palette .paletteTitle{width:100%;text-align:center;font-size:10px;color:#8e8e93;margin-bottom:3px}
.palette label{display:flex;align-items:center;gap:7px;font-size:12px;color:#e8e8e8;cursor:pointer}
.palette input{writing-mode:initial;direction:ltr;width:auto;height:auto;flex:none;margin:0;accent-color:#30d158}
.btnRow{display:flex;gap:8px;margin-top:10px}
.btn{flex:1;height:48px;border-radius:12px;border:none;font-weight:700;cursor:pointer;box-shadow:0 3px 0 rgba(0,0,0,.4)}
.btn:active{transform:translateY(2px) scale(0.97)}
#openBtn{background:#30d158;color:#000}
#closeBtn{background:#1c1c1e;color:#fff;border:1px solid #333}
#centerBtn{background:#2c2c2e;color:#ffd60a}
.msg{margin-top:8px;min-height:32px;background:#000;border-radius:8px;display:flex;align-items:center;justify-content:center;font-size:12px;color:#8e8e93}
a{color:#0a84ff;text-align:center;display:block;margin-top:10px;text-decoration:none}
input[type=range].h{width:100%;accent-color:#ff3b30;height:6px}
</style></head><body><div class="container">
<h2>👁️ Eyes Control</h2><div class="sub">Touch pad = look • Right buttons = iris color • Bottom = eyelid open/close • Open page = manual</div>
<div class="row">
<div id="pad" class="pad"><div id="knob" class="knob"></div></div>
<div class="vcol palette"><div class="paletteTitle">IRIS COLOR</div><label><input type="radio" name="eyeColor" value="0" checked> GREEN</label><label><input type="radio" name="eyeColor" value="1"> BLUE</label><label><input type="radio" name="eyeColor" value="2"> RED</label><label><input type="radio" name="eyeColor" value="3"> HAZEL</label></div>
</div>
<div class="btnRow"><button id="openBtn" class="btn" onclick="setLidPercent(100)">👁️ OPEN 100%</button><button id="centerBtn" class="btn" onclick="centerEyes()">🎯 CENTER</button><button id="closeBtn" class="btn" onclick="setLidPercent(0)">😑 CLOSED 0%</button></div>
<div style="margin-top:10px"><label style="font-size:11px">Eyelid: <span id="lidVal">70%</span> (0%=closed left, 100%=open right)</label><input type="range" id="lid" class="h" min="0" max="100" value="70"></div>
<div id="msg" class="msg">Ready - drag pad</div>
<a href="/">← Back to Player</a>
</div>
<script>
let pad=document.getElementById('pad'), knob=document.getElementById('knob'), msg=document.getElementById('msg');
let dragging=false, lastX=0, lastY=0, lastLidPercent=70, lastEyeColor=0;
function showMsg(t){msg.textContent=t;}
function lidPercentToRaw(p){ // 0%=closed lid 240 left, 100%=open lid 0 right
  return Math.round(240 - (p*240/100));
}
function rawLidToPercent(lid){ return Math.round((240-lid)*100/240); }
// Preserve the existing pupil field in the wire protocol as a compact color
// selector: 15=green, 48=blue, 80=red, 112=hazel.
function eyeColorToRaw(c){ return c===1 ? 48 : (c===2 ? 80 : (c===3 ? 112 : 15)); }
function eyeColorName(c){ return c===1 ? 'BLUE' : (c===2 ? 'RED' : (c===3 ? 'HAZEL' : 'GREEN')); }
// Keep at most one eye-control request in flight. Pointer events can arrive
// much faster than the ESP32 HTTP server can consume them; parallel fetches
// can otherwise complete out of order and briefly replay stale/center values.
let pendingEyeCommand=null, eyeCommandInFlight=false, lastEyeCommandMs=0;
function sendEye(x,y,lidPercent,colorCode){
  pendingEyeCommand={x,y,lidPercent,colorCode}; // coalesce to newest position
  flushEyeCommand();
}
async function flushEyeCommand(){
  if(eyeCommandInFlight) return;
  eyeCommandInFlight=true;
  while(pendingEyeCommand){
    const c=pendingEyeCommand; pendingEyeCommand=null;
    const wait=Math.max(0,35-(performance.now()-lastEyeCommandMs));
    if(wait>0) await new Promise(r=>setTimeout(r,wait));
    const lidRaw=lidPercentToRaw(c.lidPercent);
    const colorRaw=eyeColorToRaw(c.colorCode);
    const url=`/eye?x=${c.x}&y=${c.y}&lid=${lidRaw}&pupil=${colorRaw}`;
    try{
      const j=await fetch(url).then(r=>r.json());
      lastEyeCommandMs=performance.now();
      if(j.ok) showMsg(`x=${c.x} y=${c.y} lid ${c.lidPercent}% ${eyeColorName(c.colorCode)} manual`);
    }catch(e){}
  }
  eyeCommandInFlight=false;
  // Cover a command queued between the loop test and clearing the flag.
  if(pendingEyeCommand) flushEyeCommand();
}
function setPosFromEvent(e){
  let rect=pad.getBoundingClientRect();
  let clientX = e.touches ? e.touches[0].clientX : e.clientX;
  let clientY = e.touches ? e.touches[0].clientY : e.clientY;
  let x = clientX - rect.left; let y = clientY - rect.top;
  x = Math.max(0,Math.min(rect.width,x)); y = Math.max(0,Math.min(rect.height,y));
  // Send native GC9A01 center coordinates with one-pixel resolution.
  // X: left=-120, right=+119. Y is positive upward: top=+120, bottom=-119.
  let lx = Math.round(x/rect.width*239)-120;
  let ly = 120-Math.round(y/rect.height*239);
  lastX=lx; lastY=ly;
  knob.style.left = (x/rect.width*100)+'%'; knob.style.top = (y/rect.height*100)+'%';
  sendEye(lx,ly,lastLidPercent,lastEyeColor);
}
pad.addEventListener('pointerdown', e=>{ dragging=true; pad.setPointerCapture(e.pointerId); setPosFromEvent(e); });
pad.addEventListener('pointermove', e=>{ if(dragging) setPosFromEvent(e); });
pad.addEventListener('pointerup', e=>{ dragging=false; });
pad.addEventListener('touchmove', e=>{ e.preventDefault(); }, {passive:false});
function centerEyes(){ lastX=0; lastY=0; knob.style.left='50%'; knob.style.top='50%'; sendEye(0,0,lastLidPercent,lastEyeColor); showMsg('Centered'); }
function setLidPercent(p){ lastLidPercent=parseInt(p); document.getElementById('lidVal').textContent=p+'%'; document.getElementById('lid').value=p; sendEye(lastX,lastY,lastLidPercent,lastEyeColor); }
function setEyeColor(c){
  lastEyeColor=parseInt(c);
  sendEye(lastX,lastY,lastLidPercent,lastEyeColor);
}
document.getElementById('lid').addEventListener('input', e=>{ document.getElementById('lidVal').textContent=e.target.value+'%'; lastLidPercent=parseInt(e.target.value); });
document.getElementById('lid').addEventListener('change', e=>{ setLidPercent(e.target.value); });
document.querySelectorAll('input[name="eyeColor"]').forEach(r=>{
  r.addEventListener('change', e=>{ if(e.target.checked) setEyeColor(e.target.value); });
});
// Enter manual mode when page opens, then publish the checked default color.
fetch('/eye?manual=1').then(()=>{setEyeColor(0);showMsg('Manual control - choose GREEN, BLUE, RED, or HAZEL');}).catch(()=>{});
window.addEventListener('beforeunload', ()=>{ navigator.sendBeacon('/eye?auto=1'); });
window.addEventListener('pagehide', ()=>{ navigator.sendBeacon('/eye?auto=1'); });
</script></body></html>
)rawliteral";




const char MIC_PROCESSOR_JS[] PROGMEM = R"rawliteral(
class MicProcessor extends AudioWorkletProcessor {
  constructor(){super(); this.muted=false; this.port.onmessage=e=>{if(e.data.mute!==undefined) this.muted=e.data.mute;};}
  process(inputs){
    let input=inputs[0];
    if(input.length>0 && !this.muted){
      let ch=input[0];
      let int16=new Int16Array(ch.length);
      for(let i=0;i<ch.length;i++){let s=Math.max(-1,Math.min(1,ch[i])); int16[i]=s<0?s*0x8000:s*0x7FFF;}
      this.port.postMessage(int16.buffer,[int16.buffer]);
    }
    return true;
  }
}
registerProcessor('mic-processor',MicProcessor);
)rawliteral";

String WiFiSkullController::formatFileName(uint8_t track){ char buf[16]; snprintf(buf,sizeof(buf),"%04d.wav",track); return String(buf); }
bool WiFiSkullController::isValidVolume(int v){ return v>=0 && v<=30; }

void WiFiSkullController::handleRoot(){ server->send_P(200,"text/html",HTML_MAIN); }
void WiFiSkullController::handleUploadPage(){ server->send_P(200,"text/html",HTML_UPLOAD); }
void WiFiSkullController::handleEyesPage(){ server->send_P(200,"text/html",HTML_EYES); }
void WiFiSkullController::handleEyeControl(){
  // Handle manual mode enter/exit
  if(server->hasArg("auto") || server->hasArg("exit")){
    eyeManualActive = false;
    eyeManualUntil = 0;
    eyeManualPupil = -1;
    server->send(200,"application/json","{\"ok\":true,\"auto\":true}");
    
    return;
  }
  if(server->hasArg("manual") || server->hasArg("enter")){
    eyeManualActive = true;
    eyeManualUntil = millis() + 10*60*1000; // 10 min while page open
    server->send(200,"application/json","{\"ok\":true,\"manual\":true}");
    
    return;
  }
  int x = eyeManualX, y = eyeManualY, lid = eyeManualLid, pupil = eyeManualPupil;
  bool hasXY=false;
  if(server->hasArg("x")){ x = server->arg("x").toInt(); hasXY=true; }
  if(server->hasArg("y")){ y = server->arg("y").toInt(); hasXY=true; }
  if(server->hasArg("lid")){ lid = server->arg("lid").toInt(); }
  if(server->hasArg("pupil")){ pupil = server->arg("pupil").toInt(); }
  if(server->hasArg("p")){ pupil = server->arg("p").toInt(); }
  // constrain
  // Manual Goat/Krampus coordinates are native display-center offsets.
  // Neutral center is (0,0); complete pad travel reaches every 240px edge.
  if(x<-120) x=-120; if(x>119) x=119;
  if(y<-119) y=-119; if(y>120) y=120;
  if(lid<-1) lid=-1; if(lid>240) lid=240;
  if(pupil<-1) pupil=-1; if(pupil>112) pupil=112; if(pupil>=0 && pupil<15) pupil=15;
  if(hasXY || server->hasArg("lid") || server->hasArg("pupil") || server->hasArg("p")){
    eyeManualX = x; eyeManualY = y;
    if(server->hasArg("lid")) eyeManualLid = lid;
    if(server->hasArg("pupil") || server->hasArg("p")) eyeManualPupil = pupil;
    eyeManualActive = true;
    eyeManualUntil = millis() + 10*60*1000; // extend while page open
  }
  char resp[160];
  snprintf(resp,sizeof(resp),"{\"ok\":true,\"x\":%d,\"y\":%d,\"lid\":%d,\"pupil\":%d,\"manual\":%d}",eyeManualX,eyeManualY,eyeManualLid,eyeManualPupil,eyeManualActive?1:0);
  server->send(200,"application/json",resp);
}
bool WiFiSkullController::getEyeManual(int &x, int &y, int &lid){
  if(eyeManualActive && (eyeManualUntil==0 || millis() < eyeManualUntil)){
    x = eyeManualX; y = eyeManualY; lid = eyeManualLid;
    return true;
  }
  if(millis() < eyeManualUntil){
    x = eyeManualX; y = eyeManualY; lid = eyeManualLid;
    return true;
  }
  return false;
}
bool WiFiSkullController::getEyeManualFull(int &x, int &y, int &lid, int &pupil, bool &active){
  active = eyeManualActive;
  if(eyeManualActive){
    x=eyeManualX; y=eyeManualY; lid=eyeManualLid; pupil=eyeManualPupil;
    return true;
  }
  if(millis() < eyeManualUntil){
    x=eyeManualX; y=eyeManualY; lid=eyeManualLid; pupil=eyeManualPupil;
    active=true;
    return true;
  }
  return false;
}


static String jsonEscape(const String& input){
  String output;
  output.reserve(input.length()+8);
  for(size_t i=0;i<input.length();++i){
    const char c=input[i];
    switch(c){
      case '\\': output += "\\\\"; break;
      case '"': output += "\\\""; break;
      case '\n': output += "\\n"; break;
      case '\r': output += "\\r"; break;
      case '\t': output += "\\t"; break;
      default:
        if(static_cast<uint8_t>(c) >= 0x20) output += c;
        break;
    }
  }
  return output;
}

void WiFiSkullController::handleList(){
  const size_t total = ConfigManager::isLittleFSReady() ? LittleFS.totalBytes() : 0;
  const size_t used = ConfigManager::isLittleFSReady() ? LittleFS.usedBytes() : 0;
  uint64_t sdTotal=0, sdUsed=0;
  if(sdMounted){
    SDLockGuard lock;
    sdTotal=SD_MMC.totalBytes();
    sdUsed=SD_MMC.usedBytes();
  }

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200,"application/json","");
  char header[256];
  if(sdMounted){
    snprintf(header,sizeof(header),
      "{\"total\":%llu,\"used\":%llu,\"free\":%llu,"
      "\"sd\":{\"total\":%llu,\"used\":%llu,\"free\":%llu},\"files\":[",
      static_cast<unsigned long long>(total),
      static_cast<unsigned long long>(used),
      static_cast<unsigned long long>(total-used),
      static_cast<unsigned long long>(sdTotal),
      static_cast<unsigned long long>(sdUsed),
      static_cast<unsigned long long>(sdTotal-sdUsed));
  } else {
    snprintf(header,sizeof(header),
      "{\"total\":%llu,\"used\":%llu,\"free\":%llu,\"files\":[",
      static_cast<unsigned long long>(total),
      static_cast<unsigned long long>(used),
      static_cast<unsigned long long>(total-used));
  }
  server->sendContent(header);

  bool first=true;
  auto emitFile=[&](const String& rawName,size_t size,const char* source){
    String name=rawName;
    const int slash=name.lastIndexOf('/');
    if(slash>=0) name=name.substring(slash+1);
    if(name.isEmpty() || name.startsWith(".")) return;
    String chunk;
    chunk.reserve(name.length()+96);
    if(!first) chunk += ',';
    first=false;
    chunk += "{\"name\":\"";
    chunk += jsonEscape(name);
    chunk += "\",\"size\":";
    chunk += String(static_cast<unsigned long>(size));
    chunk += ",\"src\":\"";
    chunk += source;
    chunk += "\"}";
    server->sendContent(chunk);
  };

  if(ConfigManager::isLittleFSReady()){
    File root=LittleFS.open("/wav");
    if(!root){ LittleFS.mkdir("/wav"); root=LittleFS.open("/wav"); }
    if(root){
      for(File file=root.openNextFile(); file; file=root.openNextFile()){
        const String name=file.name();
        if(!file.isDirectory() && (name.endsWith(".wav") || name.endsWith(".WAV"))){
          emitFile(name,file.size(),"LittleFS");
        }
        file.close();
        delay(0);
      }
      root.close();
    }
  }

  auto streamSDDirectory=[&](const char* path,const char* source){
    File root;
    {
      SDLockGuard lock;
      root=SD_MMC.open(path);
    }
    if(!root) return;
    for(;;){
      String name;
      size_t size=0;
      bool isDirectory=false;
      bool haveFile=false;
      {
        SDLockGuard lock;
        File file=root.openNextFile();
        if(file){
          haveFile=true;
          name=file.name();
          size=file.size();
          isDirectory=file.isDirectory();
          file.close();
        }
      }
      if(!haveFile) break;
      if(!isDirectory && (name.endsWith(".wav") || name.endsWith(".WAV"))){
        emitFile(name,size,source);
      }
      delay(0); // let the audio task acquire sdMutex between entries
    }
    {
      SDLockGuard lock;
      root.close();
    }
  };

  if(sdMounted){
    streamSDDirectory("/","SD");
    streamSDDirectory("/wav","SD/wav");
  }

  server->sendContent("]}");
  server->sendContent("");
}

static String uploadError = "";
static String uploadLastFile = "";
static size_t uploadLastSize = 0;
static String uploadDest = "";

void WiFiSkullController::handlePlay(){
  if(server->hasArg("live")){
    int live=server->arg("live").toInt();
    if(live==1){
      isLiveMic=true; isPlaying=true;
      snprintf(lastFileName,sizeof(lastFileName),"LIVE MIC");
      snprintf(lastStatusMsg,sizeof(lastStatusMsg),"Live Mic - Talking...");
      pushCommand(WIFI_CMD_LIVE_MIC_START);
      server->send(200,"application/json","{\"ok\":true,\"live\":1,\"file\":\"LIVE MIC\"}");
      return;
    } else {
      // Idempotent live-stop: a stale/spurious browser event must never stop
      // an ordinary WAV track. Only enqueue STOP_LIVE if live mode is active.
      const bool wasLive=isLiveMic;
      if(wasLive){
        isLiveMic=false; isPlaying=false;
        snprintf(lastStatusMsg,sizeof(lastStatusMsg),"Live Mic ended");
        pushCommand(WIFI_CMD_LIVE_MIC_STOP);
      }
      server->send(200,"application/json",
        wasLive ? "{\"ok\":true,\"live\":0}" :
                  "{\"ok\":true,\"live\":0,\"ignored\":true}");
      return;
    }
  }
  if(!server->hasArg("file")){
    server->send(400,"application/json","{\"ok\":false,\"error\":\"Missing file param\"}"); return;
  }
  String file=server->arg("file");
  if(file.indexOf("..")>=0){ server->send(400,"application/json","{\"ok\":false,\"error\":\"Invalid name\"}"); return; }

  String pathSD="/"+file;
  String pathSDWav="/wav/"+file;
  String pathFS="/wav/"+file;
  String pathFSRoot="/"+file;
  bool exists = false;
  if(sdMounted){
    SDLockGuard lock;
    exists = SD_MMC.exists(pathSD) || SD_MMC.exists(pathSDWav);
  }
  if(!exists && ConfigManager::isLittleFSReady()){
    exists = LittleFS.exists(pathFS) || LittleFS.exists(pathFSRoot);
  }

  if(!exists){
    
    server->send(404,"application/json","{\"ok\":false,\"error\":\"File not found on SD or LittleFS - check /list\"}"); return;
  }

  snprintf(lastFileName,sizeof(lastFileName),"%s",file.c_str());
  snprintf(lastStatusMsg,sizeof(lastStatusMsg),"Playing %s",file.c_str());
  lastTrack=0; isPlaying=true; isLiveMic=false;
  pushCommand(WIFI_CMD_PLAY_BASE + 1);
  char resp[160]; snprintf(resp,sizeof(resp),"{\"ok\":true,\"file\":\"%s\"}",file.c_str());
  server->send(200,"application/json",resp);
  
}

void WiFiSkullController::handleStop(){
  isPlaying=false; isLiveMic=false; lastTrack=0;
  snprintf(lastStatusMsg,sizeof(lastStatusMsg),"Stopped");
  pushCommand(WIFI_CMD_STOP);
  server->send(200,"application/json","{\"ok\":true,\"msg\":\"Stopped\"}");
}

void WiFiSkullController::handleVolume(){
  if(server->hasArg("vol")){
    int v=server->arg("vol").toInt();
    if(isValidVolume(v)){ lastVolume=v; pushCommand(WIFI_CMD_VOLUME_BASE+v); char r[64]; snprintf(r,sizeof(r),"{\"ok\":true,\"vol\":%d}",v); server->send(200,"application/json",r); return; }
  }
  if(server->hasArg("micmute")){
    int m=server->arg("micmute").toInt();
    char r[64]; snprintf(r,sizeof(r),"{\"ok\":true,\"micmute\":%d}",m); server->send(200,"application/json",r); return;
  }
  server->send(400,"application/json","{\"ok\":false,\"error\":\"Missing vol\"}");
}

void WiFiSkullController::handleStatus(){
  char ipStr[32];
  if(WiFi.getMode() & WIFI_AP) snprintf(ipStr,sizeof(ipStr),"%s",WiFi.softAPIP().toString().c_str());
  else snprintf(ipStr,sizeof(ipStr),"%s",WiFi.localIP().toString().c_str());
  char json[400];
  snprintf(json,sizeof(json),
    "{\"playing\":%s,\"live\":%s,\"track\":%d,\"file\":\"%s\",\"msg\":\"%s\",\"ip\":\"%s\",\"vol\":%d}",
    isPlaying?"true":"false", isLiveMic?"true":"false", lastTrack, lastFileName, lastStatusMsg, ipStr, lastVolume);
  server->send(200,"application/json",json);
}

void WiFiSkullController::handleUpload(){
  HTTPUpload& upload=server->upload();
  static File outFile;
  static File outFileSD;
  static String outPath;
  static bool isConf=false;
  static bool littleTarget=false;
  static bool sdTarget=false;
  static bool littleWriteOK=false;
  static bool sdWriteOK=false;

  if(upload.status==UPLOAD_FILE_START){
    uploadError=""; uploadLastFile=""; uploadLastSize=0; uploadDest="";
    outFile=File(); outFileSD=File(); outPath="";
    littleTarget=sdTarget=false;
    littleWriteOK=sdWriteOK=false;

    String filename=upload.filename;
    isConf=filename.equalsIgnoreCase("skull.conf") || server->hasArg("conf");
    
    if(isPlaying){ uploadError="Stop playback before uploading files"; return; }
    if(filename.isEmpty()){ uploadError="No filename"; return; }
    if(filename.indexOf("..")>=0){ uploadError="Invalid filename"; return; }
    if(!isConf && !filename.endsWith(".wav") && !filename.endsWith(".WAV")){
      uploadError="Only .wav files are accepted"; return;
    }
    if(isConf && !filename.equalsIgnoreCase("skull.conf")){
      uploadError="Config filename must be skull.conf"; return;
    }
    const int slash=filename.lastIndexOf('/');
    if(slash>=0) filename=filename.substring(slash+1);
    uploadLastFile=filename;

    if(isConf){
      outPath="/skull.conf";
      if(ConfigManager::isLittleFSReady()){
        LittleFS.remove("/skull.conf.tmp");
        outFile=LittleFS.open("/skull.conf.tmp","w");
        littleTarget=static_cast<bool>(outFile);
        littleWriteOK=littleTarget;
      }
      if(sdMounted){
        SDLockGuard lock;
        SD_MMC.remove("/skull.conf.tmp");
        outFileSD=SD_MMC.open("/skull.conf.tmp","w");
        sdTarget=static_cast<bool>(outFileSD);
        sdWriteOK=sdTarget;
      }
      if(!littleTarget && !sdTarget){ uploadError="Could not create config temp file"; }
    } else {
      if(!ConfigManager::isLittleFSReady()){
        uploadError="LittleFS is not mounted"; return;
      }
      if(!LittleFS.exists("/wav")) LittleFS.mkdir("/wav");
      outPath="/wav/"+filename;
      const size_t freeSpace=LittleFS.totalBytes()-LittleFS.usedBytes();
      if(freeSpace<2048){ uploadError="LittleFS full"; return; }
      outFile=LittleFS.open(outPath,"w");
      littleTarget=static_cast<bool>(outFile);
      littleWriteOK=littleTarget;
      if(!littleTarget) uploadError="Could not create "+outPath;
    }
  } else if(upload.status==UPLOAD_FILE_WRITE){
    if(!uploadError.isEmpty()) return;
    bool accepted=false;
    if(littleTarget && outFile){
      const size_t written=outFile.write(upload.buf,upload.currentSize);
      littleWriteOK &= written==upload.currentSize;
      accepted |= written==upload.currentSize;
    }
    if(sdTarget && outFileSD){
      SDLockGuard lock;
      const size_t written=outFileSD.write(upload.buf,upload.currentSize);
      sdWriteOK &= written==upload.currentSize;
      accepted |= written==upload.currentSize;
    }
    if(!accepted){ uploadError="Filesystem write failed or disk full"; }
    else uploadLastSize+=upload.currentSize;
    if((uploadLastSize%2048)<static_cast<size_t>(upload.currentSize)){
      delay(1); yield(); esp_task_wdt_reset();
    }
  } else if(upload.status==UPLOAD_FILE_END){
    if(isConf){
      if(uploadLastSize<10 && uploadError.isEmpty()) uploadError="Config file is too small";
      bool littleCommitted=false;
      bool sdCommitted=false;

      if(outFile){ outFile.flush(); outFile.close(); }
      if(littleTarget && littleWriteOK && uploadError.isEmpty()){
        LittleFS.remove("/skull.conf");
        littleCommitted=LittleFS.rename("/skull.conf.tmp","/skull.conf");
      }
      if(!littleCommitted) LittleFS.remove("/skull.conf.tmp");

      if(outFileSD){
        SDLockGuard lock;
        outFileSD.flush();
        outFileSD.close();
      }
      if(sdTarget){
        SDLockGuard lock;
        if(sdWriteOK && uploadError.isEmpty()){
          SD_MMC.remove("/skull.conf");
          sdCommitted=SD_MMC.rename("/skull.conf.tmp","/skull.conf");
        }
        if(!sdCommitted) SD_MMC.remove("/skull.conf.tmp");
      }

      if(uploadError.isEmpty() && !littleCommitted && !sdCommitted){
        uploadError="Could not commit config file";
      } else if(uploadError.isEmpty()){
        uploadDest=littleCommitted && sdCommitted ? "SD+LittleFS" :
                   (sdCommitted ? "SD" : "LittleFS");
      }
      
    } else {
      if(outFile){ outFile.flush(); outFile.close(); }
      if(uploadLastSize<44 && uploadError.isEmpty()) uploadError="File is too small to be WAV";
      if(!littleWriteOK || !uploadError.isEmpty()) LittleFS.remove(outPath);
      else uploadDest="LittleFS";
      
    }
  } else if(upload.status==UPLOAD_FILE_ABORTED){
    if(outFile) outFile.close();
    if(outFileSD){ SDLockGuard lock; outFileSD.close(); }
    if(!outPath.isEmpty() && ConfigManager::isLittleFSReady()){
      LittleFS.remove(outPath);
      LittleFS.remove("/skull.conf.tmp");
    }
    if(sdMounted){ SDLockGuard lock; SD_MMC.remove("/skull.conf.tmp"); }
    if(uploadError.isEmpty()) uploadError="Upload aborted by browser";
  }
}

void WiFiSkullController::handleUploadPost(){
  if(uploadError.length()>0){
    char resp[320]; String safe=uploadError; safe.replace("\"","'"); safe.replace("\n"," ");
    snprintf(resp,sizeof(resp),"{\"ok\":false,\"error\":\"%s\"}",safe.c_str());
    server->send(400,"application/json",resp);
    Serial.printf("[Upload] POST ERROR: %s\n", resp);
  } else {
    char resp[320]; String safeFile=uploadLastFile; safeFile.replace("\"","'");
    snprintf(resp,sizeof(resp),"{\"ok\":true,\"file\":\"%s\",\"size\":%d,\"dest\":\"%s\"}",safeFile.c_str(),(int)uploadLastSize, uploadDest.c_str());
    server->send(200,"application/json",resp);
    
  }
}

void WiFiSkullController::handleDelete(){
  if(!server->hasArg("file")){ server->send(400,"application/json","{\"ok\":false,\"error\":\"Missing file\"}"); return; }
  String file=server->arg("file");
  if(file.indexOf("..")>=0){ server->send(400,"application/json","{\"ok\":false}"); return; }
  String path="/wav/"+file;
  bool ok=false;
  if(LittleFS.exists(path)) ok=LittleFS.remove(path);
  else if(LittleFS.exists("/"+file)) ok=LittleFS.remove("/"+file);
  if(ok) server->send(200,"application/json","{\"ok\":true}");
  else server->send(404,"application/json","{\"ok\":false,\"error\":\"Not found in LittleFS (SD delete on PC)\"}");
}

void WiFiSkullController::handleNotFound(){
  String uri=server->uri();
  if(uri=="/mic-processor.js"){
    server->send_P(200,"application/javascript",MIC_PROCESSOR_JS);
    return;
  }
  server->send(404,"text/plain","404 Not Found");
}

void WiFiSkullController::init(){ init(ConfigManager::get()); }

void WiFiSkullController::init(const SkullConfig& cfg){
  lastVolume=cfg.volume;
  WiFi.persistent(false); WiFi.mode(WIFI_OFF); delay(200); WiFi.setSleep(false);
  if(cfg.wifi_mode=="sta"){
    WiFi.mode(WIFI_STA);
    WiFi.setHostname("TalkingSkull");
    if(!cfg.sta_dhcp) WiFi.config(cfg.sta_ip,cfg.sta_gateway,cfg.sta_subnet);
    
    WiFi.begin(cfg.sta_ssid.c_str(), cfg.sta_password.c_str());
    unsigned long s=millis();
    while(WiFi.status()!=WL_CONNECTED && millis()-s<8000){ // reduced from 15000 to 8000 for faster boot
      delay(500); 
      yield();
    }
    
    if(WiFi.status()==WL_CONNECTED){
      
      if(MDNS.begin("talkingskull")){ MDNS.addService("http","tcp",80);  }
    } else {
      Serial.println("[WiFi] WARNING: STA failed after 8s; starting AP fallback");
      WiFi.mode(WIFI_OFF); delay(200); WiFi.mode(WIFI_AP);
      WiFi.setHostname("TalkingSkull");
      WiFi.softAPConfig(cfg.ap_ip,cfg.ap_ip,cfg.ap_subnet);
      WiFi.softAP(cfg.ap_ssid.c_str(),cfg.ap_password.c_str(),WIFI_AP_CHANNEL,0,WIFI_AP_MAX_CONN);
      if(MDNS.begin("talkingskull")){ MDNS.addService("http","tcp",80);  }
    }
  } else {
    WiFi.mode(WIFI_AP); delay(200);
    IPAddress ip=cfg.ap_ip, gw=cfg.ap_ip, mask=cfg.ap_subnet;
    WiFi.softAPConfig(ip,gw,mask);
    String pass=cfg.ap_password; if(pass.length()>0 && pass.length()<8) pass="";
    WiFi.softAP(cfg.ap_ssid.c_str(),pass.c_str(),WIFI_AP_CHANNEL,0,WIFI_AP_MAX_CONN);
    
    WiFi.setHostname("TalkingSkull");
    if(MDNS.begin("talkingskull")){ MDNS.addService("http","tcp",80);  }
  }

  // ConfigManager owns one-time filesystem mounting. Never remount or format
  // from the web layer.
  sdMounted = ConfigManager::isSDReady();
  

  server=new WebServer(80);
  server->on("/", handleRoot);
  server->on("/upload", HTTP_GET, handleUploadPage);
  server->on("/upload", HTTP_POST, handleUploadPost, handleUpload);
  server->on("/list", handleList);
  server->on("/play", handlePlay);
  server->on("/stop", handleStop);
  server->on("/pause", [](){
    // pause toggle handled via isPlaying + paused flag
    if(isPlaying){
      isPlaying=true; // keep true but paused
      snprintf(lastStatusMsg,sizeof(lastStatusMsg),"Paused %s",lastFileName);
      // push pause command
      nReceivedValue=WIFI_CMD_PAUSE; hasNewCommand=true;
      char resp[128]; snprintf(resp,sizeof(resp),"{\"ok\":true,\"file\":\"%s\",\"paused\":true}",lastFileName);
      server->send(200,"application/json",resp);
    } else {
      server->send(400,"application/json","{\"ok\":false,\"error\":\"Not playing\"}");
    }
  });
  server->on("/resume", [](){
    if(isPlaying){
      snprintf(lastStatusMsg,sizeof(lastStatusMsg),"Playing %s",lastFileName);
      nReceivedValue=WIFI_CMD_RESUME; hasNewCommand=true;
      char resp[128]; snprintf(resp,sizeof(resp),"{\"ok\":true,\"file\":\"%s\",\"paused\":false}",lastFileName);
      server->send(200,"application/json",resp);
    } else {
      server->send(400,"application/json","{\"ok\":false,\"error\":\"Not playing\"}");
    }
  });
  server->on("/volume", handleVolume);
  server->on("/status", handleStatus);
  server->on("/delete", handleDelete);
  server->on("/eyes", handleEyesPage);
  server->on("/eye", handleEyeControl);
  server->on("/mic-processor.js", HTTP_GET, [](){ server->send_P(200,"application/javascript",MIC_PROCESSOR_JS); });
  server->onNotFound(handleNotFound);
  server->begin();
  
}

void WiFiSkullController::loop(){ if(server) server->handleClient(); }
void WiFiSkullController::pushCommand(uint32_t cmd){ nReceivedValue=cmd; hasNewCommand=true; }

void WiFiSkullController::setPlayingStatus(const char* fileName, bool playing, bool live){
  isPlaying=playing; isLiveMic=live;
  if(fileName) snprintf(lastFileName,sizeof(lastFileName),"%s",fileName);
  if(playing){
    if(live) snprintf(lastStatusMsg,sizeof(lastStatusMsg),"Live Mic - Talking...");
    else snprintf(lastStatusMsg,sizeof(lastStatusMsg),"Playing %s",lastFileName);
  }
}
void WiFiSkullController::setPlayingStatus(uint8_t track, bool playing){
  if(playing){
    lastTrack=track; isPlaying=true; isLiveMic=false;
    String f=formatFileName(track);
    snprintf(lastFileName,sizeof(lastFileName),"%s",f.c_str());
    snprintf(lastStatusMsg,sizeof(lastStatusMsg),"Playing %s",lastFileName);
  } else {
    if(lastTrack!=0 && strcmp(lastFileName,"0000.wav")!=0 && !isLiveMic) snprintf(lastStatusMsg,sizeof(lastStatusMsg),"Finished playing %s",lastFileName);
    else if(isLiveMic) snprintf(lastStatusMsg,sizeof(lastStatusMsg),"Finished playing live mic");
    else snprintf(lastStatusMsg,sizeof(lastStatusMsg),"Stopped");
    isPlaying=false; isLiveMic=false;
  }
}
void WiFiSkullController::setFinishedMessage(const char* fileName){
  snprintf(lastStatusMsg,sizeof(lastStatusMsg),"Finished playing %s",fileName);
  isPlaying=false; isLiveMic=false;
}
void WiFiSkullController::setLiveMicStatus(bool active){
  isLiveMic=active; isPlaying=active;
  if(active) snprintf(lastStatusMsg,sizeof(lastStatusMsg),"Live Mic - Talking...");
  else snprintf(lastStatusMsg,sizeof(lastStatusMsg),"Finished playing live mic");
}
void WiFiSkullController::setStatusMessage(const char* msg, bool){ snprintf(lastStatusMsg,sizeof(lastStatusMsg),"%s",msg); }
void WiFiSkullController::setVolume(uint8_t vol){ lastVolume=vol; }
