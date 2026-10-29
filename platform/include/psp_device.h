#ifndef PSP_DEVICE_H
#define PSP_DEVICE_H

typedef struct {
    int model;
    unsigned max_free_memory;
} PspDeviceInfo;

PspDeviceInfo psp_device_query(void);

/* Model 0 is PSP-1000. Leave room for code, threads and network pools. */
static inline int psp_device_use_extended_client(PspDeviceInfo device)
{
    return device.model > 0 && device.max_free_memory >= 36u * 1024 * 1024;
}

#endif
