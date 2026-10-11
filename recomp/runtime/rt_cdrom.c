/* CD-ROM controller (the "CDC" behind 0x1F801800-3), enough of it for
 * PSY-Q's libcd / libds: command + parameter FIFO, first / second responses
 * as INT3 / INT2 (INT5 for errors), sectors as INT1 with the data FIFO
 * loaded on request. Sectors come from plat_read_sector (a raw 2352-byte
 * image). XA-ADPCM audio sectors are skipped while the controller is in
 * XA mode (they would go to the SPU). Timing is in emulated cycles. */
#include <string.h>

#include "recomp_rt.h"
#include "rt_internal.h"
#include "rt_platform.h"

#define ACK_DELAY 0x4000u           /* first response */
#define SECOND_DELAY 0x20000u       /* second response (Init, Pause, SeekL...) */
#define SEEK_DELAY 0x10000u         /* ReadN until the first sector */
#define SECTOR_1X (CPU_HZ / 75u)

typedef struct {
    uint64_t at;
    uint8_t irq; /* 1..5 */
    uint8_t n;
    uint8_t resp[16];
} Ev;

static uint8_t index_reg, ie, iflag;
static uint8_t param[16], nparam;
static uint8_t resp[16], nresp, resp_pos;
static Ev q[4];
static int nq;
static int busy;

static uint8_t mode, stat = 0x02;
static uint32_t setloc_lba, cur_lba;
static int setloc_pending, reading, playing;
static uint64_t next_sector;
static uint8_t sector[2352];
static uint8_t dfifo[2352];
static uint32_t dfifo_len, dfifo_pos;
static int sector_ready;

static uint8_t bcd(uint32_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static uint32_t unbcd(uint8_t v) { return (v >> 4) * 10u + (v & 15u); }

static void push(uint64_t at, uint8_t irq, const uint8_t* r, int n) {
    if (nq >= 4) {
        RT_LOG("cd: event queue full");
        return;
    }
    Ev* e = &q[nq++];
    e->at = at;
    e->irq = irq;
    e->n = (uint8_t)n;
    memcpy(e->resp, r, (size_t)n);
}
static void ack(const uint8_t* r, int n) { push(g_now + ACK_DELAY, 3, r, n); }
static void ack_stat(void) { ack(&stat, 1); }
static void second_stat(uint32_t delay) {
    uint8_t s = stat;
    push(g_now + ACK_DELAY + delay, 2, &s, 1);
}
static void error(uint8_t code) {
    uint8_t r[2] = {(uint8_t)(stat | 1), code};
    push(g_now + ACK_DELAY, 5, r, 2);
}

static void deliver(Ev* e) {
    RT_TRACE("cd: INT%d (ie %02X) resp %02X", e->irq, ie, e->resp[0]);
    iflag = e->irq;
    memcpy(resp, e->resp, e->n);
    nresp = e->n;
    resp_pos = 0;
    if (iflag & ie) hw_irq(IRQ_CDROM);
}

static void command(uint8_t c) {
    RT_TRACE("cd: command %02X (%d params: %02X %02X %02X)", c, nparam, param[0], param[1], param[2]);
    switch (c) {
    case 0x01: /* Getstat */
        ack_stat();
        break;
    case 0x02: /* Setloc */
        setloc_lba = ((unbcd(param[0]) * 60 + unbcd(param[1])) * 75 + unbcd(param[2])) - 150;
        setloc_pending = 1;
        ack_stat();
        break;
    case 0x03: /* Play (CD-DA): silent */
        playing = 1;
        reading = 0;
        stat = 0x82;
        ack_stat();
        break;
    case 0x06: /* ReadN */
    case 0x1B: /* ReadS */
        if (setloc_pending) cur_lba = setloc_lba;
        setloc_pending = 0;
        reading = 1;
        playing = 0;
        stat = 0x22;
        ack_stat();
        next_sector = g_now + ACK_DELAY + SEEK_DELAY;
        break;
    case 0x07: /* MotorOn */
        stat = 0x02;
        ack_stat();
        second_stat(SECOND_DELAY);
        break;
    case 0x08: /* Stop */
        reading = playing = 0;
        stat = 0x02;
        ack_stat();
        stat = 0x00;
        second_stat(SECOND_DELAY);
        break;
    case 0x09: /* Pause */
        ack_stat();
        reading = playing = 0;
        stat = 0x02;
        second_stat(SECOND_DELAY);
        break;
    case 0x0A: /* Init */
        mode = 0;
        reading = playing = 0;
        stat = 0x02;
        ack_stat();
        second_stat(SECOND_DELAY);
        break;
    case 0x0B: case 0x0C: /* Mute / Demute */
    case 0x0D:            /* Setfilter */
        ack_stat();
        break;
    case 0x0E: /* Setmode */
        mode = param[0];
        ack_stat();
        break;
    case 0x0F: { /* Getparam */
        uint8_t r[5] = {stat, mode, 0, 0, 0};
        ack(r, 5);
        break;
    }
    case 0x10: { /* GetlocL: the header of the last sector read */
        ack(sector + 12, 8);
        break;
    }
    case 0x11: { /* GetlocP */
        uint32_t l = cur_lba + 150, rel = cur_lba;
        uint8_t r[8] = {1, 1, bcd(rel / 4500), bcd(rel / 75 % 60), bcd(rel % 75),
                        bcd(l / 4500), bcd(l / 75 % 60), bcd(l % 75)};
        ack(r, 8);
        break;
    }
    case 0x12: /* SetSession */
        ack_stat();
        second_stat(SECOND_DELAY);
        break;
    case 0x13: { /* GetTN */
        uint8_t r[3] = {stat, 1, 1};
        ack(r, 3);
        break;
    }
    case 0x14: { /* GetTD */
        uint32_t l = param[0] == 0 ? plat_disc_sectors() + 150 : 150;
        uint8_t r[3] = {stat, bcd(l / 4500), bcd(l / 75 % 60)};
        ack(r, 3);
        break;
    }
    case 0x15: case 0x16: /* SeekL / SeekP */
        if (setloc_pending) cur_lba = setloc_lba;
        setloc_pending = 0;
        reading = 0;
        ack_stat();
        second_stat(SECOND_DELAY);
        break;
    case 0x19: /* Test */
        if (param[0] == 0x20) {
            static const uint8_t v[4] = {0x94, 0x09, 0x19, 0xC0};
            ack(v, 4);
        } else {
            ack_stat();
        }
        break;
    case 0x1A: { /* GetID: a licensed NTSC-U disc */
        static const uint8_t id[8] = {0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A'};
        ack_stat();
        push(g_now + ACK_DELAY + SECOND_DELAY, 2, id, 8);
        break;
    }
    case 0x1C: /* Reset */
        ack_stat();
        break;
    case 0x1E: /* ReadTOC */
        ack_stat();
        second_stat(SECOND_DELAY * 4);
        break;
    default:
        RT_LOG("cd: command %02X not emulated", c);
        error(0x40);
        break;
    }
    nparam = 0;
}

static void read_sector(void) {
    if (plat_read_sector(cur_lba, sector)) {
        memset(sector, 0, sizeof sector);
        RT_LOG("cd: read error at %u", (unsigned)cur_lba);
    }
    cur_lba++;
}

void cd_reset(void) {
    index_reg = iflag = 0;
    ie = 0x1F; /* the BIOS leaves every CD interrupt enabled; libcd relies on it */
    nparam = nresp = resp_pos = 0;
    nq = 0;
    busy = 0;
    mode = 0;
    stat = 0x02;
    reading = playing = 0;
    dfifo_len = dfifo_pos = 0;
    sector_ready = 0;
}

void cd_update(void) {
    /* responses wait until the previous interrupt was acknowledged */
    if (nq && iflag == 0 && g_now >= q[0].at) {
        Ev e = q[0];
        memmove(q, q + 1, sizeof(Ev) * (size_t)(nq - 1));
        nq--;
        busy = nq > 0;
        deliver(&e);
    }
    if (reading && g_now >= next_sector) {
        uint32_t period = (mode & 0x80) ? SECTOR_1X / 2 : SECTOR_1X;
        next_sector += period;
        if (next_sector < g_now) next_sector = g_now + period;
        if (iflag != 0 || nq) return; /* the game is late: hold the sector */
        read_sector();
        /* XA-ADPCM mode: audio sectors go to the SPU, not to the game */
        if ((mode & 0x40) && (sector[18] & 0x04)) return;
        sector_ready = 1;
        uint8_t s = stat;
        Ev e = {0, 1, 1, {0}};
        e.resp[0] = s;
        deliver(&e);
    }
}

uint8_t cd_read8(uint32_t reg) {
    switch (reg) {
    case 0: {
        uint8_t s = index_reg & 3;
        if (nparam == 0) s |= 0x08;   /* parameter FIFO empty */
        if (nparam < 16) s |= 0x10;   /* parameter FIFO not full */
        if (resp_pos < nresp) s |= 0x20;
        if (dfifo_pos < dfifo_len) s |= 0x40;
        if (busy) s |= 0x80;
        return s;
    }
    case 1:
        if (resp_pos < nresp) return resp[resp_pos++];
        return 0;
    case 2:
        if (dfifo_pos < dfifo_len) return dfifo[dfifo_pos++];
        return 0;
    case 3:
        if (index_reg & 1) return (uint8_t)(0xE0 | iflag);
        return (uint8_t)(0xE0 | ie);
    }
    return 0;
}

void cd_write8(uint32_t reg, uint8_t v) {
    RT_TRACE("cd: write reg%u.%u = %02X  [%s]", (unsigned)reg, index_reg, v, rt_where());
    if (reg == 0) {
        index_reg = v & 3;
        return;
    }
    switch ((reg << 4) | index_reg) {
    case 0x10: /* command */
        busy = 1;
        command(v);
        break;
    case 0x20: /* parameter */
        if (nparam < 16) param[nparam++] = v;
        break;
    case 0x21: /* interrupt enable */
        ie = v & 0x1F;
        break;
    case 0x30: /* request: bit 7 = load the sector into the data FIFO */
        if (v & 0x80) {
            if (sector_ready) {
                if (mode & 0x20) {
                    memcpy(dfifo, sector + 12, 2340);
                    dfifo_len = 2340;
                } else {
                    memcpy(dfifo, sector + 24, 2048);
                    dfifo_len = 2048;
                }
                dfifo_pos = 0;
                sector_ready = 0;
            }
        } else {
            dfifo_len = dfifo_pos = 0;
        }
        break;
    case 0x31: /* interrupt flag: write 1s to acknowledge */
        iflag &= (uint8_t)~(v & 0x1F);
        if (v & 0x40) nparam = 0;
        if (iflag == 0) {
            nresp = resp_pos = 0;
            cd_update();
        }
        break;
    default: /* volumes, sound map */
        break;
    }
}

void cd_dma(uint32_t dst, uint32_t words) {
    for (uint32_t i = 0; i < words; i++) {
        uint32_t w = 0;
        for (int b = 0; b < 4; b++) {
            uint8_t x = dfifo_pos < dfifo_len ? dfifo[dfifo_pos++] : 0;
            w |= (uint32_t)x << (8 * b);
        }
        SW(0x80000000u | (dst + 4 * i), w);
    }
}
