#include "loader_profile.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    const unsigned mib = 1024u * 1024;
    // A Fat must never select the larger client, even with a misleading free count.
    assert(!loader_use_extended_client(0, 55 * mib));
    assert(!loader_use_extended_client(0, 24 * mib));
    assert(!loader_use_extended_client(-1, 55 * mib));
    assert(loader_use_extended_client(1, 55 * mib));
    assert(loader_use_extended_client(2, 55 * mib));
    // PSPLink or firmware can launch a Slim without enabling extended RAM.
    assert(!loader_use_extended_client(1, 24 * mib));
    assert(!loader_use_extended_client(2, 36 * mib - 1));
    assert(loader_use_extended_client(2, 36 * mib));
    puts("Loader model and available-memory selection: passed");
}
