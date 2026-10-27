/* UI translation, see i18n.h. Lookup is a hash table keyed by the English string (built on first call). */
#include "i18n.h"
#include "i18n_tab.h"

#include <string.h>

#define NROWS (int)(sizeof I18N / sizeof I18N[0])
#define HSIZE 1024                                  /* > 2 * NROWS, power of two */

static int lang;
static short slot[HSIZE];                           /* row number + 1, 0 - empty */

static unsigned hash(const char *s)
{
    unsigned h = 2166136261u;
    while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

static int find(const char *s)
{
    static int built;
    unsigned h;
    int i;
    if (!built) {
        built = 1;
        for (i = 0; i < NROWS; i++) {
            h = hash(I18N[i].en) & (HSIZE - 1);
            while (slot[h]) h = (h + 1) & (HSIZE - 1);
            slot[h] = (short)(i + 1);
        }
    }
    for (h = hash(s) & (HSIZE - 1); slot[h]; h = (h + 1) & (HSIZE - 1))
        if (!strcmp(I18N[slot[h] - 1].en, s)) return slot[h] - 1;
    return -1;
}

void i18n_set_lang(int l) { lang = l == LANG_UK || l == LANG_RU ? l : LANG_EN; }
int i18n_lang(void) { return lang; }

/* Ukrainian and Russian: 1, 21 - form 0; 2-4, 22-24 - form 1; the rest (5-20, 11-14) - form 2 */
static int plural(long n)
{
    long a = n < 0 ? -n : n;
    if (a % 10 == 1 && a % 100 != 11) return 0;
    if (a % 10 >= 2 && a % 10 <= 4 && (a % 100 < 12 || a % 100 > 14)) return 1;
    return 2;
}

const char *tr_n(const char *en, long n)
{
    int i;
    if (lang == LANG_EN || !en || (i = find(en)) < 0) return en;
    return (lang == LANG_UK ? I18N[i].uk : I18N[i].ru)[plural(n)];
}

const char *tr(const char *en) { return tr_n(en, 1); }
