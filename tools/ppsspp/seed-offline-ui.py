#!/usr/bin/env python3
"""Seed synthetic API fixtures into an isolated PPSSPP memstick (never live cache)."""
import argparse
import json
from pathlib import Path
from PIL import Image
from urllib.parse import quote

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('memstick', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
cache = args.memstick / 'PSP/GAME/WiliMini/cache'
cache.mkdir(parents=True, exist_ok=True)
home = json.loads((root / 'tests/fixtures/ui-home.json').read_text())
def write(name, data):
    (cache / name).write_text(json.dumps(data, ensure_ascii=False), encoding='utf-8')
write('home.json', home)
for i, card in enumerate(home['data']['list']):
    aid = 900001 + i
    detail = dict(card, aid=aid, title=card['title'], desc='把熟悉的视频体验装进口袋。保留封面、标题与创作者，让小屏幕里的信息自然连续。\n本页为离线 UI 验证数据。',
                  pubdate=1792800000, copyright=1,
                  owner={'mid':42, 'name':card['owner']['name'], 'face':''},
                  stat=dict(aid=aid, view=card['stat']['view'], danmaku=256, favorite=1024, coin=314, share=42, like=2048, reply=38),
                  rights=dict(download=0, no_reprint=1, is_cooperation=0),
                  pages=[dict(cid=card['cid']+j*100, page=j+1, duration=card['duration'], part=title) for j,title in enumerate(['从掌机开始的一天', '小屏幕上的信息与交互', '回到熟悉的视频社区'])])
    write(f"detail_{card['bvid']}.json", {'code':0,'data':detail})
    Image.open(root / f'client/assets/wiliwili/{8+i%2:02}.png').convert('RGB').resize((320,180)).save(cache / f"{card['bvid']}.jpg", quality=85)
    def reply(n, author, message):
        return dict(ctime=1792800000, action=0, rcount=0, like=128-n*7, rpid=aid*100+n, parent=0, root=0, oid=aid, replies=None,
                    content={'message':message}, member=dict(mid='42' if n==0 else str(100+n), uname=author, avatar='', is_senior_member=0, level_info={'current_level':5},vip={'nickname_color':''}))
    top=reply(0,'手柄观察室','这次想把掌机上的阅读体验做好。标题、作者和正文保持清楚的层次，不让太多边框打断阅读。\n这是离线测试评论，不代表真实用户发言。')
    replies=[reply(1,'旧设备玩家','喜欢这样直接展开的正文。评论超过两行也应该能继续读下去，向下滚动时保留自然的行距，而不是突然把后面的内容截掉。小屏幕需要更紧凑的布局，但不需要把文字压扁。'), reply(2,'慢镜头计划','封面可以大一点，元信息则安静地放在下面。按钮提示留在底部就够了。'), reply(3,'界面研究所','中文标点、English words 以及数字 480×272 应该使用一致的测量方式。多行文本也要检查上下边界。')]
    for page, items in [(0,replies),(1,[reply(4,'林间唱片','第二页已加载。返回详情再回到列表，应恢复原来选中的视频；本地稍后观看也应保留。')])]:
        write(f'comments_{aid}_{page}.json', {'code':0,'data':dict(top_replies=[top] if page==0 else [],replies=items,cursor=dict(mode=3,next=1 if page==0 else 0,prev=0,is_end=page==1,is_begin=page==0,all_count=5),upper={'mid':42})})
print(cache)

# Search follows the production API shapes; names are percent-encoded on both sides.
topics = ['秋日旅行与沿途风景', '旧设备还能做些什么', '开源掌机开发日志', '没有结果的关键词', '接口错误测试', '长标题与中文排版测试']
write('search_topics.json', {'code':0,'data':{'trending':{'list':[dict(keyword=word,show_name=word,icon='') for word in topics]}}})
(cache.parent/'search-history.json').write_text(json.dumps(['掌机游戏','旅行记录','游戏开发'],ensure_ascii=False))
for word in topics + ['掌机游戏','旅行记录','游戏开发']:
    for page in (1,2):
        cards = home['data']['list'][:4] if page==1 else home['data']['list'][4:]
        items = [dict(type='video',aid=900001+i+(4 if page==2 else 0),bvid=card['bvid'],author=card['owner']['name'],duration=f"{card['duration']//60}:{card['duration']%60:02}",pic='',play=card['stat']['view'],danmaku=256,like=2048,pubdate=1792800000,title='<em class="keyword">'+card['title']+'</em>') for i,card in enumerate(cards)]
        data = {'code':0,'data':dict(page=page,pagesize=4,numResults=6,numPages=2,result=items)}
        if word=='没有结果的关键词':
            data['data'].update(numResults=0,numPages=0,result=[])
        elif word=='接口错误测试':
            data={'code':-352,'message':'请求被拒绝，请稍后重试'}
        write(f'search_{quote(word,safe="")}_{page}.json',data)
