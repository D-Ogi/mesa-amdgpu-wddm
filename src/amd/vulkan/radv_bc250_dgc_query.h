/* SPDX-License-Identifier: MIT
 *
 * BD-288. The pipeline-statistics query sites of a program of the BC-250 mesh DGC route
 * (radv_bc250_dgc.c), and the IB2 bodies that switch them.
 *
 * The capture records every helper dispatch of a sequence between a PIPELINESTAT_STOP and a
 * PIPELINESTAT_START event (two-dword EVENT_WRITE packets), so that a query of the application
 * never counts the helpers. Whether the events must run is known only at execute time, so:
 *
 *   - the program is uploaded with every site as a two-dword NOP: the NOP header, then the event
 *     word as its payload (radv_bc250_dgc_query_neutral());
 *   - an execute outside a native pipeline-statistics scope changes no site and adds no command;
 *   - an execute inside one calls an IB2 body of WRITE_DATA packets that write the EVENT_WRITE
 *     headers before its chains, and a second body that writes the NOPs again after them, so the
 *     buffer is neutral for the next execute of an explicit preprocess.
 *
 * Both words of a site are written: preprocessing fills an inactive sequence with single-dword
 * NOPs, and the two words make a complete packet there (an EVENT_WRITE pair with no draw between,
 * or a two-dword NOP).
 *
 * The bodies are IB2s because the calling stream itself calls the DGC chains as IB2s, and the WDDM
 * winsys therefore copies the whole calling stream into the IB1 of the submission. Before BD-288 one
 * WRITE_DATA per site sat in that stream: 130 sequences of 2049 sites made an IB1 of 1601736 dwords,
 * above the IB_SIZE field of INDIRECT_BUFFER, and the submission failed.
 *
 * Pure functions of their arguments; nothing from Mesa is included, so a host test can run the
 * shipped code. radv_bc250_dgc.c checks the packet constants below against sid.h at compile time.
 */
#ifndef RADV_BC250_DGC_QUERY_H
#define RADV_BC250_DGC_QUERY_H

#include <stdbool.h>
#include <stdint.h>

#define RADV_BC250_DGC_Q_PKT3(op, count) ((3u << 30) | (((uint32_t)(count) & 0x3fffu) << 16) | (((uint32_t)(op) & 0xffu) << 8))
#define RADV_BC250_DGC_Q_OP_NOP          0x10u
#define RADV_BC250_DGC_Q_OP_WRITE_DATA   0x37u
#define RADV_BC250_DGC_Q_OP_EVENT_WRITE  0x46u
#define RADV_BC250_DGC_Q_NOP_PAD         0xffff1000u /* the single-dword NOP of the GFX CP */
#define RADV_BC250_DGC_Q_PIPELINESTAT_START 25u
#define RADV_BC250_DGC_Q_PIPELINESTAT_STOP  26u
/* WRITE_DATA control word: DST_SEL memory (5), WR_CONFIRM, ENGINE_SEL ME (0). */
#define RADV_BC250_DGC_Q_WRITE_CONTROL   ((5u << 8) | (1u << 20))
#define RADV_BC250_DGC_Q_IB_SIZE_MAX     0xfffffu /* the IB_SIZE field of INDIRECT_BUFFER, in dwords */
#define RADV_BC250_DGC_Q_SITE_DW         6u       /* one WRITE_DATA of two words */

struct radv_bc250_dgc_query_site {
   uint64_t offset; /* bytes from the start of the preprocess buffer */
   uint32_t header, event;
};

/* Dwords of the packet at words[0] (1 for a single-dword NOP or a type-2 packet). */
static inline unsigned
radv_bc250_dgc_query_packet_dw(uint32_t header)
{
   return header == RADV_BC250_DGC_Q_NOP_PAD || header >> 30 == 2 ? 1u : ((header >> 16) & 0x3fffu) + 2u;
}

/* Scans one sequence program of ndw dwords that starts at byte base of the preprocess buffer.
 * Writes the sites into sites[] (when not NULL; it holds at least max entries) and returns their
 * number, or -1 for a program that is not a valid packet stream or has a site that is not a
 * two-dword packet. A count above max is still returned and only the first max are written. */
static inline int64_t
radv_bc250_dgc_query_scan(const uint32_t *words, unsigned ndw, uint64_t base,
                          struct radv_bc250_dgc_query_site *sites, uint64_t max)
{
   int64_t count = 0;
   for (unsigned i = 0; i < ndw;) {
      const uint32_t header = words[i];
      const unsigned len = radv_bc250_dgc_query_packet_dw(header);
      if ((header >> 30 != 2 && header >> 30 != 3) || len > ndw - i)
         return -1;
      if (header >> 30 == 3 && ((header >> 8) & 0xffu) == RADV_BC250_DGC_Q_OP_EVENT_WRITE && len > 1 &&
          ((words[i + 1] & 63u) == RADV_BC250_DGC_Q_PIPELINESTAT_START ||
           (words[i + 1] & 63u) == RADV_BC250_DGC_Q_PIPELINESTAT_STOP)) {
         if (len != 2)
            return -1;
         if (sites && (uint64_t)count < max)
            sites[count] = (struct radv_bc250_dgc_query_site){base + (uint64_t)i * 4, header, words[i + 1]};
         count++;
      }
      i += len;
   }
   return count;
}

/* The header that a site has in the uploaded program: a two-dword NOP. */
static inline uint32_t
radv_bc250_dgc_query_neutral(void)
{
   return RADV_BC250_DGC_Q_PKT3(RADV_BC250_DGC_Q_OP_NOP, 0);
}

/* True when an execute must switch the sites: it has sites and a native pipeline-statistics
 * query is active. Otherwise the execute adds no command for them. */
static inline bool
radv_bc250_dgc_query_switch(uint64_t sites, unsigned active_stat_queries)
{
   return sites && active_stat_queries;
}

/* The most sites in one IB2 body with pad_dw (a power of two) as the IB size unit. */
static inline uint64_t
radv_bc250_dgc_query_sites_per_body(unsigned pad_dw)
{
   return (RADV_BC250_DGC_Q_IB_SIZE_MAX - pad_dw) / RADV_BC250_DGC_Q_SITE_DW;
}

/* Dwords of a body of n sites: the writes, padded with single-dword NOPs to the size unit. */
static inline uint64_t
radv_bc250_dgc_query_body_dw(uint64_t n, unsigned pad_dw)
{
   const uint64_t dw = n * RADV_BC250_DGC_Q_SITE_DW;
   return (dw + pad_dw - 1) / pad_dw * pad_dw;
}

/* Writes the body of the n sites into out (radv_bc250_dgc_query_body_dw(n, pad_dw) dwords):
 * enable writes the captured EVENT_WRITE headers, disable the neutral NOP headers. Each site is
 * at preprocess_va + its offset. Returns the dwords written. */
static inline uint64_t
radv_bc250_dgc_query_body(uint32_t *out, const struct radv_bc250_dgc_query_site *sites, uint64_t n,
                          uint64_t preprocess_va, bool enable, unsigned pad_dw)
{
   uint32_t *p = out;
   for (uint64_t i = 0; i < n; i++) {
      const uint64_t dst = preprocess_va + sites[i].offset;
      *p++ = RADV_BC250_DGC_Q_PKT3(RADV_BC250_DGC_Q_OP_WRITE_DATA, 4);
      *p++ = RADV_BC250_DGC_Q_WRITE_CONTROL;
      *p++ = (uint32_t)dst;
      *p++ = (uint32_t)(dst >> 32);
      *p++ = enable ? sites[i].header : radv_bc250_dgc_query_neutral();
      *p++ = sites[i].event;
   }
   const uint64_t total = radv_bc250_dgc_query_body_dw(n, pad_dw);
   while ((uint64_t)(p - out) < total)
      *p++ = RADV_BC250_DGC_Q_NOP_PAD;
   return total;
}

#endif
