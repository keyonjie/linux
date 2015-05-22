/*
 *  Digital Audio (PCM) abstract layer
 *  Copyright (c) by Jaroslav Kysela <perex@perex.cz>
 *                   Abramo Bagnara <abramo@alsa-project.org>
 *
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */

#include <linux/slab.h>
#include <linux/time.h>
#include <linux/math64.h>
#include <linux/export.h>
#include <sound/core.h>
#include <sound/control.h>
#include <sound/tlv.h>
#include <sound/info.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/timer.h>

#ifdef CONFIG_SND_PCM_XRUN_DEBUG
#define CREATE_TRACE_POINTS
#include "pcm_trace.h"
#else
#define trace_hwptr(substream, pos, in_interrupt)
#define trace_xrun(substream)
#define trace_hw_ptr_error(substream, reason)
#endif

/*
 * fill ring buffer with silence
 * runtime->silence_start: starting pointer to silence area
 * runtime->silence_filled: size filled with silence
 * runtime->silence_threshold: threshold from application
 * runtime->silence_size: maximal size from application
 *
 * when runtime->silence_size >= runtime->boundary - fill processed area with silence immediately
 */
void snd_pcm_playback_silence(struct snd_pcm_substream *substream, snd_pcm_uframes_t new_hw_ptr)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	snd_pcm_uframes_t frames, ofs, transfer;

	if (runtime->silence_size < runtime->boundary) {
		snd_pcm_sframes_t noise_dist, n;
		if (runtime->silence_start != runtime->control->appl_ptr) {
			n = runtime->control->appl_ptr - runtime->silence_start;
			if (n < 0)
				n += runtime->boundary;
			if ((snd_pcm_uframes_t)n < runtime->silence_filled)
				runtime->silence_filled -= n;
			else
				runtime->silence_filled = 0;
			runtime->silence_start = runtime->control->appl_ptr;
		}
		if (runtime->silence_filled >= runtime->buffer_size)
			return;
		noise_dist = snd_pcm_playback_hw_avail(runtime) + runtime->silence_filled;
		if (noise_dist >= (snd_pcm_sframes_t) runtime->silence_threshold)
			return;
		frames = runtime->silence_threshold - noise_dist;
		if (frames > runtime->silence_size)
			frames = runtime->silence_size;
	} else {
		if (new_hw_ptr == ULONG_MAX) {	/* initialization */
			snd_pcm_sframes_t avail = snd_pcm_playback_hw_avail(runtime);
			if (avail > runtime->buffer_size)
				avail = runtime->buffer_size;
			runtime->silence_filled = avail > 0 ? avail : 0;
			runtime->silence_start = (runtime->status->hw_ptr +
						  runtime->silence_filled) %
						 runtime->boundary;
		} else {
			ofs = runtime->status->hw_ptr;
			frames = new_hw_ptr - ofs;
			if ((snd_pcm_sframes_t)frames < 0)
				frames += runtime->boundary;
			runtime->silence_filled -= frames;
			if ((snd_pcm_sframes_t)runtime->silence_filled < 0) {
				runtime->silence_filled = 0;
				runtime->silence_start = new_hw_ptr;
			} else {
				runtime->silence_start = ofs;
			}
		}
		frames = runtime->buffer_size - runtime->silence_filled;
	}
	if (snd_BUG_ON(frames > runtime->buffer_size))
		return;
	if (frames == 0)
		return;
	ofs = runtime->silence_start % runtime->buffer_size;
	while (frames > 0) {
		transfer = ofs + frames > runtime->buffer_size ? runtime->buffer_size - ofs : frames;
		if (runtime->access == SNDRV_PCM_ACCESS_RW_INTERLEAVED ||
		    runtime->access == SNDRV_PCM_ACCESS_MMAP_INTERLEAVED) {
			if (substream->ops->silence) {
				int err;
				err = substream->ops->silence(substream, -1, ofs, transfer);
				snd_BUG_ON(err < 0);
			} else {
				char *hwbuf = runtime->dma_area + frames_to_bytes(runtime, ofs);
				snd_pcm_format_set_silence(runtime->format, hwbuf, transfer * runtime->channels);
			}
		} else {
			unsigned int c;
			unsigned int channels = runtime->channels;
			if (substream->ops->silence) {
				for (c = 0; c < channels; ++c) {
					int err;
					err = substream->ops->silence(substream, c, ofs, transfer);
					snd_BUG_ON(err < 0);
				}
			} else {
				size_t dma_csize = runtime->dma_bytes / channels;
				for (c = 0; c < channels; ++c) {
					char *hwbuf = runtime->dma_area + (c * dma_csize) + samples_to_bytes(runtime, ofs);
					snd_pcm_format_set_silence(runtime->format, hwbuf, transfer);
				}
			}
		}
		runtime->silence_filled += transfer;
		frames -= transfer;
		ofs = 0;
	}
}

#ifdef CONFIG_SND_DEBUG
void snd_pcm_debug_name(struct snd_pcm_substream *substream,
			   char *name, size_t len)
{
	snprintf(name, len, "pcmC%dD%d%c:%d",
		 substream->pcm->card->number,
		 substream->pcm->device,
		 substream->stream ? 'c' : 'p',
		 substream->number);
}
EXPORT_SYMBOL(snd_pcm_debug_name);
#endif

#define XRUN_DEBUG_BASIC	(1<<0)
#define XRUN_DEBUG_STACK	(1<<1)	/* dump also stack */
#define XRUN_DEBUG_JIFFIESCHECK	(1<<2)	/* do jiffies check */

#ifdef CONFIG_SND_PCM_XRUN_DEBUG

#define xrun_debug(substream, mask) \
			((substream)->pstr->xrun_debug & (mask))
#else
#define xrun_debug(substream, mask)	0
#endif

#define dump_stack_on_xrun(substream) do {			\
		if (xrun_debug(substream, XRUN_DEBUG_STACK))	\
			dump_stack();				\
	} while (0)

static void xrun(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;

	trace_xrun(substream);
	if (runtime->tstamp_mode == SNDRV_PCM_TSTAMP_ENABLE)
		snd_pcm_gettime(runtime, (struct timespec *)&runtime->status->tstamp);
	snd_pcm_stop(substream, SNDRV_PCM_STATE_XRUN);
	if (xrun_debug(substream, XRUN_DEBUG_BASIC)) {
		char name[16];
		snd_pcm_debug_name(substream, name, sizeof(name));
		pcm_warn(substream->pcm, "XRUN: %s\n", name);
		dump_stack_on_xrun(substream);
	}
}

#ifdef CONFIG_SND_PCM_XRUN_DEBUG
#define hw_ptr_error(substream, in_interrupt, reason, fmt, args...)	\
	do {								\
		trace_hw_ptr_error(substream, reason);	\
		if (xrun_debug(substream, XRUN_DEBUG_BASIC)) {		\
			pr_err_ratelimited("ALSA: PCM: [%c] " reason ": " fmt, \
					   (in_interrupt) ? 'Q' : 'P', ##args);	\
			dump_stack_on_xrun(substream);			\
		}							\
	} while (0)

#else /* ! CONFIG_SND_PCM_XRUN_DEBUG */

#define hw_ptr_error(substream, fmt, args...) do { } while (0)

#endif

int snd_pcm_update_state(struct snd_pcm_substream *substream,
			 struct snd_pcm_runtime *runtime)
{
	snd_pcm_uframes_t avail;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		avail = snd_pcm_playback_avail(runtime);
	else
		avail = snd_pcm_capture_avail(runtime);
	if (avail > runtime->avail_max)
		runtime->avail_max = avail;
	if (runtime->status->state == SNDRV_PCM_STATE_DRAINING) {
		if (avail >= runtime->buffer_size) {
			snd_pcm_drain_done(substream);
			return -EPIPE;
		}
	} else {
		if (avail >= runtime->stop_threshold) {
			xrun(substream);
			return -EPIPE;
		}
	}
	if (runtime->twake) {
		if (avail >= runtime->twake)
			wake_up(&runtime->tsleep);
	} else if (avail >= runtime->control->avail_min)
		wake_up(&runtime->sleep);
	return 0;
}

static void update_audio_tstamp(struct snd_pcm_substream *substream,
				struct timespec *curr_tstamp,
				struct timespec *audio_tstamp)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	u64 audio_frames, audio_nsecs;
	struct timespec driver_tstamp;

	if (runtime->tstamp_mode != SNDRV_PCM_TSTAMP_ENABLE)
		return;

	if (!(substream->ops->get_time_info) ||
		(runtime->audio_tstamp_report.actual_type ==
			SNDRV_PCM_AUDIO_TSTAMP_TYPE_DEFAULT)) {

		/*
		 * provide audio timestamp derived from pointer position
		 * add delay only if requested
		 */

		audio_frames = runtime->hw_ptr_wrap + runtime->status->hw_ptr;

		if (runtime->audio_tstamp_config.report_delay) {
			if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
				audio_frames -=  runtime->delay;
			else
				audio_frames +=  runtime->delay;
		}
		audio_nsecs = div_u64(audio_frames * 1000000000LL,
				runtime->rate);
		*audio_tstamp = ns_to_timespec(audio_nsecs);
	}
	runtime->status->audio_tstamp = *audio_tstamp;
	runtime->status->tstamp = *curr_tstamp;

	/*
	 * re-take a driver timestamp to let apps detect if the reference tstamp
	 * read by low-level hardware was provided with a delay
	 */
	snd_pcm_gettime(substream->runtime, (struct timespec *)&driver_tstamp);
	runtime->driver_tstamp = driver_tstamp;
}

static int snd_pcm_update_hw_ptr0(struct snd_pcm_substream *substream,
				  unsigned int in_interrupt)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	snd_pcm_uframes_t pos;
	snd_pcm_uframes_t old_hw_ptr, new_hw_ptr, hw_base;
	snd_pcm_sframes_t hdelta, delta;
	unsigned long jdelta;
	unsigned long curr_jiffies;
	struct timespec curr_tstamp;
	struct timespec audio_tstamp;
	int crossed_boundary = 0;

	old_hw_ptr = runtime->status->hw_ptr;

	/*
	 * group pointer, time and jiffies reads to allow for more
	 * accurate correlations/corrections.
	 * The values are stored at the end of this routine after
	 * corrections for hw_ptr position
	 */
	pos = substream->ops->pointer(substream);
	curr_jiffies = jiffies;
	if (runtime->tstamp_mode == SNDRV_PCM_TSTAMP_ENABLE) {
		if ((substream->ops->get_time_info) &&
			(runtime->audio_tstamp_config.type_requested != SNDRV_PCM_AUDIO_TSTAMP_TYPE_DEFAULT)) {
			substream->ops->get_time_info(substream, &curr_tstamp,
						&audio_tstamp,
						&runtime->audio_tstamp_config,
						&runtime->audio_tstamp_report);

			/* re-test in case tstamp type is not supported in hardware and was demoted to DEFAULT */
			if (runtime->audio_tstamp_report.actual_type == SNDRV_PCM_AUDIO_TSTAMP_TYPE_DEFAULT)
				snd_pcm_gettime(runtime, (struct timespec *)&curr_tstamp);
		} else
			snd_pcm_gettime(runtime, (struct timespec *)&curr_tstamp);
	}

	if (pos == SNDRV_PCM_POS_XRUN) {
		xrun(substream);
		return -EPIPE;
	}
	if (pos >= runtime->buffer_size) {
		if (printk_ratelimit()) {
			char name[16];
			snd_pcm_debug_name(substream, name, sizeof(name));
			pcm_err(substream->pcm,
				"BUG: %s, pos = %ld, buffer size = %ld, period size = %ld\n",
				name, pos, runtime->buffer_size,
				runtime->period_size);
		}
		pos = 0;
	}
	pos -= pos % runtime->min_align;
	trace_hwptr(substream, pos, in_interrupt);
	hw_base = runtime->hw_ptr_base;
	new_hw_ptr = hw_base + pos;
	if (in_interrupt) {
		/* we know that one period was processed */
		/* delta = "expected next hw_ptr" for in_interrupt != 0 */
		delta = runtime->hw_ptr_interrupt + runtime->period_size;
		if (delta > new_hw_ptr) {
			/* check for double acknowledged interrupts */
			hdelta = curr_jiffies - runtime->hw_ptr_jiffies;
			if (hdelta > runtime->hw_ptr_buffer_jiffies/2 + 1) {
				hw_base += runtime->buffer_size;
				if (hw_base >= runtime->boundary) {
					hw_base = 0;
					crossed_boundary++;
				}
				new_hw_ptr = hw_base + pos;
				goto __delta;
			}
		}
	}
	/* new_hw_ptr might be lower than old_hw_ptr in case when */
	/* pointer crosses the end of the ring buffer */
	if (new_hw_ptr < old_hw_ptr) {
		hw_base += runtime->buffer_size;
		if (hw_base >= runtime->boundary) {
			hw_base = 0;
			crossed_boundary++;
		}
		new_hw_ptr = hw_base + pos;
	}
      __delta:
	delta = new_hw_ptr - old_hw_ptr;
	if (delta < 0)
		delta += runtime->boundary;

	if (runtime->no_period_wakeup) {
		snd_pcm_sframes_t xrun_threshold;
		/*
		 * Without regular period interrupts, we have to check
		 * the elapsed time to detect xruns.
		 */
		jdelta = curr_jiffies - runtime->hw_ptr_jiffies;
		if (jdelta < runtime->hw_ptr_buffer_jiffies / 2)
			goto no_delta_check;
		hdelta = jdelta - delta * HZ / runtime->rate;
		xrun_threshold = runtime->hw_ptr_buffer_jiffies / 2 + 1;
		while (hdelta > xrun_threshold) {
			delta += runtime->buffer_size;
			hw_base += runtime->buffer_size;
			if (hw_base >= runtime->boundary) {
				hw_base = 0;
				crossed_boundary++;
			}
			new_hw_ptr = hw_base + pos;
			hdelta -= runtime->hw_ptr_buffer_jiffies;
		}
		goto no_delta_check;
	}

	/* something must be really wrong */
	if (delta >= runtime->buffer_size + runtime->period_size) {
		hw_ptr_error(substream, in_interrupt, "Unexpected hw_ptr",
			     "(stream=%i, pos=%ld, new_hw_ptr=%ld, old_hw_ptr=%ld)\n",
			     substream->stream, (long)pos,
			     (long)new_hw_ptr, (long)old_hw_ptr);
		return 0;
	}

	/* Do jiffies check only in xrun_debug mode */
	if (!xrun_debug(substream, XRUN_DEBUG_JIFFIESCHECK))
		goto no_jiffies_check;

	/* Skip the jiffies check for hardwares with BATCH flag.
	 * Such hardware usually just increases the position at each IRQ,
	 * thus it can't give any strange position.
	 */
	if (runtime->hw.info & SNDRV_PCM_INFO_BATCH)
		goto no_jiffies_check;
	hdelta = delta;
	if (hdelta < runtime->delay)
		goto no_jiffies_check;
	hdelta -= runtime->delay;
	jdelta = curr_jiffies - runtime->hw_ptr_jiffies;
	if (((hdelta * HZ) / runtime->rate) > jdelta + HZ/100) {
		delta = jdelta /
			(((runtime->period_size * HZ) / runtime->rate)
								+ HZ/100);
		/* move new_hw_ptr according jiffies not pos variable */
		new_hw_ptr = old_hw_ptr;
		hw_base = delta;
		/* use loop to avoid checks for delta overflows */
		/* the delta value is small or zero in most cases */
		while (delta > 0) {
			new_hw_ptr += runtime->period_size;
			if (new_hw_ptr >= runtime->boundary) {
				new_hw_ptr -= runtime->boundary;
				crossed_boundary--;
			}
			delta--;
		}
		/* align hw_base to buffer_size */
		hw_ptr_error(substream, in_interrupt, "hw_ptr skipping",
			     "(pos=%ld, delta=%ld, period=%ld, jdelta=%lu/%lu/%lu, hw_ptr=%ld/%ld)\n",
			     (long)pos, (long)hdelta,
			     (long)runtime->period_size, jdelta,
			     ((hdelta * HZ) / runtime->rate), hw_base,
			     (unsigned long)old_hw_ptr,
			     (unsigned long)new_hw_ptr);
		/* reset values to proper state */
		delta = 0;
		hw_base = new_hw_ptr - (new_hw_ptr % runtime->buffer_size);
	}
 no_jiffies_check:
	if (delta > runtime->period_size + runtime->period_size / 2) {
		hw_ptr_error(substream, in_interrupt,
			     "Lost interrupts?",
			     "(stream=%i, delta=%ld, new_hw_ptr=%ld, old_hw_ptr=%ld)\n",
			     substream->stream, (long)delta,
			     (long)new_hw_ptr,
			     (long)old_hw_ptr);
	}

 no_delta_check:
	if (runtime->status->hw_ptr == new_hw_ptr) {
		update_audio_tstamp(substream, &curr_tstamp, &audio_tstamp);
		return 0;
	}

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK &&
	    runtime->silence_size > 0)
		snd_pcm_playback_silence(substream, new_hw_ptr);

	if (in_interrupt) {
		delta = new_hw_ptr - runtime->hw_ptr_interrupt;
		if (delta < 0)
			delta += runtime->boundary;
		delta -= (snd_pcm_uframes_t)delta % runtime->period_size;
		runtime->hw_ptr_interrupt += delta;
		if (runtime->hw_ptr_interrupt >= runtime->boundary)
			runtime->hw_ptr_interrupt -= runtime->boundary;
	}
	runtime->hw_ptr_base = hw_base;
	runtime->status->hw_ptr = new_hw_ptr;
	runtime->hw_ptr_jiffies = curr_jiffies;
	if (crossed_boundary) {
		snd_BUG_ON(crossed_boundary != 1);
		runtime->hw_ptr_wrap += runtime->boundary;
	}

	update_audio_tstamp(substream, &curr_tstamp, &audio_tstamp);

	return snd_pcm_update_state(substream, runtime);
}

/* CAUTION: call it with irq disabled */
int snd_pcm_update_hw_ptr(struct snd_pcm_substream *substream)
{
	return snd_pcm_update_hw_ptr0(substream, 0);
}

/**
 * snd_pcm_set_ops - set the PCM operators
 * @pcm: the pcm instance
 * @direction: stream direction, SNDRV_PCM_STREAM_XXX
 * @ops: the operator table
 *
 * Sets the given PCM operators to the pcm instance.
 */
void snd_pcm_set_ops(struct snd_pcm *pcm, int direction,
		     const struct snd_pcm_ops *ops)
{
	struct snd_pcm_str *stream = &pcm->streams[direction];
	struct snd_pcm_substream *substream;
	
	for (substream = stream->substream; substream != NULL; substream = substream->next)
		substream->ops = ops;
}

EXPORT_SYMBOL(snd_pcm_set_ops);

/**
 * snd_pcm_sync - set the PCM sync id
 * @substream: the pcm substream
 *
 * Sets the PCM sync identifier for the card.
 */
void snd_pcm_set_sync(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	
	runtime->sync.id32[0] = substream->pcm->card->number;
	runtime->sync.id32[1] = -1;
	runtime->sync.id32[2] = -1;
	runtime->sync.id32[3] = -1;
}

EXPORT_SYMBOL(snd_pcm_set_sync);

/*
 *  Standard ioctl routine
 */

static int snd_interval_refine_first(struct snd_interval *i)
{
	if (snd_BUG_ON(snd_interval_empty(i)))
		return -EINVAL;
	if (snd_interval_single(i))
		return 0;
	i->max = i->min;
	i->openmax = i->openmin;
	if (i->openmax)
		i->max++;
	return 1;
}

static int snd_interval_refine_last(struct snd_interval *i)
{
	if (snd_BUG_ON(snd_interval_empty(i)))
		return -EINVAL;
	if (snd_interval_single(i))
		return 0;
	i->min = i->max;
	i->openmin = i->openmax;
	if (i->openmin)
		i->min--;
	return 1;
}

static void _snd_pcm_hw_param_any(struct snd_pcm_hw_params *params,
				  snd_pcm_hw_param_t var)
{
	if (hw_is_mask(var)) {
		snd_mask_any(hw_param_mask(params, var));
		params->cmask |= 1 << var;
		params->rmask |= 1 << var;
		return;
	}
	if (hw_is_interval(var)) {
		snd_interval_any(hw_param_interval(params, var));
		params->cmask |= 1 << var;
		params->rmask |= 1 << var;
		return;
	}
	snd_BUG();
}

void _snd_pcm_hw_params_any(struct snd_pcm_hw_params *params)
{
	unsigned int k;
	memset(params, 0, sizeof(*params));
	for (k = SNDRV_PCM_HW_PARAM_FIRST_MASK; k <= SNDRV_PCM_HW_PARAM_LAST_MASK; k++)
		_snd_pcm_hw_param_any(params, k);
	for (k = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; k <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; k++)
		_snd_pcm_hw_param_any(params, k);
	params->info = ~0U;
}

EXPORT_SYMBOL(_snd_pcm_hw_params_any);

/**
 * snd_pcm_hw_param_value - return @params field @var value
 * @params: the hw_params instance
 * @var: parameter to retrieve
 * @dir: pointer to the direction (-1,0,1) or %NULL
 *
 * Return: The value for field @var if it's fixed in configuration space
 * defined by @params. -%EINVAL otherwise.
 */
int snd_pcm_hw_param_value(const struct snd_pcm_hw_params *params,
			   snd_pcm_hw_param_t var, int *dir)
{
	if (hw_is_mask(var)) {
		const struct snd_mask *mask = hw_param_mask_c(params, var);
		if (!snd_mask_single(mask))
			return -EINVAL;
		if (dir)
			*dir = 0;
		return snd_mask_value(mask);
	}
	if (hw_is_interval(var)) {
		const struct snd_interval *i = hw_param_interval_c(params, var);
		if (!snd_interval_single(i))
			return -EINVAL;
		if (dir)
			*dir = i->openmin;
		return snd_interval_value(i);
	}
	return -EINVAL;
}

EXPORT_SYMBOL(snd_pcm_hw_param_value);

void _snd_pcm_hw_param_setempty(struct snd_pcm_hw_params *params,
				snd_pcm_hw_param_t var)
{
	if (hw_is_mask(var)) {
		snd_mask_none(hw_param_mask(params, var));
		params->cmask |= 1 << var;
		params->rmask |= 1 << var;
	} else if (hw_is_interval(var)) {
		snd_interval_none(hw_param_interval(params, var));
		params->cmask |= 1 << var;
		params->rmask |= 1 << var;
	} else {
		snd_BUG();
	}
}

EXPORT_SYMBOL(_snd_pcm_hw_param_setempty);

static int _snd_pcm_hw_param_first(struct snd_pcm_hw_params *params,
				   snd_pcm_hw_param_t var)
{
	int changed;
	if (hw_is_mask(var))
		changed = snd_mask_refine_first(hw_param_mask(params, var));
	else if (hw_is_interval(var))
		changed = snd_interval_refine_first(hw_param_interval(params, var));
	else
		return -EINVAL;
	if (changed) {
		params->cmask |= 1 << var;
		params->rmask |= 1 << var;
	}
	return changed;
}


/**
 * snd_pcm_hw_param_first - refine config space and return minimum value
 * @pcm: PCM instance
 * @params: the hw_params instance
 * @var: parameter to retrieve
 * @dir: pointer to the direction (-1,0,1) or %NULL
 *
 * Inside configuration space defined by @params remove from @var all
 * values > minimum. Reduce configuration space accordingly.
 *
 * Return: The minimum, or a negative error code on failure.
 */
int snd_pcm_hw_param_first(struct snd_pcm_substream *pcm, 
			   struct snd_pcm_hw_params *params, 
			   snd_pcm_hw_param_t var, int *dir)
{
	int changed = _snd_pcm_hw_param_first(params, var);
	if (changed < 0)
		return changed;
	if (params->rmask) {
		int err = snd_pcm_hw_refine(pcm, params);
		if (snd_BUG_ON(err < 0))
			return err;
	}
	return snd_pcm_hw_param_value(params, var, dir);
}

EXPORT_SYMBOL(snd_pcm_hw_param_first);

static int _snd_pcm_hw_param_last(struct snd_pcm_hw_params *params,
				  snd_pcm_hw_param_t var)
{
	int changed;
	if (hw_is_mask(var))
		changed = snd_mask_refine_last(hw_param_mask(params, var));
	else if (hw_is_interval(var))
		changed = snd_interval_refine_last(hw_param_interval(params, var));
	else
		return -EINVAL;
	if (changed) {
		params->cmask |= 1 << var;
		params->rmask |= 1 << var;
	}
	return changed;
}


/**
 * snd_pcm_hw_param_last - refine config space and return maximum value
 * @pcm: PCM instance
 * @params: the hw_params instance
 * @var: parameter to retrieve
 * @dir: pointer to the direction (-1,0,1) or %NULL
 *
 * Inside configuration space defined by @params remove from @var all
 * values < maximum. Reduce configuration space accordingly.
 *
 * Return: The maximum, or a negative error code on failure.
 */
int snd_pcm_hw_param_last(struct snd_pcm_substream *pcm, 
			  struct snd_pcm_hw_params *params,
			  snd_pcm_hw_param_t var, int *dir)
{
	int changed = _snd_pcm_hw_param_last(params, var);
	if (changed < 0)
		return changed;
	if (params->rmask) {
		int err = snd_pcm_hw_refine(pcm, params);
		if (snd_BUG_ON(err < 0))
			return err;
	}
	return snd_pcm_hw_param_value(params, var, dir);
}

EXPORT_SYMBOL(snd_pcm_hw_param_last);

/**
 * snd_pcm_hw_param_choose - choose a configuration defined by @params
 * @pcm: PCM instance
 * @params: the hw_params instance
 *
 * Choose one configuration from configuration space defined by @params.
 * The configuration chosen is that obtained fixing in this order:
 * first access, first format, first subformat, min channels,
 * min rate, min period time, max buffer size, min tick time
 *
 * Return: Zero if successful, or a negative error code on failure.
 */
int snd_pcm_hw_params_choose(struct snd_pcm_substream *pcm,
			     struct snd_pcm_hw_params *params)
{
	static int vars[] = {
		SNDRV_PCM_HW_PARAM_ACCESS,
		SNDRV_PCM_HW_PARAM_FORMAT,
		SNDRV_PCM_HW_PARAM_SUBFORMAT,
		SNDRV_PCM_HW_PARAM_CHANNELS,
		SNDRV_PCM_HW_PARAM_RATE,
		SNDRV_PCM_HW_PARAM_PERIOD_TIME,
		SNDRV_PCM_HW_PARAM_BUFFER_SIZE,
		SNDRV_PCM_HW_PARAM_TICK_TIME,
		-1
	};
	int err, *v;

	for (v = vars; *v != -1; v++) {
		if (*v != SNDRV_PCM_HW_PARAM_BUFFER_SIZE)
			err = snd_pcm_hw_param_first(pcm, params, *v, NULL);
		else
			err = snd_pcm_hw_param_last(pcm, params, *v, NULL);
		if (snd_BUG_ON(err < 0))
			return err;
	}
	return 0;
}

static int snd_pcm_lib_ioctl_reset(struct snd_pcm_substream *substream,
				   void *arg)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned long flags;
	snd_pcm_stream_lock_irqsave(substream, flags);
	if (snd_pcm_running(substream) &&
	    snd_pcm_update_hw_ptr(substream) >= 0)
		runtime->status->hw_ptr %= runtime->buffer_size;
	else {
		runtime->status->hw_ptr = 0;
		runtime->hw_ptr_wrap = 0;
	}
	snd_pcm_stream_unlock_irqrestore(substream, flags);
	return 0;
}

static int snd_pcm_lib_ioctl_channel_info(struct snd_pcm_substream *substream,
					  void *arg)
{
	struct snd_pcm_channel_info *info = arg;
	struct snd_pcm_runtime *runtime = substream->runtime;
	int width;
	if (!(runtime->info & SNDRV_PCM_INFO_MMAP)) {
		info->offset = -1;
		return 0;
	}
	width = snd_pcm_format_physical_width(runtime->format);
	if (width < 0)
		return width;
	info->offset = 0;
	switch (runtime->access) {
	case SNDRV_PCM_ACCESS_MMAP_INTERLEAVED:
	case SNDRV_PCM_ACCESS_RW_INTERLEAVED:
		info->first = info->channel * width;
		info->step = runtime->channels * width;
		break;
	case SNDRV_PCM_ACCESS_MMAP_NONINTERLEAVED:
	case SNDRV_PCM_ACCESS_RW_NONINTERLEAVED:
	{
		size_t size = runtime->dma_bytes / runtime->channels;
		info->first = info->channel * size * 8;
		info->step = width;
		break;
	}
	default:
		snd_BUG();
		break;
	}
	return 0;
}

static int snd_pcm_lib_ioctl_fifo_size(struct snd_pcm_substream *substream,
				       void *arg)
{
	struct snd_pcm_hw_params *params = arg;
	snd_pcm_format_t format;
	int channels;
	ssize_t frame_size;

	params->fifo_size = substream->runtime->hw.fifo_size;
	if (!(substream->runtime->hw.info & SNDRV_PCM_INFO_FIFO_IN_FRAMES)) {
		format = params_format(params);
		channels = params_channels(params);
		frame_size = snd_pcm_format_size(format, channels);
		if (frame_size > 0)
			params->fifo_size /= (unsigned)frame_size;
	}
	return 0;
}

/**
 * snd_pcm_lib_ioctl - a generic PCM ioctl callback
 * @substream: the pcm substream instance
 * @cmd: ioctl command
 * @arg: ioctl argument
 *
 * Processes the generic ioctl commands for PCM.
 * Can be passed as the ioctl callback for PCM ops.
 *
 * Return: Zero if successful, or a negative error code on failure.
 */
int snd_pcm_lib_ioctl(struct snd_pcm_substream *substream,
		      unsigned int cmd, void *arg)
{
	switch (cmd) {
	case SNDRV_PCM_IOCTL1_INFO:
		return 0;
	case SNDRV_PCM_IOCTL1_RESET:
		return snd_pcm_lib_ioctl_reset(substream, arg);
	case SNDRV_PCM_IOCTL1_CHANNEL_INFO:
		return snd_pcm_lib_ioctl_channel_info(substream, arg);
	case SNDRV_PCM_IOCTL1_FIFO_SIZE:
		return snd_pcm_lib_ioctl_fifo_size(substream, arg);
	}
	return -ENXIO;
}

EXPORT_SYMBOL(snd_pcm_lib_ioctl);

/**
 * snd_pcm_period_elapsed - update the pcm status for the next period
 * @substream: the pcm substream instance
 *
 * This function is called from the interrupt handler when the
 * PCM has processed the period size.  It will update the current
 * pointer, wake up sleepers, etc.
 *
 * Even if more than one periods have elapsed since the last call, you
 * have to call this only once.
 */
void snd_pcm_period_elapsed(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime;
	unsigned long flags;

	if (PCM_RUNTIME_CHECK(substream))
		return;
	runtime = substream->runtime;

	if (runtime->transfer_ack_begin)
		runtime->transfer_ack_begin(substream);

	snd_pcm_stream_lock_irqsave(substream, flags);
	if (!snd_pcm_running(substream) ||
	    snd_pcm_update_hw_ptr0(substream, 1) < 0)
		goto _end;

	if (substream->timer_running)
		snd_timer_interrupt(substream->timer, 1);
 _end:
	snd_pcm_stream_unlock_irqrestore(substream, flags);
	if (runtime->transfer_ack_end)
		runtime->transfer_ack_end(substream);
	kill_fasync(&runtime->fasync, SIGIO, POLL_IN);
}

EXPORT_SYMBOL(snd_pcm_period_elapsed);

/*
 * Wait until avail_min data becomes available
 * Returns a negative error code if any error occurs during operation.
 * The available space is stored on availp.  When err = 0 and avail = 0
 * on the capture stream, it indicates the stream is in DRAINING state.
 */
static int wait_for_avail(struct snd_pcm_substream *substream,
			      snd_pcm_uframes_t *availp)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	int is_playback = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
	wait_queue_t wait;
	int err = 0;
	snd_pcm_uframes_t avail = 0;
	long wait_time, tout;

	init_waitqueue_entry(&wait, current);
	set_current_state(TASK_INTERRUPTIBLE);
	add_wait_queue(&runtime->tsleep, &wait);

	if (runtime->no_period_wakeup)
		wait_time = MAX_SCHEDULE_TIMEOUT;
	else {
		wait_time = 10;
		if (runtime->rate) {
			long t = runtime->period_size * 2 / runtime->rate;
			wait_time = max(t, wait_time);
		}
		wait_time = msecs_to_jiffies(wait_time * 1000);
	}

	for (;;) {
		if (signal_pending(current)) {
			err = -ERESTARTSYS;
			break;
		}

		/*
		 * We need to check if space became available already
		 * (and thus the wakeup happened already) first to close
		 * the race of space already having become available.
		 * This check must happen after been added to the waitqueue
		 * and having current state be INTERRUPTIBLE.
		 */
		if (is_playback)
			avail = snd_pcm_playback_avail(runtime);
		else
			avail = snd_pcm_capture_avail(runtime);
		if (avail >= runtime->twake)
			break;
		snd_pcm_stream_unlock_irq(substream);

		tout = schedule_timeout(wait_time);

		snd_pcm_stream_lock_irq(substream);
		set_current_state(TASK_INTERRUPTIBLE);
		switch (runtime->status->state) {
		case SNDRV_PCM_STATE_SUSPENDED:
			err = -ESTRPIPE;
			goto _endloop;
		case SNDRV_PCM_STATE_XRUN:
			err = -EPIPE;
			goto _endloop;
		case SNDRV_PCM_STATE_DRAINING:
			if (is_playback)
				err = -EPIPE;
			else 
				avail = 0; /* indicate draining */
			goto _endloop;
		case SNDRV_PCM_STATE_OPEN:
		case SNDRV_PCM_STATE_SETUP:
		case SNDRV_PCM_STATE_DISCONNECTED:
			err = -EBADFD;
			goto _endloop;
		case SNDRV_PCM_STATE_PAUSED:
			continue;
		}
		if (!tout) {
			pcm_dbg(substream->pcm,
				"%s write error (DMA or IRQ trouble?)\n",
				is_playback ? "playback" : "capture");
			err = -EIO;
			break;
		}
	}
 _endloop:
	set_current_state(TASK_RUNNING);
	remove_wait_queue(&runtime->tsleep, &wait);
	*availp = avail;
	return err;
}
	
static int snd_pcm_lib_write_transfer(struct snd_pcm_substream *substream,
				      unsigned int hwoff,
				      unsigned long data, unsigned int off,
				      snd_pcm_uframes_t frames)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	int err;
	char __user *buf = (char __user *) data + frames_to_bytes(runtime, off);
	if (substream->ops->copy) {
		if ((err = substream->ops->copy(substream, -1, hwoff, buf, frames)) < 0)
			return err;
	} else {
		char *hwbuf = runtime->dma_area + frames_to_bytes(runtime, hwoff);
		if (copy_from_user(hwbuf, buf, frames_to_bytes(runtime, frames)))
			return -EFAULT;
	}
	return 0;
}
 
snd_pcm_sframes_t snd_pcm_lib_write1(struct snd_pcm_substream *substream,
					    unsigned long data,
					    snd_pcm_uframes_t size,
					    int nonblock,
					    transfer_f transfer)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	snd_pcm_uframes_t xfer = 0;
	snd_pcm_uframes_t offset = 0;
	snd_pcm_uframes_t avail;
	int err = 0;

	if (size == 0)
		return 0;

	snd_pcm_stream_lock_irq(substream);
	switch (runtime->status->state) {
	case SNDRV_PCM_STATE_PREPARED:
	case SNDRV_PCM_STATE_RUNNING:
	case SNDRV_PCM_STATE_PAUSED:
		break;
	case SNDRV_PCM_STATE_XRUN:
		err = -EPIPE;
		goto _end_unlock;
	case SNDRV_PCM_STATE_SUSPENDED:
		err = -ESTRPIPE;
		goto _end_unlock;
	default:
		err = -EBADFD;
		goto _end_unlock;
	}

	runtime->twake = runtime->control->avail_min ? : 1;
	if (runtime->status->state == SNDRV_PCM_STATE_RUNNING)
		snd_pcm_update_hw_ptr(substream);
	avail = snd_pcm_playback_avail(runtime);
	while (size > 0) {
		snd_pcm_uframes_t frames, appl_ptr, appl_ofs;
		snd_pcm_uframes_t cont;
		if (!avail) {
			if (nonblock) {
				err = -EAGAIN;
				goto _end_unlock;
			}
			runtime->twake = min_t(snd_pcm_uframes_t, size,
					runtime->control->avail_min ? : 1);
			err = wait_for_avail(substream, &avail);
			if (err < 0)
				goto _end_unlock;
		}
		frames = size > avail ? avail : size;
		cont = runtime->buffer_size - runtime->control->appl_ptr % runtime->buffer_size;
		if (frames > cont)
			frames = cont;
		if (snd_BUG_ON(!frames)) {
			runtime->twake = 0;
			snd_pcm_stream_unlock_irq(substream);
			return -EINVAL;
		}
		appl_ptr = runtime->control->appl_ptr;
		appl_ofs = appl_ptr % runtime->buffer_size;
		snd_pcm_stream_unlock_irq(substream);
		err = transfer(substream, appl_ofs, data, offset, frames);
		snd_pcm_stream_lock_irq(substream);
		if (err < 0)
			goto _end_unlock;
		switch (runtime->status->state) {
		case SNDRV_PCM_STATE_XRUN:
			err = -EPIPE;
			goto _end_unlock;
		case SNDRV_PCM_STATE_SUSPENDED:
			err = -ESTRPIPE;
			goto _end_unlock;
		default:
			break;
		}
		appl_ptr += frames;
		if (appl_ptr >= runtime->boundary)
			appl_ptr -= runtime->boundary;
		runtime->control->appl_ptr = appl_ptr;
		if (substream->ops->ack)
			substream->ops->ack(substream);

		offset += frames;
		size -= frames;
		xfer += frames;
		avail -= frames;
		if (runtime->status->state == SNDRV_PCM_STATE_PREPARED &&
		    snd_pcm_playback_hw_avail(runtime) >= (snd_pcm_sframes_t)runtime->start_threshold) {
			err = snd_pcm_start(substream);
			if (err < 0)
				goto _end_unlock;
		}
	}
 _end_unlock:
	runtime->twake = 0;
	if (xfer > 0 && err >= 0)
		snd_pcm_update_state(substream, runtime);
	snd_pcm_stream_unlock_irq(substream);
	return xfer > 0 ? (snd_pcm_sframes_t)xfer : err;
}

/* sanity-check for read/write methods */
int pcm_sanity_check(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime;
	if (PCM_RUNTIME_CHECK(substream))
		return -ENXIO;
	runtime = substream->runtime;
	if (snd_BUG_ON(!substream->ops->copy && !runtime->dma_area))
		return -EINVAL;
	if (runtime->status->state == SNDRV_PCM_STATE_OPEN)
		return -EBADFD;
	return 0;
}

snd_pcm_sframes_t snd_pcm_lib_write(struct snd_pcm_substream *substream, const void __user *buf, snd_pcm_uframes_t size)
{
	struct snd_pcm_runtime *runtime;
	int nonblock;
	int err;

	err = pcm_sanity_check(substream);
	if (err < 0)
		return err;
	runtime = substream->runtime;
	nonblock = !!(substream->f_flags & O_NONBLOCK);

	if (runtime->access != SNDRV_PCM_ACCESS_RW_INTERLEAVED &&
	    runtime->channels > 1)
		return -EINVAL;
	return snd_pcm_lib_write1(substream, (unsigned long)buf, size, nonblock,
				  snd_pcm_lib_write_transfer);
}

EXPORT_SYMBOL(snd_pcm_lib_write);

static int snd_pcm_lib_read_transfer(struct snd_pcm_substream *substream, 
				     unsigned int hwoff,
				     unsigned long data, unsigned int off,
				     snd_pcm_uframes_t frames)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	int err;
	char __user *buf = (char __user *) data + frames_to_bytes(runtime, off);
	if (substream->ops->copy) {
		if ((err = substream->ops->copy(substream, -1, hwoff, buf, frames)) < 0)
			return err;
	} else {
		char *hwbuf = runtime->dma_area + frames_to_bytes(runtime, hwoff);
		if (copy_to_user(buf, hwbuf, frames_to_bytes(runtime, frames)))
			return -EFAULT;
	}
	return 0;
}

snd_pcm_sframes_t snd_pcm_lib_read1(struct snd_pcm_substream *substream,
					   unsigned long data,
					   snd_pcm_uframes_t size,
					   int nonblock,
					   transfer_f transfer)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	snd_pcm_uframes_t xfer = 0;
	snd_pcm_uframes_t offset = 0;
	snd_pcm_uframes_t avail;
	int err = 0;

	if (size == 0)
		return 0;

	snd_pcm_stream_lock_irq(substream);
	switch (runtime->status->state) {
	case SNDRV_PCM_STATE_PREPARED:
		if (size >= runtime->start_threshold) {
			err = snd_pcm_start(substream);
			if (err < 0)
				goto _end_unlock;
		}
		break;
	case SNDRV_PCM_STATE_DRAINING:
	case SNDRV_PCM_STATE_RUNNING:
	case SNDRV_PCM_STATE_PAUSED:
		break;
	case SNDRV_PCM_STATE_XRUN:
		err = -EPIPE;
		goto _end_unlock;
	case SNDRV_PCM_STATE_SUSPENDED:
		err = -ESTRPIPE;
		goto _end_unlock;
	default:
		err = -EBADFD;
		goto _end_unlock;
	}

	runtime->twake = runtime->control->avail_min ? : 1;
	if (runtime->status->state == SNDRV_PCM_STATE_RUNNING)
		snd_pcm_update_hw_ptr(substream);
	avail = snd_pcm_capture_avail(runtime);
	while (size > 0) {
		snd_pcm_uframes_t frames, appl_ptr, appl_ofs;
		snd_pcm_uframes_t cont;
		if (!avail) {
			if (runtime->status->state ==
			    SNDRV_PCM_STATE_DRAINING) {
				snd_pcm_stop(substream, SNDRV_PCM_STATE_SETUP);
				goto _end_unlock;
			}
			if (nonblock) {
				err = -EAGAIN;
				goto _end_unlock;
			}
			runtime->twake = min_t(snd_pcm_uframes_t, size,
					runtime->control->avail_min ? : 1);
			err = wait_for_avail(substream, &avail);
			if (err < 0)
				goto _end_unlock;
			if (!avail)
				continue; /* draining */
		}
		frames = size > avail ? avail : size;
		cont = runtime->buffer_size - runtime->control->appl_ptr % runtime->buffer_size;
		if (frames > cont)
			frames = cont;
		if (snd_BUG_ON(!frames)) {
			runtime->twake = 0;
			snd_pcm_stream_unlock_irq(substream);
			return -EINVAL;
		}
		appl_ptr = runtime->control->appl_ptr;
		appl_ofs = appl_ptr % runtime->buffer_size;
		snd_pcm_stream_unlock_irq(substream);
		err = transfer(substream, appl_ofs, data, offset, frames);
		snd_pcm_stream_lock_irq(substream);
		if (err < 0)
			goto _end_unlock;
		switch (runtime->status->state) {
		case SNDRV_PCM_STATE_XRUN:
			err = -EPIPE;
			goto _end_unlock;
		case SNDRV_PCM_STATE_SUSPENDED:
			err = -ESTRPIPE;
			goto _end_unlock;
		default:
			break;
		}
		appl_ptr += frames;
		if (appl_ptr >= runtime->boundary)
			appl_ptr -= runtime->boundary;
		runtime->control->appl_ptr = appl_ptr;
		if (substream->ops->ack)
			substream->ops->ack(substream);

		offset += frames;
		size -= frames;
		xfer += frames;
		avail -= frames;
	}
 _end_unlock:
	runtime->twake = 0;
	if (xfer > 0 && err >= 0)
		snd_pcm_update_state(substream, runtime);
	snd_pcm_stream_unlock_irq(substream);
	return xfer > 0 ? (snd_pcm_sframes_t)xfer : err;
}

snd_pcm_sframes_t snd_pcm_lib_read(struct snd_pcm_substream *substream, void __user *buf, snd_pcm_uframes_t size)
{
	struct snd_pcm_runtime *runtime;
	int nonblock;
	int err;
	
	err = pcm_sanity_check(substream);
	if (err < 0)
		return err;
	runtime = substream->runtime;
	nonblock = !!(substream->f_flags & O_NONBLOCK);
	if (runtime->access != SNDRV_PCM_ACCESS_RW_INTERLEAVED)
		return -EINVAL;
	return snd_pcm_lib_read1(substream, (unsigned long)buf, size, nonblock, snd_pcm_lib_read_transfer);
}

EXPORT_SYMBOL(snd_pcm_lib_read);


/*
 * standard channel mapping helpers
 */

/* default channel maps for multi-channel playbacks, up to 8 channels */
const struct snd_pcm_chmap_elem snd_pcm_std_chmaps[] = {
	{ .channels = 1,
	  .map = { SNDRV_CHMAP_MONO } },
	{ .channels = 2,
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR } },
	{ .channels = 4,
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR,
		   SNDRV_CHMAP_RL, SNDRV_CHMAP_RR } },
	{ .channels = 6,
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR,
		   SNDRV_CHMAP_RL, SNDRV_CHMAP_RR,
		   SNDRV_CHMAP_FC, SNDRV_CHMAP_LFE } },
	{ .channels = 8,
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR,
		   SNDRV_CHMAP_RL, SNDRV_CHMAP_RR,
		   SNDRV_CHMAP_FC, SNDRV_CHMAP_LFE,
		   SNDRV_CHMAP_SL, SNDRV_CHMAP_SR } },
	{ }
};
EXPORT_SYMBOL_GPL(snd_pcm_std_chmaps);

/* alternative channel maps with CLFE <-> surround swapped for 6/8 channels */
const struct snd_pcm_chmap_elem snd_pcm_alt_chmaps[] = {
	{ .channels = 1,
	  .map = { SNDRV_CHMAP_MONO } },
	{ .channels = 2,
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR } },
	{ .channels = 4,
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR,
		   SNDRV_CHMAP_RL, SNDRV_CHMAP_RR } },
	{ .channels = 6,
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR,
		   SNDRV_CHMAP_FC, SNDRV_CHMAP_LFE,
		   SNDRV_CHMAP_RL, SNDRV_CHMAP_RR } },
	{ .channels = 8,
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR,
		   SNDRV_CHMAP_FC, SNDRV_CHMAP_LFE,
		   SNDRV_CHMAP_RL, SNDRV_CHMAP_RR,
		   SNDRV_CHMAP_SL, SNDRV_CHMAP_SR } },
	{ }
};
EXPORT_SYMBOL_GPL(snd_pcm_alt_chmaps);

static bool valid_chmap_channels(const struct snd_pcm_chmap *info, int ch)
{
	if (ch > info->max_channels)
		return false;
	return !info->channel_mask || (info->channel_mask & (1U << ch));
}

static int pcm_chmap_ctl_info(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_info *uinfo)
{
	struct snd_pcm_chmap *info = snd_kcontrol_chip(kcontrol);

	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 0;
	uinfo->count = info->max_channels;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = SNDRV_CHMAP_LAST;
	return 0;
}

/* get callback for channel map ctl element
 * stores the channel position firstly matching with the current channels
 */
static int pcm_chmap_ctl_get(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *ucontrol)
{
	struct snd_pcm_chmap *info = snd_kcontrol_chip(kcontrol);
	unsigned int idx = snd_ctl_get_ioffidx(kcontrol, &ucontrol->id);
	struct snd_pcm_substream *substream;
	const struct snd_pcm_chmap_elem *map;

	if (snd_BUG_ON(!info->chmap))
		return -EINVAL;
	substream = snd_pcm_chmap_substream(info, idx);
	if (!substream)
		return -ENODEV;
	memset(ucontrol->value.integer.value, 0,
	       sizeof(ucontrol->value.integer.value));
	if (!substream->runtime)
		return 0; /* no channels set */
	for (map = info->chmap; map->channels; map++) {
		int i;
		if (map->channels == substream->runtime->channels &&
		    valid_chmap_channels(info, map->channels)) {
			for (i = 0; i < map->channels; i++)
				ucontrol->value.integer.value[i] = map->map[i];
			return 0;
		}
	}
	return -EINVAL;
}

/* tlv callback for channel map ctl element
 * expands the pre-defined channel maps in a form of TLV
 */
static int pcm_chmap_ctl_tlv(struct snd_kcontrol *kcontrol, int op_flag,
			     unsigned int size, unsigned int __user *tlv)
{
	struct snd_pcm_chmap *info = snd_kcontrol_chip(kcontrol);
	const struct snd_pcm_chmap_elem *map;
	unsigned int __user *dst;
	int c, count = 0;

	if (snd_BUG_ON(!info->chmap))
		return -EINVAL;
	if (size < 8)
		return -ENOMEM;
	if (put_user(SNDRV_CTL_TLVT_CONTAINER, tlv))
		return -EFAULT;
	size -= 8;
	dst = tlv + 2;
	for (map = info->chmap; map->channels; map++) {
		int chs_bytes = map->channels * 4;
		if (!valid_chmap_channels(info, map->channels))
			continue;
		if (size < 8)
			return -ENOMEM;
		if (put_user(SNDRV_CTL_TLVT_CHMAP_FIXED, dst) ||
		    put_user(chs_bytes, dst + 1))
			return -EFAULT;
		dst += 2;
		size -= 8;
		count += 8;
		if (size < chs_bytes)
			return -ENOMEM;
		size -= chs_bytes;
		count += chs_bytes;
		for (c = 0; c < map->channels; c++) {
			if (put_user(map->map[c], dst))
				return -EFAULT;
			dst++;
		}
	}
	if (put_user(count, tlv + 1))
		return -EFAULT;
	return 0;
}

static void pcm_chmap_ctl_private_free(struct snd_kcontrol *kcontrol)
{
	struct snd_pcm_chmap *info = snd_kcontrol_chip(kcontrol);
	info->pcm->streams[info->stream].chmap_kctl = NULL;
	kfree(info);
}

/**
 * snd_pcm_add_chmap_ctls - create channel-mapping control elements
 * @pcm: the assigned PCM instance
 * @stream: stream direction
 * @chmap: channel map elements (for query)
 * @max_channels: the max number of channels for the stream
 * @private_value: the value passed to each kcontrol's private_value field
 * @info_ret: store struct snd_pcm_chmap instance if non-NULL
 *
 * Create channel-mapping control elements assigned to the given PCM stream(s).
 * Return: Zero if successful, or a negative error value.
 */
int snd_pcm_add_chmap_ctls(struct snd_pcm *pcm, int stream,
			   const struct snd_pcm_chmap_elem *chmap,
			   int max_channels,
			   unsigned long private_value,
			   struct snd_pcm_chmap **info_ret)
{
	struct snd_pcm_chmap *info;
	struct snd_kcontrol_new knew = {
		.iface = SNDRV_CTL_ELEM_IFACE_PCM,
		.access = SNDRV_CTL_ELEM_ACCESS_READ |
			SNDRV_CTL_ELEM_ACCESS_TLV_READ |
			SNDRV_CTL_ELEM_ACCESS_TLV_CALLBACK,
		.info = pcm_chmap_ctl_info,
		.get = pcm_chmap_ctl_get,
		.tlv.c = pcm_chmap_ctl_tlv,
	};
	int err;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
	info->pcm = pcm;
	info->stream = stream;
	info->chmap = chmap;
	info->max_channels = max_channels;
	if (stream == SNDRV_PCM_STREAM_PLAYBACK)
		knew.name = "Playback Channel Map";
	else
		knew.name = "Capture Channel Map";
	knew.device = pcm->device;
	knew.count = pcm->streams[stream].substream_count;
	knew.private_value = private_value;
	info->kctl = snd_ctl_new1(&knew, info);
	if (!info->kctl) {
		kfree(info);
		return -ENOMEM;
	}
	info->kctl->private_free = pcm_chmap_ctl_private_free;
	err = snd_ctl_add(pcm->card, info->kctl);
	if (err < 0)
		return err;
	pcm->streams[stream].chmap_kctl = info->kctl;
	if (info_ret)
		*info_ret = info;
	return 0;
}
EXPORT_SYMBOL_GPL(snd_pcm_add_chmap_ctls);
