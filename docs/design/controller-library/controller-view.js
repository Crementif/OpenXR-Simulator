/* Standalone asset adapter. Does not connect to the simulator or real devices. */
(() => {
let nextInstance = 0;
const clamp = (v, lo = 0, hi = 1) => Number.isFinite(Number(v)) ? Math.max(lo, Math.min(hi, Number(v))) : 0;
class ControllerView extends EventTarget {
  constructor(host, model, {interactive = true} = {}) {
    super(); this.host = host; this.model = model; this.interactive = interactive;
    this.state = {}; this.connected = true; this.disabled = new Set(); this.pointers = new Map();
    this.touchOnly = false; this.latch = false;
    const prefix = `controller-${++nextInstance}-`;
    host.innerHTML = model.svg.replace(/id="([^"]+)"/g, (_, id) => `id="${prefix}${id}"`).replace(/url\(#([^)]+)\)/g, (_, id) => `url(#${prefix}${id})`);
    this.svg = host.querySelector('svg');
    this.svg.id = prefix + 'root';
    const style = this.svg.querySelector('style');
    style.textContent = style.textContent.replace(/(^|})([^{}]+)\{/g, (_, boundary, selectors) => boundary + selectors.split(',').map(selector => {
      const trimmed = selector.trim();
      return trimmed.startsWith('.controller') ? trimmed.replace('.controller', '#' + this.svg.id) : '#' + this.svg.id + ' ' + trimmed;
    }).join(',') + '{');
    this.elements = new Map([...this.svg.querySelectorAll('[data-input]')].map(e => [e.dataset.input, e]));
    this.abort = new AbortController();
    for (const c of model.controls) {
      const el = this.elements.get(c.key);
      el.setAttribute('role', c.kind === 'analog' ? 'slider' : 'button');
      el.setAttribute('aria-label', `${model.hand} ${c.label}`);
      el.setAttribute('tabindex', interactive ? '0' : '-1');
      if (c.kind === 'analog') {
        el.setAttribute('aria-valuemin', '0'); el.setAttribute('aria-valuemax', '100'); el.setAttribute('aria-orientation', 'vertical');
      }
      if (!interactive) continue;
      const on = (type, fn) => el.addEventListener(type, fn, {signal:this.abort.signal});
      on('pointerdown', e => this.pointerDown(e, c));
      on('pointermove', e => this.pointerMove(e, c));
      on('pointerup', e => this.pointerEnd(e, c));
      on('pointercancel', e => this.pointerEnd(e, c, true));
      on('lostpointercapture', e => { if (this.pointers.has(e.pointerId)) this.pointerEnd(e, c, true); });
      on('keydown', e => this.key(e, c, true)); on('keyup', e => this.key(e, c, false));
      on('blur', () => { if (!this.latch) this.input(c.key, {}); });
    }
    this.render();
  }
  point(e) {
    const p = new DOMPoint(e.clientX, e.clientY);
    return p.matrixTransform(this.svg.getScreenCTM().inverse());
  }
  allowed(c) { return this.connected && !this.disabled.has(c.key); }
  pointerDown(e, c) {
    if (e.button !== 0 || !this.allowed(c)) return;
    e.preventDefault(); e.currentTarget.focus(); e.currentTarget.setPointerCapture(e.pointerId);
    this.pointers.set(e.pointerId, {key:c.key, start:this.point(e), moved:false});
    if (this.touchOnly) { if (c.touch) this.input(c.key, {touched:true}); return; }
    if (c.kind === 'button') this.input(c.key, {value:this.latch && this.state[c.key]?.value ? 0 : 1, pressed:!(this.latch && this.state[c.key]?.value), touched:c.touch});
    else this.pointerMove(e, c);
  }
  pointerMove(e, c) {
    const drag = this.pointers.get(e.pointerId);
    if (!drag || drag.key !== c.key || this.touchOnly || !this.allowed(c)) return;
    const p = this.point(e);
    drag.moved ||= Math.hypot(p.x-drag.start.x, p.y-drag.start.y)>4;
    if (c.kind === 'pad') {
      let x=(p.x-c.x)/c.r, y=(c.y-p.y)/c.r; const length=Math.hypot(x,y);
      if(length>1){x/=length;y/=length;}
      this.input(c.key,{x,y,touched:c.touch,pressed:false});
    } else if (c.kind === 'analog') this.input(c.key,{value:clamp((c.y+23-p.y)/46),touched:c.touch});
  }
  pointerEnd(e, c, cancelled = false) {
    const drag = this.pointers.get(e.pointerId); if(!drag)return;
    this.pointers.delete(e.pointerId);
    if(!cancelled && !this.touchOnly && c.kind === 'pad' && !drag.moved && c.click) this.input(c.key,{...this.state[c.key],pressed:true});
    if(cancelled || !this.latch) this.input(c.key,{});
    if(e.currentTarget.hasPointerCapture(e.pointerId)) e.currentTarget.releasePointerCapture(e.pointerId);
  }
  key(e,c,down) {
    if(!this.allowed(c))return;
    if(e.key==='Escape'){this.release();return;}
    if(e.key===' ' || e.key==='Enter') {
      e.preventDefault();
      if(this.touchOnly){if(c.touch)this.input(c.key,{touched:down});}
      else if(c.kind!=='pad'||c.click)this.input(c.key,{value:down?1:0,pressed:down,touched:down&&c.touch});
    } else if(e.key.startsWith('Arrow')) {
      e.preventDefault();
      if(c.kind==='pad')this.input(c.key,{x:down?(e.key==='ArrowRight'?1:e.key==='ArrowLeft'?-1:0):0,y:down?(e.key==='ArrowUp'?1:e.key==='ArrowDown'?-1:0):0,touched:down&&c.touch});
      else if(c.kind==='analog')this.input(c.key,{value:down?clamp((this.state[c.key]?.value||0)+(e.key==='ArrowUp'||e.key==='ArrowRight'?.1:-.1)):0,touched:down&&c.touch});
    }
  }
  // Complete per-control state replacement; omitted controls retain their values.
  setState(patch = {}) {
    for(const c of this.model.controls) {
      if(!(c.key in patch))continue;
      const s=patch[c.key]||{};
      let x=clamp(s.x,-1,1),y=clamp(s.y,-1,1);const len=Math.hypot(x,y);if(len>1){x/=len;y/=len;}
      this.state[c.key]={value:clamp(s.value),pressed:!!s.pressed,touched:c.touch&&!!s.touched,x,y};
    }
    this.render();
  }
  input(key,state) {
    this.setState({[key]:state});
    this.dispatchEvent(new CustomEvent('controller-input',{detail:{family:this.model.id,hand:this.model.hand,input:key,state:{...this.state[key]}}}));
  }
  render() {
    this.svg.dataset.connected=String(this.connected);
    for(const c of this.model.controls) {
      const e=this.elements.get(c.key),s=this.state[c.key]||{};
      const disabled=!this.connected||this.disabled.has(c.key);
      const pressed=!!s.pressed||(s.value||0)>=(c.kind==='analog'?.98:.5);
      e.dataset.pressed=String(pressed); e.dataset.touched=String(!!s.touched);e.dataset.disabled=String(disabled);
      e.setAttribute('aria-disabled',String(disabled));e.setAttribute('tabindex',this.interactive&&!disabled?'0':'-1');
      if(c.kind==='analog')e.setAttribute('aria-valuenow',String(Math.round((s.value||0)*100)));
      else e.setAttribute('aria-pressed',String(pressed));
      const fill=e.querySelector('[data-fill]');
      if(fill){const h=36*(s.value||0);fill.setAttribute('height',h);fill.setAttribute('y',c.y+18-h);fill.style.opacity=s.value?'.85':'0';}
      const marker=e.querySelector('[data-contact]');
      if(marker){marker.setAttribute('transform',`translate(${c.x+(s.x||0)*c.r*.7} ${c.y-(s.y||0)*c.r*.7})`);marker.style.opacity=s.touched||s.x||s.y||pressed?'1':'0';}
      if(c.key==='thumbstick'){
        const cap=e.querySelector('.cap'),line=e.querySelector('.rimline');
        for(const el of [cap,line])if(el)el.setAttribute('transform',`translate(${(s.x||0)*6} ${-(s.y||0)*6})`);
      }
    }
  }
  setConnected(value) {if(!value)this.release();this.connected=!!value;this.render();}
  setDisabled(keys=[]) {this.disabled=new Set(keys);for(const key of keys)if(key in this.state)this.input(key,{});this.render();}
  release() {
    this.pointers.clear();
    for(const c of this.model.controls){const s=this.state[c.key];if(s&&(s.value||s.pressed||s.touched||s.x||s.y))this.input(c.key,{});}
  }
  destroy() {this.release();this.abort.abort();this.host.replaceChildren();}
}
window.ControllerView=ControllerView;
})();
