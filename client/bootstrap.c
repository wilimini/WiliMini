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
#include "me_backend_select.h"
PSP_MODULE_INFO("WiliMini loader",PSP_MODULE_USER,0,1);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(128);
static int start(const char *path){
    SceUID m=sceKernelLoadModule(path,0,0);if(m<0)return m;
    int status=0;return sceKernelStartModule(m,strlen(path)+1,(void*)path,&status,0);
}
static int file_exists(const char *path){
    SceIoStat stat;
    return sceIoGetstat(path,&stat)>=0 && FIO_S_ISREG(stat.st_mode);
}
/* settings.json is written by the client; see client/preferences.cpp. */
static int saved_me_backend(const char *root){
    static char text[4097];
    char path[128];
    snprintf(path,sizeof(path),"%s/settings.json",root);
    SceUID file=sceIoOpen(path,PSP_O_RDONLY,0);
    if(file<0)return WILI_ME_BACKEND_CUSTOM_CORE;
    const int length=sceIoRead(file,text,sizeof(text)-1);
    sceIoClose(file);
    return length>0?wili_me_backend_from_settings(text,(size_t)length):WILI_ME_BACKEND_CUSTOM_CORE;
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
    if(sce_mpeg){
        r=sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC);
        if(r<0 && (unsigned)r!=SCE_ERROR_MODULE_ALREADY_LOADED)goto failed;
        r=sceUtilityLoadModule(PSP_MODULE_AV_MPEGBASE);
        if(r<0 && (unsigned)r!=SCE_ERROR_MODULE_ALREADY_LOADED)goto failed;
    }else{
        int backend=saved_me_backend(root);
        r=start_bridge(root,backend);
        if(r<0 && backend!=WILI_ME_BACKEND_MIST){
            printf("LOADER me_backend=%s failed=0x%08x; falling back to mist\n",wili_me_backend_name(backend),r);
            backend=WILI_ME_BACKEND_MIST;
            r=start_bridge(root,backend);
        }
        if(r<0)goto failed;
        printf("LOADER me_backend=%s\n",wili_me_backend_name(backend));
    }
    printf("LOADER player=%s\n",sce_mpeg?"ppsspp-sceMpeg":"psp-me");
    const PspDeviceInfo device=psp_device_query();
    const int extended=psp_device_use_extended_client(device);
    snprintf(path,sizeof(path),"%s/%s",root,extended?"wili_mini.prx":"wili_mini_1000.prx");
    printf("LOADER model=%d max_free=%u profile=%s root=%s\n",
           device.model,device.max_free_memory,extended?"extended":"psp1000",root);
    r=start(path);if(r<0)goto failed;
    sceKernelExitDeleteThread(0);return 0;
failed:
    pspDebugScreenInit();pspDebugScreenPrintf("WiliMini startup failed: 0x%08x\nRoot: %s\nCheck PRXs, cacert.pem and wqy.bmf.\nSearch order: ms0, ef0, host0.\n",r,root?root:"no complete package");
    sceKernelDelayThread(5000000);sceKernelExitGame();return 0;
}
