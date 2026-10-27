/* UI translation (en / uk / ru, design: Vehicle › Language). Table: tools/i18n.tsv -> i18n_tab.h.
 * text_draw / text_width translate by themselves: a string in the table gets its translation, otherwise (names, numbers) it is used as is.
 * Strings with substitution: tr(format) / tr_n(format, number) where they are built. */
#ifndef I18N_H
#define I18N_H

enum { LANG_EN, LANG_UK, LANG_RU };
void i18n_set_lang(int lang);
int  i18n_lang(void);
const char *tr(const char *en);
const char *tr_n(const char *en, long n);     /* phrase with a number: plural form */

#endif
