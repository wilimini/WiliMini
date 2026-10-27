// Native PSP visual review. Included in ui.cpp to share its framebuffer lifecycle.
// Fixture screens; service integration is deliberately separate from this review build.
#include "assets/wiliwili/generated.hpp"
namespace {
constexpr uint32_t rvBg=color(242,243,245), rvInk=color(43,46,53), rvMuted=color(117,123,134);
constexpr uint32_t rvWhite=color(255,255,255), rvPink=color(255,102,153), rvFocus=color(0,174,236);
int rvClipTop=0, rvClipBottom=272;
void rvPixel(int x,int y,uint32_t c,unsigned a) {
    if(x<0||x>=480||y<rvClipTop||y>=rvClipBottom||!a)return;
    uint32_t &d=framebuffer()[y*512+x], out=0xff000000;
    for(int n=0;n<24;n+=8)out|=((((c>>n)&255)*a+((d>>n)&255)*(255-a))/255)<<n;
    d=out;
}
void rvText(int x,int y,const char* s,int size=14,uint32_t c=rvInk) {
    const unsigned char* p=(const unsigned char*)s;
    while(*p){unsigned cp=*p++;if(cp>=0xc0){int n=(cp<0xe0?1:2);cp&=(n==1?31:15);while(n--&&*p)cp=(cp<<6)|(*p++&63);}
        const ReviewGlyph* g=nullptr;for(const auto& v:reviewGlyphs)if(v.cp==cp&&v.size==size){g=&v;break;}
        if(!g){x+=size;continue;} if(x+g->advance>474)break;
        for(int yy=0;yy<size+5;yy++)for(int xx=0;xx<size+2;xx++)rvPixel(x+xx,y+yy,c,((reviewAlpha[(g->offset+yy*(size+2)+xx)/2] >> (((g->offset+yy*(size+2)+xx)%2)*4)) & 15)*17);
        x+=g->advance;
    }
}
// A single surface: compact rows, a shared baseline, and a small focus marker.
void rvAsset(int id,int x,int y,int w=0,int h=0,int radius=0) {
    const auto& a=reviewImages[id];
    static std::vector<unsigned short> decoded[10];
    auto& data=decoded[id];
    if(data.empty()) for(unsigned i=0;i<a.count;i+=2)
        data.insert(data.end(),reviewPixels[a.offset+i],reviewPixels[a.offset+i+1]);
    if (!w) w = a.w;
    if (!h) h = a.h;
    for(int yy=0;yy<h;yy++) for(int xx=0;xx<w;xx++) {
        if(radius){int dx=std::max(radius-xx-1,xx-(w-radius));int dy=std::max(radius-yy-1,yy-(h-radius));if(dx>0&&dy>0&&dx*dx+dy*dy>radius*radius)continue;}
        unsigned v=data[(yy*a.h/h)*a.w+xx*a.w/w];
        rvPixel(x+xx,y+yy,color(((v>>8)&15)*17,((v>>4)&15)*17,(v&15)*17),((v>>12)&15)*17);
    }
}
int rvParagraph(int x,int y,const char* text,int width,int maxLines=3) {
    // Wrap on Unicode codepoint boundaries using the same advances as drawing.
    std::string line;int used=0,lines=0;
    const unsigned char* p=(const unsigned char*)text;
    while(*p&&lines<maxLines){const unsigned char* start=p;unsigned cp=*p++;if(cp>=0xc0){int n=cp<0xe0?1:2;cp&=n==1?31:15;while(n--&&*p)cp=(cp<<6)|(*p++&63);}
        int advance=14;for(const auto& g:reviewGlyphs)if(g.cp==cp&&g.size==14){advance=g.advance;break;}
        if(used+advance>width||cp=='\n'){rvText(x,y+lines*18,line.c_str());lines++;line.clear();used=0;if(lines==maxLines)break;}
        if(cp!='\n'){line.append((const char*)start,p-start);used+=advance;}
    }
    if(!line.empty()&&lines<maxLines){rvText(x,y+lines*18,line.c_str());lines++;}
    return lines*18;
}
void rvLine(int y,int x=38){fillRect(x,y,468-x,1,color(221,224,229));}
void rvMark(int x,int y,int h=16){fillRect(x,y,2,h,rvPink);}
void rvTabs(const char* selected,const char* others,int x=38){rvText(x,30,selected,12,color(204,65,113));rvText(x+65,30,others,12,rvMuted);}
void rvItem(int y,const char* title,const char* meta,bool focus=false){if(focus)rvMark(32,y+3,33);rvText(39,y,title,14);rvText(39,y+19,meta,12,rvMuted);}
void rvVideo(int y,int id,const char* title,const char* subtitle,const char* meta,bool focus=false){
    if(focus)rvMark(32,y,56);
    rvAsset(id,39,y,100,56,4);
    rvText(147,y-2,title,14,focus?color(204,65,113):rvInk);
    rvText(147,y+17,subtitle,14);
    rvText(147,y+38,meta,12,rvMuted);
}
void rvShell(const char* title,int nav,int page){
    fillRect(0,0,480,272,rvBg);
    bool nested=page>=6&&page<=8;
    if(!nested){
        for(int i=0;i<5;i++)rvAsset(3+i,6,i<3?38+i*33:191+(i-3)*30,19,19);
        rvMark(0,nav<3?37+nav*33:190+(nav-3)*30,22);
    }
    rvText(nested?12:38,4,title,16);
    rvText(389,7,page==7?"按键控制":"布局预览",12,rvMuted);
    rvLine(26,nested?12:38);
    if (page != 7) {
        rvLine(253,12);
        rvText(12,255,page==8?"↑↓ 选择  × 回复  △ 写评论  ○ 返回":"L/R 翻页   × 预览   ○ 返回",12,rvMuted);
        char n[32];std::snprintf(n,sizeof(n),"%02d/14",page+1);rvText(433,255,n,12,rvMuted);
    }
}
void rvComment(int y,const char* author,const char* time,const char* body,const char* reply,bool selected){
    if(selected)rvMark(6,y+2,54);
    rvText(13,y,author,12,selected?color(204,65,113):rvMuted);rvText(337,y,time,12,rvMuted);
    int height=rvParagraph(13,y+17,body,450,2);
    rvText(13,y+19+height,reply,12,rvMuted);
}
}
void ui_render_review(int page,int focus,unsigned playerFlags,int playerSeconds,
                      int playerControl,int playerQuality,int playerDanmaku,
                      int playerPart,int playerAspect){
 beginFrame();
 const char* titles[]={"推荐","搜索","我的 / 扫码登录","我的 / 等待确认","我的 / 二维码过期","我的","视频 / 详情","正在播放","视频 / 评论  128","动态","发现","收藏与稍后再看","消息","设置"};
 rvShell(titles[page],(page>=2&&page<=5)||page==11?2:page==9?1:page==12?3:page==13?4:0,page);
 switch(page){
 case 0:{
  rvTabs("推荐","直播   热门   追番   影视   搜索");
  static float scroll=0, velocity=0;
  static unsigned long long lastTime=0;
  const auto now=sceKernelGetSystemTimeWide();
  float dt=lastTime?std::min(0.033f,float(now-lastTime)/1000000.0f):0.016f;
  lastTime=now;
  const int selected=focus%4;
  const float target=selected>=2?120.0f:0.0f;
  // Damped spring keeps the selected row visible without snapping the viewport.
  velocity+=(190.0f*(target-scroll)-24.0f*velocity)*dt;
  scroll+=velocity*dt;
  if(scroll<0){scroll=0;velocity=0;}
  const char* names[]={"把世界装进口袋","周末出发，去看看远方","老掌机的新玩法","沿着山路，走到海边"};
  const char* stats[]={"旅行记录 · 12.8万 · 12:40","日常观察 · 3.6万 · 08:24","开发日志 · 8,240 · 06:18","旅行记录 · 2.1万 · 10:32"};
  rvClipTop=49;rvClipBottom=251;
  for(int i=0;i<4;i++){
    int x=39+(i%2)*218, y=53+(i/2)*162-int(scroll+0.5f);
    rvAsset(8+i%2,x,y,210,118,4);
    rvText(x,y+120,names[i],14,i==selected?color(204,65,113):rvInk);
    rvText(x,y+140,stats[i],12,rvMuted);
    if(i==selected)for(int yy=y;yy<y+118;yy++)for(int xx=x-5;xx<x-3;xx++)rvPixel(xx,yy,rvPink,255);
  }
  rvClipTop=0;rvClipBottom=272;
  break;}
 case 1:
  rvText(39,34,"搜索视频、番剧、影视",14,rvMuted);rvLine(56);rvText(39,64,"历史",12,rvMuted);rvText(95,63,"掌机游戏    旅行记录    游戏开发",14);
  rvText(39,95,"热搜",12,color(204,65,113));
  rvItem(115,"01   秋日旅行与沿途风景","热度 128.4万",true);rvItem(158,"02   旧设备还能做些什么","热度 96.2万");rvItem(201,"03   开源掌机开发日志","热度 84.6万");break;
 case 2:case 3:case 4:
  rvText(39,36,"使用哔哩哔哩手机客户端扫码",14);
  fillRect(171,64,128,128,rvWhite);rvText(192,101,page==4?"已过期":"扫码区域",16,rvMuted);rvText(186,134,"未连接登录服务",12,rvMuted);
  rvAsset(0,71,111);rvAsset(1,333,111);
  rvText(154,201,page==2?"等待扫码":page==3?"已扫码，请在手机确认":"二维码已失效",14,color(204,65,113));
  rvText(166,228,page==4?"× 刷新二维码":"○ 取消登录",12,rvMuted);break;
 case 5:
  rvAsset(2,39,36,34,34,4);rvText(83,32,"掌机旅行者",16);rvText(83,55,"示例账号 · 关注 128 · 粉丝 256",12,rvMuted);
  rvText(39,83,"历史记录   收藏夹   稍后再看   追番 / 追剧",12,color(204,65,113));
  rvVideo(109,8,"把世界装进口袋","继续上次旅程","看到 03:18 / 12:40 · 今天",true);
  rvVideo(177,9,"周末出发，去看看远方","旅行记录","看到 01:26 / 08:24 · 昨天");break;
 case 6:
  rvAsset(8,12,35,128,72,4);rvText(151,32,"把世界装进口袋：掌机旅行",16);rvText(151,55,"旅行记录  ·  关注",12,color(204,65,113));rvText(151,75,"12.8万播放  /  256弹幕  /  12:40",12,rvMuted);rvText(151,96,"× 播放    赞 1280    投币    收藏",12);
  rvParagraph(12,121,"带上掌机去旅行，记录沿途的山路、海风和小镇。\n这一期聊聊：有限的屏幕，怎样展示更多有用的信息。",454,2);
  rvLine(165,12);rvText(12,173,"分P  2",12,color(204,65,113));rvText(99,173,"评论 128    相关推荐    简介",12,rvMuted);
  rvText(12,199,"01  出发，沿着山路去海边",14);rvText(404,201,"12:40",12,rvMuted);
  rvText(12,227,"02  回到小镇，整理一路的记录",14);rvText(404,229,"08:24",12,rvMuted);break;
 case 7:
  fillRect(0,0,480,272,color(22,25,31));rvAsset(8,0,0,480,197);
    if (playerFlags & (1u | 2u | 8u)) {
    fillRect(0,0,480,24,color(10,12,15));
    rvText(12,5,"把世界装进口袋 · P1",14,rvWhite);
    rvText(323,6,"←→选择 ↑↓调整 ×操作",12,rvWhite);
    fillRect(0,197,480,56,color(10,12,15));
    char clock[32];std::snprintf(clock,sizeof(clock),"%02d:%02d / 12:40",playerSeconds/60,playerSeconds%60);
    rvText(12,201,(playerFlags&2u)?"暂停":"播放中",14,rvWhite);
    rvText(378,201,clock,12,rvWhite);
    fillRect(12,220,456,2,rvMuted);fillRect(12,220,playerSeconds*456/760,2,rvPink);
    const int positions[]={12,68,222,300,372,427};
    const char* controls[]={playerFlags&2u?"▶ 继续":"Ⅱ 暂停","定位","360P","弹幕关","P1","比例"};
    const char* quality[]={"360P","480P","720P"};
    const char* danmaku[]={"弹幕关","弹幕开"};
    const char* parts[]={"P1","P2"};
    const char* aspects[]={"原比例","铺满","裁切"};
    for(int i=0;i<6;i++){
      const char* value=i==2?quality[playerQuality]:i==3?danmaku[playerDanmaku]:i==4?parts[playerPart-1]:i==5?aspects[playerAspect]:controls[i];
      const uint32_t tint=i==playerControl?rvPink:rvWhite;
      rvText(positions[i],232,value,12,tint);
      if(i==playerControl)fillRect(positions[i],249,(i==0?48:i==1?42:i==2?34:i==3?44:i==4?18:36),2,rvPink);
    }
  }
  break;
 case 8:
  rvTabs("热门","最新   /   共128条评论",13);
  rvComment(52,"山间旅行者","2小时前 · 128赞","这段山路拍得真舒服。小屏幕也能看清沿途的风景，\n比起堆满按钮，我更喜欢这样安静地看完一段旅程。","UP主：下一期去海边。  查看全部12条回复 ›",focus%3==0);
  rvLine(127,13);
  rvComment(133,"开发日志  UP","1小时前 · 36赞","字幕和进度条不应该挡住画面。信息集中在需要的地方，\n操作的时候才出现，看视频的时候就把屏幕还给内容。","查看3条回复 ›",focus%3==1);
  rvLine(208,13);
  rvComment(214,"路过的玩家","刚刚 · 6赞","这个排版舒服多了，正文终于不再挤成一小行。","",focus%3==2);break;
 case 9:
  rvTabs("全部动态","关注的人   图文");
  rvVideo(53,8,"旅行记录  ·  2小时前","新作品：把世界装进口袋","12.8万播放 · 128条评论",focus%3==0);
  rvVideo(119,9,"日常观察  ·  4小时前","今天也去看看远方","3.6万播放 · 36条评论",focus%3==1);
  rvText(39,188,"开发日志  ·  昨天",12,color(204,65,113));rvParagraph(39,207,"这周继续调整掌机界面。先把文字排好，\n再考虑要不要加装饰。",420,2);break;
 case 10:
  rvTabs("直播","番剧   影视   分区 / 筛选");rvVideo(53,8,"游戏与生活","正在直播：一起玩一会儿","主播示例 · 1.2万人气",true);
  rvText(39,122,"本周更新",12,color(204,65,113));rvItem(145,"山海之间  ·  更新至第8话","周六更新 / 继续第3话");rvItem(191,"小镇日记  ·  全12话","完结 / 日常 / 查看分集");break;
 case 11:
  rvTabs("收藏夹","稍后再看   历史记录");
  rvItem(55,"默认收藏夹","12个视频 · 今天更新",focus%4==0);rvItem(101,"旅行与生活","8个视频 · 昨天更新",focus%4==1);rvItem(147,"游戏开发笔记","24个视频 · 3天前更新",focus%4==2);rvItem(193,"稍后再看","3个视频 · 继续上次进度",focus%4==3);break;
 case 12:
  rvTabs("回复我的","私信   @我   收到的赞");
  rvItem(55,"山间旅行者  ·  回复你的评论","这段路太美了，下次也想去看看。",focus%4==0);rvText(39,92,"5分钟前 · 把世界装进口袋",12,rvMuted);rvLine(111);
  rvItem(119,"开发日志  ·  回复你的评论","谢谢建议，下个版本会调整字幕位置。",focus%4==1);rvText(39,156,"1小时前 · 老掌机的新玩法",12,rvMuted);rvLine(175);
  rvItem(183,"日常观察  ·  回复你的评论","原来你也走过这条山路！",focus%4==2);rvText(39,220,"昨天 · 周末出发",12,rvMuted);break;
 case 13:{
  rvTabs("界面","播放   网络   账号   关于");
  const char* names[]={"主题","正文字号","列表布局","封面加载","播放清晰度","缓存容量","Wi-Fi","开源许可"};
  const char* values[]={"浅色","14 px","紧凑列表","按需加载","360P 优先","32 MB","连接设置 ›","查看 ›"};
  for(int i=0;i<8;i++){int y=55+i*23;if(focus%8==i)rvMark(32,y+3,16);rvText(39,y,names[i]);rvText(340,y,values[i],14,focus%8==i?color(204,65,113):rvMuted);}break;}
 }
 presentFrame();
}
