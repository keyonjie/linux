/*
 * Intel tdf8532 codec machine driver
 * Copyright (c) 2016, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */


#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/slab.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/jack.h>

static int sof_tdf8532_codec_fixup(struct snd_soc_pcm_runtime *rtd,
					struct snd_pcm_hw_params *params)
{
	// TODO: read this from topology
	return 0;
}

static struct snd_soc_ops sof_tdf8532_ops = {};

static int tdf8532_rtd_init(struct snd_soc_pcm_runtime *rtd)
{
	snd_soc_set_dmi_name(rtd->card, NULL);

	return 0;
}

/* we just set some BEs - FE provided by topology */
static struct snd_soc_dai_link sof_tdf8532_dais[] = {
	{
		/* SSP4 - Amplifier */
		.name = "SSP4-Codec",
		.id = 0,
		.cpu_dai_name = "sof-audio",
		.platform_name = "sof-audio",
		.codec_name = "i2c-INT34C3:00",
		.codec_dai_name = "tdf8532-hifi",
		.ops = &sof_tdf8532_ops,
		.dai_fmt = SND_SOC_DAIFMT_DSP_B | SND_SOC_DAIFMT_NB_NF |
			SND_SOC_DAIFMT_CBS_CFS,
		.ignore_suspend = 1,
		.dpcm_capture = 1,
		.dpcm_playback = 1,
		.no_pcm = 1,
	},
#if 0
	/* Back End DAI links */
	{
	       /* SSP0 - Codec */
	       .name = "NoCodec",
	       .stream_name = "I2S",
	       .id = 0,
	       .init = tdf8532_rtd_init,
	       .cpu_dai_name = "sof-audio",
	       .platform_name = "sof-audio",
	       .no_pcm = 1,
	       .codec_dai_name = "snd-soc-dummy-dai",
	       .codec_name = "snd-soc-dummy",
	       .ops = &sof_tdf8532_ops,
	       .dai_fmt = SND_SOC_DAIFMT_DSP_B | SND_SOC_DAIFMT_NB_NF |
			SND_SOC_DAIFMT_CBS_CFS,
	       .ignore_suspend = 1,
	       .be_hw_params_fixup = sof_tdf8532_codec_fixup,
	       .dpcm_playback = 1,
	       .dpcm_capture = 1,
	},
	{
	       /* SSP0 - BT */
	       .name = "SSP0-Codec",
	       .id = 0,
	       .cpu_dai_name = "SSP0 Pin",
	       .codec_name = "snd-soc-dummy",
	       .codec_dai_name = "snd-soc-dummy-dai",
	       .cpu_dai_name = "sof-audio",
	       .platform_name = "sof-audio",
	       .ignore_suspend = 1,
	       .dpcm_capture = 1,
	       .dpcm_playback = 1,
	       .no_pcm = 1,
	},

	{
	       /* SSP1 - HDMI-In */
	       .name = "SSP1-Codec",
	       .id = 1,
	       .cpu_dai_name = "SSP1 Pin",
	       .codec_name = "snd-soc-dummy",
	       .codec_dai_name = "snd-soc-dummy-dai",
	       .platform_name = "0000:00:0e.0",
	       .ignore_suspend = 1,
	       .dpcm_capture = 1,
	       .no_pcm = 1,
	},
	{
	       /* SSP2 - Dirana */
	       .name = "SSP2-Codec",
	       .id = 2,
	       .cpu_dai_name = "SSP2 Pin",
	       .codec_name = "snd-soc-dummy",
	       .codec_dai_name = "snd-soc-dummy-dai",
	       .platform_name = "0000:00:0e.0",
	       .ignore_suspend = 1,
	       .dpcm_capture = 1,
	       .dpcm_playback = 1,
	       .no_pcm = 1,
	},
	{
	       /* SSP3 - Modem */
	       .name = "SSP3-Codec",
	       .id = 3,
	       .cpu_dai_name = "SSP3 Pin",
	       .codec_name = "snd-soc-dummy",
	       .codec_dai_name = "snd-soc-dummy-dai",
	       .platform_name = "0000:00:0e.0",
	       .ignore_suspend = 1,
	       .dpcm_capture = 1,
	       .dpcm_playback = 1,
	       .no_pcm = 1,
	},
	{
	       /* SSP4 - Amplifier */
	       .name = "SSP4-Codec",
	       .id = 4,
	       .cpu_dai_name = "SSP4 Pin",
	       .codec_name = "i2c-INT34C3:00",
	       .codec_dai_name = "tdf8532-hifi",
	       .platform_name = "0000:00:0e.0",
	       .ignore_suspend = 1,
	       .dpcm_playback = 1,
	       .no_pcm = 1,
	},
	{
	       /* SSP5 - TestPin */
	       .name = "SSP5-Codec",
	       .id = 5,
	       .cpu_dai_name = "SSP5 Pin",
	       .codec_name = "snd-soc-dummy",
	       .codec_dai_name = "snd-soc-dummy-dai",
	       .platform_name = "0000:00:0e.0",
	       .ignore_suspend = 1,
	       .dpcm_capture = 1,
	       .dpcm_playback = 1,
	       .no_pcm = 1,
	},
#endif

};

static struct snd_soc_card sof_tdf8532_card = {
	.name = "sof-tdf8532",
	.dai_link = sof_tdf8532_dais,
	.num_links = ARRAY_SIZE(sof_tdf8532_dais),
};

static int sof_tdf8532_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &sof_tdf8532_card;

	card->dev = &pdev->dev;

	return devm_snd_soc_register_card(&pdev->dev, card);
}

static int sof_tdf8532_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver sof_tdf8532_audio = {
	.probe = sof_tdf8532_probe,
	.remove = sof_tdf8532_remove,
	.driver = {
	       .name = "sof-tdf8532",
	       .pm = &snd_soc_pm_ops,
	},
};
module_platform_driver(sof_tdf8532_audio)

MODULE_DESCRIPTION("ASoC sof tdf8532");
MODULE_AUTHOR("Xiuli Pan");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:sof-tdf8532");
