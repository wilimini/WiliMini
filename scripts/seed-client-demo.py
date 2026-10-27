#!/usr/bin/env python3
"""Fetch real public Bilibili data for an explicitly offline USB demo.
This does not proxy PSP traffic or transcode video. PSP online uses its own curl.
"""
import argparse,json,urllib.request,hashlib
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--video',action='store_true',help='also cache the first actual 360P MP4')
a=p.parse_args()
r=Path(__file__).resolve().parents[1];cache=r/'dist/PSP/GAME/WiliMini/cache';cache.mkdir(parents=True,exist_ok=True)
def get(url):
    return urllib.request.urlopen(urllib.request.Request(url,headers={'User-Agent':'Mozilla/5.0','Referer':'https://www.bilibili.com/'}),timeout=45).read()
raw=get('https://api.bilibili.com/x/web-interface/popular?ps=6&pn=1');j=json.loads(raw)
assert j['code']==0,j
(cache/'home.json').write_bytes(raw)
for i,v in enumerate(j['data']['list'][:6]):
    data=get(v['pic']+'@288w_162h_1c.jpg');(cache/(v['bvid']+'.jpg')).write_bytes(data)
    print(v['bvid'],v['title'],flush=True)
    if i==0 and a.video:
        response=json.loads(get('https://api.bilibili.com/x/player/playurl?bvid=%s&cid=%s&qn=16&fnval=1&platform=html5'%(v['bvid'],v['cid'])))
        assert response['code']==0,response
        streams=response['data']['durl']
        data=b''.join(get(stream['url'].replace('\\u0026','&')) for stream in streams)
        assert len(data)==sum(stream['size'] for stream in streams)
        (cache/('%s_%s.mp4' % (v['bvid'], v['cid']))).write_bytes(data)
        print('MP4',len(data),hashlib.sha256(data).hexdigest(),flush=True)
