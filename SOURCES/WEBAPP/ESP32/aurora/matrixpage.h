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
.slot{border:1px solid #606060;border-radius:3px;padding:2px 8px;margin:8px 0}
.lbl{flex:0 0 40px}
#plot{width:100%;height:200px;background:#101010;border:1px solid #606060}
button.wide{width:100%;height:32px;border-top-width:1px}
button:disabled{opacity:.4}
.linkset{border-left:3px solid #606060;padding-left:6px;margin:4px 0 8px}
.linkset b{flex:0 0 54px}
.linkset button{width:auto;min-width:58px;height:28px;border-top-width:1px}
.top{display:flex;align-items:center;justify-content:space-between;gap:8px}
#wifi{position:fixed;top:0;right:0;bottom:0;left:0;background:rgba(0,0,0,.6);overflow-y:auto;z-index:10}
#wifi .box{box-sizing:border-box;max-width:400px;width:calc(100% - 16px);margin:40px auto;padding:8px 12px;background:#202020;border:1px solid #606060;border-radius:3px}
#wifi input{box-sizing:border-box;width:100%;height:30px;margin-bottom:6px}
#wifi .row button{flex:1 1 0;height:32px;border-top-width:1px}
#apwarn{color:#ff6060;font-weight:bold}
#wmsg{min-height:18px}
</style>
</head>
<body onload="load()">
<div class="top"><h1>Aurora DSP</h1><button class="mute" onclick="openWifi()">WiFi</button></div>
<div id="msg"></div>
<div id="main">
<div id="presets" class="row"></div>
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
<h2>Access point</h2>
<div id="apwarn" style="display:none"></div>
<label for="apname">Name (1-32 characters)</label><input type="text" id="apname" maxlength="32" autocomplete="off">
<label for="apchannel">Channel (1-13)</label><input type="number" id="apchannel" min="1" max="13" inputmode="numeric">
<label for="pwdap">Password (at least 8 characters, empty: open AP)</label><input type="password" id="pwdap" autocomplete="new-password">
<div id="wmsg"></div>
<div class="row"><button onclick="storeAp()">Store AP config</button><button onclick="closeWifi()">Close</button></div>
</div></div>
<script>
var st=null,seq=0,seen=0,late=false,view=null,cf=null,cseq=0,csig="",ct=0,activePreset=-1;
function say(t){document.getElementById("msg").textContent=t||"";}
function waitIdle(){return fetch("/idle").then(function(r){return r.text();}).then(function(t){
  if(t=="1")return;return new Promise(function(r){setTimeout(r,100);}).then(waitIdle);});}
// Settles once the matrix and the master volume have been read (or failed to).
function load(){
  var n=++seq;
  fetch("/preset").then(function(r){return r.json();}).then(function(p){drawPresets(p.pre);});
  return Promise.all([
   fetch("/matrix").then(function(r){return r.json();}).then(function(s){if(n<seen)return;seen=n;st=s;activePreset=s.preset;draw();})
    .catch(function(){say("Cannot read the matrix");}),
   fetch("/mvol").then(function(r){return r.json();}).then(function(v){mv=v.vol;showVol(v.vol);}).catch(function(){})]);
}
function showVol(v){
  document.getElementById("vol").value=v;
  document.getElementById("volv").textContent=parseFloat(v).toFixed(1)+" dB";
}
var vt=0,vp=null,mv=null;
function sendVol(v){  // while dragging: at once, then at most every 100 ms with the latest value
  if(switching)return;  // the reload puts the slider back on the loaded volume
  showVol(v);vp=v;if(vt)return;
  var go=function(){var x=vp;vp=null;if(x===null){vt=0;return;}
    fetch("/mvol",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({vol:parseFloat(x)})});
    vt=setTimeout(go,100);};
  go();
}
function drawPresets(cur){
  var h="";for(var i=0;i<4;i++)h+="<button class='"+(i==cur?"on":"")+"' onclick='preset("+i+")'>"+"ABCD"[i]+"</button>";
  document.getElementById("presets").innerHTML=h;
}
// From the click until the state has been reloaded no edit is sent: edits made meanwhile, and a
// master volume value still waiting for the throttle, are dropped rather than sent to the new Preset.
var switching=false;
function preset(p){
  if(switching)return;
  switching=true;clearTimeout(vt);vt=0;vp=null;
  say("Switching preset...");
  fetch("/preset",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({pre:p})})
   .then(function(r){return r.text().then(function(t){if(!r.ok)throw new Error(t||"Preset switch failed");});})
   .then(waitIdle).then(function(){say("");},function(e){say(e.message);})
   .then(load).then(function(){switching=false;if(mv!==null)showVol(mv);});  // a drag meanwhile moved the thumb
}
// Sends a patch, the reply is the new state or the reason it was refused.
function send(ch,idx,f){
  var p={ch:ch,idx:idx,preset:activePreset};for(var k in f)p[k]=f[k];
  return post(p);
}
function post(p){
  if(switching)return Promise.resolve();
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
  if(switching){el.value=d.v;return;}  // dropped; the reload shows the loaded value
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

// ---- WiFi dialog: the same checks, messages and open-AP warning as the settings UI (aurora.js)
function wifiEl(id){return document.getElementById(id);}
function wsay(t){wifiEl("wmsg").textContent=t||"";}
function openWifi(){
  wifiEl("wifi").style.display="block";wsay("");wifiEl("pwdap").value="";  // the password is never shown
  return fetch("/wificonfig").then(function(r){return r.json();}).then(function(d){
    wifiEl("apname").value=d.apname;wifiEl("apchannel").value=d.apchannel;
    var w=wifiEl("apwarn");w.textContent=apWarningText(d);w.style.display=w.textContent?"block":"none";
  }).catch(function(){wsay("Cannot read the WiFi settings");});
}
function closeWifi(){wifiEl("wifi").style.display="none";}
function apWarningText(d){
  if(!d.apopenfallback)return "";
  return "Warning: the access point is running OPEN because the stored AP password is invalid"
   +(d.appwderror?" ("+d.appwderror+")":"")+". Anyone nearby can reach this DSP. Store a valid AP password and restart.";
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
</script>
</body>
</html>
)!^!";
