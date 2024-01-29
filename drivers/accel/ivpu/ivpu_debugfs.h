/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020-2023 Intel Corporation
 */

#ifndef __IVPU_DEBUGFS_H__
#define __IVPU_DEBUGFS_H__

/* Upstream: remove */
#include <linux/version.h>
#define DRM_DEBUGFS_MOVED_TO_DEV (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 7, 0))

struct ivpu_device;

#if DRM_DEBUGFS_MOVED_TO_DEV

#if defined(CONFIG_DEBUG_FS)
void ivpu_debugfs_init(struct ivpu_device *vdev);
#else
static inline void ivpu_debugfs_init(struct ivpu_device *vdev) { }
#endif

#else

struct drm_minor;

void ivpu_debugfs_init(struct drm_minor *minor);

#endif

#endif /* __IVPU_DEBUGFS_H__ */
