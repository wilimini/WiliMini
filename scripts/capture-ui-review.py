#!/usr/bin/env python3
"""Capture actual PSP framebuffer output. Build first with the command in UI-DESIGN-PLAN.md."""
import argparse, json, shutil, subprocess
from pathlib import Path
from PIL import Image, ImageDraw
p=argparse.ArgumentParser();p.add_argument('--input-check',action='store_true');args=p.parse_args()
root=Path(__file__).resolve().parents[1];out=root/'results/ui-review';out.mkdir(parents=True,exist_ok=True)
mem=root/('build/ui-review-input-memstick' if args.input_check else 'build/ui-review-memstick')
game=mem/'PSP/GAME/WiliMini';game.mkdir(parents=True,exist_ok=True)
shutil.copytree(root/'build/ui-review-dist/PSP/GAME/WiliMini',game,dirs_exist_ok=True)
if not args.input_check:(game/'review-auto.txt').touch()
else:
 if (game/'review-auto.txt').exists():(game/'review-auto.txt').unlink()
frames=[(i*2+1.5,f'replica-{i+1:02}') for i in range(14)] if not args.input_check else [(1.5,'input-home'),(3.5,'input-detail'),(5.5,'input-player'),(6.5,'input-osd'),(8.5,'input-seek'),(10.5,'input-seek-adjusted'),(12.5,'input-paused'),(16.5,'input-quality'),(20.5,'input-danmaku'),(22.5,'input-hidden-osd'),(24.5,'input-back'),(26.5,'input-home-after-player'),(28.5,'input-focus'),(30.5,'input-grid-down'),(32.5,'input-grid-up')]
cmd=[str(root/'build/ppsspp-headless/PPSSPPHeadless'),'--graphics=software','--me-core','--root='+str(root),'--memstick='+str(mem),'--timeout=90','--timeout-emulated='+str(35 if args.input_check else 29),'--screenshot-at='+','.join(f'{t}:{out}/{name}.png' for t,name in frames)]
if args.input_check:cmd+=['--press=2:0x4000:0.2,4:0x4000:0.2,6:0x1000:0.2,8:0x20:0.2,10:0x40:0.2,11:0x80:0.2,12:0x4000:0.2,14:0x20:0.2,16:0x40:0.2,18:0x20:0.2,20:0x40:0.2,20.8:0x4000:0.2,21.5:0x8:0.2,22.5:0x1000:0.2,24.5:0x2000:0.2,26.5:0x2000:0.2,28.5:0x20:0.2,30.5:0x40:0.2,32.5:0x10:0.2']
cmd+=[str(root/'build/ui-review/loader/bootstrap.prx')]
for _,name in frames:
 path=out/(name+'.png')
 if path.exists():path.unlink()
log=root/('build/ui-review-input.log' if args.input_check else 'build/ui-review-capture.log')
with log.open('w') as f:r=subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT)
# Emulated timeout intentionally ends this continuously running UI, so inspect actual captures.
for _,name in frames:
 path=out/(name+'.png')
 if not path.exists():raise RuntimeError('Missing screenshot: '+str(path))
 im=Image.open(path)
 if im.height!=272 or im.width not in (480,512):raise RuntimeError('Unexpected framebuffer shape')
 if args.input_check:continue
 # Preserve raw PPSSPP screenshots; derive exact 480px display crops separately.
 im.crop((0,0,480,272)).save(out/(name+'-display.png'))
if not args.input_check:
 sheet=Image.new('RGB',(1000,7*304),'#d6d9df');d=ImageDraw.Draw(sheet)
 for i,(_,name) in enumerate(frames):
  sheet.paste(Image.open(out/(name+'-display.png')),(10+(i%2)*500,24+(i//2)*304));d.text((10+(i%2)*500,7+(i//2)*304),name,fill='#222222')
 sheet.save(out/'contact-sheet.png')
else:
 home=Image.open(out/'input-home.png'); back=Image.open(out/'input-home-after-player.png');detail=Image.open(out/'input-detail.png');player=Image.open(out/'input-player.png');osd=Image.open(out/'input-osd.png');seek=Image.open(out/'input-seek.png');adjusted=Image.open(out/'input-seek-adjusted.png');paused=Image.open(out/'input-paused.png');quality=Image.open(out/'input-quality.png');danmaku=Image.open(out/'input-danmaku.png');hidden=Image.open(out/'input-hidden-osd.png');focus=Image.open(out/'input-focus.png');down=Image.open(out/'input-grid-down.png');up=Image.open(out/'input-grid-up.png')
 assert back.getpixel((250,100))[:3] == (242,243,245),'Circle must leave playback and restore the home surface'
 assert back.tobytes()!=detail.tobytes(),'Playback return must not remain on the detail page'
 assert home.tobytes()!=detail.tobytes()!=player.tobytes(),'Cross must navigate'
 assert home.getpixel((34,53))!=focus.getpixel((34,53)),'Right must move card focus'
 assert player.tobytes()!=osd.tobytes()!=seek.tobytes()!=adjusted.tobytes()!=paused.tobytes(),'Player OSD, seek and pause states must differ'
 assert quality.tobytes()!=osd.tobytes(),'Quality control must show its value and selection'
 assert danmaku.tobytes()!=quality.tobytes(),'Danmaku control must be independently selectable'
 assert hidden.tobytes()!=danmaku.tobytes(),'Triangle must hide the control overlay'
 assert down.tobytes()!=focus.tobytes(),'Down must scroll to second row'
 assert up.tobytes()!=down.tobytes(),'Up must move the grid viewport back toward the first row'
 print('PASS: Inline player controls select/adjust; OSD, pause, seek, quality, danmaku; Down/Up scroll grid')
player_states=[('input-player.png','Full-screen'),('input-osd.png','Controls visible'),('input-seek-adjusted.png','Seek adjusted'),('input-quality.png','Quality selected'),('input-danmaku.png','Danmaku toggle')]
state_sheet=Image.new('RGB',(984,3*310),'#e2e5e9');state_draw=ImageDraw.Draw(state_sheet)
for i,(filename,label) in enumerate(player_states):
 x=8+(i%2)*492;y=(i//2)*310
 state_draw.text((x,y+5),label,fill='#333333')
 state_sheet.paste(Image.open(out/filename).crop((0,0,480,272)),(x,y+30))
state_sheet.save(out/'player-controls-preview.png')
(out/('input-verification.json' if args.input_check else 'capture-manifest.json')).write_text(json.dumps({'command':cmd,'exit_code':r.returncode,'intentional_emulated_timeout':True,'frames':[n for _,n in frames]},indent=2)+'\n')
print('Captured',len(frames),'frames in',out)
