// Matrix page (plugin kind "matrix"), served by the firmware instead of a generated dsp.html.
// The layout, Source list and gain range all come from GET /matrix.
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
.row{display:flex;align-items:center;gap:6px;margin:4px 0}
.row *{box-sizing:border-box}
.name{flex:1 1 90px;min-width:0;width:90px}
input.name{height:30px}
select{flex:1 1 90px;min-width:0;height:30px;border:1px solid #808080;border-radius:2px;color:#fff}
.gain{flex:2 1 110px;min-width:0}
.gv{width:52px;text-align:right}
button.mute{width:56px;height:30px;border-top-width:1px}
button.on{background-color:#802020}
#msg{color:#e08080;min-height:18px}
#presets button{width:44px;height:30px;border-top-width:1px}
</style>
</head>
<body onload="load()">
<h1>Aurora DSP</h1>
<div id="presets" class="row"></div>
<div id="msg"></div>
<h2>Buses</h2>
<div id="buses"></div>
<h2>Outputs</h2>
<div id="outs"></div>
<script>
var st=null,timers={};
function say(t){document.getElementById("msg").textContent=t||"";}
function waitIdle(){return fetch("/idle").then(function(r){return r.text();}).then(function(t){
  if(t=="1")return;return new Promise(function(r){setTimeout(r,100);}).then(waitIdle);});}
function load(){
  fetch("/matrix").then(function(r){return r.json();}).then(function(s){st=s;draw();})
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
  return fetch("/matrix",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(p)})
   .then(function(r){return r.text().then(function(t){
     if(r.ok){st=JSON.parse(t);say("");}else{say(t);}
     draw();});});
}
// Sliders are sent while dragging, at most every 100 ms.
function sendGain(ch,idx,v){
  document.getElementById("gv"+ch+idx).textContent=v;
  var k=ch+idx;if(timers[k])return;
  timers[k]=setTimeout(function(){timers[k]=0;},100);
  send(ch,idx,{gain:parseFloat(v)});
}
function esc(s){return s.replace(/&/g,"&amp;").replace(/"/g,"&quot;").replace(/</g,"&lt;");}
function opts(list,sel,val,txt){
  return list.map(function(o,i){var v=val(o,i);return "<option value='"+v+"'"+(v==sel?" selected":"")+">"+txt(o)+"</option>";}).join("");
}
function row(ch,idx,c,sel){
  return "<div class='row'><input type='text' class='name' maxlength='16' value=\""+esc(c.name)+"\" onchange=\"send('"+ch+"',"+idx+",{name:this.value})\">"
   +sel
   +"<button class='mute"+(c.mute?" on":"")+"' onclick=\"send('"+ch+"',"+idx+",{mute:"+!c.mute+"})\">"+(c.mute?"Muted":"Mute")+"</button>"
   +"<input type='range' class='gain' min='"+st.gainmin+"' max='"+st.gainmax+"' step='0.5' value='"+c.gain+"' oninput=\"sendGain('"+ch+"',"+idx+",this.value)\" onchange=\"send('"+ch+"',"+idx+",{gain:parseFloat(this.value)})\">"
   +"<span class='gv' id='gv"+ch+idx+"'>"+c.gain+"</span></div>";
}
function draw(){
  if(!st)return;
  var b="",o="";
  st.buses.forEach(function(c,i){
    b+=row("bus",i,c,"<select onchange=\"send('bus',"+i+",{source:parseInt(this.value)})\">"+opts(st.sources,c.source,function(s){return s.v;},function(s){return s.n;})+"</select>");
  });
  st.outs.forEach(function(c,i){
    o+=row("out",i,c,"<select onchange=\"send('out',"+i+",{route:parseInt(this.value)})\">"+opts(st.routes,c.route,function(s,j){return j;},function(s){return s;})+"</select>");
  });
  var a=document.activeElement;
  if(a&&a.type=="range")return;  // do not rebuild under a slider being dragged
  document.getElementById("buses").innerHTML=b;
  document.getElementById("outs").innerHTML=o;
}
</script>
</body>
</html>
)!^!";
