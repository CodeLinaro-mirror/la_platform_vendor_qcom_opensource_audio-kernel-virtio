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
 */
#include <sound/pcm_params.h>

#include "virtio_card.h"
#define MAX_VARIANT_NAME 16
#define WAIT_AVAIL_TIME_MS 500
#define MSG_DRAIN_TIMEOUT_MS 1000

static char *audio_variant = "audioreach";
module_param(audio_variant, charp, 0644);
MODULE_PARM_DESC(audio_variant, "The audio framework being used from either audioreach or awe");

struct virtsnd_a2v_format {
	unsigned int alsa_bit;
	unsigned int vio_bit;
};

static const struct virtsnd_a2v_format g_a2v_format_map[] = {
	{ SNDRV_PCM_FORMAT_IMA_ADPCM, VIRTIO_SND_PCM_FMT_IMA_ADPCM },
	{ SNDRV_PCM_FORMAT_MU_LAW, VIRTIO_SND_PCM_FMT_MU_LAW },
	{ SNDRV_PCM_FORMAT_A_LAW, VIRTIO_SND_PCM_FMT_A_LAW },
	{ SNDRV_PCM_FORMAT_S8, VIRTIO_SND_PCM_FMT_S8 },
	{ SNDRV_PCM_FORMAT_U8, VIRTIO_SND_PCM_FMT_U8 },
	{ SNDRV_PCM_FORMAT_S16_LE, VIRTIO_SND_PCM_FMT_S16 },
	{ SNDRV_PCM_FORMAT_U16_LE, VIRTIO_SND_PCM_FMT_U16 },
	{ SNDRV_PCM_FORMAT_S18_3LE, VIRTIO_SND_PCM_FMT_S18_3 },
	{ SNDRV_PCM_FORMAT_U18_3LE, VIRTIO_SND_PCM_FMT_U18_3 },
	{ SNDRV_PCM_FORMAT_S20_3LE, VIRTIO_SND_PCM_FMT_S20_3 },
	{ SNDRV_PCM_FORMAT_U20_3LE, VIRTIO_SND_PCM_FMT_U20_3 },
	{ SNDRV_PCM_FORMAT_S24_3LE, VIRTIO_SND_PCM_FMT_S24_3 },
	{ SNDRV_PCM_FORMAT_U24_3LE, VIRTIO_SND_PCM_FMT_U24_3 },
#ifdef SNDRV_PCM_FORMAT_S20
	{ SNDRV_PCM_FORMAT_S20_LE, VIRTIO_SND_PCM_FMT_S20 },
#endif
#ifdef SNDRV_PCM_FORMAT_U20
	{ SNDRV_PCM_FORMAT_U20_LE, VIRTIO_SND_PCM_FMT_U20 },
#endif
	{ SNDRV_PCM_FORMAT_S24_LE, VIRTIO_SND_PCM_FMT_S24 },
	{ SNDRV_PCM_FORMAT_U24_LE, VIRTIO_SND_PCM_FMT_U24 },
	{ SNDRV_PCM_FORMAT_S32_LE, VIRTIO_SND_PCM_FMT_S32 },
	{ SNDRV_PCM_FORMAT_U32_LE, VIRTIO_SND_PCM_FMT_U32 },
	{ SNDRV_PCM_FORMAT_FLOAT_LE, VIRTIO_SND_PCM_FMT_FLOAT },
	{ SNDRV_PCM_FORMAT_FLOAT64_LE, VIRTIO_SND_PCM_FMT_FLOAT64 },
	{ SNDRV_PCM_FORMAT_DSD_U8, VIRTIO_SND_PCM_FMT_DSD_U8 },
	{ SNDRV_PCM_FORMAT_DSD_U16_LE, VIRTIO_SND_PCM_FMT_DSD_U16 },
	{ SNDRV_PCM_FORMAT_DSD_U32_LE, VIRTIO_SND_PCM_FMT_DSD_U32 },
	{ SNDRV_PCM_FORMAT_IEC958_SUBFRAME_LE,
	  VIRTIO_SND_PCM_FMT_IEC958_SUBFRAME }
};

struct virtsnd_a2v_rate {
	unsigned int rate;
	unsigned int vio_bit;
};

static const struct virtsnd_a2v_rate g_a2v_rate_map[] = {
	{ 5512, VIRTIO_SND_PCM_RATE_5512 },
	{ 8000, VIRTIO_SND_PCM_RATE_8000 },
	{ 11025, VIRTIO_SND_PCM_RATE_11025 },
	{ 12000, VIRTIO_SND_PCM_RATE_12000},
	{ 16000, VIRTIO_SND_PCM_RATE_16000 },
	{ 22050, VIRTIO_SND_PCM_RATE_22050 },
	{ 24000, VIRTIO_SND_PCM_RATE_24000 },
	{ 32000, VIRTIO_SND_PCM_RATE_32000 },
	{ 44100, VIRTIO_SND_PCM_RATE_44100 },
	{ 48000, VIRTIO_SND_PCM_RATE_48000 },
	{ 64000, VIRTIO_SND_PCM_RATE_64000 },
	{ 88200, VIRTIO_SND_PCM_RATE_88200 },
	{ 96000, VIRTIO_SND_PCM_RATE_96000 },
	{ 176400, VIRTIO_SND_PCM_RATE_176400 },
	{ 192000, VIRTIO_SND_PCM_RATE_192000 },
	{ 384000, VIRTIO_SND_PCM_RATE_384000 }
};

/* Conventional and unconventional sample rate supported */
static unsigned int supported_sample_rates[] = {
	5512, 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000,
	64000, 88200, 96000, 176400, 192000, 384000
};

static struct snd_pcm_hw_constraint_list constraints_sample_rates = {
	.count = ARRAY_SIZE(supported_sample_rates),
	.list = supported_sample_rates,
	.mask = 0,
};

static inline bool virtsnd_pcm_released(struct virtio_pcm_substream *substream)
{
	return atomic_read(&substream->msg_count) == 0;
}

static int virtsnd_pcm_release(struct virtio_pcm_substream *substream)
{
	struct virtio_snd *snd = substream->snd;
	struct virtio_snd_msg *msg;
	int rc;
	pr_debug("virtsnd_pcm_release: for stream_id[%d] enter\n", substream->sid);
	msg = virtsnd_pcm_ctl_msg_alloc(substream, VIRTIO_SND_R_PCM_RELEASE,
					GFP_KERNEL);
	if (IS_ERR(msg))
		return PTR_ERR(msg);

	rc = virtsnd_ctl_msg_send_sync(snd, msg);
	if (!rc) {
		long wait_ret = wait_event_interruptible_timeout(substream->msg_empty,
				virtsnd_pcm_released(substream),
				msecs_to_jiffies(MSG_DRAIN_TIMEOUT_MS));
		if (wait_ret <= 0) {
			pr_err("SID %u: drain timed out, msg_count[%d] -> resetting to 0\n",
			       substream->sid, atomic_read(&substream->msg_count));
			atomic_set(&substream->msg_count, 0);
		}
	} else {
		pr_err("Stream already closed, reset msg_count\n");
		atomic_set(&substream->msg_count, 0);
	}
	vsnd_dma_area_unexport(substream, substream->export_id);
	pr_debug("virtsnd_pcm_release: for stream_id[%d] exit with rc[%d]\n", substream->sid, rc);
	return rc;
}

static int virtsnd_pcm_open(struct snd_pcm_substream *substream)
{
	struct virtio_pcm *pcm = snd_pcm_substream_chip(substream);
	struct virtio_pcm_substream *ss = NULL;
	int ret = 0;
	pr_info("kpi : virtsnd_pcm_open: enter\n");

	if (pcm) {
		switch (substream->stream) {
			case SNDRV_PCM_STREAM_PLAYBACK:
			case SNDRV_PCM_STREAM_CAPTURE: {
				struct virtio_pcm_stream *stream =
					&pcm->streams[substream->stream];
				if (substream->number < stream->nsubstreams)
					ss = stream->substreams[substream->number];
				pr_debug("virtsnd_pcm_open: for stream_id[%d]\n", ss->sid);
				snd_pcm_hw_constraint_step(substream->runtime, 0,
					SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 64);
				snd_pcm_hw_constraint_step(substream->runtime, 0,
					SNDRV_PCM_HW_PARAM_BUFFER_BYTES, 64);
				atomic_set(&ss->suspended, 0);

				if (stream->substreams[substream->number]->hw.rates & SNDRV_PCM_RATE_KNOT) {

					ret = snd_pcm_hw_constraint_list(substream->runtime, 0,
								SNDRV_PCM_HW_PARAM_RATE,
								&constraints_sample_rates);
					if (ret < 0)
						pr_err("snd_pcm_hw_constraint_list failed\n");
				}

				break;
			}
		}
	}

	if (!ss)
		return -EBADFD;

	substream->runtime->hw = ss->hw;
	substream->private_data = ss;

	return 0;
}

static int virtsnd_pcm_close(struct snd_pcm_substream *substream)
{
	return 0;
}

static int virtsnd_pcm_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *hw_params)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct virtio_pcm_substream *ss = snd_pcm_substream_chip(substream);
	struct virtio_device *vdev = ss->snd->vdev;
	struct virtio_snd_msg *msg;
	struct virtio_snd_pcm_set_params *request;
	struct virtio_snd_pcm_set_params_v2 *request_v2;
	snd_pcm_format_t format;
	struct snd_dma_buffer *dma_buf = &substream->dma_buffer;
	unsigned int channels;
	unsigned int rate;
	unsigned int buffer_bytes;
	unsigned int period_bytes;
	unsigned int periods;
	unsigned int i;
	unsigned int is_mmap_noirq;
	int vformat = -1;
	int vrate = -1;
	int rc;
	u32 version = ss->snd->version;
	pr_debug("virtsnd_pcm_hw_params: for stream_id[%d] enter\n", ss->sid);
	if (!atomic_read(&ss->suspended)) {
		/*
		 * If we got here after ops->trigger() was called, the queue may
		 * still contain messages. In this case, we return an error
		 */
		/* Race condition between pcm queue and ctrl queue,  mgs_count might be less than 0 */
		if (atomic_read(&ss->msg_count) > 0) {
			dev_err(&vdev->dev, "SID %u: invalid I/O queue state\n",
				ss->sid);
			return -EBADFD;
		}
	}

	/* Set hardware parameters in device */
	if (hw_params) {
		format = params_format(hw_params);
		channels = params_channels(hw_params);
		rate = params_rate(hw_params);
		buffer_bytes = params_buffer_bytes(hw_params);
		period_bytes = params_period_bytes(hw_params);
		periods = params_periods(hw_params);
		is_mmap_noirq = hw_params->flags & SNDRV_PCM_HW_PARAMS_NO_PERIOD_WAKEUP ? 1 : 0;
	} else {
		format = runtime->format;
		channels = runtime->channels;
		rate = runtime->rate;
		buffer_bytes = frames_to_bytes(runtime, runtime->buffer_size);
		period_bytes = frames_to_bytes(runtime, runtime->period_size);
		periods = runtime->periods;
		is_mmap_noirq = 0;
	}

	for (i = 0; i < ARRAY_SIZE(g_a2v_format_map); ++i)
		if (g_a2v_format_map[i].alsa_bit == format) {
			vformat = g_a2v_format_map[i].vio_bit;

			break;
		}

	for (i = 0; i < ARRAY_SIZE(g_a2v_rate_map); ++i)
		if (g_a2v_rate_map[i].rate == rate) {
			vrate = g_a2v_rate_map[i].vio_bit;

			break;
		}

	if (vformat == -1 || vrate == -1)
		return -EINVAL;

	if (!runtime->dma_area) {
		/* set runtime buffer to prealloced dma buf*/
		dma_buf->dev.type = SNDRV_DMA_TYPE_DEV;
		dma_buf->dev.dev = substream->pcm->card->dev;
		dma_buf->private_data = NULL;
		dma_buf->area = ss->dma_data[DMA_BUF_DATA].vmap->vaddr;
		dma_buf->addr = 0;
		dma_buf->bytes = PAGE_ALIGN(buffer_bytes);
		snd_pcm_set_runtime_buffer(substream, &substream->dma_buffer);


		if (!is_mmap_noirq) {
			/* Allocate and initialize I/O messages */
			rc = virtsnd_pcm_msg_alloc(ss, periods, runtime->dma_area,
						   period_bytes);

			if (rc) {
		 		snd_pcm_set_runtime_buffer(substream, NULL);
				return rc;
			}
		}
	}

	msg = virtsnd_pcm_ctl_msg_alloc(ss, VIRTIO_SND_R_PCM_SET_PARAMS,
					GFP_KERNEL);
	if (IS_ERR(msg))
		return PTR_ERR(msg);

	request = (struct virtio_snd_pcm_set_params*)sg_virt(&msg->sg_request);

	request->buffer_bytes = cpu_to_virtio32(vdev, buffer_bytes);
	request->period_bytes = cpu_to_virtio32(vdev, period_bytes);
	request->channels = channels;
	request->format = vformat;
	request->rate = vrate;
	request->is_mmap_noirq = is_mmap_noirq;

	if (ss->features & (1U << VIRTIO_SND_PCM_F_MSG_POLLING))
		request->features |=
			cpu_to_virtio32(vdev,
				1U << VIRTIO_SND_PCM_F_MSG_POLLING);

	if (ss->features & (1U << VIRTIO_SND_PCM_F_EVT_XRUNS))
		request->features |=
			cpu_to_virtio32(vdev,
				1U << VIRTIO_SND_PCM_F_EVT_XRUNS);

	if (ss->features & (1U << VIRTIO_SND_PCM_F_HOSTLESS))
		request->features |=
			cpu_to_virtio32(vdev,
				1U << VIRTIO_SND_PCM_F_HOSTLESS);

	if (version == VSND_VERSION_2) {
		request_v2 = (struct virtio_snd_pcm_set_params_v2*)sg_virt(&msg->sg_request);
		request_v2->export_id = ss->export_id;
		request_v2->dma_bytes = dma_buf->bytes;

	}

	rc = virtsnd_ctl_msg_send_sync(ss->snd, msg);
	pr_debug("virtsnd_pcm_hw_params: for stream_id[%d] exit with rc [%d]\n", ss->sid, rc);
	return rc;
}

static int virtsnd_pcm_hw_free(struct snd_pcm_substream *substream)
{
	struct virtio_pcm_substream *ss = snd_pcm_substream_chip(substream);
	int rc;
	pr_debug("virtsnd_pcm_hw_free: for stream_id[%d] enter\n", ss->sid);
	rc = virtsnd_pcm_release(ss);

	/*
	 * Even if we failed to send the RELEASE message or wait for the queue
	 * flush to complete, we can safely delete the buffer. Because after
	 * receiving the STOP command, the device must stop all I/O message
	 * processing. If there are still pending messages in the queue, the
	 * next ops->hw_params() call should deal with this.
	 */
	snd_pcm_set_runtime_buffer(substream, NULL);

	if (atomic_read(&substream->mmap_count))
		atomic_set(&substream->mmap_count, 0);
	pr_debug("virtsnd_pcm_hw_free: for stream_id[%d] exit\n", ss->sid);
	return rc;
}

static int virtsnd_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct virtio_pcm_substream *ss = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct virtio_device *vdev = ss->snd->vdev;
	struct virtio_snd_msg *msg;
	unsigned long flags;
	int rc = 0;
	unsigned int wait_time_ms = 0;
	pr_debug("virtsnd_pcm_prepare: for stream_id[%d] enter\n", ss->sid);
	substream->runtime->stop_threshold = substream->runtime->boundary;

	//set wait time = 2 * period_time + fixed wait time
	if((runtime->rate) > 0 && (runtime->channels > 0)){
		wait_time_ms = WAIT_AVAIL_TIME_MS +
			(runtime->period_size * 2000) / (runtime->rate * runtime->channels);
	} else {
		pr_warn("Invalid runtime parameters for wait time, using default for stream_id[%d].\n", ss->sid);
		wait_time_ms = WAIT_AVAIL_TIME_MS;
	}
	substream->wait_time = wait_time_ms;
	pr_debug("virtsnd_pcm_prepare: wait_time_ms[%u] = 2*period_size[%lu]/(rate[%u]*channels[%u]) for stream_id[%d]\n",
		wait_time_ms, runtime->period_size, runtime->rate, runtime->channels, ss->sid);

	if (!atomic_read(&ss->suspended)) {
		struct virtio_snd_queue *queue = virtsnd_pcm_queue(ss);

		/*
		 * If we got here after ops->trigger() was called, the queue may
		 * still contain messages. In this case, return an error
		 */
		/* Race condition between pcm queue and ctrl queue,  mgs_count might be less than 0 */
		if (atomic_read(&ss->msg_count) > 0) {
			dev_err(&vdev->dev, "SID %u: invalid I/O queue state\n",
				ss->sid);
			return -EBADFD;
		}

		spin_lock_irqsave(&queue->lock, flags);
		spin_unlock_irqrestore(&queue->lock, flags);

		atomic_set(&ss->hw_ptr, 0);
	}
	memset(&ss->pcm_indirect, 0, sizeof(ss->pcm_indirect));
        ss->pcm_indirect.sw_buffer_size =
        	ss->pcm_indirect.hw_buffer_size =
        		snd_pcm_lib_buffer_bytes(substream);

	atomic_set(&ss->xfer_xrun, 0);
	atomic_set(&ss->msg_count, 0);
	atomic_set(&ss->suspended, 0);

	msg = virtsnd_pcm_ctl_msg_alloc(ss, VIRTIO_SND_R_PCM_PREPARE,
					GFP_KERNEL);
	if (IS_ERR(msg))
		return PTR_ERR(msg);

	rc = virtsnd_ctl_msg_send_sync(ss->snd, msg);
	pr_debug("virtsnd_pcm_prepare: for stream_id[%d] exit with rc [%d]\n", ss->sid, rc);
	dev_info(&vdev->dev, "kpi : virtsnd_pcm_prepare: exit\n");
	return rc;
}

static int virtsnd_pcm_trigger(struct snd_pcm_substream *substream, int command)
{
	struct virtio_pcm_substream *ss = snd_pcm_substream_chip(substream);
	struct virtio_snd *snd = ss->snd;
	struct virtio_snd_msg *msg;
	int rc = 0;

	switch (command) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
	case SNDRV_PCM_TRIGGER_RESUME: {
		pr_info("kpi : SNDRV_PCM_TRIGGER_START: enter\n");
		pr_debug("virtsnd_pcm_msg_send: first frame stream_id[%d] enter\n", ss->sid);
		if (!(ss->features & (1U << VIRTIO_SND_PCM_F_HOSTLESS)) &&
		    !(substream->runtime->no_period_wakeup)) {
			/* remove spin lock here to since alsa core already have stream lock*/
			if (ss->direction == SNDRV_PCM_STREAM_CAPTURE)
				rc = virtsnd_pcm_msg_send(ss, 0, ss->pcm_indirect.hw_buffer_size);
			if (rc)
				return rc;
		}

		atomic_set(&ss->xfer_enabled, 1);
		atomic_set(&ss->first_frame_done, 0);
		pr_info("virtsnd_pcm_trigger: start stream_id[%d] enter\n", ss->sid);
		msg = virtsnd_pcm_ctl_msg_alloc(ss, VIRTIO_SND_R_PCM_START,
						GFP_ATOMIC);
		if (IS_ERR(msg))
			return PTR_ERR(msg);

		rc = virtsnd_ctl_msg_send(snd, msg);
		pr_debug("virtsnd_pcm_trigger: start stream_id[%d] exit with rc [%d]\n", ss->sid, rc);
		pr_info("kpi : SNDRV_PCM_TRIGGER_START: exit\n");
		return rc;
	}
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_SUSPEND: {
		pr_debug("virtsnd_pcm_trigger: stop stream_id[%d] enter\n", ss->sid);
		atomic_set(&ss->xfer_enabled, 0);
		atomic_set(&ss->suspended, 1);

		msg = virtsnd_pcm_ctl_msg_alloc(ss, VIRTIO_SND_R_PCM_STOP,
						GFP_ATOMIC);
		if (IS_ERR(msg))
			return PTR_ERR(msg);

		rc = virtsnd_ctl_msg_send(snd, msg);
		pr_debug("virtsnd_pcm_trigger: stop stream_id[%d] exit with rc [%d]\n", ss->sid, rc);
		return rc;
	}
	default: {
		return -EINVAL;
	}
	}
}

static int virtsnd_pcm_mmap(struct snd_pcm_substream *substream, struct vm_area_struct *vma)
{
	int rc = 0;
	struct virtio_pcm_substream *vss = snd_pcm_substream_chip(substream);
	struct virtio_device *vdev = vss->snd->vdev;
	struct snd_pcm_runtime *runtime = substream->runtime;
	size_t dma_bytes = PAGE_ALIGN(runtime->dma_bytes);
	unsigned long len = vma->vm_end - vma->vm_start;
	unsigned long offset = vma->vm_pgoff * PAGE_SIZE;
	struct file *fptr = NULL;
	int data_fd = dma_buf_fd(vss->dma_data[DMA_BUF_DATA].dma_buf, O_CLOEXEC);
	int pos_fd = 0;

	/* set write permission for data buffer fd so userspace can write to it */
	fptr = fget(data_fd);
	if (!fptr) {
		dev_err(&vdev->dev, "%s: bad data fd", __func__);
		return -EBADFD;
	}

	fptr->f_mode = fptr->f_mode | FMODE_WRITE;

	if (substream->runtime->no_period_wakeup) {

		struct virtio_snd_pcm_push_pull_info *buf_info = NULL;
		struct virtio_snd_msg *data_msg = NULL;

		/* allocate dmabuf for push-pull mode position buffer */
		rc = virtsnd_alloc_dmabuf(vss, sizeof(struct virtio_pcm_push_pull_pos_buf), DMA_BUF_POS);
		if (rc)
			return -ENOMEM;

		/* determine fd position buffers after allocation */
		pos_fd = dma_buf_fd(vss->dma_data[DMA_BUF_POS].dma_buf, O_CLOEXEC);

		/* export data and position buffers to PVM */
		rc = vsnd_dma_area_export(vss, vss->dma_data[DMA_BUF_DATA].dma_buf,
					  dma_bytes, &vss->export_id);
		if (rc)
			return -EIO;

		rc = vsnd_dma_area_export(vss, vss->dma_data[DMA_BUF_POS].dma_buf,
				PAGE_ALIGN(sizeof(struct virtio_pcm_push_pull_pos_buf)), &vss->pos_buf_export_id);
		if (rc)
			return -EIO;

		/* send data and pos buf fd, size and export_id to PVM */
		data_msg = virtsnd_ctl_msg_alloc(vdev, sizeof(struct virtio_snd_pcm_push_pull_info),
						 sizeof(struct virtio_snd_hdr), GFP_KERNEL);
		if (IS_ERR(data_msg))
			return PTR_ERR(data_msg);

		buf_info = (struct virtio_snd_pcm_push_pull_info *)sg_virt(&data_msg->sg_request);
		buf_info->hdr.hdr.code = cpu_to_virtio32(vdev, VIRTIO_SND_R_PCM_MMAP);
		buf_info->hdr.stream_id = cpu_to_virtio32(vdev, vss->sid);
		buf_info->data_fd = cpu_to_virtio32(vdev, data_fd);
		buf_info->pos_fd = cpu_to_virtio32(vdev, pos_fd);
		buf_info->data_size = cpu_to_virtio32(vdev, PAGE_ALIGN(dma_bytes));
		buf_info->pos_size = cpu_to_virtio32(vdev, PAGE_ALIGN(sizeof(struct virtio_pcm_push_pull_pos_buf)));
		buf_info->data_export_id = cpu_to_virtio32(vdev, vss->export_id);
		buf_info->pos_export_id = cpu_to_virtio32(vdev, vss->pos_buf_export_id);

		rc = virtsnd_ctl_msg_send_sync(vss->snd, data_msg);
		if (rc)
			return rc;

		if (!strncmp(audio_variant, "awe", strlen("awe"))) {
			struct virtio_pcm_push_pull_pos_buf_awe *pos_buf = (struct virtio_pcm_push_pull_pos_buf_awe*)
									vss->dma_data[DMA_BUF_POS].vmap->vaddr;
			if (vss->direction == SNDRV_PCM_STREAM_PLAYBACK)
				pos_buf->write_index = INT_MAX;
			else
				pos_buf->read_index = INT_MAX;
		}
	}

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	if (offset >= len) {
		dev_err(&vdev->dev, "%s: offset is too large, offset %lu, len %lu", __func__, offset, len);
		return -EINVAL;
	}

	return dma_buf_mmap(vss->dma_data[DMA_BUF_DATA].dma_buf, vma, vma->vm_pgoff);
}


static snd_pcm_uframes_t
virtsnd_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct virtio_pcm_substream *ss = snd_pcm_substream_chip(substream);

	if (atomic_read(&ss->xfer_xrun))
		return SNDRV_PCM_POS_XRUN;

	if (substream->runtime->no_period_wakeup) {
		snd_pcm_sframes_t hw_frame_ptr;
		snd_pcm_sframes_t period_size = substream->runtime->period_size;

		if (!strncmp(audio_variant, "awe", strlen("awe"))) {
			struct virtio_pcm_push_pull_pos_buf_awe *pos_buf = (struct virtio_pcm_push_pull_pos_buf_awe*)
									ss->dma_data[DMA_BUF_POS].vmap->vaddr;
			if (ss->direction == SNDRV_PCM_STREAM_PLAYBACK)
				hw_frame_ptr = pos_buf->read_index;
			else
				hw_frame_ptr = pos_buf->write_index;
		} else {

			struct virtio_pcm_push_pull_pos_buf* pos_buf = (struct virtio_pcm_push_pull_pos_buf*)
							      ss->dma_data[DMA_BUF_POS].vmap->vaddr;
			uint32_t frame_cnt1, frame_cnt2;
			uint32_t read_index = 0;

			int i, j;

			/* try to get the latest update in the pos buffer */
			for (i = 0; i < 2; i++) {
				/* retry until there is an update from DSP */
				for (j = 0; j < 5; j++) {
					frame_cnt1 = pos_buf->frame_counter;
					if (frame_cnt1 != 0)
						break;
				}
				read_index = pos_buf->index;
				frame_cnt2 = pos_buf->frame_counter;

				if (frame_cnt1 != frame_cnt2)
					continue;
			}

			hw_frame_ptr = bytes_to_frames(substream->runtime, read_index);
		}
		return (hw_frame_ptr/period_size) * period_size;
	}

	return (snd_pcm_uframes_t)atomic_read(&ss->hw_ptr);
}

static void virtsnd_pcm_trans_copy(struct snd_pcm_substream *substream,
				   struct snd_pcm_indirect *rec, size_t bytes)
{
	struct virtio_pcm_substream *vss = snd_pcm_substream_chip(substream);
	virtsnd_pcm_msg_send(vss, rec->sw_data, bytes);
}

static snd_pcm_uframes_t virtsnd_pcm_pb_pointer(struct snd_pcm_substream *substream)
{
	struct virtio_pcm_substream *ss = snd_pcm_substream_chip(substream);

	if (substream->runtime->no_period_wakeup)
		return virtsnd_pcm_pointer(substream);

	if (atomic_read(&ss->xfer_xrun))
		return SNDRV_PCM_POS_XRUN;

	return snd_pcm_indirect_playback_pointer(substream, &ss->pcm_indirect,
			(snd_pcm_uframes_t)atomic_read(&ss->hw_ptr));
}

static snd_pcm_uframes_t virtsnd_pcm_cp_pointer(struct snd_pcm_substream *substream)
{
	struct virtio_pcm_substream *ss = snd_pcm_substream_chip(substream);

	if (substream->runtime->no_period_wakeup)
		return virtsnd_pcm_pointer(substream);

	if (atomic_read(&ss->xfer_xrun))
		return SNDRV_PCM_POS_XRUN;

	return snd_pcm_indirect_capture_pointer(substream, &ss->pcm_indirect,
			(snd_pcm_uframes_t)atomic_read(&ss->hw_ptr));
}

static int virtsnd_pcm_pb_ack(struct snd_pcm_substream *substream)
{
	struct virtio_pcm_substream *ss = snd_pcm_substream_chip(substream);
	int rc;

	if (substream->runtime->no_period_wakeup)
		return 0;

	/* remove spin lock here to since alsa core already have stream lock*/
	rc = snd_pcm_indirect_playback_transfer(substream, &ss->pcm_indirect,
						virtsnd_pcm_trans_copy);

	return rc;
}

static int virtsnd_pcm_cp_ack(struct snd_pcm_substream *substream)
{
	struct virtio_pcm_substream *ss = snd_pcm_substream_chip(substream);
	int rc;

	if (substream->runtime->no_period_wakeup)
		return 0;
	/* remove spin lock here to since alsa core already have stream lock*/
	rc = snd_pcm_indirect_capture_transfer(substream, &ss->pcm_indirect,
					       virtsnd_pcm_trans_copy);

	return rc;
}

const struct snd_pcm_ops virtsnd_pcm_ops[] = {
	{
		// Playback Ops
		.open = virtsnd_pcm_open,
		.close = virtsnd_pcm_close,
		.ioctl = snd_pcm_lib_ioctl,
		.hw_params = virtsnd_pcm_hw_params,
		.hw_free = virtsnd_pcm_hw_free,
		.prepare = virtsnd_pcm_prepare,
		.trigger = virtsnd_pcm_trigger,
		.pointer = virtsnd_pcm_pb_pointer,
		.ack = virtsnd_pcm_pb_ack,
		.mmap = virtsnd_pcm_mmap,
	},
	{
		// Capture Ops
		.open = virtsnd_pcm_open,
		.close = virtsnd_pcm_close,
		.ioctl = snd_pcm_lib_ioctl,
		.hw_params = virtsnd_pcm_hw_params,
		.hw_free = virtsnd_pcm_hw_free,
		.prepare = virtsnd_pcm_prepare,
		.trigger = virtsnd_pcm_trigger,
		.pointer = virtsnd_pcm_cp_pointer,
		.ack = virtsnd_pcm_cp_ack,
		.mmap = virtsnd_pcm_mmap,
	}
};
