/* SPDX-License-Identifier: GPL-2.0+ */
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
#ifndef VIRTIO_SND_PCM_H
#define VIRTIO_SND_PCM_H

#include <linux/version.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/virtio_config.h>
#include <sound/pcm.h>
#include <linux/dma-mapping.h>
#include <linux/dma-buf.h>
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0))
#include <linux/iosys-map.h>
#else
#include <linux/dma-buf-map.h>
#endif
#include <linux/dma-heap.h>
MODULE_IMPORT_NS(DMA_BUF);

struct virtio_pcm;
struct virtio_pcm_msg;
struct virtio_snd_queue;

enum dma_buf_index {
	DMA_BUF_INDEX_NONE = -1,
	DMA_BUF_DATA,
	DMA_BUF_POS,

	DMA_BUF_INDEX_MAX = DMA_BUF_POS,
};

struct dma_buf_data {
#ifdef __DMA_BUF_MAP_H__ // kernel 5.15 uses dma-buf-map.h
	struct dma_buf_map *vmap;
#else                    // kernel 6.1 uses iosys-map.h
	struct iosys_map *vmap;
#endif
	struct dma_buf *dma_buf;
	struct dma_buf_attachment *attach;
	struct sg_table *table;
};

/**
 * struct virtio_pcm_substream - virtio PCM substream representation.
 * @snd: Virtio sound card device.
 * @nid: Functional group node identifier.
 * @sid: Stream identifier.
 * @direction: Stream data flow direction (VIRTIO_SND_D_XXX).
 * @features: Stream virtio feature bit map (1 << VIRTIO_SND_PCM_F_XXX).
 * @substream: Kernel substream.
 * @hw: Kernel substream hardware descriptor.
 * @hw_ptr: Substream hardware pointer value.
 * @xfer_enabled: Data transfer state.
 * @xfer_draining: Data draining state.
 * @xfer_xrun: Data underflow/overflow state.
 * @msg_list: Pending I/O message list.
 * @msg_empty: Notify when msg_list is empty.
 */
struct virtio_pcm_substream {
	struct virtio_snd *snd;
	unsigned int nid;
	unsigned int sid;
	u32 direction;
	u32 features;
	struct snd_pcm_substream *substream;
	struct snd_pcm_hardware hw;
	atomic_t hw_ptr;
	atomic_t xfer_enabled;
	atomic_t xfer_xrun;
	atomic_t suspended;
	struct virtio_pcm_msg *msgs;
	int msg_last_enqueued;
	atomic_t msg_count;
	wait_queue_head_t msg_empty;

	int export_ready; /*dma area is shared with PVM */
	u32 export_id;
	u32 pos_buf_export_id; /* this export id is used in push-pull mode only */
	struct dma_buf_data dma_data[DMA_BUF_INDEX_MAX + 1];
};

struct virtio_pcm_push_pull_pos_buf {

	volatile uint32_t frame_counter;
	/**  Counter used to handle interprocessor synchronization issues associated
		with reading write_index, timestamp_us_lsw, and timestamp_us_msw.
		These are invalid when frame_counter = 0.

		Read the frame_counter value both before and after reading these values
		to make sure the spf did not update them while the client was reading them.
	*/

	volatile uint32_t index;
	/**  Index in bytes to where the spf is writing (push mode) or reading (pull mode).
		"0 &ge; index &gt; sh_mem_pull_push_mode_cfg_t::shared_circ_buf_size - 1"}
	*/

	volatile uint32_t timestamp_us_lsw;
	/**  Upper 32 bits of the 64-bit timestamp in microseconds.
		For pull mode, the timestamp is the timestamp at which index was updated.
		For push mode, the timestamp is the buffer or the capture timestamp of the sample at index.
	*/

	volatile uint32_t timestamp_us_msw;
	/**  Upper 32 bits of the 64-bit timestamp in microseconds.
		For pull mode, the timestamp is the timestamp at which index was updated.
		For push mode, the timestamp is the buffer or the capture timestamp of the sample at index.
	*/	
};

/**
 * struct virtio_pcm_stream - virtio PCM stream representation.
 * @substreams: Virtio substreams belonging to the stream.
 * @nsubstreams: Number of substreams.
 * @chmaps: Kernel channel maps belonging to the stream.
 * @nchmaps: Number of channel maps.
 */
struct virtio_pcm_stream {
	struct virtio_pcm_substream **substreams;
	unsigned int nsubstreams;
	struct snd_pcm_chmap_elem *chmaps;
	unsigned int nchmaps;
};

/**
 * struct virtio_pcm - virtio PCM device representation.
 * @list: PCM list entry.
 * @nid: Functional group node identifier.
 * @pcm: Kernel PCM device.
 * @streams: Virtio streams (playback and capture).
 */
struct virtio_pcm {
	struct list_head list;
	unsigned int nid;
	struct snd_pcm *pcm;
	struct virtio_pcm_stream streams[SNDRV_PCM_STREAM_LAST + 1];
};

extern const struct snd_pcm_ops virtsnd_pcm_ops;

int virtsnd_pcm_validate(struct virtio_device *vdev);

int virtsnd_pcm_parse_cfg(struct virtio_snd *snd);

int virtsnd_pcm_check_cfg(struct virtio_snd *snd);

int virtsnd_pcm_build_devs(struct virtio_snd *snd);

#ifdef CONFIG_PM_SLEEP
int virtsnd_pcm_restore(struct virtio_snd *snd);
#endif /* CONFIG_PM_SLEEP */

void virtsnd_pcm_event(struct virtio_snd *snd, struct virtio_snd_event *event);

void virtsnd_pcm_tx_notify_cb(struct virtqueue *vqueue);

void virtsnd_pcm_rx_notify_cb(struct virtqueue *vqueue);

struct virtio_pcm *virtsnd_pcm_find(struct virtio_snd *snd, unsigned int nid);

struct virtio_pcm *virtsnd_pcm_find_or_create(struct virtio_snd *snd,
					      unsigned int nid);

struct virtio_snd_msg *
virtsnd_pcm_ctl_msg_alloc(struct virtio_pcm_substream *substream,
			  unsigned int command, gfp_t gfp);

int virtsnd_pcm_msg_alloc(struct virtio_pcm_substream *substream,
			  unsigned int nmsg, u8 *dma_area,
			  unsigned int period_bytes);

int virtsnd_pcm_msg_send(struct virtio_pcm_substream *substream);

int vsnd_dma_area_export(struct virtio_pcm_substream *vss,
			 struct dma_buf *dma_area, size_t dma_bytes,
			 uint32_t *export_id);
void vsnd_process_pcm_msg(struct virtio_snd_queue *queue, struct virtio_pcm_msg *msg);

int virtsnd_alloc_dmabuf(struct virtio_pcm_substream *substream, size_t size, enum dma_buf_index index);

void vsnd_dma_area_unexport(struct virtio_pcm_substream *vss, uint32_t export_id);

#endif /* VIRTIO_SND_PCM_H */
