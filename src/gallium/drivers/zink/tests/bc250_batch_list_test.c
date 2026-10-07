/*
 * Copyright (c) 2026 D-Ogi
 * SPDX-License-Identifier: MIT
 */

/*
 * Host test of bc250_batch_return_states() (zink_bc250_batch_list.h), the
 * list step of zink_context_destroy(). Each case builds the context's lists,
 * returns them to a screen list and walks the result with a step bound: a walk
 * that does not end within the bound is a cycle and fails the case.
 * Run by bc250_batch_list.py, which also runs the negative control.
 */
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#include "zink_bc250_batch_list.h"

struct node {
   int id;
   struct node *next;
};

#define LINK offsetof(struct node, next)
#define MAX_STEPS 64

static int failures;

static void
chain(struct node **nodes, int count)
{
   for (int i = 0; i < count; i++)
      nodes[i]->next = i + 1 < count ? nodes[i + 1] : NULL;
}

/* Walks head and returns its length, or -1 for a cycle. */
static int
walk(struct node *head, struct node **tail)
{
   int steps = 0;
   *tail = NULL;
   for (struct node *n = head; n; n = n->next) {
      if (++steps > MAX_STEPS)
         return -1;
      *tail = n;
   }
   return steps;
}

static int
count_of(struct node *head, const struct node *node)
{
   int count = 0, steps = 0;
   for (struct node *n = head; n && steps < MAX_STEPS; n = n->next, steps++)
      count += n == node;
   return count;
}

static void
check(const char *name, struct node *head, struct node *last, int want_length, const struct node *current)
{
   struct node *tail;
   int length = walk(head, &tail);
   int ok = 1;

   if (length < 0) {
      printf("FAIL %s: the screen list has a cycle (no end within %d steps)\n", name, MAX_STEPS);
      failures++;
      return;
   }
   if (length != want_length) {
      printf("FAIL %s: length %d, expected %d\n", name, length, want_length);
      ok = 0;
   }
   if (last != tail) {
      printf("FAIL %s: last is node %d, the list ends at node %d\n", name, last ? last->id : -1,
             tail ? tail->id : -1);
      ok = 0;
   }
   if (current && count_of(head, current) != 1) {
      printf("FAIL %s: the current state is on the list %d times\n", name, count_of(head, current));
      ok = 0;
   }
   if (ok)
      printf("PASS %s: length %d\n", name, length);
   else
      failures++;
}

static void
begin(const char *name)
{
   printf("RUN  %s\n", name);
}

int
main(void)
{
   setvbuf(stdout, NULL, _IONBF, 0);
   struct node pool[16];
   for (int i = 0; i < 16; i++) {
      pool[i].id = i;
      pool[i].next = NULL;
   }

   /* Lab trial D1 shape: the lost current state is the tail of batch_states, the screen list is empty. */
   {
      begin("d1-current-in-batch-states-empty-screen");
      struct node *a = &pool[0], *b = &pool[1], *bs = &pool[2];
      struct node *batch[] = {a, b, bs};
      chain(batch, 3);
      void *head = NULL, *last = NULL;
      bc250_batch_return_states(&head, &last, a, NULL, bs, LINK);
      check("d1-current-in-batch-states-empty-screen", head, last, 3, bs);
   }

   /* The same, with states already on the screen list. */
   {
      begin("d1-current-in-batch-states-screen-has-states");
      struct node *s1 = &pool[3], *s2 = &pool[4], *a = &pool[5], *bs = &pool[6];
      struct node *screen[] = {s1, s2};
      struct node *batch[] = {a, bs};
      chain(screen, 2);
      chain(batch, 2);
      void *head = s1, *last = s2;
      bc250_batch_return_states(&head, &last, a, NULL, bs, LINK);
      check("d1-current-in-batch-states-screen-has-states", head, last, 4, bs);
   }

   /* The current state is the only submitted state. */
   {
      begin("current-only-in-batch-states");
      struct node *bs = &pool[7];
      bs->next = NULL;
      void *head = NULL, *last = NULL;
      bc250_batch_return_states(&head, &last, bs, NULL, bs, LINK);
      check("current-only-in-batch-states", head, last, 1, bs);
   }

   /* The current state is on the context's free list. */
   {
      begin("current-in-free-states");
      struct node *a = &pool[8], *f = &pool[9], *bs = &pool[10];
      struct node *free_states[] = {f, bs};
      a->next = NULL;
      chain(free_states, 2);
      void *head = NULL, *last = NULL;
      bc250_batch_return_states(&head, &last, a, f, bs, LINK);
      check("current-in-free-states", head, last, 3, bs);
   }

   /* Normal path: the current state is on no list and its link is stale. It is appended once, with a NULL link. */
   {
      begin("current-unlisted-stale-link");
      struct node *a = &pool[11], *b = &pool[12], *f = &pool[13], *cur = &pool[14];
      struct node *batch[] = {a, b};
      chain(batch, 2);
      f->next = NULL;
      cur->next = a;
      void *head = NULL, *last = NULL;
      bc250_batch_return_states(&head, &last, a, f, cur, LINK);
      check("current-unlisted-stale-link", head, last, 4, cur);
   }

   /* No current state and empty context lists. */
   {
      begin("no-current-empty-context");
      struct node *s = &pool[15];
      s->next = NULL;
      void *head = s, *last = s;
      bc250_batch_return_states(&head, &last, NULL, NULL, NULL, LINK);
      check("no-current-empty-context", head, last, 1, NULL);
   }

   printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
   return failures ? 1 : 0;
}
