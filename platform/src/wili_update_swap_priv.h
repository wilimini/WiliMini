#ifndef WILI_UPDATE_SWAP_PRIV_H
#define WILI_UPDATE_SWAP_PRIV_H
/* Internal to wili_update_swap.c and wili_update_swap_run.c. */
#include "wili_update_swap.h"
#define MAX_PATH 224
#define MAX_REL 100
typedef enum { S_NONE, S_BACKUP, S_INSTALL, S_TRIAL, S_ROLLBACK, S_ROLLBACK_B, S_DONE, S_ROLLBACK2 } phase_t;
typedef struct { phase_t phase; int trial; } state_t;
int swap_write_state(const wili_swap_fs *fs, const char *root, phase_t ph, int trial);
state_t swap_read_state(const wili_swap_fs *fs, const char *root);
void swap_finish_files(const wili_swap_fs *fs, const char *root);
#endif
