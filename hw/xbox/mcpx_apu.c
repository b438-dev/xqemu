/*
 * QEMU MCPX Audio Processing Unit implementation
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/i386/pc.h"
#include "hw/pci/pci.h"
#include "cpu.h"
#include "hw/xbox/dsp/dsp.h"
#include <math.h>

#define NUM_SAMPLES_PER_FRAME 32
#define NUM_MIXBINS 32

#include "hw/xbox/mcpx_apu.h"

#define NV_PAPU_ISTS                                     0x00001000
#   define NV_PAPU_ISTS_GINTSTS                               (1 << 0)
#   define NV_PAPU_ISTS_FETINTSTS                             (1 << 4)
#define NV_PAPU_IEN                                      0x00001004
#define NV_PAPU_FECTL                                    0x00001100
#   define NV_PAPU_FECTL_FEMETHMODE                         0x000000E0
#       define NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING            0x00000000
#       define NV_PAPU_FECTL_FEMETHMODE_HALTED                  0x00000080
#       define NV_PAPU_FECTL_FEMETHMODE_TRAPPED                 0x000000E0
#   define NV_PAPU_FECTL_FETRAPREASON                       0x00000F00
#       define NV_PAPU_FECTL_FETRAPREASON_REQUESTED             0x00000F00
#define NV_PAPU_FECV                                     0x00001110
#define NV_PAPU_FEAV                                     0x00001118
#   define NV_PAPU_FEAV_VALUE                               0x0000FFFF
#   define NV_PAPU_FEAV_LST                                 0x00030000
#define NV_PAPU_FEDECMETH                                0x00001300
#define NV_PAPU_FEDECPARAM                               0x00001304
#define NV_PAPU_FEMEMADDR                                0x00001324
#define NV_PAPU_FEMEMDATA                                0x00001334
#define NV_PAPU_FETFORCE0                                0x00001500
#define NV_PAPU_FETFORCE1                                0x00001504
#   define NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE               (1 << 15)
#define NV_PAPU_SECTL                                    0x00002000
#   define NV_PAPU_SECTL_XCNTMODE                           0x00000018
#       define NV_PAPU_SECTL_XCNTMODE_OFF                       0
#define NV_PAPU_XGSCNT                                   0x0000200C
#define NV_PAPU_VPVADDR                                  0x0000202C
#define NV_PAPU_GPSADDR                                  0x00002040
#define NV_PAPU_GPFADDR                                  0x00002044
#define NV_PAPU_EPSADDR                                  0x00002048
#define NV_PAPU_EPFADDR                                  0x0000204C
#define NV_PAPU_TVL2D                                    0x00002054
#define NV_PAPU_CVL2D                                    0x00002058
#define NV_PAPU_NVL2D                                    0x0000205C
#define NV_PAPU_TVL3D                                    0x00002060
#define NV_PAPU_CVL3D                                    0x00002064
#define NV_PAPU_NVL3D                                    0x00002068
#define NV_PAPU_TVLMP                                    0x0000206C
#define NV_PAPU_CVLMP                                    0x00002070
#define NV_PAPU_NVLMP                                    0x00002074
#define NV_PAPU_GPSMAXSGE                                0x000020D4
#define NV_PAPU_GPFMAXSGE                                0x000020D8
#define NV_PAPU_EPSMAXSGE                                0x000020DC
#define NV_PAPU_EPFMAXSGE                                0x000020E0

/* Each FIFO has the same fields */
#define NV_PAPU_GPOFBASE0                                0x00003024
#   define NV_PAPU_GPOFBASE0_VALUE                          0x00FFFF00
#define NV_PAPU_GPOFEND0                                 0x00003028
#   define NV_PAPU_GPOFEND0_VALUE                           0x00FFFF00
#define NV_PAPU_GPOFCUR0                                 0x0000302C
#   define NV_PAPU_GPOFCUR0_VALUE                           0x00FFFFFC
#define NV_PAPU_GPOFBASE1                                0x00003034
#define NV_PAPU_GPOFEND1                                 0x00003038
#define NV_PAPU_GPOFCUR1                                 0x0000303C
#define NV_PAPU_GPOFBASE2                                0x00003044
#define NV_PAPU_GPOFEND2                                 0x00003048
#define NV_PAPU_GPOFCUR2                                 0x0000304C
#define NV_PAPU_GPOFBASE3                                0x00003054
#define NV_PAPU_GPOFEND3                                 0x00003058
#define NV_PAPU_GPOFCUR3                                 0x0000305C

/* Fields are same as for the 4 output FIFOs, but only 2 input FIFOs */
#define NV_PAPU_GPIFBASE0                                0x00003064
#define NV_PAPU_GPIFEND0                                 0x00003068
#define NV_PAPU_GPIFCUR0                                 0x0000306C
#define NV_PAPU_GPIFBASE1                                0x00003074
#define NV_PAPU_GPIFEND1                                 0x00003078
#define NV_PAPU_GPIFCUR1                                 0x0000307C

/* Fields, strides and count is same as for GP FIFOs */
#define NV_PAPU_EPOFBASE0                                0x00004024
#define NV_PAPU_EPOFEND0                                 0x00004028
#define NV_PAPU_EPOFCUR0                                 0x0000402C
#define NV_PAPU_EPIFBASE0                                0x00004064
#define NV_PAPU_EPIFEND0                                 0x00004068
#define NV_PAPU_EPIFCUR0                                 0x0000406C

#define NV_PAPU_GPXMEM                                   0x00000000
#define NV_PAPU_GPMIXBUF                                 0x00005000
#define NV_PAPU_GPYMEM                                   0x00006000
#define NV_PAPU_GPPMEM                                   0x0000A000
#define NV_PAPU_GPRST                                    0x0000FFFC
#define NV_PAPU_GPRST_GPRST                                 (1 << 0)
#define NV_PAPU_GPRST_GPDSPRST                              (1 << 1)
#define NV_PAPU_GPRST_GPNMI                                 (1 << 2)
#define NV_PAPU_GPRST_GPABORT                               (1 << 3)

#define NV_PAPU_EPXMEM                                   0x00000000
#define NV_PAPU_EPYMEM                                   0x00006000
#define NV_PAPU_EPPMEM                                   0x0000A000
#define NV_PAPU_EPRST                                    0x0000FFFC

static const struct {
    hwaddr top, current, next;
} voice_list_regs[] = {
    {NV_PAPU_TVL2D, NV_PAPU_CVL2D, NV_PAPU_NVL2D}, //2D
    {NV_PAPU_TVL3D, NV_PAPU_CVL3D, NV_PAPU_NVL3D}, //3D
    {NV_PAPU_TVLMP, NV_PAPU_CVLMP, NV_PAPU_NVLMP}, //MP
};


/* audio processor object / front-end messages */
#define NV1BA0_PIO_FREE                                  0x00000010
#define NV1BA0_PIO_SET_ANTECEDENT_VOICE                  0x00000120
#   define NV1BA0_PIO_SET_ANTECEDENT_VOICE_HANDLE           0x0000FFFF
#   define NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST             0x00030000
#       define NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_INHERIT     0
#       define NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_2D_TOP      1
#       define NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_3D_TOP      2
#       define NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_MP_TOP      3
#define NV1BA0_PIO_VOICE_ON                              0x00000124
#   define NV1BA0_PIO_VOICE_ON_HANDLE                       0x0000FFFF
#define NV1BA0_PIO_VOICE_OFF                             0x00000128
#   define NV1BA0_PIO_VOICE_OFF_HANDLE                      0x0000FFFF
#define NV1BA0_PIO_VOICE_PAUSE                           0x00000140
#   define NV1BA0_PIO_VOICE_PAUSE_HANDLE                    0x0000FFFF
#   define NV1BA0_PIO_VOICE_PAUSE_ACTION                    (1 << 18)
#define NV1BA0_PIO_SET_CURRENT_VOICE                     0x000002F8

#define SE2FE_IDLE_VOICE                                 0x00008000


/* voice structure */
#define NV_PAVS_SIZE                                     0x00000080
#define NV_PAVS_VOICE_PAR_STATE                          0x00000054
#   define NV_PAVS_VOICE_PAR_STATE_PAUSED                   (1 << 18)
#   define NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE             (1 << 21)
#define NV_PAVS_VOICE_TAR_PITCH_LINK                     0x0000007c
#   define NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE   0x0000FFFF


#define GP_DSP_MIXBUF_BASE 0x001400

#define GP_OUTPUT_FIFO_COUNT  4
#define GP_INPUT_FIFO_COUNT   2

#define EP_OUTPUT_FIFO_COUNT  4
#define EP_INPUT_FIFO_COUNT   2

#define MCPX_HW_MAX_VOICES 256

#define GET_MASK(v, mask) (((v) & (mask)) >> ctz32(mask))

#define SET_MASK(v, mask, val)                                       \
    do {                                                             \
        (v) &= ~(mask);                                              \
        (v) |= ((val) << ctz32(mask)) & (mask);                      \
    } while (0)

// #define MCPX_DEBUG
#ifdef MCPX_DEBUG
# define MCPX_DPRINTF(format, ...)       printf(format, ## __VA_ARGS__)
#else
# define MCPX_DPRINTF(format, ...)       do { } while (0)
#endif

/* More debug functionality */
#define GENERATE_MIXBIN_BEEP      0

typedef struct MCPXAPUState {
    PCIDevice dev;

    MemoryRegion *ram;
    uint8_t *ram_ptr;

    MemoryRegion mmio;

    /* Setup Engine */
    struct {
        QEMUTimer *frame_timer;
    } se;

    /* Voice Processor */
    struct {
        MemoryRegion mmio;
    } vp;

    /* Global Processor */
    struct {
        MemoryRegion mmio;
        DSPState *dsp;
        uint32_t regs[0x10000];
    } gp;

    /* Encode Processor */
    struct {
        MemoryRegion mmio;
        DSPState *dsp;
        uint32_t regs[0x10000];
    } ep;

    uint32_t regs[0x20000];

} MCPXAPUState;

#define MCPX_APU_DEVICE(obj) \
    OBJECT_CHECK(MCPXAPUState, (obj), "mcpx-apu")

static uint32_t voice_get_mask(MCPXAPUState *d,
                               unsigned int voice_handle,
                               hwaddr offset,
                               uint32_t mask)
{
    assert(voice_handle < 0xFFFF);
    hwaddr voice = d->regs[NV_PAPU_VPVADDR]
                    + voice_handle * NV_PAVS_SIZE;
    return (ldl_le_phys(&address_space_memory, voice + offset) & mask)
              >> ctz32(mask);
}
static void voice_set_mask(MCPXAPUState *d,
                           unsigned int voice_handle,
                           hwaddr offset,
                           uint32_t mask,
                           uint32_t val)
{
    assert(voice_handle < 0xFFFF);
    hwaddr voice = d->regs[NV_PAPU_VPVADDR]
                    + voice_handle * NV_PAVS_SIZE;
    uint32_t v = ldl_le_phys(&address_space_memory, voice + offset) & ~mask;
    stl_le_phys(&address_space_memory, voice + offset,
                v | ((val << ctz32(mask)) & mask));
}

static void update_irq(MCPXAPUState *d)
{
    if ((d->regs[NV_PAPU_IEN] & NV_PAPU_ISTS_GINTSTS)
        && ((d->regs[NV_PAPU_ISTS] & ~NV_PAPU_ISTS_GINTSTS)
              & d->regs[NV_PAPU_IEN])) {

        d->regs[NV_PAPU_ISTS] |= NV_PAPU_ISTS_GINTSTS;
        MCPX_DPRINTF("mcpx irq raise\n");
        pci_irq_assert(&d->dev);
    } else {
        d->regs[NV_PAPU_ISTS] &= ~NV_PAPU_ISTS_GINTSTS;
        MCPX_DPRINTF("mcpx irq lower\n");
        pci_irq_deassert(&d->dev);
    }
}

static uint64_t mcpx_apu_read(void *opaque,
                              hwaddr addr, unsigned int size)
{
    MCPXAPUState *d = opaque;

    uint64_t r = 0;
    switch (addr) {
    case NV_PAPU_XGSCNT:
        r = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 100; //???
        break;
    default:
        if (addr < 0x20000) {
            r = d->regs[addr];
        }
        break;
    }

    MCPX_DPRINTF("mcpx apu: read [0x%llx] -> 0x%llx\n", addr, r);
    return r;
}

static void mcpx_apu_write(void *opaque, hwaddr addr,
                           uint64_t val, unsigned int size)
{
    MCPXAPUState *d = opaque;

    MCPX_DPRINTF("mcpx apu: [0x%llx] = 0x%llx\n", addr, val);

    switch (addr) {
    case NV_PAPU_ISTS:
        /* the bits of the interrupts to clear are wrtten */
        d->regs[NV_PAPU_ISTS] &= ~val;
        update_irq(d);
        break;
    case NV_PAPU_SECTL:
        if (((val & NV_PAPU_SECTL_XCNTMODE) >> 3)
              == NV_PAPU_SECTL_XCNTMODE_OFF) {
            timer_del(d->se.frame_timer);
        } else {
            timer_mod(d->se.frame_timer,
                qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10);
        }
        d->regs[addr] = val;
        break;
    case NV_PAPU_FEMEMDATA:
        /* 'magic write'
         * This value is expected to be written to FEMEMADDR on completion of
         * something to do with notifies. Just do it now :/ */
        stl_le_phys(&address_space_memory, d->regs[NV_PAPU_FEMEMADDR], val);
        d->regs[addr] = val;
        break;
    default:
        if (addr < 0x20000) {
            d->regs[addr] = val;
        }
        break;
    }
}

static const MemoryRegionOps mcpx_apu_mmio_ops = {
    .read = mcpx_apu_read,
    .write = mcpx_apu_write,
};

static void fe_method(MCPXAPUState *d,
                      uint32_t method, uint32_t argument)
{
    MCPX_DPRINTF("mcpx fe_method 0x%x 0x%x\n", method, argument);

    //assert((d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE) == 0);

    d->regs[NV_PAPU_FEDECMETH] = method;
    d->regs[NV_PAPU_FEDECPARAM] = argument;
    unsigned int selected_handle, list;
    switch (method) {
    case NV1BA0_PIO_SET_ANTECEDENT_VOICE:
        d->regs[NV_PAPU_FEAV] = argument;
        break;
    case NV1BA0_PIO_VOICE_ON:
        selected_handle = argument & NV1BA0_PIO_VOICE_ON_HANDLE;
        list = GET_MASK(d->regs[NV_PAPU_FEAV], NV_PAPU_FEAV_LST);
        if (list != NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_INHERIT) {
            /* voice is added to the top of the selected list */
            unsigned int top_reg = voice_list_regs[list - 1].top;
            voice_set_mask(d, selected_handle,
                NV_PAVS_VOICE_TAR_PITCH_LINK,
                NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                d->regs[top_reg]);
            d->regs[top_reg] = selected_handle;
        } else {
            unsigned int antecedent_voice =
                GET_MASK(d->regs[NV_PAPU_FEAV], NV_PAPU_FEAV_VALUE);
            /* voice is added after the antecedent voice */
            assert(antecedent_voice != 0xFFFF);

            uint32_t next_handle = voice_get_mask(d, antecedent_voice,
                NV_PAVS_VOICE_TAR_PITCH_LINK,
                NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
            voice_set_mask(d, selected_handle,
                NV_PAVS_VOICE_TAR_PITCH_LINK,
                NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                next_handle);
            voice_set_mask(d, antecedent_voice,
                NV_PAVS_VOICE_TAR_PITCH_LINK,
                NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                selected_handle);
        }
        voice_set_mask(d, selected_handle,
                NV_PAVS_VOICE_PAR_STATE,
                NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE,
                1);
        break;
    case NV1BA0_PIO_VOICE_OFF:
        voice_set_mask(d, argument & NV1BA0_PIO_VOICE_OFF_HANDLE,
                NV_PAVS_VOICE_PAR_STATE,
                NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE,
                0);
        break;
    case NV1BA0_PIO_VOICE_PAUSE:
        voice_set_mask(d, argument & NV1BA0_PIO_VOICE_PAUSE_HANDLE,
                NV_PAVS_VOICE_PAR_STATE,
                NV_PAVS_VOICE_PAR_STATE_PAUSED,
                (argument & NV1BA0_PIO_VOICE_PAUSE_ACTION) != 0);
        break;
    case NV1BA0_PIO_SET_CURRENT_VOICE:
        d->regs[NV_PAPU_FECV] = argument;
        break;
    case SE2FE_IDLE_VOICE:
        if (d->regs[NV_PAPU_FETFORCE1] & NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE) {

            d->regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FEMETHMODE;
            d->regs[NV_PAPU_FECTL] |= NV_PAPU_FECTL_FEMETHMODE_TRAPPED;

            d->regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FETRAPREASON;
            d->regs[NV_PAPU_FECTL] |= NV_PAPU_FECTL_FETRAPREASON_REQUESTED;

            d->regs[NV_PAPU_ISTS] |= NV_PAPU_ISTS_FETINTSTS;
            update_irq(d);
        } else {
            assert(false);
        }
        break;
    default:
        assert(false);
        break;
    }
}

static uint64_t vp_read(void *opaque,
                        hwaddr addr, unsigned int size)
{
    MCPX_DPRINTF("mcpx apu VP: read [0x%llx]\n", addr);
    switch (addr) {
    case NV1BA0_PIO_FREE:
        /* we don't simulate the queue for now,
         * pretend to always be empty */
        return 0x80;
    default:
        break;
    }
    return 0;
}

static void vp_write(void *opaque, hwaddr addr,
                     uint64_t val, unsigned int size)
{
    MCPXAPUState *d = opaque;

    MCPX_DPRINTF("mcpx apu VP: [0x%llx] = 0x%llx\n", addr, val);

    switch (addr) {
    case NV1BA0_PIO_SET_ANTECEDENT_VOICE:
    case NV1BA0_PIO_VOICE_ON:
    case NV1BA0_PIO_VOICE_OFF:
    case NV1BA0_PIO_VOICE_PAUSE:
    case NV1BA0_PIO_SET_CURRENT_VOICE:
        /* TODO: these should instead be queueing up fe commands */
        fe_method(d, addr, val);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps vp_ops = {
    .read = vp_read,
    .write = vp_write,
};

static void scatter_gather_rw(MCPXAPUState *d,
                              hwaddr sge_base, unsigned int max_sge,
                              uint8_t *ptr, uint32_t addr, size_t len,
                              bool dir)
{
    unsigned int page_entry = addr / TARGET_PAGE_SIZE;
    unsigned int offset_in_page = addr % TARGET_PAGE_SIZE;
    unsigned int bytes_to_copy = TARGET_PAGE_SIZE - offset_in_page;

    while (len > 0) {
        assert(page_entry <= max_sge);

        uint32_t prd_address = ldl_le_phys(&address_space_memory,
                                           sge_base + page_entry * 8 + 0);
        /* uint32_t prd_control = ldl_le_phys(&address_space_memory,
                                            sge_base + page_entry * 8 + 4); */

        hwaddr paddr = prd_address + offset_in_page;

        if (bytes_to_copy > len) {
            bytes_to_copy = len;
        }

        assert(paddr + bytes_to_copy < memory_region_size(d->ram));

        if (dir) {
            memcpy(&d->ram_ptr[paddr], ptr, bytes_to_copy);
            memory_region_set_dirty(d->ram, paddr, bytes_to_copy);
        } else {
            memcpy(ptr, &d->ram_ptr[paddr], bytes_to_copy);
        }

        ptr += bytes_to_copy;
        len -= bytes_to_copy;

        /* After the first iteration, we are page aligned */
        page_entry += 1;
        bytes_to_copy = TARGET_PAGE_SIZE;
        offset_in_page = 0;
    }
}

static void gp_scratch_rw(void *opaque,
                          uint8_t *ptr,
                          uint32_t addr,
                          size_t len,
                          bool dir)
{
    MCPXAPUState *d = opaque;
    scatter_gather_rw(d, d->regs[NV_PAPU_GPSADDR], d->regs[NV_PAPU_GPSMAXSGE],
                      ptr, addr, len, dir);
}

static void ep_scratch_rw(void *opaque,
                          uint8_t *ptr,
                          uint32_t addr,
                          size_t len,
                          bool dir)
{
    MCPXAPUState *d = opaque;
    scatter_gather_rw(d, d->regs[NV_PAPU_EPSADDR], d->regs[NV_PAPU_EPSMAXSGE],
                      ptr, addr, len, dir);
}

static uint32_t circular_scatter_gather_rw(MCPXAPUState *d,
                                           hwaddr sge_base,
                                           unsigned int max_sge,
                                           uint8_t *ptr,
                                           uint32_t base, uint32_t end,
                                           uint32_t cur,
                                           size_t len,
                                           bool dir)
{
    while (len > 0) {
        unsigned int bytes_to_copy = end - cur;

        if (bytes_to_copy > len) {
            bytes_to_copy = len;
        }

        MCPX_DPRINTF("circular scatter gather %s in range 0x%x - 0x%x at 0x%x of length 0x%x / 0x%lx bytes\n",
            dir ? "write" : "read", base, end, cur, bytes_to_copy, len);

        assert((cur >= base) && ((cur + bytes_to_copy) <= end));
        scatter_gather_rw(d, sge_base, max_sge, ptr, cur, bytes_to_copy, dir);

        ptr += bytes_to_copy;
        len -= bytes_to_copy;

        /* After the first iteration we might have to wrap */
        cur += bytes_to_copy;
        if (cur >= end) {
            assert(cur == end);
            cur = base;
        }
    }

    return cur;
}

static void gp_fifo_rw(void *opaque, uint8_t *ptr,
                       unsigned int index, size_t len,
                       bool dir)
{
    MCPXAPUState *d = opaque;
    uint32_t base;
    uint32_t end;
    hwaddr cur_reg;
    if (dir) {
        assert(index < GP_OUTPUT_FIFO_COUNT);
        base = GET_MASK(d->regs[NV_PAPU_GPOFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_GPOFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_GPOFCUR0 + 0x10 * index;
    } else {
        assert(index < GP_INPUT_FIFO_COUNT);
        base = GET_MASK(d->regs[NV_PAPU_GPIFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_GPIFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_GPIFCUR0 + 0x10 * index;
    }

    uint32_t cur = GET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE);

    /* DSP hangs if current >= end; but forces current >= base */
    assert(cur < end);
    if (cur < base) {
        cur = base;
    }

    cur = circular_scatter_gather_rw(d,
        d->regs[NV_PAPU_GPFADDR], d->regs[NV_PAPU_GPFMAXSGE],
        ptr, base, end, cur, len, dir);

    SET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE, cur);
}

static void ep_fifo_rw(void *opaque, uint8_t *ptr,
                       unsigned int index, size_t len,
                       bool dir)
{
    MCPXAPUState *d = opaque;
    uint32_t base;
    uint32_t end;
    hwaddr cur_reg;
    if (dir) {
        assert(index < EP_OUTPUT_FIFO_COUNT);
        base = GET_MASK(d->regs[NV_PAPU_EPOFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_EPOFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_EPOFCUR0 + 0x10 * index;
    } else {
        assert(index < EP_INPUT_FIFO_COUNT);
        base = GET_MASK(d->regs[NV_PAPU_EPIFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_EPIFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_EPIFCUR0 + 0x10 * index;
    }

    uint32_t cur = GET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE);

    /* DSP hangs if current >= end; but forces current >= base */
    assert(cur < end);
    if (cur < base) {
        cur = base;
    }

    cur = circular_scatter_gather_rw(d,
        d->regs[NV_PAPU_EPFADDR], d->regs[NV_PAPU_EPFMAXSGE],
        ptr, base, end, cur, len, dir);

    SET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE, cur);
}

static void proc_rst_write(DSPState *dsp, uint32_t oldval, uint32_t val)
{
    if (!(val & NV_PAPU_GPRST_GPRST) || !(val & NV_PAPU_GPRST_GPDSPRST)) {
        dsp_reset(dsp);
    } else if (
        (!(oldval & NV_PAPU_GPRST_GPRST) || !(oldval & NV_PAPU_GPRST_GPDSPRST))
        && ((val & NV_PAPU_GPRST_GPRST) && (val & NV_PAPU_GPRST_GPDSPRST))) {
        dsp_bootstrap(dsp);
    }
}

/* Global Processor - programmable DSP */
static uint64_t gp_read(void *opaque,
                        hwaddr addr,
                        unsigned int size)
{
    MCPXAPUState *d = opaque;

    assert(size == 4);
    assert(addr % 4 == 0);

    uint64_t r = 0;
    switch (addr) {
    case NV_PAPU_GPXMEM ... NV_PAPU_GPXMEM + 0x1000 * 4 - 1: {
        uint32_t xaddr = (addr - NV_PAPU_GPXMEM) / 4;
        r = dsp_read_memory(d->gp.dsp, 'X', xaddr);
        break;
    }
    case NV_PAPU_GPMIXBUF ... NV_PAPU_GPMIXBUF + 0x400 * 4 - 1: {
        uint32_t xaddr = (addr - NV_PAPU_GPMIXBUF) / 4;
        r = dsp_read_memory(d->gp.dsp, 'X', GP_DSP_MIXBUF_BASE + xaddr);
        break;
    }
    case NV_PAPU_GPYMEM ... NV_PAPU_GPYMEM + 0x800 * 4 - 1: {
        uint32_t yaddr = (addr - NV_PAPU_GPYMEM) / 4;
        r = dsp_read_memory(d->gp.dsp, 'Y', yaddr);
        break;
    }
    case NV_PAPU_GPPMEM ... NV_PAPU_GPPMEM + 0x1000 * 4 - 1: {
        uint32_t paddr = (addr - NV_PAPU_GPPMEM) / 4;
        r = dsp_read_memory(d->gp.dsp, 'P', paddr);
        break;
    }
    default:
        r = d->gp.regs[addr];
        break;
    }
    MCPX_DPRINTF("mcpx apu GP: read [0x%llx] -> 0x%llx\n", addr, r);
    return r;
}

static void gp_write(void *opaque, hwaddr addr,
                     uint64_t val, unsigned int size)
{
    MCPXAPUState *d = opaque;

    assert(size == 4);
    assert(addr % 4 == 0);

    MCPX_DPRINTF("mcpx apu GP: [0x%llx] = 0x%llx\n", addr, val);

    switch (addr) {
    case NV_PAPU_GPXMEM ... NV_PAPU_GPXMEM + 0x1000 * 4 - 1: {
        uint32_t xaddr = (addr - NV_PAPU_GPXMEM) / 4;
        dsp_write_memory(d->gp.dsp, 'X', xaddr, val);
        break;
    }
    case NV_PAPU_GPMIXBUF ... NV_PAPU_GPMIXBUF + 0x400 * 4 - 1: {
        uint32_t xaddr = (addr - NV_PAPU_GPMIXBUF) / 4;
        dsp_write_memory(d->gp.dsp, 'X', GP_DSP_MIXBUF_BASE + xaddr, val);
        break;
    }
    case NV_PAPU_GPYMEM ... NV_PAPU_GPYMEM + 0x800 * 4 - 1: {
        uint32_t yaddr = (addr - NV_PAPU_GPYMEM) / 4;
        dsp_write_memory(d->gp.dsp, 'Y', yaddr, val);
        break;
    }
    case NV_PAPU_GPPMEM ... NV_PAPU_GPPMEM + 0x1000 * 4 - 1: {
        uint32_t paddr = (addr - NV_PAPU_GPPMEM) / 4;
        dsp_write_memory(d->gp.dsp, 'P', paddr, val);
        break;
    }
    case NV_PAPU_GPRST:
        proc_rst_write(d->gp.dsp, d->gp.regs[NV_PAPU_GPRST], val);
        d->gp.regs[NV_PAPU_GPRST] = val;
        break;
    default:
        d->gp.regs[addr] = val;
        break;
    }
}

static const MemoryRegionOps gp_ops = {
    .read = gp_read,
    .write = gp_write,
};

/* Encode Processor - encoding DSP */
static uint64_t ep_read(void *opaque,
                        hwaddr addr,
                        unsigned int size)
{
    MCPXAPUState *d = opaque;

    assert(size == 4);
    assert(addr % 4 == 0);

    uint64_t r = 0;
    switch (addr) {
    case NV_PAPU_EPXMEM ... NV_PAPU_EPXMEM + 0xC00 * 4 - 1: {
        uint32_t xaddr = (addr - NV_PAPU_EPXMEM) / 4;
        r = dsp_read_memory(d->ep.dsp, 'X', xaddr);
        break;
    }
    case NV_PAPU_EPYMEM ... NV_PAPU_EPYMEM + 0x100 * 4 - 1: {
        uint32_t yaddr = (addr - NV_PAPU_EPYMEM) / 4;
        r = dsp_read_memory(d->ep.dsp, 'Y', yaddr);
        break;
    }
    case NV_PAPU_EPPMEM ... NV_PAPU_EPPMEM + 0x1000 * 4 - 1: {
        uint32_t paddr = (addr - NV_PAPU_EPPMEM) / 4;
        r = dsp_read_memory(d->ep.dsp, 'P', paddr);
        break;
    }
    default:
        r = d->ep.regs[addr];
        break;
    }
    MCPX_DPRINTF("mcpx apu EP: read [0x%llx] -> 0x%llx\n", addr, r);
    return r;
}

static void ep_write(void *opaque, hwaddr addr,
                     uint64_t val, unsigned int size)
{
    MCPXAPUState *d = opaque;

    assert(size == 4);
    assert(addr % 4 == 0);

    MCPX_DPRINTF("mcpx apu EP: [0x%llx] = 0x%llx\n", addr, val);

    switch (addr) {
    case NV_PAPU_EPXMEM ... NV_PAPU_EPXMEM + 0xC00 * 4 - 1: {
        uint32_t xaddr = (addr - NV_PAPU_EPXMEM) / 4;
        dsp_write_memory(d->ep.dsp, 'X', xaddr, val);
        break;
    }
    case NV_PAPU_EPYMEM ... NV_PAPU_EPYMEM + 0x100 * 4 - 1: {
        uint32_t yaddr = (addr - NV_PAPU_EPYMEM) / 4;
        dsp_write_memory(d->ep.dsp, 'Y', yaddr, val);
        break;
    }
    case NV_PAPU_EPPMEM ... NV_PAPU_EPPMEM + 0x1000 * 4 - 1: {
        uint32_t paddr = (addr - NV_PAPU_EPPMEM) / 4;
        dsp_write_memory(d->ep.dsp, 'P', paddr, val);
        break;
    }
    case NV_PAPU_EPRST:
        proc_rst_write(d->ep.dsp, d->ep.regs[NV_PAPU_EPRST], val);
        d->ep.regs[NV_PAPU_EPRST] = val;
        break;
    default:
        d->ep.regs[addr] = val;
        break;
    }
}

static const MemoryRegionOps ep_ops = {
    .read = ep_read,
    .write = ep_write,
};

static void process_voice(MCPXAPUState *d,
                          int32_t mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME],
                          uint32_t voice)
{
    /* FIXME: Implement */
}

/* This routine must run at 1500 Hz */
/* TODO: this should be on a thread so it waits on the voice lock */
static void se_frame(void *opaque)
{
    MCPXAPUState *d = opaque;
    int mixbin;
    int sample;

    timer_mod(d->se.frame_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10);
    MCPX_DPRINTF("mcpx frame ping\n");

    /* Buffer for all mixbins for this frame */
    int32_t mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME] = { 0 };

    /* Process all voices, mixing each into the affected MIXBINs */
    int list;
    for (list = 0; list < 3; list++) {
        hwaddr top, current, next;
        top = voice_list_regs[list].top;
        current = voice_list_regs[list].current;
        next = voice_list_regs[list].next;

        d->regs[current] = d->regs[top];
        MCPX_DPRINTF("list %d current voice %d\n", list, d->regs[current]);
        while (d->regs[current] != 0xFFFF) {
            d->regs[next] = voice_get_mask(d, d->regs[current],
                NV_PAVS_VOICE_TAR_PITCH_LINK,
                NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
            if (!voice_get_mask(d, d->regs[current],
                    NV_PAVS_VOICE_PAR_STATE,
                    NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE)) {
                MCPX_DPRINTF("voice %d not active...!\n", d->regs[current]);
                fe_method(d, SE2FE_IDLE_VOICE, d->regs[current]);
            } else {
                process_voice(d, mixbins, d->regs[current]);
            }
            MCPX_DPRINTF("next voice %d\n", d->regs[next]);
            d->regs[current] = d->regs[next];
        }
    }

#if GENERATE_MIXBIN_BEEP
    /* Inject some audio to the mixbin for debugging.
     * Signal is 1500 Hz sine wave, phase shifted by mixbin number. */
    for (mixbin = 0; mixbin < NUM_MIXBINS; mixbin++) {
        for (sample = 0; sample < NUM_SAMPLES_PER_FRAME; sample++) {
            /* Avoid multiple of 1.0 / NUM_SAMPLES_PER_FRAME for phase shift,
             * or waves cancel out */
            float offset = sample / (float)NUM_SAMPLES_PER_FRAME -
                           mixbin / (float)(NUM_SAMPLES_PER_FRAME + 1);
            float wave = sinf(offset * M_PI * 2.0f);
            mixbins[mixbin][sample] += wave * 0x3FFFFF;
        }
    }
#endif

    /* Write VP results to the GP DSP MIXBUF */
    for (mixbin = 0; mixbin < NUM_MIXBINS; mixbin++) {
        for (sample = 0; sample < NUM_SAMPLES_PER_FRAME; sample++) {
            dsp_write_memory(d->gp.dsp,
                             'X', GP_DSP_MIXBUF_BASE + mixbin * 0x20 + sample,
                             mixbins[mixbin][sample] & 0xFFFFFF);
        }
    }

    /* Kickoff DSP processing */
    if ((d->gp.regs[NV_PAPU_GPRST] & NV_PAPU_GPRST_GPRST)
        && (d->gp.regs[NV_PAPU_GPRST] & NV_PAPU_GPRST_GPDSPRST)) {
        dsp_start_frame(d->gp.dsp);

        // hax
        dsp_run(d->gp.dsp, 1000);
    }
    if ((d->ep.regs[NV_PAPU_EPRST] & NV_PAPU_GPRST_GPRST)
        && (d->ep.regs[NV_PAPU_EPRST] & NV_PAPU_GPRST_GPDSPRST)) {
        dsp_start_frame(d->ep.dsp);

        // hax
        // dsp_run(d->ep.dsp, 1000);
    }
}

static void mcpx_apu_realize(PCIDevice *dev, Error **errp)
{
    MCPXAPUState *d = MCPX_APU_DEVICE(dev);

    dev->config[PCI_INTERRUPT_PIN] = 0x01;

    memory_region_init_io(&d->mmio, OBJECT(dev), &mcpx_apu_mmio_ops, d,
                          "mcpx-apu-mmio", 0x80000);

    memory_region_init_io(&d->vp.mmio, OBJECT(dev), &vp_ops, d,
                          "mcpx-apu-vp", 0x10000);
    memory_region_add_subregion(&d->mmio, 0x20000, &d->vp.mmio);

    memory_region_init_io(&d->gp.mmio, OBJECT(dev), &gp_ops, d,
                          "mcpx-apu-gp", 0x10000);
    memory_region_add_subregion(&d->mmio, 0x30000, &d->gp.mmio);

    memory_region_init_io(&d->ep.mmio, OBJECT(dev), &ep_ops, d,
                          "mcpx-apu-ep", 0x10000);
    memory_region_add_subregion(&d->mmio, 0x50000, &d->ep.mmio);

    pci_register_bar(&d->dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &d->mmio);


    d->se.frame_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, se_frame, d);
    d->gp.dsp = dsp_init(d, gp_scratch_rw, gp_fifo_rw);
    d->ep.dsp = dsp_init(d, ep_scratch_rw, ep_fifo_rw);
}

static void mcpx_apu_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->vendor_id = PCI_VENDOR_ID_NVIDIA;
    k->device_id = PCI_DEVICE_ID_NVIDIA_MCPX_APU;
    k->revision = 210;
    k->class_id = PCI_CLASS_MULTIMEDIA_AUDIO;
    k->realize = mcpx_apu_realize;

    dc->desc = "MCPX Audio Processing Unit";
}

static const TypeInfo mcpx_apu_info = {
    .name          = "mcpx-apu",
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(MCPXAPUState),
    .class_init    = mcpx_apu_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void mcpx_apu_register(void)
{
    type_register_static(&mcpx_apu_info);
}
type_init(mcpx_apu_register);

void mcpx_apu_init(PCIBus *bus, int devfn, MemoryRegion *ram)
{
    PCIDevice *dev = pci_create_simple(bus, devfn, "mcpx-apu");
    MCPXAPUState *d = MCPX_APU_DEVICE(dev);

    /* Keep pointers to system memory */
    d->ram = ram;
    d->ram_ptr = memory_region_get_ram_ptr(d->ram);
}
