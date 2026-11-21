/* cycle 8 (G6): bot names. The game names bots from texts.csv TID_BOT_1..9 ("Bot 1".."Bot 9"); shims/shim_asset.c serves localization/texts.csv
 * from memory with those values replaced (the user's file on the card is never changed). Our own invented player-style names; the set is
 * rotated by a seed (the boot time) so every session has other bots. */
#include "botnames.h"

#include <stdlib.h>
#include <string.h>

static const char *const k_names[] = {
    "xX_Shadow_Xx", "Brawler2020", "NoScopeNinja", "PixelMain", "Kai", "Lena", "Marco", "Yuki", "TheRealBear", "LocoLoco", "SpikeyBoi", "Mia",
    "GG_EZ", "Leo", "Sofia", "ProGemGrabber", "BarleyBrewer", "SniperCat", "Jonas", "Aiko", "Tomas", "Nova", "Zed", "RavenFeather", "NightMain",
    "Elif", "Rafael", "Hugo", "Emma", "Diego", "Noah", "Lucas", "Iris", "TickTockTim", "BoomBox", "DynaMax99", "PowerPrime", "RoseBloom",
    "StarPlayer", "Clutch_King", "Lil_Dash", "HomerunHero", "8BitHero", "Mr_Penny", "Frankie", "Nils", "Oskar", "Ines", "Mateo", "Zoe",
    "Tryhard_Tim", "Sneaky_Sam", "Busy_Bee", "MaxSpeed", "JackyJack", "MagicGene", "SuperMom", "SproutLife", "Captain_Pick", "TarotQueen"};
#define NNAMES ((int)(sizeof k_names / sizeof k_names[0]))

const char *bv_botname_override[10];
const char *bv_botname(uint32_t seed, int n) {
    if (n > 0 && n < 10 && bv_botname_override[n]) return bv_botname_override[n];
    return k_names[(int)((seed + (uint32_t)n * 7u) % (uint32_t)NNAMES)];
}

char *bv_botnames_patch(const char *in, size_t len, size_t *out_len, uint32_t seed, int *replaced) {
    size_t cap = len + 64 * 16 + 1;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t o = 0, i = 0;
    int nrep = 0;
    while (i < len) {
        size_t e = i;
        while (e < len && in[e] != '\n') e++;
        size_t l = e - i;   /* line without the newline */
        int n = 0;
        if (l > 10 && !memcmp(in + i, "\"TID_BOT_", 9)) {
            size_t k = i + 9;
            while (k < e && in[k] >= '0' && in[k] <= '9') { n = n * 10 + (in[k] - '0'); k++; }
            if (!(k + 3 <= e && in[k] == '"' && in[k + 1] == ',' && in[k + 2] == '"')) n = 0;
        }
        if (n > 0 && nrep < 64) {
            const char *name = bv_botname(seed, n);
            const char *comma = (const char *)memchr(in + i, ',', l);
            size_t head = comma ? (size_t)(comma - (in + i)) : l;
            /* "TID_BOT_n","<name>" + the rest after the second field (other columns, CR) */
            const char *p2 = in + i + head + 2;                                    /* first char of the old value */
            const char *q = memchr(p2, '"', (size_t)(e - (size_t)(p2 - in)));     /* its closing quote */
            if (q) {
                memcpy(out + o, in + i, head + 2);
                o += head + 2;
                size_t nl = strlen(name);
                memcpy(out + o, name, nl);
                o += nl;
                size_t rest = (size_t)((in + e) - q);
                memcpy(out + o, q, rest);
                o += rest;
                nrep++;
            } else {
                memcpy(out + o, in + i, l);
                o += l;
            }
        } else {
            memcpy(out + o, in + i, l);
            o += l;
        }
        if (e < len) out[o++] = '\n';
        i = e + 1;
    }
    out[o] = 0;
    if (out_len) *out_len = o;
    if (replaced) *replaced = nrep;
    return out;
}
