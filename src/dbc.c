//
// Copyright (C) 2019 Assured Information Security, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <dbc.h>
#include <endpoint.h>
#include <erst.h>
#include <stdio.h>
#include <string.h>
#include <sys.h>
#include <trb.h>
#include <trb_event.h>
#include <trb_link.h>
#include <trb_normal.h>
#include <trb_ring.h>
#include <xhc.h>

#define __cachealign __attribute__((aligned(64)))
#define __pagealign __attribute__((aligned(4096)))

/**
 * Info context strings. Each string is UTF-16LE encoded
 */

/* clang-format off */
/* String 0 descriptor */
#define STR0_LEN 6
static const char str0[STR0_LEN] = {
    STR0_LEN, /* bLength */
    3,        /* bDescriptorType */
    9, 0,     /* English */
    4, 0      /* United States */
};

/* Manufacturer string descriptor */
#define MFR_LEN 8
static const char mfr[MFR_LEN] = {
    MFR_LEN, /* bLength */
    3,       /* bDescriptorType */
    'A', 0, 'I', 0, 'S', 0
};

/* Product string descriptor */
#define PROD_LEN 32
static const char prod[PROD_LEN] = {
    PROD_LEN, /* bLength */
    3,        /* bDescriptorType */
    'x', 0, 'H', 0, 'C', 0, 'I', 0, ' ', 0,
    'D', 0, 'b', 0, 'C', 0, ' ', 0,
    'D', 0, 'r', 0, 'i', 0, 'v', 0, 'e', 0, 'r', 0
};
/* clang-format on */

static struct dbc g_dbc __cachealign;
static struct dbc_ctx g_ctx __cachealign;
static struct erst_segment g_erst[NR_SEGS] __cachealign;

static struct trb g_etrb[TRB_PER_PAGE] __pagealign;
static struct trb g_otrb[TRB_PER_PAGE] __pagealign;
static struct trb g_itrb[TRB_PER_PAGE] __pagealign;

static struct trb_ring g_ering;
static struct trb_ring g_oring;
static struct trb_ring g_iring;

static void trb_dump(struct trb *trb)
{
    switch (trb_type(trb)) {
    case 1:
        trb_norm_dump(trb);
        return;
    case 6:
        trb_link_dump(trb);
        return;
    case 32:
        trb_te_dump(trb);
        return;
    case 34:
        trb_psce_dump(trb);
        return;
    default:
        printf("unknown TRB type: %d\n", trb_type(trb));
        return;
    }
}

static inline void *dbc_alloc(unsigned long long size)
{
    return sys_alloc_aligned(64, size);
}

static inline void dbc_free(void *ptr)
{
    sys_free(ptr);
}

static void init_info(unsigned int *info)
{
    unsigned long long *sda = (unsigned long long *)info;
    unsigned long long *mfa = (unsigned long long *)(&info[2]);
    unsigned long long *pfa = (unsigned long long *)(&info[4]);

    *sda = sys_virt_to_phys(str0);
    *mfa = sys_virt_to_phys(mfr);
    *pfa = sys_virt_to_phys(prod);

    info[8] = (PROD_LEN << 16) | (MFR_LEN << 8) | STR0_LEN;
}

void dbc_dump_regs(struct dbc_reg *reg)
{
    printf("DbC registers:\n");

    printf("    - id: 0x%x\n", reg->id);
    printf("    - db: 0x%x\n", reg->db);
    printf("    - erstsz: 0x%x\n", reg->erstsz);
    printf("    - erstba: 0x%llx\n", reg->erstba);
    printf("    - erdp: 0x%llx\n", reg->erdp);
    printf("    - ctrl: 0x%x\n", reg->ctrl);
    printf("    - st: 0x%x\n", reg->st);
    printf("    - portsc: 0x%x\n", reg->portsc);
    printf("    - cp: 0x%llx\n", reg->cp);
    printf("    - ddi1: 0x%x\n", reg->ddi1);
    printf("    - ddi2: 0x%x\n", reg->ddi2);
}

/* See section 7.6.4.1 for explanation of the initialization sequence */
int dbc_init()
{
    memset(&g_dbc, 0, sizeof(g_dbc));

    /* Registers */
    struct dbc_reg *reg = xhc_find_dbc_base();
    if (!reg) {
        return 0;
    }
    g_dbc.regs = reg;

    int erstmax = (reg->id & 0x1F0000) >> 16;
    if (NR_SEGS > (1 << erstmax)) {
        return 0;
    }

    /* TRB rings */
    init_consumer_ring(&g_ering, g_etrb);
    init_producer_ring(&g_oring, g_otrb);
    init_producer_ring(&g_iring, g_itrb);

    g_dbc.ering = &g_ering;
    g_dbc.oring = &g_oring;
    g_dbc.iring = &g_iring;

    /* Event ring segment table */
    memset(&g_erst, 0, sizeof(g_erst));
    unsigned long long erdp = sys_virt_to_phys(g_ering.trb);
    if (!erdp) {
        return 0;
    }

    g_erst[0].base = erdp;
    g_erst[0].nr_trb = SEG_PER_RING * PAGE_PER_SEG * TRB_PER_PAGE;
    g_dbc.erst = g_erst;

    /* Info and endpoint context */
    memset(&g_ctx, 0, sizeof(g_ctx));

    unsigned int max_burst = (reg->ctrl & 0xFF0000) >> 16;
    unsigned long long out = sys_virt_to_phys(g_oring.trb);
    unsigned long long in = sys_virt_to_phys(g_iring.trb);

    init_endpoint(g_ctx.ep_out, max_burst, bulk_out, out);
    init_endpoint(g_ctx.ep_in, max_burst, bulk_in, in);
    init_info(g_ctx.info);

    g_dbc.ctx = &g_ctx;

    /* Hardware registers */
    reg->erstsz = SEG_PER_RING;
    reg->erstba = sys_virt_to_phys(g_erst);
    reg->erdp = erdp;
    reg->cp = sys_virt_to_phys(&g_ctx);

    dbc_enable();
    return 1;
}

int dbc_is_enabled()
{
    return g_dbc.regs->ctrl & (1UL << 31);
}

void dbc_enable()
{
    g_dbc.regs->ctrl |= (1UL << 31);
}

void dbc_disable()
{
    g_dbc.regs->ctrl &= ~(1UL << 31);
}

int dbc_state()
{
    const struct dbc_reg *regs = g_dbc.regs;
    if (!regs) {
        return dbc_off;
    }

    unsigned int ctrl = regs->ctrl;
    unsigned int port = regs->portsc;

    if (ctrl & (1UL << CTRL_DCR_SHIFT)) {
        return dbc_configured;
    }

    if (!(ctrl & (1UL << CTRL_DCE_SHIFT))) {
        return dbc_off;
    }

    if (!(port & (1UL << PORTSC_CCS_SHIFT))) {
        return dbc_disconnected;
    }

    if (port & (1UL << PORTSC_PR_SHIFT)) {
        return dbc_resetting;
    }

    if (port & (1UL << PORTSC_PED_SHIFT)) {
        int pls = (port & PORTSC_PLS_MASK) >> PORTSC_PLS_SHIFT;
        if (pls == 6) {
            /* PLS inactive */
            return dbc_error;
        } else {
            /* PLS not inactive */
            return dbc_enabled;
        }
    } else {
        int pls = (port & PORTSC_PLS_MASK) >> PORTSC_PLS_SHIFT;
        if (pls == 4) {
            /* PLS disabled */
            return dbc_disabled;
        } else {
            /* PLS not inactive */
            return dbc_enabled;
        }
    }
}

void dbc_dump()
{
    printf("ST: 0x%x\n", g_dbc.regs->st);
    printf("CTRL: 0x%x\n", g_dbc.regs->ctrl);
    printf("PORTSC: 0x%x\n", g_dbc.regs->portsc);
}

struct trb *dbc_event()
{
    int erne = g_dbc.regs->st & (1UL << ST_ERNE_SHIFT);
    if (erne) {
        return g_dbc.ering->trb + g_dbc.ering->deq;
    } else {
        return NULL;
    }
}

void dbc_write(const char *data, unsigned int size)
{
    struct trb *etrb = dbc_event();
    if (etrb) {
        trb_dump(etrb);
    }
}
