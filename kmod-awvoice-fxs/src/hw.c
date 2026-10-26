// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
/*
 * Audio PLL, audio-subsystem clocks and the PCM block of the SoC.
 *
 * The running kernel registers none of these clocks, so the module brings them up itself, in the order
 * the hardware needs: block reset released, PLL locked, PCM root clock running, pins muxed, PCM block
 * reset and started.
 */
#include <linux/delay.h>
#include <linux/iopoll.h>

#include "awfxs.h"

#define PLL_LOCK_WAIT_US	5000
#define CLK_UPDATE_WAIT_US	5000

static void __iomem *window(unsigned long pa, size_t len)
{
	return ioremap(pa, len);
}

void awfxs_hw_unmap(struct awfxs *z)
{
	void __iomem *held[] = { z->gcc_reset, z->pll_vote, z->pll, z->glb, z->clk, z->pcm, z->bit_pin, z->frame_pin };
	size_t i;

	for (i = 0; i < ARRAY_SIZE(held); i++)
		if (held[i])
			iounmap(held[i]);
	z->gcc_reset = NULL;
	z->pll_vote = NULL;
	z->pll = NULL;
	z->glb = NULL;
	z->clk = NULL;
	z->pcm = NULL;
	z->bit_pin = z->frame_pin = NULL;
}

int awfxs_hw_map(struct awfxs *z)
{
	BUILD_BUG_ON(sizeof(struct aw_pll) != 0x40 || sizeof(struct aw_adss_glb) != 0x34 ||
		     sizeof(struct aw_adss_clk) != 0x20 || sizeof(struct aw_pcm) != 0x40);

	z->gcc_reset = window(AW_PA_GCC + AW_GCC_ADSS_RESET, sizeof(u32));
	z->pll_vote = window(AW_PA_GCC + AW_GCC_PLL_VOTE, sizeof(u32));
	z->pll = window(AW_PA_GCC + AW_GCC_AUDIO_PLL, sizeof(struct aw_pll));
	z->glb = window(AW_PA_ADSS, sizeof(struct aw_adss_glb));
	z->clk = window(AW_PA_ADSS_CLK, sizeof(struct aw_adss_clk));
	z->pcm = window(AW_PA_PCM, sizeof(struct aw_pcm));
	z->bit_pin = window(AW_PA_TLMM + AW_PIN_AUDIO_BIT * AW_TLMM_WINDOW, 2 * sizeof(u32));
	z->frame_pin = window(AW_PA_TLMM + AW_PIN_AUDIO_FRAME * AW_TLMM_WINDOW, 2 * sizeof(u32));
	if (z->gcc_reset && z->pll_vote && z->pll && z->glb && z->clk && z->pcm && z->bit_pin && z->frame_pin)
		return 0;
	awfxs_hw_unmap(z);
	return -ENOMEM;
}

static bool pll_already_running(struct aw_pll __iomem *p)
{
	u32 want = AW_PLL_OUT_ON | AW_PLL_LOCKED;

	return (readl(&p->mode) & want) == want && readl(&p->l) == AW_PLL_INT_PART &&
	       readl(&p->alpha_hi) == AW_PLL_FRAC_HI;
}

/*
 * The PLL is started only when nobody else uses it: a vote from the clock controller, or a running PLL with
 * other settings, is somebody else's and is left alone. A PLL that already runs with our settings is used as
 * it is; only a PLL started here is stopped again when the module is unloaded.
 */
static int pll_start(struct awfxs *z)
{
	struct aw_pll __iomem *p = z->pll;
	u32 mode;

	if (readl(z->pll_vote) & AW_PLL_VOTE_AUDIO)
		return -EBUSY;
	if (pll_already_running(p))
		return 0;
	if (readl(&p->mode) & (AW_PLL_OUT_ON | AW_PLL_LOCKED))
		return -EBUSY;

	writel(0, &p->mode);
	writel(AW_PLL_INT_PART, &p->l);
	writel(AW_PLL_FRAC_LO, &p->alpha_lo);
	writel(AW_PLL_FRAC_HI, &p->alpha_hi);
	writel(AW_PLL_CONFIG, &p->config_ctl);
	writel(AW_PLL_USER_FRACTION | AW_PLL_USER_POST_DIV4 | AW_PLL_USER_MAIN, &p->user_ctl);

	writel(AW_PLL_NOT_BYPASS, &p->mode);
	udelay(5);
	writel(AW_PLL_NOT_BYPASS | AW_PLL_NOT_RESET, &p->mode);
	if (readl_poll_timeout_atomic(&p->mode, mode, mode & AW_PLL_LOCKED, 10, PLL_LOCK_WAIT_US))
		return -ETIMEDOUT;
	writel(mode | AW_PLL_OUT_ON, &p->mode);
	z->pll_started = true;
	return 0;
}

static int root_clock_start(struct awfxs *z)
{
	struct aw_adss_clk __iomem *c = z->clk;
	u32 cmd;

	if (!z->gates_saved) {
		z->gate_was = readl(&c->gate);
		z->zsi_gate_was = readl(&c->zsi_gate);
		z->gates_saved = true;
	}

	writel(AW_CLK_CFG_FROM_PLL | AW_CLK_CFG_HALF_DIV, &c->config);
	writel(AW_CLK_MISC_DIV2, &c->misc);
	writel(AW_CLK_CMD_ROOT_ON | AW_CLK_CMD_UPDATE, &c->command);
	if (readl_poll_timeout_atomic(&c->command, cmd, !(cmd & AW_CLK_CMD_UPDATE), 10, CLK_UPDATE_WAIT_US))
		return -ETIMEDOUT;
	writel(1, &c->gate);
	writel(1, &c->zsi_gate);
	return 0;
}

static void stereo_block_run(unsigned long pa)
{
	u32 __iomem *reg = ioremap(pa, sizeof(u32));

	if (!reg)
		return;
	writel(AW_STEREO_RESET, reg);
	udelay(50);
	writel(0, reg);
	udelay(50);
	writel(AW_STEREO_RUN, reg);
	iounmap(reg);
}

static void audio_pins_mux(void)
{
	unsigned int pin;

	for (pin = AW_PIN_AUDIO_RX; pin <= AW_PIN_AUDIO_BIT; pin++) {
		u32 __iomem *cfg = ioremap(AW_PA_TLMM + pin * AW_TLMM_WINDOW, sizeof(u32));

		if (!cfg)
			continue;
		writel(AW_TLMM_AUDIO_FUNC, cfg);
		iounmap(cfg);
	}
}

/* number of level changes in 2048 back-to-back reads (several frame periods) */
static unsigned int activity(u32 __iomem *pin_window)
{
	unsigned int n, flips = 0;
	u32 before = readl(pin_window + 1) & 1, now;

	for (n = 0; n < 2048; n++) {
		now = readl(pin_window + 1) & 1;
		flips += now ^ before;
		before = now;
	}
	return flips;
}

int awfxs_pcm_up(struct awfxs *z, struct awfxs_pcm_up *up)
{
	int err;

	if (up->cfg.divider > 0xff)
		return -EINVAL;
	if (z->state & AWFXS_STATE_PCM_RUN)
		awfxs_pcm_down(z);

	if (readl(z->gcc_reset) & BIT(0)) {
		writel(0, z->gcc_reset);
		udelay(100);
	}
	err = pll_start(z);
	up->pll_mode = readl(&z->pll->mode);
	up->pll_locked = !!(up->pll_mode & AW_PLL_LOCKED);
	if (err)
		return err;
	z->state |= AWFXS_STATE_PLL_UP;

	writel(AW_GLB_ALL_BLOCKS, &z->glb->block_reset);
	udelay(50);
	writel(0, &z->glb->block_reset);
	udelay(50);
	writel(AW_GLB_CHIP_CTL_ON, &z->glb->chip_ctl);

	err = root_clock_start(z);
	if (err)
		return err;

	writel(AW_GLB_MODE_TDM, &z->glb->mode);
	writel(readl(&z->glb->clk_ctl) | AW_GLB_CLK_OUT_ENABLE, &z->glb->clk_ctl);
	writel(AW_GLB_TDM_VALUE, &z->glb->tdm_ctl);
	stereo_block_run(AW_PA_STEREO_A);
	stereo_block_run(AW_PA_STEREO_B);
	audio_pins_mux();

	writel(1, &z->glb->pcm_reset);
	udelay(50);
	writel(0, &z->glb->pcm_reset);
	udelay(50);
	writel(up->cfg.bitmap, &z->pcm->slotmask);
	writel(up->cfg.ctrl, &z->pcm->ctrl);
	writel(up->cfg.offset, &z->pcm->offset);
	writel(up->cfg.divider, &z->pcm->divider);
	writel(1, &z->pcm->start);
	z->state |= AWFXS_STATE_PCM_RUN;

	msleep(200);	/* the line chip wants a steady bit clock before it is reset */
	up->pclk_changes = activity(z->bit_pin);
	up->fsync_changes = activity(z->frame_pin);
	return 0;
}

/* hand the clocks back the way they were found; called once, when the module is unloaded */
void awfxs_hw_release(struct awfxs *z)
{
	awfxs_pcm_down(z);
	if (z->gates_saved) {
		writel(z->gate_was, &z->clk->gate);
		writel(z->zsi_gate_was, &z->clk->zsi_gate);
		z->gates_saved = false;
	}
	if (z->pll_started) {
		writel(0, &z->pll->mode);
		z->pll_started = false;
	}
	z->state &= ~AWFXS_STATE_PLL_UP;
}

void awfxs_pcm_down(struct awfxs *z)
{
	awfxs_bridge_stop(z);
	if (z->state & AWFXS_STATE_PCM_RUN)
		writel(0, &z->pcm->start);
	z->state &= ~AWFXS_STATE_PCM_RUN;
}

/* poll the receive FIFO for up to snap->timeout_us and collect what arrives */
int awfxs_snap(struct awfxs *z, struct awfxs_snap *snap, u32 *words)
{
	u64 deadline = ktime_get_ns() + (u64)snap->timeout_us * 1000;
	u32 n = 0, avail;

	if (!(z->state & AWFXS_STATE_PCM_RUN))
		return -ENODEV;

	for (avail = AW_PCM_RX_COUNT(readl(&z->pcm->fifo_count)); avail; avail--)
		(void)readl(&z->pcm->rx16);	/* stale words from before the capture */

	do {
		for (avail = AW_PCM_RX_COUNT(readl(&z->pcm->fifo_count)); avail && n < snap->max_words; avail--)
			words[n++] = readl(&z->pcm->rx16);
		cpu_relax();
	} while (n < snap->max_words && ktime_get_ns() < deadline);

	snap->got = n;
	snap->fifo_err = readl(&z->pcm->fifo_error);
	return 0;
}

static void dump_words(u32 *dst, const u32 __iomem *src, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++)
		dst[i] = readl(src + i);
}

/*
 * Words of the audio subsystem that may be read for a register dump, as word indexes. The FIFO data
 * ports and the words nobody has read yet are left out: reading a data port of an empty FIFO can
 * end in a bus error, which resets the router.
 */
static const u8 glb_readable[] = { 4, 5, 6, 7, 12 };
static const u8 pcm_readable[] = { 0, 1, 3, 4, 8 };

static void dump_listed(u32 *dst, const u32 __iomem *src, const u8 *words, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++)
		dst[words[i]] = readl(src + words[i]);
}

void awfxs_regs(struct awfxs *z, struct awfxs_regs *r)
{
	memset(r, 0, sizeof(*r));
	r->flags = 1;
	dump_words(r->pll, (const u32 __iomem *)z->pll, ARRAY_SIZE(r->pll));
	r->adss_reset = readl(z->gcc_reset);
	r->gcc_pll_vote = readl(z->pll_vote);
	if (!(z->state & AWFXS_STATE_PLL_UP))
		return;

	/* the audio subsystem answers only after the block reset is released and the PLL runs */
	r->flags |= 2;
	dump_listed(r->glb, (const u32 __iomem *)z->glb, glb_readable, ARRAY_SIZE(glb_readable));
	/* clock words 4, 5 and 7 are not guaranteed to answer: leave them out */
	r->clk[0] = readl(&z->clk->command);
	r->clk[1] = readl(&z->clk->config);
	r->clk[2] = readl(&z->clk->misc);
	r->clk[3] = readl(&z->clk->gate);
	r->clk[6] = readl(&z->clk->zsi_gate);
	dump_listed(r->pcm, (const u32 __iomem *)z->pcm, pcm_readable, ARRAY_SIZE(pcm_readable));
}
