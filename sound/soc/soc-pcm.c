/*
 * soc-pcm.c  --  ALSA SoC PCM
 *
 * Copyright 2005 Wolfson Microelectronics PLC.
 * Copyright 2005 Openedhand Ltd.
 * Copyright (C) 2010 Slimlogic Ltd.
 * Copyright (C) 2010 Texas Instruments Inc.
 *
 * Authors: Liam Girdwood <lrg@ti.com>
 *          Mark Brown <broonie@opensource.wolfsonmicro.com>
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/export.h>
#include <linux/debugfs.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dpcm.h>
#include <sound/initval.h>

/*
 * snd_soc_dai_stream_valid() - check if a DAI supports the given stream
 *
 * Returns true if the DAI supports the indicated stream type.
 */
static bool snd_soc_dai_stream_valid(struct snd_soc_dai *dai, int stream)
{
	struct snd_soc_pcm_stream *codec_stream;

	if (stream == SNDRV_PCM_STREAM_PLAYBACK)
		codec_stream = &dai->driver->playback;
	else
		codec_stream = &dai->driver->capture;

	/* If the codec specifies any rate at all, it supports the stream. */
	return codec_stream->rates;
}

/**
 * snd_soc_runtime_activate() - Increment active count for PCM runtime components
 * @rtd: ASoC PCM runtime that is activated
 * @stream: Direction of the PCM stream
 *
 * Increments the active count for all the DAIs and components attached to a PCM
 * runtime. Should typically be called when a stream is opened.
 *
 * Must be called with the rtd->pcm_mutex being held
 */
void snd_soc_runtime_activate(struct snd_soc_pcm_runtime *rtd, int stream)
{
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	int i;

	lockdep_assert_held(&rtd->pcm_mutex);

	if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
		cpu_dai->playback_active++;
		for (i = 0; i < rtd->num_codecs; i++)
			rtd->codec_dais[i]->playback_active++;
	} else {
		cpu_dai->capture_active++;
		for (i = 0; i < rtd->num_codecs; i++)
			rtd->codec_dais[i]->capture_active++;
	}

	cpu_dai->active++;
	cpu_dai->component->active++;
	for (i = 0; i < rtd->num_codecs; i++) {
		rtd->codec_dais[i]->active++;
		rtd->codec_dais[i]->component->active++;
	}
}

/**
 * snd_soc_runtime_deactivate() - Decrement active count for PCM runtime components
 * @rtd: ASoC PCM runtime that is deactivated
 * @stream: Direction of the PCM stream
 *
 * Decrements the active count for all the DAIs and components attached to a PCM
 * runtime. Should typically be called when a stream is closed.
 *
 * Must be called with the rtd->pcm_mutex being held
 */
void snd_soc_runtime_deactivate(struct snd_soc_pcm_runtime *rtd, int stream)
{
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	int i;

	lockdep_assert_held(&rtd->pcm_mutex);

	if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
		cpu_dai->playback_active--;
		for (i = 0; i < rtd->num_codecs; i++)
			rtd->codec_dais[i]->playback_active--;
	} else {
		cpu_dai->capture_active--;
		for (i = 0; i < rtd->num_codecs; i++)
			rtd->codec_dais[i]->capture_active--;
	}

	cpu_dai->active--;
	cpu_dai->component->active--;
	for (i = 0; i < rtd->num_codecs; i++) {
		rtd->codec_dais[i]->component->active--;
		rtd->codec_dais[i]->active--;
	}
}

/**
 * snd_soc_runtime_ignore_pmdown_time() - Check whether to ignore the power down delay
 * @rtd: The ASoC PCM runtime that should be checked.
 *
 * This function checks whether the power down delay should be ignored for a
 * specific PCM runtime. Returns true if the delay is 0, if it the DAI link has
 * been configured to ignore the delay, or if none of the components benefits
 * from having the delay.
 */
bool snd_soc_runtime_ignore_pmdown_time(struct snd_soc_pcm_runtime *rtd)
{
	int i;
	bool ignore = true;

	if (!rtd->pmdown_time || rtd->dai_link->ignore_pmdown_time)
		return true;

	for (i = 0; i < rtd->num_codecs; i++)
		ignore &= rtd->codec_dais[i]->component->ignore_pmdown_time;

	return rtd->cpu_dai->component->ignore_pmdown_time && ignore;
}

/**
 * snd_soc_set_runtime_hwparams - set the runtime hardware parameters
 * @substream: the pcm substream
 * @hw: the hardware parameters
 *
 * Sets the substream runtime hardware parameters.
 */
int snd_soc_set_runtime_hwparams(struct snd_pcm_substream *substream,
	const struct snd_pcm_hardware *hw)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	runtime->hw.info = hw->info;
	runtime->hw.formats = hw->formats;
	runtime->hw.period_bytes_min = hw->period_bytes_min;
	runtime->hw.period_bytes_max = hw->period_bytes_max;
	runtime->hw.periods_min = hw->periods_min;
	runtime->hw.periods_max = hw->periods_max;
	runtime->hw.buffer_bytes_max = hw->buffer_bytes_max;
	runtime->hw.fifo_size = hw->fifo_size;
	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_set_runtime_hwparams);

static int soc_pcm_apply_symmetry(struct snd_pcm_substream *substream,
					struct snd_soc_dai *soc_dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	int ret;

	if (soc_dai->rate && (soc_dai->driver->symmetric_rates ||
				rtd->dai_link->symmetric_rates)) {
		dev_dbg(soc_dai->dev, "ASoC: Symmetry forces %dHz rate\n",
				soc_dai->rate);

		ret = snd_pcm_hw_constraint_minmax(substream->runtime,
						SNDRV_PCM_HW_PARAM_RATE,
						soc_dai->rate, soc_dai->rate);
		if (ret < 0) {
			dev_err(soc_dai->dev,
				"ASoC: Unable to apply rate constraint: %d\n",
				ret);
			return ret;
		}
	}

	if (soc_dai->channels && (soc_dai->driver->symmetric_channels ||
				rtd->dai_link->symmetric_channels)) {
		dev_dbg(soc_dai->dev, "ASoC: Symmetry forces %d channel(s)\n",
				soc_dai->channels);

		ret = snd_pcm_hw_constraint_minmax(substream->runtime,
						SNDRV_PCM_HW_PARAM_CHANNELS,
						soc_dai->channels,
						soc_dai->channels);
		if (ret < 0) {
			dev_err(soc_dai->dev,
				"ASoC: Unable to apply channel symmetry constraint: %d\n",
				ret);
			return ret;
		}
	}

	if (soc_dai->sample_bits && (soc_dai->driver->symmetric_samplebits ||
				rtd->dai_link->symmetric_samplebits)) {
		dev_dbg(soc_dai->dev, "ASoC: Symmetry forces %d sample bits\n",
				soc_dai->sample_bits);

		ret = snd_pcm_hw_constraint_minmax(substream->runtime,
						SNDRV_PCM_HW_PARAM_SAMPLE_BITS,
						soc_dai->sample_bits,
						soc_dai->sample_bits);
		if (ret < 0) {
			dev_err(soc_dai->dev,
				"ASoC: Unable to apply sample bits symmetry constraint: %d\n",
				ret);
			return ret;
		}
	}

	return 0;
}

static int soc_pcm_params_symmetry(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	unsigned int rate, channels, sample_bits, symmetry, i;

	rate = params_rate(params);
	channels = params_channels(params);
	sample_bits = snd_pcm_format_physical_width(params_format(params));

	/* reject unmatched parameters when applying symmetry */
	symmetry = cpu_dai->driver->symmetric_rates ||
		rtd->dai_link->symmetric_rates;

	for (i = 0; i < rtd->num_codecs; i++)
		symmetry |= rtd->codec_dais[i]->driver->symmetric_rates;

	if (symmetry && cpu_dai->rate && cpu_dai->rate != rate) {
		dev_err(rtd->dev, "ASoC: unmatched rate symmetry: %d - %d\n",
				cpu_dai->rate, rate);
		return -EINVAL;
	}

	symmetry = cpu_dai->driver->symmetric_channels ||
		rtd->dai_link->symmetric_channels;

	for (i = 0; i < rtd->num_codecs; i++)
		symmetry |= rtd->codec_dais[i]->driver->symmetric_channels;

	if (symmetry && cpu_dai->channels && cpu_dai->channels != channels) {
		dev_err(rtd->dev, "ASoC: unmatched channel symmetry: %d - %d\n",
				cpu_dai->channels, channels);
		return -EINVAL;
	}

	symmetry = cpu_dai->driver->symmetric_samplebits ||
		rtd->dai_link->symmetric_samplebits;

	for (i = 0; i < rtd->num_codecs; i++)
		symmetry |= rtd->codec_dais[i]->driver->symmetric_samplebits;

	if (symmetry && cpu_dai->sample_bits && cpu_dai->sample_bits != sample_bits) {
		dev_err(rtd->dev, "ASoC: unmatched sample bits symmetry: %d - %d\n",
				cpu_dai->sample_bits, sample_bits);
		return -EINVAL;
	}

	return 0;
}

static bool soc_pcm_has_symmetry(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai_driver *cpu_driver = rtd->cpu_dai->driver;
	struct snd_soc_dai_link *link = rtd->dai_link;
	unsigned int symmetry, i;

	symmetry = cpu_driver->symmetric_rates || link->symmetric_rates ||
		cpu_driver->symmetric_channels || link->symmetric_channels ||
		cpu_driver->symmetric_samplebits || link->symmetric_samplebits;

	for (i = 0; i < rtd->num_codecs; i++)
		symmetry = symmetry ||
			rtd->codec_dais[i]->driver->symmetric_rates ||
			rtd->codec_dais[i]->driver->symmetric_channels ||
			rtd->codec_dais[i]->driver->symmetric_samplebits;

	return symmetry;
}

static void soc_pcm_set_msb(struct snd_pcm_substream *substream, int bits)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	int ret;

	if (!bits)
		return;

	ret = snd_pcm_hw_constraint_msbits(substream->runtime, 0, 0, bits);
	if (ret != 0)
		dev_warn(rtd->dev, "ASoC: Failed to set MSB %d: %d\n",
				 bits, ret);
}

static void soc_pcm_apply_msb(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_dai *codec_dai;
	int i;
	unsigned int bits = 0, cpu_bits;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		for (i = 0; i < rtd->num_codecs; i++) {
			codec_dai = rtd->codec_dais[i];
			if (codec_dai->driver->playback.sig_bits == 0) {
				bits = 0;
				break;
			}
			bits = max(codec_dai->driver->playback.sig_bits, bits);
		}
		cpu_bits = cpu_dai->driver->playback.sig_bits;
	} else {
		for (i = 0; i < rtd->num_codecs; i++) {
			codec_dai = rtd->codec_dais[i];
			if (codec_dai->driver->capture.sig_bits == 0) {
				bits = 0;
				break;
			}
			bits = max(codec_dai->driver->capture.sig_bits, bits);
		}
		cpu_bits = cpu_dai->driver->capture.sig_bits;
	}

	soc_pcm_set_msb(substream, bits);
	soc_pcm_set_msb(substream, cpu_bits);
}

static void soc_pcm_init_runtime_hw(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_pcm_hardware *hw = &runtime->hw;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai_driver *cpu_dai_drv = rtd->cpu_dai->driver;
	struct snd_soc_dai_driver *codec_dai_drv;
	struct snd_soc_pcm_stream *codec_stream;
	struct snd_soc_pcm_stream *cpu_stream;
	unsigned int chan_min = 0, chan_max = UINT_MAX;
	unsigned int rate_min = 0, rate_max = UINT_MAX;
	unsigned int rates = UINT_MAX;
	u64 formats = ULLONG_MAX;
	int i;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		cpu_stream = &cpu_dai_drv->playback;
	else
		cpu_stream = &cpu_dai_drv->capture;

	/* first calculate min/max only for CODECs in the DAI link */
	for (i = 0; i < rtd->num_codecs; i++) {

		/*
		 * Skip CODECs which don't support the current stream type.
		 * Otherwise, since the rate, channel, and format values will
		 * zero in that case, we would have no usable settings left,
		 * causing the resulting setup to fail.
		 * At least one CODEC should match, otherwise we should have
		 * bailed out on a higher level, since there would be no
		 * CODEC to support the transfer direction in that case.
		 */
		if (!snd_soc_dai_stream_valid(rtd->codec_dais[i],
					      substream->stream))
			continue;

		codec_dai_drv = rtd->codec_dais[i]->driver;
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			codec_stream = &codec_dai_drv->playback;
		else
			codec_stream = &codec_dai_drv->capture;
		chan_min = max(chan_min, codec_stream->channels_min);
		chan_max = min(chan_max, codec_stream->channels_max);
		rate_min = max(rate_min, codec_stream->rate_min);
		rate_max = min_not_zero(rate_max, codec_stream->rate_max);
		formats &= codec_stream->formats;
		rates = snd_pcm_rate_mask_intersect(codec_stream->rates, rates);
	}

	/*
	 * chan min/max cannot be enforced if there are multiple CODEC DAIs
	 * connected to a single CPU DAI, use CPU DAI's directly and let
	 * channel allocation be fixed up later
	 */
	if (rtd->num_codecs > 1) {
		chan_min = cpu_stream->channels_min;
		chan_max = cpu_stream->channels_max;
	}

	hw->channels_min = max(chan_min, cpu_stream->channels_min);
	hw->channels_max = min(chan_max, cpu_stream->channels_max);
	if (hw->formats)
		hw->formats &= formats & cpu_stream->formats;
	else
		hw->formats = formats & cpu_stream->formats;
	hw->rates = snd_pcm_rate_mask_intersect(rates, cpu_stream->rates);

	snd_pcm_limit_hw_rates(runtime);

	hw->rate_min = max(hw->rate_min, cpu_stream->rate_min);
	hw->rate_min = max(hw->rate_min, rate_min);
	hw->rate_max = min_not_zero(hw->rate_max, cpu_stream->rate_max);
	hw->rate_max = min_not_zero(hw->rate_max, rate_max);
}

/*
 * Called by ALSA when a PCM substream is opened, the runtime->hw record is
 * then initialized and any private data can be allocated. This also calls
 * startup for the cpu DAI, platform, machine and codec DAI.
 */
int soc_pcm_open(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_soc_platform *platform = rtd->platform;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_dai *codec_dai;
	const char *codec_dai_name = "multicodec";
	int i, ret = 0;

	pinctrl_pm_select_default_state(cpu_dai->dev);
	for (i = 0; i < rtd->num_codecs; i++)
		pinctrl_pm_select_default_state(rtd->codec_dais[i]->dev);
	pm_runtime_get_sync(cpu_dai->dev);
	for (i = 0; i < rtd->num_codecs; i++)
		pm_runtime_get_sync(rtd->codec_dais[i]->dev);
	pm_runtime_get_sync(platform->dev);

	mutex_lock_nested(&rtd->pcm_mutex, rtd->pcm_subclass);

	/* startup the audio subsystem */
	if (cpu_dai->driver->ops && cpu_dai->driver->ops->startup) {
		ret = cpu_dai->driver->ops->startup(substream, cpu_dai);
		if (ret < 0) {
			dev_err(cpu_dai->dev, "ASoC: can't open interface"
				" %s: %d\n", cpu_dai->name, ret);
			goto out;
		}
	}

	if (platform->driver->ops && platform->driver->ops->open) {
		ret = platform->driver->ops->open(substream);
		if (ret < 0) {
			dev_err(platform->dev, "ASoC: can't open platform"
				" %s: %d\n", platform->component.name, ret);
			goto platform_err;
		}
	}

	for (i = 0; i < rtd->num_codecs; i++) {
		codec_dai = rtd->codec_dais[i];
		if (codec_dai->driver->ops && codec_dai->driver->ops->startup) {
			ret = codec_dai->driver->ops->startup(substream,
							      codec_dai);
			if (ret < 0) {
				dev_err(codec_dai->dev,
					"ASoC: can't open codec %s: %d\n",
					codec_dai->name, ret);
				goto codec_dai_err;
			}
		}

		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			codec_dai->tx_mask = 0;
		else
			codec_dai->rx_mask = 0;
	}

	if (rtd->dai_link->ops && rtd->dai_link->ops->startup) {
		ret = rtd->dai_link->ops->startup(substream);
		if (ret < 0) {
			pr_err("ASoC: %s startup failed: %d\n",
			       rtd->dai_link->name, ret);
			goto machine_err;
		}
	}

	/* Dynamic PCM DAI links compat checks use dynamic capabilities */
	if (rtd->dai_link->dynamic || rtd->dai_link->no_pcm)
		goto dynamic;

	/* Check that the codec and cpu DAIs are compatible */
	soc_pcm_init_runtime_hw(substream);

	if (rtd->num_codecs == 1)
		codec_dai_name = rtd->codec_dai->name;

	if (soc_pcm_has_symmetry(substream))
		runtime->hw.info |= SNDRV_PCM_INFO_JOINT_DUPLEX;

	ret = -EINVAL;
	if (!runtime->hw.rates) {
		printk(KERN_ERR "ASoC: %s <-> %s No matching rates\n",
			codec_dai_name, cpu_dai->name);
		goto config_err;
	}
	if (!runtime->hw.formats) {
		printk(KERN_ERR "ASoC: %s <-> %s No matching formats\n",
			codec_dai_name, cpu_dai->name);
		goto config_err;
	}
	if (!runtime->hw.channels_min || !runtime->hw.channels_max ||
	    runtime->hw.channels_min > runtime->hw.channels_max) {
		printk(KERN_ERR "ASoC: %s <-> %s No matching channels\n",
				codec_dai_name, cpu_dai->name);
		goto config_err;
	}

	soc_pcm_apply_msb(substream);

	/* Symmetry only applies if we've already got an active stream. */
	if (cpu_dai->active) {
		ret = soc_pcm_apply_symmetry(substream, cpu_dai);
		if (ret != 0)
			goto config_err;
	}

	for (i = 0; i < rtd->num_codecs; i++) {
		if (rtd->codec_dais[i]->active) {
			ret = soc_pcm_apply_symmetry(substream,
						     rtd->codec_dais[i]);
			if (ret != 0)
				goto config_err;
		}
	}

	pr_debug("ASoC: %s <-> %s info:\n",
			codec_dai_name, cpu_dai->name);
	pr_debug("ASoC: rate mask 0x%x\n", runtime->hw.rates);
	pr_debug("ASoC: min ch %d max ch %d\n", runtime->hw.channels_min,
		 runtime->hw.channels_max);
	pr_debug("ASoC: min rate %d max rate %d\n", runtime->hw.rate_min,
		 runtime->hw.rate_max);

dynamic:

	snd_soc_runtime_activate(rtd, substream->stream);

	mutex_unlock(&rtd->pcm_mutex);
	return 0;

config_err:
	if (rtd->dai_link->ops && rtd->dai_link->ops->shutdown)
		rtd->dai_link->ops->shutdown(substream);

machine_err:
	i = rtd->num_codecs;

codec_dai_err:
	while (--i >= 0) {
		codec_dai = rtd->codec_dais[i];
		if (codec_dai->driver->ops->shutdown)
			codec_dai->driver->ops->shutdown(substream, codec_dai);
	}

	if (platform->driver->ops && platform->driver->ops->close)
		platform->driver->ops->close(substream);

platform_err:
	if (cpu_dai->driver->ops->shutdown)
		cpu_dai->driver->ops->shutdown(substream, cpu_dai);
out:
	mutex_unlock(&rtd->pcm_mutex);

	pm_runtime_put(platform->dev);
	for (i = 0; i < rtd->num_codecs; i++)
		pm_runtime_put(rtd->codec_dais[i]->dev);
	pm_runtime_put(cpu_dai->dev);
	for (i = 0; i < rtd->num_codecs; i++) {
		if (!rtd->codec_dais[i]->active)
			pinctrl_pm_select_sleep_state(rtd->codec_dais[i]->dev);
	}
	if (!cpu_dai->active)
		pinctrl_pm_select_sleep_state(cpu_dai->dev);

	return ret;
}

/*
 * Power down the audio subsystem pmdown_time msecs after close is called.
 * This is to ensure there are no pops or clicks in between any music tracks
 * due to DAPM power cycling.
 */
static void close_delayed_work(struct work_struct *work)
{
	struct snd_soc_pcm_runtime *rtd =
			container_of(work, struct snd_soc_pcm_runtime, delayed_work.work);
	struct snd_soc_dai *codec_dai = rtd->codec_dais[0];

	mutex_lock_nested(&rtd->pcm_mutex, rtd->pcm_subclass);

	dev_dbg(rtd->dev, "ASoC: pop wq checking: %s status: %s waiting: %s\n",
		 codec_dai->driver->playback.stream_name,
		 codec_dai->playback_active ? "active" : "inactive",
		 rtd->pop_wait ? "yes" : "no");

	/* are we waiting on this codec DAI stream */
	if (rtd->pop_wait == 1) {
		rtd->pop_wait = 0;
		snd_soc_dapm_stream_event(rtd, SNDRV_PCM_STREAM_PLAYBACK,
					  SND_SOC_DAPM_STREAM_STOP);
	}

	mutex_unlock(&rtd->pcm_mutex);
}

/*
 * Called by ALSA when a PCM substream is closed. Private data can be
 * freed here. The cpu DAI, codec DAI, machine and platform are also
 * shutdown.
 */
int soc_pcm_close(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_platform *platform = rtd->platform;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_dai *codec_dai;
	int i;

	mutex_lock_nested(&rtd->pcm_mutex, rtd->pcm_subclass);

	snd_soc_runtime_deactivate(rtd, substream->stream);

	/* clear the corresponding DAIs rate when inactive */
	if (!cpu_dai->active)
		cpu_dai->rate = 0;

	for (i = 0; i < rtd->num_codecs; i++) {
		codec_dai = rtd->codec_dais[i];
		if (!codec_dai->active)
			codec_dai->rate = 0;
	}

	snd_soc_dai_digital_mute(cpu_dai, 1, substream->stream);

	if (cpu_dai->driver->ops->shutdown)
		cpu_dai->driver->ops->shutdown(substream, cpu_dai);

	for (i = 0; i < rtd->num_codecs; i++) {
		codec_dai = rtd->codec_dais[i];
		if (codec_dai->driver->ops->shutdown)
			codec_dai->driver->ops->shutdown(substream, codec_dai);
	}

	if (rtd->dai_link->ops && rtd->dai_link->ops->shutdown)
		rtd->dai_link->ops->shutdown(substream);

	if (platform->driver->ops && platform->driver->ops->close)
		platform->driver->ops->close(substream);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		if (snd_soc_runtime_ignore_pmdown_time(rtd)) {
			/* powered down playback stream now */
			snd_soc_dapm_stream_event(rtd,
						  SNDRV_PCM_STREAM_PLAYBACK,
						  SND_SOC_DAPM_STREAM_STOP);
		} else {
			/* start delayed pop wq here for playback streams */
			rtd->pop_wait = 1;
			queue_delayed_work(system_power_efficient_wq,
					   &rtd->delayed_work,
					   msecs_to_jiffies(rtd->pmdown_time));
		}
	} else {
		/* capture streams can be powered down now */
		snd_soc_dapm_stream_event(rtd, SNDRV_PCM_STREAM_CAPTURE,
					  SND_SOC_DAPM_STREAM_STOP);
	}

	mutex_unlock(&rtd->pcm_mutex);

	pm_runtime_put(platform->dev);
	for (i = 0; i < rtd->num_codecs; i++)
		pm_runtime_put(rtd->codec_dais[i]->dev);
	pm_runtime_put(cpu_dai->dev);
	for (i = 0; i < rtd->num_codecs; i++) {
		if (!rtd->codec_dais[i]->active)
			pinctrl_pm_select_sleep_state(rtd->codec_dais[i]->dev);
	}
	if (!cpu_dai->active)
		pinctrl_pm_select_sleep_state(cpu_dai->dev);

	return 0;
}

/*
 * Called by ALSA when the PCM substream is prepared, can set format, sample
 * rate, etc.  This function is non atomic and can be called multiple times,
 * it can refer to the runtime info.
 */
int soc_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_platform *platform = rtd->platform;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_dai *codec_dai;
	int i, ret = 0;

	mutex_lock_nested(&rtd->pcm_mutex, rtd->pcm_subclass);

	if (rtd->dai_link->ops && rtd->dai_link->ops->prepare) {
		ret = rtd->dai_link->ops->prepare(substream);
		if (ret < 0) {
			dev_err(rtd->card->dev, "ASoC: machine prepare error:"
				" %d\n", ret);
			goto out;
		}
	}

	if (platform->driver->ops && platform->driver->ops->prepare) {
		ret = platform->driver->ops->prepare(substream);
		if (ret < 0) {
			dev_err(platform->dev, "ASoC: platform prepare error:"
				" %d\n", ret);
			goto out;
		}
	}

	for (i = 0; i < rtd->num_codecs; i++) {
		codec_dai = rtd->codec_dais[i];
		if (codec_dai->driver->ops && codec_dai->driver->ops->prepare) {
			ret = codec_dai->driver->ops->prepare(substream,
							      codec_dai);
			if (ret < 0) {
				dev_err(codec_dai->dev,
					"ASoC: codec DAI prepare error: %d\n",
					ret);
				goto out;
			}
		}
	}

	if (cpu_dai->driver->ops && cpu_dai->driver->ops->prepare) {
		ret = cpu_dai->driver->ops->prepare(substream, cpu_dai);
		if (ret < 0) {
			dev_err(cpu_dai->dev,
				"ASoC: cpu DAI prepare error: %d\n", ret);
			goto out;
		}
	}

	/* cancel any delayed stream shutdown that is pending */
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK &&
	    rtd->pop_wait) {
		rtd->pop_wait = 0;
		cancel_delayed_work(&rtd->delayed_work);
	}

	snd_soc_dapm_stream_event(rtd, substream->stream,
			SND_SOC_DAPM_STREAM_START);

	for (i = 0; i < rtd->num_codecs; i++)
		snd_soc_dai_digital_mute(rtd->codec_dais[i], 0,
					 substream->stream);
	snd_soc_dai_digital_mute(cpu_dai, 0, substream->stream);

out:
	mutex_unlock(&rtd->pcm_mutex);
	return ret;
}

static void soc_pcm_codec_params_fixup(struct snd_pcm_hw_params *params,
				       unsigned int mask)
{
	struct snd_interval *interval;
	int channels = hweight_long(mask);

	interval = hw_param_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS);
	interval->min = channels;
	interval->max = channels;
}

int soc_dai_hw_params(struct snd_pcm_substream *substream,
		      struct snd_pcm_hw_params *params,
		      struct snd_soc_dai *dai)
{
	int ret;

	if (dai->driver->ops && dai->driver->ops->hw_params) {
		ret = dai->driver->ops->hw_params(substream, params, dai);
		if (ret < 0) {
			dev_err(dai->dev, "ASoC: can't set %s hw params: %d\n",
				dai->name, ret);
			return ret;
		}
	}

	return 0;
}

/*
 * Called by ALSA when the hardware params are set by application. This
 * function can also be called multiple times and can allocate buffers
 * (using snd_pcm_lib_* ). It's non-atomic.
 */
int soc_pcm_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_platform *platform = rtd->platform;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	int i, ret = 0;

	mutex_lock_nested(&rtd->pcm_mutex, rtd->pcm_subclass);

	ret = soc_pcm_params_symmetry(substream, params);
	if (ret)
		goto out;

	if (rtd->dai_link->ops && rtd->dai_link->ops->hw_params) {
		ret = rtd->dai_link->ops->hw_params(substream, params);
		if (ret < 0) {
			dev_err(rtd->card->dev, "ASoC: machine hw_params"
				" failed: %d\n", ret);
			goto out;
		}
	}

	for (i = 0; i < rtd->num_codecs; i++) {
		struct snd_soc_dai *codec_dai = rtd->codec_dais[i];
		struct snd_pcm_hw_params codec_params;

		/*
		 * Skip CODECs which don't support the current stream type,
		 * the idea being that if a CODEC is not used for the currently
		 * set up transfer direction, it should not need to be
		 * configured, especially since the configuration used might
		 * not even be supported by that CODEC. There may be cases
		 * however where a CODEC needs to be set up although it is
		 * actually not being used for the transfer, e.g. if a
		 * capture-only CODEC is acting as an LRCLK and/or BCLK master
		 * for the DAI link including a playback-only CODEC.
		 * If this becomes necessary, we will have to augment the
		 * machine driver setup with information on how to act, so
		 * we can do the right thing here.
		 */
		if (!snd_soc_dai_stream_valid(codec_dai, substream->stream))
			continue;

		/* copy params for each codec */
		codec_params = *params;

		/* fixup params based on TDM slot masks */
		if (codec_dai->tx_mask)
			soc_pcm_codec_params_fixup(&codec_params,
						   codec_dai->tx_mask);
		if (codec_dai->rx_mask)
			soc_pcm_codec_params_fixup(&codec_params,
						   codec_dai->rx_mask);

		ret = soc_dai_hw_params(substream, &codec_params, codec_dai);
		if(ret < 0)
			goto codec_err;

		codec_dai->rate = params_rate(&codec_params);
		codec_dai->channels = params_channels(&codec_params);
		codec_dai->sample_bits = snd_pcm_format_physical_width(
						params_format(&codec_params));
	}

	ret = soc_dai_hw_params(substream, params, cpu_dai);
	if (ret < 0)
		goto interface_err;

	if (platform->driver->ops && platform->driver->ops->hw_params) {
		ret = platform->driver->ops->hw_params(substream, params);
		if (ret < 0) {
			dev_err(platform->dev, "ASoC: %s hw params failed: %d\n",
			       platform->component.name, ret);
			goto platform_err;
		}
	}

	/* store the parameters for each DAIs */
	cpu_dai->rate = params_rate(params);
	cpu_dai->channels = params_channels(params);
	cpu_dai->sample_bits =
		snd_pcm_format_physical_width(params_format(params));

out:
	mutex_unlock(&rtd->pcm_mutex);
	return ret;

platform_err:
	if (cpu_dai->driver->ops && cpu_dai->driver->ops->hw_free)
		cpu_dai->driver->ops->hw_free(substream, cpu_dai);

interface_err:
	i = rtd->num_codecs;

codec_err:
	while (--i >= 0) {
		struct snd_soc_dai *codec_dai = rtd->codec_dais[i];
		if (codec_dai->driver->ops && codec_dai->driver->ops->hw_free)
			codec_dai->driver->ops->hw_free(substream, codec_dai);
		codec_dai->rate = 0;
	}

	if (rtd->dai_link->ops && rtd->dai_link->ops->hw_free)
		rtd->dai_link->ops->hw_free(substream);

	mutex_unlock(&rtd->pcm_mutex);
	return ret;
}

/*
 * Frees resources allocated by hw_params, can be called multiple times
 */
int soc_pcm_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_platform *platform = rtd->platform;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_dai *codec_dai;
	bool playback = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
	int i;

	mutex_lock_nested(&rtd->pcm_mutex, rtd->pcm_subclass);

	/* clear the corresponding DAIs parameters when going to be inactive */
	if (cpu_dai->active == 1) {
		cpu_dai->rate = 0;
		cpu_dai->channels = 0;
		cpu_dai->sample_bits = 0;
	}

	for (i = 0; i < rtd->num_codecs; i++) {
		codec_dai = rtd->codec_dais[i];
		if (codec_dai->active == 1) {
			codec_dai->rate = 0;
			codec_dai->channels = 0;
			codec_dai->sample_bits = 0;
		}
	}

	/* apply codec digital mute */
	for (i = 0; i < rtd->num_codecs; i++) {
		if ((playback && rtd->codec_dais[i]->playback_active == 1) ||
		    (!playback && rtd->codec_dais[i]->capture_active == 1))
			snd_soc_dai_digital_mute(rtd->codec_dais[i], 1,
						 substream->stream);
	}

	/* free any machine hw params */
	if (rtd->dai_link->ops && rtd->dai_link->ops->hw_free)
		rtd->dai_link->ops->hw_free(substream);

	/* free any DMA resources */
	if (platform->driver->ops && platform->driver->ops->hw_free)
		platform->driver->ops->hw_free(substream);

	/* now free hw params for the DAIs  */
	for (i = 0; i < rtd->num_codecs; i++) {
		codec_dai = rtd->codec_dais[i];
		if (codec_dai->driver->ops && codec_dai->driver->ops->hw_free)
			codec_dai->driver->ops->hw_free(substream, codec_dai);
	}

	if (cpu_dai->driver->ops && cpu_dai->driver->ops->hw_free)
		cpu_dai->driver->ops->hw_free(substream, cpu_dai);

	mutex_unlock(&rtd->pcm_mutex);
	return 0;
}

int soc_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_platform *platform = rtd->platform;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_dai *codec_dai;
	int i, ret;

	for (i = 0; i < rtd->num_codecs; i++) {
		codec_dai = rtd->codec_dais[i];
		if (codec_dai->driver->ops && codec_dai->driver->ops->trigger) {
			ret = codec_dai->driver->ops->trigger(substream,
							      cmd, codec_dai);
			if (ret < 0)
				return ret;
		}
	}

	if (platform->driver->ops && platform->driver->ops->trigger) {
		ret = platform->driver->ops->trigger(substream, cmd);
		if (ret < 0)
			return ret;
	}

	if (cpu_dai->driver->ops && cpu_dai->driver->ops->trigger) {
		ret = cpu_dai->driver->ops->trigger(substream, cmd, cpu_dai);
		if (ret < 0)
			return ret;
	}

	if (rtd->dai_link->ops && rtd->dai_link->ops->trigger) {
		ret = rtd->dai_link->ops->trigger(substream, cmd);
		if (ret < 0)
			return ret;
	}

	return 0;
}

/*
 * soc level wrapper for pointer callback
 * If cpu_dai, codec_dai, platform driver has the delay callback, than
 * the runtime->delay will be updated accordingly.
 */
static snd_pcm_uframes_t soc_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_platform *platform = rtd->platform;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_dai *codec_dai;
	struct snd_pcm_runtime *runtime = substream->runtime;
	snd_pcm_uframes_t offset = 0;
	snd_pcm_sframes_t delay = 0;
	snd_pcm_sframes_t codec_delay = 0;
	int i;

	if (platform->driver->ops && platform->driver->ops->pointer)
		offset = platform->driver->ops->pointer(substream);

	if (cpu_dai->driver->ops && cpu_dai->driver->ops->delay)
		delay += cpu_dai->driver->ops->delay(substream, cpu_dai);

	for (i = 0; i < rtd->num_codecs; i++) {
		codec_dai = rtd->codec_dais[i];
		if (codec_dai->driver->ops && codec_dai->driver->ops->delay)
			codec_delay = max(codec_delay,
					codec_dai->driver->ops->delay(substream,
								    codec_dai));
	}
	delay += codec_delay;

	/*
	 * None of the existing platform drivers implement delay(), so
	 * for now the codec_dai of first multicodec entry is used
	 */
	if (platform->driver->delay)
		delay += platform->driver->delay(substream, rtd->codec_dais[0]);

	runtime->delay = delay;

	return offset;
}

static int soc_pcm_ioctl(struct snd_pcm_substream *substream,
		     unsigned int cmd, void *arg)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_platform *platform = rtd->platform;

	if (platform->driver->ops && platform->driver->ops->ioctl)
		return platform->driver->ops->ioctl(substream, cmd, arg);
	return snd_pcm_lib_ioctl(substream, cmd, arg);
}

/* create a new pcm */
int soc_new_pcm(struct snd_soc_pcm_runtime *rtd, int num)
{
	struct snd_soc_platform *platform = rtd->platform;
	struct snd_soc_dai *codec_dai;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_pcm *pcm;
	char new_name[64];
	int ret = 0, playback = 0, capture = 0;
	int i;

	if (rtd->dai_link->dynamic || rtd->dai_link->no_pcm) {
		playback = rtd->dai_link->dpcm_playback;
		capture = rtd->dai_link->dpcm_capture;
	} else {
		for (i = 0; i < rtd->num_codecs; i++) {
			codec_dai = rtd->codec_dais[i];
			if (codec_dai->driver->playback.channels_min)
				playback = 1;
			if (codec_dai->driver->capture.channels_min)
				capture = 1;
		}

		capture = capture && cpu_dai->driver->capture.channels_min;
		playback = playback && cpu_dai->driver->playback.channels_min;
	}

	if (rtd->dai_link->playback_only) {
		playback = 1;
		capture = 0;
	}

	if (rtd->dai_link->capture_only) {
		playback = 0;
		capture = 1;
	}

	/* create the PCM */
	if (rtd->dai_link->no_pcm) {
		snprintf(new_name, sizeof(new_name), "(%s)",
			rtd->dai_link->stream_name);

		ret = snd_pcm_new_internal(rtd->card->snd_card, new_name, num,
				playback, capture, &pcm);
	} else {
		if (rtd->dai_link->dynamic)
			snprintf(new_name, sizeof(new_name), "%s (*)",
				rtd->dai_link->stream_name);
		else
			snprintf(new_name, sizeof(new_name), "%s %s-%d",
				rtd->dai_link->stream_name,
				(rtd->num_codecs > 1) ?
				"multicodec" : rtd->codec_dai->name, num);

		ret = snd_pcm_new(rtd->card->snd_card, new_name, num, playback,
			capture, &pcm);
	}
	if (ret < 0) {
		dev_err(rtd->card->dev, "ASoC: can't create pcm for %s\n",
			rtd->dai_link->name);
		return ret;
	}
	dev_dbg(rtd->card->dev, "ASoC: registered pcm #%d %s\n",num, new_name);

	/* DAPM dai link stream work */
	INIT_DELAYED_WORK(&rtd->delayed_work, close_delayed_work);

	pcm->nonatomic = rtd->dai_link->nonatomic;
	rtd->pcm = pcm;
	pcm->private_data = rtd;

	if (rtd->dai_link->no_pcm) {
		if (playback)
			pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream->private_data = rtd;
		if (capture)
			pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream->private_data = rtd;
		goto out;
	}

	/* ASoC PCM operations */
	if (rtd->dai_link->dynamic) {
		rtd->ops.open		= dpcm_fe_dai_open;
		rtd->ops.hw_params	= dpcm_fe_dai_hw_params;
		rtd->ops.prepare	= dpcm_fe_dai_prepare;
		rtd->ops.trigger	= dpcm_fe_dai_trigger;
		rtd->ops.hw_free	= dpcm_fe_dai_hw_free;
		rtd->ops.close		= dpcm_fe_dai_close;
		rtd->ops.pointer	= soc_pcm_pointer;
		rtd->ops.ioctl		= soc_pcm_ioctl;
	} else {
		rtd->ops.open		= soc_pcm_open;
		rtd->ops.hw_params	= soc_pcm_hw_params;
		rtd->ops.prepare	= soc_pcm_prepare;
		rtd->ops.trigger	= soc_pcm_trigger;
		rtd->ops.hw_free	= soc_pcm_hw_free;
		rtd->ops.close		= soc_pcm_close;
		rtd->ops.pointer	= soc_pcm_pointer;
		rtd->ops.ioctl		= soc_pcm_ioctl;
	}

	if (platform->driver->ops) {
		rtd->ops.ack		= platform->driver->ops->ack;
		rtd->ops.copy		= platform->driver->ops->copy;
		rtd->ops.silence	= platform->driver->ops->silence;
		rtd->ops.page		= platform->driver->ops->page;
		rtd->ops.mmap		= platform->driver->ops->mmap;
	}

	if (playback)
		snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &rtd->ops);

	if (capture)
		snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &rtd->ops);

	if (platform->driver->pcm_new) {
		ret = platform->driver->pcm_new(rtd);
		if (ret < 0) {
			dev_err(platform->dev,
				"ASoC: pcm constructor failed: %d\n",
				ret);
			return ret;
		}
	}

	pcm->private_free = platform->driver->pcm_free;
out:
	dev_info(rtd->card->dev, "%s <-> %s mapping ok\n",
		 (rtd->num_codecs > 1) ? "multicodec" : rtd->codec_dai->name,
		 cpu_dai->name);
	return ret;
}

int snd_soc_platform_trigger(struct snd_pcm_substream *substream,
		int cmd, struct snd_soc_platform *platform)
{
	if (platform->driver->ops && platform->driver->ops->trigger)
		return platform->driver->ops->trigger(substream, cmd);
	return 0;
}
EXPORT_SYMBOL_GPL(snd_soc_platform_trigger);

