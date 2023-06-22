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
 *​​​​​ Changes from Qualcomm Innovation Center are provided under the following license:
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/version.h>
#include <linux/virtio_config.h>
#include <sound/initval.h>

#include "virtio_card.h"

static int virtsnd_card_info(struct virtio_snd *snd)
{
	if (VIRTIO_HAS_OPSY_EXTENSION(snd, DEV_EXT_INFO)) {
		int code;

		code = virtsnd_ctl_alsa_card_info(snd);
		if (!code || code != -EOPNOTSUPP)
			return code;
	}

	strlcpy(snd->card->id, "viosnd", sizeof(snd->card->id));
	strlcpy(snd->card->driver, "virtio_snd", sizeof(snd->card->driver));
	strlcpy(snd->card->shortname, "VIOSND", sizeof(snd->card->shortname));
	strlcpy(snd->card->longname, "VirtIO Sound Card",
		sizeof(snd->card->longname));

	return 0;
}

static int virtsnd_build_devs(struct virtio_snd *snd)
{
	static struct snd_device_ops ops = { 0 };
	struct virtio_device *vdev = snd->vdev;
	int rc;

	rc = snd_card_new(&vdev->dev, SNDRV_DEFAULT_IDX1, SNDRV_DEFAULT_STR1,
			  THIS_MODULE, 0, &snd->card);
	if (rc < 0)
		return rc;

	snd->card->private_data = snd;

	rc = virtsnd_card_info(snd);
	if (rc)
		return rc;

	rc = snd_device_new(snd->card, SNDRV_DEV_LOWLEVEL, snd, &ops);
	if (rc < 0)
		return rc;

	rc = virtsnd_pcm_parse_cfg(snd);
	if (rc)
		return rc;

	rc = virtsnd_dc_parse_cfg(snd);
	if (rc)
		return rc;

	if (snd->nsubstreams) {
		rc = virtsnd_pcm_build_devs(snd);
		if (rc)
			return rc;
	}

	return snd_card_register(snd->card);
}

void process_ctl_msg(struct virtio_snd *snd, void *buff);
static DECLARE_COMPLETION(setup_done);

struct dma_area_export {
	struct virtio_snd_hdr hdr;
	uint32_t export_id;
	uint32_t dma_bytes;
	uint64_t dma_addr;
};

/* hab socket common receiving handler */
static int vsnd_kthread(void *d)
{
	struct vs_thread_struct *p = (struct vs_thread_struct *)d;
	int32_t ret = 0;
	unsigned char *buff = NULL;
	uint32_t sz = HAB_BUFFER_SIZE;
	struct virtio_snd *snd = (struct virtio_snd *)p->data;

	pr_info("%s mmid %d\n", __func__, p->mmid);

	ret = habmm_socket_open(&p->hab_socket, p->mmid, -1, 0);
	pr_info("%s mmid %d open return %d\n", __func__, p->mmid, ret);
	if (!ret) {
		pr_info("hab socket open mmid %d OK %X\n", p->mmid,
			p->hab_socket);

		if (p->mmid == MM_AUD_1)
			complete(&setup_done);
	} else {
		pr_err("hab open failed mmid %d ret %d\n", p->mmid, ret);
	}

	buff = kmalloc(sizeof(unsigned char) * HAB_BUFFER_SIZE, GFP_KERNEL);
	if (!buff) {
		pr_err("Hab buffer allocation failed\n");
		habmm_socket_close(p->hab_socket);
		return 1;
	}

	while (!p->stop) {
		struct virtio_snd_queue *queue = NULL;
		memset(buff, 0, sz);
		sz = HAB_BUFFER_SIZE;
		ret = habmm_socket_recv(p->hab_socket, buff, &sz, (uint32_t)-1,
					0); // request + response + payload
		if (ret) {
			pr_err("%s mmid %d failed %d size %d\n", __func__,
			       p->mmid, ret, sz);
			if (ret == -ENODEV)
				break;
		} else {
			pr_info("%s mmid %d ok size %d\n",
				__func__, p->mmid, sz);
		}

		if (p->mmid == MM_AUD_1) {
			process_ctl_msg(snd, buff);
		}

		else if (p->mmid == MM_AUD_3) {
			queue = virtsnd_tx_queue(snd);
			vsnd_process_pcm_msg(
				queue, (struct virtio_pcm_msg *)buff);
		} else if (p->mmid == MM_AUD_4) {
			queue = virtsnd_rx_queue(snd);
			vsnd_process_pcm_msg(
				queue, (struct virtio_pcm_msg *)buff);
		}
	}

	p->bexited = 1;
	ret = habmm_socket_close(p->hab_socket);
	pr_info("exit kthread mmid %d\n", p->mmid);
	return 0;
}



static struct virtio_device *g_vdev;

int vsnd_dma_area_export(struct virtio_pcm_substream *vss,
			 unsigned char *dma_area, size_t dma_bytes,
			 uint32_t *export_id)
{
	struct virtio_snd *snd = vss->snd;
	int ret;
	int32_t hab_socket;

	if (vss->direction == SNDRV_PCM_STREAM_PLAYBACK) {
		hab_socket =
			snd->queues[VIRTIO_SND_VQ_RX]
				.thread_data
				.hab_socket; // assume it is always RX for dma
	} else {
		hab_socket =
			snd->queues[VIRTIO_SND_VQ_TX].thread_data.hab_socket;
	}

	if (vss->export_ready)
		pr_err("dma area exported already! direction %d vcid %X exp_id %d\n",
		       vss->direction, hab_socket, vss->export_id);
	else
		pr_info("dma area export request direction %d vcid %X exp_id %d\n",
			vss->direction, hab_socket, vss->export_id);

	ret = habmm_export(hab_socket, dma_area, dma_bytes, export_id, HABMM_EXP_MEM_TYPE_DMA);
	if (!ret) {
		pr_info("dma area export ok on RX %zu bytes exp id %d\n",
			dma_bytes, *export_id);
	} else {
		pr_err("dma area export failed %d vcid %X\n", ret, hab_socket);
	}
	return ret;
}

#define VSND_EVENTQ_SZ 32

static void vsnd_reset(struct virtio_device *dev)
{
	pr_info("%s: virtio_device is being reset!\n", __func__);

}

static void vsnd_set_status(struct virtio_device *dev, uint8_t status)
{
	pr_info("%s: setting status %d\n", __func__, status);

}

static uint8_t vsnd_get_status(struct virtio_device *dev)
{
	pr_info("%s: getting status\n", __func__);
	return 0;
}
static const struct virtio_config_ops virtio_snd_config_ops = {
	.reset = vsnd_reset,
	.set_status = vsnd_set_status,
	.get_status = vsnd_get_status,
};


static int __init vsnd_init(void)
{
	struct virtio_snd *snd;
	unsigned int i;
	int rc;
	struct virtio_device *vdev = NULL;
	vdev = kzalloc(sizeof(struct virtio_device), GFP_KERNEL);

	if (vdev != NULL) {
		vdev->config = &virtio_snd_config_ops;
		pr_info("%s: registering virtio device\n", __func__);
		rc = register_virtio_device(vdev);
		if (rc) {
			pr_err("%s: virtio_device registration failed\n", __func__);
			return -1;
		}
	}
	else {
		pr_err("%s: failed to allocate virtio_device\n", __func__);

		return -1;
	}

	g_vdev = vdev;
	snd = devm_kzalloc(&vdev->dev, sizeof(*snd), GFP_KERNEL);
	if (!snd)
		return -ENOMEM;

	snd->vdev = vdev;
	INIT_LIST_HEAD(&snd->ctl_msgs);
	INIT_LIST_HEAD(&snd->pcm_list);

	vdev->priv = snd;

	for (i = 0; i < VIRTIO_SND_VQ_MAX; ++i) {
		spin_lock_init(&snd->queues[i].lock);

		snd->queues[i].thread_data.mmid = MM_AUD_1 + i;
		snd->queues[i].thread_data.data = snd;

		snd->queues[i].kthread =
			kthread_run(vsnd_kthread, &snd->queues[i].thread_data,
				    "vsnd kthread");
		if (IS_ERR(snd->queues[i].kthread)) {
			pr_err("failed to create kthread mmid %d ret %p\n",
			       snd->queues[i].thread_data.mmid,
			       snd->queues[i].kthread);
			rc = -EINVAL;
			goto err;
		}
	}

	wait_for_completion(&setup_done);

	snd->event_msgs = kmalloc_array(VSND_EVENTQ_SZ,
					sizeof(*snd->event_msgs), GFP_KERNEL);
	if (!snd->event_msgs) {
		pr_err("failed to allocate event array %d bytes\n",
		       VSND_EVENTQ_SZ * sizeof(*snd->event_msgs));
		return -ENOMEM;
	}

	for (i = 0; i < VSND_EVENTQ_SZ; ++i) {
	}

	rc = virtsnd_build_devs(snd);

err:
	return 0;
}

static void __exit vsnd_exit(void)
{
	struct virtio_device *vdev = g_vdev;
	struct virtio_snd *snd = vdev->priv;

	pr_info("%s\n", __func__);

	if (snd->card)
		snd_card_free(snd->card);
}


module_init(vsnd_init); // disable it if kernel config is already done to prevent loading
module_exit(vsnd_exit);

MODULE_DESCRIPTION("Virtio sound card driver");
MODULE_LICENSE("GPL");
