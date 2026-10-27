#!/usr/bin/env python3
"""Synthetic Wiliwili responses for the production community UI replay."""
import hashlib
import json
from pathlib import Path
import sys
from urllib.parse import urlencode, quote

api='https://api.bilibili.com'
vc='https://api.vc.bilibili.com'
live='https://api.live.bilibili.com'
def seed(game):
    cache=game/'cache'
    cache.mkdir(parents=True,exist_ok=True)
    def fixture(endpoint, params, data):
        query=urlencode(params,quote_via=quote,safe='-_.~')
        url=endpoint+('?' + query if query else '')
        name='api_'+hashlib.md5(url.encode()).hexdigest()+'.json'
        (cache/name).write_text(json.dumps(dict(code=0,data=data),ensure_ascii=False))
    (game/'session.json').write_text(json.dumps(dict(version=1,cookies='SESSDATA=fixture; bili_jct=fixture; DedeUserID=42')))
    (cache/'session.json').write_text(json.dumps(dict(code=0,data=dict(isLogin=True,uname='离线测试账号',mid=42))))
    def dynamic(identity, title):
        return dict(id_str=identity, modules=dict(module_author=dict(name='示例作者',mid=7,pub_time='今天'),
            module_dynamic=dict(desc=dict(text=title),major=dict(archive=dict(bvid='BV1kRn36oEvr',aid=900001,title='正式页面视频',cover=''))),
            module_stat=dict(comment=dict(count=4))),basic=dict(comment_id_str='900001',comment_type=1))
    fixture(api+'/x/polymer/web-dynamic/desktop/v1/feed/all',[('page','1'),('offset',''),('host_mid','0')],dict(items=[dynamic('100','动态正文：通过正式服务进入详情。')],has_more=True,offset='offset-two'))
    fixture(api+'/x/polymer/web-dynamic/desktop/v1/feed/all',[('page','2'),('offset','offset-two'),('host_mid','0')],dict(items=[dynamic('101','第二页动态')],has_more=False,offset='done'))
    fixture(api+'/x/polymer/web-dynamic/desktop/v1/detail',[('id','100')],dict(item=dynamic('100','动态完整正文。'+('正文支持连续阅读。'*35))))
    fixture(vc+'/account/v1/user/cards',[('uids','7')],[dict(mid=7,name='示例作者',level=6,face='')])
    fixture(api+'/x/relation',[('fid','7')],dict(attribute=2))
    fixture(api+'/x/space/arc/list',[('mid','7'),('pn','1'),('ps','20')],dict(archives=[dict(aid=900001,bvid='BV1kRn36oEvr',title='用户投稿',duration=30)],page=dict(count=1)))
    fixture(api+'/x/polymer/web-dynamic/v1/uplist',[],dict(items=[dict(mid=7,uname='示例作者',face='',has_update=True)]))
    fixture(api+'/x/msgfeed/reply',[('id',''),('reply_time','')],dict(cursor=dict(id=0,time=0,is_end=True),items=[dict(id=1,user=dict(mid=7,nickname='示例作者'),item=dict(type='video',title='回复我的评论',source_content='你好，这是回复内容。',uri='https://www.bilibili.com/video/BV1kRn36oEvr',subject_id=900001))]))
    fixture(vc+'/session_svr/v1/session_svr/new_sessions',[('begin_ts','0'),('mobi_app','web')],dict(session_list=[dict(talker_id=7,session_type=1,account_info=dict(name='示例作者'),unread_count=2,last_msg=dict(content=json.dumps(dict(content='私信内容'))))],has_more=0))
    fixture(vc+'/svr_sync/v1/svr_sync/fetch_session_msgs',[('sender_device_id','1'),('talker_id','7'),('session_type','1'),('begin_seqno',''),('size','20'),('mobi_app','web')],dict(messages=[dict(msg_key=1,msg_seqno=2,sender_uid=7,msg_type=1,content=json.dumps(dict(content='通过真实私信解析路径加载。')))],has_more=0,max_seqno=2))
    fixture(api+'/x/v2/history/toview/web',[],dict(list=[dict(aid=900001)]))
    fixture(api+'/x/v3/fav/folder/created/list-all',[('rid','900001'),('type','2'),('up_mid','42')],dict(list=[dict(id=3,title='默认收藏夹',fav_state=1),dict(id=4,title='想看的视频',fav_state=0)]))
    for name in ('bangumi','cinema'):
        fixture(api+'/pgc/page/pc/'+name+'/tab',[('is_refresh','1'),('cursor','')],dict(modules=[dict(items=[dict(season_id=12,title='示例剧集',cover='')])],has_next=0,next_cursor=''))
    fixture(api+'/pgc/view/pc/season',[('season_id','12')],dict(title='示例剧集',evaluate='这里是剧集简介。',episodes=[dict(id=33,aid=900001,cid=600001,bvid='BV1kRn36oEvr',title='第1话',long_title='分集标题',duration=60000)]))
    fixture(live+'/xlive/app-interface/v2/index/getAreaList',[('platform','web')],dict(list=[dict(id=1,name='游戏',area_list=[dict(id=2,parent_id=1,name='掌机')])]))
    fixture(live+'/xlive/app-interface/v2/second/getList',[('parent_area_id','1'),('area_id','2'),('page','1'),('platform','web'),('device','switch'),('page_size','20'),('scale','xxhdpi'),('source_name','pc'),('mobi_app','pc_electron')],dict(list=[dict(roomid=100,title='示例直播间',uname='示例主播',uid=7)]))
    fixture(live+'/xlive/web-room/v2/index/getRoomPlayInfo',[('room_id','100'),('no_playurl','0'),('mask','1'),('qn','0'),('protocol','0,1'),('format','0,1,2'),('codec','0,1,2'),('platform','web'),('dolby','5'),('ptype','8'),('panorama','1')],dict(uid=7,live_status=1,playurl_info=dict(playurl=dict(stream=[dict(format=[dict(format_name='flv',codec=[dict(codec_name='avc',base_url='/stream.flv',current_qn=80,url_info=[dict(host='https://example.invalid',extra='')])])])]))))
    comment=dict(rpid=90000100,root=0,parent=0,member=dict(mid='42',uname='测试用户'),content=dict(message='评论全文。'+('回复页也可展开阅读。'*12)),like=5,rcount=1,action=1)
    reply=dict(rpid=90000101,root=90000100,parent=90000100,member=dict(mid='7',uname='示例作者'),content=dict(message='这是一条楼中楼回复。'),like=0,rcount=0,action=0)
    fixture(api+'/x/v2/reply/detail',[('oid','900001'),('root','90000100'),('type','1'),('next','0')],dict(root=dict(comment,replies=[reply]),cursor=dict(next=0,is_end=True)))
    return fixture
if __name__=='__main__':seed(Path(sys.argv[1]))
