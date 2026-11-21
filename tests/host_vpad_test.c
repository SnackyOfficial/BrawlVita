/* Host test of loader/vpad.c (cycle 7, J1; update 146: the finger lands on the joystick centre, then slides out at most 24 px per poll). */
#include <stdio.h>
#include <string.h>

#include "../loader/vpad.h"

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static struct { int phase, x, y, id; } ev[256];
static int nev;
static void emit(void *ctx, int phase, int x, int y, int id) {
    (void)ctx;
    if (nev < 256) { ev[nev].phase = phase; ev[nev].x = x; ev[nev].y = y; ev[nev].id = id; nev++; }
}
/* n polls with the same input */
static void polls(vpad_state *s, int n, uint32_t b, int lx, int ly, int rx, int ry) {
    for (int i = 0; i < n; i++) vpad_update(s, b, lx, ly, rx, ry, emit, NULL);
}
/* the last event of finger `id` (-1 if none) */
static int last_of(int id) {
    for (int i = nev - 1; i >= 0; i--) if (ev[i].id == id) return i;
    return -1;
}
static int max_step(int id) {   /* largest jump between consecutive positions of one finger */
    int px = -1, py = -1, m = 0;
    for (int i = 0; i < nev; i++) {
        if (ev[i].id != id) continue;
        if (px >= 0) { int dx = ev[i].x - px, dy = ev[i].y - py; if (dx < 0) dx = -dx; if (dy < 0) dy = -dy; if (dx > m) m = dx; if (dy > m) m = dy; }
        px = ev[i].x; py = ev[i].y;
    }
    return m;
}

int main(void) {
    vpad_cfg c;
    vpad_default(&c);
    vpad_state s;
    vpad_init(&s, &c);
    CHECK(c.radius >= 100 && c.aim_radius >= 100, "joystick reach (update 146): %d / %d", c.radius, c.aim_radius);

    nev = 0;
    polls(&s, 1, 0, 128, 128, 128, 128);
    CHECK(nev == 0, "centred sticks: no events (%d)", nev);

    nev = 0;
    polls(&s, 1, 0, 255, 128, 128, 128);   /* left stick full right: first poll = the finger lands on the centre */
    CHECK(nev == 1 && ev[0].phase == 0 && ev[0].id == VPAD_ID_MOVE && ev[0].x == c.move_x && ev[0].y == c.move_y, "move: down at the joystick centre first (%d)", nev);
    polls(&s, 10, 0, 255, 128, 128, 128);
    int k = last_of(VPAD_ID_MOVE);
    CHECK(k > 1 && ev[k].phase == 2 && ev[k].x == c.move_x + c.radius && ev[k].y == c.move_y, "move: slid out to the right edge (%d,%d)", k >= 0 ? ev[k].x : -1, k >= 0 ? ev[k].y : -1);
    CHECK(max_step(VPAD_ID_MOVE) <= 24, "move: a drag in small steps (max %d px)", max_step(VPAD_ID_MOVE));
    CHECK(nev >= 5, "move: several move events, not one jump (%d)", nev);
    nev = 0;
    polls(&s, 1, 0, 255, 128, 128, 128);
    CHECK(nev == 0, "same stick after reaching the edge: no new events");
    nev = 0;
    polls(&s, 1, 0, 128, 128, 128, 128);
    CHECK(nev == 1 && ev[0].phase == 1 && ev[0].id == VPAD_ID_MOVE, "stick released: finger up");

    nev = 0;
    polls(&s, 10, 0, 128, 128, 128, 0);     /* right stick up: aim */
    k = last_of(VPAD_ID_AIM);
    CHECK(ev[0].id == VPAD_ID_AIM && ev[0].phase == 0 && ev[0].x == c.attack_x && k > 0 && ev[k].y == c.attack_y - c.aim_radius, "aim: attack joystick dragged up");
    nev = 0;
    polls(&s, 1, 0, 128, 128, 128, 128);
    CHECK(nev == 1 && ev[0].phase == 1 && ev[0].id == VPAD_ID_AIM && ev[0].y == c.attack_y - c.aim_radius, "aim released at the aimed point = shot");

    nev = 0;
    polls(&s, 1, VPAD_BTN_R, 128, 128, 128, 128);
    polls(&s, 1, 0, 128, 128, 128, 128);
    CHECK(nev == 2 && ev[0].phase == 0 && ev[1].phase == 1 && ev[0].x == c.attack_x && ev[0].id == VPAD_ID_TAP, "R: quick attack tap");

    nev = 0;
    polls(&s, 1, VPAD_BTN_L, 128, 128, 128, 128);
    CHECK(nev == 0, "L held alone: nothing yet");
    polls(&s, 1, 0, 128, 128, 128, 128);
    CHECK(nev == 2 && ev[0].x == c.super_x && ev[1].phase == 1, "L released: super tap");

    nev = 0;
    polls(&s, 10, VPAD_BTN_L, 128, 128, 255, 128);   /* L + right stick: aimed super */
    k = last_of(VPAD_ID_AIM);
    CHECK(ev[0].x == c.super_x && ev[0].id == VPAD_ID_AIM && k > 0 && ev[k].x == c.super_x + c.aim_radius, "L + right stick drags the super");
    nev = 0;
    polls(&s, 1, VPAD_BTN_L, 128, 128, 128, 128);
    polls(&s, 1, 0, 128, 128, 128, 128);
    CHECK(nev == 1 && ev[0].phase == 1 && ev[0].x == c.super_x + c.aim_radius, "aimed super released once, no extra tap (%d)", nev);

    nev = 0;
    polls(&s, 10, VPAD_BTN_UP, 128, 128, 128, 128);
    k = last_of(VPAD_ID_MOVE);
    CHECK(k > 0 && ev[k].y == c.move_y - c.radius, "d-pad up moves");
    nev = 0;
    vpad_release_all(&s, emit, NULL);
    CHECK(nev == 1 && ev[0].phase == 1, "release_all lifts the move finger");

    if (g_fail) { printf("%d FAILURES\n", g_fail); return 1; }
    printf("ALL CHECKS PASSED\n");
    return 0;
}
