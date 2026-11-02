/* SPDX-License-Identifier: MIT
 *
 * BD-288: the query sites of the BC-250 mesh DGC route (radv_bc250_dgc_query.h), run as shipped.
 *
 * A small CP model runs the programs of three sequences of a preprocess buffer (two active, one
 * inactive, as the preprocess shader leaves it: single-dword NOPs) and the WRITE_DATA bodies that
 * switch the sites. It checks the three states an execute can see:
 *
 *   - no pipeline-statistics query: the neutral buffer runs no PIPELINESTAT event and the execute
 *     adds no body;
 *   - a query: after the enable body, each active sequence runs exactly the captured program (stop,
 *     helper, start), and the inactive one only complete stop/start pairs with nothing between;
 *   - after the disable body the active sequences are byte-identical to the neutral ones and the
 *     inactive one runs nothing, so the next execute of an explicit preprocess sees no event
 *     without a query.
 *
 * It also checks the body size against the IB_SIZE field (the field that the BD-288 submission
 * overflowed with 1601736 dwords) and the scan of malformed and decoy packets.
 */
#include <stdio.h>
#include <string.h>

#include "radv_bc250_dgc_query.h"

#define PKT3 RADV_BC250_DGC_Q_PKT3
#define NOP_PAD RADV_BC250_DGC_Q_NOP_PAD
#define OP_SET_SH_REG 0x76u
#define OP_DISPATCH_DIRECT 0x15u
#define OP_DISPATCH_MESH_INDIRECT_MULTI 0xa2u
#define EVENT_CS_PARTIAL_FLUSH 7u

#define SEQS 3u
#define ACTIVE 2u     /* sequences below this index are active */
#define STRIDE_DW 64u /* bytes per sequence / 4 */
#define CODE_DW 48u   /* the program part of each sequence; the rest is data */
#define PAD 8u        /* ib_pad_dw_mask + 1 of the GFX queue */
#define SITES_PER_SEQ 2u

static const uint64_t base_va = 0x0000800012345600ull; /* a high word, so a lost upper half shows */

static int checks, failures;

static void
check(int ok, const char *what)
{
   checks++;
   if (!ok) {
      failures++;
      printf("FAIL %s\n", what);
   }
}

/* One captured sequence program as the task route records it: helper state, a helper dispatch
 * between STOP and START, a flush event that is no site, a big NOP whose payload looks like a
 * site (a decoy), the draw, then single-dword NOPs up to CODE_DW. */
static void
build_program(uint32_t *w, uint32_t seq)
{
   unsigned i = 0;
   w[i++] = PKT3(OP_SET_SH_REG, 2); w[i++] = 0x2c0; w[i++] = 0x1000 + seq; w[i++] = 0x2000 + seq;
   w[i++] = PKT3(RADV_BC250_DGC_Q_OP_EVENT_WRITE, 0); w[i++] = RADV_BC250_DGC_Q_PIPELINESTAT_STOP | (2u << 8);
   w[i++] = PKT3(OP_DISPATCH_DIRECT, 3); w[i++] = 1; w[i++] = 1; w[i++] = 1; w[i++] = 0;
   w[i++] = PKT3(RADV_BC250_DGC_Q_OP_EVENT_WRITE, 0); w[i++] = RADV_BC250_DGC_Q_PIPELINESTAT_START | (2u << 8);
   w[i++] = PKT3(RADV_BC250_DGC_Q_OP_EVENT_WRITE, 0); w[i++] = EVENT_CS_PARTIAL_FLUSH | (4u << 8);
   w[i++] = PKT3(RADV_BC250_DGC_Q_OP_NOP, 2);
   w[i++] = PKT3(RADV_BC250_DGC_Q_OP_EVENT_WRITE, 0); w[i++] = RADV_BC250_DGC_Q_PIPELINESTAT_STOP; w[i++] = 0;
   w[i++] = PKT3(OP_DISPATCH_MESH_INDIRECT_MULTI, 7);
   for (unsigned k = 0; k < 8; k++)
      w[i++] = 0x100 * seq + k;
   while (i < CODE_DW)
      w[i++] = NOP_PAD;
   for (; i < STRIDE_DW; i++)
      w[i] = 0xd0000000u + seq * 0x100 + i; /* data words: never executed */
}

/* The CP model. Runs ndw dwords at mem[at]; appends one letter per packet that does something:
 * P stop, S start, e another event, D a dispatch or draw, s state; NOPs append nothing. A
 * WRITE_DATA writes into mem when it targets the buffer (anything else is a failure).
 * Returns 0, or -1 for a malformed stream. */
static int
cp_run(uint32_t *mem, unsigned at, unsigned ndw, char *trace, unsigned cap)
{
   unsigned t = 0;
   for (unsigned i = 0; i < ndw;) {
      const uint32_t h = mem[at + i];
      if (h == NOP_PAD) { i++; continue; }
      if (h >> 30 != 3)
         return -1;
      const unsigned len = ((h >> 16) & 0x3fffu) + 2;
      if (len > ndw - i)
         return -1;
      const unsigned op = (h >> 8) & 0xffu;
      const uint32_t *p = mem + at + i;
      char c = 0;
      if (op == RADV_BC250_DGC_Q_OP_NOP) {
      } else if (op == RADV_BC250_DGC_Q_OP_EVENT_WRITE) {
         if (len != 2)
            return -1;
         c = (p[1] & 63u) == RADV_BC250_DGC_Q_PIPELINESTAT_STOP ? 'P' :
             (p[1] & 63u) == RADV_BC250_DGC_Q_PIPELINESTAT_START ? 'S' : 'e';
      } else if (op == OP_DISPATCH_DIRECT || op == OP_DISPATCH_MESH_INDIRECT_MULTI) {
         c = 'D';
      } else if (op == OP_SET_SH_REG) {
         c = 's';
      } else if (op == RADV_BC250_DGC_Q_OP_WRITE_DATA) {
         if (len < 5 || p[1] != RADV_BC250_DGC_Q_WRITE_CONTROL)
            return -1;
         const uint64_t dst = (uint64_t)p[2] | (uint64_t)p[3] << 32;
         if (dst < base_va || (dst - base_va) % 4 || (dst - base_va) / 4 + (len - 4) > SEQS * STRIDE_DW)
            return -1;
         memcpy(mem + (dst - base_va) / 4, p + 4, (len - 4) * 4);
         c = 'w';
      } else {
         return -1;
      }
      if (c && t + 1 < cap)
         trace[t++] = c;
      i += len;
   }
   trace[t] = 0;
   return 0;
}

static int
run_seq(uint32_t *mem, unsigned seq, char *trace)
{
   return cp_run(mem, seq * STRIDE_DW, CODE_DW, trace, 64);
}

/* The inactive sequence may only run complete stop/start pairs. */
static int
pairs_only(const char *t)
{
   for (; *t; t += 2)
      if (t[0] != 'P' || t[1] != 'S')
         return 0;
   return 1;
}

int
main(void)
{
   static uint32_t raw[SEQS * STRIDE_DW], snapshot[SEQS * STRIDE_DW], buffer[SEQS * STRIDE_DW], neutral[SEQS * STRIDE_DW];
   static uint32_t body[4096];
   struct radv_bc250_dgc_query_site sites[SEQS * SITES_PER_SEQ];
   char trace[64], want[64];

   for (unsigned s = 0; s < SEQS; s++)
      build_program(raw + s * STRIDE_DW, s);

   /* 1. The scan finds the stop and the start of each sequence, not the flush event and not the
    * event inside the NOP payload, with byte offsets from the start of the buffer. */
   uint64_t count = 0;
   int scan_ok = 1;
   for (unsigned s = 0; s < SEQS; s++) {
      const int64_t n = radv_bc250_dgc_query_scan(raw + s * STRIDE_DW, CODE_DW, (uint64_t)s * STRIDE_DW * 4,
                                                  sites + count, SEQS * SITES_PER_SEQ - count);
      scan_ok &= n == SITES_PER_SEQ;
      count += n > 0 ? (uint64_t)n : 0;
   }
   check(scan_ok && count == SEQS * SITES_PER_SEQ, "scan: two sites per sequence (no flush event, no NOP payload)");
   check(sites[0].offset == 4 * 4 && sites[1].offset == 11 * 4 && sites[2].offset == STRIDE_DW * 4 + 4 * 4,
         "scan: byte offsets from the start of the buffer");
   check(sites[0].header == PKT3(RADV_BC250_DGC_Q_OP_EVENT_WRITE, 0) &&
         (sites[0].event & 63u) == RADV_BC250_DGC_Q_PIPELINESTAT_STOP &&
         (sites[1].event & 63u) == RADV_BC250_DGC_Q_PIPELINESTAT_START, "scan: captured header and event words");
   check(radv_bc250_dgc_query_scan(raw, CODE_DW, 0, sites, 1) == SITES_PER_SEQ && sites[0].offset == 16,
         "scan: a short array gets the first sites, the count stays whole");
   {
      uint32_t bad[4] = {PKT3(OP_DISPATCH_DIRECT, 3), 1, 1, 1};
      check(radv_bc250_dgc_query_scan(bad, 4, 0, NULL, 0) == -1, "scan: a packet past the end is refused");
      uint32_t long_site[3] = {PKT3(RADV_BC250_DGC_Q_OP_EVENT_WRITE, 1), RADV_BC250_DGC_Q_PIPELINESTAT_START, 0};
      check(radv_bc250_dgc_query_scan(long_site, 3, 0, NULL, 0) == -1, "scan: a site that is not two dwords is refused");
      uint32_t type0[2] = {0x00001234u, NOP_PAD};
      check(radv_bc250_dgc_query_scan(type0, 2, 0, NULL, 0) == -1, "scan: a type-0 packet is refused");
   }
   /* Reference: the captured program of an active sequence. */
   check(run_seq(raw, 0, want) == 0 && strcmp(want, "sPDSeD") == 0, "model: the captured program runs sPDSeD");

   /* 2. Upload as bc250_dgc_prepare() does: every site neutral. Then the preprocess shader: active
    * sequences are copies, the program part of an inactive one is single-dword NOPs. */
   memcpy(snapshot, raw, sizeof(raw));
   for (uint64_t i = 0; i < count; i++)
      snapshot[sites[i].offset / 4] = radv_bc250_dgc_query_neutral();
   memcpy(buffer, snapshot, sizeof(snapshot));
   for (unsigned i = ACTIVE * STRIDE_DW; i < ACTIVE * STRIDE_DW + CODE_DW; i++)
      buffer[i] = NOP_PAD;
   memcpy(neutral, buffer, sizeof(buffer));
   {
      int64_t n = 0;
      for (unsigned s = 0; s < SEQS; s++)
         n += radv_bc250_dgc_query_scan(buffer + s * STRIDE_DW, CODE_DW, 0, NULL, 0);
      check(n == 0, "neutral buffer: no site is left (so the program is built before the upload)");
   }

   /* 3. No query: no body, and the buffer runs no PIPELINESTAT event. */
   check(!radv_bc250_dgc_query_switch(count, 0), "no query: the execute adds no body");
   check(!radv_bc250_dgc_query_switch(0, 1), "no site: the execute adds no body inside a query");
   check(run_seq(buffer, 0, trace) == 0 && strcmp(trace, "sDeD") == 0, "no query: an active sequence runs sDeD");
   check(run_seq(buffer, 1, trace) == 0 && strcmp(trace, "sDeD") == 0, "no query: the second active sequence runs sDeD");
   check(run_seq(buffer, ACTIVE, trace) == 0 && trace[0] == 0, "no query: the inactive sequence runs nothing");

   /* 4. A query: the enable body. */
   check(radv_bc250_dgc_query_switch(count, 1), "query: the execute switches the sites");
   for (unsigned round = 0; round < 2; round++) {
      const uint64_t dw = radv_bc250_dgc_query_body(body, sites, count, base_va, true, PAD);
      check(dw == radv_bc250_dgc_query_body_dw(count, PAD) && dw % PAD == 0 && dw >= count * 6 && dw < count * 6 + PAD,
            "enable body: the size is the writes padded to the IB size unit");
      char bt[64];
      /* The body runs after the buffer in one model memory, so its writes land in the buffer. */
      static uint32_t combined[SEQS * STRIDE_DW + 4096];
      memcpy(combined, buffer, sizeof(buffer));
      memcpy(combined + SEQS * STRIDE_DW, body, dw * 4);
      int body_ok = cp_run(combined, SEQS * STRIDE_DW, (unsigned)dw, bt, 64) == 0;
      unsigned writes = 0;
      for (const char *c = bt; *c; c++)
         writes += *c == 'w';
      body_ok &= writes == count && strspn(bt, "w") == strlen(bt);
      check(body_ok, "enable body: only WRITE_DATA into the buffer and single-dword NOPs");
      memcpy(buffer, combined, sizeof(buffer));
      check(memcmp(buffer, raw, ACTIVE * STRIDE_DW * 4) == 0, "enable: the active sequences are the captured programs, byte for byte");
      check(run_seq(buffer, 0, trace) == 0 && strcmp(trace, want) == 0, "enable: an active sequence runs the captured sPDSeD");
      check(run_seq(buffer, ACTIVE, trace) == 0 && trace[0] && pairs_only(trace),
            "enable: the inactive sequence runs only complete stop/start pairs");

      /* 5. After the chains: the disable body. */
      const uint64_t dw2 = radv_bc250_dgc_query_body(body, sites, count, base_va, false, PAD);
      memcpy(combined, buffer, sizeof(buffer));
      memcpy(combined + SEQS * STRIDE_DW, body, dw2 * 4);
      check(dw2 == dw && cp_run(combined, SEQS * STRIDE_DW, (unsigned)dw2, bt, 64) == 0, "disable body: same size, valid");
      memcpy(buffer, combined, sizeof(buffer));
      /* The inactive sequence keeps a two-dword NOP at each site where the shader wrote two
       * single-dword NOPs: other bytes, the same commands (none). */
      check(memcmp(buffer, neutral, ACTIVE * STRIDE_DW * 4) == 0, "disable: the active sequences are neutral again, byte for byte");
      check(memcmp(buffer + ACTIVE * STRIDE_DW + CODE_DW, neutral + ACTIVE * STRIDE_DW + CODE_DW,
                   (STRIDE_DW - CODE_DW) * 4) == 0, "disable: the data of the inactive sequence is untouched");
      check(run_seq(buffer, 0, trace) == 0 && strcmp(trace, "sDeD") == 0, "disable: the next execute without a query runs sDeD");
      check(run_seq(buffer, ACTIVE, trace) == 0 && trace[0] == 0, "disable: the inactive sequence runs nothing again");
   }

   /* 6. The IB_SIZE field. A body never exceeds it, and one more site would. The BD-288 executes
    * (at most 64 sequences of 2049 sites) need one body each way; 130 sequences in one call two. */
   const uint64_t per = radv_bc250_dgc_query_sites_per_body(PAD);
   check(radv_bc250_dgc_query_body_dw(per, PAD) <= RADV_BC250_DGC_Q_IB_SIZE_MAX, "a full body fits the IB_SIZE field");
   check(radv_bc250_dgc_query_body_dw(per + 1, PAD) > RADV_BC250_DGC_Q_IB_SIZE_MAX, "one site more would not fit");
   check(64ull * 2049 <= per && 130ull * 2049 > per, "the BD-288 executes: one body each way for 64 sequences");
   {
      static struct radv_bc250_dgc_query_site one[1];
      one[0] = (struct radv_bc250_dgc_query_site){0x10, PKT3(RADV_BC250_DGC_Q_OP_EVENT_WRITE, 0), RADV_BC250_DGC_Q_PIPELINESTAT_START};
      const uint64_t dw = radv_bc250_dgc_query_body(body, one, 1, base_va, true, PAD);
      check(dw == PAD && body[2] == (uint32_t)(base_va + 0x10) && body[3] == (uint32_t)((base_va + 0x10) >> 32) &&
            body[6] == NOP_PAD && body[7] == NOP_PAD, "one site: the 64-bit address and the NOP padding");
   }

   printf("radv_bc250_dgc_query_test: checks=%d, failures=%d\n", checks, failures);
   return failures ? 1 : 0;
}
