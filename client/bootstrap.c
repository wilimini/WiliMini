/* Firmware network/ME exports must exist before the client PRX is linked.
 * This also makes PSPLink launches follow the same order as XMB launches. */
#include <pspkernel.h>
#include <psputility.h>
#include <pspdebug.h>
#include <kubridge.h>
#include <stdio.h>
#include <string.h>
#include "loader_profile.h"
PSP_MODULE_INFO("WiliMini loader",PSP_MODULE_USER,0,1);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(128);
static int start(const char *path){
    SceUID m=sceKernelLoadModule(path,0,0);if(m<0)return m;
    int status=0;return sceKernelStartModule(m,strlen(path)+1,(void*)path,&status,0);
}
int main(int argc,char **argv){
    (void)argc;(void)argv;
    int r=sceUtilityLoadModule(PSP_MODULE_NET_COMMON);
    if(r>=0 || (unsigned)r==SCE_ERROR_MODULE_ALREADY_LOADED)r=sceUtilityLoadModule(PSP_MODULE_NET_INET);
    if(r<0 && (unsigned)r!=SCE_ERROR_MODULE_ALREADY_LOADED)goto failed;
    r=start("ms0:/PSP/GAME/WiliMini/me_bridge.prx");if(r<0)goto failed;
    const int model=kuKernelGetModel();
    const unsigned max_free=sceKernelMaxFreeMemSize();
    const int extended=loader_use_extended_client(model,max_free);
    const char *path=extended ? "ms0:/PSP/GAME/WiliMini/wili_mini.prx"
                              : "ms0:/PSP/GAME/WiliMini/wili_mini_1000.prx";
    printf("LOADER model=%d max_free=%u profile=%s\n",model,max_free,extended?"extended":"psp1000");
    /* Always load the packaged client paired with this memory stick. */
    r=start(path);if(r<0)goto failed;
    sceKernelExitDeleteThread(0);return 0;
failed:
    pspDebugScreenInit();pspDebugScreenPrintf("WiliMini startup failed: 0x%08x\nCheck both client PRXs and me_bridge.prx in WiliMini.\n",r);
    sceKernelDelayThread(5000000);sceKernelExitGame();return 0;
}
