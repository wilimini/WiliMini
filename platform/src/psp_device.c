#include <kubridge.h>
#include <pspkernel.h>
#include "psp_device.h"

PspDeviceInfo psp_device_query(void)
{
    PspDeviceInfo device = {
        kuKernelGetModel(),
        sceKernelMaxFreeMemSize()
    };
    return device;
}
