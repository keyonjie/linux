/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Copyright (C) 2020-2023 Intel Corporation
 */
#ifndef __IVPU_PROFILING_H__
#define __IVPU_PROFILING_H__

struct ivpu_metric_streamer_data {
	struct ivpu_bo *bo;
	struct list_head ms_data_node;
	u64 mask;
	u64 buffer_size;
	u64 active_buff_off;
	u64 leftover_bytes;
	void *leftover_addr;
};

struct ivpu_metric_streamer_info {
	struct ivpu_bo *info_bo;
	struct mutex lock; /* Protects info_bo */
};

int ivpu_metric_streamer_start_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int ivpu_metric_streamer_stop_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int ivpu_metric_streamer_get_data_ioctl(struct drm_device *dev, void *data, struct drm_file *file);
int ivpu_metric_streamer_get_info_ioctl(struct drm_device *dev, void *data, struct drm_file *file);

int ivpu_metric_streamer_init(struct ivpu_device *vdev);
void ivpu_metric_streamer_fini(struct ivpu_device *vdev);
void ivpu_metric_streamer_stop(struct ivpu_file_priv *file_priv);

#endif /* __IVPU_PROFILING_H__ */
