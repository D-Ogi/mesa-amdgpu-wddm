/*
 * Copyright (c) 2026 D-Ogi
 * SPDX-License-Identifier: MIT
 */

/*
 * How zink_context_destroy() gives a context's batch states back to the
 * screen's free list, as list operations only, so that a host test
 * (bc250_batch_list_test.c, run_bc250_batch_list_test.ps1) runs the same code.
 * The lists are zink's intrusive singly linked lists: a node's link is the
 * pointer at byte offset `link` (offsetof(struct zink_batch_state, next)).
 *
 * The defect this guards against (BC-250 hang recovery, lab trial D1 of KMD
 * 0.7.216.17, 2026-10-07). A submit that fails marks the batch state lost,
 * and flush_batch() then does not start a new batch: ctx->bs stays the state
 * that zink_end_batch() already appended to ctx->batch_states. Upstream's
 * destroy appended ctx->bs to the screen's list a second time, which made
 * bs->next == bs, and the walk to the end of the list never ended. DWM, which
 * destroys its device after a device loss to make a new one, spun in that walk
 * at 100 % of a core with the desktop black. A current state that one of the
 * context's own lists already holds is therefore not appended again.
 */
#ifndef ZINK_BC250_BATCH_LIST_H
#define ZINK_BC250_BATCH_LIST_H

#include <stdbool.h>
#include <stddef.h>

static inline void **
bc250_batch_link(void *node, size_t link)
{
   return (void **)((char *)node + link);
}

static inline bool
bc250_batch_listed(void *list, size_t link, const void *node)
{
   for (; list; list = *bc250_batch_link(list, link)) {
      if (list == node)
         return true;
   }
   return false;
}

/* Appends `list` (may be NULL) to the list *head ... *last and moves *last to its end. */
static inline void
bc250_batch_append_list(void **head, void **last, void *list, size_t link)
{
   if (list) {
      if (*head)
         *bc250_batch_link(*last, link) = list;
      else
         *head = *last = list;
   }
   while (*last && *bc250_batch_link(*last, link))
      *last = *bc250_batch_link(*last, link);
}

/* The screen's free list *head ... *last takes the context's submitted states,
 * then its free states, then its current state unless one of those two lists
 * already holds it. */
static inline void
bc250_batch_return_states(void **head, void **last, void *batch_states, void *free_states, void *current,
                          size_t link)
{
   const bool current_listed = current && (bc250_batch_listed(batch_states, link, current) ||
                                           bc250_batch_listed(free_states, link, current));

   bc250_batch_append_list(head, last, batch_states, link);
   bc250_batch_append_list(head, last, free_states, link);
   if (current && !current_listed) {
      *bc250_batch_link(current, link) = NULL;
      bc250_batch_append_list(head, last, current, link);
   }
}

#endif /* ZINK_BC250_BATCH_LIST_H */
