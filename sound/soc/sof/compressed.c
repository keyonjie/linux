// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2017 Intel Corporation. All rights reserved.
 *
 * Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
 */

#include <linux/slab.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/sof.h>
#include <sound/compress_driver.h>
#include <uapi/sound/sof-ipc.h>
#include "sof-priv.h"

#if 0
/* compress stream operations */
static void sof_period_elapsed(void *arg)
{
	struct snd_compr_stream *cstream = (struct snd_compr_stream *)arg;

	snd_compr_fragment_elapsed(cstream);
}

static void sof_drain_notify(void *arg)
{
	struct snd_compr_stream *cstream = (struct snd_compr_stream *)arg;

	snd_compr_drain_notify(cstream);
}
#endif

static int sof_compressed_open(struct snd_compr_stream *cstream)
{
	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_sof_dev *sdev =
		snd_soc_platform_get_drvdata(rtd->platform);
	struct snd_sof_pcm *spcm = rtd->sof;

	mutex_lock(&spcm->mutex);
	pm_runtime_get_sync(sdev->dev);
	mutex_unlock(&spcm->mutex);
	return 0;
}

static int sof_compressed_free(struct snd_compr_stream *cstream)
{
	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_sof_dev *sdev =
		snd_soc_platform_get_drvdata(rtd->platform);
	struct snd_sof_pcm *spcm = rtd->sof;

	mutex_lock(&spcm->mutex);
	pm_runtime_put(sdev->dev);
	mutex_unlock(&spcm->mutex);
	return 0;
}


static int sof_vorbis_set_params(struct snd_compr_stream *cstream,
					struct snd_compr_params *params)
{
#if 0
//	struct snd_compr_runtime *runtime = cstream->runtime;
	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_sof_dev *sdev =
		snd_soc_platform_get_drvdata(rtd->platform);
	struct snd_sof_pcm *spcm = rtd->sof;
//	struct snd_soc_tplg_stream_caps *caps = 
//		&spcm->pcm.caps[cstream->direction];
#endif
	return 0;
}

static int sof_mp3_set_params(struct snd_compr_stream *cstream,
					struct snd_compr_params *params)
{
#if 0
//	struct snd_compr_runtime *runtime = cstream->runtime;
	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_sof_dev *sdev =
		snd_soc_platform_get_drvdata(rtd->platform);
	struct snd_sof_pcm *spcm = rtd->sof;
//	struct snd_soc_tplg_stream_caps *caps = 
//		&spcm->pcm.caps[cstream->direction];
#endif
	return 0;
}

static int sof_compressed_set_params(struct snd_compr_stream *cstream,
					struct snd_compr_params *params)
{
	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_sof_dev *sdev =
		snd_soc_platform_get_drvdata(rtd->platform);

	switch (params->codec.id) {
	case SND_AUDIOCODEC_VORBIS:
		return sof_vorbis_set_params(cstream, params);
	case SND_AUDIOCODEC_MP3:
		return sof_mp3_set_params(cstream, params);
	default:
		dev_err(sdev->dev, "error: codec id %d not supported\n",
			params->codec.id);
		return -EINVAL;
	}
}

static int sof_compressed_trigger(struct snd_compr_stream *cstream, int cmd)
{
	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_sof_dev *sdev =
		snd_soc_platform_get_drvdata(rtd->platform);
	struct snd_sof_pcm *spcm = rtd->sof;
	struct sof_ipc_stream stream;
	struct sof_ipc_reply reply;

	stream.hdr.size = sizeof(stream);
	stream.hdr.cmd = SOF_IPC_GLB_STREAM_MSG;
	stream.comp_id = spcm->stream[cstream->direction].comp_id;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		stream.hdr.cmd |= SOF_IPC_STREAM_TRIG_START;
		break;
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		stream.hdr.cmd |= SOF_IPC_STREAM_TRIG_RELEASE;
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		stream.hdr.cmd |= SOF_IPC_STREAM_TRIG_STOP;
		break;		
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		stream.hdr.cmd |= SOF_IPC_STREAM_TRIG_PAUSE;
		break;
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_RESUME:
	default:
		break;
	}

	/* send IPC to the DSP */
	return sof_ipc_tx_message(sdev->ipc, stream.hdr.cmd, &stream,
		sizeof(stream), & reply, sizeof(reply));
}

static int sof_compressed_pointer(struct snd_compr_stream *cstream,
					struct snd_compr_tstamp *tstamp)
{
	struct snd_soc_pcm_runtime *rtd = cstream->private_data;
	struct snd_sof_dev *sdev =
		snd_soc_platform_get_drvdata(rtd->platform);
	struct sof_ipc_stream_posn posn;
	struct snd_sof_pcm *spcm = rtd->sof;

	snd_sof_ipc_stream_posn(sdev, spcm, cstream->direction, &posn);

	dev_vdbg(sdev->dev, "CPCM: DMA position %llu DAI position %llu\n",
		posn.host_posn, posn.dai_posn);

	return 0;
}

static int sof_compressed_ack(struct snd_compr_stream *cstream,
					size_t bytes)
{
	return 0;
}

static int sof_compressed_get_caps(struct snd_compr_stream *cstream,
					struct snd_compr_caps *caps)
{
	return 0;
}

static int sof_compressed_get_codec_caps(struct snd_compr_stream *cstream,
					struct snd_compr_codec_caps *codec)
{
	return 0;
}

static int sof_compressed_set_metadata(struct snd_compr_stream *cstream,
					struct snd_compr_metadata *metadata)
{
	return 0;
}

struct snd_compr_ops sof_compressed_ops = {

	.open = sof_compressed_open,
	.free = sof_compressed_free,
	.set_params = sof_compressed_set_params,
	.set_metadata = sof_compressed_set_metadata,
	.trigger = sof_compressed_trigger,
	.pointer = sof_compressed_pointer,
	.ack = sof_compressed_ack,
	.get_caps = sof_compressed_get_caps,
	.get_codec_caps = sof_compressed_get_codec_caps,
};
