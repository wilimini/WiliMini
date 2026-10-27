/* Firmware network/ME exports must exist before the client PRX is linked.
 * This also makes PSPLink launches follow the same order as XMB launches. */
#include <pspkernel.h>
#include <psputility.h>
#include <pspdebug.h>
#include <stdio.h>
#include <string.h>
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
    const char *path="ms0:/PSP/GAME/WiliMini/wili_mini.prx";
    /* Always load the packaged client paired with this memory stick. */
    r=start(path);if(r<0)goto failed;
    sceKernelExitDeleteThread(0);return 0;
failed:
    pspDebugScreenInit();pspDebugScreenPrintf("WiliMini startup failed: 0x%08x\nCheck wili_mini.prx and me_bridge.prx in WiliMini.\n",r);
    sceKernelDelayThread(5000000);sceKernelExitGame();return 0;
}
