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

#include <sound/core.h>
#include <sound/pcm.h>
#include <linux/uio.h>
#include <linux/slab.h>

static int snd_pcm_lib_readv_transfer(struct snd_pcm_substream *substream,
				      unsigned int hwoff,
				      unsigned long data, unsigned int off,
				      snd_pcm_uframes_t frames)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	int err;
	void __user **bufs = (void __user **)data;
	int channels = runtime->channels;
	int c;
	if (substream->ops->copy) {
		for (c = 0; c < channels; ++c, ++bufs) {
			char __user *buf;
			if (*bufs == NULL)
				continue;
			buf = *bufs + samples_to_bytes(runtime, off);
			if ((err = substream->ops->copy(substream, c, hwoff, buf, frames)) < 0)
				return err;
		}
	} else {
		snd_pcm_uframes_t dma_csize = runtime->dma_bytes / channels;
		for (c = 0; c < channels; ++c, ++bufs) {
			char *hwbuf;
			char __user *buf;
			if (*bufs == NULL)
				continue;

			hwbuf = runtime->dma_area + (c * dma_csize) + samples_to_bytes(runtime, hwoff);
			buf = *bufs + samples_to_bytes(runtime, off);
			if (copy_to_user(buf, hwbuf, samples_to_bytes(runtime, frames)))
				return -EFAULT;
		}
	}
	return 0;
}

snd_pcm_sframes_t snd_pcm_lib_readv(struct snd_pcm_substream *substream,
				    void __user **bufs,
				    snd_pcm_uframes_t frames)
{
	struct snd_pcm_runtime *runtime;
	int nonblock;
	int err;

	err = pcm_sanity_check(substream);
	if (err < 0)
		return err;
	runtime = substream->runtime;
	if (runtime->status->state == SNDRV_PCM_STATE_OPEN)
		return -EBADFD;

	nonblock = !!(substream->f_flags & O_NONBLOCK);
	if (runtime->access != SNDRV_PCM_ACCESS_RW_NONINTERLEAVED)
		return -EINVAL;
	return snd_pcm_lib_read1(substream, (unsigned long)bufs, frames, nonblock, snd_pcm_lib_readv_transfer);
}

EXPORT_SYMBOL(snd_pcm_lib_readv);

static int snd_pcm_lib_writev_transfer(struct snd_pcm_substream *substream,
                                       unsigned int hwoff,
                                       unsigned long data, unsigned int off,
                                       snd_pcm_uframes_t frames)
{
        struct snd_pcm_runtime *runtime = substream->runtime;
        int err;
        void __user **bufs = (void __user **)data;
        int channels = runtime->channels;
        int c;
        if (substream->ops->copy) {
                if (snd_BUG_ON(!substream->ops->silence))
                        return -EINVAL;
                for (c = 0; c < channels; ++c, ++bufs) {
                        if (*bufs == NULL) {
                                if ((err = substream->ops->silence(substream, c, hwoff, frames)) < 0)
                                        return err;
                        } else {
                                char __user *buf = *bufs + samples_to_bytes(runtime, off);
                                if ((err = substream->ops->copy(substream, c, hwoff, buf, frames)) < 0)
                                        return err;
                        }
                }
        } else {
                /* default transfer behaviour */
                size_t dma_csize = runtime->dma_bytes / channels;
                for (c = 0; c < channels; ++c, ++bufs) {
                        char *hwbuf = runtime->dma_area + (c * dma_csize) + samples_to_bytes(runtime, hwoff);
                        if (*bufs == NULL) {
                                snd_pcm_format_set_silence(runtime->format, hwbuf, frames);
                        } else {
                                char __user *buf = *bufs + samples_to_bytes(runtime, off);
                                if (copy_from_user(hwbuf, buf, samples_to_bytes(runtime, frames)))
                                        return -EFAULT;
                        }
                }
        }
        return 0;
}

snd_pcm_sframes_t snd_pcm_lib_writev(struct snd_pcm_substream *substream,
                                     void __user **bufs,
                                     snd_pcm_uframes_t frames)
{
        struct snd_pcm_runtime *runtime;
        int nonblock;
        int err;

        err = pcm_sanity_check(substream);
        if (err < 0)
                return err;
        runtime = substream->runtime;
        nonblock = !!(substream->f_flags & O_NONBLOCK);

        if (runtime->access != SNDRV_PCM_ACCESS_RW_NONINTERLEAVED)
                return -EINVAL;
        return snd_pcm_lib_write1(substream, (unsigned long)bufs, frames,
                                  nonblock, snd_pcm_lib_writev_transfer);
}

EXPORT_SYMBOL(snd_pcm_lib_writev);

ssize_t snd_pcm_readv(struct kiocb *iocb, struct iov_iter *to)
{
	struct snd_pcm_file *pcm_file;
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	snd_pcm_sframes_t result;
	unsigned long i;
	void __user **bufs;
	snd_pcm_uframes_t frames;

	pcm_file = iocb->ki_filp->private_data;
	substream = pcm_file->substream;
	if (PCM_RUNTIME_CHECK(substream))
		return -ENXIO;
	runtime = substream->runtime;
	if (runtime->status->state == SNDRV_PCM_STATE_OPEN)
		return -EBADFD;
	if (!iter_is_iovec(to))
		return -EINVAL;
	if (to->nr_segs > 1024 || to->nr_segs != runtime->channels)
		return -EINVAL;
	if (!frame_aligned(runtime, to->iov->iov_len))
		return -EINVAL;
	frames = bytes_to_samples(runtime, to->iov->iov_len);
	bufs = kmalloc(sizeof(void *) * to->nr_segs, GFP_KERNEL);
	if (bufs == NULL)
		return -ENOMEM;
	for (i = 0; i < to->nr_segs; ++i)
		bufs[i] = to->iov[i].iov_base;
	result = snd_pcm_lib_readv(substream, bufs, frames);
	if (result > 0)
		result = frames_to_bytes(runtime, result);
	kfree(bufs);
	return result;
}

EXPORT_SYMBOL(snd_pcm_readv);

ssize_t snd_pcm_writev(struct kiocb *iocb, struct iov_iter *from)
{
	struct snd_pcm_file *pcm_file;
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	snd_pcm_sframes_t result;
	unsigned long i;
	void __user **bufs;
	snd_pcm_uframes_t frames;

	pcm_file = iocb->ki_filp->private_data;
	substream = pcm_file->substream;
	if (PCM_RUNTIME_CHECK(substream))
		return -ENXIO;
	runtime = substream->runtime;
	if (runtime->status->state == SNDRV_PCM_STATE_OPEN)
		return -EBADFD;
	if (!iter_is_iovec(from))
		return -EINVAL;
	if (from->nr_segs > 128 || from->nr_segs != runtime->channels ||
	    !frame_aligned(runtime, from->iov->iov_len))
		return -EINVAL;
	frames = bytes_to_samples(runtime, from->iov->iov_len);
	bufs = kmalloc(sizeof(void *) * from->nr_segs, GFP_KERNEL);
	if (bufs == NULL)
		return -ENOMEM;
	for (i = 0; i < from->nr_segs; ++i)
		bufs[i] = from->iov[i].iov_base;
	result = snd_pcm_lib_writev(substream, bufs, frames);
	if (result > 0)
		result = frames_to_bytes(runtime, result);
	kfree(bufs);
	return result;
}

EXPORT_SYMBOL(snd_pcm_writev);
