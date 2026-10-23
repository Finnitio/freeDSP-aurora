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
button.mute{width:56px;height:30px;border-top-width:1px}
button.on{background-color:#802020}
#msg{color:#e08080;min-height:18px}
#presets button{width:44px;height:30px;border-top-width:1px}
.slot{border:1px solid #606060;border-radius:3px;padding:2px 8px;margin:8px 0}
.lbl{flex:0 0 40px}
#plot{width:100%;height:200px;background:#101010;border:1px solid #606060}
button.wide{width:100%;height:32px;border-top-width:1px}
button:disabled{opacity:.4}
</style>
</head>
<body onload="load()">
<h1>Aurora DSP</h1>
<div id="msg"></div>
<div id="main">
<div id="presets" class="row"></div>
<h2>Buses</h2>
<div id="buses"></div>
<h2>Outputs</h2>
<div id="outs"></div>
</div>
<div id="flt" style="display:none">
<div class="row"><button class="mute" onclick="closeF()">Back</button><h2 id="ftitle" style="margin:0"></h2></div>
<canvas id="plot" width="600" height="200"></canvas>
<div id="top"></div>
<div id="budget"></div>
<div id="slots"></div>
<button id="add" class="wide" onclick="addSlot()">+ add</button>
</div>
<script>
var st=null,timers={},seq=0,seen=0,late=false,view=null,cf=null,cseq=0,lv={};
function say(t){document.getElementById("msg").textContent=t||"";}
function waitIdle(){return fetch("/idle").then(function(r){return r.text();}).then(function(t){
  if(t=="1")return;return new Promise(function(r){setTimeout(r,100);}).then(waitIdle);});}
function load(){
  var n=++seq;
  fetch("/matrix").then(function(r){return r.json();}).then(function(s){if(n<seen)return;seen=n;st=s;draw();})
   .catch(function(){say("Cannot read the matrix");});
  fetch("/preset").then(function(r){return r.json();}).then(function(p){drawPresets(p.pre);});
}
function drawPresets(cur){
  var h="";for(var i=0;i<4;i++)h+="<button class='"+(i==cur?"on":"")+"' onclick='preset("+i+")'>"+"ABCD"[i]+"</button>";
  document.getElementById("presets").innerHTML=h;
}
function preset(p){
  say("Switching preset...");
  fetch("/preset",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({pre:p})})
   .then(waitIdle).then(function(){say("");load();});
}
// Sends a patch, the reply is the new state or the reason it was refused.
function send(ch,idx,f){
  var p={ch:ch,idx:idx};for(var k in f)p[k]=f[k];
  var n=++seq;  // replies older than one already shown are ignored
  return fetch("/matrix",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(p)})
   .then(function(r){return r.text().then(function(t){
     if(r.ok){if(n<seen)return;seen=n;st=JSON.parse(t);say("");}else{say(t);}
     draw();});});
}
// Sliders are sent while dragging: at once, then at most every 100 ms with the latest value.
function live(k,ch,idx,f){
  var e=lv[k]||(lv[k]={t:0,p:null});e.p=f;if(e.t)return;
  var go=function(){var p=e.p;e.p=null;if(!p){e.t=0;return;}send(ch,idx,p);e.t=setTimeout(go,100);};
  go();
}
function sendGain(ch,idx,v){
  document.getElementById("gv"+ch+idx).textContent=v+" dB";
  live("g"+ch+idx,ch,idx,{gain:parseFloat(v)});
}
function esc(s){return s.replace(/&/g,"&amp;").replace(/"/g,"&quot;").replace(/</g,"&lt;");}
function opts(list,sel,val,txt){
  return list.map(function(o,i){var v=val(o,i);return "<option value='"+v+"'"+(v==sel?" selected":"")+">"+txt(o)+"</option>";}).join("");
}
function row(ch,idx,c,sel){
  return "<div class='row'><input type='text' class='name' maxlength='16' value=\""+esc(c.name)+"\" onchange=\"send('"+ch+"',"+idx+",{name:this.value})\">"
   +sel
   +"<button class='mute"+(c.mute?" on":"")+"' onclick=\"send('"+ch+"',"+idx+",{mute:"+!c.mute+"})\">"+(c.mute?"Muted":"Mute")+"</button>"
   +"<button class='mute' onclick=\"openF('"+ch+"',"+idx+")\">Filter</button>"
   +"<input type='range' class='gain' min='"+st.gainmin+"' max='"+st.gainmax+"' step='0.5' value='"+c.gain+"' oninput=\"sendGain('"+ch+"',"+idx+",this.value)\" onchange=\"send('"+ch+"',"+idx+",{gain:parseFloat(this.value)})\">"
   +"<span class='gv' id='gv"+ch+idx+"'>"+c.gain+" dB</span></div>";
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
  // do not rebuild under an open select, a name being typed or a slider being dragged
  if(busy("#buses,#outs")){late=true;return;}
  late=false;
  document.getElementById("buses").innerHTML=b;
  document.getElementById("outs").innerHTML=o;
}
document.addEventListener("focusout",function(){if(late)setTimeout(draw,0);});

// ---- filter page
function chan(){return (view.ch=="bus"?st.buses:st.outs)[view.idx];}
function openF(ch,i){view={ch:ch,idx:i};cf=null;late=false;
  document.getElementById("main").style.display="none";document.getElementById("flt").style.display="";
  drawF();}
function closeF(){view=null;late=false;
  document.getElementById("main").style.display="";document.getElementById("flt").style.display="none";draw();}
function fpos(f){return Math.round(1000*Math.log(f/st.peq.fmin)/Math.log(st.peq.fmax/st.peq.fmin));}
function fval(p){return parseFloat((st.peq.fmin*Math.pow(st.peq.fmax/st.peq.fmin,p/1000)).toPrecision(4));}
function slider(i,lbl,key,min,max,step,val,unit,pos){
  var id="s"+i+key;
  return "<div class='row'><span class='lbl'>"+lbl+"</span><input type='range' class='gain' min='"+min+"' max='"+max+"' step='"+step+"' value='"+val+"'"
   +" oninput=\"slotSet("+i+",'"+key+"',this.value)\" onchange=\"slotSend("+i+",'"+key+"',this.value)\">"
   +"<span class='gv' id='"+id+"'>"+(key=="freq"?fval(pos):val)+" "+unit+"</span></div>";
}
function slotVal(key,v){return key=="freq"?fval(parseInt(v)):parseFloat(v);}
function slotSet(i,key,v){
  var x=slotVal(key,v);
  document.getElementById("s"+i+key).textContent=x+" "+{freq:"Hz",q:"",gain:"dB"}[key];
  var f={op:"set",i:i};f[key]=x;
  live("s"+i+key,view.ch,view.idx,{slot:f});
}
function slotSend(i,key,v){
  var f={op:"set",i:i};f[key]=slotVal(key,v);
  send(view.ch,view.idx,{slot:f});
}
function addSlot(){send(view.ch,view.idx,{slot:{op:"add"}});}
function delSlot(i){send(view.ch,view.idx,{slot:{op:"del",i:i}});}
function bypass(i,b){send(view.ch,view.idx,{slot:{op:"set",i:i,bypass:b}});}
function topGain(v){
  document.getElementById("tgv").textContent=v+" dB";
  live("tg"+view.ch+view.idx,view.ch,view.idx,{gain:parseFloat(v)});
}
function sendTop(f){send(view.ch,view.idx,f);}
// gain block (all Channels), polarity and delay (Outputs only)
function topHtml(c){
  var h="<div class='slot'><div class='row'><b class='name'>Gain</b>"
   +"<button class='mute"+(c.gainBypass?" on":"")+"' onclick='sendTop({gainBypass:"+!c.gainBypass+"})'>Bypass</button></div>"
   +"<div class='row'><input type='range' class='gain' min='"+st.gainmin+"' max='"+st.gainmax+"' step='0.5' value='"+c.gain+"' oninput='topGain(this.value)' onchange='sendTop({gain:parseFloat(this.value)})'>"
   +"<span class='gv' id='tgv'>"+c.gain+" dB</span></div></div>";
  if(view.ch!="out")return h;
  h+="<div class='slot'><div class='row'><b class='name'>Polarity</b>"
   +"<button class='mute"+(c.polarity?" on":"")+"' onclick='sendTop({polarity:"+!c.polarity+"})'>"+(c.polarity?"Inverted":"Normal")+"</button></div></div>";
  h+="<div class='slot'><div class='row'><b class='name'>Delay</b>"
   +"<button class='mute"+(c.delayBypass?" on":"")+"' onclick='sendTop({delayBypass:"+!c.delayBypass+"})'>Bypass</button></div>"
   +"<div class='row'><input type='number' class='name' min='0' max='"+Math.round(st.delaymax*1e6/48000)+"' step='1' value='"+Math.round(c.delayUs)+"' onchange='sendTop({delayUs:parseFloat(this.value)})'><span>&micro;s</span></div>"
   +"<div class='row'>Applied: "+c.delayUs.toFixed(1)+" &micro;s = "+c.delaySamples+" samples = "+c.delayMm.toFixed(1)+" mm"+(c.delayBypass?" (bypassed)":"")+"</div></div>";
  return h;
}
function drawF(){
  var c=chan();
  document.getElementById("ftitle").textContent=c.name;
  document.getElementById("budget").textContent="Biquads: "+c.used+" of "+c.budget+" used, "+c.free+" free";
  document.getElementById("add").disabled=c.addable.indexOf("peq")<0;
  coeffs();
  if(busy("#slots,#top")){late=true;return;}
  late=false;
  document.getElementById("top").innerHTML=topHtml(c);
  var h="",p=st.peq;
  c.slots.forEach(function(s,i){
    h+="<div class='slot'><div class='row'><b class='name'>PEQ "+(i+1)+"</b>"
     +"<button class='mute"+(s.bypass?" on":"")+"' onclick='bypass("+i+","+!s.bypass+")'>Bypass</button>"
     +"<button class='mute' onclick='delSlot("+i+")'>Delete</button></div>"
     +slider(i,"Freq","freq",0,1000,1,fpos(s.freq),"Hz",fpos(s.freq))
     +slider(i,"Q","q",p.qmin,p.qmax,0.1,s.q,"",0)
     +slider(i,"Gain","gain",p.gmin,p.gmax,0.5,s.gain,"dB",0)+"</div>";
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
  var w=2*Math.PI*f/cf.fs,c1=Math.cos(w),s1=Math.sin(w),c2=Math.cos(2*w),s2=Math.sin(2*w),db=0;
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
</script>
</body>
</html>
)!^!";
