// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Copyright (C) 2020-2023 Intel Corporation
 */

#include <drm/drm_file.h>

#include "ivpu_drv.h"
#include "ivpu_jsm_msg.h"
#include "ivpu_gem.h"
#include "ivpu_profiling.h"
#include "ivpu_pm.h"

#define IVPU_METRIC_STREAMER_INFO_BUFFER_SIZE	SZ_16K

static struct ivpu_metric_streamer_data *
get_metric_streamer_data_by_mask(struct ivpu_file_priv *file, u64 metric_mask)
{
	struct list_head *head = &file->ms_data_list;
	struct ivpu_metric_streamer_data *ms;

	list_for_each_entry(ms, head, ms_data_node) {
		if (ms->mask == metric_mask)
			return ms;
	}

	return NULL;
}

int ivpu_metric_streamer_start_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct ivpu_file_priv *file_priv = file->driver_priv;
	struct ivpu_metric_streamer_data *ms_data;
	struct drm_ivpu_metric_streamer_start *args = data;
	struct ivpu_device *vdev = file_priv->vdev;
	struct ivpu_bo *buffer;
	u64 buffer_size, mask;
	u32 sample_size;
	int ret;

	mask = args->metric_group_mask;
	mutex_lock(&file_priv->lock);
	if (get_metric_streamer_data_by_mask(file_priv, mask)) {
		ret = -EINVAL;
		ivpu_err(vdev, "Metric streamer exists for mask: %#llx\n", mask);
		goto unlock;
	}

	ms_data = kzalloc(sizeof(*ms_data), GFP_KERNEL);
	if (!ms_data) {
		ret = -ENOMEM;
		ivpu_err(vdev, "Failed to allocate metric_streamer_data\n");
		goto unlock;
	}

	ret = ivpu_jsm_metric_streamer_info(vdev, mask, 0, 0, &sample_size, NULL);
	if (ret)
		goto err_free_ms_data;

	buffer_size = PAGE_ALIGN(2 * sample_size * args->read_rate);
	buffer = ivpu_bo_create_global(vdev, buffer_size * 2,
				       DRM_IVPU_BO_CACHED | DRM_IVPU_BO_MAPPABLE);
	if (!buffer) {
		ivpu_err(vdev, "Failed to allocate metric streamer buffer\n");
		ret = -ENOMEM;
		goto err_free_ms_data;
	}

	ms_data->buffer_size = buffer_size;
	ret = ivpu_jsm_metric_streamer_start(vdev, mask, args->sampling_rate_ns, buffer->vpu_addr,
					     buffer_size, &sample_size);
	if (ret)
		goto err_free_buffer;

	ms_data->active_buff_off = 0;
	ms_data->bo = buffer;
	ms_data->mask = mask;
	args->sample_size = sample_size;
	list_add_tail(&ms_data->ms_data_node, &file_priv->ms_data_list);
	goto unlock;

err_free_buffer:
	ivpu_bo_free(buffer);
err_free_ms_data:
	kfree(ms_data);
unlock:
	mutex_unlock(&file_priv->lock);
	return ret;
}

static int
ms_get_data_update_buffer(struct ivpu_device *vdev, struct ivpu_metric_streamer_data *ms_data,
			  struct drm_ivpu_metric_streamer_get_data *args)
{
	u64 bytes_written, copy_bytes, user_size, inactive_buff_off;
	void __user *user_buffer = (void __user *)args->buffer_ptr;
	struct ivpu_bo *buffer = ms_data->bo;
	void *inactive_buffer;
	int ret;

	if (!args->buffer_ptr)
		return -EINVAL;

	user_size = args->size;
	copy_bytes = 0;

	if (ms_data->leftover_bytes) {
		copy_bytes = min(user_size, ms_data->leftover_bytes);
		ret = copy_to_user(user_buffer, ms_data->leftover_addr, copy_bytes);
		if (ret)
			return -EFAULT;
		ms_data->leftover_bytes -= copy_bytes;
		ms_data->leftover_addr += copy_bytes;
		if (copy_bytes == user_size)
			return ret;
		user_buffer += copy_bytes;
	}

	inactive_buff_off = ms_data->buffer_size - ms_data->active_buff_off;
	ret = ivpu_jsm_metric_streamer_update(vdev, args->metric_group_mask,
					      buffer->vpu_addr + inactive_buff_off,
					      ms_data->buffer_size, &bytes_written);
	if (ret)
		return ret;
	inactive_buffer = ivpu_bo_vaddr(buffer) + ms_data->active_buff_off;
	ms_data->active_buff_off = inactive_buff_off;

	copy_bytes = min(user_size - copy_bytes, bytes_written);
	ret = copy_to_user(user_buffer, inactive_buffer, copy_bytes);
	if (ret)
		return -EFAULT;
	ms_data->leftover_bytes = bytes_written - copy_bytes;
	ms_data->leftover_addr = inactive_buffer + copy_bytes;
	args->size = (u64 __force)user_buffer + copy_bytes - args->buffer_ptr;

	return 0;
}

int ivpu_metric_streamer_get_data_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_ivpu_metric_streamer_get_data *args = data;
	struct ivpu_file_priv *file_priv = file->driver_priv;
	struct ivpu_metric_streamer_data *ms_data;
	struct ivpu_device *vdev = file_priv->vdev;
	u64 bytes_written;
	int ret;
	u64 mask = args->metric_group_mask;

	mutex_lock(&file_priv->lock);
	ms_data = get_metric_streamer_data_by_mask(file_priv, mask);
	if (!ms_data) {
		ivpu_err(vdev, "Metric streamer does not exist for mask: %#llx\n", mask);
		ret = -EINVAL;
		goto unlock;
	}

	if (!args->size) {
		ret = ivpu_jsm_metric_streamer_update(vdev, args->metric_group_mask, 0, 0,
						      &bytes_written);
		if (ret)
			goto unlock;
		args->size = bytes_written + ms_data->leftover_bytes;
		goto unlock;
	}

	if (!args->buffer_ptr) {
		ret = -EINVAL;
		goto unlock;
	}

	ret = ms_get_data_update_buffer(vdev, ms_data, args);
unlock:
	mutex_unlock(&file_priv->lock);
	return ret;
}

static void
ivpu_metric_streamer_data_del(struct ivpu_device *vdev, struct ivpu_metric_streamer_data *ms_data)
{
	list_del(&ms_data->ms_data_node);
	ivpu_jsm_metric_streamer_stop(vdev, ms_data->mask);
	ivpu_bo_free(ms_data->bo);
	kfree(ms_data);
}

int ivpu_metric_streamer_stop_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct ivpu_file_priv *file_priv = file->driver_priv;
	struct ivpu_device *vdev = file_priv->vdev;
	struct drm_ivpu_metric_streamer_stop *args = data;
	struct ivpu_metric_streamer_data *ms_data;
	u64 mask = args->metric_group_mask;

	mutex_lock(&file_priv->lock);
	ms_data = get_metric_streamer_data_by_mask(file_priv, mask);
	if (!ms_data)
		goto unlock;

	ivpu_metric_streamer_data_del(vdev, ms_data);

unlock:
	mutex_unlock(&file_priv->lock);
	return 0;
}

int ivpu_metric_streamer_get_info_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_ivpu_metric_streamer_get_data *args = data;
	struct ivpu_file_priv *file_priv = file->driver_priv;
	struct ivpu_device *vdev = file_priv->vdev;
	struct ivpu_metric_streamer_info *ms = vdev->ms;
	struct ivpu_bo *buffer;
	u64 buffer_size;
	int ret;

	ret = ivpu_jsm_metric_streamer_info(vdev, args->metric_group_mask, 0, 0, NULL,
					    &buffer_size);
	if (ret)
		return ret;

	if (!args->size) {
		args->size = buffer_size;
		return ret;
	}

	if (!args->buffer_ptr || args->size < buffer_size)
		return -EINVAL;

	mutex_lock(&ms->lock);

	buffer = ms->info_bo;
	memset(ivpu_bo_vaddr(buffer), 0, ivpu_bo_size(buffer));

	ret = ivpu_jsm_metric_streamer_info(vdev, args->metric_group_mask, buffer->vpu_addr,
					    buffer_size, NULL, &buffer_size);
	if (ret)
		goto unlock;

	args->size = buffer_size;

	ret = copy_to_user((void __user *)args->buffer_ptr, ivpu_bo_vaddr(buffer), buffer_size);
	if (ret)
		ret = -EFAULT;

unlock:
	mutex_unlock(&ms->lock);
	return ret;
}

int ivpu_metric_streamer_init(struct ivpu_device *vdev)
{
	struct ivpu_metric_streamer_info *ms = vdev->ms;
	int ret;

	ret = drmm_mutex_init(&vdev->drm, &ms->lock);
	if (ret)
		return ret;

	ms->info_bo = ivpu_bo_create_global(vdev, IVPU_METRIC_STREAMER_INFO_BUFFER_SIZE,
					    DRM_IVPU_BO_CACHED | DRM_IVPU_BO_MAPPABLE);
	if (!ms->info_bo)
		ret = -ENOMEM;

	return ret;
}

void ivpu_metric_streamer_fini(struct ivpu_device *vdev)
{
	struct ivpu_metric_streamer_info *ms = vdev->ms;

	if (ms->info_bo)
		ivpu_bo_free(ms->info_bo);
}

void ivpu_metric_streamer_stop(struct ivpu_file_priv *file_priv)
{
	struct ivpu_device *vdev = file_priv->vdev;
	struct ivpu_metric_streamer_data *ms_data, *m;

	list_for_each_entry_safe(ms_data, m, &file_priv->ms_data_list, ms_data_node) {
		ivpu_metric_streamer_data_del(vdev, ms_data);
	}
}
