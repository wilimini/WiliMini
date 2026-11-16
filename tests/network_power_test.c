#include "network_power.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    NetworkPower power = {0};
    unsigned old = network_power_generation(&power);
    assert(!network_power_interrupted(&power, old));
    // Registration and duplicate resume notifications do not drop connections.
    network_power_set_suspended(&power, 0);
    assert(network_power_generation(&power) == old);
    network_power_set_suspended(&power, 1);
    unsigned next = network_power_generation(&power);
    assert(next != old);
    network_power_set_suspended(&power, 1);
    assert(network_power_generation(&power) == next);
    assert(network_power_interrupted(&power, old));
    assert(network_power_interrupted(&power, next));
    // Firmware can resume before the disconnect worker gets scheduled.
    network_power_set_suspended(&power, 0);
    assert(network_power_interrupted(&power, next));
    network_power_disconnected(&power, next);
    assert(!network_power_interrupted(&power, next));
    assert(network_power_interrupted(&power, old));
    network_power_set_suspended(&power, 0);
    assert(!network_power_interrupted(&power, next));
    // Disconnect acknowledgement must not release the sleep gate.
    network_power_set_suspended(&power, 1);
    unsigned third = network_power_generation(&power);
    network_power_disconnected(&power, third);
    assert(network_power_interrupted(&power, third));
    network_power_set_suspended(&power, 0);
    assert(!network_power_interrupted(&power, third));
    assert(network_power_interrupted(&power, next));
    // A delayed acknowledgement from an older sleep cannot release new work.
    network_power_set_suspended(&power, 1);
    network_power_set_suspended(&power, 0);
    network_power_disconnected(&power, third);
    assert(network_power_interrupted(&power, network_power_generation(&power)));
    puts("network power lifecycle passed");
    return 0;
}
