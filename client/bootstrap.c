/* Firmware network/ME exports must exist before the client PRX is linked.
 * This also makes PSPLink launches follow the same order as XMB launches. */
#include <pspkernel.h>
#include <psputility.h>
#include <pspdebug.h>
#include <pspiofilemgr.h>
#include <kubridge.h>
#include <stdio.h>
#include <string.h>
#include "storage_paths.h"
#include <psp_device.h>
#include <psp_runtime.h>
#include <wili_log.h>
#include "me_backend_select.h"
PSP_MODULE_INFO("WiliMini loader",PSP_MODULE_USER,0,1);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(128);
/* argv[0] is the path; the optional flag tells the client that no ME bridge is loaded. */
static int start(const char *path,int no_me){
    SceUID m=sceKernelLoadModule(path,0,0);if(m<0)return m;
    char args[160];
    const size_t n=strlen(path)+1;
    memcpy(args,path,n);
    size_t size=n;
    if(no_me){memcpy(args+size,"--no-me",8);size+=8;}
    int status=0;return sceKernelStartModule(m,size,args,&status,0);
}
static int file_exists(const char *path){
    SceIoStat stat;
    return sceIoGetstat(path,&stat)>=0 && FIO_S_ISREG(stat.st_mode);
}
/* settings.json is written by the client; see client/preferences.cpp. */
static int read_settings(const char *root,char *text,size_t capacity){
    char path[128];
    snprintf(path,sizeof(path),"%s/settings.json",root);
    SceUID file=sceIoOpen(path,PSP_O_RDONLY,0);
    if(file<0)return 0;
    const int length=sceIoRead(file,text,capacity-1);
    sceIoClose(file);
    return length>0?length:0;
}
static int saved_me_backend(const char *settings,int length){
    return length>0?wili_me_backend_from_settings(settings,(size_t)length):WILI_ME_BACKEND_CUSTOM_CORE;
}
/* A bridge that fails to start is unloaded so the fallback can export me_lab. */
static int start_bridge(const char *root,int backend){
    char path[128];
    snprintf(path,sizeof(path),"%s/%s",root,wili_me_bridge_file(backend));
    SceUID m=sceKernelLoadModule(path,0,0);if(m<0)return m;
    int status=0;
    int r=sceKernelStartModule(m,strlen(path)+1,(void*)path,&status,0);
    if(r<0)sceKernelUnloadModule(m);
    return r;
}
int main(int argc,char **argv){
    (void)argc;(void)argv;
    const int sce_mpeg=BILI_PPSSPP_SCE_MPEG &&
                      psp_runtime_detect() == PSP_RUNTIME_PPSSPP;
    const char *root=wili_select_storage(file_exists,!sce_mpeg);
    char path[128];
    int r=-1;
    if(!root)goto failed;
    r=sceUtilityLoadModule(PSP_MODULE_NET_COMMON);
    if(r>=0 || (unsigned)r==SCE_ERROR_MODULE_ALREADY_LOADED)r=sceUtilityLoadModule(PSP_MODULE_NET_INET);
    if(r<0 && (unsigned)r!=SCE_ERROR_MODULE_ALREADY_LOADED)goto failed;
    /* sceMpeg (user mode): the client links its stubs and wili-server streams are decoded
     * with it, so the AV modules must be resident before the client PRX is linked. */
    r=sceUtilityLoadAvModule(PSP_AV_MODULE_AVCODEC);
    if(r<0 && (unsigned)r!=SCE_ERROR_MODULE_ALREADY_LOADED)goto failed;
    r=sceUtilityLoadAvModule(PSP_AV_MODULE_MPEGBASE);
    if(r<0 && (unsigned)r!=SCE_ERROR_MODULE_ALREADY_LOADED)goto failed;
    static char settings[4097];
    const int settings_length=read_settings(root,settings,sizeof(settings));
    /* wili-server streams use sceMpeg, which owns the Media Engine: with the stream server on,
     * no ME bridge is loaded and the client must not touch the ME (it is told with --no-me). */
    const int stream_server=wili_stream_server_from_settings(settings,(size_t)settings_length);
    if(stream_server)WLOG_I(WLC_LOADER,"stream_server=on; ME bridge not loaded");
    if(!sce_mpeg && !stream_server){
        int backend=saved_me_backend(settings,settings_length);
        r=start_bridge(root,backend);
        if(r<0 && backend!=WILI_ME_BACKEND_MIST){
            WLOG_W(WLC_LOADER,"me_backend=%s failed=0x%08x; falling back to mist",wili_me_backend_name(backend),r);
            backend=WILI_ME_BACKEND_MIST;
            r=start_bridge(root,backend);
        }
        if(r<0)goto failed;
        WLOG_I(WLC_LOADER,"me_backend=%s",wili_me_backend_name(backend));
    }
    WLOG_I(WLC_LOADER,"player=%s",sce_mpeg?"ppsspp-sceMpeg":stream_server?"psp-sceMpeg":"psp-me");
    const PspDeviceInfo device=psp_device_query();
    const int extended=psp_device_use_extended_client(device);
    snprintf(path,sizeof(path),"%s/%s",root,extended?"wili_mini.prx":"wili_mini_1000.prx");
    WLOG_I(WLC_LOADER,"model=%d max_free=%u profile=%s root=%s",
           device.model,device.max_free_memory,extended?"extended":"psp1000",root);
    r=start(path,stream_server);if(r<0)goto failed;
    sceKernelExitDeleteThread(0);return 0;
failed:
    pspDebugScreenInit();pspDebugScreenPrintf("WiliMini startup failed: 0x%08x\nRoot: %s\nCheck PRXs, cacert.pem and wqy.bmf.\nSearch order: ms0, ef0, host0.\n",r,root?root:"no complete package");
    sceKernelDelayThread(5000000);sceKernelExitGame();return 0;
}
