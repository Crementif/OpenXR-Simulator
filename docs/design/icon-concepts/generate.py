"""Author and regenerate the optional icon expansion. Application sources are untouched."""
from pathlib import Path
import json, html, re
OUT=Path(__file__).resolve().parent

def p(d,**attrs):
 return '<path d="'+d+'"'+''.join(f' {k.replace("_","-")}="{v}"' for k,v in attrs.items())+'/>'
def c(x,y,r,fill=False):
 return f'<circle cx="{x}" cy="{y}" r="{r}"'+(' fill="currentColor" stroke="none"' if fill else '')+'/>'
def r(x,y,w,h,rx=1.5):return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}"/>'
def dots(points):return ''.join(c(x,y,.8,True) for x,y in points)
icons=[]
def add(name,group,use,body):
 file=re.sub(r'(?<!^)(?=[A-Z])','-',name).lower()+'.svg'
 icons.append(dict(name=name,label=re.sub(r'([a-z])([A-Z])',r'\1 \2',name),category=group,use=use,file='svg/'+file,body=body))
g='Basics'
add('Add',g,'Add a device, field, keyframe or item',p('M12 5v14M5 12h14'))
add('Remove',g,'Remove an item from a collection',p('M5 12h14'))
add('Check',g,'Success, enabled or completed',p('m4 12 5 5L20 6'))
add('Undo',g,'Undo the last edit',p('M4 5v6h6M4 11l4-4a7 7 0 1 1-1 10'))
add('Redo',g,'Redo an undone edit','<g transform="translate(24 0) scale(-1 1)">'+p('M4 5v6h6M4 11l4-4a7 7 0 1 1-1 10')+'</g>')
add('Lock',g,'Lock pose, selection or value',r(5,10,14,11,2)+p('M8 10V7a4 4 0 0 1 8 0v3')+p('M12 14v3'))
add('Unlock',g,'Unlock pose, selection or value',r(5,10,14,11,2)+p('M8 10V7a4 4 0 0 1 7.5-2')+p('M12 14v3'))
add('Link',g,'Link devices or follow a parent',p('m10 8 3-3a4.2 4.2 0 0 1 6 6l-3 3M14 16l-3 3a4.2 4.2 0 0 1-6-6l3-3m1 5 6-6'))
add('Unlink',g,'Detach a parent relationship',p('m13 5 .5-.5a4.2 4.2 0 0 1 6 6l-.5.5M11 19l-.5.5a4.2 4.2 0 0 1-6-6l.5-.5M5 3v3H2m17 15v-3h3M8 8l8 8'))
g='Files & presets'
folder=p('M3 7V5a2 2 0 0 1 2-2h4l3 3h7a2 2 0 0 1 2 2v11a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z')
add('Folder',g,'Take library, script directory or asset folder',folder)
add('FolderOpen',g,'Browse a folder',p('M3 19V5a2 2 0 0 1 2-2h4l3 3h6a2 2 0 0 1 2 2M3 20l3-10h16l-3 10Z'))
add('Import',g,'Import a take, rig or configuration',p('M14 3h5a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2h-5M3 12h12m-4-4 4 4-4 4'))
add('Save',g,'Save a take or preset',p('M4 3h13l4 4v13a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1ZM7 3v6h9V3M7 21v-8h10v8'))
add('Copy',g,'Duplicate a value or item',r(8,8,13,13,2)+p('M5 16H4a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1h11a1 1 0 0 1 1 1v1'))
add('Clipboard',g,'Paste copied settings or pose values',r(8,2,8,4,1)+p('M8 4H5a1 1 0 0 0-1 1v15a1 1 0 0 0 1 1h14a1 1 0 0 0 1-1V5a1 1 0 0 0-1-1h-3M8 11h8m-8 4h5'))
add('Download',g,'Download or retrieve an asset',p('M12 3v12m-4-4 4 4 4-4M4 16v5h16v-5'))
add('Upload',g,'Upload or send an asset',p('M12 15V3m-4 4 4-4 4 4M4 16v5h16v-5'))
add('Preset',g,'Reusable rig or settings preset',r(3,3,18,18,2)+p('M8 6v12m8-12v12M6 10h4m4 4h4')+c(8,10,1.7,True)+c(16,14,1.7,True))
g='Views & panels'
window=r(3,4,18,16,2)
add('PanelLeft',g,'Show the device rail or left panel',window+p('M9 4v16M6 8v8'))
add('PanelRight',g,'Show the inspector or right panel',window+p('M15 4v16M18 8v8'))
add('PanelBottom',g,'Show timeline or bottom panel',window+p('M3 14h18M8 17h8'))
add('SplitView',g,'Compare two views side by side',window+p('M12 4v16m-5-8h1m8 0h1'))
add('StereoView',g,'Display left and right eye images',r(2,5,9,14,2)+r(13,5,9,14,2)+p('m4 14 2-3 3 4m6-1 2-3 3 4'))
add('FitView',g,'Fit preview to available space',p('M3 8V3h5m8 0h5v5m0 8v5h-5m-8 0H3v-5')+r(7,8,10,8,1))
add('Fullscreen',g,'Expand preview to full screen',p('M3 9V3h6m6 0h6v6m0 6v6h-6m-6 0H3v-6'))
add('Minimize',g,'Restore preview from full screen',p('M3 9h6V3m6 0v6h6m0 6h-6v6m-6 0v-6H3'))
add('Screenshot',g,'Capture the current preview image',p('M4 7h3l2-3h6l2 3h3a1 1 0 0 1 1 1v11a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1V8a1 1 0 0 1 1-1Z')+c(12,13,4))
g='Pose & space'
add('Translate',g,'Move along position axes',p('M12 2v20M2 12h20M9 5l3-3 3 3M9 19l3 3 3-3M5 9l-3 3 3 3m14-6 3 3-3 3'))
add('Rotate',g,'Rotate a selected device',p('M20 9a8 8 0 1 0 0 6M20 3v6h-6')+c(12,12,2))
add('Scale',g,'Change rig or room scale',r(3,13,8,8,1)+p('m11 13 10-10M14 3h7v7')+p('M3 9V3h6',stroke_dasharray='3 3'))
add('ResetPose',g,'Restore default device pose',p('M4 4v6h6M4 10a8 8 0 1 1 1 8')+p('m12 8-3 4h2v5h2v-5h2Z'))
add('Recenter',g,'Center the user or tracking origin',c(12,12,6)+c(12,12,1.8,True)+p('M12 2v3m0 14v3M2 12h3m14 0h3'))
add('Anchor',g,'Choose or pin a spatial anchor',c(12,5,2)+p('M12 7v14M7 11h10M3 14v3l3-1m15-2v3l-3-1M3 17c2 0 4 4 9 4s7-4 9-4'))
add('FloorHeight',g,'Adjust floor offset or standing height',p('M3 21h18M12 3v14m-3-3 3 3 3-3M8 3h8M5 17v1m14-1v1'))
add('Reach',g,'Change arm reach or controller distance',c(6,6,2)+p('M6 8v6l5 6M6 11l7-4M13 7h8m-3-3 3 3-3 3')+p('M3 21h18',stroke_dasharray='2 3'))
add('Snap',g,'Snap to grid, axis or angle',p('M5 3v8a7 7 0 0 0 14 0V3h-4v8a3 3 0 0 1-6 0V3ZM5 7h4m6 0h4')+dots([(5,21),(12,21),(19,21)]))
g='Tracking & rendering'
add('TrackingGood',g,'Pose tracking available',p('M8 3H3v5m13-5h5v5M3 16v5h5m13-5v5h-5m-9-9 3 3 7-7'))
add('TrackingLost',g,'Pose tracking lost or unavailable',p('M8 3H3v5m13-5h5v5M3 16v5h5m13-5v5h-5M9 9l6 6m0-6-6 6'))
add('Boundary',g,'Safety boundary or playspace bounds',p('m3 9 9-5 9 5v9l-9 4-9-4Z M3 9l9 5 9-5M12 14v8',stroke_dasharray='3 2')+c(12,10,1.5,True))
add('RoomScale',g,'Room-scale play area',p('m3 7 9-4 9 4v12l-9 3-9-3ZM12 3v7M3 7v12l9-4 9 4')+p('M9 13h6m-3-3v6'))
add('Calibration',g,'Calibrate tracking or device alignment',c(12,12,7)+p('M12 2v4m0 12v4M2 12h4m12 0h4M9 12l2 2 4-4'))
add('Reprojection',g,'Rotational reprojection or synthesized frames',r(3,4,12,9,1)+p('M18 11h3v10H9v-5M17 3a5 5 0 0 1 4 5m-3-1 3 1 1-3M7 21a5 5 0 0 1-5-5m-1 3 1-3 3 1'))
add('Foveation',g,'Foveated rendering or gaze focus region',p('M2 12s4-6 10-6 10 6 10 6-4 6-10 6S2 12 2 12Z')+c(12,12,3)+c(12,12,.8,True)+p('M4 3h2m12 0h2M4 21h2m12 0h2'))
add('Passthrough',g,'See the environment through the headset',p('M3 9h18v10h-6l-2-3h-2l-2 3H3Z M5 6V3h14v3m-7-3v4')+p('m7 12 2 2 3-3 5 3'))
add('Depth',g,'Inspect depth or distance layers',p('m3 7 9-4 9 4-9 4ZM3 12l9 4 9-4M3 17l9 4 9-4'))
g='Input & interaction'
add('Thumbstick',g,'Analog stick movement and click',c(12,8,5)+p('M9 12v5h6v-5M6 16c-2 .5-3 1.2-3 2 0 1.7 4 3 9 3s9-1.3 9-3c0-.8-1-1.5-3-2'))
add('Trigger',g,'Index trigger actuation',p('M6 3h11v5c0 7-5 12-11 13v-5c4-1 6-4 6-8H6ZM6 7h6'))
add('Grip',g,'Squeeze or grip actuation',r(9,3,6,18,3)+p('M2 12h4m-2-2 2 2-2 2m18-2h-4m2-2-2 2 2 2'))
add('HandTracking',g,'Hand pose and finger tracking',p('M7 12V6a1.5 1.5 0 0 1 3 0v5-7a1.5 1.5 0 0 1 3 0v7-6a1.5 1.5 0 0 1 3 0v7-4a1.5 1.5 0 0 1 3 0v7c0 4-2.5 7-6 7h-1c-2 0-3.5-1-4.5-2.5L3 13a1.5 1.5 0 0 1 2-2l2 1Z'))
add('Haptics',g,'Vibration or haptic feedback',r(8,5,8,14,3)+p('M5 7a10 10 0 0 0 0 10m14-10a10 10 0 0 1 0 10M3 4a18 18 0 0 0 0 16M21 4a18 18 0 0 1 0 16'))
add('Keyboard',g,'Keyboard bindings or keyboard pilot input',r(2,5,20,14,2)+dots([(6,9),(10,9),(14,9),(18,9),(6,12),(10,12),(14,12),(18,12)])+p('M7 16h10'))
add('Mouse',g,'Mouse look or pointer input',r(6,2,12,20,6)+p('M6 10h12M12 3v7')+p('M12 5v2',stroke_width='2.5'))
add('Bindings',g,'Map physical inputs to actions',c(5,5,2)+c(5,19,2)+c(19,12,2)+p('M7 5h2a3 3 0 0 1 3 3v8a3 3 0 0 1-3 3H7M12 12h5'))
add('Touch',g,'Capacitive contact without a press',p('M10 21v-5l-4-4a1.5 1.5 0 0 1 2-2l2 2V7a2 2 0 0 1 4 0v5h2a3 3 0 0 1 3 3v3l-2 3')+p('M6 6a6 6 0 0 1 12 0',stroke_dasharray='2 3'))
g='Recording & timeline'
add('Timeline',g,'Timeline overview and playhead',p('M3 6h18M3 18h18M9 4v16')+r(3,9,4,6,1)+r(12,9,9,6,1))
add('Take',g,'Recorded input take or capture clip',r(3,3,18,18,2)+p('M7 3v18M17 3v18M3 8h4m-4 8h4M17 8h4m-4 8h4')+c(12,12,2.5,True))
add('Loop',g,'Repeat a playback range',p('M4 10V8a3 3 0 0 1 3-3h13m-3-3 3 3-3 3M20 14v2a3 3 0 0 1-3 3H4m3-3-3 3 3 3'))
add('Marker',g,'Timeline bookmark or event marker',p('M5 22V3h14l-3 4 3 4H5'))
add('Trim',g,'Trim a selected capture range',p('M5 3v18M2 6h3m-3 12h3M19 3v18m0-15h3m-3 12h3')+r(8,8,8,8,1))
add('PlaybackSpeed',g,'Playback rate or time scale',p('M3 18a10 10 0 1 1 18 0M12 3v2M5 7l1.5 1.5M19 7l-1.5 1.5M12 13l4-5')+c(12,13,1.5)+p('m9 18 6 3-6 2Z',fill='currentColor',stroke='none'))
add('History',g,'Inspect past frames or recent changes',p('M3 4v6h6M3 10a9 9 0 1 1 1 8M12 7v6l4 2'))
add('CompareFrames',g,'Compare a live frame with a past frame',r(2,3,9,13,1)+r(13,3,9,13,1)+p('M6 20h12m-10-2-2 2 2 2m8-4 2 2-2 2'))
add('Keyframe',g,'Pose keyframe or animation waypoint',p('m12 5 7 7-7 7-7-7ZM2 12h3m14 0h3'))
g='Status & diagnostics'
add('Connection',g,'Connected runtime or device transport',p('M2 12h3m14 0h3M5 8h5v8H5ZM14 8h5v8h-5ZM10 10h4m-4 4h4'))
add('Disconnect',g,'Disconnected runtime or transport',p('M2 12h2m16 0h2M4 8h4v8H4ZM16 8h4v8h-4ZM8 10h2m-2 4h2m4-4h2m-2 4h2'))
add('Battery',g,'Device battery level',r(2,6,18,12,2)+p('M22 10v4M6 10v4m4-4v4m4-4v4'))
add('BatteryCharging',g,'Battery charging state',p('M8 6H4a2 2 0 0 0-2 2v8a2 2 0 0 0 2 2h3m9-12h2a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2h-2M22 10v4m-9-12-5 11h5l-2 9 6-12h-5Z'))
add('Performance',g,'Performance or CPU/GPU activity',p('M3 3v18h18M7 17v-5m5 5V8m5 9V4'))
add('FrameTime',g,'Frame-time trace and slow-frame spikes',p('M3 3v18h18M5 15h3l2-5 3 8 3-13 2 10h3'))
add('Memory',g,'GPU or host memory usage',r(5,5,14,14,2)+r(9,9,6,6,1)+p('M8 2v3m4-3v3m4-3v3M8 19v3m4-3v3m4-3v3M2 8h3m-3 4h3m-3 4h3m14-8h3m-3 4h3m-3 4h3'))
add('Log',g,'Runtime log, diagnostic output or console',r(3,3,18,18,2)+p('m6 8 3 3-3 3M12 15h5'))
add('Bug',g,'Validation issue or defect report',r(7,8,10,12,5)+p('M9 8V6a3 3 0 0 1 6 0v2M12 10v10M3 7l4 3m-5 4h5m-4 7 4-3M21 7l-4 3m5 4h-5m4 7-4-3M8 2l2 2m6-2-2 2'))

assert len(icons)==72
assert len({i['file'] for i in icons})==len(icons)
def svg(body,title='',size=24):
 return f'<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"><title>{html.escape(title)}</title>{body}</svg>'
for i in icons:(OUT/i['file']).write_text(svg(i['body'],i['label'])+'\n')
manifest=dict(name='OpenXR Simulator optional icons',grid=24,strokeWidth=1.75,color='currentColor',status='Asset proposals; no application enum assignments',icons=[{k:v for k,v in i.items() if k!='body'} for i in icons])
(OUT/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n')
(OUT/'icons.json').write_text(json.dumps(icons,ensure_ascii=False,indent=2)+'\n')
(OUT/'index.html').write_text((OUT/'gallery-template.html').read_text().replace('__ICONS__',json.dumps(icons,ensure_ascii=False)))
parts=['<svg xmlns="http://www.w3.org/2000/svg" width="1440" height="1530" viewBox="0 0 1440 1530"><rect width="1440" height="1530" fill="#13161b"/><g font-family="Arial,sans-serif"><text x="36" y="48" fill="#e6e9ef" font-size="26">OpenXR Simulator / Optional icon library</text><text x="36" y="76" fill="#8b93a1" font-size="14">72 original SVG assets · 24-unit grid · 1.75-unit stroke · basics and future feature concepts</text>']
for group_index,group in enumerate(dict.fromkeys(i['category'] for i in icons)):
 y=110+group_index*175
 parts.append(f'<text x="36" y="{y}" fill="#74c8d8" font-size="15">{html.escape(group)}</text>')
 for j,i in enumerate(x for x in icons if x['category']==group):
  x=36+j*152
  parts.append(f'<rect x="{x}" y="{y+14}" width="140" height="126" rx="8" fill="#1a1e25" stroke="#2c323d"/><g transform="translate({x+52} {y+32}) scale(1.5)" fill="none" color="#e6e9ef" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round">{i["body"]}</g><text x="{x+70}" y="{y+101}" text-anchor="middle" fill="#a2adba" font-size="11">{html.escape(i["label"])}</text>')
parts.append('</g></svg>');(OUT/'contact-sheet.svg').write_text(''.join(parts)+'\n')
print(f'Generated {len(icons)} icons, manifest, gallery and contact sheet.')
