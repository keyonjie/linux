/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2020-2023 Intel Corporation
 */

#ifndef __IVPU_COREDUMP_H__
#define __IVPU_COREDUMP_H__

#include "ivpu_drv.h"
#include "ivpu_fw_log.h"

struct ivpu_coredump {
	struct ivpu_device *vdev;
	int num_logs;
	struct ivpu_fw_log fw_log[2];
};

#ifdef CONFIG_DEV_COREDUMP
void ivpu_dev_coredump(struct ivpu_device *vdev);
#else
static inline void ivpu_dev_coredump(struct ivpu_device *vdev)
{
	ivpu_fw_log_dump(vdev);
}
#endif

#endif /* __IVPU_COREDUMP_H__ */
