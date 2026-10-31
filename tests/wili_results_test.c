#include "wili_results.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    /* Names: only run-NNN with 3 to 6 digits. */
    assert(wili_run_index("run-001") == 1);
    assert(wili_run_index("run-999") == 999);
    assert(wili_run_index("run-1000") == 1000);
    assert(wili_run_index("run-999999") == 999999);
    assert(wili_run_index("run-1000000") == -1);
    assert(wili_run_index("run-00") == -1);
    assert(wili_run_index("run-000") == -1);
    assert(wili_run_index("run-01a") == -1);
    assert(wili_run_index("run-") == -1);
    /* A FAT memory stick lists these folders in uppercase. */
    assert(wili_run_index("RUN-001") == 1);
    assert(wili_run_index("Run-042") == 42);
    assert(wili_run_index("RUN-1000") == 1000);
    assert(wili_run_index("run_001") == -1);
    assert(wili_run_index("RUN001") == -1);
    assert(wili_run_index("RUN-0O1") == -1);
    assert(wili_run_index("summary.json") == -1);
    assert(wili_run_index("latest") == -1);
    assert(wili_run_index(NULL) == -1);

    int next, drop[64];
    /* Empty folder. */
    assert(wili_run_plan(NULL, 0, 20, &next, drop, 8) == 0 && next == 1);
    /* Below the limit: nothing dropped. */
    int few[] = {1, 2, 3};
    assert(wili_run_plan(few, 3, 20, &next, drop, 8) == 0 && next == 4);
    /* keep=20 with 19 existing: the new run makes 20, still nothing dropped. */
    int nineteen[19];
    for (int i = 0; i < 19; ++i) nineteen[i] = i + 1;
    assert(wili_run_plan(nineteen, 19, 20, &next, drop, 8) == 0 && next == 20);
    /* 20 existing: the new run would make 21, so the single oldest goes. */
    int twenty[20];
    for (int i = 0; i < 20; ++i) twenty[i] = i + 1;
    assert(wili_run_plan(twenty, 20, 20, &next, drop, 8) == 1 && drop[0] == 1 && next == 21);
    /* Unsorted input with gaps: lowest numbers are dropped, next follows the highest. */
    int gaps[] = {50, 7, 900, 3, 12};
    assert(wili_run_plan(gaps, 5, 3, &next, drop, 8) == 3 && next == 901);
    assert(drop[0] == 3 && drop[1] == 7 && drop[2] == 12);
    /* Old backlog drains in steps of drop_cap. */
    int big[999];
    for (int i = 0; i < 999; ++i) big[i] = i + 1;
    assert(wili_run_plan(big, 999, 20, &next, drop, 8) == 8 && next == 1000);
    for (int i = 0; i < 8; ++i) assert(drop[i] == i + 1);
    /* keep=0 keeps everything; numbering continues past 999. */
    assert(wili_run_plan(big, 999, 0, &next, drop, 8) == 0 && next == 1000);
    /* Duplicates do not produce duplicate drops. */
    int dup[] = {4, 4, 5, 6};
    assert(wili_run_plan(dup, 4, 2, &next, drop, 8) == 3 && drop[0] == 4 && drop[1] == 5 && drop[2] == 6);
    /* Exhausted numbering. */
    int last[] = {999999};
    assert(wili_run_plan(last, 1, 20, &next, drop, 8) == -1);
    puts("Run folder naming, numbering and pruning plan: passed");
    return 0;
}
