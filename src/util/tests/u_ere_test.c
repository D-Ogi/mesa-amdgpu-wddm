/* SPDX-License-Identifier: MIT
 *
 * Host driver for u_ere (util/u_ere.h), run by u_ere_check.py. Each input line is
 * "<pattern hex> <subject hex> <cflags>" ("-" for an empty string); each output line is
 * "<u_ere_comp result> <u_ere_exec result, or -1 when the pattern does not compile>".
 *
 *   clang-cl -O2 -D_CRT_SECURE_NO_WARNINGS -I src src/util/u_ere.c src/util/tests/u_ere_test.c -Fe:u_ere_test.exe
 *   python src/util/tests/u_ere_check.py . u_ere_test.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/u_ere.h"

static char *
unhex(const char *h, size_t n)
{
   char *s = malloc(n / 2 + 1);
   for (size_t i = 0; i < n / 2; i++) {
      unsigned v;
      sscanf(h + 2 * i, "%2x", &v);
      s[i] = (char)v;
   }
   s[n / 2] = 0;
   return s;
}

int
main(void)
{
   static char line[1 << 16];
   while (fgets(line, sizeof(line), stdin)) {
      char *a = strtok(line, " \r\n");
      char *b = strtok(NULL, " \r\n");
      char *f = strtok(NULL, " \r\n");
      if (!a || !b || !f)
         continue;
      char *pat = unhex(a, strcmp(a, "-") ? strlen(a) : 0);
      char *sub = unhex(b, strcmp(b, "-") ? strlen(b) : 0);
      struct u_ere re;
      int c = u_ere_comp(&re, pat, atoi(f));
      int e = -1;
      if (c == U_ERE_OK) {
         e = u_ere_exec(&re, sub);
         u_ere_free(&re);
      }
      printf("%d %d\n", c, e);
      free(pat);
      free(sub);
   }
   return 0;
}
