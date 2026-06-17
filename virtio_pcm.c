// SPDX-License-Identifier: GPL-2.0+
/*
 * Sound card driver for virtio
 * Copyright (C) 2020  OpenSynergy GmbH
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 *
 *​​​​ Changes from Qualcomm Innovation Center are provided under the following license:
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
#include <linux/moduleparam.h>
#include <linux/virtio_config.h>
#include <linux/dma-resv.h>

#include "virtio_card.h"

static unsigned int pcm_buffer_ms = 160;
module_param(pcm_buffer_ms, uint, 0644);
MODULE_PARM_DESC(pcm_buffer_ms, "PCM substream buffer time in milliseconds");

static unsigned int pcm_periods_min = 2;
module_param(pcm_periods_min, uint, 0644);
MODULE_PARM_DESC(pcm_periods_min, "Minimum number of PCM periods");

static unsigned int pcm_periods_max = 512;
module_param(pcm_periods_max, uint, 0644);
MODULE_PARM_DESC(pcm_periods_max, "Maximum number of PCM periods");

static unsigned int pcm_period_ms_min = 10;
module_param(pcm_period_ms_min, uint, 0644);
MODULE_PARM_DESC(pcm_period_ms_min, "Minimum PCM period time in milliseconds");

static unsigned int pcm_period_ms_max = 80;
module_param(pcm_period_ms_max, uint, 0644);
MODULE_PARM_DESC(pcm_period_ms_max, "Maximum PCM period time in milliseconds");

static const unsigned int g_v2a_format_map[] = {
	[VIRTIO_SND_PCM_FMT_IMA_ADPCM] = SNDRV_PCM_FORMAT_IMA_ADPCM,
	[VIRTIO_SND_PCM_FMT_MU_LAW] = SNDRV_PCM_FORMAT_MU_LAW,
	[VIRTIO_SND_PCM_FMT_A_LAW] = SNDRV_PCM_FORMAT_A_LAW,
	[VIRTIO_SND_PCM_FMT_S8] = SNDRV_PCM_FORMAT_S8,
	[VIRTIO_SND_PCM_FMT_U8] = SNDRV_PCM_FORMAT_U8,
	[VIRTIO_SND_PCM_FMT_S16] = SNDRV_PCM_FORMAT_S16_LE,
	[VIRTIO_SND_PCM_FMT_U16] = SNDRV_PCM_FORMAT_U16_LE,
	[VIRTIO_SND_PCM_FMT_S18_3] = SNDRV_PCM_FORMAT_S18_3LE,
	[VIRTIO_SND_PCM_FMT_U18_3] = SNDRV_PCM_FORMAT_U18_3LE,
	[VIRTIO_SND_PCM_FMT_S20_3] = SNDRV_PCM_FORMAT_S20_3LE,
	[VIRTIO_SND_PCM_FMT_U20_3] = SNDRV_PCM_FORMAT_U20_3LE,
	[VIRTIO_SND_PCM_FMT_S24_3] = SNDRV_PCM_FORMAT_S24_3LE,
	[VIRTIO_SND_PCM_FMT_U24_3] = SNDRV_PCM_FORMAT_U24_3LE,
#ifdef SNDRV_PCM_FORMAT_S20
	[VIRTIO_SND_PCM_FMT_S20] = SNDRV_PCM_FORMAT_S20_LE,
#endif
#ifdef SNDRV_PCM_FORMAT_U20
	[VIRTIO_SND_PCM_FMT_U20] = SNDRV_PCM_FORMAT_U20_LE,
#endif
	[VIRTIO_SND_PCM_FMT_S24] = SNDRV_PCM_FORMAT_S24_LE,
	[VIRTIO_SND_PCM_FMT_U24] = SNDRV_PCM_FORMAT_U24_LE,
	[VIRTIO_SND_PCM_FMT_S32] = SNDRV_PCM_FORMAT_S32_LE,
	[VIRTIO_SND_PCM_FMT_U32] = SNDRV_PCM_FORMAT_U32_LE,
	[VIRTIO_SND_PCM_FMT_FLOAT] = SNDRV_PCM_FORMAT_FLOAT_LE,
	[VIRTIO_SND_PCM_FMT_FLOAT64] = SNDRV_PCM_FORMAT_FLOAT64_LE,
	[VIRTIO_SND_PCM_FMT_DSD_U8] = SNDRV_PCM_FORMAT_DSD_U8,
	[VIRTIO_SND_PCM_FMT_DSD_U16] = SNDRV_PCM_FORMAT_DSD_U16_LE,
	[VIRTIO_SND_PCM_FMT_DSD_U32] = SNDRV_PCM_FORMAT_DSD_U32_LE,
	[VIRTIO_SND_PCM_FMT_IEC958_SUBFRAME] =
		SNDRV_PCM_FORMAT_IEC958_SUBFRAME_LE
};

struct virtsnd_v2a_rate {
	unsigned int alsa_bit;
	unsigned int rate;
};

static const struct virtsnd_v2a_rate g_v2a_rate_map[] = {
	[VIRTIO_SND_PCM_RATE_5512] = { SNDRV_PCM_RATE_5512, 5512 },
	[VIRTIO_SND_PCM_RATE_8000] = { SNDRV_PCM_RATE_8000, 8000 },
	[VIRTIO_SND_PCM_RATE_11025] = { SNDRV_PCM_RATE_11025, 11025 },
	[VIRTIO_SND_PCM_RATE_12000] = { SNDRV_PCM_RATE_KNOT, 12000 },
	[VIRTIO_SND_PCM_RATE_16000] = { SNDRV_PCM_RATE_16000, 16000 },
	[VIRTIO_SND_PCM_RATE_22050] = { SNDRV_PCM_RATE_22050, 22050 },
	[VIRTIO_SND_PCM_RATE_24000] = { SNDRV_PCM_RATE_KNOT, 24000 },
	[VIRTIO_SND_PCM_RATE_32000] = { SNDRV_PCM_RATE_32000, 32000 },
	[VIRTIO_SND_PCM_RATE_44100] = { SNDRV_PCM_RATE_44100, 44100 },
	[VIRTIO_SND_PCM_RATE_48000] = { SNDRV_PCM_RATE_48000, 48000 },
	[VIRTIO_SND_PCM_RATE_64000] = { SNDRV_PCM_RATE_64000, 64000 },
	[VIRTIO_SND_PCM_RATE_88200] = { SNDRV_PCM_RATE_88200, 88200 },
	[VIRTIO_SND_PCM_RATE_96000] = { SNDRV_PCM_RATE_96000, 96000 },
	[VIRTIO_SND_PCM_RATE_176400] = { SNDRV_PCM_RATE_176400, 176400 },
	[VIRTIO_SND_PCM_RATE_192000] = { SNDRV_PCM_RATE_192000, 192000 },
	[VIRTIO_SND_PCM_RATE_384000] = { SNDRV_PCM_RATE_384000, 384000 }
};

static int virtsnd_pcm_build_hw(struct virtio_pcm_substream *substream,
				struct virtio_snd_pcm_info *info)
{
	struct virtio_device *vdev = substream->snd->vdev;
	unsigned int i;
	u64 values;
	size_t sample_max = 0;
	size_t sample_min = 0;

	substream->features = le32_to_cpu(info->features);

	/*
	 * TODO: set SNDRV_PCM_INFO_{BATCH,BLOCK_TRANSFER} if device supports
	 * only message-based transport.
	 */
	substream->hw.info =
		SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_BATCH |
		SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_RESUME |
		SNDRV_PCM_INFO_PAUSE |
		SNDRV_PCM_INFO_NO_PERIOD_WAKEUP;

	if (!info->channels_min || info->channels_min > info->channels_max) {
		dev_err(&vdev->dev,
			"SID %u: invalid channel range [%u %u]", substream->sid,
			info->channels_min, info->channels_max);
		return -EINVAL;
	}

	substream->hw.channels_min = info->channels_min;
	substream->hw.channels_max = info->channels_max;

	values = le64_to_cpu(info->formats);

	for (i = 0; i < ARRAY_SIZE(g_v2a_format_map); ++i)
		if (values & (1ULL << i)) {
			unsigned int alsa_fmt = g_v2a_format_map[i];
			int bytes = snd_pcm_format_physical_width(alsa_fmt) / 8;

			if (!sample_min || sample_min > bytes)
				sample_min = bytes;

			if (sample_max < bytes)
				sample_max = bytes;

			substream->hw.formats |= (1ULL << alsa_fmt);
		}

	if (!substream->hw.formats) {
		dev_err(&vdev->dev,
			"SID %u: no supported PCM sample formats found",
			substream->sid);
		return -EINVAL;
	}

	values = le64_to_cpu(info->rates);

	for (i = 0; i < ARRAY_SIZE(g_v2a_rate_map); ++i)
		if (values & (1ULL << i)) {
			if (!substream->hw.rate_min ||
			    substream->hw.rate_min > g_v2a_rate_map[i].rate)
				substream->hw.rate_min = g_v2a_rate_map[i].rate;

			if (substream->hw.rate_max < g_v2a_rate_map[i].rate)
				substream->hw.rate_max = g_v2a_rate_map[i].rate;

			substream->hw.rates |= g_v2a_rate_map[i].alsa_bit;
		}

	if (!substream->hw.rates) {
		dev_err(&vdev->dev,
			"SID %u: no supported PCM frame rates found",
			substream->sid);
		return -EINVAL;
	}

	substream->hw.periods_min = pcm_periods_min;
	substream->hw.periods_max = pcm_periods_max;

	/*
	 * We must ensure that there is enough space in the buffer to store
	 * pcm_buffer_ms ms for the combination (Cmax, Smax, Rmax), where:
	 *   Cmax = maximum supported number of channels,
	 *   Smax = maximum supported sample size in bytes,
	 *   Rmax = maximum supported frame rate.
	 */
	substream->hw.buffer_bytes_max =
		sample_max * substream->hw.channels_max * pcm_buffer_ms *
		(substream->hw.rate_max / MSEC_PER_SEC);

	/* Align the buffer size to the page size */
	substream->hw.buffer_bytes_max =
		(substream->hw.buffer_bytes_max + PAGE_SIZE - 1) & -PAGE_SIZE;

	/*
	 * We must ensure that the minimum period size is enough to store
	 * pcm_period_ms_min ms for the combination (Cmin, Smin, Rmin), where:
	 *   Cmin = minimum supported number of channels,
	 *   Smin = minimum supported sample size in bytes,
	 *   Rmin = minimum supported frame rate.
	 */
	substream->hw.period_bytes_min =
		sample_min * substream->hw.channels_min * pcm_period_ms_min *
		(substream->hw.rate_min / MSEC_PER_SEC);

	/*
	 * We must ensure that the maximum period size is enough to store
	 * pcm_period_ms_max ms for the combination (Cmax, Smax, Rmax).
	 */
	substream->hw.period_bytes_max =
		sample_max * substream->hw.channels_max * pcm_period_ms_max *
		(substream->hw.rate_max / MSEC_PER_SEC);

	return 0;
}

// static void virtsnd_pcm_prealloc_pages(struct virtio_pcm_substream *substream)
// {
// 	struct snd_pcm_substream *ksubstream = substream->substream;
// 	size_t size = substream->hw.buffer_bytes_max;
// 	struct device *data = NULL;

// 	snd_pcm_lib_preallocate_pages(ksubstream,
// 								  SNDRV_DMA_TYPE_CONTINUOUS, data,
// 								  size, size);
// }

int virtsnd_alloc_dmabuf(struct virtio_pcm_substream *substream, size_t size, enum dma_buf_index index)
{
	int rc = -EINVAL;
	struct dma_heap *heap = NULL;
	struct virtio_device *vdev = substream->snd->vdev;
	size_t alloc_size = PAGE_ALIGN(size);

	if (substream->dma_data[index].vmap != NULL) {
		dev_err(&vdev->dev, "%s: virtsnd_alloc_buffer: buffer index %d already allocated", __func__, index);
		return 0;
	}
#ifdef __DMA_BUF_MAP_H__ // kernel 5.15
	substream->dma_data[index].vmap = kzalloc(sizeof(struct dma_buf_map), GFP_KERNEL);
#else					 // kernel 6.1
	substream->dma_data[index].vmap = kzalloc(sizeof(struct iosys_map), GFP_KERNEL);
#endif
	if (!substream->dma_data[index].vmap) {
		rc = -ENOMEM;
		goto err;
	}

	heap = dma_heap_find("system");
	if (!heap)
		goto err;

	substream->dma_data[index].dma_buf = dma_heap_buffer_alloc(heap, alloc_size, 0, 0);
	if (IS_ERR_OR_NULL((void *)substream->dma_data[index].dma_buf)) {
		rc = -ENOMEM;
		goto err;
	}

	if (!substream->dma_data[index].dma_buf->resv) {
		rc = -EINVAL;
		goto put_dma_buf;
	}

	dma_resv_lock(substream->dma_data[index].dma_buf->resv, NULL);
	rc = dma_buf_vmap(substream->dma_data[index].dma_buf, substream->dma_data[index].vmap);
	dma_resv_unlock(substream->dma_data[index].dma_buf->resv);
	if (rc)
		goto put_dma_buf;

	return 0;

put_dma_buf:
	dma_buf_put(substream->dma_data[index].dma_buf);
	substream->dma_data[index].dma_buf = NULL;

 err:
	if (substream->dma_data[index].vmap) {
		kfree(substream->dma_data[index].vmap);
		substream->dma_data[index].vmap = NULL;
	}

	return rc;
}

struct virtio_pcm *virtsnd_pcm_find(struct virtio_snd *snd, unsigned int nid)
{
	struct virtio_pcm *pcm;

	list_for_each_entry(pcm, &snd->pcm_list, list)
		if (pcm->nid == nid)
			return pcm;

	return ERR_PTR(-ENOENT);
}

struct virtio_pcm *virtsnd_pcm_find_or_create(struct virtio_snd *snd,
					      unsigned int nid)
{
	struct virtio_device *vdev = snd->vdev;
	struct virtio_pcm *pcm;

	pcm = virtsnd_pcm_find(snd, nid);
	if (!IS_ERR(pcm))
		return pcm;

	pcm = devm_kzalloc(&vdev->dev, sizeof(*pcm), GFP_KERNEL);
	if (!pcm)
		return ERR_PTR(-ENOMEM);

	pcm->nid = nid;
	list_add_tail(&pcm->list, &snd->pcm_list);

	return pcm;
}

int virtsnd_pcm_validate(struct virtio_device *vdev)
{
	if (pcm_periods_min < 2 || pcm_periods_min > pcm_periods_max) {
		dev_err(&vdev->dev,
			"invalid range [%u %u] of the number of PCM periods",
			pcm_periods_min, pcm_periods_max);
		return -EINVAL;
	}

	if (!pcm_period_ms_min || pcm_period_ms_min > pcm_period_ms_max) {
		dev_err(&vdev->dev,
			"invalid range [%u %u] of the size of the PCM period",
			pcm_period_ms_min, pcm_period_ms_max);
		return -EINVAL;
	}

	if (pcm_buffer_ms < pcm_periods_min * pcm_period_ms_min) {
		dev_err(&vdev->dev,
			"pcm_buffer_ms(=%u) value cannot be < %u ms",
			pcm_buffer_ms, pcm_periods_min * pcm_period_ms_min);
		return -EINVAL;
	}

	if (pcm_period_ms_max > pcm_buffer_ms / 2) {
		dev_err(&vdev->dev,
			"pcm_period_ms_max(=%u) value cannot be > %u ms",
			pcm_period_ms_max, pcm_buffer_ms / 2);
		return -EINVAL;
	}

	return 0;
}

/**
 * virtsnd_pcm_xrun_work() - Work handler to stop a capture stream on xrun.
 * @work: xrun work item embedded in virtio_pcm_substream.
 *
 * Called from a kernel workqueue (process context) so it is safe to call
 * snd_pcm_stop_xrun() here, unlike the spinlock-held interrupt context in
 * virtsnd_pcm_msg_complete() where the xrun was first detected.
 */
static void virtsnd_pcm_xrun_work(struct work_struct *work)
{
	struct virtio_pcm_substream *substream =
		container_of(work, struct virtio_pcm_substream, xrun_work);

	if (atomic_read(&substream->xfer_enabled))
		snd_pcm_stop_xrun(substream->substream);
}

int virtsnd_pcm_parse_cfg(struct virtio_snd *snd)
{
	struct virtio_device *vdev = snd->vdev;
	struct virtio_snd_pcm_info *info;
	unsigned int i;
	int rc;
        struct virtio_snd_msg *msg;
        struct virtio_snd_hdr *hdr;
        struct scatterlist sg_response_ext;
        int code;

        __le32 *count = devm_kzalloc(&vdev->dev, sizeof(*count), GFP_KERNEL);

        msg = virtsnd_ctl_msg_alloc(vdev, sizeof(*hdr),
                                    sizeof(struct virtio_snd_hdr), GFP_KERNEL);

        if (IS_ERR(msg)) {
                devm_kfree(&vdev->dev, count);
                return PTR_ERR(msg);
        }

        hdr = sg_virt(&msg->sg_request);
        hdr->code = cpu_to_virtio32(vdev, VIRTIO_SND_R_PCM_COUNT);

        sg_init_one(&sg_response_ext, count, sizeof(*count));
        msg->sg_response_ext = &sg_response_ext;
        msg->reply = count;
        msg->reply_size = sizeof(*count);

        code = virtsnd_ctl_msg_send_sync(snd, msg);
        if (code) {
                dev_err(&vdev->dev, "failed to query pcm count: %d\n",
                         code);
                devm_kfree(&vdev->dev, count);
                return code;
        }

        snd->nsubstreams = *count;

	if (!snd->nsubstreams)
		return 0;

	snd->substreams = devm_kcalloc(&vdev->dev, snd->nsubstreams,
				       sizeof(*snd->substreams), GFP_KERNEL);
	if (!snd->substreams)
		return -ENOMEM;

	info = devm_kcalloc(&vdev->dev, snd->nsubstreams, sizeof(*info),
			    GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	rc = virtsnd_ctl_query_info(snd, VIRTIO_SND_R_PCM_INFO, 0,
				    snd->nsubstreams, sizeof(*info), info);
	if (rc)
		return rc;

	for (i = 0; i < snd->nsubstreams; ++i) {
		struct virtio_pcm_substream *substream = &snd->substreams[i];
		struct virtio_pcm *pcm;

		substream->snd = snd;
		substream->sid = i;
		init_waitqueue_head(&substream->msg_empty);
		INIT_WORK(&substream->xrun_work, virtsnd_pcm_xrun_work);

		rc = virtsnd_pcm_build_hw(substream, &info[i]);
		if (rc)
			return rc;

		substream->nid = le32_to_cpu(info[i].hdr.hda_fn_nid);

		pcm = virtsnd_pcm_find_or_create(snd, substream->nid);
		if (IS_ERR(pcm))
			return PTR_ERR(pcm);

		switch (info[i].direction) {
		case VIRTIO_SND_D_OUTPUT: {
			substream->direction = SNDRV_PCM_STREAM_PLAYBACK;
			break;
		}
		case VIRTIO_SND_D_INPUT: {
			substream->direction = SNDRV_PCM_STREAM_CAPTURE;
			break;
		}
		default: {
			dev_err(&vdev->dev, "SID %u: unknown direction (%u)",
				substream->sid, info[i].direction);
			return -EINVAL;
		}
		}

		pcm->streams[substream->direction].nsubstreams++;
	}

	devm_kfree(&vdev->dev, info);

	return 0;
}

static int virtsnd_pcm_check_entity_cfg(struct virtio_pcm_substream *substream,
					struct virtio_snd_pcm_info *info)
{
	unsigned int i;
	u64 values;
	u64 formats = 0;
	u64 rates = 0;
	bool changed = false;
	int rc;

	if (substream->nid != le32_to_cpu(info->hdr.hda_fn_nid))
		return -EINVAL;
	if (substream->direction != info->direction)
		return -EINVAL;

	if (substream->features != le32_to_cpu(info->features))
		changed = true;
	if (substream->hw.channels_min != info->channels_min)
		changed = true;
	if (substream->hw.channels_max != info->channels_max)
		changed = true;

	values = le64_to_cpu(info->formats);

	for (i = 0; i < ARRAY_SIZE(g_v2a_format_map); ++i)
		if (values & (1ULL << i))
			formats |= (1ULL << g_v2a_format_map[i]);

	if (substream->hw.formats != formats)
		changed = true;

	values = le64_to_cpu(info->rates);

	for (i = 0; i < ARRAY_SIZE(g_v2a_rate_map); ++i)
		if (values & (1ULL << i))
			rates |= g_v2a_rate_map[i].alsa_bit;

	if (substream->hw.rates != rates)
		changed = true;

	if (changed) {
		struct snd_pcm_runtime *runtime = substream->substream->runtime;
		size_t buffer_bytes_max = substream->hw.buffer_bytes_max;

		if (runtime && runtime->status &&
		    runtime->status->state != SNDRV_PCM_STATE_OPEN)
			return -EINVAL;

		rc = virtsnd_pcm_build_hw(substream, info);
		if (rc)
			return rc;

		if (buffer_bytes_max < substream->hw.buffer_bytes_max)
			return -EINVAL;
	}

	return 0;
}

int virtsnd_pcm_check_cfg(struct virtio_snd *snd)
{
	struct virtio_device *vdev = snd->vdev;
	struct virtio_snd_pcm_info *info;
	unsigned int i = 0;
	int rc;

	virtio_cread(vdev, struct virtio_snd_config, streams, &i);
	if (snd->nsubstreams != i) {
		dev_warn(&vdev->dev,
			 "config: number of streams has changed (%u->%u)",
			 snd->nsubstreams, i);
		return -EINVAL;
	}

	if (!snd->nsubstreams)
		return 0;

	info = devm_kcalloc(&vdev->dev, snd->nsubstreams, sizeof(*info),
			    GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	rc = virtsnd_ctl_query_info(snd, VIRTIO_SND_R_PCM_INFO, 0,
				      snd->nsubstreams, sizeof(*info), info);
	if (rc)
		goto on_failure;

	for (i = 0; i < snd->nsubstreams; ++i) {
		rc = virtsnd_pcm_check_entity_cfg(&snd->substreams[i],
						  &info[i]);
		if (rc) {
			dev_warn(&vdev->dev,
				 "config: stream#%u configuration has changed",
				 i);
			break;
		}
	}

on_failure:
	devm_kfree(&vdev->dev, info);

	return rc;
}

static int virtsnd_pcm_info(struct virtio_snd *snd, struct snd_pcm *pcm)
{
	if (VIRTIO_HAS_OPSY_EXTENSION(snd, DEV_EXT_INFO)) {
		int code;

		code = virtsnd_ctl_alsa_pcm_info(snd, pcm);
		if (!code || code != -EOPNOTSUPP)
			return code;
	}

	strscpy(pcm->name, "VirtIO PCM", sizeof(pcm->name));
	return 0;
}

int virtsnd_pcm_build_devs(struct virtio_snd *snd)
{
	struct virtio_device *vdev = snd->vdev;
	struct virtio_pcm *pcm;
	unsigned int i;
	int code;

	list_for_each_entry(pcm, &snd->pcm_list, list) {
		unsigned int npbs =
			pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].nsubstreams;
		unsigned int ncps =
			pcm->streams[SNDRV_PCM_STREAM_CAPTURE].nsubstreams;

		if (!npbs && !ncps)
			continue;

		code = snd_pcm_new(snd->card, "virtio_snd", pcm->nid, npbs,
				   ncps, &pcm->pcm);
		if (code) {
			dev_err(&vdev->dev, "snd_pcm_new[%u] failed: %d",
				pcm->nid, code);
			return code;
		}

		code = virtsnd_pcm_info(snd, pcm->pcm);
		if (code)
			return code;

		pcm->pcm->info_flags = 0;
		pcm->pcm->dev_class = SNDRV_PCM_CLASS_GENERIC;
		pcm->pcm->dev_subclass = SNDRV_PCM_SUBCLASS_GENERIC_MIX;

		pcm->pcm->private_data = pcm;

		for (i = 0; i < ARRAY_SIZE(pcm->streams); ++i) {
			struct virtio_pcm_stream *stream = &pcm->streams[i];

			if (!stream->nsubstreams)
				continue;

			stream->substreams =
				devm_kcalloc(&vdev->dev,
					     stream->nsubstreams,
					     sizeof(*stream->substreams),
					     GFP_KERNEL);
			if (!stream->substreams)
				return -ENOMEM;

			stream->nsubstreams = 0;
		}
	}

	for (i = 0; i < snd->nsubstreams; ++i) {
		struct virtio_pcm_substream *substream = &snd->substreams[i];
		struct virtio_pcm_stream *stream;

		pcm = virtsnd_pcm_find(snd, substream->nid);
		if (IS_ERR(pcm))
			return PTR_ERR(pcm);

		stream = &pcm->streams[substream->direction];
		stream->substreams[stream->nsubstreams++] = substream;
	}

	list_for_each_entry(pcm, &snd->pcm_list, list)
		for (i = 0; i < ARRAY_SIZE(pcm->streams); ++i) {
			struct virtio_pcm_stream *stream = &pcm->streams[i];
			struct snd_pcm_str *kstream;
			struct snd_pcm_substream *ksubstream;

			if (!stream->nsubstreams)
				continue;

			kstream = &pcm->pcm->streams[i];
			ksubstream = kstream->substream;

			while (ksubstream) {
				struct virtio_pcm_substream *substream =
					stream->substreams[ksubstream->number];

				substream->substream = ksubstream;
				ksubstream = ksubstream->next;

				code = virtsnd_alloc_dmabuf(substream, substream->hw.buffer_bytes_max, DMA_BUF_DATA);
				if (code) {
					dev_err(&vdev->dev, "Failed to allocate dma_buf for substream %d, failing", i);
				}
			}

			snd_pcm_set_ops(pcm->pcm, i, &virtsnd_pcm_ops[i]);
		}

	return 0;
}

#ifdef CONFIG_PM_SLEEP
int virtsnd_pcm_restore(struct virtio_snd *snd)
{
	unsigned int i;

	for (i = 0; i < snd->nsubstreams; ++i) {
		struct snd_pcm_substream *substream =
			snd->substreams[i].substream;
		struct snd_pcm_runtime *runtime = substream->runtime;
		int rc;

		if (!runtime || !runtime->status ||
		    runtime->status->state != SNDRV_PCM_STATE_SUSPENDED)
			continue;

		rc = substream->ops->hw_params(substream, NULL);
		if (rc)
			return rc;

		rc = substream->ops->prepare(substream);
		if (rc)
			return rc;
	}

	return 0;
}
#endif /* CONFIG_PM_SLEEP */

void virtsnd_pcm_event(struct virtio_snd *snd, struct virtio_snd_event *event)
{
	struct virtio_pcm_substream *substream;
	unsigned int sid = le32_to_cpu(event->data);

	if (sid >= snd->nsubstreams)
		return;

	substream = &snd->substreams[sid];

	switch (le32_to_cpu(event->hdr.code)) {
	case VIRTIO_SND_EVT_PCM_PERIOD_ELAPSED: {
		/* TODO: deal with shmem elapsed period */
		break;
	}
	case VIRTIO_SND_EVT_PCM_XRUN: {
		if (atomic_read(&substream->xfer_enabled))
			atomic_set(&substream->xfer_xrun, 1);
		break;
	}
	}
}
