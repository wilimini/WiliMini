#ifndef BILI_NETWORK_POWER_H
#define BILI_NETWORK_POWER_H

/* One power-callback writer; request owners read without blocking. The low bit
 * gates sleep, and upper bits distinguish connection lifetimes across wake. */
typedef struct {
    unsigned state, disconnected_generation;
} NetworkPower;

static inline unsigned network_power_generation(const NetworkPower *power) {
    return __atomic_load_n(&power->state, __ATOMIC_ACQUIRE) & ~1u;
}
static inline int network_power_interrupted(const NetworkPower *power, unsigned generation) {
    return __atomic_load_n(&power->state, __ATOMIC_ACQUIRE) != generation ||
           __atomic_load_n(&power->disconnected_generation, __ATOMIC_ACQUIRE) != generation;
}
static inline void network_power_set_suspended(NetworkPower *power, int suspended) {
    unsigned state = __atomic_load_n(&power->state, __ATOMIC_RELAXED);
    if (!!(state & 1u) == !!suspended) return;
    state = suspended ? ((state + 2u) | 1u) : (state & ~1u);
    __atomic_store_n(&power->state, state, __ATOMIC_RELEASE);
}
static inline void network_power_disconnected(NetworkPower *power, unsigned generation) {
    __atomic_store_n(&power->disconnected_generation, generation, __ATOMIC_RELEASE);
}
#endif
