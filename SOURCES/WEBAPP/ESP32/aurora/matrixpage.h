// Matrix page (plugin kind "matrix"), served by the firmware instead of a generated dsp.html.
// The layout, Source list, gain range, Slot limits and Biquad budget all come from GET /matrix;
// the plot is computed here from GET /matrix/coeffs.
const char matrixpage_html[] = "<!DOCTYPE html>"
R"!^!(
<html lang="en">
<head>
<meta name="viewport" content="width=device-width, initial-scale=1" charset="utf-8">
<title>AURORA DSP</title>
<link rel="stylesheet" href="dark.css">
<style>
body{padding:0 8px;max-width:640px;margin:0 auto}
h2{margin:14px 0 6px}
.row{display:flex;flex-wrap:wrap;align-items:center;gap:6px;margin:6px 0}
.row *{box-sizing:border-box}
.name{flex:1 1 80px;min-width:0;width:80px}
input.name{height:30px}
select{flex:1 1 80px;min-width:0;width:80px;height:30px;border:1px solid #808080;border-radius:2px;color:#fff}
.gain{flex:2 1 100px;min-width:0}
.gv{flex:0 0 64px;text-align:right}
input.num{flex:0 0 76px;width:76px;height:30px;text-align:right}
button.mute{width:56px;height:30px;border-top-width:1px}
button.on{background-color:#802020}
#msg{color:#e08080;min-height:18px}
#presets button{width:44px;height:30px;border-top-width:1px}
.file{display:inline-block;box-sizing:border-box;height:30px;line-height:28px;padding:0 10px;border:1px solid #808080;border-radius:2px;color:#fff;text-decoration:none;cursor:pointer}
.slot{border:1px solid #606060;border-radius:3px;padding:2px 8px;margin:8px 0}
.lbl{flex:0 0 40px}
#plot{width:100%;height:200px;background:#101010;border:1px solid #606060}
button.wide{width:100%;height:32px;border-top-width:1px}
button:disabled{opacity:.4}
.linkset{border-left:3px solid #606060;padding-left:6px;margin:4px 0 8px}
.linkset b{flex:0 0 54px}
.linkset button{width:auto;min-width:58px;height:28px;border-top-width:1px}
.top{display:flex;align-items:center;justify-content:space-between;gap:8px}
#wifi,#abt{position:fixed;top:0;right:0;bottom:0;left:0;background:rgba(0,0,0,.6);overflow-y:auto;z-index:10}
#wifi .box,#abt .box{box-sizing:border-box;max-width:400px;width:calc(100% - 16px);margin:40px auto;padding:8px 12px;background:#202020;border:1px solid #606060;border-radius:3px}
#wifi input{box-sizing:border-box;width:100%;height:30px;margin-bottom:6px}
#wifi input[type=checkbox]{width:auto;height:auto;margin:0 6px 0 0;vertical-align:middle}
#wifi .check{display:block;margin-bottom:6px}
#wifi .row button,#abt .row button{flex:1 1 0;height:32px;border-top-width:1px}
#apwarn{color:#ff6060;font-weight:bold}
#wmsg,#smsg,#tmsg{min-height:18px}
#wstat{margin-bottom:6px;overflow-wrap:anywhere}
#abt{background:#181818}  /* opaque: the active Preset letter must not show through */
#trounds{width:76px;height:30px}
.tr .tv{flex:0 0 64px;text-align:right}
.tr .tp{flex:0 0 44px;text-align:right}
.tr .tb{flex:1 1 40px;height:12px;background:#303030}
.tr .tb i{display:block;height:100%;background:#60c060}
.tr .tw{flex:0 0 56px}
</style>
</head>
<body onload="load()">
<div class="top"><h1>Aurora DSP</h1><button class="mute" onclick="openWifi()">WiFi</button></div>
<div id="msg"></div>
<div id="main">
<div class="row"><div id="presets" class="row" style="margin:0"></div><span id="pname"></span><a id="export" class="file" href="/preset.param" download>Export</a><label class="file">Import<input type="file" accept=".json,application/json" onchange="importFile(this)" hidden></label><button class="mute" title="A/B test: compare Presets blind" onclick="abOpen()">Test</button></div>
<div class="row"><span class="lbl">Vol</span><input type="range" id="vol" class="gain" min="-80" max="0" step="0.5" value="-60" oninput="sendVol(this.value)"><span class="gv" id="volv"></span></div>
<h2>Buses</h2>
<div id="buslinks"></div>
<div id="buses"></div>
<h2>Outputs</h2>
<div id="outlinks"></div>
<div id="outs"></div>
<h2>SPDIF out</h2>
<div class="row"><select id="spdif" onchange="send('spdif',0,{sel:parseInt(this.value)})"></select></div>
</div>
<div id="flt" style="display:none">
<div class="row"><button class="mute" onclick="closeF()">Back</button><h2 id="ftitle" style="margin:0"></h2></div>
<canvas id="plot" width="600" height="200"></canvas>
<div id="top"></div>
<div id="budget"></div>
<div id="slots"></div>
<button id="add" class="wide" onclick="addSlot()">+ add</button>
</div>
<div id="wifi" style="display:none"><div class="box">
<h2>WiFi network</h2>
<div id="wstat"></div>
<label for="ssid">Network name (SSID)</label><input type="text" id="ssid" maxlength="32" autocomplete="off" autocapitalize="none" spellcheck="false">
<label for="pwd">Password (8-63 characters, empty: open network; the stored one is not shown)</label><input type="password" id="pwd" autocomplete="new-password">
<label class="check"><input type="checkbox" id="apoff" onchange="storeApOff()">Switch the access point off while connected (the DSP is then only reachable on its WiFi IP; the access point comes back when the connection is lost)</label>
<div id="smsg"></div>
<div class="row"><button onclick="joinWifi()">Join network</button><button onclick="forgetWifi()">Forget network</button></div>
<h2>Access point</h2>
<div id="apwarn" style="display:none"></div>
<label for="apname">Name (1-32 characters)</label><input type="text" id="apname" maxlength="32" autocomplete="off">
<label for="apchannel">Channel (1-13)</label><input type="number" id="apchannel" min="1" max="13" inputmode="numeric">
<label for="pwdap">Password (at least 8 characters, empty: open AP)</label><input type="password" id="pwdap" autocomplete="new-password">
<div id="wmsg"></div>
<div class="row"><button onclick="storeAp()">Store AP config</button><button onclick="closeWifi()">Close</button></div>
</div></div>
<div id="abt" style="display:none"><div class="box">
<h2>A/B test</h2>
<div class="row"><span class="lbl">Vol</span><input type="range" id="tvol" class="gain" min="-80" max="0" step="0.5" value="-60" oninput="sendVol(this.value)"><span class="gv" id="tvolv"></span></div>
<div id="tmsg"></div>
<div id="tset">
<p>Tick 2 to 4 Presets. Each Round plays them behind the labels 1-4 in a new order; pick the one you like best.</p>
<div id="tpre" class="row"></div>
<label for="trounds">Rounds (3-50)</label><input type="number" id="trounds" min="3" max="50" value="10" inputmode="numeric">
<div class="row"><button id="tstart" onclick="abStart()">Start</button><button onclick="abClose()">Cancel</button></div>
</div>
<div id="trun" style="display:none">
<h2 id="tround"></h2>
<div id="tlabels" class="row"></div>
<div class="row"><button id="tvote" onclick="abVote()">This one is best</button><button onclick="abEnd()">Abort</button></div>
<p>Reloading the page ends the test without switching back to the previous Preset.</p>
</div>
<div id="tres" style="display:none"></div></div></div>
<script>
var st=null,seq=0,seen=0,late=false,view=null,cf=null,cseq=0,csig="",ct=0,activePreset=-1;
var presetUsed=[];  // per Preset slot: has a Preset file (GET /preset "used")
var presetNames=[];  // per Preset slot: its Preset name, "" = none (GET /preset "names")
function say(t){document.getElementById("msg").textContent=document.getElementById("tmsg").textContent=t||"";}  // tmsg: the Preset test overlay
function waitIdle(){return fetch("/idle").then(function(r){return r.text();}).then(function(t){
  if(t=="1")return;return new Promise(function(r){setTimeout(r,100);}).then(waitIdle);});}
// Settles once the matrix and the master volume have been read (or failed to).
function load(){
  var n=++seq;
  fetch("/preset").then(function(r){return r.json();}).then(function(p){presetUsed=p.used||[];presetNames=p.names||[];drawPresets(p.pre);});
  return Promise.all([
   fetch("/matrix").then(function(r){return r.json();}).then(function(s){if(n<seen)return;seen=n;st=s;activePreset=s.preset;draw();})
    .catch(function(){say("Cannot read the matrix");}),
   fetch("/mvol").then(function(r){return r.json();}).then(function(v){if(!vt)showVol(v.vol);}).catch(function(){})]);  // not under a drag
}
function showVol(v){
  document.getElementById("vol").value=document.getElementById("tvol").value=v;  // tvol: the Preset test overlay
  document.getElementById("volv").textContent=document.getElementById("tvolv").textContent=parseFloat(v).toFixed(1)+" dB";
}
// The master volume is global, not part of a Preset: it is sent during a Preset switch too.
var vt=0,vp=null;
function sendVol(v){  // while dragging: at once, then at most every 100 ms with the latest value
  showVol(v);vp=v;if(vt)return;
  var go=function(){var x=vp;vp=null;if(x===null){vt=0;return;}
    fetch("/mvol",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({vol:parseFloat(x)})});
    vt=setTimeout(go,100);};
  go();
}
// The A-D buttons: on(i) marks slot i, off(i) disables it, a click runs click+"(i)".
function presetButtons(on,click,off){
  var h="";for(var i=0;i<4;i++)h+="<button class='"+(on(i)?"on":"")+"'"+(off&&off(i)?" disabled":"")+" onclick='"+click+"("+i+")'>"+"ABCD"[i]+"</button>";
  return h;
}
// The active Preset's name next to the row; committed like a Channel name, as a normal edit.
// Not rebuilt while it is being typed in.
function drawPresetName(){
  var a=document.activeElement;
  presetNames[st.preset]=st.presetname||"";
  if(a&&a.id=="presetname")return;
  document.getElementById("pname").innerHTML="<input type='text' class='name' id='presetname' maxlength='16' placeholder='Preset name' value=\""
   +esc(st.presetname||"")+"\" onchange='sendPresetName(this.value)'>";
}
function sendPresetName(v){return post({ch:"preset",name:v,preset:activePreset});}
// "A" or "A · name": a Preset with its name, where only real names may show (Evaluation).
function presetLabel(p){return "ABCD"[p]+(presetNames[p]?" \u00b7 "+esc(presetNames[p]):"");}
function drawPresets(cur){  // during a Preset test no slot is marked: the row is hidden, not removed
  document.getElementById("presets").innerHTML=presetButtons(function(i){return i==cur&&!ab;},"if(!ab)preset");
}
// From the click until the state has been reloaded no Preset edit is sent: edits made meanwhile
// are dropped rather than sent to the new Preset. Settles once that is over (the Preset test chains on it).
var switching=false;
function editsBlocked(){return switching||ab;}  // ab: no edit during a Preset test
function preset(p){
  if(switching)return Promise.resolve();
  switching=true;
  say("Switching preset...");
  return fetch("/preset",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({pre:p})})
   .then(function(r){return r.text().then(function(t){if(!r.ok)throw new Error(t||"Preset switch failed");});})
   .then(waitIdle).then(function(){say("");},function(e){say(e.message);})
   .then(load).then(function(){switching=false;});
}
// Import replaces the active Preset with a Preset file (the inverse of Export); the firmware
// writes the difference and Autosave stores it. Edits wait for the reply like during a switch.
function importFile(el){
  var f=el.files&&el.files[0];el.value="";  // the same file can be chosen again
  if(f)f.text().then(importPreset,function(){say("Import failed: cannot read the file");});
}
function importPreset(text){
  if(editsBlocked())return;
  switching=true;
  say("Importing preset...");
  fetch("/matrix/import?preset="+activePreset,{method:"POST",headers:{"Content-Type":"application/json"},body:text})
   .then(function(r){return r.text().then(function(t){
     if(!r.ok){say("Import failed: "+t);if(r.status==409)return load();return;}
     seen=++seq;st=JSON.parse(t);activePreset=st.preset;draw();say("Preset imported");});},
    function(){say("Import failed: no answer from the DSP");})
   .catch(function(){say("Import failed: unreadable reply from the DSP");})
   .then(function(){switching=false;});  // always: a failure above must not lock the page
}
// Sends a patch, the reply is the new state or the reason it was refused.
function send(ch,idx,f){
  var p={ch:ch,idx:idx,preset:activePreset};for(var k in f)p[k]=f[k];
  return post(p);
}
function post(p){
  if(editsBlocked())return Promise.resolve();
  var n=++seq;  // replies older than one already shown are ignored
  return fetch("/matrix",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(p)})
   .then(function(r){return r.text().then(function(t){
     if(r.ok){if(n<seen)return;seen=n;st=JSON.parse(t);say("");}else{say(t);if(r.status==409)load();}
     draw();});});
}
// ---- text fields: a value is sent on Commit (Enter, Tab or click outside), never while typing.
// data-v is the value the field stands for; a Commit of that value sends nothing, Escape restores it.
// i is the Slot index, -1 for a Channel value (gain, delayUs).
function fmt(key,v){return String(key=="delayUs"?parseFloat(v.toFixed(1)):parseFloat(v.toPrecision(6)));}
function field(id,ch,idx,i,key,v){
  var t=fmt(key,v);
  return "<input type='text' class='num' id='"+id+"'"+(key=="gain"?"":" inputmode='decimal'")  // phone decimal pads lack a minus
   +" value='"+t+"' data-v='"+t+"' data-ch='"+ch+"' data-idx='"+idx+"' data-i='"+i+"' data-key='"+key+"'"
   +" onkeydown='fieldKey(event,this)' onblur='commit(this)'>";
}
function num(t){t=String(t).trim().replace(",",".");return t===""?NaN:Number(t);}
function fieldKey(e,el){
  if(e.key=="Enter"){e.preventDefault();commit(el);}
  else if(e.key=="Escape"){e.preventDefault();el.value=el.dataset.v;}
}
// Text that is no number is sent as null: the firmware's reply names the problem and the field reverts.
function commit(el){
  var d=el.dataset,x=num(el.value);
  if(editsBlocked()){el.value=d.v;return;}  // dropped; the reload shows the loaded value
  if(el.value==d.v||x===num(d.v))return;
  d.v=el.value;  // until the reply: Enter followed by Tab sends once
  var i=parseInt(d.i),f={};
  if(i<0)f[d.key]=x;else{f.slot={op:"set",i:i};f.slot[d.key]=x;}
  send(d.ch,parseInt(d.idx),f);
}
// Without a rebuild the fields still follow the model, except the one being typed in.
function syncFields(){
  var a=document.activeElement;
  Array.prototype.forEach.call(document.querySelectorAll("input.num"),function(el){
    var d=el.dataset,c=(d.ch=="bus"?st.buses:st.outs)[d.idx],i=parseInt(d.i);
    var o=c&&(i<0?c:c.slots[i]);
    if(!o||typeof o[d.key]!="number"||(el===a&&el.value!=d.v))return;
    el.value=d.v=fmt(d.key,o[d.key]);
  });
}
function esc(s){return s.replace(/&/g,"&amp;").replace(/"/g,"&quot;").replace(/</g,"&lt;");}
function opts(list,sel,val,txt){
  return list.map(function(o,i){var v=val(o,i);return "<option value='"+v+"'"+(v==sel?" selected":"")+">"+txt(o)+"</option>";}).join("");
}
function linkOpts(ch,sel){
  var ids=ch=="bus"?["","bus"]:["","A","B","C","D"];
  return ids.map(function(id){var name=id=="bus"?"Bus link":id||"No link";
    return "<option value='"+id+"'"+(id==sel?" selected":"")+">"+name+"</option>";}).join("");
}
function row(ch,idx,c,sel){
  return "<div class='row'><input type='text' class='name' maxlength='16' value=\""+esc(c.name)+"\" onchange=\"send('"+ch+"',"+idx+",{name:this.value})\">"
   +sel
   +"<select title='Link group' onchange=\"send('"+ch+"',"+idx+",{link:this.value})\">"+linkOpts(ch,c.link)+"</select>"
   +"<button class='mute"+(c.mute?" on":"")+"' onclick=\"send('"+ch+"',"+idx+",{mute:"+!c.mute+"})\">"+(c.mute?"Muted":"Mute")+"</button>"
   +"<button class='mute' onclick=\"openF('"+ch+"',"+idx+")\">Filter</button>"
   +field("g"+ch+idx,ch,idx,-1,"gain",c.gain)+"<span>dB</span></div>";
}
function sendLink(id,key,value){
  var p={linkGroup:id,preset:activePreset};p[key]=value;
  return post(p);
}
function linkPanel(scope,l){
  var keys=["mute","filters","gain","delay"],names=["Mute","Filters","Gain","Delay"];
  return "<div class='row linkset'><b>"+(scope=="bus"?"Bus link":l.id)+"</b>"+keys.map(function(k,i){
    var disabled=scope=="bus"&&k=="delay";
    return "<button"+(disabled?" disabled":"")+" class='"+(l[k]?"on":"")+"' onclick=\"sendLink('"+l.id+"','"+k+"',"+!l[k]+")\">"+names[i]+"</button>";
  }).join("")+"</div>";
}
function busy(sel){
  var a=document.activeElement;
  return a&&(a.tagName=="SELECT"||a.tagName=="INPUT")&&a.closest(sel);
}
function draw(){
  if(!st)return;
  if(view){drawF();return;}
  drawPresetName();
  var b="",o="";
  st.buses.forEach(function(c,i){
    b+=row("bus",i,c,"<select onchange=\"send('bus',"+i+",{source:parseInt(this.value)})\">"+opts(st.sources,c.source,function(s){return s.v;},function(s){return s.n;})+"</select>");
  });
  st.outs.forEach(function(c,i){
    o+=row("out",i,c,"<select onchange=\"send('out',"+i+",{route:parseInt(this.value)})\">"+opts(st.routes,c.route,function(s,j){return j;},function(s){return s;})+"</select>");
  });
  // do not rebuild under an open select or a field being typed in
  if(busy("#buses,#outs")){late=true;syncFields();return;}
  late=false;
  document.getElementById("buslinks").innerHTML=linkPanel("bus",st.links.bus);
  document.getElementById("outlinks").innerHTML=st.links.outs.map(function(l){return linkPanel("out",l);}).join("");
  document.getElementById("spdif").innerHTML=opts(st.spdifopts,st.spdifout,function(s){return s.v;},function(s){return s.n;});
  document.getElementById("buses").innerHTML=b;
  document.getElementById("outs").innerHTML=o;
}
// A deferred rebuild must not land between pointerdown and pointerup: the button under the
// pointer would be replaced and the click lost. So wait for the pointer to be released.
var down=false;
document.addEventListener("pointerdown",function(){down=true;});
function released(){down=false;if(late)setTimeout(draw,0);}  // after the click has been dispatched
document.addEventListener("pointerup",released);
document.addEventListener("pointercancel",released);
document.addEventListener("focusout",function(){if(late&&!down)setTimeout(draw,0);});

// ---- filter page
function chan(){return (view.ch=="bus"?st.buses:st.outs)[view.idx];}
function openF(ch,i){view={ch:ch,idx:i};cf=null;cseq++;csig="";late=false;  // drop replies for the previous Channel
  document.getElementById("main").style.display="none";document.getElementById("flt").style.display="";
  drawF();}
function closeF(){view=null;late=false;
  document.getElementById("main").style.display="";document.getElementById("flt").style.display="none";draw();}
function param(i,lbl,key,s){
  return "<div class='row'><span class='lbl'>"+lbl+"</span>"+field("s"+i+key,view.ch,view.idx,i,key,s[key])
   +"<span>"+UNIT[key]+"</span></div>";
}
var UNIT={freq:"Hz",q:"",gain:"dB",slope:""};
// the parameters each Slot type has
var KEYS={peq:["freq","q","gain"],hp:["freq"],lp:["freq"],lshelf:["freq","gain","slope"],hshelf:["freq","gain","slope"],phase:["freq","q"]};
var TNAME={peq:"PEQ",hp:"HP",lp:"LP",lshelf:"Low shelf",hshelf:"High shelf",phase:"Phase"};
function cname(o){return o.d?o.d.toUpperCase()+" "+o.o:TNAME[o.t];}
function addSlot(){send(view.ch,view.idx,{slot:{op:"add"}});}
function delSlot(i){send(view.ch,view.idx,{slot:{op:"del",i:i}});}
function bypass(i,b){send(view.ch,view.idx,{slot:{op:"set",i:i,bypass:b}});}
function setType(i,t){send(view.ch,view.idx,{slot:{op:"set",i:i,type:t}});}
function setOrder(i,j){var o=st.choices[j];send(view.ch,view.idx,{slot:{op:"set",i:i,design:o.d,order:o.o}});}
// Type and order lists: entries that need more biquads than the Slot may take (s.avail) are greyed out.
function typeSel(i,s){
  return "<select class='name' onchange=\"setType("+i+",this.value)\">"+st.types.map(function(t){
    var ok=st.choices.some(function(o){return o.t==t&&o.c<=s.avail;});
    return "<option value='"+t+"'"+(t==s.type?" selected":"")+(ok?"":" disabled")+">"+TNAME[t]+"</option>";}).join("")+"</select>";
}
function orderSel(i,s){
  if(s.type!="hp"&&s.type!="lp")return "";
  return "<select onchange=\"setOrder("+i+",this.value)\">"+st.choices.map(function(o,j){
    if(o.t!=s.type)return "";
    return "<option value='"+j+"'"+(o.d==s.design&&o.o==s.order?" selected":"")+(o.c<=s.avail?"":" disabled")+">"+cname(o)+" ("+o.c+")</option>";}).join("")+"</select>";
}
function sendTop(f){send(view.ch,view.idx,f);}
// gain block (all Channels), polarity and delay (Outputs only)
function topHtml(c){
  var h="<div class='slot'><div class='row'><b class='name'>Gain</b>"
   +"<button class='mute"+(c.gainBypass?" on":"")+"' onclick='sendTop({gainBypass:"+!c.gainBypass+"})'>Bypass</button></div>"
   +"<div class='row'>"+field("tg"+view.ch+view.idx,view.ch,view.idx,-1,"gain",c.gain)+"<span>dB</span></div></div>";
  if(view.ch!="out")return h;
  h+="<div class='slot'><div class='row'><b class='name'>Polarity</b>"
   +"<button class='mute"+(c.polarity?" on":"")+"' onclick='sendTop({polarity:"+!c.polarity+"})'>"+(c.polarity?"Inverted":"Normal")+"</button></div></div>";
  h+="<div class='slot'><div class='row'><b class='name'>Delay</b>"
   +"<button class='mute"+(c.delayBypass?" on":"")+"' onclick='sendTop({delayBypass:"+!c.delayBypass+"})'>Bypass</button></div>"
   +"<div class='row'>"+field("td"+view.ch+view.idx,view.ch,view.idx,-1,"delayUs",c.delayUs)
   +"<span>&micro;s (max "+Math.floor(st.delaymax*1e6/48000)+")</span></div>"
   +"<div class='row' id='dapp'>"+applied(c)+"</div></div>";
  return h;
}
function applied(c){
  return "Applied: "+c.delayUs.toFixed(1)+" &micro;s = "+c.delaySamples+" samples = "+c.delayMm.toFixed(1)+" mm"+(c.delayBypass?" (bypassed)":"");
}
// While an input has focus the DOM stays, but the fields and labels follow the model.
function refreshLabels(c){
  var e;
  if(view.ch=="out"&&(e=document.getElementById("dapp")))e.innerHTML=applied(c);
  syncFields();
}
function drawF(){
  var c=chan();
  document.getElementById("ftitle").textContent=c.name;
  document.getElementById("budget").textContent="Biquads: "+c.used+" of "+c.budget+" used, "+c.free+" free";
  document.getElementById("add").disabled=!st.choices.some(function(o){return o.c<=c.free;});  // nothing fits any more
  var sig=JSON.stringify({gain:c.gain,gainBypass:c.gainBypass,slots:c.slots});
  if(sig!=csig){csig=sig;if(!ct)ct=setTimeout(function(){ct=0;if(view)coeffs();},150);}
  if(busy("#slots,#top")){late=true;refreshLabels(c);return;}
  late=false;
  document.getElementById("top").innerHTML=topHtml(c);
  var h="";
  c.slots.forEach(function(s,i){
    var k=KEYS[s.type];
    h+="<div class='slot'><div class='row'>"+typeSel(i,s)+orderSel(i,s)
     +"<button class='mute"+(s.bypass?" on":"")+"' onclick='bypass("+i+","+!s.bypass+")'>Bypass</button>"
     +"<button class='mute' onclick='delSlot("+i+")'>Delete</button></div>"
     +param(i,"Freq","freq",s)
     +(k.indexOf("q")<0?"":param(i,"Q","q",s))
     +(k.indexOf("gain")<0?"":param(i,"Gain","gain",s))
     +(k.indexOf("slope")<0?"":param(i,"Slope","slope",s))+"</div>";
  });
  document.getElementById("slots").innerHTML=h;
}
function coeffs(){
  var n=++cseq;
  fetch("/matrix/coeffs?ch="+view.ch+"&idx="+view.idx).then(function(r){return r.json();})
   .then(function(c){if(n==cseq&&view){cf=c;plot();}});
}
// Magnitude of the firmware's (8.24 quantized) biquads, H=(b0+b1z^-1+b2z^-2)/(1-a1z^-1-a2z^-2).
function mag(f){
  var c=chan(),w=2*Math.PI*f/cf.fs,c1=Math.cos(w),s1=Math.sin(w),c2=Math.cos(2*w),s2=Math.sin(2*w),db=c.gainBypass?0:c.gain;
  cf.bq.forEach(function(q){
    var nr=q[0]+q[1]*c1+q[2]*c2,ni=-q[1]*s1-q[2]*s2,dr=1-q[3]*c1-q[4]*c2,di=q[3]*s1+q[4]*s2;
    db+=10*Math.log10((nr*nr+ni*ni)/(dr*dr+di*di));
  });
  return db;
}
function plot(){
  var cv=document.getElementById("plot"),g=cv.getContext("2d"),W=cv.width,H=cv.height,R=24;
  var X=function(f){return W*Math.log(f/20)/Math.log(1000);},Y=function(d){return H/2-H/2*Math.max(-R,Math.min(R,d))/R;};
  g.clearRect(0,0,W,H);g.strokeStyle="#404040";g.fillStyle="#909090";g.font="11px sans-serif";g.lineWidth=1;
  [100,1000,10000].forEach(function(f){g.beginPath();g.moveTo(X(f),0);g.lineTo(X(f),H);g.stroke();g.fillText(f>=1000?f/1000+"k":f,X(f)+2,H-2);});
  [-12,0,12].forEach(function(d){g.beginPath();g.moveTo(0,Y(d));g.lineTo(W,Y(d));g.stroke();g.fillText(d+" dB",2,Y(d)-2);});
  if(!cf)return;
  g.strokeStyle="#60c060";g.lineWidth=2;g.beginPath();
  for(var i=0;i<=240;i++){var f=20*Math.pow(1000,i/240),y=Y(mag(f));if(i)g.lineTo(X(f),y);else g.moveTo(X(f),y);}
  g.stroke();
}

// ---- Preset test statistics: exact multinomial test of the Votes against "all Presets equally liked"
// votes: index of the chosen Preset per Round; k: number of Presets in the test.
function tally(votes,k){
  var counts=[],i;
  for(i=0;i<k;i++)counts[i]=0;
  votes.forEach(function(v){counts[v]++;});
  return {counts:counts,pct:counts.map(function(c){return votes.length?100*c/votes.length:0;})};
}
function sum(a){return a.reduce(function(s,v){return s+v;},0);}  // the Votes in counts
// p = sum of the probabilities of all outcomes no more likely than the observed one (k<=4, N<=50: cheap).
function exactP(counts){
  var k=counts.length,n=sum(counts),lf=[0],i,p=0,obs,c=[];
  for(i=1;i<=n;i++)lf[i]=lf[i-1]+Math.log(i);
  var lp=function(c){return lf[n]-c.reduce(function(s,v){return s+lf[v];},0)-n*Math.log(k);};
  obs=lp(counts);
  (function walk(j,left){  // every split of the n Votes over the k Presets
    if(j==k-1){c[j]=left;var l=lp(c);if(l<=obs+1e-9)p+=Math.exp(l);return;}
    for(var v=0;v<=left;v++){c[j]=v;walk(j+1,left-v);}
  })(0,n);
  return Math.min(1,p);
}
// winner: index of the single leader, -1 on a tie for first; p: null below 3 Votes; sig: a winner with p < 0.05.
function verdict(counts){
  var n=sum(counts),top=Math.max.apply(null,counts),lead=counts.filter(function(c){return c==top;}).length,p;
  var w=n&&lead==1?counts.indexOf(top):-1;
  p=n<3?null:exactP(counts);
  return {winner:w,p:p,sig:w>=0&&p!==null&&p<0.05};
}

// ---- Preset test ("A/B test"): the ticked Presets play behind labels 1..k, reshuffled every Round.
// Client only: a reload ends the test without switching back.
function abEl(id){return document.getElementById(id);}
var abSel=[];  // ticked Preset slots in the setup dialog
function abShow(id){abEl("abt").style.display="block";["tset","trun","tres"].forEach(function(x){abEl(x).style.display=x==id?"":"none";});}
// sel, n: ticks and Rounds to start with ("Run again"); none: no ticks, 10 Rounds.
function abOpen(sel,n){
  if(ab)return;abSel=(sel||[]).filter(function(i){return presetUsed[i];});say("");showVol(abEl("vol").value);
  abEl("trounds").value=n||10;abShow("tset");abTicks();
}
function abTicks(){
  abEl("tpre").innerHTML=presetButtons(function(i){return abSel.indexOf(i)>=0;},"abTick",function(i){return !presetUsed[i];});abEl("tstart").disabled=abSel.length<2;
}
function abTick(i){var j=abSel.indexOf(i);if(j<0)abSel.push(i);else abSel.splice(j,1);abTicks();}
function abClose(){abEl("abt").style.display="none";}
// The running test, null otherwise: pre = ticked slots (ascending), n = Rounds, votes = per Round the
// index into pre of the chosen Preset, order = label -> index into pre, cur = label playing (-1: none
// yet this Round), back = Preset to switch back to, sw = the last switch (the switch back waits for it).
// While it runs the overlay covers the page and post() sends no edit, so Autosave cannot write into a
// Preset under test; only the Master volume (not part of a Preset) stays usable.
var ab=null;
function abStart(){
  if(abSel.length<2||switching)return;
  var n=Math.max(3,Math.min(50,parseInt(abEl("trounds").value)||10));
  ab={pre:abSel.slice().sort(function(a,b){return a-b;}),n:n,votes:[],order:[],cur:-1,back:activePreset};
  abShow("trun");abEl("tround").textContent="Waiting for the DSP...";abEl("tlabels").innerHTML="";abEl("tvote").disabled=true;
  var t=ab;
  t.sw=waitIdle().then(function(){t.back=activePreset;if(!t.end)abRound();});  // the DSP is idle, the active Preset settled
  // An edit Autosave has not stored yet is not lost: POST /preset saves the old Preset first (ADR 0003).
}
function shuffle(a){  // Fisher-Yates
  for(var i=a.length-1;i>0;i--){var j=Math.floor(Math.random()*(i+1)),x=a[i];a[i]=a[j];a[j]=x;}
  return a;
}
function abRound(){ab.order=shuffle(ab.pre.map(function(p,i){return i;}));ab.cur=-1;abDraw();}
function abDraw(){
  abEl("tround").textContent="Round "+(ab.votes.length+1)+"/"+ab.n;
  abEl("tlabels").innerHTML=ab.order.map(function(x,i){
    return "<button class='"+(i==ab.cur?"on":"")+"' onclick='abPick("+i+")'>"+(i+1)+"</button>";}).join("");
  abEl("tvote").disabled=ab.cur<0;
}
// A label plays the Preset behind it through the normal switch; it is current only once that has loaded.
function abPick(i){
  if(!ab||ab.end||switching)return;
  var t=ab,r=t.votes.length,p=t.pre[t.order[i]];
  t.cur=-1;abDraw();
  t.sw=preset(p).then(function(){if(!t.end&&t.votes.length==r&&activePreset==p){t.cur=i;abDraw();}});
}
function abVote(){
  if(!ab||ab.end||ab.cur<0||switching)return;
  ab.votes.push(ab.order[ab.cur]);
  if(ab.votes.length<ab.n)abRound();else abEnd();
}
// Last Round or Abort: switch back to the remembered Preset, then hand the Votes to the Evaluation.
function abEnd(){
  var t=ab;if(!t||t.end)return;
  t.end=true;abEl("tround").textContent="Switching back...";abEl("tlabels").innerHTML="";abEl("tvote").disabled=true;
  t.sw.then(function(){return preset(t.back);}).then(function(){ab=null;drawPresets(activePreset);abClose();abResult(t.pre,t.votes,t.n);});
}
// Evaluation: pre = the ticked Preset slots (0-3 = A-D, ascending), votes = per Round the index into
// pre of the chosen Preset, n = Rounds set (fewer Votes: aborted). Per Preset its real letter, Votes,
// share and bar, the winner marked, then the verdict. No Votes: nothing to show.
function abResult(pre,votes,n){
  if(!votes.length)return;
  var t=tally(votes,pre.length),v=verdict(t.counts),
      h=votes.length<n?"<p>Aborted after "+votes.length+" of "+n+" Rounds.</p>":"";
  pre.forEach(function(p,i){
    var c=t.counts[i],pc=Math.round(t.pct[i]);
    h+="<div class='row tr'><b class='lbl'>"+presetLabel(p)+"</b><span class='tv'>"+c+" Vote"+(c==1?"":"s")+"</span><span class='tp'>"+pc+
       "%</span><span class='tb'><i style='width:"+pc+"%'></i></span><b class='tw'>"+(i==v.winner?"Winner":"")+"</b></div>";
  });
  // p is rounded for display: it carries float noise from the sum.
  h+="<p>"+(v.winner<0?"No winner: tie for first.":"Winner: "+presetLabel(pre[v.winner])+". "+(v.p===null?"Fewer than 3 Votes: no p-value.":
     (v.p<0.001?"p &lt; 0.001":"p = "+(+v.p.toFixed(3)))+": "+(v.sig?"significant.":"not significant, could be chance.")))+"</p>";
  abEl("tres").innerHTML=h+"<div class='row'><button onclick='abClose()'>Close</button><button onclick='abOpen(["+pre+"],"+n+")'>Run again</button></div>";
  abShow("tres");
}

// ---- WiFi dialog: the same checks, messages and open-AP warning as the settings UI (aurora.js)
function wifiEl(id){return document.getElementById(id);}
function wsay(t){wifiEl("wmsg").textContent=t||"";}
var wtimer=null;
function openWifi(){
  wifiEl("wifi").style.display="block";wsay("");ssay("");
  wifiEl("pwdap").value="";wifiEl("pwd").value="";  // stored passwords are never shown
  return fetch("/wificonfig").then(function(r){return r.json();}).then(function(d){
    wifiEl("ssid").value=d.ssid;wifiEl("apoff").checked=d.apoff===true;
    wifiEl("apname").value=d.apname;wifiEl("apchannel").value=d.apchannel;
    var w=wifiEl("apwarn");w.textContent=apWarningText(d);w.style.display=w.textContent?"block":"none";
    showWifiState(d);
  }).catch(function(){wsay("Cannot read the WiFi settings");});
}
function closeWifi(){wifiEl("wifi").style.display="none";clearTimeout(wtimer);wtimer=null;}
// Shows the connection state and keeps it fresh while the dialog is open; the inputs stay as typed.
function showWifiState(d){
  var ap=!d.apoff?"the access point stays available.":d.apup===false?"the access point is off while connected.":"the access point switches off shortly.";
  wifiEl("wstat").textContent=!d.ssid?"No WiFi network stored. The DSP is reachable on its access point."
   :d.connected?"Connected to \""+d.ssid+"\", IP "+d.ip+". In that network the DSP is reachable at http://"+d.ip+"/; "+ap
   :"Not connected to \""+d.ssid+"\". The DSP keeps trying; the access point stays available.";
  clearTimeout(wtimer);
  wtimer=wifiEl("wifi").style.display=="block"?setTimeout(refreshWifiState,3000):null;
}
function refreshWifiState(){
  wtimer=null;
  return fetch("/wificonfig").then(function(r){return r.json();}).then(showWifiState)
   .catch(function(){if(wifiEl("wifi").style.display=="block")wtimer=setTimeout(refreshWifiState,3000);});
}
function apWarningText(d){
  if(!d.apopenfallback)return "";
  var why=d.appwderror?" ("+d.appwderror+")":"";
  if(d.apup===false)  // "AP off while connected": not open now, but once it is back
    return "Note: the access point is off now. When it comes back it runs OPEN because the stored AP password is invalid"+why+". Store a valid AP password and restart.";
  return "Warning: the access point is running OPEN because the stored AP password is invalid"
   +why+". Anyone nearby can reach this DSP. Store a valid AP password and restart.";
}
function apCheck(d){
  if(d.pwdap.length>0&&d.pwdap.length<8)return "Please select a password with at least 8 characters.";
  if(d.apname.length<1||d.apname.length>32)return "The name must have 1 to 32 characters.";
  if(!(d.apchannel>=1&&d.apchannel<=13))return "The channel must be between 1 and 13.";
  return "";
}
function storeAp(){
  var d={pwdap:wifiEl("pwdap").value,apname:wifiEl("apname").value,apchannel:parseInt(wifiEl("apchannel").value,10)};
  var e=apCheck(d);
  if(e){wsay(e);return Promise.resolve();}
  wsay("Storing...");
  return fetch("/pwdap",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(d)})
   .then(function(r){return r.text().then(function(t){
     if(r.ok){wifiEl("pwdap").value="";wsay("Success! The configuration of the access point has been stored. It is applied after a restart.");}
     else wsay("Not stored: "+t);});})
   .catch(function(){wsay("Not stored: no answer from the DSP");});
}
// ---- Joining a WiFi: the same checks and messages as sendWifi() in aurora.js
function ssay(t){wifiEl("smsg").textContent=t||"";}
function wifiCheck(d){
  if(d.ssid.length>32)return "The network name can have at most 32 characters.";
  if((d.pwd.length>0&&d.pwd.length<8)||d.pwd.length>63)return "The WiFi password must have 8 to 63 characters, or be empty for an open network.";
  return "";
}
function joinWifi(){
  var d={ssid:wifiEl("ssid").value,pwd:wifiEl("pwd").value};
  // unlike the settings UI, an empty name does not forget the network here: that has its own button
  var e=d.ssid?wifiCheck(d):"Please enter the name of the network to join. To stop using a network, use \"Forget network\".";
  if(e){ssay(e);return Promise.resolve();}
  return postWifi(d,"Stored. The DSP now joins \""+d.ssid+"\". Once connected, it is reachable in that network on the IP shown above; the access point "
   +(wifiEl("apoff").checked?"then switches off after 30 s.":"stays available."));
}
function forgetWifi(){
  wifiEl("ssid").value="";wifiEl("pwd").value="";
  return postWifi({ssid:"",pwd:""},"The network has been forgotten. The DSP is now reachable on its access point only.");
}
function postWifi(d,ok){
  ssay("Storing...");
  return fetch("/wifi",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(d)})
   .then(function(r){return r.text().then(function(t){
     if(r.ok){wifiEl("pwd").value="";ssay(ok);}
     else ssay("Not stored: "+t);});})
   .catch(function(){ssay("Not stored: no answer from the DSP");});
}
// "AP off while connected", applied at once; the same texts as sendApOff() in aurora.js.
// 30 s here and in joinWifi() = ApSwitch::GRACE_MS (checked by test_matrixwifi.js)
function storeApOff(){
  var c=wifiEl("apoff").checked===true;
  ssay("Storing...");
  return fetch("/apoff",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({apoff:c})})
   .then(function(r){return r.text().then(function(t){
     if(r.ok)ssay(apOffText(c));
     else{wifiEl("apoff").checked=!c;ssay("Not stored: "+t);}});})
   .catch(function(){wifiEl("apoff").checked=!c;ssay("Not stored: no answer from the DSP");});
}
function apOffText(c){
  return c?"Stored. The access point switches off 30 s after the DSP has joined its WiFi; the DSP is then only reachable on its WiFi IP. The access point comes back when the connection is lost."
   :"Stored. The access point stays available.";
}
</script>
</body>
</html>
)!^!";
