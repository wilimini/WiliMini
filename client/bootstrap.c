/* Firmware network/ME exports must exist before the client PRX is linked.
 * This also makes PSPLink launches follow the same order as XMB launches. */
#include <pspkernel.h>
#include <psputility.h>
#include <pspdebug.h>
#include <pspiofilemgr.h>
#include <kubridge.h>
#include <stdio.h>
#include <string.h>
#include "loader_profile.h"
#include "storage_paths.h"
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
int main(int argc,char **argv){
    (void)argc;(void)argv;
    const char *root=wili_select_storage(file_exists);
    char path[128];
    int r=-1;
    if(!root)goto failed;
    r=sceUtilityLoadModule(PSP_MODULE_NET_COMMON);
    if(r>=0 || (unsigned)r==SCE_ERROR_MODULE_ALREADY_LOADED)r=sceUtilityLoadModule(PSP_MODULE_NET_INET);
    if(r<0 && (unsigned)r!=SCE_ERROR_MODULE_ALREADY_LOADED)goto failed;
    snprintf(path,sizeof(path),"%s/me_bridge.prx",root);
    r=start(path);if(r<0)goto failed;
    const int model=kuKernelGetModel();
    const unsigned max_free=sceKernelMaxFreeMemSize();
    const int extended=loader_use_extended_client(model,max_free);
    snprintf(path,sizeof(path),"%s/%s",root,extended?"wili_mini.prx":"wili_mini_1000.prx");
    printf("LOADER model=%d max_free=%u profile=%s root=%s\n",model,max_free,extended?"extended":"psp1000",root);
    r=start(path);if(r<0)goto failed;
    sceKernelExitDeleteThread(0);return 0;
failed:
    pspDebugScreenInit();pspDebugScreenPrintf("WiliMini startup failed: 0x%08x\nRoot: %s\nCheck PRXs, cacert.pem and wqy.bmf.\nSearch order: ms0, ef0, host0.\n",r,root?root:"no complete package");
    sceKernelDelayThread(5000000);sceKernelExitGame();return 0;
}
