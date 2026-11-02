#include "psp_device.h"
#include <assert.h>
#include <stdio.h>

static int use(PspDeviceInfo device) { return psp_device_use_extended_client(device); }

int main(void) {
    const unsigned mib = 1024u * 1024;
    // A Fat must never select the larger client, even with a misleading free count.
    assert(!use((PspDeviceInfo){0, 55 * mib}));
    assert(!use((PspDeviceInfo){0, 24 * mib}));
    assert(!use((PspDeviceInfo){-1, 55 * mib}));
    assert(use((PspDeviceInfo){1, 55 * mib}));
    assert(use((PspDeviceInfo){2, 55 * mib}));
    // PSPLink or firmware can launch a Slim without enabling extended RAM.
    assert(!use((PspDeviceInfo){1, 24 * mib}));
    assert(!use((PspDeviceInfo){2, 36 * mib - 1}));
    assert(use((PspDeviceInfo){2, 36 * mib}));
    puts("Loader model and available-memory selection: passed");
}
