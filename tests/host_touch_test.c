/* Host test of the touch state machine (loader/touch.c).
 * Build: cc -Iloader tests/host_touch_test.c loader/touch.c -o host_touch_test
 */
#include <stdio.h>
#include <string.h>

#include "touch.h"

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); g_fail++; } } while (0)

typedef struct {
    int n;
    int phase[32], x[32], y[32], id[32];
} log_t;

static void emit(void *ctx, int phase, int x, int y, int id) {
    log_t *l = (log_t *)ctx;
    if (l->n < 32) {
        l->phase[l->n] = phase;
        l->x[l->n] = x;
        l->y[l->n] = y;
        l->id[l->n] = id;
        l->n++;
    }
}

int main(void) {
    bvt_state s;
    log_t l;
    bvt_report r[2];

    /* the front panel is 1920x1088, the game surface 960x544: every panel unit is half a pixel */
    bvt_init(&s, 960, 544, 0, 0, 1919, 1087);

    /* a tap: down, then up at the last position */
    memset(&l, 0, sizeof l);
    r[0] = (bvt_report){7, 1000, 600};
    bvt_update(&s, r, 1, emit, &l);
    CHECK(l.n == 1 && l.phase[0] == BVT_DOWN && l.x[0] == 500 && l.y[0] == 300 && l.id[0] == 0, "down at 500,300, pointer id 0 although the panel said 7 (n=%d)", l.n);
    bvt_update(&s, r, 1, emit, &l);
    CHECK(l.n == 1, "an unchanged snapshot sends nothing (n=%d)", l.n);
    bvt_update(&s, r, 0, emit, &l);
    CHECK(l.n == 2 && l.phase[1] == BVT_UP && l.x[1] == 500 && l.y[1] == 300 && l.id[1] == 0, "up at the last position");
    CHECK(s.n == 0, "nothing left down");

    /* a drag */
    memset(&l, 0, sizeof l);
    r[0] = (bvt_report){1, 0, 0};
    bvt_update(&s, r, 1, emit, &l);
    r[0].x = 400;
    r[0].y = 200;
    bvt_update(&s, r, 1, emit, &l);
    CHECK(l.n == 2 && l.phase[1] == BVT_MOVE && l.x[1] == 200 && l.y[1] == 100, "move to 200,100");
    r[0].x = 401; /* half a pixel: the game sees the same position */
    bvt_update(&s, r, 1, emit, &l);
    CHECK(l.n == 2, "a sub-pixel change is not a move (n=%d)", l.n);
    bvt_update(&s, r, 0, emit, &l);

    /* two fingers: a move of one reports both (like a MotionEvent), the second finger lifting reports its own position */
    memset(&l, 0, sizeof l);
    bvt_init(&s, 960, 544, 0, 0, 1919, 1087);
    r[0] = (bvt_report){1, 200, 200};
    bvt_update(&s, r, 1, emit, &l);
    r[1] = (bvt_report){2, 1600, 800};
    bvt_update(&s, r, 2, emit, &l);
    CHECK(l.n == 2 && l.phase[1] == BVT_DOWN && l.id[1] == 1 && l.x[1] == 800 && l.y[1] == 400, "second finger down (pointer id 1)");
    r[1].x = 1700;
    bvt_update(&s, r, 2, emit, &l);
    CHECK(l.n == 4 && l.phase[2] == BVT_MOVE && l.phase[3] == BVT_MOVE && l.id[2] == 0 && l.id[3] == 1 && l.x[3] == 850, "both pointers in one move");
    bvt_update(&s, r, 1, emit, &l); /* finger 2 gone */
    CHECK(l.n == 5 && l.phase[4] == BVT_UP && l.id[4] == 1 && l.x[4] == 850, "second finger up at 850,400");
    bvt_cancel_all(&s, emit, &l);
    CHECK(l.n == 6 && l.phase[5] == BVT_CANCEL && s.n == 0, "cancel releases the rest");
    bvt_cancel_all(&s, emit, &l);
    CHECK(l.n == 6, "cancel with nothing down sends nothing");

    /* order inside one snapshot: ups, then downs, then moves; ids may be reused */
    memset(&l, 0, sizeof l);
    bvt_init(&s, 960, 544, 0, 0, 1919, 1087);
    r[0] = (bvt_report){1, 100, 100};
    bvt_update(&s, r, 1, emit, &l);
    r[0] = (bvt_report){2, 300, 300}; /* finger 1 left, finger 2 arrived in the same snapshot */
    bvt_update(&s, r, 1, emit, &l);
    CHECK(l.n == 3 && l.phase[1] == BVT_UP && l.id[1] == 0 && l.phase[2] == BVT_DOWN && l.id[2] == 0, "up before down; the freed pointer id 0 is reused");

    /* the panel's contact ids keep counting up; the game must only ever see small recycled ids like on Android */
    memset(&l, 0, sizeof l);
    bvt_init(&s, 960, 544, 0, 0, 1919, 1087);
    for (int round = 0; round < 40; round++) {
        r[0] = (bvt_report){100 + 2 * round, 400, 400};
        r[1] = (bvt_report){101 + 2 * round, 800, 400};
        bvt_update(&s, r, 2, emit, &l);
        bvt_update(&s, r, 0, emit, &l);
        l.n = l.n > 24 ? 0 : l.n; /* keep the log small, the checks below look at the last events */
    }
    for (int i = 0; i < l.n; i++) CHECK(l.id[i] == 0 || l.id[i] == 1, "pointer id %d is not a small recycled slot", l.id[i]);
    CHECK(s.n == 0, "everything released after 40 rounds");

    /* limits: coordinates outside the active area are clamped, a seventh contact is ignored */
    memset(&l, 0, sizeof l);
    bvt_init(&s, 960, 544, 0, 0, 1919, 1087);
    bvt_report many[8];
    for (int i = 0; i < 8; i++) many[i] = (bvt_report){i + 1, 5000, -20};
    bvt_update(&s, many, 8, emit, &l);
    CHECK(l.n == 6 && l.x[0] == 959 && l.y[0] == 0, "clamped to the surface, six contacts at most (n=%d x=%d y=%d)", l.n, l.x[0], l.y[0]);

    /* an active area with an offset (some firmware reports 0..1919 x 108..979) */
    memset(&l, 0, sizeof l);
    bvt_init(&s, 960, 544, 0, 108, 1919, 979);
    r[0] = (bvt_report){1, 960, 108};
    bvt_update(&s, r, 1, emit, &l);
    CHECK(l.n == 1 && l.x[0] == 480 && l.y[0] == 0, "offset active area: %d,%d", l.x[0], l.y[0]);
    r[0] = (bvt_report){1, 960, 979};
    bvt_update(&s, r, 1, emit, &l);
    CHECK(l.y[1] == 543, "bottom edge maps to the last row (%d)", l.y[1]);

    if (g_fail) {
        printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    printf("ALL CHECKS PASSED\n");
    return 0;
}
