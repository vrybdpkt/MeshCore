#pragma once

#ifdef WITH_WEB_INTERFACE

// Stored in flash (PROGMEM) — not loaded into RAM until served.
static const char WEB_UI_HTML[] PROGMEM = R"rawhtml(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MeshCore Repeater</title>
<style>
:root{--bg:#0d1117;--panel:#161b22;--border:#30363d;--text:#e6edf3;--dim:#8b949e;--green:#3fb950;--red:#f85149;--blue:#58a6ff;--orange:#e3b341}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--text);font-family:'SF Mono',Consolas,'Courier New',monospace;font-size:13px;height:100vh;display:flex;flex-direction:column}
header{background:var(--panel);border-bottom:1px solid var(--border);padding:10px 16px;display:flex;align-items:center;gap:10px;flex-shrink:0}
header h1{font-size:14px;font-weight:600;color:var(--blue);margin-right:auto}
.badge{font-size:11px;padding:2px 8px;border-radius:10px;background:#21262d;color:var(--dim)}
.badge.ok{background:#0d4a1e;color:var(--green)}
.badge.err{background:#4a0d0d;color:var(--red)}
nav{display:flex;border-bottom:1px solid var(--border);background:var(--panel);flex-shrink:0}
nav button{background:none;border:none;border-bottom:2px solid transparent;color:var(--dim);padding:9px 14px;cursor:pointer;font:inherit;font-size:12px;transition:color .15s}
nav button.active{color:var(--text);border-bottom-color:var(--blue)}
nav button:hover{color:var(--text)}
main{flex:1;overflow:hidden;display:flex;flex-direction:column}
.tab{display:none;flex:1;overflow-y:auto;padding:16px;flex-direction:column}
.tab.active{display:flex}
/* Cards */
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(160px,1fr));gap:10px;margin-bottom:14px}
.card{background:var(--panel);border:1px solid var(--border);border-radius:6px;padding:12px}
.card .lbl{color:var(--dim);font-size:10px;text-transform:uppercase;letter-spacing:.5px;margin-bottom:4px}
.card .val{font-size:18px;font-weight:600}
.card .val.sm{font-size:14px}
/* Table */
.tbl-wrap{overflow-x:auto}
table{width:100%;border-collapse:collapse}
th{color:var(--dim);font-weight:normal;font-size:11px;text-transform:uppercase;padding:6px 10px;border-bottom:1px solid var(--border);text-align:left}
td{padding:7px 10px;border-bottom:1px solid #21262d;font-size:12px}
tr:last-child td{border-bottom:none}
/* Info table */
.info-tbl td:first-child{color:var(--dim);width:120px}
/* MQTT row */
.mqtt-row{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:8px}
.mqtt-status{font-size:12px}
/* Messages */
.msg-wrap{flex:1;display:flex;flex-direction:column;overflow:hidden;min-height:0}
.ch-tabs{display:flex;border-bottom:1px solid var(--border);flex-shrink:0;overflow-x:auto;margin-bottom:6px}
.ch-tab{background:none;border:none;border-bottom:2px solid transparent;color:var(--dim);padding:5px 10px;cursor:pointer;font:inherit;font-size:11px;white-space:nowrap;flex-shrink:0}
.ch-tab.active{color:var(--text);border-bottom-color:var(--blue)}
.ch-tab:hover{color:var(--text)}
/* ch-panels is a flex column; only the active ch-panel participates in layout */
.ch-panels{flex:1;display:flex;flex-direction:column;min-height:0;overflow:hidden}
.ch-panel{display:none;flex:1;min-height:0;overflow-y:auto;flex-direction:column;gap:6px;padding:4px 0}
.ch-panel.active{display:flex}
/* Messages tab must not scroll at the tab level — scrolling is inside ch-panel */
#msgs.tab{overflow:hidden}
.msg{padding:7px 12px;border-radius:6px;max-width:85%;font-size:12px}
.msg.in{background:var(--panel);border:1px solid var(--border);align-self:flex-start}
.msg.out{background:#0d2d4a;border:1px solid #1f4e79;align-self:flex-end}
.msg .from{font-size:10px;color:var(--dim);margin-bottom:2px}
.ch-tag{color:var(--blue);font-size:10px;padding:0 4px;border-radius:3px;background:#0d2540}
.compose{display:flex;gap:6px;padding-top:10px;border-top:1px solid var(--border);flex-shrink:0}
/* CLI */
.cli-out{background:#010409;border:1px solid var(--border);border-radius:4px;padding:10px;height:320px;overflow-y:auto;white-space:pre-wrap;font-size:12px;flex-shrink:0;margin-bottom:8px}
.row{display:flex;gap:6px}
/* Log */
.log-out{background:#010409;border:1px solid var(--border);border-radius:4px;padding:10px;flex:1;overflow-y:auto;white-space:pre-wrap;font-size:11px;min-height:200px}
/* Controls */
input[type=text],select,textarea{background:#21262d;border:1px solid var(--border);color:var(--text);padding:6px 10px;border-radius:4px;font:inherit;font-size:12px}
input[type=text]:focus,select:focus{outline:1px solid var(--blue);border-color:var(--blue)}
input.flex1,textarea.flex1{flex:1;min-width:0}
select.narrow{width:130px;flex-shrink:0}
button{background:var(--blue);color:#000;border:none;padding:6px 14px;border-radius:4px;cursor:pointer;font:inherit;font-size:12px;font-weight:600;flex-shrink:0}
button:hover{opacity:.85}
button.sec{background:#21262d;color:var(--text)}
button.red{background:#4a0d0d;color:var(--red)}
.snr-good{color:var(--green)}
.snr-ok{color:var(--orange)}
.snr-bad{color:var(--red)}
.dim{color:var(--dim)}
h2{font-size:12px;font-weight:600;color:var(--dim);text-transform:uppercase;letter-spacing:.5px;margin-bottom:10px}
</style>
</head>
<body>
<header>
  <h1 id="hdrName">MeshCore Repeater</h1>
  <span class="badge" id="badgeMqtt">MQTT --</span>
  <span class="badge" id="badgeWifi">WiFi --</span>
</header>
<nav>
  <button class="active" onclick="tab('dash',this)">Dashboard</button>
  <button onclick="tab('msgs',this)">Messages</button>
  <button onclick="tab('nbrs',this)">Neighbors</button>
  <button onclick="tab('cli',this)">CLI</button>
  <button onclick="tab('log',this);loadLog()">Log</button>
</nav>
<main>

<!-- DASHBOARD -->
<div id="dash" class="tab active">
  <div class="grid">
    <div class="card"><div class="lbl">Uptime</div><div class="val sm" id="dUptime">--</div></div>
    <div class="card"><div class="lbl">Packets RX</div><div class="val" id="dRx">--</div></div>
    <div class="card"><div class="lbl">Packets TX</div><div class="val" id="dTx">--</div></div>
    <div class="card"><div class="lbl">Last SNR</div><div class="val sm" id="dSnr">--</div></div>
    <div class="card"><div class="lbl">Last RSSI</div><div class="val sm" id="dRssi">--</div></div>
    <div class="card"><div class="lbl">Battery</div><div class="val sm" id="dBatt">--</div></div>
    <div class="card"><div class="lbl">MQTT TX</div><div class="val" id="dMTx">--</div></div>
    <div class="card"><div class="lbl">MQTT RX</div><div class="val" id="dMRx">--</div></div>
  </div>
  <div class="card" style="margin-bottom:12px">
    <h2>Node Info</h2>
    <div class="tbl-wrap"><table class="info-tbl">
      <tr><td>Name</td><td id="iName">--</td></tr>
      <tr><td>Node ID</td><td id="iId" style="font-family:monospace">--</td></tr>
      <tr><td>Firmware</td><td id="iFw">--</td></tr>
      <tr><td>Frequency</td><td id="iFreq">--</td></tr>
      <tr><td>TX Power</td><td id="iTxPwr">--</td></tr>
    </table></div>
  </div>
  <div class="card">
    <h2>MQTT Bridge</h2>
    <div class="mqtt-status dim" id="mqttStatusTxt">--</div>
    <div class="mqtt-row">
      <button class="sec" onclick="mqttCtrl('start')">Start MQTT</button>
      <button class="sec" onclick="mqttCtrl('stop')">Stop MQTT</button>
    </div>
  </div>
  <div class="card" style="margin-top:12px">
    <h2>Channel Activity</h2>
    <div class="tbl-wrap"><table>
      <thead><tr><th>Hash</th><th>Name</th><th>Packets</th><th>Last Seen</th><th>Avg SNR</th><th>PSK</th></tr></thead>
      <tbody id="chStatBody"><tr><td colspan="6" class="dim">No channel traffic seen yet.</td></tr></tbody>
    </table></div>
  </div>
</div>

<!-- MESSAGES -->
<div id="msgs" class="tab">
  <div class="msg-wrap">
    <div class="ch-tabs" id="chTabs"></div>
    <div class="ch-panels" id="chPanels"></div>
    <div class="compose">
      <select class="narrow" id="msgTo">
        <option value="flood">All ACL clients</option>
      </select>
      <select class="narrow" id="msgScope" style="display:none" title="Restrict flood to this region (blank = unscoped)">
        <option value="">No scope</option>
      </select>
      <input type="text" class="flex1" id="msgText" placeholder="Type a message…" onkeydown="if(event.key==='Enter')sendMsg()">
      <button onclick="sendMsg()">Send</button>
    </div>
  </div>
</div>

<!-- NEIGHBORS -->
<div id="nbrs" class="tab">
  <h2 style="margin-bottom:6px">Radio Neighbors <span style="font-size:10px;color:var(--dim);text-transform:none;letter-spacing:0">(direct zero-hop, from ADVERT packets)</span></h2>
  <div class="tbl-wrap" style="margin-bottom:14px">
    <table>
      <thead><tr><th>Node ID</th><th>SNR</th><th>Last ADVERT</th></tr></thead>
      <tbody id="nbrBody"></tbody>
    </table>
  </div>
  <h2 style="margin-bottom:6px">Logged-in Clients <span style="font-size:10px;color:var(--dim);text-transform:none;letter-spacing:0">(have authenticated with this repeater)</span></h2>
  <div class="tbl-wrap">
    <table>
      <thead><tr><th>Node ID</th><th>Last Activity</th></tr></thead>
      <tbody id="clientBody"></tbody>
    </table>
  </div>
</div>

<!-- CLI -->
<div id="cli" class="tab">
  <div class="cli-out" id="cliOut"><div style="color:var(--dim)">MeshCore CLI — type / for help, or /command to see options.</div>
</div>
  <div class="row">
    <input type="text" class="flex1" id="cliIn" placeholder="Command (/ for help)…" onkeydown="cliKey(event)">
    <button onclick="runCli()">Run</button>
  </div>
</div>

<!-- LOG -->
<div id="log" class="tab">
  <div style="display:flex;gap:8px;margin-bottom:8px;flex-shrink:0">
    <button class="sec" onclick="loadLog()">Refresh</button>
    <button class="red" onclick="if(confirm('Erase packet log?'))runCliSilent('log erase').then(loadLog)">Erase Log</button>
    <button class="sec" onclick="var r=document.getElementById('logRef');r.style.display=r.style.display==='none'?'block':'none'">Type Reference</button>
  </div>
  <div id="logRef" style="display:none;background:#161b22;border:1px solid var(--border);border-radius:4px;padding:8px 12px;margin-bottom:8px;font-size:11px;line-height:1.7">
    <strong style="color:var(--text)">Payload types</strong> &nbsp;
    <span class="dim">00</span> REQ &nbsp;
    <span class="dim">01</span> RESPONSE &nbsp;
    <span class="dim">02</span> TXT_MSG &nbsp;
    <span class="dim">03</span> ACK &nbsp;
    <span class="dim">04</span> ADVERT &nbsp;
    <span class="dim">05</span> GRP_TXT &nbsp;
    <span class="dim">06</span> GRP_DATA &nbsp;
    <span class="dim">07</span> ANON_REQ &nbsp;
    <span class="dim">08</span> PATH &nbsp;
    <span class="dim">09</span> TRACE &nbsp;
    <span class="dim">0A</span> MULTIPART &nbsp;
    <span class="dim">0B</span> CONTROL &nbsp;
    <span class="dim">0F</span> RAW_CUSTOM<br>
    <strong style="color:var(--text)">Route types</strong> &nbsp;
    <span class="dim">F</span> flood &nbsp;
    <span class="dim">D</span> direct &nbsp;
    <span class="dim">TF</span> transport flood (region-scoped) &nbsp;
    <span class="dim">TD</span> transport direct
  </div>
  <div class="log-out" id="logOut">Press Refresh to load the packet log.</div>
</div>

</main>
<script>
var curTab='dash', msgSeq=0, cliHist=[], cliHistIdx=-1;
var chKeys={};       // key → {btn, panel}  (keyed by hash like "8F", "all", or "flood")
var chHashToVal={};  // hash hex → compose "To" option value (e.g. "8F" → "channel0")

function initChTabs(){
  getOrCreateCh('all','All');
  switchChTab('all',chKeys['all'].btn);
}

function getOrCreateCh(key,label){
  if(chKeys[key])return chKeys[key].panel;
  var tabs=document.getElementById('chTabs');
  var btn=document.createElement('button');
  btn.className='ch-tab';
  btn.textContent=label||key;
  btn.onclick=(function(k,b){return function(){switchChTab(k,b);};})(key,btn);
  tabs.appendChild(btn);
  var panels=document.getElementById('chPanels');
  var panel=document.createElement('div');
  panel.className='ch-panel';
  panels.appendChild(panel);
  chKeys[key]={btn:btn,panel:panel};
  return panel;
}

function switchChTab(key,btn){
  Object.keys(chKeys).forEach(function(k){
    chKeys[k].btn.classList.remove('active');
    chKeys[k].panel.classList.remove('active');
  });
  btn.classList.add('active');
  chKeys[key].panel.classList.add('active');
  // Sync compose "To" dropdown when switching to a channel-specific tab
  if(key!=='all'){
    var sel=document.getElementById('msgTo');
    if(key==='flood'){
      sel.value='flood';
    } else {
      var optVal=chHashToVal[key];
      if(optVal)sel.value=optVal;
    }
  }
}

function appendMsgToPanel(panel,m){
  var ph=panel.querySelector('p.dim');
  if(ph)ph.remove();
  var div=document.createElement('div');
  div.className='msg '+(m.outbound?'out':'in');
  var ch=m.channel?'<span class="ch-tag">'+esc(m.channel)+'</span> ':'';
  // For inbound messages, append signal metadata when available
  var meta='';
  if(!m.outbound&&m.snr!=null){
    var snrVal=(m.snr/4).toFixed(1);
    var snrCls=m.snr>=20?'snr-good':(m.snr>=-20?'snr-ok':'snr-bad');
    meta+=' <span class="'+snrCls+'" style="font-size:9px">'+snrVal+' dB</span>';
    if(m.hops!=null&&m.hops>0)meta+=' <span class="dim" style="font-size:9px">'+m.hops+(m.hops===1?' hop':' hops')+'</span>';
  }
  div.innerHTML='<div class="from">'+esc(m.from)+' &bull; '+ch+fmtSeen(m.ts)+meta+'</div><div>'+esc(m.text)+'</div>';
  panel.appendChild(div);
  panel.scrollTop=panel.scrollHeight;
}

function tab(id,btn){
  document.querySelectorAll('.tab').forEach(function(e){e.classList.remove('active');});
  document.querySelectorAll('nav button').forEach(function(e){e.classList.remove('active');});
  document.getElementById(id).classList.add('active');
  btn.classList.add('active');
  curTab=id;
  if(id==='nbrs')loadNbrs();
  if(id==='msgs')loadMsgs();
}

function fmtUptime(s){
  var d=Math.floor(s/86400),h=Math.floor((s%86400)/3600),m=Math.floor((s%3600)/60),sec=s%60;
  if(d)return d+'d '+h+'h '+m+'m';
  if(h)return h+'h '+m+'m '+sec+'s';
  return m+'m '+sec+'s';
}
// fmtAgo: format a relative age given seconds elapsed (always valid, no RTC needed).
function fmtAgo(secs){
  if(secs==null||secs<0)return '?';
  if(secs<60)return secs+'s ago';
  if(secs<3600)return Math.floor(secs/60)+'m ago';
  return Math.floor(secs/3600)+'h ago';
}
// fmtSeen: format from an absolute RTC Unix timestamp.
// Falls back to '?' when the timestamp is 0 (RTC unsynced) or the delta is
// implausibly large (RTC synced *after* the event was recorded with ts=0).
function fmtSeen(ts){
  if(!ts)return '?';
  var d=Math.floor(Date.now()/1000)-ts;
  if(d<0)return 'just now';
  if(d>365*24*3600)return '?';  // implausibly old — RTC was unsynced at record time
  if(d<60)return d+'s ago';
  if(d<3600)return Math.floor(d/60)+'m ago';
  return Math.floor(d/3600)+'h ago';
}
function esc(s){
  return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
}
function snrClass(snr4){
  var s=snr4/4;
  if(s>=5)return 'snr-good';
  if(s>=-5)return 'snr-ok';
  return 'snr-bad';
}

function loadStatus(){
  fetch('/api/status').then(function(r){return r.json();}).then(function(d){
    document.getElementById('hdrName').textContent=d.name||'MeshCore';
    document.getElementById('dUptime').textContent=fmtUptime(d.uptime_secs||0);
    document.getElementById('dRx').textContent=d.packets_recv!=null?d.packets_recv:'--';
    document.getElementById('dTx').textContent=d.packets_sent!=null?d.packets_sent:'--';
    var snr=d.last_snr!=null?(d.last_snr/4).toFixed(1)+' dB':'--';
    document.getElementById('dSnr').textContent=snr;
    document.getElementById('dRssi').textContent=d.last_rssi!=null?d.last_rssi+' dBm':'--';
    document.getElementById('dBatt').textContent=d.battery_mv?((d.battery_mv/1000).toFixed(2)+' V'):'--';
    document.getElementById('dMTx').textContent=d.mqtt_tx!=null?d.mqtt_tx:'--';
    document.getElementById('dMRx').textContent=d.mqtt_rx!=null?d.mqtt_rx:'--';
    document.getElementById('iName').textContent=d.name||'--';
    // Update compose placeholder to show who messages will be sent as
    var sn=d.sender_name||d.name||'';
    document.getElementById('msgText').placeholder=sn?'Message as '+sn+'…':'Type a message…';
    document.getElementById('iId').textContent=d.node_id||'--';
    document.getElementById('iFw').textContent=(d.firmware||'--')+' ('+( d.build_date||'--')+')';
    document.getElementById('iFreq').textContent=d.freq_mhz?d.freq_mhz+' MHz':'--';
    document.getElementById('iTxPwr').textContent=d.tx_power_dbm!=null?d.tx_power_dbm+' dBm':'--';
    document.getElementById('mqttStatusTxt').textContent=d.mqtt_status||'--';
    var bm=document.getElementById('badgeMqtt');
    bm.textContent='MQTT '+(d.mqtt_connected?'on':'off');
    bm.className='badge '+(d.mqtt_connected?'ok':'err');
    var bw=document.getElementById('badgeWifi');
    bw.textContent='WiFi '+(d.wifi_ip||'down');
    bw.className='badge '+(d.wifi_ip?'ok':'');
  }).catch(function(){});
}

function loadNbrs(){
  fetch('/api/neighbors').then(function(r){return r.json();}).then(function(d){
    // Radio neighbors (from ADVERT packets)
    var rows='';
    (d.neighbors||[]).forEach(function(n){
      var cls=snrClass(n.snr);
      rows+='<tr><td style="font-family:monospace">'+esc(n.id)+'</td><td class="'+cls+'">'+(n.snr/4).toFixed(1)+' dB</td><td class="dim">'+fmtAgo(n.secs_ago)+'</td></tr>';
    });
    document.getElementById('nbrBody').innerHTML=rows||'<tr><td colspan="3" class="dim">No direct neighbors heard yet — waiting for ADVERT packets (~2 min interval).</td></tr>';
    // Logged-in clients (from ACL)
    var crows='';
    (d.clients||[]).forEach(function(c){
      crows+='<tr><td style="font-family:monospace">'+esc(c.id)+'</td><td class="dim">'+(c.last_activity?fmtSeen(c.last_activity):'never')+'</td></tr>';
    });
    document.getElementById('clientBody').innerHTML=crows||'<tr><td colspan="2" class="dim">No clients logged in yet.</td></tr>';
  }).catch(function(){});
}

// Number of channel options currently in the dropdown (set by loadChannels).
var numChOpts=0;

function loadChannels(){
  fetch('/api/channels').then(function(r){return r.json();}).then(function(chs){
    var sel=document.getElementById('msgTo');
    var prev=sel.value;
    // Remove existing channel options (everything before "All ACL clients" at index 0)
    while(sel.options.length>1)sel.remove(0);
    // Insert channel entries before "All ACL clients"
    var floodOpt=sel.options[0];
    chs.forEach(function(ch){
      var label=ch.name?ch.name+' ['+ch.hash+']':'Channel '+ch.idx+' ['+ch.hash+']';
      var o=new Option(label,'channel'+ch.idx);
      o.dataset.hash=ch.hash;
      chHashToVal[ch.hash]='channel'+ch.idx;
      sel.insertBefore(o,floodOpt);
      // Pre-create the channel sub-tab so it appears even before messages arrive
      getOrCreateCh(ch.hash,label);
    });
    numChOpts=chs.length;
    // Restore previous selection or default to first channel / flood
    if(prev&&[].slice.call(sel.options).some(function(o){return o.value===prev;})){
      sel.value=prev;
    } else {
      sel.value=chs.length>0?'channel'+chs[0].idx:'flood';
    }
  }).catch(function(){});
}

// Load region names from /api/regions and populate the scope dropdown.
// The dropdown is hidden when no regions are configured.
function loadRegions(){
  fetch('/api/regions').then(function(r){return r.json();}).then(function(regions){
    var sel=document.getElementById('msgScope');
    // Rebuild options: keep the first "No scope" entry
    while(sel.options.length>1)sel.remove(1);
    regions.forEach(function(name){
      sel.appendChild(new Option(name,name));
    });
    // Only show the scope dropdown when there is at least one region
    sel.style.display=regions.length>0?'':'none';
  }).catch(function(){});
}

function loadMsgs(){
  fetch('/api/messages?since='+msgSeq).then(function(r){return r.json();}).then(function(d){
    // Update contacts dropdown — keep channel options + flood, remove old contacts
    var sel=document.getElementById('msgTo');
    var prev=sel.value;
    while(sel.options.length>numChOpts+1)sel.remove(numChOpts+1);  // +1 for flood
    (d.contacts||[]).forEach(function(c){
      var o=new Option(c.name||c.id,c.id);
      sel.appendChild(o);
    });
    if([].slice.call(sel.options).some(function(o){return o.value===prev;})){
      sel.value=prev;
    }

    // Dispatch messages to per-channel panels and the "All" panel
    var msgs=d.messages||[];
    msgs.forEach(function(m){
      msgSeq=Math.max(msgSeq,(m.seq||0)+1);
      // Extract channel hash from tag like "FamilyNe[8F]" → "8F"
      var hash=null;
      if(m.channel){
        var match=m.channel.match(/\[([0-9A-Fa-f]+)\]/);
        if(match)hash=match[1].toUpperCase();
      }
      // Add to channel-specific panel (create on demand if not yet known)
      var chPanel=hash
        ? getOrCreateCh(hash,m.channel)
        : getOrCreateCh('flood','Flood/Direct');
      appendMsgToPanel(chPanel,m);
      // Also add to the "All" panel
      if(chKeys['all'])appendMsgToPanel(chKeys['all'].panel,m);
    });
  }).catch(function(){});
}

function sendMsg(){
  var to=document.getElementById('msgTo').value;
  var text=document.getElementById('msgText').value.trim();
  var scope=document.getElementById('msgScope');
  var region=scope&&scope.style.display!=='none'?scope.value:'';
  if(!text)return;
  document.getElementById('msgText').value='';
  var body={to:to,text:text};
  if(region)body.region=region;
  fetch('/api/messages',{
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify(body)
  }).then(function(){setTimeout(loadMsgs,500);}).catch(function(){});
}

// Slash-command help tree.  Each entry is [command, description].
// Commands with <placeholders> or | fill the input; plain commands run immediately.
var SLASH={
  log:[
    ['log start','start logging packets'],['log stop','stop logging'],
    ['log erase','erase log file'],['log','dump packet log (serial only)']
  ],
  ver:[['ver','firmware version'],['board','board manufacturer']],
  reboot:[['reboot','reboot device'],['start ota','start OTA firmware update']],
  stats:[
    ['stats-packets','packet statistics'],['stats-radio','radio stats'],['stats-core','core stats'],
    ['clear stats','reset all statistics']
  ],
  advert:[['advert','send flood advertisement now']],
  clock:[
    ['clock','show current time'],['clock sync','sync clock from sender timestamp'],
    ['time <epoch>','set time to epoch seconds']
  ],
  neighbors:[
    ['neighbors','list neighboring nodes'],
    ['neighbor.remove <pubkey>','remove neighbor by public key hex']
  ],
  get:[
    ['get name','node name'],['get role','node role'],['get public.key','public key hex'],
    ['get freq','frequency MHz'],['get radio','full radio config (freq/bw/sf/cr)'],
    ['get sf','spreading factor'],['get bw','bandwidth'],['get cr','coding rate'],
    ['get tx','TX power dBm'],['get lat','latitude'],['get lon','longitude'],
    ['get password','admin password'],['get guest.password','guest password'],
    ['get owner.info','owner information'],
    ['get advert.interval','local advert interval (mins)'],
    ['get flood.advert.interval','flood advert interval (hours)'],
    ['get af','airtime budget factor'],
    ['get repeat','packet forwarding enabled'],
    ['get flood.max','max flood count'],
    ['get rxdelay','RX delay base'],['get txdelay','TX delay factor'],
    ['get direct.txdelay','direct TX delay factor'],
    ['get int.thresh','interference threshold'],
    ['get agc.reset.interval','AGC reset interval ms'],
    ['get multi.acks','extra ACK transmit count'],
    ['get allow.read.only','allow read-only clients'],
    ['get adc.multiplier','ADC voltage multiplier'],
    ['get bridge.type','bridge type (rs232/espnow/mqtt/none)'],
    ['get bridge.enabled','bridge enabled status'],
    ['get bridge.delay','bridge forward delay ms'],
    ['get bridge.source','bridge packet source filter'],
    ['get bridge.status','WiFi/MQTT status'],['get bridge.autostart','MQTT autostart 0/1'],
    ['get channel.0.psk','channel 0 decode key'],['get channel.1.psk','channel 1 decode key'],
    ['get channel.2.psk','channel 2 decode key'],['get channel.3.psk','channel 3 decode key']
  ],
  set:[
    ['set name <val>','node name'],['set sender_name <val>','display name for web-sent messages (empty = node name)'],
    ['set freq <MHz>','frequency (serial only)'],
    ['set radio <freq> <bw> <sf> <cr>','full radio config'],
    ['set sf <val>','spreading factor'],['set bw <val>','bandwidth'],['set cr <val>','coding rate'],
    ['set tx <dBm>','TX power'],
    ['set lat <val>','latitude'],['set lon <val>','longitude'],
    ['set password <val>','admin password'],['set guest.password <val>','guest password'],
    ['set owner.info <val>','owner info (use | for newlines)'],
    ['set advert.interval <mins>','local advert interval (60–240)'],
    ['set flood.advert.interval <hours>','flood advert interval (3–168)'],
    ['set af <val>','airtime budget factor'],
    ['set repeat on|off','packet forwarding'],
    ['set flood.max <val>','max flood count (0–64)'],
    ['set rxdelay <val>','RX delay base'],['set txdelay <val>','TX delay factor'],
    ['set direct.txdelay <val>','direct TX delay factor'],
    ['set int.thresh <val>','interference threshold'],
    ['set agc.reset.interval <ms>','AGC reset interval'],
    ['set multi.acks <val>','extra ACK transmit count'],
    ['set allow.read.only on|off','allow read-only clients'],
    ['set adc.multiplier <val>','ADC voltage multiplier'],
    ['set bridge.enabled 0|1','bridge enabled'],
    ['set bridge.delay <ms>','bridge forward delay (0–10000)'],
    ['set bridge.source 0|1','bridge source: 0=logTx 1=logRx'],
    ['set bridge.autostart 0|1','MQTT autostart on boot'],
    ['set bridge.mqtt.ssid <val>','WiFi SSID'],['set bridge.mqtt.wifi_pass <val>','WiFi password'],
    ['set bridge.mqtt.server <val>','MQTT server'],['set bridge.mqtt.port <val>','MQTT port'],
    ['set bridge.mqtt.topic <val>','MQTT topic'],
    ['set bridge.mqtt.user <val>','MQTT username'],['set bridge.mqtt.pass <val>','MQTT password'],
    ['set channel.0.psk <32|64hex>','channel 0 decode key'],['set channel.1.psk <32|64hex>','channel 1 decode key'],
    ['set channel.2.psk <32|64hex>','channel 2 decode key'],['set channel.3.psk <32|64hex>','channel 3 decode key']
  ],
  tempradio:[['tempradio <freq> <bw> <sf> <cr> <mins>','temporary radio override']],
  powersaving:[
    ['powersaving on','enable power saving'],
    ['powersaving off','disable power saving'],
    ['powersaving','show power saving status']
  ],
  bridge:[['bridge start','enable bridge'],['bridge stop','disable bridge']],
  wifi:[['wifi connect','reconnect WiFi+MQTT'],['wifi disconnect','disconnect WiFi+MQTT']],
  channel:[
    ['get channel.0.psk','ch0 key hex'],['set channel.0.psk <32|64hex>','set ch0 PSK'],['clear channel.0.psk','clear ch0 PSK'],
    ['get channel.0.name','ch0 name'],['set channel.0.name <name>','set ch0 name'],
    ['get channel.1.psk','ch1 key hex'],['set channel.1.psk <32|64hex>','set ch1 PSK'],['clear channel.1.psk','clear ch1 PSK'],
    ['get channel.1.name','ch1 name'],['set channel.1.name <name>','set ch1 name'],
    ['get channel.2.psk','ch2 key hex'],['set channel.2.psk <32|64hex>','set ch2 PSK'],['clear channel.2.psk','clear ch2 PSK'],
    ['get channel.2.name','ch2 name'],['set channel.2.name <name>','set ch2 name'],
    ['get channel.3.psk','ch3 key hex'],['set channel.3.psk <32|64hex>','set ch3 PSK'],['clear channel.3.psk','clear ch3 PSK'],
    ['get channel.3.name','ch3 name'],['set channel.3.name <name>','set ch3 name']
  ],
  region:[
    ['region','show region map'],['region load','begin region load mode'],
    ['region save','save region map'],['region home <name>','set home region'],
    ['region get <name>','show region info'],['region put <name> <parent>','add region'],
    ['region remove <name>','remove region'],
    ['region list allowed','list allowed regions'],['region list denied','list denied regions']
  ],
  setperm:[['setperm <pubkey> <perms>','set ACL client permissions']]
};

function fillCli(v){var i=document.getElementById('cliIn');i.value=v;i.focus();}

function cliSlash(key){
  var k=key.toLowerCase().trim();
  var o=document.getElementById('cliOut');
  var wrap=document.createElement('div');
  wrap.style.margin='3px 0';
  var hdr=document.createElement('div');
  hdr.style.color='var(--dim)';
  if(!k||!SLASH[k]){
    hdr.textContent='Command groups — click to expand:';
    wrap.appendChild(hdr);
    var row=document.createElement('div');
    row.style.cssText='display:flex;flex-wrap:wrap;gap:6px;margin-top:4px';
    Object.keys(SLASH).forEach(function(g){
      var s=document.createElement('span');
      s.textContent='/'+g;
      s.style.cssText='color:var(--blue);cursor:pointer;padding:2px 6px;border:1px solid var(--border);border-radius:3px';
      s.onclick=(function(gg){return function(){fillCli('/'+gg);runCli();};})(g);
      row.appendChild(s);
    });
    wrap.appendChild(row);
  } else {
    hdr.textContent='/'+k+' — click to use (★ = fills input for you to complete):';
    wrap.appendChild(hdr);
    SLASH[k].forEach(function(c){
      var cmd=c[0],desc=c[1];
      var needsInput=cmd.indexOf('<')>=0||cmd.indexOf('|')>=0;
      var div=document.createElement('div');
      div.style.cssText='display:flex;align-items:baseline;gap:8px;padding:2px 0';
      var s=document.createElement('span');
      s.textContent=(needsInput?'★ ':'')+cmd;
      s.style.cssText='color:var(--green);cursor:pointer;font-family:monospace;white-space:nowrap';
      s.onclick=needsInput
        ?(function(v){return function(){fillCli(v);};})(cmd)
        :(function(v){return function(){fillCli(v);runCli();};})(cmd);
      var d=document.createElement('span');
      d.textContent=desc;
      d.style.color='var(--dim)';
      div.appendChild(s);div.appendChild(d);
      wrap.appendChild(div);
    });
  }
  o.appendChild(wrap);
  o.scrollTop=o.scrollHeight;
}

function runCli(){
  var cmd=document.getElementById('cliIn').value.trim();
  if(!cmd)return;
  cliHist.unshift(cmd);cliHistIdx=-1;
  document.getElementById('cliIn').value='';
  var echo=document.createElement('div');
  echo.textContent='> '+cmd;
  echo.style.color='var(--blue)';
  document.getElementById('cliOut').appendChild(echo);
  if(cmd[0]==='/'){cliSlash(cmd.slice(1));return;}
  fetch('/api/cli',{
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({command:cmd})
  }).then(function(r){return r.json();}).then(function(d){
    cliAppend(d.result||'(no output)');
  }).catch(function(e){cliAppend('Error: '+e.message);});
}

function runCliSilent(cmd){
  return fetch('/api/cli',{
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({command:cmd})
  }).then(function(r){return r.json();});
}

function cliAppend(txt){
  var o=document.getElementById('cliOut');
  var div=document.createElement('div');
  div.textContent=txt;
  o.appendChild(div);
  o.scrollTop=o.scrollHeight;
}

function cliKey(e){
  if(e.key==='Enter'){runCli();return;}
  if(e.key==='ArrowUp'){
    cliHistIdx=Math.min(cliHistIdx+1,cliHist.length-1);
    document.getElementById('cliIn').value=cliHist[cliHistIdx]||'';
    e.preventDefault();
  }
  if(e.key==='ArrowDown'){
    cliHistIdx=Math.max(cliHistIdx-1,-1);
    document.getElementById('cliIn').value=cliHistIdx<0?'':(cliHist[cliHistIdx]||'');
    e.preventDefault();
  }
}

function loadLog(){
  var o=document.getElementById('logOut');
  o.textContent='Loading…';
  fetch('/api/log').then(function(r){return r.text();}).then(function(t){
    o.textContent=t||'(log is empty)';
    o.scrollTop=o.scrollHeight;
  }).catch(function(e){o.textContent='Error: '+e.message;});
}

function mqttCtrl(action){
  fetch('/api/mqtt/'+action,{method:'POST'}).then(function(){
    setTimeout(loadStatus,1200);
  }).catch(function(){});
}

function loadChannelStats(){
  fetch('/api/channel_stats').then(function(r){return r.json();}).then(function(stats){
    var rows='';
    stats.forEach(function(s){
      var snrTxt=s.avg_snr!=null?(s.avg_snr>0?'+':'')+s.avg_snr.toFixed(1)+' dB':'--';
      var snrCls=s.avg_snr>=5?'snr-good':(s.avg_snr>=-5?'snr-ok':'snr-bad');
      var psk=s.has_psk
        ?'<span style="color:var(--green)">&#10003;</span>'
        :'<span style="color:var(--red)" title="No PSK — traffic is encrypted but unreadable">&#8212;</span>';
      rows+='<tr>'
        +'<td style="font-family:monospace">['+esc(s.hash)+']</td>'
        +'<td>'+(s.name?esc(s.name):'<span class="dim">unknown</span>')+'</td>'
        +'<td>'+s.pkts+'</td>'
        +'<td class="dim">'+fmtAgo(s.secs_ago)+'</td>'
        +'<td class="'+snrCls+'">'+snrTxt+'</td>'
        +'<td>'+psk+'</td>'
        +'</tr>';
    });
    document.getElementById('chStatBody').innerHTML=rows||'<tr><td colspan="6" class="dim">No channel traffic seen yet.</td></tr>';
  }).catch(function(){});
}

function refresh(){
  loadStatus();
  if(curTab==='dash')loadChannelStats();
  if(curTab==='nbrs')loadNbrs();
  if(curTab==='msgs')loadMsgs();
}

initChTabs();        // create the "All" channel tab before any messages load
loadStatus();
loadChannels();      // populate compose To dropdown and pre-create channel sub-tabs
loadRegions();       // populate compose Scope dropdown from configured regions
loadMsgs();
loadChannelStats();  // initial Dashboard channel activity table
setInterval(refresh,3000);
</script>
</body>
</html>)rawhtml";

#endif // WITH_WEB_INTERFACE
