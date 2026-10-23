(() => {
const all=window.CONTROLLER_LIBRARY;
const $=id=>document.getElementById(id);
let family='frame',selected='left',views={},minis={},demoTimer=null,switching=false,sequence=0,events=[];
const models=()=>all.filter(m=>m.id===family);
const familyList=all.filter(m=>m.hand==='left');
$('families').innerHTML=familyList.map(m=>`<button class="family" data-family="${m.id}" aria-pressed="false"><img src="${m.file}" alt=""><span><strong>${m.name}</strong><small>${m.brand}</small></span></button>`).join('');
document.querySelectorAll('[data-family].family').forEach(b=>b.addEventListener('click',()=>load(b.dataset.family)));
function activeText(view) {
 return view.model.controls.flatMap(c=>{
  const s=view.state[c.key]||{};
  if(s.value>0)return [c.kind==='analog'?`${c.label} ${Math.round(s.value*100)}%`:c.label];
  if(s.pressed)return [c.label+' click'];
  if(s.x||s.y)return [`${c.label} ${s.x.toFixed(1)}, ${s.y.toFixed(1)}`];
  if(s.touched)return [c.label+' touch'];
  return [];
 });
}
function sync() {
 for(const hand of ['left','right']) {
  if(!views[hand])continue;
  const v=views[hand],held=activeText(v);
  minis[hand].setState(v.state);minis[hand].setConnected(v.connected);
  $(hand+'-held').textContent=v.connected?(held.join(' · ')||'No inputs held'):'Disconnected';
  $(hand+'-mini-state').textContent=v.connected?(held.join(' · ')||'Idle'):'Disconnected';
  document.querySelector(`.hand-panel.${hand} .signal`).textContent=v.connected?'● Connected':'○ Disconnected';
 }
 const view=views[selected];if(!view)return;
 document.querySelectorAll('#control-inputs [data-control]').forEach(el=>{
  const s=view.state[el.dataset.control]||{},c=view.model.controls.find(c=>c.key===el.dataset.control);
  if(el.type==='range'){el.value=Math.round((s.value||0)*100);el.nextElementSibling.value=el.value+'%';}
  else{el.dataset.active=String(s.pressed||s.value>=.5);el.dataset.touched=String(!!s.touched);el.setAttribute('aria-pressed',String(s.pressed||s.value>=.5));}
  el.disabled=!view.connected||($('touch-mode').checked&&(!c.touch||el.type==='range')); // Touch mode cannot change analog actuation.
 });
 $('connected').checked=view.connected;
}
function onInput(e) {
 if(switching)return;
 const d=e.detail;
 if(demoTimer){
  stopDemo(false);
  for(const v of Object.values(views))v.setState(Object.fromEntries(v.model.controls.map(c=>[c.key,{}])));
  views[d.hand].setState({[d.input]:d.state});
 }
 sequence++;
 events.unshift(`#${sequence} ${d.hand} / ${d.input}  ${JSON.stringify(d.state)}`);events=events.slice(0,16);
 $('event-count').textContent=sequence;$('event-log').textContent=events.join('\n');sync();
}
function buildControls() {
 const model=views[selected].model;
 $('test-title').textContent=(selected==='left'?'Left':'Right')+' hand';
 $('control-inputs').style.setProperty('--selected-color',selected==='left'?'#6ca8ff':'#ff8a65');
 $('control-inputs').innerHTML=model.controls.map(c=>c.kind==='analog'?`<label class="axis-control">${c.label}<input type="range" min="0" max="100" step="1" value="0" data-control="${c.key}" aria-label="${selected} ${c.label} amount"><output>0%</output></label>`:c.kind==='pad'?`<button class="input-button" data-control="${c.key}" aria-label="${selected} ${c.label} click" aria-pressed="false" ${c.click?'':'disabled'} title="${c.click?'Hold to click':'Touch or drag the pad on the controller'}">${c.label}${c.click?' click':' touch'}</button>`:`<button class="input-button" data-control="${c.key}" aria-label="${selected} ${c.label} press" aria-pressed="false">${c.label}</button>`).join('');
 for(const el of document.querySelectorAll('#control-inputs [data-control]')) {
  const c=model.controls.find(c=>c.key===el.dataset.control);
  if(el.type==='range'){el.addEventListener('input',()=>views[selected].input(c.key,{value:Number(el.value)/100,touched:c.touch}));continue;}
  const down=()=>{
   const v=views[selected];if(!v.connected)return;
   if($('touch-mode').checked){if(c.touch)v.input(c.key,{touched:true});return;}
   if(c.kind==='pad'&&!c.click){if(c.touch)v.input(c.key,{touched:true});return;}
   const on=!($('latch').checked&&(v.state[c.key]?.pressed||v.state[c.key]?.value));
   v.input(c.key,{value:c.kind==='pad'?0:(on?1:0),pressed:on,touched:on&&c.touch});
  };
  const up=(cancelled=false)=>{if(cancelled||!$('latch').checked)views[selected].input(c.key,{});};
  el.addEventListener('pointerdown',e=>{if(e.button!==0)return;e.preventDefault();el.focus();el.setPointerCapture(e.pointerId);down();});
  el.addEventListener('pointerup',()=>up());el.addEventListener('pointercancel',()=>up(true));
  el.addEventListener('lostpointercapture',()=>{if(!$('latch').checked)up();});
  el.addEventListener('keydown',e=>{if(e.key===' '||e.key==='Enter'){e.preventDefault();if(!e.repeat)down();}});
  el.addEventListener('keyup',e=>{if(e.key===' '||e.key==='Enter'){e.preventDefault();up();}});
  el.addEventListener('blur',()=>{if(!$('latch').checked)up();});
 }
 sync();
}
function chooseHand(hand){selected=hand;document.querySelectorAll('.hand-choice,.mini-slot').forEach(b=>b.setAttribute('aria-pressed',String(b.dataset.hand===hand)));buildControls();}
function stopDemo(clear=true){if(demoTimer){clearInterval(demoTimer);demoTimer=null;}$('demo').textContent='▶ Play input demo';$('demo').setAttribute('aria-pressed','false');if(clear)release(false);}
function release(stop=true){if(stop)stopDemo(false);for(const v of Object.values(views))v.release();sync();}
function load(id) {
 stopDemo(false);switching=true;
 for(const v of Object.values(views))v.destroy();for(const v of Object.values(minis))v.destroy();
 family=id;views={};minis={};
 for(const model of models()){
  const hand=model.hand;
  views[hand]=new ControllerView($(hand+'-view'),model);
  views[hand].latch=$('latch').checked;views[hand].touchOnly=$('touch-mode').checked;
  minis[hand]=new ControllerView($(hand+'-mini'),model,{interactive:false});minis[hand].svg.setAttribute('aria-hidden','true');
  views[hand].addEventListener('controller-input',onInput);
 }
 switching=false;const m=models()[0];
 $('maker').textContent=m.brand;$('family-title').textContent=m.name;$('family-detail').textContent=m.subtitle;
 $('hotbar-family').textContent=m.name;$('reference').href=m.source;
 document.querySelectorAll('.family').forEach(b=>b.setAttribute('aria-pressed',String(b.dataset.family===id)));
 showHardware(id);chooseHand(selected);sync();
}
function showHardware(id) {
 const ref=window.CONTROLLER_REFERENCES[id],img=$('hardware-image');
 $('hardware-model').textContent=ref.model;
 $('hardware-source').textContent=ref.credit+' ↗';$('hardware-source').href=ref.source;
 $('hardware-note').textContent=ref.note;$('hardware-image-link').href=ref.image;
 $('hardware-image-status').hidden=false;$('hardware-image-status').textContent='Loading reference…';
 img.hidden=true;img.alt=ref.model+' — '+ref.credit;
 img.onload=()=>{img.hidden=false;$('hardware-image-status').hidden=true;};
 img.onerror=()=>{img.hidden=true;$('hardware-image-status').hidden=false;$('hardware-image-status').textContent='Reference unavailable. Open the source below.';};
 img.src=ref.image;
}
$('show-hardware').addEventListener('change',()=>{
 $('hardware-panel').hidden=!$('show-hardware').checked;
 $('comparison').classList.toggle('drawing-only',!$('show-hardware').checked);
});
document.querySelectorAll('.hand-choice,.mini-slot').forEach(b=>b.addEventListener('click',()=>chooseHand(b.dataset.hand)));
$('release').addEventListener('click',()=>release());
$('connected').addEventListener('change',()=>{stopDemo(false);views[selected].setConnected($('connected').checked);sync();});
$('touch-mode').addEventListener('change',()=>{release();for(const v of Object.values(views))v.touchOnly=$('touch-mode').checked;sync();});
$('latch').addEventListener('change',()=>{release();for(const v of Object.values(views))v.latch=$('latch').checked;});
$('demo').addEventListener('click',()=>{
 if(demoTimer){stopDemo();return;} release();
 for(const v of Object.values(views))v.setConnected(true);
 $('demo').textContent='■ Stop demo';$('demo').setAttribute('aria-pressed','true');
 const start=performance.now();
 demoTimer=setInterval(()=>{
  const t=(performance.now()-start)/1000;
  for(const [hand,v] of Object.entries(views)){
   const phase=t+(hand==='left'?0:1.2),patch={};
   for(const c of v.model.controls){
    const i=v.model.controls.indexOf(c);
    if(c.kind==='analog')patch[c.key]={value:(Math.sin(phase*1.8+i)+1)/2,touched:true};
    else if(c.kind==='pad')patch[c.key]={x:Math.cos(phase)*.65,y:Math.sin(phase)*.65,touched:true,pressed:c.click&&Math.floor(phase)%4===0};
    else patch[c.key]={value:Math.floor(phase*2)%v.model.controls.length===i?1:0,touched:Math.floor(phase*2+1)%v.model.controls.length===i};
   }
   v.setState(patch);
  }sync();
 },80);
});
window.addEventListener('blur',()=>release());document.addEventListener('visibilitychange',()=>{if(document.hidden)release();});
window.addEventListener('keydown',e=>{if(e.key==='Escape')release();});
window.ControllerLab={setState(hand,patch){views[hand].setState(patch);sync();},release,selectFamily:load};
load(family);
})();
