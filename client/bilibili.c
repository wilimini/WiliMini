#include "services/playback_request_policy.h"
#include "bilibili.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#ifndef HOST_TEST
#include <pspkernel.h>
#include <psputility.h>
#include <psputility_sysparam.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <pspnet_apctl.h>
#include <pspwlan.h>
#include <curl/curl.h>
#include "http_transport.h"
#include <unistd.h>
#include <pspiofilemgr.h>
#endif
#ifndef BILI_STREAM_PROFILE
#define BILI_STREAM_PROFILE 0
#endif
#ifndef BILI_STREAM_FORCE_HTTP
#define BILI_STREAM_FORCE_HTTP 0
#endif
#ifndef BILI_STREAM_CURL_BUFSIZE
#define BILI_STREAM_CURL_BUFSIZE 0
#endif
#ifndef BILI_STREAM_THREAD_PRIORITY
#define BILI_STREAM_THREAD_PRIORITY 0x28
#endif
#ifndef BILI_STREAM_USE_RANGE
#define BILI_STREAM_USE_RANGE 0
#endif
/* Leave 1 GiB below UINT_MAX: stream_cache_write() adds a chunk length to a
 * 32-bit ring position before comparing it with the RAM window. */
#define STREAM_MAX_BYTES (3ULL * 1024 * 1024 * 1024)
volatile int transfer_cancel;
volatile unsigned transfer_kb, transfer_total_kb;
char network_error[160];
static JsonToken tokens[8192];
static int parse(Json *j,const char *text) {
    if(json_parse(j,text,tokens,8192)<0){snprintf(network_error,sizeof(network_error),"Invalid API JSON");return -1;}
    int code=json_key(j,0,"code");
    if(code<0 || json_number(j,code)!=0){snprintf(network_error,sizeof(network_error),"Bilibili API: %lld",json_number(j,code));return -1;}return 0;
}
int bili_parse_home(const char *text,BiliHome *out) {
    Json j;if(parse(&j,text)<0)return -1;
    int data=json_key(&j,0,"data"),list=json_key(&j,data,"list");
    if(list<0 || text[tokens[list].start]!='[')return -1;
    memset(out,0,sizeof(*out));
    for(int n=list+1;n<tokens[list].next && out->count<HOME_COUNT;n=tokens[n].next) {
        BiliVideo *v=&out->videos[out->count];
        json_string(&j,json_key(&j,n,"bvid"),v->bvid,sizeof(v->bvid));
        json_string(&j,json_key(&j,n,"title"),v->title,sizeof(v->title));
        json_string(&j,json_key(&j,n,"pic"),v->picture,sizeof(v->picture));
        json_string(&j,json_key(&j,json_key(&j,n,"owner"),"name"),v->author,sizeof(v->author));
        v->cid=json_number(&j,json_key(&j,n,"cid"));v->duration=json_number(&j,json_key(&j,n,"duration"));
        v->views=json_number(&j,json_key(&j,json_key(&j,n,"stat"),"view"));
        if(strncmp(v->bvid,"BV",2) || v->cid<=0)continue;
        ++out->count;
    }return out->count?0:-1;
}
int bili_parse_stream(const char *text,BiliStream *out) {
    Json j;if(parse(&j,text)<0)return -1;
    int data=json_key(&j,0,"data"),durl=json_key(&j,data,"durl");
    if(durl<0 || tokens[durl].children<=0 || tokens[durl].children>BILI_STREAM_SEGMENTS){
        snprintf(network_error,sizeof(network_error),"MP4 stream unavailable");return -1;
    }
    memset(out,0,sizeof(*out));
    int stream=durl+1;
    for(int i=0;i<tokens[durl].children;++i){
        if(stream<0 || stream>=8192)return -1;
        char *url=out->segments[i].url;
        if(json_string(&j,json_key(&j,stream,"url"),url,sizeof(out->segments[i].url))<0)return -1;
        unsigned long long size=(unsigned long long)json_number(&j,json_key(&j,stream,"size"));
        /* Some HTML5 responses encode separators twice. Decode only this known
         * URL separator; generic repeated JSON unescaping would damage signatures. */
        char *p;while((p=strstr(url,"\\u0026"))){*p='&';memmove(p+1,p+6,strlen(p+6)+1);}
        if(strncmp(url,"https://",8) && strncmp(url,"http://",7))return -1;
        if(!size || size>STREAM_MAX_BYTES || out->size>STREAM_MAX_BYTES-size){
            snprintf(network_error,sizeof(network_error),"Unsupported video size");return -1;
        }
        out->segments[i].size=size;
        out->size+=size;
        stream=tokens[stream].next;
    }
    out->count=tokens[durl].children;
    snprintf(out->url,sizeof(out->url),"%s",out->segments[0].url);
    return 0;
}
#ifndef HOST_TEST
static int initialized;
enum { NET_IDLE, NET_CONNECTING, NET_AUTHENTICATING, NET_GETTING_IP,
       NET_SIGNING, NET_RECOMMEND, NET_FALLBACK, NET_REQUEST, NET_PLAYURL, NET_VIDEO_SOURCE, NET_DETAIL, NET_PARTS, NET_COMMENTS, NET_RELATION, NET_PROGRESS };
static volatile int network_phase;
void network_progress_reset(void) { network_phase=NET_IDLE; }
const char *network_progress_message(void) {
    switch(network_phase){
    case NET_CONNECTING:return "正在连接 Wi-Fi…";
    case NET_AUTHENTICATING:return "正在验证 Wi-Fi 连接…";
    case NET_GETTING_IP:return "正在获取 IP 地址…";
    case NET_SIGNING:return "正在准备请求签名…";
    case NET_RECOMMEND:return "正在获取推荐列表…";
    case NET_FALLBACK:return "正在获取热门列表…";
    case NET_PLAYURL:return "正在获取播放地址…";
    case NET_VIDEO_SOURCE:return "正在连接视频源…";
    case NET_PROGRESS:return "正在同步观看进度…";
    case NET_DETAIL:return "正在获取视频详情…";
    case NET_PARTS:return "正在获取分集信息…";
    case NET_COMMENTS:return "正在获取视频评论…";
    case NET_RELATION:return "正在获取收藏与互动状态…";
    case NET_REQUEST:return "正在请求数据…";
    default:return "";
    }
}
static CURL *transport;
static CURL *api_transport; /* Serialized JSON requests, separate from media/covers. */
static char buvid3[33];
static char session_cookies[4096];
void bili_http_set_session(const char *cookies) {
    snprintf(session_cookies, sizeof(session_cookies), "%s", cookies ? cookies : "");
}

static unsigned buvid_seed;

static unsigned buvid_random(void) {
    unsigned value=buvid_seed;
    value^=value<<13;value^=value>>17;value^=value<<5;
    buvid_seed=value;
    return value;
}

static int buvid3_valid(const char *value) {
    if(!value || strlen(value)!=32)return 0;
    for(int i=0;i<32;++i){
        const char c=value[i];
        if(!((c>='0'&&c<='9')||(c>='A'&&c<='F')))return 0;
    }
    return strcmp(value+28,"infoc")!=0;
}

/* Wiliwili uses a random 32-character uppercase buvid3 for anonymous
 * sessions. Keep it across launches so Bilibili sees a stable client. */
static void network_ensure_buvid3(void) {
    if(buvid3[0])return;
    SceUID fd=sceIoOpen(CLIENT_PATH("/buvid3.txt"),PSP_O_RDONLY,0);
    if(fd>=0){
        char stored[34]={0};
        const int read=sceIoRead(fd,stored,32);
        sceIoClose(fd);
        if(read==32 && buvid3_valid(stored)){
            memcpy(buvid3,stored,32);buvid3[32]=0;
            return;
        }
    }
    static const char hex[]="0123456789ABCDEF";
    buvid_seed=(unsigned)sceKernelGetSystemTimeWide() ^ (unsigned)sceKernelGetSystemTimeLow() ^ 0xA5C31E27u;
    for(int i=0;i<32;++i)buvid3[i]=hex[buvid_random()&15u];
    buvid3[32]=0;
    fd=sceIoOpen(CLIENT_PATH("/buvid3.txt"),PSP_O_WRONLY|PSP_O_CREAT|PSP_O_TRUNC,0777);
    if(fd>=0){
        sceIoWrite(fd,buvid3,32);
        sceIoClose(fd);
    }
}

int network_init(void) {
    int mods[]={PSP_MODULE_NET_COMMON,PSP_MODULE_NET_INET};
    for(unsigned i=0;i<2;++i){int r=sceUtilityLoadModule(mods[i]);if(r<0 && (unsigned)r!=SCE_ERROR_MODULE_ALREADY_LOADED)return r;}
    int r=sceNetInit(1024*1024,0x30,0x1000,0x30,0x1000);if(r<0)return r;
    if((r=sceNetInetInit())<0)return r;
    if((r=sceNetResolverInit())<0)return r;
    if((r=sceNetApctlInit(0x8000,0x30))<0)return r;
    if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK)return -1;
    transport=curl_easy_init();if(!transport)return -1;
    initialized=1;return 0;
}
int network_ready(void){int state=0;return initialized && sceNetApctlGetState(&state)>=0 && state==PSP_NET_APCTL_STATE_GOT_IP;}
#ifdef BILI_DEVICE_LIVE
static void network_report(void) {
    SceNetApctlInfo info;
    int sys_save=-1;
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_WLAN_POWERSAVE,&sys_save);
    if(sceNetApctlGetInfo(PSP_NET_APCTL_INFO_SSID,&info)>=0)
        printf("NET ssid=%.*s",(int)(info.ssidLength<32?info.ssidLength:32),info.ssid);
    else printf("NET ssid=?");
    if(sceNetApctlGetInfo(PSP_NET_APCTL_INFO_STRENGTH,&info)>=0)printf(" strength=%u",info.strength);
    if(sceNetApctlGetInfo(PSP_NET_APCTL_INFO_CHANNEL,&info)>=0)printf(" channel=%u",info.channel);
    if(sceNetApctlGetInfo(PSP_NET_APCTL_INFO_POWER_SAVE,&info)>=0)printf(" ap_power_save=%u",info.powerSave);
    if(sceNetApctlGetInfo(PSP_NET_APCTL_INFO_IP,&info)>=0)printf(" ip=%s",info.ip);
    if(sceNetApctlGetInfo(PSP_NET_APCTL_INFO_GATEWAY,&info)>=0)printf(" gateway=%s",info.gateway);
    printf(" sys_power_save=%d\n",sys_save);
}
#endif
int network_has_profile(void) {
    FILE *f=fopen(CLIENT_PATH("/wifi-profile.txt"),"r");if(!f)return 0;
    int profile=0;int n=fscanf(f,"%d",&profile);fclose(f);
    return n==1 && profile>0 && profile<100 ? profile : 0;
}
void network_remember_profile(void) {
    if(!network_ready())return;
    SceNetApctlInfo current;
    if(sceNetApctlGetInfo(PSP_NET_APCTL_INFO_PROFILE_NAME,&current)<0)return;
    for(int id=1;id<100;++id){
        netData data;memset(&data,0,sizeof(data));
        if(sceUtilityCheckNetParam(id)>=0 && sceUtilityGetNetParam(id,PSP_NETPARAM_NAME,&data)>=0 && !strcmp(data.asString,current.name)){
            FILE *f=fopen(CLIENT_PATH("/wifi-profile.txt"),"w");if(f){fprintf(f,"%d\n",id);fclose(f);}return;
        }
    }
}
int network_connect(void) {
    if(network_ready()){
#ifdef BILI_DEVICE_LIVE
        network_report();
#endif
        return 0;
    }
    network_phase=NET_CONNECTING;
    int profile=network_has_profile();
    if(profile && sceNetApctlConnect(profile)>=0){
        for(int n=0;n<200&&!transfer_cancel;++n){
            if(network_ready()){
#ifdef BILI_DEVICE_LIVE
                network_report();
#endif
                return 0;
            }
            int state=0;
            if(sceNetApctlGetState(&state)>=0){
                if(state==PSP_NET_APCTL_STATE_GETTING_IP)network_phase=NET_GETTING_IP;
                else if(state==PSP_NET_APCTL_STATE_EAP_AUTH || state==PSP_NET_APCTL_STATE_KEY_EXCHANGE)
                    network_phase=NET_AUTHENTICATING;
            }
            sceKernelDelayThread(100000);
        }
        sceNetApctlDisconnect();
        int state=0;sceNetApctlGetState(&state);
        snprintf(network_error,sizeof(network_error),"Wi-Fi connect failed (state %d)",state);
        return -1;
    }
    snprintf(network_error,sizeof(network_error),profile?"Wi-Fi connect request failed":"SELECT: choose a Wi-Fi connection");
    return -1;
}
void network_close(void){if(!initialized)return;curl_easy_cleanup(api_transport);api_transport=NULL;curl_easy_cleanup(transport);transport=NULL;curl_global_cleanup();sceNetApctlDisconnect();sceNetApctlTerm();sceNetResolverTerm();sceNetInetTerm();sceNetTerm();initialized=0;}
typedef struct {
    FILE *file;
    char *text;
    size_t used, limit;
    volatile int *cancel;
    BiliStreamCache *stream;
    BiliHttpResponse *response;
    int flush;
    const char *form;
} Sink;
static int stream_cache_write(BiliStreamCache *stream,const char *data,size_t bytes);
#if BILI_STREAM_PROFILE
static void stream_profile_http(CURL *c,BiliStreamCache *s);
#endif
static size_t write_data(char *data,size_t size,size_t count,void *ctx) {
    Sink *s=ctx;size_t bytes=size*count;
    int cancelled=s->cancel?*s->cancel:transfer_cancel;
    if(cancelled || s->used>s->limit || bytes>s->limit-s->used)return 0;
    if(s->stream){if(stream_cache_write(s->stream,data,bytes)<0)return 0;}
    else if(s->file){if(fwrite(data,1,bytes,s->file)!=bytes)return 0;}
    else {memcpy(s->text+s->used,data,bytes);s->text[s->used+bytes]=0;}
    if(s->flush && fflush(s->file)!=0)return 0;
    s->used+=bytes;transfer_kb=s->used/1024;
    return bytes;
}
static int progress(void *ctx,curl_off_t total,curl_off_t now,curl_off_t up,curl_off_t done){
    Sink *s=ctx;(void)now;(void)up;(void)done;
    transfer_total_kb=total>0?total/1024:0;
    return s&&s->cancel?*s->cancel:transfer_cancel;
}

/* Timings are cumulative from transfer start, in milliseconds. Never log URLs,
 * signed queries, credentials or response bodies. */
static void http_timing(CURL *curl, const char *kind) {
#ifdef BILI_NETWORK_DIAGNOSTICS
    double dns=0,connect=0,tls=0,first=0,total=0;
    long connections=0,status=0;
    curl_easy_getinfo(curl,CURLINFO_NAMELOOKUP_TIME,&dns);
    curl_easy_getinfo(curl,CURLINFO_CONNECT_TIME,&connect);
    curl_easy_getinfo(curl,CURLINFO_APPCONNECT_TIME,&tls);
    curl_easy_getinfo(curl,CURLINFO_STARTTRANSFER_TIME,&first);
    curl_easy_getinfo(curl,CURLINFO_TOTAL_TIME,&total);
    curl_easy_getinfo(curl,CURLINFO_NUM_CONNECTS,&connections);
    curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);
    printf("HTTP timing kind=%s status=%ld new_connections=%ld dns_ms=%.1f connect_ms=%.1f tls_ms=%.1f first_ms=%.1f total_ms=%.1f\n",
           kind,status,connections,dns*1000,connect*1000,tls*1000,first*1000,total*1000);
#else
    (void)curl;(void)kind;
#endif
}

static int request(const char *url,Sink *sink) {
    if (!sink->cancel) network_phase=playback_address_request(url)?NET_PLAYURL:strstr(url,"/x/web-interface/nav")?NET_SIGNING:
        strstr(url,"/wbi/index/top/feed/rcmd")?NET_RECOMMEND:
        strstr(url,"/x/web-interface/popular")?NET_FALLBACK:
        strstr(url,"/x/web-interface/view")?NET_DETAIL:
        strstr(url,"/x/player/wbi/v2") || strstr(url,"/x/player/v2")?NET_PROGRESS:
        strstr(url,"/pgc/view/pc/season") || strstr(url,"/x/player/pagelist")?NET_PARTS:
        strstr(url,"/reply/")?NET_COMMENTS:
        strstr(url,"/x/web-interface/archive/relation") || strstr(url,"/pgc/season/episode/web/info") || strstr(url,"/pgc/view/web/season/user/status")?NET_RELATION:NET_REQUEST;
    CURL *c=sink->response?bili_http_transport_acquire(&api_transport):transport;
    if(!c){if(sink->response)sink->response->failure=BILI_HTTP_MEMORY;return -1;}
    network_ensure_buvid3();
    if(!sink->response)curl_easy_reset(c);
    transfer_kb=transfer_total_kb=0;
    curl_easy_setopt(c,CURLOPT_URL,url);
    if(sink->form){
        curl_easy_setopt(c,CURLOPT_POST,1L);
        curl_easy_setopt(c,CURLOPT_POSTFIELDS,sink->form);
        curl_easy_setopt(c,CURLOPT_POSTFIELDSIZE,(long)strlen(sink->form));
    }
#ifdef BILI_NETWORK_DIAGNOSTICS
    /* Never print login keys, signed queries, Cookie headers or Set-Cookie. */
    printf("HTTP request started\n");
    curl_easy_setopt(c,CURLOPT_VERBOSE,0L);
#endif
    char detail[CURL_ERROR_SIZE]={0};
    curl_easy_setopt(c,CURLOPT_ERRORBUFFER,detail);
    curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(c,CURLOPT_USERAGENT,"wiliwili");
    curl_easy_setopt(c,CURLOPT_REFERER,"https://www.bilibili.com/client");
    struct curl_slist *headers=NULL;
    headers=curl_slist_append(headers,"Origin: https://www.bilibili.com");
    /* Current anonymous comment replies are capped to three entries when a
     * buvid3 cookie is present. Keep Wiliwili's cookie for other APIs, but let
     * the reply endpoint use its full anonymous result set. */
    const int authenticated=session_cookies[0] && bili_http_credential_host(url);
    if(authenticated || !strstr(url,"/x/v2/reply/main")){
        char cookie[4200];
        if(authenticated)snprintf(cookie,sizeof(cookie),"Cookie: buvid3=%s; %s",buvid3,session_cookies);
        else snprintf(cookie,sizeof(cookie),"Cookie: buvid3=%s; DedeUserID=0",buvid3);
        headers=curl_slist_append(headers,cookie);
    }
    if(sink->response && bili_http_login_host(url)){
        curl_easy_setopt(c,CURLOPT_HEADERFUNCTION,bili_http_capture_cookie);
        curl_easy_setopt(c,CURLOPT_HEADERDATA,sink->response);
    }
    if(headers)curl_easy_setopt(c,CURLOPT_HTTPHEADER,headers);
    curl_easy_setopt(c,CURLOPT_CAINFO,CLIENT_PATH("/cacert.pem"));
    curl_easy_setopt(c,CURLOPT_CAPATH,(char *)NULL);
    curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,authenticated?0L:1L);
    if(sink->response && bili_http_login_host(url))curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,0L);
    curl_easy_setopt(c,CURLOPT_MAXREDIRS,5L);
    curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"http,https");
    curl_easy_setopt(c,CURLOPT_REDIR_PROTOCOLS_STR,"http,https");
    curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,12L);
    curl_easy_setopt(c,CURLOPT_LOW_SPEED_LIMIT,512L);
    curl_easy_setopt(c,CURLOPT_LOW_SPEED_TIME,playback_address_request(url)?45L:15L);
    curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,write_data);curl_easy_setopt(c,CURLOPT_WRITEDATA,sink);
    curl_easy_setopt(c,CURLOPT_XFERINFOFUNCTION,progress);curl_easy_setopt(c,CURLOPT_XFERINFODATA,sink);
    curl_easy_setopt(c,CURLOPT_NOPROGRESS,0L);
    curl_easy_setopt(c,CURLOPT_ACCEPT_ENCODING,"");
#if BILI_STREAM_CURL_BUFSIZE
    curl_easy_setopt(c,CURLOPT_BUFFERSIZE,(long)BILI_STREAM_CURL_BUFSIZE);
#endif
#if BILI_STREAM_USE_RANGE
    if(sink->stream){
        curl_easy_setopt(c,CURLOPT_RANGE,"0-");
        curl_easy_setopt(c,CURLOPT_TCP_NODELAY,1L);
    }
#endif
    curl_easy_setopt(c,CURLOPT_TIMEOUT,(sink->file || sink->stream)?0L:playback_address_request(url)?45L:25L);
    CURLcode r=curl_easy_perform(c);
    long status=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);
    const char *kind = !sink->response ? "download" :
        strstr(url,"/x/web-interface/view?") ? "detail" :
        strstr(url,"/playurl?") ? "playurl" :
        strstr(url,"/x/web-interface/nav") ? "nav" : "api";
    http_timing(c,kind);
    if(sink->response){
        sink->response->status=(int)status;
        sink->response->transport_code=(int)r;
        if((sink->cancel&&*sink->cancel) || transfer_cancel)sink->response->failure=BILI_HTTP_CANCELLED;
        else if(r!=CURLE_OK)sink->response->failure=BILI_HTTP_TRANSPORT;
        else if(status!=200)sink->response->failure=BILI_HTTP_STATUS;
    }
#if BILI_STREAM_PROFILE
    if(sink->stream)stream_profile_http(c,sink->stream);
#endif
    if(sink->response)bili_http_transport_release(c);
    if(headers)curl_slist_free_all(headers);
    if(r!=CURLE_OK || status!=200){
        if (sink->cancel) return -1; /* Overlay failures stay local to their request. */
        if((sink->cancel&&*sink->cancel) || transfer_cancel)snprintf(network_error,sizeof(network_error),"Cancelled");
        else snprintf(network_error,sizeof(network_error),"HTTP %ld: %s",status,detail[0]?detail:curl_easy_strerror(r));
        return -1;
    }return 0;
}
static int get_json(const char *url,char **out) {
    char *s=calloc(1,128*1024+1);if(!s)return -1;
    Sink sink={.text=s,.limit=128*1024};int r=request(url,&sink);if(r<0){free(s);return r;}*out=s;return 0;
}
static int get_file(const char *url,const char *path,size_t max,unsigned long long expected) {
    char tmp[256];snprintf(tmp,sizeof(tmp),"%s.part",path);
    FILE *f=fopen(tmp,"wb");if(!f){snprintf(network_error,sizeof(network_error),"Cannot write Memory Stick cache");return -1;}
    Sink sink={.file=f,.limit=max};int r=request(url,&sink);
    if(fclose(f)!=0)r=-1;
    if(expected && sink.used!=expected){snprintf(network_error,sizeof(network_error),"Incomplete MP4 download");r=-1;}
    if(r==0 && rename(tmp,path)!=0)r=-1;
    if(r<0)remove(tmp);
    return r;
}
int bili_http_get_response_cancel(const char *url,BiliHttpResponse *out,size_t max_bytes,volatile int *cancel) {
    if(!out)return -1;
    memset(out,0,sizeof(*out));
    if(!url || max_bytes<2 || max_bytes>1024*1024){out->failure=BILI_HTTP_MEMORY;return -1;}
    if(cancel && (*cancel || !network_ready())) {out->failure=*cancel?BILI_HTTP_CANCELLED:BILI_HTTP_DISCONNECTED;return -1;}
    if(!cancel && network_connect()<0){out->failure=transfer_cancel?BILI_HTTP_CANCELLED:BILI_HTTP_DISCONNECTED;return -1;}
    char *text=calloc(1,max_bytes+1);
    if(!text){out->failure=BILI_HTTP_MEMORY;return -1;}
    Sink sink={.text=text,.limit=max_bytes,.response=out,.cancel=cancel};
    int result=request(url,&sink);
    if(result<0){free(text);return -1;}
    out->text=text;
    return 0;
}
int bili_http_get_response(const char *url,BiliHttpResponse *out,size_t max_bytes) {
    return bili_http_get_response_cancel(url,out,max_bytes,NULL);
}
int bili_http_post_response(const char *url,const char *form,BiliHttpResponse *out,size_t max_bytes) {
    if(!out)return -1;
    memset(out,0,sizeof(*out));
    if(!url || !form || !bili_http_credential_host(url) || max_bytes<2 || max_bytes>1024*1024){
        out->failure=BILI_HTTP_STATUS;return -1;
    }
    if(network_connect()<0){out->failure=transfer_cancel?BILI_HTTP_CANCELLED:BILI_HTTP_DISCONNECTED;return -1;}
    char *text=calloc(1,max_bytes+1);
    if(!text){out->failure=BILI_HTTP_MEMORY;return -1;}
    Sink sink={.text=text,.limit=max_bytes,.response=out,.form=form};
    int result=request(url,&sink);
    if(result<0){free(text);return -1;}
    out->text=text;return 0;
}
int bili_http_get_text(const char *url,char **out,size_t max_bytes) {
    if(!url || !out || max_bytes<2)return -1;
    *out=NULL;
    if(network_connect()<0)return -1;
    char *text=calloc(1,max_bytes+1);
    if(!text){snprintf(network_error,sizeof(network_error),"Cannot allocate HTTP response");return -1;}
    Sink sink={.text=text,.limit=(unsigned)max_bytes};
    int result=request(url,&sink);
    if(result<0){free(text);return result;}
    *out=text;
    return 0;
}
int bili_http_download(const char *url,const char *path,size_t max_bytes) {
    if(!url || !path || !max_bytes)return -1;
    if(network_connect()<0)return -1;
    return get_file(url,path,max_bytes,0);
}
void bili_http_free(void *memory) { free(memory); }
int bili_home(int page,BiliHome *out) {
    char url[256],*json=0;
    snprintf(url,sizeof(url),"https://api.bilibili.com/x/web-interface/popular?ps=6&pn=%d",page);
    if(network_connect()<0)return -1;
#ifdef BILI_NETWORK_DIAGNOSTICS
    static int probed;
    if(!probed){
        probed=1;
        const char *sites[]={"http://example.com/","https://example.com/","https://www.baidu.com/"};
        for(unsigned i=0;i<3&&!transfer_cancel;++i){
            char *body=calloc(1,128*1024+1);
            if(!body)break;
            Sink sink={.text=body,.limit=128*1024};
            int result=request(sites[i],&sink);
            printf("SITE TEST %s result=%d bytes=%u error=%s\n",sites[i],result,(unsigned)sink.used,result<0?network_error:"");
            free(body);
        }
    }
#endif
    if(get_json(url,&json)<0)return -1;
    int r=bili_parse_home(json,out);
    if(r==0){
        out->page=page;
        for(int i=0;i<out->count&&!transfer_cancel;++i){
            char path[192];snprintf(path,sizeof(path),CLIENT_PATH("/cache/%s.jpg"),out->videos[i].bvid);
            snprintf(url,sizeof(url),"%s@288w_162h_1c.jpg",out->videos[i].picture);
            get_file(url,path,256*1024,0);
        }
        if(transfer_cancel){free(json);return -1;}
        FILE *f=fopen(CLIENT_PATH("/cache/home.json.part"),"wb");
        if(f){size_t len=strlen(json);int ok=fwrite(json,1,len,f)==len;if(fclose(f))ok=0;if(ok)rename(CLIENT_PATH("/cache/home.json.part"),CLIENT_PATH("/cache/home.json"));}
    }free(json);return r;
}
/* Small videos stay in a RAM window. Large videos are spooled to a temporary
 * file because a rolling window cannot satisfy every MP4 demuxer seek. */
#ifndef STREAM_RAM_CACHE_BYTES
#define STREAM_RAM_CACHE_BYTES (512u*1024)
#endif
#define STREAM_READER_SLOTS 4
typedef struct {
    SceUID tid;
    unsigned position;
#if BILI_STREAM_PROFILE
    uint64_t wait_us, lock_us, copy_us, max_wait_us;
    unsigned calls, bytes, waits;
#endif
} StreamReader;
struct BiliStreamCache {
    int segment_count;
    struct {
        char url[4096];
        unsigned long long size;
    } segments[BILI_STREAM_SEGMENTS];
    unsigned expected, capacity;
    unsigned char *ring;
    char path[256], part[264];
    SceUID fd;
    int disk;
    volatile unsigned available, base;
    volatile int cancelled, complete, failed, reads_interrupted;
    char error[160];
    SceUID io_lock, wake, thread;
    StreamReader readers[STREAM_READER_SLOTS];
    PspVideoSource source;
#if BILI_STREAM_PROFILE
    struct {
        uint64_t started_us, first_data_us, last_arrival_us, last_data_us, finished_us;
        uint64_t callback_us, backpressure_us, lock_us, copy_us;
        uint64_t min_us, wake_us, unlock_us;
        uint64_t max_gap_us, stall_us;
        uint64_t connect_us, first_byte_us, http_total_us, http_speed_bps;
        unsigned writes, bytes, max_chunk, stalls, waits, wake_errors;
    } prof;
#endif
};
static BiliStreamCache active_stream;

#ifndef BILI_RANGE_CACHE_BYTES
#define BILI_RANGE_CACHE_BYTES (512u * 1024u)
#endif
#ifndef BILI_RANGE_BACK_BYTES
#define BILI_RANGE_BACK_BYTES (64u * 1024u)
#endif
#define BILI_RANGE_THREAD_STACK (128u * 1024u)

struct BiliRangeStream {
    char label[12];
    char urls[BILI_MEDIA_URLS][4096];
    int url_count;
    int next_url;
    int64_t size;
    unsigned expected;
    unsigned char *ring;
    unsigned capacity;
    volatile unsigned base, available;
    unsigned restart_offset;
    unsigned generation;
    volatile int cancelled, failed, complete, restart, reads_interrupted;
    char error[160];
    SceUID lock, wake, space, sized, thread;
    PspVideoSource source;
};

typedef struct {
    BiliRangeStream *stream;
    unsigned generation, requested_start, response_start, total;
    int content_range;
    char range[64];
} RangeTransfer;

typedef struct {
    unsigned long long start, end, total, content_length;
    int content_range, has_length, received;
} RangeProbe;

static int range_header_prefix(const char *line, size_t length, const char *name) {
    const size_t name_length = strlen(name);
    if (length < name_length + 1) return 0;
    for (size_t i = 0; i < name_length; ++i) {
        char a = line[i], b = name[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return line[name_length] == ':';
}

static const char *range_header_value(const char *line, size_t length, const char *name, size_t *value_length) {
    if (!range_header_prefix(line, length, name)) return NULL;
    size_t offset = strlen(name) + 1;
    while (offset < length && (line[offset] == ' ' || line[offset] == '\t')) ++offset;
    size_t end = length;
    while (end > offset && (line[end - 1] == '\r' || line[end - 1] == '\n' || line[end - 1] == ' ')) --end;
    *value_length = end - offset;
    return line + offset;
}

static void range_parse_headers(const char *line, size_t length, RangeTransfer *transfer, RangeProbe *probe) {
    size_t value_length = 0;
    const char *value = range_header_value(line, length, "content-range", &value_length);
    if (value) {
        char text[160];
        if (value_length >= sizeof(text)) value_length = sizeof(text) - 1;
        memcpy(text, value, value_length);
        text[value_length] = 0;
        unsigned long long start = 0, end = 0, total = 0;
        if (sscanf(text, "bytes %llu-%llu/%llu", &start, &end, &total) == 3) {
            if (transfer) {
                transfer->response_start = (unsigned)start;
                transfer->total = (unsigned)total;
                transfer->content_range = 1;
            }
            if (probe) {
                probe->start = start;
                probe->end = end;
                probe->total = total;
                probe->content_range = 1;
            }
        }
    }
    value = range_header_value(line, length, "content-length", &value_length);
    if (value && probe) {
        char text[64];
        if (value_length >= sizeof(text)) value_length = sizeof(text) - 1;
        memcpy(text, value, value_length);
        text[value_length] = 0;
        unsigned long long bytes = 0;
        if (sscanf(text, "%llu", &bytes) == 1) {
            probe->content_length = bytes;
            probe->has_length = 1;
        }
    }
}

static size_t range_write_data(char *data, size_t size, size_t count, void *opaque) {
    RangeTransfer *transfer = opaque;
    BiliRangeStream *stream = transfer->stream;
    if ((transfer->content_range && transfer->response_start != transfer->requested_start) ||
        (!transfer->content_range && transfer->requested_start != 0))
        return 0;
    size_t bytes = size * count;
    size_t written = 0;
    while (written < bytes) {
        if (transfer_cancel) return 0;
        sceKernelWaitSema(stream->lock, 1, NULL);
        if (transfer_cancel || stream->cancelled || stream->failed ||
            transfer->generation != stream->generation) {
            sceKernelSignalSema(stream->lock, 1);
            return 0;
        }
        unsigned used = stream->available - stream->base;
        unsigned room = used < stream->capacity ? stream->capacity - used : 0;
        if (room) {
            size_t chunk = bytes - written;
            if (chunk > room) chunk = room;
            if (chunk > stream->expected - stream->available)
                chunk = stream->expected - stream->available;
            if (!chunk) {
                sceKernelSignalSema(stream->lock, 1);
                return bytes;
            }
            unsigned offset = stream->available % stream->capacity;
            size_t first = stream->capacity - offset;
            if (first > chunk) first = chunk;
            memcpy(stream->ring + offset, data + written, first);
            if (first < chunk) memcpy(stream->ring, data + written + first, chunk - first);
            __asm__ volatile("" ::: "memory");
            stream->available += (unsigned)chunk;
            written += chunk;
            sceKernelSignalSema(stream->wake, 1);
        }
        sceKernelSignalSema(stream->lock, 1);
        if (written < bytes) {
            SceUInt timeout = 100000;
            while (sceKernelWaitSema(stream->space, 1, &timeout) < 0) {
                if (transfer_cancel || stream->cancelled || stream->failed) return 0;
                timeout = 100000;
            }
        }
    }
    return bytes;
}

static size_t range_transfer_header(char *data, size_t size, size_t count, void *opaque) {
    size_t length = size * count;
    RangeTransfer *transfer = opaque;
    range_parse_headers(data, length, transfer, NULL);
    if (transfer->content_range && transfer->total > 0 && transfer->stream->size == 0) {
        transfer->stream->size = (int64_t)transfer->total;
        transfer->stream->expected = transfer->total;
        sceKernelSignalSema(transfer->stream->sized, 1);
    }
    return length;
}

static int range_progress(void *opaque, curl_off_t total, curl_off_t now, curl_off_t up, curl_off_t done) {
    (void)total; (void)now; (void)up; (void)done;
    BiliRangeStream *stream = opaque;
    return (transfer_cancel || stream->cancelled || stream->failed || stream->restart) ? 1 : 0;
}

static struct curl_slist *range_headers(void) {
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Origin: https://www.bilibili.com");
    headers = curl_slist_append(headers, "Accept-Encoding: identity");
    return headers;
}

static void range_configure(CURL *curl, const char *url, const char *range,
                            size_t (*write_cb)(char *, size_t, size_t, void *),
                            size_t (*header_cb)(char *, size_t, size_t, void *),
                            void *write_data, void *header_data,
                            char *error, struct curl_slist **headers) {
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_RANGE, range);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "wiliwili");
    curl_easy_setopt(curl, CURLOPT_REFERER, "https://www.bilibili.com");
    curl_easy_setopt(curl, CURLOPT_CAINFO, CLIENT_PATH("/cacert.pem"));
    curl_easy_setopt(curl, CURLOPT_CAPATH, (char *)NULL);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 12L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 512L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, write_data);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, header_data);
    *headers = range_headers();
    if (*headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, *headers);
}

static int range_download_thread(SceSize args, void *argp) {
    (void)args;
    BiliRangeStream *stream = argp ? *(BiliRangeStream **)argp : NULL;
    if (!stream) return -1;
    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(stream->error, sizeof(stream->error), "%s: cannot create HTTP handle", stream->label);
        stream->failed = 1;
        sceKernelSignalSema(stream->wake, 1);
        sceKernelSignalSema(stream->sized, 1);
        return -1;
    }
    for (;;) {
        sceKernelWaitSema(stream->lock, 1, NULL);
        if (transfer_cancel) stream->cancelled = 1;
        if (stream->cancelled || stream->failed) {
            sceKernelSignalSema(stream->lock, 1);
            sceKernelSignalSema(stream->wake, 1);
            sceKernelSignalSema(stream->sized, 1);
            break;
        }
        if (stream->complete) {
            // Keep the HTTP owner alive so a later backward seek can refill
            // bytes that have already fallen out of the bounded ring.
            sceKernelSignalSema(stream->lock, 1);
            SceUInt idle = 100000;
            sceKernelWaitSemaCB(stream->space, 1, &idle);
            continue;
        }
        unsigned generation = stream->generation;
        unsigned start = stream->available;
        int url_index = stream->next_url;
        stream->restart = 0;
        sceKernelSignalSema(stream->lock, 1);

        int moved = 0;
        int successful = 0;
        for (int attempt = 0; attempt < stream->url_count && !moved; ++attempt) {
            const char *url = stream->urls[url_index];
            if (!url[0]) break;
            RangeTransfer transfer = {.stream = stream, .generation = generation,
                                      .requested_start = start};
            snprintf(transfer.range, sizeof(transfer.range), "%u-", start);
            char curl_error[CURL_ERROR_SIZE] = {0};
            struct curl_slist *headers = NULL;
            range_configure(curl, url, transfer.range, range_write_data, range_transfer_header,
                            &transfer, &transfer, curl_error, &headers);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, range_progress);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, stream);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            CURLcode code = curl_easy_perform(curl);
            http_timing(curl, stream->label);
            long status = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
            if (headers) curl_slist_free_all(headers);

            sceKernelWaitSema(stream->lock, 1, NULL);
            moved = stream->generation != generation;
            const int range_ok = status == 206 ? transfer.content_range &&
                                                   transfer.response_start == start
                                 : status == 200 ? start == 0 && !transfer.content_range
                                                 : status == 416;
            successful = !transfer_cancel && !moved && range_ok &&
                         (code == CURLE_OK || status == 416);
            if (!successful && !moved && !transfer_cancel && !stream->cancelled &&
                code != CURLE_WRITE_ERROR) {
                if (status) snprintf(stream->error, sizeof(stream->error), "%s HTTP %ld", stream->label, status);
                else snprintf(stream->error, sizeof(stream->error), "%s: %s", stream->label,
                              curl_error[0] ? curl_error : curl_easy_strerror(code));
            }
            sceKernelSignalSema(stream->lock, 1);
            if (!successful && !moved) {
                url_index = (url_index + 1) % stream->url_count;
                sceKernelDelayThreadCB(200000);
            }
        }
        sceKernelWaitSema(stream->lock, 1, NULL);
        if (stream->generation != generation) {
            sceKernelSignalSema(stream->lock, 1);
            continue;
        }
        if (transfer_cancel) stream->cancelled = 1;
        if (stream->cancelled) {
            sceKernelSignalSema(stream->lock, 1);
            break;
        }
        if (successful) stream->complete = 1;
        else stream->failed = 1;
        stream->next_url = url_index;
        sceKernelSignalSema(stream->wake, 1);
        sceKernelSignalSema(stream->sized, 1);
        sceKernelSignalSema(stream->lock, 1);
        if (!successful) break;
    }
    curl_easy_cleanup(curl);
    return 0;
}

static int range_read_at(void *opaque, int64_t offset, void *buffer, int bytes) {
    BiliRangeStream *stream = opaque;
    if (!stream || stream->failed || offset < 0 || bytes < 0 || offset > stream->size) return -1;
    if ((int64_t)bytes > stream->size - offset) bytes = (int)(stream->size - offset);
    if (!bytes) return 0;
    unsigned start = (unsigned)offset;
    unsigned target = start + (unsigned)bytes;
    unsigned last_available = stream->available;
    uint64_t last_change = sceKernelGetSystemTimeWide();
    for (;;) {
        sceKernelWaitSema(stream->lock, 1, NULL);
        if (stream->cancelled || stream->failed || stream->reads_interrupted) {
            sceKernelSignalSema(stream->lock, 1);
            return -1;
        }
        // A forward read must fit in the retained window. Otherwise the
        // producer fills the ring before reaching target and both sides wait.
        if (start < stream->base || target - stream->base > stream->capacity) {
            ++stream->generation;
            stream->restart_offset = start;
            stream->restart = 1;
            stream->complete = 0;
            stream->base = stream->available = start;
            sceKernelSignalSema(stream->space, 1);
            sceKernelSignalSema(stream->wake, 1);
            sceKernelSignalSema(stream->lock, 1);
            continue;
        }
        if (stream->available >= target && start >= stream->base) {
            unsigned position = start;
            size_t copied = 0;
            while (position < target) {
                unsigned ring_offset = position % stream->capacity;
                size_t chunk = stream->capacity - ring_offset;
                if (chunk > (size_t)(target - position)) chunk = (size_t)(target - position);
                memcpy((unsigned char *)buffer + copied, stream->ring + ring_offset, chunk);
                copied += chunk;
                position += (unsigned)chunk;
            }
            unsigned retained = target > BILI_RANGE_BACK_BYTES ? target - BILI_RANGE_BACK_BYTES : 0;
            if (retained > stream->base) stream->base = retained;
            sceKernelSignalSema(stream->space, 1);
            sceKernelSignalSema(stream->lock, 1);
            return bytes;
        }
        unsigned available = stream->available;
        sceKernelSignalSema(stream->lock, 1);
        uint64_t now = sceKernelGetSystemTimeWide();
        if (available != last_available) {
            last_available = available;
            last_change = now;
        } else if (now - last_change >= 30000000u) {
            snprintf(stream->error, sizeof(stream->error), "%s buffering timed out", stream->label);
            stream->failed = 1;
            sceKernelSignalSema(stream->wake, 1);
            return -1;
        }
        SceUInt timeout = 100000;
        sceKernelWaitSemaCB(stream->wake, 1, &timeout);
    }
}

static void range_interrupt_reads(void *opaque, int interrupted) {
    BiliRangeStream *stream = opaque;
    stream->reads_interrupted = interrupted;
    sceKernelSignalSema(stream->wake, 1);
}
static void range_cancel(void *opaque) {
    BiliRangeStream *stream = opaque;
    if (!stream) return;
    stream->cancelled = 1;
    sceKernelSignalSema(stream->space, 1);
    sceKernelSignalSema(stream->wake, 1);
    sceKernelSignalSema(stream->sized, 1);
}

static int range_begin(const BiliMediaUrl *media, const char *label, int64_t *size,
                       BiliRangeStream **out, char *error, size_t error_size) {
    if (!media || !media->url || !out) return -1;
    if (transfer_cancel) {
        snprintf(error, error_size, "Cancelled");
        return -1;
    }
    BiliRangeStream *stream = calloc(1, sizeof(*stream));
    if (!stream) {
        snprintf(error, error_size, "Cannot allocate media stream");
        return -1;
    }
    snprintf(stream->label, sizeof(stream->label), "%s", label ? label : "media");
    stream->lock = stream->wake = stream->space = stream->sized = stream->thread = -1;
    stream->capacity = BILI_RANGE_CACHE_BYTES;
    stream->ring = malloc(stream->capacity);
    if (!stream->ring) {
        snprintf(error, error_size, "Cannot allocate media buffer");
        goto fail;
    }
    stream->url_count = 1 + (media->backup_count > 0 ? media->backup_count : 0);
    if (stream->url_count > BILI_MEDIA_URLS) stream->url_count = BILI_MEDIA_URLS;
    snprintf(stream->urls[0], sizeof(stream->urls[0]), "%s", media->url);
    for (int i = 1; i < stream->url_count; ++i)
        snprintf(stream->urls[i], sizeof(stream->urls[i]), "%s", media->backups[i - 1]);
    stream->lock = sceKernelCreateSema("bili_range_lock", 0, 1, 1, NULL);
    stream->wake = sceKernelCreateSema("bili_range_data", 0, 0, 1, NULL);
    stream->space = sceKernelCreateSema("bili_range_space", 0, 1, 1, NULL);
    stream->sized = sceKernelCreateSema("bili_range_size", 0, 0, 1, NULL);
    if (stream->lock < 0 || stream->wake < 0 || stream->space < 0 || stream->sized < 0) {
        snprintf(error, error_size, "Cannot create media synchronization");
        goto fail;
    }
    stream->thread = sceKernelCreateThread("bili_range", range_download_thread, 0x28,
                                            BILI_RANGE_THREAD_STACK, PSP_THREAD_ATTR_USER, NULL);
    if (stream->thread < 0) {
        snprintf(error, error_size, "Cannot create media thread");
        goto fail;
    }
    if (sceKernelStartThread(stream->thread, sizeof(stream), &stream) < 0) {
        sceKernelDeleteThread(stream->thread);
        stream->thread = -1;
        snprintf(error, error_size, "Cannot start media thread");
        goto fail;
    }
    const int64_t deadline = (int64_t)sceKernelGetSystemTimeWide() + 30000000;
    int sized = 0;
    while (!transfer_cancel && sceKernelGetSystemTimeWide() < deadline) {
        SceUInt timeout = 100000;
        if (sceKernelWaitSema(stream->sized, 1, &timeout) >= 0) {
            sized = 1;
            break;
        }
        if (stream->failed) break;
    }
    if (!sized || transfer_cancel || stream->size <= 0 ||
        (unsigned long long)stream->size > STREAM_MAX_BYTES || stream->expected == 0) {
        if (transfer_cancel) stream->cancelled = 1;
        snprintf(error, error_size, "%s", transfer_cancel ? "Cancelled" :
                 stream->error[0] ? stream->error : "Media size timeout");
        goto fail;
    }
    stream->source = (PspVideoSource){stream, stream->size, range_read_at, range_cancel, NULL, range_interrupt_reads};
    *size = stream->size;
    *out = stream;
    return 0;
fail:
    if (stream->thread >= 0) {
        stream->cancelled = 1;
        sceKernelSignalSema(stream->space, 1);
        sceKernelWaitThreadEndCB(stream->thread, NULL);
        sceKernelDeleteThread(stream->thread);
    }
    if (stream->lock >= 0) sceKernelDeleteSema(stream->lock);
    if (stream->wake >= 0) sceKernelDeleteSema(stream->wake);
    if (stream->space >= 0) sceKernelDeleteSema(stream->space);
    if (stream->sized >= 0) sceKernelDeleteSema(stream->sized);
    free(stream->ring);
    free(stream);
    return -1;
}

static void range_finish(BiliRangeStream *stream, int *failed) {
    if (!stream) return;
    stream->cancelled = 1;
    if (stream->space >= 0) sceKernelSignalSema(stream->space, 1);
    if (stream->wake >= 0) sceKernelSignalSema(stream->wake, 1);
    if (stream->thread >= 0) {
        sceKernelWaitThreadEndCB(stream->thread, NULL);
        sceKernelDeleteThread(stream->thread);
    }
    if (stream->failed && stream->error[0]) {
        snprintf(network_error, sizeof(network_error), "%s", stream->error);
        *failed = 1;
    }
    if (stream->lock >= 0) sceKernelDeleteSema(stream->lock);
    if (stream->wake >= 0) sceKernelDeleteSema(stream->wake);
    if (stream->space >= 0) sceKernelDeleteSema(stream->space);
    if (stream->sized >= 0) sceKernelDeleteSema(stream->sized);
    free(stream->ring);
    free(stream);
}

#if BILI_STREAM_PROFILE
static void stream_profile_http(CURL *c,BiliStreamCache *s) {
    double value=0;curl_off_t speed=0;
    if(curl_easy_getinfo(c,CURLINFO_CONNECT_TIME,&value)==CURLE_OK)
        s->prof.connect_us=(uint64_t)(value*1000000.0);
    if(curl_easy_getinfo(c,CURLINFO_STARTTRANSFER_TIME,&value)==CURLE_OK)
        s->prof.first_byte_us=(uint64_t)(value*1000000.0);
    if(curl_easy_getinfo(c,CURLINFO_TOTAL_TIME,&value)==CURLE_OK)
        s->prof.http_total_us=(uint64_t)(value*1000000.0);
    if(curl_easy_getinfo(c,CURLINFO_SPEED_DOWNLOAD_T,&speed)==CURLE_OK)
        s->prof.http_speed_bps=(uint64_t)speed;
}
#endif

static unsigned stream_min_reader_locked(BiliStreamCache *s) {
    unsigned min=0;int found=0,count=0;
    for(int i=0;i<STREAM_READER_SLOTS;++i)if(s->readers[i].tid){
        if(!found || s->readers[i].position<min)min=s->readers[i].position;
        found=1;++count;
    }
    /* Video and audio open the source independently and both start at byte 0.
     * Do not retire the beginning until both readers have registered. */
    (void)count;
    return found ? min : 0;
}

static StreamReader *stream_reader_locked(BiliStreamCache *s,SceUID tid,int create) {
    int free_slot=-1;
    for(int i=0;i<STREAM_READER_SLOTS;++i){
        if(s->readers[i].tid==tid)return &s->readers[i];
        if(!s->readers[i].tid && free_slot<0)free_slot=i;
    }
    if(create && free_slot>=0){
        s->readers[free_slot].tid=tid;
        s->readers[free_slot].position=0;
        return &s->readers[free_slot];
    }
    return NULL;
}

static void stream_touch_reader_locked(BiliStreamCache *s,unsigned position) {
    StreamReader *r=stream_reader_locked(s,sceKernelGetThreadId(),0);
    if(r)r->position=position;
}

static int stream_cache_write(BiliStreamCache *s,const char *data,size_t bytes) {
    if(!s || !bytes)return -1;
    if(s->disk){
        if(sceKernelWaitSema(s->io_lock,1,NULL)<0)return -1;
        int ok=0;
        if(!s->cancelled && !s->failed){
            const SceOff position=sceIoLseek(s->fd,(SceOff)s->available,SEEK_SET);
            const int written=sceIoWrite(s->fd,data,(SceSize)bytes);
            if(position==(SceOff)s->available && written==(int)bytes){
                __asm__ volatile("" ::: "memory");
                s->available+=(unsigned)bytes;
                ok=1;
            }
        }
        sceKernelSignalSema(s->io_lock,1);
        if(ok) sceKernelSignalSema(s->wake,1);
        return ok?(int)bytes:-1;
    }
    if(!s->ring || bytes>s->capacity)return -1;
#if BILI_STREAM_PROFILE
    uint64_t call_begin=sceKernelGetSystemTimeWide();
    uint64_t wait_total=0,lock_total=0,copy_total=0,min_total=0,wake_total=0,unlock_total=0,wait_begin=0;
    unsigned waits=0;int waiting=0;
    uint64_t arrival=call_begin;
    if(s->prof.last_arrival_us){
        uint64_t gap=arrival-s->prof.last_arrival_us;
        if(gap>s->prof.max_gap_us)s->prof.max_gap_us=gap;
        if(gap>100000u){++s->prof.stalls;s->prof.stall_us+=gap;}
    }
    if(!s->prof.first_data_us)s->prof.first_data_us=arrival;
    s->prof.last_arrival_us=arrival;
    if(bytes>s->prof.max_chunk)s->prof.max_chunk=(unsigned)bytes;
#endif
    for(;;){
        if(s->cancelled || s->failed)return -1;
#if BILI_STREAM_PROFILE
        uint64_t lock_begin=sceKernelGetSystemTimeWide();
#endif
        if(sceKernelWaitSema(s->io_lock,1,NULL)<0)return -1;
#if BILI_STREAM_PROFILE
        lock_total+=sceKernelGetSystemTimeWide()-lock_begin;
        if(waiting){wait_total+=sceKernelGetSystemTimeWide()-wait_begin;waiting=0;}
#endif
        unsigned used=s->available;
        unsigned end=used+(unsigned)bytes;
        unsigned overwrite=end>s->capacity?end-s->capacity:0;
#if BILI_STREAM_PROFILE
        uint64_t min_begin=sceKernelGetSystemTimeWide();
#endif
        unsigned min_reader=stream_min_reader_locked(s);
#if BILI_STREAM_PROFILE
        min_total+=sceKernelGetSystemTimeWide()-min_begin;
#endif
        if(!overwrite || min_reader>=overwrite){
            unsigned start=used%s->capacity;
            size_t first=s->capacity-start;
            if(first>bytes)first=bytes;
#if BILI_STREAM_PROFILE
            uint64_t copy_begin=sceKernelGetSystemTimeWide();
#endif
            memcpy(s->ring+start,data,first);
            if(first<bytes)memcpy(s->ring,data+first,bytes-first);
#if BILI_STREAM_PROFILE
            copy_total+=sceKernelGetSystemTimeWide()-copy_begin;
#endif
            __asm__ volatile("" ::: "memory");
            s->available=end;
            if(end>s->capacity)s->base=end-s->capacity;
#if BILI_STREAM_PROFILE
            uint64_t signal_begin=sceKernelGetSystemTimeWide();
#endif
            int wake_ret=sceKernelSignalSema(s->wake,1);
#if BILI_STREAM_PROFILE
            wake_total+=sceKernelGetSystemTimeWide()-signal_begin;
            uint64_t unlock_begin=sceKernelGetSystemTimeWide();
#endif
            sceKernelSignalSema(s->io_lock,1);
#if BILI_STREAM_PROFILE
            unlock_total+=sceKernelGetSystemTimeWide()-unlock_begin;
            {
                uint64_t now=sceKernelGetSystemTimeWide();
                s->prof.callback_us+=now-call_begin-wait_total;
                s->prof.backpressure_us+=wait_total;
                s->prof.lock_us+=lock_total;
                s->prof.copy_us+=copy_total;
                s->prof.min_us+=min_total;
                s->prof.wake_us+=wake_total;
                s->prof.unlock_us+=unlock_total;
                s->prof.waits+=waits;
                if(wake_ret<0)++s->prof.wake_errors;
                s->prof.last_data_us=now;
                ++s->prof.writes;
                s->prof.bytes+=(unsigned)bytes;
            }
#else
            (void)wake_ret;
#endif
            return (int)bytes;
        }
        sceKernelSignalSema(s->io_lock,1);
#if BILI_STREAM_PROFILE
        wait_begin=sceKernelGetSystemTimeWide();
        waiting=1;++waits;
#endif
        sceKernelDelayThread(2000);
    }
}

static int stream_read_at(void *opaque,int64_t offset,void *buffer,int bytes) {
    BiliStreamCache *s=opaque;
    if(!s || s->failed || s->reads_interrupted || offset<0 || bytes<0 || (uint64_t)offset>s->expected)return -1;
    if((uint64_t)bytes>s->expected-(uint64_t)offset)bytes=(int)(s->expected-(uint64_t)offset);
    if(!bytes)return 0;
    unsigned target=(unsigned)((uint64_t)offset+(unsigned)bytes);
    uint64_t wait_begin=sceKernelGetSystemTimeWide();
    unsigned waited=0;
    StreamReader *reader=NULL;
    if(sceKernelWaitSemaCB(s->io_lock,1,NULL)>=0){
        reader=stream_reader_locked(s,sceKernelGetThreadId(),1);
        sceKernelSignalSema(s->io_lock,1);
    }
#if BILI_STREAM_PROFILE
    if(reader)++reader->calls;
#else
    (void)wait_begin;
    (void)waited;
    (void)reader;
#endif
    unsigned last_available=s->available,last_change=sceKernelGetSystemTimeLow();
    while(s->available<target){
        if(s->cancelled || s->failed || s->complete || s->reads_interrupted)return -1;
        SceUInt timeout=100000;
        sceKernelWaitSemaCB(s->wake,1,&timeout);
#if BILI_STREAM_PROFILE
        waited=1;
#endif
        unsigned now=sceKernelGetSystemTimeLow();
        if(s->available!=last_available){last_available=s->available;last_change=now;}
        else if((unsigned)(now-last_change)>=30000000u){
            snprintf(s->error,sizeof(s->error),"Online video buffering timed out");
            s->failed=s->cancelled=1;
            return -1;
        }
    }
#if BILI_STREAM_PROFILE
    if(reader){
        uint64_t wait_us=sceKernelGetSystemTimeWide()-wait_begin;
        reader->wait_us+=wait_us;
        if(wait_us>reader->max_wait_us)reader->max_wait_us=wait_us;
        if(waited)++reader->waits;
    }
#endif
    int result=-1;
#if BILI_STREAM_PROFILE
    uint64_t lock_begin=sceKernelGetSystemTimeWide();
#endif
    if(sceKernelWaitSemaCB(s->io_lock,1,NULL)>=0){
#if BILI_STREAM_PROFILE
        if(reader)reader->lock_us+=sceKernelGetSystemTimeWide()-lock_begin;
#endif
        unsigned start=(unsigned)offset;
        if((s->disk || start>=s->base) && s->available>=target){
            if(s->disk){
                int total=0;
                for(int attempt=0;attempt<2 && total<bytes;++attempt){
                    if(attempt)sceIoSync(client_storage_device,0);
                    const SceOff position=sceIoLseek(s->fd,(SceOff)(start+(unsigned)total),SEEK_SET);
                    if(position!=(SceOff)(start+(unsigned)total))break;
                    while(total<bytes){
                        const int read=sceIoRead(s->fd,(unsigned char *)buffer+total,(SceSize)(bytes-total));
                        if(read<=0)break;
                        total+=read;
                    }
                }
                if(total==bytes){
                    stream_touch_reader_locked(s,target);
                    result=bytes;
                }else{
                    snprintf(s->error,sizeof(s->error),"Cache read failed at %u (%d/%d, available %u)",
                             start,total,bytes,s->available);
                    s->failed=1;
                }
            }else{
                unsigned ring_start=start%s->capacity;
                size_t first=s->capacity-ring_start;
                if(first>(size_t)bytes)first=(size_t)bytes;
#if BILI_STREAM_PROFILE
                uint64_t copy_begin=sceKernelGetSystemTimeWide();
#endif
                memcpy(buffer,s->ring+ring_start,first);
                if(first<(size_t)bytes)memcpy((unsigned char *)buffer+first,s->ring,(size_t)bytes-first);
#if BILI_STREAM_PROFILE
                if(reader){reader->copy_us+=sceKernelGetSystemTimeWide()-copy_begin;reader->bytes+=(unsigned)bytes;}
#endif
                stream_touch_reader_locked(s,target);
                result=bytes;
            }
        }
        sceKernelSignalSema(s->io_lock,1);
    }
    return result;
}

static void stream_interrupt_reads(void *opaque, int interrupted) {
    BiliStreamCache *s = opaque;
    s->reads_interrupted = interrupted;
    if (!interrupted) {
        /* Audio reader thread IDs change after a seek. No old reader is live. */
        sceKernelWaitSema(s->io_lock, 1, NULL);
        memset(s->readers, 0, sizeof(s->readers));
        sceKernelSignalSema(s->io_lock, 1);
    }
    sceKernelSignalSema(s->wake, 1);
}
static void stream_cancel(void *opaque) {
    BiliStreamCache *s=opaque;
    if(!s)return;
    s->cancelled=1;
    if(s->wake>=0)sceKernelSignalSema(s->wake,1);
}

static int stream_download_thread(SceSize size,void *arg) {
    (void)size;
    BiliStreamCache *s=arg ? *(BiliStreamCache **)arg : NULL;
    if(!s)return -1;
    int r=0;
    for(int i=0;i<s->segment_count && r==0 && !s->cancelled;++i){
        Sink sink={.limit=(size_t)s->segments[i].size,.cancel=&s->cancelled,.stream=s};
        r=request(s->segments[i].url,&sink);
        if(r==0 && (unsigned long long)sink.used!=s->segments[i].size){
            snprintf(network_error,sizeof(network_error),"Incomplete online stream");
            r=-1;
        }
    }
    if(!s->cancelled){
        if(r<0){
            s->failed=1;
            if(!network_error[0])snprintf(network_error,sizeof(network_error),"Incomplete online stream");
            snprintf(s->error,sizeof(s->error),"%s",network_error);
        }else{
            __asm__ volatile("" ::: "memory");
            s->complete=1;
        }
    }
#if BILI_STREAM_PROFILE
    s->prof.finished_us=sceKernelGetSystemTimeWide();
#endif
    if(s->wake>=0)sceKernelSignalSema(s->wake,1);
    return 0;
}

#if BILI_STREAM_PROFILE
static void stream_report(BiliStreamCache *s) {
    uint64_t span=s->prof.last_data_us>s->prof.first_data_us?s->prof.last_data_us-s->prof.first_data_us:0;
    uint64_t elapsed=s->prof.finished_us>s->prof.started_us?s->prof.finished_us-s->prof.started_us:0;
    uint64_t rate=span?(uint64_t)s->prof.bytes*1000000u/span:0;
    printf("STREAM http connect_us=%llu first_byte_us=%llu total_us=%llu speed_bps=%llu\n",
           (unsigned long long)s->prof.connect_us,(unsigned long long)s->prof.first_byte_us,
           (unsigned long long)s->prof.http_total_us,(unsigned long long)s->prof.http_speed_bps);
    printf("STREAM dl writes=%u bytes=%u elapsed_us=%llu span_us=%llu rate_bps=%llu callback_us=%llu backpressure_us=%llu waits=%u lock_us=%llu copy_us=%llu min_us=%llu wake_us=%llu unlock_us=%llu wake_errors=%u max_chunk=%u max_gap_us=%llu stalls=%u stall_us=%llu\n",
           s->prof.writes,s->prof.bytes,(unsigned long long)elapsed,(unsigned long long)span,
           (unsigned long long)rate,(unsigned long long)s->prof.callback_us,
           (unsigned long long)s->prof.backpressure_us,s->prof.waits,
           (unsigned long long)s->prof.lock_us,(unsigned long long)s->prof.copy_us,
           (unsigned long long)s->prof.min_us,(unsigned long long)s->prof.wake_us,
           (unsigned long long)s->prof.unlock_us,s->prof.wake_errors,
           s->prof.max_chunk,(unsigned long long)s->prof.max_gap_us,
           s->prof.stalls,(unsigned long long)s->prof.stall_us);
    for(int i=0;i<STREAM_READER_SLOTS;++i){
        StreamReader *r=&s->readers[i];
        if(!r->tid)continue;
        uint64_t copy_bps=r->copy_us?(uint64_t)r->bytes*1000000u/r->copy_us:0;
        printf("STREAM reader slot=%d tid=0x%08x calls=%u bytes=%u waits=%u wait_us=%llu max_wait_us=%llu lock_us=%llu copy_us=%llu copy_bps=%llu\n",
               i,(unsigned)r->tid,r->calls,r->bytes,r->waits,
               (unsigned long long)r->wait_us,(unsigned long long)r->max_wait_us,
               (unsigned long long)r->lock_us,(unsigned long long)r->copy_us,
               (unsigned long long)copy_bps);
    }
}
#endif

static int stream_begin(const BiliStream *stream,BiliPlayback *out) {
    BiliStreamCache *s=&active_stream;
    memset(s,0,sizeof(*s));
    s->io_lock=s->wake=s->thread=s->fd=-1;
#if BILI_STREAM_PROFILE
    s->prof.started_us=sceKernelGetSystemTimeWide();
#endif
    s->expected=(unsigned)stream->size;
    s->disk=s->expected>STREAM_RAM_CACHE_BYTES;
    s->capacity=s->disk?0:s->expected;
    s->segment_count=stream->count?stream->count:1;
    for(int i=0;i<s->segment_count && i<BILI_STREAM_SEGMENTS;++i){
        const char *url=i<stream->count?stream->segments[i].url:stream->url;
        unsigned long long bytes=i<stream->count?stream->segments[i].size:stream->size;
        snprintf(s->segments[i].url,sizeof(s->segments[i].url),"%s",url);
        s->segments[i].size=bytes;
    }
#ifdef BILI_DEVICE_LIVE
    printf("LIVE STREAM ram=%u bytes expected=%u\n",s->capacity,s->expected);
#endif
    if(s->disk){
        snprintf(s->path,sizeof(s->path),"%s",out->path);
        snprintf(s->part,sizeof(s->part),"%s.part",out->path);
        s->fd=sceIoOpen(s->part,PSP_O_RDWR|PSP_O_CREAT|PSP_O_TRUNC,0777);
        if(s->fd<0){
            snprintf(network_error,sizeof(network_error),"Cannot open streaming cache");goto fail;
        }
    }else{
        s->ring=malloc(s->capacity);
        if(!s->ring){snprintf(network_error,sizeof(network_error),"Cannot allocate online video buffer");return -1;}
    }
    s->io_lock=sceKernelCreateSema("bili_stream_io",0,1,1,NULL);
    s->wake=sceKernelCreateSema("bili_stream_data",0,0,1,NULL);
    if(s->io_lock<0 || s->wake<0){snprintf(network_error,sizeof(network_error),"Cannot create stream synchronization");goto fail;}
    s->source=(PspVideoSource){s,(int64_t)s->expected,stream_read_at,stream_cancel,NULL,stream_interrupt_reads};
    s->thread=sceKernelCreateThread("bili_stream",stream_download_thread,
                                    BILI_STREAM_THREAD_PRIORITY,128*1024,
                                    PSP_THREAD_ATTR_USER,NULL);
    if(s->thread<0){snprintf(network_error,sizeof(network_error),"Cannot start stream worker");goto fail;}
    int started=sceKernelStartThread(s->thread,sizeof(s),&s);
    if(started<0){sceKernelDeleteThread(s->thread);s->thread=-1;snprintf(network_error,sizeof(network_error),"Cannot start stream worker");goto fail;}
    out->streaming=1;
    out->stream=s;
    return 0;
fail:
    if(s->fd>=0){sceIoClose(s->fd);s->fd=-1;}
    if(s->io_lock>=0){sceKernelDeleteSema(s->io_lock);s->io_lock=-1;}
    if(s->wake>=0){sceKernelDeleteSema(s->wake);s->wake=-1;}
    free(s->ring);s->ring=NULL;
    if(s->disk && s->part[0])remove(s->part);
    return -1;
}

int bili_prepare_stream_playback(const BiliStream *stream,const char *cache_key,BiliPlayback *out) {
    if(!stream||!cache_key||!out)return -1;
    memset(out,0,sizeof(*out));
    snprintf(out->path,sizeof(out->path),CLIENT_PATH("/cache/%s.mp4"),cache_key);
    return stream_begin(stream,out);
}
int bili_prepare_playback(const BiliVideo *v,BiliPlayback *out) {
    if(!v || !out)return -1;
    memset(out,0,sizeof(*out));
    snprintf(out->path,sizeof(out->path),CLIENT_PATH("/cache/%s_%lld.mp4"),v->bvid,v->cid);
    SceUID cached=sceIoOpen(out->path,PSP_O_RDONLY,0);
    if(cached>=0){
        const SceOff end=sceIoLseek(cached,0,SEEK_END);
        sceIoClose(cached);
        if(end>0)return 0;
    }
    if(network_connect()<0)return -1;
    char url[256],*json=0;
    snprintf(url,sizeof(url),"https://api.bilibili.com/x/player/playurl?bvid=%s&cid=%lld&qn=16&fnval=1&platform=html5",v->bvid,v->cid);
    if(get_json(url,&json)<0)return -1;
    BiliStream stream;
    int r=bili_parse_stream(json,&stream);
    free(json);
    if(r<0)return r;
#if BILI_STREAM_FORCE_HTTP
    if(!strncmp(stream.url,"https://",8)){
        memmove(stream.url+4,stream.url+5,strlen(stream.url+5)+1);
        memcpy(stream.url,"http",4);
    }
#endif
    return stream_begin(&stream,out);
}

int bili_prepare_range_playback(const BiliVideo *v, const BiliMediaUrl *video_url,
                                const BiliMediaUrl *audio_url, BiliPlayback *out) {
    if (!v || !video_url || !audio_url || !out) return -1;
    memset(out, 0, sizeof(*out));
    snprintf(out->path, sizeof(out->path), "dash");
    if (network_connect() < 0) return -1;
    network_phase = NET_VIDEO_SOURCE;
    char error[160] = {0};
    int64_t video_size = 0, audio_size = 0;
    BiliRangeStream *video = NULL, *audio = NULL;
    if (range_begin(video_url, "video", &video_size, &video, error, sizeof(error)) < 0) {
        snprintf(network_error, sizeof(network_error), "%s", error);
        return -1;
    }
    if (range_begin(audio_url, "audio", &audio_size, &audio, error, sizeof(error)) < 0) {
        int ignored = 0;
        range_finish(video, &ignored);
        snprintf(network_error, sizeof(network_error), "%s", error);
        return -1;
    }
#ifdef BILI_DEVICE_LIVE
    printf("LIVE RANGE video=%u audio=%u\n", (unsigned)video_size, (unsigned)audio_size);
#endif
    out->streaming = 1;
    out->video = video;
    out->audio = audio;
    return 0;
}

const PspVideoSource *bili_playback_source(const BiliPlayback *playback) {
    if (!playback || !playback->streaming) return NULL;
    if (playback->live) return bili_live_source(playback->live,0);
    if (playback->video) return &playback->video->source;
    return playback->stream ? &playback->stream->source : NULL;
}

const PspVideoSource *bili_playback_audio_source(const BiliPlayback *playback) {
    if (playback && playback->live) return bili_live_source(playback->live,1);
    if (!playback || !playback->streaming || !playback->audio) return NULL;
    return &playback->audio->source;
}

int bili_finish_playback(BiliPlayback *playback) {
    if(!playback) return 0;
    if(playback->live){int result=bili_live_finish(playback->live);playback->live=NULL;playback->streaming=0;return result;}
    if(playback->video || playback->audio) {
        int failed = 0;
        range_finish(playback->video, &failed);
        range_finish(playback->audio, &failed);
        playback->video = NULL;
        playback->audio = NULL;
        playback->streaming = 0;
        return failed ? -1 : 0;
    }
    if(!playback->stream)return 0;
    BiliStreamCache *s=playback->stream;
    int failed=s->failed;
    if(!s->complete)s->cancelled=1;
    if(s->wake>=0)sceKernelSignalSema(s->wake,1);
    if(s->thread>=0){
        sceKernelWaitThreadEndCB(s->thread,NULL);
        sceKernelDeleteThread(s->thread);
        s->thread=-1;
    }
    if(s->fd>=0){sceIoClose(s->fd);s->fd=-1;}
    if(s->disk){
        if(!failed && s->complete && s->available==s->expected){
            remove(s->path);
            if(rename(s->part,s->path)!=0){
                failed=1;
                snprintf(network_error,sizeof(network_error),"Cannot finalize video cache");
            }
        }else{
            remove(s->part);
        }
    }
#if BILI_STREAM_PROFILE
    stream_report(s);
#endif
    if(failed && s->error[0])snprintf(network_error,sizeof(network_error),"%s",s->error);
    if(s->io_lock>=0)sceKernelDeleteSema(s->io_lock);
    if(s->wake>=0)sceKernelDeleteSema(s->wake);
    free(s->ring);s->ring=NULL;
    s->io_lock=s->wake=-1;
    playback->stream=NULL;
    playback->streaming=0;
    return failed?-1:0;
}
#endif
