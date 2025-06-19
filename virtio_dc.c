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
#include <sound/control.h>
#include <linux/virtio_config.h>
#include <linux/string.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/limits.h>
#include "virtio_card.h"

/**
 * struct virtio_kctl - virtio device control representation.
 * @kctl: Kernel device control.
 * @info: Device control information.
 * @enum_values: Values for the ENUMERATED control type.
 */
struct virtio_kctl {
	struct snd_kcontrol *kctl;
	struct virtio_snd_dc_info *info;
	struct virtio_snd_dc_enum_value *enum_values;
};

/**
 * struct virtio_kctl_ctx - device control context.
 * @events_enabled: Event handling state.
 * @kctls: Virtio device controls.
 * @nkctls: Number of device controls.
 */
struct virtio_kctl_ctx {
	atomic_t events_enabled;
	struct virtio_kctl *kctls;
	unsigned int nkctls;
};

/**
 * struct virtio_snd_dc_info - virtio device control information.
 * @events_enabled: Event handling state.
 * @hdr: Common virtio item information header.
 * @elem_info: ALSA element information (uapi/sound/asound.h).
 */
struct virtio_snd_dc_info {
	struct virtio_snd_info hdr;
	struct snd_ctl_elem_info elem_info;
};

DECLARE_COMPLETION(dc_setup_done);

static int virtsnd_dc_info(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_info *uinfo)
{
	struct virtio_snd *snd = kcontrol->private_data;
	struct virtio_kctl_ctx *ctx = snd->kctl_ctx;
	struct virtio_kctl *kctl = &ctx->kctls[kcontrol->private_value];
	struct snd_ctl_elem_info *info = &kctl->info->elem_info;

	if (info->type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
		unsigned int item = uinfo->value.enumerated.item;

		if (item >= info->value.enumerated.items)
			return -EINVAL;

		strscpy(info->value.enumerated.name,kctl->enum_values[item].name,sizeof(info->value.enumerated.name));
	}

	memcpy(uinfo, info, sizeof(*uinfo));

	uinfo->id = info->id;

	return 0;
}

static int virtsnd_dc_get(struct snd_kcontrol *kcontrol,
			  struct snd_ctl_elem_value *ucontrol)
{
	struct virtio_snd *snd = kcontrol->private_data;
	struct virtio_device *vdev = snd->vdev;
	struct virtio_snd_msg *msg;
	struct virtio_snd_dc_hdr *hdr;
	unsigned int subcid = snd_ctl_get_ioff(kcontrol, &ucontrol->id);
	struct scatterlist sg;
	msg = virtsnd_ctl_msg_alloc(vdev, sizeof(*hdr),
				    sizeof(struct virtio_snd_hdr), GFP_KERNEL);
	if (IS_ERR(msg))
		return PTR_ERR(msg);

	hdr = sg_virt(&msg->sg_request);
	hdr->hdr.code = cpu_to_virtio32(vdev, VIRTIO_SND_R_DC_READ);
	hdr->control_id = cpu_to_virtio16(vdev, kcontrol->private_value);
	hdr->subcontrol_id = cpu_to_virtio16(vdev, subcid);

	sg_init_one(&sg, ucontrol, sizeof(*ucontrol));
	msg->sg_response_ext = &sg;

	msg->reply = ucontrol;
	msg->reply_size = sizeof(*ucontrol);

	return virtsnd_ctl_msg_send_sync(snd, msg);
}

static int virtsnd_dc_put(struct snd_kcontrol *kcontrol,
			  struct snd_ctl_elem_value *ucontrol)
{
	struct virtio_snd *snd = kcontrol->private_data;
	struct virtio_device *vdev = snd->vdev;
	struct virtio_snd_msg *msg;
	struct virtio_snd_dc_hdr *hdr;
	unsigned int subcid = snd_ctl_get_ioff(kcontrol, &ucontrol->id);
	struct scatterlist sg;
	msg = virtsnd_ctl_msg_alloc(vdev, sizeof(*hdr),
				    sizeof(struct virtio_snd_hdr), GFP_KERNEL);
	if (IS_ERR(msg))
		return PTR_ERR(msg);

	hdr = sg_virt(&msg->sg_request);
	hdr->hdr.code = cpu_to_virtio32(vdev, VIRTIO_SND_R_DC_WRITE);
	hdr->control_id = cpu_to_virtio16(vdev, kcontrol->private_value);
	hdr->subcontrol_id = cpu_to_virtio16(vdev, subcid);

	sg_init_one(&sg, ucontrol, sizeof(*ucontrol));
	msg->sg_request_ext = &sg;

	msg->request_ext_size = sizeof(*ucontrol);

	return virtsnd_ctl_msg_send_sync(snd, msg);
}

static int vsnd_dc_dma_area_export(struct virtio_snd *snd,
	struct dma_buf* dma_area, size_t dma_bytes,
	uint32_t *export_id)
{
	int ret;
	int32_t hab_socket;

	/* use control queue to export dmabuf */

	hab_socket = snd->queues[VIRTIO_SND_VQ_CONTROL].thread_data.hab_socket;
	if (hab_socket <= 0) {
		dev_err(&snd->vdev->dev, "Invalid HAB socket %d", hab_socket);
		*export_id = 0;
		return -EINVAL;
	}
	ret = habmm_export(hab_socket, dma_area, dma_bytes, export_id, HABMM_EXPIMP_FLAGS_DMABUF);
	if (!ret) {
		dev_info(&snd->vdev->dev,"dma area export ok on RX %zu bytes exp id %d\n",
			dma_bytes, *export_id);
	} else {
		dev_err(&snd->vdev->dev,
			"dma area export failed %d vcid %X", ret, hab_socket);
		*export_id = 0;
	}
	return ret;
}

static void vsnd_dc_dma_area_unexport(struct virtio_snd *snd, uint32_t export_id)
{
	int32_t hab_socket;
	int ret;

	/* use control queue to unexport dmabuf */
	hab_socket = snd->queues[VIRTIO_SND_VQ_CONTROL].thread_data.hab_socket;
	if (hab_socket <= 0) {
		dev_err(&snd->vdev->dev, "Invalid HAB socket %d", hab_socket);
		return;
	}
	ret = habmm_unexport(hab_socket, export_id, HABMM_EXPIMP_FLAGS_DMABUF);
	if (ret)
		dev_err(&snd->vdev->dev, "%s: habmm_unexport failed: %d", __func__, ret);

}

static int virtsnd_dc_shmem_map_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct virtio_snd *snd = kcontrol->private_data;
	struct virtio_device *vdev = snd->vdev;
	struct virtio_snd_msg *msg;
	struct virtio_snd_dc_hdr *hdr;
	unsigned int subcid = snd_ctl_get_ioff(kcontrol, &ucontrol->id);
	struct scatterlist sg;
	struct dma_buf *dma_buf = NULL;
	int rc = -EINVAL;
	size_t alloc_size = 0;
	uint32_t export_id = 0;
	uint32_t *pint = (uint32_t *)&ucontrol->value.bytes.data[0];
	// ucontrol has expected elemenents in ucontrol->value.bytes.value[]
	// [0] - data_export_id OUT
	// [1] - fd
	// [2] - export_fd (ignored)
	// [3] - data_size IN
	// [4-7] -addr_lsw 0(ignored)
	// [8-11] -addr_msw 0(ignored)
	int fd = 0;

	if (pint[3] == 0 || PAGE_ALIGN(pint[3]) < pint[3]) {
		dev_err(&vdev->dev, "virtsnd_dc_shmem_map_put: invalid allocation size %u", pint[3]);
		rc = -EINVAL;
		goto exit;
	}
	alloc_size = PAGE_ALIGN(pint[3]);
	fd = pint[1];
	if (fd < 0) {
		dev_err(&vdev->dev, "virtsnd_dc_shmem_map_put: invalid fd %d", fd);
		rc = -EINVAL;
		goto exit;
	}

	dma_buf = dma_buf_get(fd);
	if (IS_ERR_OR_NULL(dma_buf)) {
		dev_err(&vdev->dev,
			"virtsnd_dc_shmem_map_put: failed to get dmabuf with fd %d",fd);
		rc = -ENOMEM;
		goto exit;
	}

	rc = vsnd_dc_dma_area_export(snd, dma_buf, alloc_size, &export_id);
	if (rc || export_id == 0){
		dev_err(&vdev->dev,
			"dma_area_export fails %d",rc);
		goto exit;
	}
	msg = virtsnd_ctl_msg_alloc(vdev, sizeof(*hdr),
			sizeof(struct virtio_snd_hdr), GFP_KERNEL);
	if (IS_ERR(msg)){
		dev_err(&vdev->dev,
			"Failed to allocate message!");
		vsnd_dc_dma_area_unexport(snd, export_id);
		rc = -ENOMEM;
		goto exit;
	}

	hdr = sg_virt(&msg->sg_request);
	hdr->hdr.code = cpu_to_virtio32(vdev, VIRTIO_SND_R_SHMEM_MAP);
	hdr->control_id = cpu_to_virtio16(vdev, kcontrol->private_value);
	hdr->subcontrol_id = cpu_to_virtio16(vdev, subcid);

	pint[0] = cpu_to_virtio32(vdev,export_id);
	pint[1] = cpu_to_virtio32(vdev,fd);//data_fd

	sg_init_one(&sg, ucontrol, sizeof(*ucontrol));
	msg->sg_request_ext = &sg;

	msg->request_ext_size = sizeof(*ucontrol);
	rc = virtsnd_ctl_msg_send_sync(snd, msg);
	if(rc){
		dev_err(&vdev->dev,
			"ShmemMap:failed to send message, err %d!", rc);
	}
exit:
	if (!IS_ERR_OR_NULL(dma_buf))
		dma_buf_put(dma_buf);
	return rc;
}


static int virtsnd_dc_shmem_unmap_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct virtio_snd *snd = kcontrol->private_data;
	struct virtio_device *vdev = snd->vdev;
	struct virtio_snd_msg *msg;
	struct virtio_snd_dc_hdr *hdr;
	unsigned int subcid = snd_ctl_get_ioff(kcontrol, &ucontrol->id);
	struct scatterlist sg;
	uint32_t export_id = 0;
	int rc = 0;
	int fd = 0;
	uint32_t *pint = (uint32_t *)&ucontrol->value.bytes.data[0];
	// ucontrol has expected elemenents in ucontrol->value.bytes.value[]
	// [0] - data_export_id OUT
	// [1] - fd
	// [2] - export_fd (ignored)
	// [3] - data_size IN
	// [4-7] -addr_lsw 0(ignored)
	// [8-11] -addr_msw 0(ignored)
	dev_info(&vdev->dev,"Enter virtsnd_dc_shmem_unmap_put expid %08x, fd %08x",pint[0], pint[1]);

	export_id = pint[0];
	fd = pint[1];
	if (export_id == 0 || fd < 0){
		dev_err(&vdev->dev, "virtsnd_dc_shmem_unmap_put: invalid input data");
		rc = -EINVAL;
		goto exit;
	}

	msg = virtsnd_ctl_msg_alloc(vdev, sizeof(*hdr),
			sizeof(struct virtio_snd_hdr), GFP_KERNEL);
	if (IS_ERR(msg)){
		rc = -ENOMEM;
		goto exit;
	}

	hdr = sg_virt(&msg->sg_request);
	hdr->hdr.code = cpu_to_virtio32(vdev, VIRTIO_SND_R_SHMEM_UNMAP);
	hdr->control_id = cpu_to_virtio16(vdev, kcontrol->private_value);
	hdr->subcontrol_id = cpu_to_virtio16(vdev, subcid);

	sg_init_one(&sg, ucontrol, sizeof(*ucontrol));
	msg->sg_request_ext = &sg;

	msg->request_ext_size = sizeof(*ucontrol);

	rc = virtsnd_ctl_msg_send_sync(snd, msg);
	if(rc){
		dev_err(&vdev->dev,
			"ShmemUnmap:failed to send message, err %d!", rc);
	}

	vsnd_dc_dma_area_unexport(snd, export_id);
exit:
	return rc;
}

static int virtsnd_dc_tlv_op(struct snd_kcontrol *kcontrol, int op_flag,
			     unsigned int size, unsigned int *utlv)
{
	struct virtio_snd *snd = kcontrol->private_data;
	struct virtio_device *vdev = snd->vdev;
	struct virtio_snd_msg *msg;
	struct virtio_snd_dc_hdr *hdr;
	unsigned int cmd = 0;
	unsigned int *tlv = NULL;
	struct scatterlist sg_request_ext;
	struct scatterlist sg_response_ext;
	int code;

	switch (op_flag) {
	case SNDRV_CTL_TLV_OP_READ: {
		cmd = VIRTIO_SND_R_DC_TLV_READ;
		break;
	}
	case SNDRV_CTL_TLV_OP_WRITE: {
		cmd = VIRTIO_SND_R_DC_TLV_WRITE;
		break;
	}
	case SNDRV_CTL_TLV_OP_CMD: {
		cmd = VIRTIO_SND_R_DC_TLV_COMMAND;
		break;
	}
	default: {
		return -EINVAL;
	}
	}

	msg = virtsnd_ctl_msg_alloc(vdev, sizeof(*hdr),
				    sizeof(struct virtio_snd_hdr), GFP_KERNEL);
	if (IS_ERR(msg))
		return PTR_ERR(msg);

	hdr = sg_virt(&msg->sg_request);
	hdr->hdr.code = cpu_to_virtio32(vdev, cmd);
	hdr->control_id = cpu_to_virtio16(vdev, kcontrol->private_value);

	tlv = devm_kzalloc(&vdev->dev, size, GFP_KERNEL);
	if (!tlv)
		return -ENOMEM;

	if (cmd == VIRTIO_SND_R_DC_TLV_READ) {
		sg_init_one(&sg_response_ext, tlv, size);
		msg->sg_response_ext = &sg_response_ext;
		msg->reply = tlv;
		msg->reply_size = size;
	} else {
		if (copy_from_user(tlv, utlv, size)) {
			code = -EFAULT;
			goto on_failure;
		}

		sg_init_one(&sg_request_ext, tlv, size);
		msg->sg_request_ext = &sg_request_ext;
		msg->request_ext_size = size;
	}

	code = virtsnd_ctl_msg_send_sync(snd, msg);
	if (!code)
		if (cmd == VIRTIO_SND_R_DC_TLV_READ)
			if (copy_to_user(utlv, tlv, size))
				code = -EFAULT;

on_failure:
	devm_kfree(&vdev->dev, tlv);

	return code;
}

static int virtsnd_dc_query_enum_info(struct virtio_snd *snd, unsigned int cid,
				      unsigned int nvalues)
{
	struct virtio_kctl_ctx *ctx = snd->kctl_ctx;
	struct virtio_device *vdev = snd->vdev;
	struct virtio_kctl *kctl = &ctx->kctls[cid];
	struct virtio_snd_msg *msg;
	struct virtio_snd_dc_hdr *hdr;
	struct virtio_snd_dc_enum_value *values;
	struct scatterlist sg_response_ext;
	int code;

	values = devm_kcalloc(&vdev->dev, nvalues, sizeof(*values), GFP_KERNEL);
	if (!values)
		return -ENOMEM;

	msg = virtsnd_ctl_msg_alloc(vdev, sizeof(*hdr),
				    sizeof(struct virtio_snd_hdr), GFP_KERNEL);
	if (IS_ERR(msg)) {
		devm_kfree(&vdev->dev, values);
		return PTR_ERR(msg);
	}

	hdr = sg_virt(&msg->sg_request);
	hdr->hdr.code = cpu_to_virtio32(vdev, VIRTIO_SND_R_DC_ENUM_INFO);
	hdr->control_id = cpu_to_virtio16(vdev, cid);

	sg_init_one(&sg_response_ext, values, nvalues * sizeof(*values));
	msg->sg_response_ext = &sg_response_ext;

	msg->reply = values;
	msg->reply_size = nvalues * sizeof(*values);
	code = virtsnd_ctl_msg_send_sync(snd, msg);
	if (code) {
		dev_warn(&vdev->dev,
			 "Failed to query enumerated information: %d\n",
			 code);
		devm_kfree(&vdev->dev, values);
		return code;
	}

	kctl->enum_values = values;

	return 0;
}

static void virtsnd_dc_work(struct work_struct *work)
{
	struct virtio_snd *snd =
		container_of(work, struct virtio_snd, kctl_work);
	struct virtio_kctl_ctx *ctx = snd->kctl_ctx;
	struct virtio_device *vdev = snd->vdev;
	struct virtio_snd_dc_info *info;
	unsigned int i;
	unsigned int tlv_mask = SNDRV_CTL_ELEM_ACCESS_TLV_READ |
				SNDRV_CTL_ELEM_ACCESS_TLV_WRITE |
				SNDRV_CTL_ELEM_ACCESS_TLV_COMMAND;
	int code;

	int controls_remaining = ctx->nkctls;
	int max_control_request = (HAB_BUFFER_SIZE - sizeof(struct virtio_snd_query_info) - sizeof(struct virtio_snd_hdr)) / sizeof(struct virtio_snd_dc_info);
	info = devm_kcalloc(&vdev->dev, ctx->nkctls, sizeof(*info), GFP_KERNEL);
	if (!info)
		return;

	while (controls_remaining) {
		int num_requested = controls_remaining < max_control_request ? controls_remaining : max_control_request;
		int start_id = ctx->nkctls - controls_remaining;

		code = virtsnd_ctl_query_info(snd, VIRTIO_SND_R_DC_INFO, start_id, num_requested, sizeof(*info),
						((unsigned char*)info + (sizeof(*info) * start_id)));
		if (code) {
			dev_warn(&vdev->dev,
				"Failed to query control element information: %d\n",
				code);
			devm_kfree(&vdev->dev, info);
			return;
		}

		controls_remaining -= num_requested;
	}

	for (i = 0; i < ctx->nkctls; ++i) {
		struct virtio_kctl *kctl = &ctx->kctls[i];
		struct snd_ctl_elem_info *elem_info = &info[i].elem_info;
		struct snd_kcontrol_new kctl_new;

		kctl->info = &info[i];

		if (elem_info->type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
			unsigned int nvalues =
				elem_info->value.enumerated.items;

			code = virtsnd_dc_query_enum_info(snd, i, nvalues);
			if (code)
				continue;
		}

		memset(&kctl_new, 0, sizeof(kctl_new));

		kctl_new.iface = elem_info->id.iface;
		if (kctl_new.iface == SNDRV_CTL_ELEM_IFACE_PCM)
			kctl_new.device = le32_to_cpu(info->hdr.hda_fn_nid);

		kctl_new.name = elem_info->id.name;
		kctl_new.index = elem_info->id.index;

		elem_info->access &= ~(SNDRV_CTL_ELEM_ACCESS_LOCK |
				       SNDRV_CTL_ELEM_ACCESS_OWNER |
				       SNDRV_CTL_ELEM_ACCESS_USER);
		if (elem_info->access & tlv_mask) {
			elem_info->access |= SNDRV_CTL_ELEM_ACCESS_TLV_CALLBACK;
			kctl_new.tlv.c = virtsnd_dc_tlv_op;
		}

		kctl_new.access = elem_info->access;

		kctl_new.info = virtsnd_dc_info;
		kctl_new.get = virtsnd_dc_get;
		if (!strncmp(kctl_new.name, "ShmemMap", strlen("ShmemMap"))){
			kctl_new.put = virtsnd_dc_shmem_map_put;
		} else if (!strncmp(kctl_new.name, "ShmemUnmap", strlen("ShmemUnmap"))){
			kctl_new.put = virtsnd_dc_shmem_unmap_put;
		} else {
			kctl_new.put = virtsnd_dc_put;
		}
		kctl->kctl = snd_ctl_new1(&kctl_new, snd);
		if (!kctl->kctl) {
			dev_warn(&vdev->dev,
				 "Failed to create a control [#%u]\n", i);
			continue;
		}

		kctl->kctl->private_value = i;

		code = snd_ctl_add(snd->card, kctl->kctl);
		if (code)
			dev_warn(&vdev->dev,
				 "Failed to add a control [#%u]: %d\n", i,
				 code);
	}

	atomic_set(&ctx->events_enabled, 1);
	complete(&dc_setup_done);
}

int virtsnd_dc_parse_cfg(struct virtio_snd *snd)
{
	struct virtio_device *vdev = snd->vdev;
	struct virtio_kctl_ctx *ctx;
	unsigned int nkctls;
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
	hdr->code = cpu_to_virtio32(vdev, VIRTIO_SND_R_DC_COUNT);

	sg_init_one(&sg_response_ext, count, sizeof(*count));
	msg->sg_response_ext = &sg_response_ext;
	msg->reply = count;
	msg->reply_size = sizeof(*count);

	code = virtsnd_ctl_msg_send_sync(snd, msg);
	if (code) {
		dev_warn(&vdev->dev, "failed to query dc count: %d\n",
			 code);
		devm_kfree(&vdev->dev, count);
		return code;
	}

        nkctls = *count;
	if (!nkctls)
		return 0;

	ctx = devm_kzalloc(&vdev->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->nkctls = nkctls;
	ctx->kctls = devm_kcalloc(&vdev->dev, nkctls, sizeof(*ctx->kctls),
				  GFP_KERNEL);
	if (!ctx->kctls)
		return -ENOMEM;

	snd->kctl_ctx = ctx;
	INIT_WORK(&snd->kctl_work, virtsnd_dc_work);

	schedule_work(&snd->kctl_work);
	wait_for_completion(&dc_setup_done);

	return 0;
}

void virtsnd_dc_event(struct virtio_snd *snd, struct virtio_snd_event *event)
{
	struct virtio_kctl_ctx *ctx = snd->kctl_ctx;
	struct virtio_snd_dc_event *dce =
		(struct virtio_snd_dc_event *)event;
	struct virtio_kctl *kctl;
	unsigned int cid = le16_to_cpu(dce->control_id);

	if (!atomic_read(&ctx->events_enabled) || cid >= ctx->nkctls)
		return;

	kctl = &ctx->kctls[cid];

	snd_ctl_notify(snd->card, le16_to_cpu(dce->mask), &kctl->kctl->id);
}
