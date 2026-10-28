/* SPDX-License-Identifier: MIT
 *
 * A small matcher for POSIX extended regular expressions (u_ere.h).
 *
 * u_ere_comp parses the pattern into a tree, then writes the tree as a program of a Thompson NFA.
 * u_ere_exec runs the program over the subject with a Pike VM: each instruction is in a thread list at most
 * once per subject position, so the time is linear in the subject length and a pattern cannot loop.
 */

#include "u_ere.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Limits for a pattern: tree nodes, group depth, a bound value and program instructions. */
#define U_ERE_MAX_NODES 1024
#define U_ERE_MAX_DEPTH 64
#define U_ERE_MAX_BOUND 255
#define U_ERE_MAX_PROG  16384

enum node_type {
   N_EMPTY,
   N_CHAR,
   N_ANY,
   N_CLASS,
   N_BOL,
   N_EOL,
   N_CAT,
   N_ALT,
   N_REPEAT,
};

struct node {
   enum node_type type;
   uint8_t c;
   int left, right; /* N_CAT, N_ALT; N_REPEAT uses left */
   int min, max;    /* N_REPEAT; max < 0 is no upper bound */
   uint32_t set[8]; /* N_CLASS */
};

enum inst_op {
   I_CHAR,
   I_ANY,
   I_CLASS,
   I_BOL,
   I_EOL,
   I_SPLIT,
   I_JMP,
   I_MATCH,
};

struct u_ere_inst {
   uint8_t op;
   uint8_t c;
   int x, y;
   uint32_t set[8];
};

struct parser {
   const unsigned char *p;
   struct node *nodes;
   int count;
   int depth;
   bool icase;
   int error;
};

static int
new_node(struct parser *ps, enum node_type type)
{
   if (ps->count >= U_ERE_MAX_NODES) {
      ps->error = U_ERE_ESPACE;
      return 0;
   }
   struct node *n = &ps->nodes[ps->count];
   memset(n, 0, sizeof(*n));
   n->type = type;
   return ps->count++;
}

static void
set_add(uint32_t *set, unsigned c)
{
   set[c >> 5] |= 1u << (c & 31);
}

static bool
set_has(const uint32_t *set, unsigned c)
{
   return set[c >> 5] & (1u << (c & 31));
}

static void
set_add_icase(uint32_t *set, unsigned c, bool icase)
{
   set_add(set, c);
   if (icase && isalpha(c)) {
      set_add(set, (unsigned char)tolower(c));
      set_add(set, (unsigned char)toupper(c));
   }
}

/* A literal character. With U_ERE_ICASE, a letter becomes a class of both cases. */
static int
literal(struct parser *ps, unsigned char c)
{
   if (ps->icase && isalpha(c)) {
      int n = new_node(ps, N_CLASS);
      if (!ps->error)
         set_add_icase(ps->nodes[n].set, c, true);
      return n;
   }
   int n = new_node(ps, N_CHAR);
   if (!ps->error)
      ps->nodes[n].c = c;
   return n;
}

static const struct {
   const char *name;
   int (*test)(int);
} char_classes[] = {
   {"alnum", isalnum}, {"alpha", isalpha}, {"blank", isblank}, {"cntrl", iscntrl},
   {"digit", isdigit}, {"graph", isgraph}, {"lower", islower}, {"print", isprint},
   {"punct", ispunct}, {"space", isspace}, {"upper", isupper}, {"xdigit", isxdigit},
};

/* A bracket expression; ps->p is after the '['. */
static int
bracket(struct parser *ps)
{
   int n = new_node(ps, N_CLASS);
   if (ps->error)
      return 0;
   uint32_t set[8] = {0};
   bool negate = false, first = true;

   if (*ps->p == '^') {
      negate = true;
      ps->p++;
   }
   for (;;) {
      unsigned char c = *ps->p;
      if (!c) {
         ps->error = U_ERE_BADPAT;
         return 0;
      }
      if (c == ']' && !first) {
         ps->p++;
         break;
      }
      first = false;
      if (c == '[' && (ps->p[1] == '.' || ps->p[1] == '=')) {
         ps->error = U_ERE_BADPAT;
         return 0;
      }
      if (c == '[' && ps->p[1] == ':') {
         const char *name = (const char *)ps->p + 2;
         const char *end = strstr(name, ":]");
         unsigned i;
         if (!end) {
            ps->error = U_ERE_BADPAT;
            return 0;
         }
         for (i = 0; i < sizeof(char_classes) / sizeof(char_classes[0]); i++) {
            if (strlen(char_classes[i].name) == (size_t)(end - name) &&
                !strncmp(char_classes[i].name, name, end - name))
               break;
         }
         if (i == sizeof(char_classes) / sizeof(char_classes[0])) {
            ps->error = U_ERE_BADPAT;
            return 0;
         }
         for (unsigned ch = 1; ch < 128; ch++) {
            if (char_classes[i].test(ch))
               set_add_icase(set, ch, ps->icase);
         }
         ps->p = (const unsigned char *)end + 2;
         continue;
      }
      ps->p++;
      if (*ps->p == '-' && ps->p[1] && ps->p[1] != ']') {
         unsigned char hi = ps->p[1];
         if (hi == '[' || hi < c) {
            ps->error = U_ERE_BADPAT;
            return 0;
         }
         for (unsigned ch = c; ch <= hi; ch++)
            set_add_icase(set, ch, ps->icase);
         ps->p += 2;
      } else {
         set_add_icase(set, c, ps->icase);
      }
   }
   if (negate) {
      for (unsigned i = 0; i < 8; i++)
         set[i] = ~set[i];
   }
   /* The subject ends at NUL: no class matches it. */
   set[0] &= ~1u;
   memcpy(ps->nodes[n].set, set, sizeof(set));
   return n;
}

static int alternation(struct parser *ps);

static int
atom(struct parser *ps)
{
   unsigned char c = *ps->p;

   switch (c) {
   case '(': {
      if (++ps->depth > U_ERE_MAX_DEPTH) {
         ps->error = U_ERE_ESPACE;
         return 0;
      }
      ps->p++;
      int n = alternation(ps);
      if (ps->error)
         return 0;
      if (*ps->p != ')') {
         ps->error = U_ERE_BADPAT;
         return 0;
      }
      ps->p++;
      ps->depth--;
      return n;
   }
   case '[':
      ps->p++;
      return bracket(ps);
   case '.':
      ps->p++;
      return new_node(ps, N_ANY);
   case '^':
      ps->p++;
      return new_node(ps, N_BOL);
   case '$':
      ps->p++;
      return new_node(ps, N_EOL);
   case '\\':
      c = ps->p[1];
      /* No back references, and no GNU escapes such as \w, \b or \<. */
      if (!c || isalnum(c) || c == '<' || c == '>' || c == '`' || c == '\'') {
         ps->error = U_ERE_BADPAT;
         return 0;
      }
      ps->p += 2;
      return literal(ps, c);
   case '*':
   case '+':
   case '?':
   case '{':
      /* A bound with nothing to repeat. */
      ps->error = U_ERE_BADPAT;
      return 0;
   default:
      ps->p++;
      return literal(ps, c);
   }
}

static int
bound_number(struct parser *ps)
{
   int v = 0;
   if (!isdigit(*ps->p))
      return -1;
   while (isdigit(*ps->p)) {
      v = v * 10 + (*ps->p++ - '0');
      if (v > U_ERE_MAX_BOUND)
         return -2;
   }
   return v;
}

static int
piece(struct parser *ps)
{
   int n = atom(ps);

   while (!ps->error) {
      int min, max;
      unsigned char c = *ps->p;

      if (c == '*') {
         min = 0, max = -1;
         ps->p++;
      } else if (c == '+') {
         min = 1, max = -1;
         ps->p++;
      } else if (c == '?') {
         min = 0, max = 1;
         ps->p++;
      } else if (c == '{') {
         ps->p++;
         min = bound_number(ps);
         max = min;
         if (min >= 0 && *ps->p == ',') {
            ps->p++;
            max = isdigit(*ps->p) ? bound_number(ps) : -1;
            if (max == -2 || (max >= 0 && max < min))
               min = -1;
         }
         if (min < 0 || *ps->p != '}') {
            ps->error = min == -2 || max == -2 ? U_ERE_ESPACE : U_ERE_BADPAT;
            return 0;
         }
         ps->p++;
      } else {
         break;
      }
      enum node_type t = ps->nodes[n].type;
      if (t == N_BOL || t == N_EOL) {
         ps->error = U_ERE_BADPAT;
         return 0;
      }
      int r = new_node(ps, N_REPEAT);
      if (ps->error)
         return 0;
      ps->nodes[r].left = n;
      ps->nodes[r].min = min;
      ps->nodes[r].max = max;
      n = r;
   }
   return n;
}

static int
concatenation(struct parser *ps)
{
   int n = new_node(ps, N_EMPTY);

   while (!ps->error && *ps->p && *ps->p != '|' && *ps->p != ')') {
      int p = piece(ps);
      if (ps->error)
         return 0;
      if (ps->nodes[n].type == N_EMPTY) {
         n = p;
      } else {
         int cat = new_node(ps, N_CAT);
         if (ps->error)
            return 0;
         ps->nodes[cat].left = n;
         ps->nodes[cat].right = p;
         n = cat;
      }
   }
   return n;
}

static int
alternation(struct parser *ps)
{
   int n = concatenation(ps);

   while (!ps->error && *ps->p == '|') {
      ps->p++;
      int r = concatenation(ps);
      if (ps->error)
         return 0;
      int alt = new_node(ps, N_ALT);
      if (ps->error)
         return 0;
      ps->nodes[alt].left = n;
      ps->nodes[alt].right = r;
      n = alt;
   }
   return n;
}

/* The program size of a tree, or more than U_ERE_MAX_PROG when it is too large. */
static uint64_t
prog_size(const struct node *nodes, int n)
{
   const struct node *nd = &nodes[n];
   uint64_t c;

   switch (nd->type) {
   case N_EMPTY:
      return 0;
   case N_CAT:
      return prog_size(nodes, nd->left) + prog_size(nodes, nd->right);
   case N_ALT:
      return 2 + prog_size(nodes, nd->left) + prog_size(nodes, nd->right);
   case N_REPEAT:
      c = prog_size(nodes, nd->left);
      if (c > U_ERE_MAX_PROG)
         return c;
      return nd->min * c + (nd->max < 0 ? c + 2 : (uint64_t)(nd->max - nd->min) * (c + 1));
   default:
      return 1;
   }
}

static int
emit(const struct node *nodes, int n, struct u_ere_inst *prog, int pc)
{
   const struct node *nd = &nodes[n];
   struct u_ere_inst *i = &prog[pc];

   switch (nd->type) {
   case N_EMPTY:
      return pc;
   case N_CHAR:
      i->op = I_CHAR;
      i->c = nd->c;
      return pc + 1;
   case N_ANY:
      i->op = I_ANY;
      return pc + 1;
   case N_CLASS:
      i->op = I_CLASS;
      memcpy(i->set, nd->set, sizeof(i->set));
      return pc + 1;
   case N_BOL:
      i->op = I_BOL;
      return pc + 1;
   case N_EOL:
      i->op = I_EOL;
      return pc + 1;
   case N_CAT:
      pc = emit(nodes, nd->left, prog, pc);
      return emit(nodes, nd->right, prog, pc);
   case N_ALT: {
      int split = pc, jmp;
      prog[split].op = I_SPLIT;
      prog[split].x = pc + 1;
      jmp = emit(nodes, nd->left, prog, pc + 1);
      prog[jmp].op = I_JMP;
      prog[split].y = jmp + 1;
      pc = emit(nodes, nd->right, prog, jmp + 1);
      prog[jmp].x = pc;
      return pc;
   }
   case N_REPEAT: {
      for (int k = 0; k < nd->min; k++)
         pc = emit(nodes, nd->left, prog, pc);
      if (nd->max < 0) {
         int loop = pc;
         prog[loop].op = I_SPLIT;
         prog[loop].x = loop + 1;
         pc = emit(nodes, nd->left, prog, loop + 1);
         prog[pc].op = I_JMP;
         prog[pc].x = loop;
         pc++;
         prog[loop].y = pc;
         return pc;
      }
      /* max - min optional copies, each one a SPLIT and the child. Each SPLIT can skip to the end. */
      int first = pc, copy = (int)prog_size(nodes, nd->left) + 1;
      for (int k = nd->min; k < nd->max; k++) {
         prog[pc].op = I_SPLIT;
         prog[pc].x = pc + 1;
         pc = emit(nodes, nd->left, prog, pc + 1);
      }
      for (int s = first; s < pc; s += copy)
         prog[s].y = pc;
      return pc;
   }
   }
   return pc;
}

int
u_ere_comp(struct u_ere *re, const char *pattern, int cflags)
{
   struct parser ps = {
      .p = (const unsigned char *)pattern,
      .icase = cflags & U_ERE_ICASE,
   };

   memset(re, 0, sizeof(*re));
   if (!pattern)
      return U_ERE_BADPAT;
   ps.nodes = malloc(sizeof(struct node) * U_ERE_MAX_NODES);
   if (!ps.nodes)
      return U_ERE_ESPACE;

   int root = alternation(&ps);
   if (!ps.error && *ps.p)
      ps.error = U_ERE_BADPAT; /* an unmatched ')' */
   uint64_t size = ps.error ? 0 : prog_size(ps.nodes, root) + 1;
   if (!ps.error && size > U_ERE_MAX_PROG)
      ps.error = U_ERE_ESPACE;
   if (!ps.error) {
      re->prog = calloc(size, sizeof(struct u_ere_inst));
      if (!re->prog)
         ps.error = U_ERE_ESPACE;
   }
   if (ps.error) {
      free(ps.nodes);
      return ps.error;
   }
   int end = emit(ps.nodes, root, re->prog, 0);
   re->prog[end].op = I_MATCH;
   re->len = end + 1;
   re->icase = ps.icase;
   free(ps.nodes);
   return U_ERE_OK;
}

struct vm {
   const struct u_ere_inst *prog;
   const unsigned char *s;
   unsigned *mark;
   int *stack;
   unsigned gen;
   bool matched;
};

/* Adds pc and every instruction that it reaches without input at position pos to list. */
static void
add_thread(struct vm *vm, int *list, int *count, int pc, size_t pos)
{
   int sp = 0;

   vm->stack[sp++] = pc;
   while (sp) {
      pc = vm->stack[--sp];
      if (vm->mark[pc] == vm->gen)
         continue;
      vm->mark[pc] = vm->gen;

      const struct u_ere_inst *i = &vm->prog[pc];
      switch (i->op) {
      case I_JMP:
         vm->stack[sp++] = i->x;
         break;
      case I_SPLIT:
         vm->stack[sp++] = i->y;
         vm->stack[sp++] = i->x;
         break;
      case I_BOL:
         if (pos == 0)
            vm->stack[sp++] = pc + 1;
         break;
      case I_EOL:
         if (!vm->s[pos])
            vm->stack[sp++] = pc + 1;
         break;
      case I_MATCH:
         vm->matched = true;
         break;
      default:
         list[(*count)++] = pc;
         break;
      }
   }
}

int
u_ere_exec(const struct u_ere *re, const char *subject)
{
   if (!re->prog)
      return U_ERE_NOMATCH;

   /* A failed allocation is no match: a caller takes every other result as a match. */
   unsigned *mark = calloc(re->len, sizeof(unsigned));
   int *buf = malloc(sizeof(int) * (re->len * 4 + 1));
   if (!mark || !buf) {
      free(mark);
      free(buf);
      return U_ERE_NOMATCH;
   }

   struct vm vm = {
      .prog = re->prog,
      .s = (const unsigned char *)(subject ? subject : ""),
      .mark = mark,
      .stack = buf + 2 * re->len, /* each instruction pushes at most two */
      .gen = 1,
   };
   int *clist = buf, *nlist = buf + re->len;
   int ccount = 0, ncount;

   for (size_t pos = 0;; pos++) {
      /* A match can start at every position. */
      add_thread(&vm, clist, &ccount, 0, pos);
      if (vm.matched || !vm.s[pos])
         break;

      unsigned char c = vm.s[pos];
      vm.gen++;
      ncount = 0;
      for (int t = 0; t < ccount; t++) {
         const struct u_ere_inst *i = &vm.prog[clist[t]];
         bool ok = (i->op == I_CHAR && i->c == c) || i->op == I_ANY ||
                   (i->op == I_CLASS && set_has(i->set, c));
         if (ok)
            add_thread(&vm, nlist, &ncount, clist[t] + 1, pos + 1);
      }
      if (vm.matched)
         break;
      int *tmp = clist;
      clist = nlist;
      nlist = tmp;
      ccount = ncount;
   }

   free(mark);
   free(buf);
   return vm.matched ? U_ERE_OK : U_ERE_NOMATCH;
}

void
u_ere_free(struct u_ere *re)
{
   free(re->prog);
   re->prog = NULL;
   re->len = 0;
}
