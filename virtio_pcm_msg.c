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
#include <sound/pcm_params.h>
#include <linux/mm.h>

#include "virtio_card.h"
#define MAX_SEND_PACKET_RETRY    10

/**
 * enum pcm_msg_sg_index - Scatter-gather element indexes for an I/O message
 * @PCM_MSG_SG_XFER: Element containing a virtio_snd_pcm_xfer structure
 * @PCM_MSG_SG_DATA: Element containing a data buffer
 * @PCM_MSG_SG_STATUS: Element containing a virtio_snd_pcm_status structure
 * @PCM_MSG_SG_MAX: The maximum number of elements in the scatter-gather table
 *
 * These values are used as the index of the scatter-gather table.
 */
enum pcm_msg_sg_index {
	PCM_MSG_SG_XFER = 0,
	PCM_MSG_SG_DATA,
	PCM_MSG_SG_STATUS,
	PCM_MSG_SG_MAX
};

/**
 * struct virtio_pcm_msg - I/O message representation
 * @list: Pending I/O message list entry
 * @stream: Pointer to virtio PCM stream structure
 * @xfer: I/O message header payload
 * @status: I/O message status payload
 * @one_shot_data: if the message should not be resent to the device, the field
 *                 contains a pointer to the optional payload that should be
 *                 released after completion
 * @sgs: I/O message payload scatter-gather table
 */
struct virtio_pcm_msg {
	struct virtio_pcm_substream *substream;
	struct virtio_snd_pcm_xfer xfer;
	struct virtio_snd_pcm_status status;
	size_t length;
	unsigned int sid;

	struct dma_data_desc {
		uint64_t addr;
		uint32_t offset;
		uint32_t period;
		uint32_t dma_bytes;
		uint32_t export_id;
	} desc;

	struct scatterlist sgs[PCM_MSG_SG_MAX];
};

int virtsnd_pcm_msg_alloc(struct virtio_pcm_substream *substream,
			  unsigned int nmsg, u8 *dma_area,
			  unsigned int period_bytes)
{
	struct virtio_device *vdev = substream->snd->vdev;
	struct snd_pcm_runtime *runtime = substream->substream->runtime;
	unsigned int i;
	int32_t ret;
	size_t dma_bytes = PAGE_ALIGN(runtime->dma_bytes);


	if (substream->msgs) {
		devm_kfree(&vdev->dev, substream->msgs);
		substream->msgs = NULL;
	}

	substream->msgs = devm_kcalloc(&vdev->dev, nmsg,
				       sizeof(*substream->msgs), GFP_KERNEL);
	if (!substream->msgs)
		return -ENOMEM;

	if (IS_ERR_OR_NULL((void*)substream->dma_data[DMA_BUF_DATA].dma_buf))
		return -ENOMEM;

	/* export dma area to remote VM */
	ret = vsnd_dma_area_export(substream, substream->dma_data[DMA_BUF_DATA].dma_buf, dma_bytes,
				   &substream->export_id);
	if (ret) {
		pr_err("failed to export dma area of %zu bytes to PVM return %d\n",
		       runtime->dma_bytes, ret);
		substream->export_id = -1;
		substream->export_ready = 0;
	} else {
		pr_info("export dma area of %zu bytes OK exp id %d\n",
			runtime->dma_bytes, substream->export_id);
		substream->export_ready = 1;
	}

	for (i = 0; i < nmsg; ++i) {
		u8 *data = runtime->dma_area + period_bytes * i;
		struct virtio_pcm_msg *msg = &substream->msgs[i];

		msg->substream = substream;

		sg_init_table(msg->sgs, PCM_MSG_SG_MAX);
		sg_init_one(&msg->sgs[PCM_MSG_SG_XFER], &msg->xfer,
			    sizeof(msg->xfer));
		sg_init_one(&msg->sgs[PCM_MSG_SG_DATA],
			    dma_area + period_bytes * i, period_bytes);
		sg_init_one(&msg->sgs[PCM_MSG_SG_STATUS], &msg->status,
			    sizeof(msg->status));


		msg->desc.addr = (uint64_t)data;
		msg->desc.offset = period_bytes * i;
		msg->desc.period = i;
		msg->desc.export_id = substream->export_id;
		msg->desc.dma_bytes = dma_bytes;
	}

	return 0;
}

int virtsnd_pcm_msg_send(struct virtio_pcm_substream *substream, unsigned long offset, unsigned long bytes)
{
	struct virtio_snd *snd = substream->snd;
	struct virtio_device *vdev = snd->vdev;
	unsigned long period_bytes = snd_pcm_lib_period_bytes(substream->substream);
	unsigned long start, end, i;
	int32_t hab_socket;
	int retry_times = 0;
	int rc;
	start = offset / period_bytes;
	end = (offset + bytes - 1) / period_bytes;
	for (i = start; i <= end; i++) {
		struct virtio_pcm_msg *msg = &substream->msgs[i];
		unsigned long n;

		n = period_bytes - (offset % period_bytes);
		if (n > bytes)
			n = bytes;

		msg->length += n;
		if (msg->length == period_bytes) {
			msg->xfer.stream_id = cpu_to_virtio32(vdev, substream->sid);
			memset(&msg->status, 0, sizeof(msg->status));
			atomic_inc(&substream->msg_count);

			if (substream->direction == SNDRV_PCM_STREAM_PLAYBACK)

				hab_socket = snd->queues[VIRTIO_SND_VQ_RX].thread_data.hab_socket; // Playback uses RX
			else {
				hab_socket = snd->queues[VIRTIO_SND_VQ_TX].thread_data.hab_socket; // Capture uses TX
			}

 retry_send_packet:
			rc = habmm_socket_send(hab_socket, msg, sizeof(*msg), HABMM_SOCKET_SEND_FLAGS_NON_BLOCKING);
			if (rc) {
				dev_err(&vdev->dev,
					"SID %u: failed to send I/O message vcid %X ret %d msgsz %zd】\n",
					substream->sid, hab_socket, rc, sizeof(*msg));
				if ((rc == -EAGAIN) && (retry_times < MAX_SEND_PACKET_RETRY)) {
					retry_times++;
					dev_err(&vdev->dev, "send packet retry %d", retry_times);
					goto retry_send_packet;
				}
				atomic_dec(&substream->msg_count);
				return -EIO;
			}
		}
		offset = 0;
		bytes -= n;
	}
	return 0;
}

static void virtsnd_pcm_msg_complete(struct virtio_pcm_msg *msg, size_t size)
{
	struct virtio_pcm_substream *substream = msg->substream;
	struct snd_pcm_runtime *runtime = substream->substream->runtime;
	snd_pcm_uframes_t hw_ptr;
	u32 msg_length;
	msg_length = size - sizeof(msg->status);
	/* TODO: propagate an error to upper layer? */
	if (le32_to_cpu(msg->status.status) != VIRTIO_SND_S_OK)
	{
		pr_err("virtsnd_pcm_msg_complete: get error response\n");
		return;
	}

	if (!atomic_read(&substream->first_frame_done)) {
		pr_info("kpi : virtsnd_pcm_msg_complete first_frame_done for stream_id[%d]\n", substream->sid);
		atomic_set(&substream->first_frame_done, 1);
	}

	hw_ptr = (snd_pcm_uframes_t)atomic_read(&substream->hw_ptr);

	if (substream->direction == SNDRV_PCM_STREAM_PLAYBACK) {
		hw_ptr += msg_length;
	} else {
		if (size > sizeof(struct virtio_snd_pcm_status))
			size -= sizeof(struct virtio_snd_pcm_status);
		else
			/* TODO: propagate an error to upper layer? */
			{
				pr_err("virtsnd_pcm_msg_complete: not enough size[%zu]\n", size);
				return;
			}
		hw_ptr += size;
	}

	atomic_set(&substream->hw_ptr, (u32)(hw_ptr % snd_pcm_lib_buffer_bytes(substream->substream)));
	atomic_set(&substream->xfer_xrun, 0);

	runtime->delay = bytes_to_frames(
		runtime, le32_to_cpu(msg->status.latency_bytes));

	substream->msgs[msg->desc.period].length = 0;
	snd_pcm_period_elapsed(substream->substream);
}

static inline void virtsnd_pcm_notify_cb(struct virtio_snd_queue *queue, struct virtio_pcm_msg *msg)
{
	unsigned long flags;
	struct virtio_pcm_substream *substream;
	unsigned int msg_count;
	u32 length;
	spin_lock_irqsave(&queue->lock, flags);

			length = msg->length;
			substream = msg->substream;
			msg_count = atomic_dec_return(&substream->msg_count);

			if (atomic_read(&substream->xfer_enabled)) {
				virtsnd_pcm_msg_complete(msg, length);
			} else if (!msg_count) {
				wake_up_all(&substream->msg_empty);
			}
	spin_unlock_irqrestore(&queue->lock, flags);
}

void vsnd_process_pcm_msg(struct virtio_snd_queue *queue,
			  struct virtio_pcm_msg *msg)
{
	virtsnd_pcm_notify_cb(queue, msg);
}

struct virtio_snd_msg *
virtsnd_pcm_ctl_msg_alloc(struct virtio_pcm_substream *substream,
			  unsigned int command, gfp_t gfp)
{
	struct virtio_device *vdev = substream->snd->vdev;
	size_t request_size = sizeof(struct virtio_snd_pcm_hdr);
	size_t response_size = sizeof(struct virtio_snd_hdr);
	struct virtio_snd_msg *msg;

	switch (command) {
	case VIRTIO_SND_R_PCM_SET_PARAMS: {
		if (substream->snd->version == VSND_VERSION_2)
			request_size = sizeof(struct virtio_snd_pcm_set_params_v2);
		else
			request_size = sizeof(struct virtio_snd_pcm_set_params);
		break;
	}
	}

	msg = virtsnd_ctl_msg_alloc(vdev, request_size, response_size, gfp);
	if (!IS_ERR(msg)) {
		struct virtio_snd_pcm_hdr *hdr = sg_virt(&msg->sg_request);

		hdr->hdr.code = cpu_to_virtio32(vdev, command);
		hdr->stream_id = cpu_to_virtio32(vdev, substream->sid);
	}

	return msg;
}
