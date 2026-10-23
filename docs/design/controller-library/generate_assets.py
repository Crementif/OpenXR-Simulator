"""Generate original, editable controller diagrams. No application dependencies."""
from pathlib import Path
import json, html, math
OUT=Path(__file__).resolve().parent
SOURCES={
'frame':'https://partner.steamgames.com/doc/steamhardware/steamframe/input',
'vive':'https://www.vive.com/us/support/vive-pro-hmd/category_howto/about-the-controllers.html',
'quest':'https://developers.meta.com/vr/documentation/native/pc/native-touch-plus-controllers/',
'index':'https://www.valvesoftware.com/en/index/controllers',
'focus':'https://www.vive.com/us/support/vive-xr/category_howto/about-the-controllers.html',
'pico':'https://developer.picoxr.com/document/unity/input-mapping/',
'ultra':'https://www.picoxr.com/games/products/pico4-ultra',
'wmr':'https://learn.microsoft.com/en-us/windows/mixed-reality/design/motion-controllers',
'g2':'https://learn.microsoft.com/en-us/windows/mixed-reality/develop/unity/unity-reverb-g2-controllers',
'sense':'https://www.playstation.com/en-us/support/hardware/ps-vr-2-sense-controller/',
}
families=[
('frame','Steam Frame','Valve','Split gamepad · dual-stage controls','frame','dark','/interaction_profiles/valve/frame_controller_valve'),
('quest','Quest Touch Plus / Pro','Meta','Touch Plus geometry · shared Pro input layout','touch','light','/interaction_profiles/meta/touch_controller_plus'),
('quest-ring','Quest / Rift S Touch','Meta / Oculus','Quest 2 geometry · shared ringed Touch inputs','touch-ring','light','/interaction_profiles/oculus/touch_controller'),
('index','Index','Valve','Knuckles strap · force-sensitive grip','index','dark','/interaction_profiles/valve/index_controller'),
('vive','Vive Wand','HTC','Vive / Vive Pro · trackpad','wand','dark','/interaction_profiles/htc/vive_controller'),
('focus','Vive Focus / XR','HTC','XR Series geometry · broad upper halo','focus','dark',None),
('pico','Pico 4','Pico','Swept tracking ring','pico','light',None),
('pico-ultra','Pico 4 Ultra','Pico','Ringless · white thumb deck · contoured grip','ultra','light',None),
('wmr','Mixed Reality','Microsoft ecosystem','WMR reference · stick + trackpad','wmr','dark','/interaction_profiles/microsoft/motion_controller'),
('g2','Reverb G2','HP','Face buttons · analog grip','g2','dark','/interaction_profiles/hp/mixed_reality_controller'),
('sense','PS VR2 Sense','Sony','White orbital ring · split face buttons','sense','light',None),
]
def path(d,cls='shell',extra=''):return f'<path class="{cls}" d="{d}" {extra}/>'
def circle(x,y,r,cls,extra=''):return f'<circle class="{cls}" cx="{x}" cy="{y}" r="{r}" {extra}/>'
def label(x,y,t,size=13,cls='legend'):return f'<text class="{cls}" x="{x}" y="{y}" font-size="{size}" text-anchor="middle" dominant-baseline="central">{html.escape(t)}</text>'
styles='''
.shell{fill:url(#shell);stroke:#78838f;stroke-width:1.6}.rim{fill:url(#rim);stroke:#82909f;stroke-width:1.5}.face{fill:url(#face);stroke:#53616e;stroke-width:1.5}.seam{fill:none;stroke:#697784;stroke-width:1.2;opacity:.6}.inset{fill:#11171d;stroke:#66717b;stroke-width:1.3}.strap{fill:#343a41;stroke:#77818a;stroke-width:1.5}.stitch{fill:none;stroke:#87919a;stroke-width:1;stroke-dasharray:2 3}.cord{fill:none;stroke:#66727e;stroke-width:2.4}.light-face{fill:url(#lightFace);stroke:#a5afb8;stroke-width:1.5}.hardware-button{fill:var(--hardware-cap,#aeb8c0);stroke:#707e8b;stroke-width:1.2}.dark-shell{fill:url(#face);stroke:#66727d;stroke-width:1.6}.legend{fill:#b3bec9;font-family:Arial,sans-serif;pointer-events:none}.control .cap{fill:#27323e;stroke:#8292a2;stroke-width:1.5;transition:fill 70ms,stroke 70ms}.control .rimline{fill:none;stroke:#10161d;stroke-width:2;pointer-events:none}.control .symbol{fill:#e5ebf2;font-family:Arial,sans-serif;font-weight:600;pointer-events:none}.control .surface-line{stroke:#536474;stroke-width:1;fill:none;pointer-events:none}.control .contact{fill:none;stroke:var(--accent,#6ca8ff);stroke-width:1.8;stroke-linecap:round;opacity:0;pointer-events:none}.control .hit{fill:transparent;stroke:none}.control .touch-ring{fill:none;stroke:var(--accent,#6ca8ff);stroke-width:2;stroke-dasharray:3 3;opacity:0;pointer-events:none}.control[data-touched=true] .touch-ring{opacity:1}.control[data-pressed=true] .cap{fill:var(--accent,#6ca8ff);stroke:#eef8ff;stroke-width:2.4}.control[data-pressed=true] .symbol{fill:#0d1823}.control[data-touched=true] .contact{opacity:1}.control:focus-visible{outline:none}.control:focus-visible .cap{stroke:#fff;stroke-width:3;stroke-dasharray:4 2}.control[data-disabled=true]{opacity:.3}.actuation{fill:var(--accent,#6ca8ff);opacity:0;pointer-events:none}.haptic{fill:none;stroke:var(--accent,#6ca8ff);stroke-width:2;opacity:0}.controller[data-connected=false]{opacity:.32}.control{cursor:pointer;touch-action:none}
'''
models=[]
for fid,name,brand,subtitle,shape,material,profile in families:
 for hand in ['left','right']:
  uid=f'{fid}-{hand}'
  # All diagrams face the viewer. Shell mirroring never mirrors the labels.
  flip=hand=='right';mirror='translate(320 0) scale(-1 1)' if flip else ''
  x=lambda v:320-v if flip else v
  # Inset face controls around the deck centre. The caps and their touch/focus
  # outlines need breathing room inside the physical rim, including at hotbar size.
  face_spacing={
   'quest':(148,113,.88,.90),
   'quest-ring':(148,158,.88,.90),
   'index':(168,108,.86,.90),
   'focus':(145,142,.90,.92),
   'pico':(163,108,.90,.92),
   'pico-ultra':(145,119,.90,.92),
   'g2':(144,158,.83,.88),
   'sense':(178,111,.84,.90),
  }
  def face_geometry(cx,cy,r):
   if fid not in face_spacing:return cx,cy,r
   ax,ay,spacing,size=face_spacing[fid]
   ax=x(ax)
   return round(ax+(cx-ax)*spacing,3),round(ay+(cy-ay)*spacing,3),round(r*size,3)
  controls=[]
  def add(key,title,kind,body,cx,cy,r=16,touch=False,click=True,path_suffix=None):
   controls.append(dict(key=key,label=title,kind=kind,x=cx,y=cy,r=r,touch=touch,click=click,path=path_suffix))
   return f'<g id="{uid}-{key}" class="control" data-input="{key}" data-kind="{kind}" data-pressed="false" data-touched="false"><title>{html.escape(title)}</title>{body}</g>'
  def button(key,title,cx,cy,t,r=15,touch=False,suffix=None):
   cx,cy,r=face_geometry(cx,cy,r)
   b=circle(cx,cy,r+4,'touch-ring')+circle(cx,cy,r,'cap')+label(cx,cy+.5,t,min(12,round(r*1.05,2)),'symbol')+circle(cx,cy,r+5,'hit')
   return add(key,title,'button',b,cx,cy,r,touch,path_suffix=suffix)
  def pad(key,title,cx,cy,r=25,oval=False,touch=True,click=True):
   cx,cy,r=face_geometry(cx,cy,r)
   ry=r*1.65 if oval else r
   b=f'<ellipse cx="{cx}" cy="{cy}" rx="{r+4}" ry="{ry+4}" class="touch-ring"/>'
   b+=f'<ellipse cx="{cx}" cy="{cy}" rx="{r}" ry="{ry}" class="cap"/>'
   if key=='thumbstick':
    b+=circle(cx,cy,r-6,'rimline')
   else:
    b+=path(f'M{cx-r+7} {cy}h{2*r-14}M{cx} {cy-ry+7}v{2*ry-14}','surface-line')
   if key!='thumbstick':b+=f'<g class="contact" data-contact="{key}" transform="translate({cx} {cy})"><path d="M-4.5 0h9M0-4.5v9"/></g>'
   b+=f'<ellipse cx="{cx}" cy="{cy}" rx="{r+3}" ry="{ry+3}" class="hit"/>'
   return add(key,title,'pad',b,cx,cy,r,touch,click,'/input/'+key)
  def analog(key,title,cx,cy,short,digital=False,suffix=None):
   b=f'<rect class="touch-ring" x="{cx-15}" y="{cy-26}" width="30" height="52" rx="11"/><rect class="cap" x="{cx-11}" y="{cy-23}" width="22" height="46" rx="8"/><rect class="actuation" data-fill="{key}" x="{cx-8}" y="{cy+18}" width="16" height="0" rx="4"/>'+label(cx,cy,short,9,'symbol')+f'<rect class="hit" x="{cx-18}" y="{cy-27}" width="36" height="54" rx="10"/>'
   return add(key,title,'button' if digital else 'analog',b,cx,cy,24,touch=(fid in ['frame','quest','quest-ring','index','sense']),path_suffix=suffix)
  # Left-hand geometry is authored explicitly; right-hand shells mirror it.
  # Input labels stay upright and x() also updates the interaction hit coordinates.
  shell='';face='';detail='';ui='';ring='';ring_front='';strap=''
  def plate(d,light=False):return path(d,'light-face' if light else 'face')
  def ring_shape(d):return path(d,'rim','fill-rule="evenodd"')
  def face_buttons(lower,upper,lower_pos,upper_pos,r=13):
   symbols={'square':'□','triangle':'△','cross':'×','circle':'○'}
   result=''
   for key,(cx,cy) in [(lower,lower_pos),(upper,upper_pos)]:
    result+=button(key,key.title() if fid=='sense' else key.upper(),x(cx),cy,symbols.get(key,key.upper()),r,touch=fid in ['quest','quest-ring','index','sense'],suffix='/input/'+key+'/click')
   return result
  def utility(key,cx,cy,symbol,r=9,pill=False):
   title={'system':'System','capture':'Capture'}.get(key,key.title())
   suffix=None if fid in ['focus','pico','pico-ultra','g2','sense'] else '/input/'+key+'/click'
   if not pill:
    return button(key,title,x(cx),cy,symbol,r,touch=fid=='frame',suffix=suffix)
   px,cy,r=face_geometry(x(cx),cy,r)
   body=f'<rect class="cap" x="{px-r}" y="{cy-4}" width="{2*r}" height="8" rx="4"/>'
   body+=f'<rect class="touch-ring" x="{px-r-3}" y="{cy-7}" width="{2*r+6}" height="14" rx="6"/>'
   body+=f'<rect class="hit" x="{px-r-3}" y="{cy-8}" width="{2*r+6}" height="16" rx="6"/>'
   if fid=='sense':body=f'<g transform="rotate(90 {px} {cy})">{body}</g>'
   return add(key,title,'button',body,px,cy,r,touch=fid=='frame',path_suffix=suffix)
  primary,secondary=('x','y') if hand=='left' else ('a','b')
  trigger_y=216;grip_y=302

  if shape=='frame':
   # Broad circular face and a long, softly bowed grip (Valve front diagram).
   shell=path('M91 155C87 200 91 262 98 316C102 343 112 359 130 359C150 358 157 340 159 313L167 192L184 164Z')
   face=circle(148,112,78,'rim')+circle(148,110,73,'face')
   detail=path('M108 194C107 235 111 303 121 343','seam')
   ui+=pad('thumbstick','Thumbstick',x(175),129,20)
   ui+=utility('view' if hand=='left' else 'menu',154,75,'▱' if hand=='left' else '≡',pill=True)
   ui+=utility('system',130,149,'○',9)
   if hand=='left':
    for key,angle in [('dpad_up',0),('dpad_right',90),('dpad_down',180),('dpad_left',270)]:
     radians=math.radians(angle+10)
     cx,cy=111+18*math.sin(radians),107-18*math.cos(radians)
     arm=path('M104 79h14v18l-7 7-7-7Z','cap',f'transform="rotate({angle+10} 111 107)"')
     arm+=path('M101 76h20v22l-10 10-10-10Z','touch-ring',f'transform="rotate({angle+10} 111 107)"')
     ui+=add(key,key.replace('_',' ').title(),'button',arm,cx,cy,12,True,path_suffix='/input/'+key+'/click')
   else:
    for key,angle in [('y',0),('b',90),('a',180),('x',270)]:
     radians=math.radians(angle-10)
     cx,cy=320-112+19*math.sin(radians),103-19*math.cos(radians)
     ui+=button(key,key.upper(),cx,cy,key.upper(),10,touch=True,suffix='/input/'+key+'/click')
   ui+=analog('bumper','Bumper',x(280),107,'B',True,'/input/bumper/click')
   trigger_y=203;grip_y=299

  elif shape in ['touch','touch-ring']:
   # Touch Plus / Quest 2 share input positions, not the tracking structure.
   # The grip sweeps outwards beneath the face instead of narrowing to a cone.
   shell=path('M87 121C86 153 67 196 53 240C40 281 45 319 68 333C92 349 115 325 130 294C146 260 152 221 176 194C191 179 207 165 211 137L181 107Z')
   detail=path('M87 163C78 200 59 252 58 287C57 307 63 319 73 324','seam')
   detail+=path('M172 183C157 180 143 182 137 193C132 204 133 217 141 222L157 222C161 204 166 194 172 183Z','hardware-button')
   if shape=='touch-ring':
    # Tilted halo: a thin rear arc and a deep front band with a true open centre.
    ring=ring_shape('M69 33C98 5 145 13 189 54C230 92 247 145 222 175C194 207 145 192 101 152C55 111 41 64 69 33Z M83 48C63 70 78 110 114 141C149 172 188 186 207 162C225 140 211 100 176 70C140 38 103 27 83 48Z')
    ring+=path('M69 33C98 5 145 13 189 54C220 84 239 121 237 147','seam')
    # Keep the loop seated at the deck while reducing its excessive rise.
    ring=f'<g transform="translate(0 49) scale(1 .72)">{ring}</g>'
   face=path('M75 115C74 78 110 57 154 62C196 64 224 87 222 118C220 151 187 174 146 172C105 170 76 151 75 115Z','rim')
   face+=plate('M80 110C80 81 112 62 153 67C191 69 218 89 217 115C214 143 184 163 147 162C110 160 81 143 80 110Z')
   deck_y=lambda value:62+.85*value if shape=='touch-ring' else value
   if shape=='touch-ring':
    shell=f'<g transform="translate(0 62) scale(1 .85)">{shell}</g>'
    face=f'<g transform="translate(0 62) scale(1 .85)">{face}</g>'
    detail=f'<g transform="translate(0 62) scale(1 .85)">{detail}</g>'
   ui+=pad('thumbstick','Thumbstick',x(114),deck_y(100),20)
   ui+=face_buttons(primary,secondary,(165,deck_y(139)),(184,deck_y(103)),12 if shape=='touch-ring' else 13)
   ui+=utility('menu' if hand=='left' else 'system',124,deck_y(145),'≡' if hand=='left' else '∞',9)

  elif shape=='index':
   # Rail and fabric strap are both outside the hand. The grip is inside the rail.
   ring=ring_shape('M86 55C49 85 32 135 37 195C44 278 88 340 151 352L166 350L163 333C108 326 69 271 61 193C55 140 69 98 94 73C103 64 98 48 86 55Z')
   ring+=path('M64 84 125 83 125 105 56 107Z','shell')
   strap=path('M112 122C87 133 73 160 73 194C75 240 91 270 116 292L129 280C111 256 99 229 96 196C94 168 100 149 120 139Z','strap')
   strap+=path('M109 128C87 151 84 179 86 202C89 239 102 265 120 285','stitch')
   strap+=path('M123 286C133 309 145 324 163 334','cord')
   shell=path('M119 133C120 177 114 211 124 262L137 335C140 353 162 360 178 347C187 339 184 318 183 299C179 250 183 204 196 159L205 127Z')
   detail=path('M141 163C130 213 146 273 150 323M180 185C167 222 167 267 173 287','seam')+circle(158,331,13,'inset')
   face=path('M110 97C112 72 141 53 175 57C203 59 224 81 222 106C219 137 192 157 164 159C134 158 108 137 110 97Z','rim')
   face+=plate('M115 96C119 75 143 58 173 62C200 64 219 84 216 106C213 131 190 151 165 152C137 151 113 133 115 96Z')
   ui+=pad('thumbstick','Thumbstick',x(132),104,17)
   ui+=pad('trackpad','Trackpad',x(166),92,10,oval=True,click=False)
   ui+=face_buttons('a','b',(190,126),(202,103),10)
   ui+=utility('system',173,143,'○',7)
   trigger_y=210;grip_y=306

  elif shape=='wand':
   # Vive's faceted tracking crown is much broader than its tapered handle.
   ring=ring_shape('M47 94C44 66 62 37 91 26C124 13 168 13 198 31C222 45 236 71 230 94L215 123L196 115L186 94H91L78 115L60 122Z M77 63C77 44 104 31 140 31C175 31 205 47 204 65C202 84 174 94 140 94C103 93 77 82 77 63Z')
   ring+=path('M60 78 81 91 89 107M218 78l-20 15-6 14','seam')
   shell=path('M92 97C97 134 106 161 107 199L115 335C116 360 126 376 143 376C160 376 169 360 171 337L179 196C179 157 188 126 190 97Z')
   face=plate('M104 119Q141 111 178 120L171 252Q141 266 112 251Z')
   detail=path('M127 279 133 354','seam')
   ui+=utility('menu',141,140,'≡',11)+pad('trackpad','Trackpad',x(141),196,30)
   ui+=utility('system',141,255,'◇',10)
   trigger_y=179;grip_y=295

  elif shape=='focus':
   # A nearly horizontal, broad halo sits above the face, supported at the sides.
   ring=ring_shape('M39 62C39 31 83 15 144 17C207 18 246 40 243 69L234 102C225 125 187 137 143 134C94 132 52 118 45 96Z M61 65C61 47 97 36 144 38C190 40 222 52 222 69C221 86 185 98 143 97C99 96 63 83 61 65Z')
   ring+=path('M45 76C60 101 101 113 145 113C187 114 224 105 239 84','seam')
   shell=path('M78 133C80 172 92 202 94 241L96 326C97 351 111 364 130 359C152 354 163 334 167 307L180 233C184 200 202 172 208 142Z')
   face=path('M71 133C71 101 106 84 146 87C190 87 218 109 215 139C212 171 183 196 144 195C105 193 75 171 71 133Z','rim')
   face+=plate('M78 132C78 107 107 91 147 94C183 93 210 113 208 138C205 164 179 188 145 187C111 186 82 165 78 132Z')
   detail=path('M112 217C108 260 110 312 122 343','seam')+path('M180 204Q159 197 157 216L158 239 169 236Z','hardware-button')
   ui+=pad('thumbstick','Thumbstick',x(110),125,20,touch=False)
   ui+=face_buttons(primary,secondary,(162,160),(179,135))
   ui+=utility('menu' if hand=='left' else 'system',111,172,'≡' if hand=='left' else '◇',9)
   trigger_y=226;grip_y=317

  elif shape=='pico':
   # One oblique loop: the far arc passes behind the deck; the near band crosses
   # the lower grip. Both arcs meet, with no exposed decorative curl endpoints.
   ring=ring_shape('M53 145C22 102 28 57 61 28C93-2 132 8 178 34L166 51C132 29 107 22 86 40C61 59 58 91 65 132Z')
   shell=path('M101 102C98 145 127 192 143 229L171 316C178 342 192 352 210 344C230 336 231 315 226 290L207 201C204 176 227 139 233 109Z')
   face=path('M91 104C88 67 116 32 158 29C201 26 237 53 239 91C241 124 223 153 196 168C171 178 137 160 117 143C101 129 94 118 91 104Z','rim')
   face+=plate('M98 102C94 69 119 39 158 36C196 34 229 57 232 92C235 121 216 146 192 160C172 169 141 153 122 137C106 124 101 114 98 102Z',True)
   detail=path('M160 226 193 323','seam')+path('M216 183C203 191 196 213 204 224L220 225 217 202Z','hardware-button')
   ring_front=path('M45 83C51 124 84 170 123 212L238 329C249 340 243 359 229 368C216 376 201 372 190 362L94 258C53 209 28 157 30 118C30 104 35 92 45 83Z','rim')
   ring_front+=path('M39 111C42 154 69 200 107 243L211 360','seam')
   ui+=pad('thumbstick','Thumbstick',x(148),91,19)
   ui+=face_buttons(primary,secondary,(175,135),(196,101),12)
   ui+=utility('menu' if hand=='left' else 'capture',120,128,'≡' if hand=='left' else '▣',9)
   ui+=utility('system',144,148,'○',9)
   trigger_y=218;grip_y=308

  elif shape=='ultra':
   # Ultra has its own straighter, heavier grip and white thumb deck.
   shell=path('M81 132C81 169 96 195 98 231L98 319C98 347 111 363 133 361C157 359 167 341 168 314L171 227C172 195 190 170 199 141Z')
   face=path('M72 118C71 83 103 60 144 62C186 63 218 88 218 121C218 155 185 179 146 179C107 178 75 154 72 118Z','rim')
   face+=plate('M79 115C79 89 105 68 144 69C182 70 211 92 211 120C211 148 182 171 146 171C111 170 82 150 79 115Z',True)
   detail=path('M116 211C113 256 113 316 125 346','seam')+path('M182 194Q162 186 155 202L156 227 169 226Z','hardware-button')
   ui+=pad('thumbstick','Thumbstick',x(111),101,19)
   ui+=face_buttons(primary,secondary,(166,141),(185,111),12)
   ui+=utility('menu' if hand=='left' else 'capture',104,143,'≡' if hand=='left' else '▣',9)+utility('system',134,158,'○',9)

  elif shape in ['wmr','g2']:
   # These reference designs have a tilted ring on stalks, not Touch's face halo.
   ring=ring_shape('M40 65C40 31 83 13 140 13C199 13 243 34 243 66L231 97C207 113 74 112 51 96Z M63 61C63 43 98 31 141 31C186 31 220 46 220 62C219 80 182 91 141 91C98 91 64 80 63 61Z')
   ring+=path('M51 96C87 110 198 112 231 97M83 105 89 140m109-35-7 35','seam')
   shell=path('M84 159C90 196 101 224 103 257L107 329C109 353 122 366 141 363C162 361 172 344 174 320L181 255C184 224 199 197 201 163Z')
   detail=path('M124 255 132 346','seam')
   if shape=='wmr':
    face=plate('M67 155C69 119 104 99 147 100C191 103 218 130 215 165L207 194C190 213 163 220 132 213C94 211 68 190 67 155Z')
    ui+=pad('thumbstick','Thumbstick',x(109),142,20,touch=False)
    ui+=pad('trackpad','Trackpad',x(170),165,30)
    ui+=utility('menu',112,187,'≡',9)+utility('system',141,242,'⊞',11)
   else:
    ring=ring_shape('M40 54C40 33 84 23 141 23C199 23 242 35 242 55L231 86C192 104 84 103 51 85Z M61 51C61 40 98 34 141 34C186 34 221 42 221 53C219 65 182 73 141 73C98 73 63 64 61 51Z')
    ring+=path('M51 85C97 98 193 98 231 86','seam')
    ring+=path('M85 94 92 120 103 117 98 97ZM187 97l-5 21 12 3 7-27Z','shell')
    face=path('M72 155C72 120 105 99 144 100C185 101 218 126 216 158C213 191 183 214 143 211C105 210 74 185 72 155Z','rim')
    face+=plate('M79 154C79 126 108 106 144 107C180 108 211 130 209 158C206 185 180 206 144 204C110 203 81 182 79 154Z')
    ui+=pad('thumbstick','Thumbstick',x(120),153,21,touch=False)
    ui+=face_buttons(primary,secondary,(166,185),(182,151),13)
    ui+=utility('menu',105,191,'≡',8)+utility('system',132,199,'⊞',8)
   trigger_y=223;grip_y=319

  elif shape=='sense':
   # Sony front view: a connected outer structure, not an open C plus a swoosh.
   ring=ring_shape('M222 76C178 40 118 40 76 65C28 94 13 149 20 202C23 225 29 247 42 264C76 290 125 304 174 303C215 302 243 281 248 253C251 236 236 223 216 220L204 151C205 124 230 102 222 76Z M145 129C145 103 128 89 109 92C76 95 54 121 53 148C52 173 68 187 96 192L146 199Z M49 264C94 266 136 279 182 286C200 289 215 286 227 279C214 296 180 298 147 292C107 287 71 277 49 264Z')
   shell=path('M151 129C149 164 145 191 148 225L155 268C159 286 176 292 191 280C201 270 197 251 194 232L193 167C193 144 211 122 212 101Z','dark-shell')
   face=plate('M108 76C142 53 194 59 222 82C237 97 220 123 207 142C195 158 193 173 194 187L151 187C151 155 153 127 141 109C131 92 118 83 108 76Z')
   ring_front=path('M38 222C86 213 158 227 222 246C242 252 249 260 241 275L228 292C219 301 205 301 191 295C142 273 86 263 42 264Z','rim')
   ring_front+=path('M43 231C97 225 162 242 219 261M111 273C112 264 117 256 124 252','seam')
   ui+=pad('thumbstick','Thumbstick',x(169),94,16,touch=True)
   primary,secondary=('square','triangle') if hand=='left' else ('cross','circle')
   ui+=face_buttons(primary,secondary,(199,121),(210,96),9)
   ui+=utility('create' if hand=='left' else 'options',136,83,'−' if hand=='left' else '≡',7,pill=True)
   ui+=utility('system',169,132,'PS',10)
   trigger_y=201;grip_y=315

  # Rear trigger and squeeze controls remain separately addressable indicators.
  # They are explicitly detached from the physical front-view drawing.
  ui+=analog('trigger','Trigger',x(280),trigger_y,'T',suffix='/input/trigger/value')
  digital=fid in ['vive','wmr','sense']
  ui+=analog('squeeze','Grip',x(280),grip_y,'G',digital,'/input/squeeze/'+('click' if digital else 'value'))
  callout=label(x(280),trigger_y-34,'TRIGGER',8)+label(x(280),grip_y-34,'GRIP',8)
  shell=f'<g data-part="tracking-ring">{ring}</g><g data-part="hand-strap">{strap}</g><g data-part="grip-shell">{shell}</g>'
  face=f'<g data-part="thumb-deck">{face}</g>'
  detail=f'<g data-part="hardware-details">{detail}</g><g data-part="tracking-ring-front">{ring_front}</g>'
  defs=f'''<defs><linearGradient id="shell" x1="0" y1="0" x2="1" y2=".5"><stop stop-color="{'#f0f2f4' if material=='light' else '#4a535f'}"/><stop offset=".48" stop-color="{'#c9d0d7' if material=='light' else '#303944'}"/><stop offset="1" stop-color="{'#838f9e' if material=='light' else '#151c24'}"/></linearGradient><linearGradient id="face" x2=".4" y2="1"><stop stop-color="#424d5a"/><stop offset="1" stop-color="#171e27"/></linearGradient><linearGradient id="rim" x2="1" y2="1"><stop stop-color="{'#e7edf1' if material=='light' else '#56626e'}"/><stop offset="1" stop-color="{'#a2aeba' if material=='light' else '#242e39'}"/></linearGradient><linearGradient id="lightFace" x2=".4" y2="1"><stop stop-color="#f1f3f4"/><stop offset="1" stop-color="#b3bec8"/></linearGradient></defs>'''
  artwork=f'<g transform="{mirror}">{shell}{face}{detail}</g>{callout}{ui}<ellipse class="haptic" cx="160" cy="345" rx="90" ry="22"/>'
  svg=f'<svg xmlns="http://www.w3.org/2000/svg" class="controller" viewBox="0 0 320 400" width="320" height="400" data-family="{fid}" data-hand="{hand}" data-connected="true" style="--accent:{"#6ca8ff" if hand=="left" else "#ff8a65"};--hardware-cap:{"#aeb8c0" if material=="light" else "#35414d"}"><title>{html.escape(name)} · {hand}</title>{defs}<style>{styles}</style>{artwork}</svg>'
  (OUT/'svg'/f'{uid}.svg').write_text(svg+'\n')
  sourcekey='quest' if fid=='quest-ring' else 'ultra' if fid=='pico-ultra' else fid
  models.append(dict(id=fid,name=name,brand=brand,subtitle=subtitle,hand=hand,shape=shape,profile=profile,source=SOURCES[sourcekey],file=f'svg/{uid}.svg',controls=controls,svg=svg))
(OUT/'library-data.js').write_text('/* Original artwork; generated by generate_assets.py. */\nwindow.CONTROLLER_LIBRARY = '+json.dumps(models)+';\n')
(OUT/'manifest.json').write_text(json.dumps({'version':4,'viewBox':[0,0,320,400],'families':len(families),'artwork':'Original reference-guided illustrations, revision 4; front/upper view with detached rear-input indicators. Exact pictured variants are documented in hardware-references.js.','models':[{k:v for k,v in m.items() if k!='svg'} for m in models]},indent=2)+'\n')
print(f'Generated {len(models)} SVGs across {len(families)} families.')
