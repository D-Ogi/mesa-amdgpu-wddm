/* SPDX-License-Identifier: MIT
 *
 * A small matcher for POSIX extended regular expressions, for platforms with no <regex.h> (Windows).
 *
 * It gives only a match result (REG_NOSUB), on bytes, in the C locale. It accepts:
 *   literals, '\' followed by any character, '.', bracket expressions with ranges, negation and the
 *   [:name:] classes, '^', '$', '(...)', '|', and the bounds '*', '+', '?', '{m}', '{m,}' and '{m,n}'.
 * It refuses every other construct (back references, [. .] and [= =], an empty bound, an unmatched
 * parenthesis), so that a pattern that it cannot match exactly never matches by mistake.
 *
 * The compiled program runs as a Thompson NFA (Pike VM): the time is linear in the subject length.
 */

#ifndef U_ERE_H
#define U_ERE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define U_ERE_EXTENDED 1
#define U_ERE_ICASE    2
#define U_ERE_NOSUB    4

/* Results, the same numbers as glibc. */
#define U_ERE_OK       0
#define U_ERE_NOMATCH  1
#define U_ERE_BADPAT   2
#define U_ERE_ESPACE   12

struct u_ere_inst;

struct u_ere {
   struct u_ere_inst *prog;
   unsigned len;
   bool icase;
};

/* Compiles pattern into re. Returns U_ERE_OK, or an error, and then re holds nothing to free. */
int u_ere_comp(struct u_ere *re, const char *pattern, int cflags);

/* Returns U_ERE_OK when the pattern matches a part of subject, else U_ERE_NOMATCH. A NULL subject is "". */
int u_ere_exec(const struct u_ere *re, const char *subject);

void u_ere_free(struct u_ere *re);

#ifdef __cplusplus
}
#endif

#endif
