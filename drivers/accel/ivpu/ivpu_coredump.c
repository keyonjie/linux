// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020-2023 Intel Corporation
 */

#include <linux/devcoredump.h>

#include "ivpu_coredump.h"
#include "ivpu_fw.h"
#include "ivpu_gem.h"

static ssize_t ivpu_coredump_read(char *buffer, loff_t offset,
				  size_t count, void *data, size_t datalen)
{
	struct ivpu_coredump *dump = data;
	struct ivpu_device *vdev = dump->vdev;
	struct drm_print_iterator pi = {
		.data = buffer,
		.offset = 0,
		.start = offset,
		.remain = count,
	};
	struct drm_printer p = drm_coredump_printer(&pi);

	ivpu_fw_log_print(vdev, dump->fw_log, dump->num_logs, false, &p);

	return count - pi.remain;
}

static void ivpu_coredump_free(void *data)
{
	struct ivpu_coredump *dump = data;

	vfree(dump->fw_log[0].vaddr);
	vfree(dump->fw_log[1].vaddr);

	kfree(dump);
}

void ivpu_dev_coredump(struct ivpu_device *vdev)
{
	const int log_crit_size = ivpu_bo_size(vdev->fw->mem_log_crit);
	const int log_verb_size = ivpu_bo_size(vdev->fw->mem_log_verb);
	struct ivpu_coredump *dump;
	struct ivpu_fw_log *fw_log;

	dump = kzalloc(sizeof(*dump), GFP_KERNEL);
	if (!dump)
		return;

	dump->vdev = vdev;

	fw_log = &dump->fw_log[0];
	fw_log->vaddr = vmalloc(log_crit_size);
	if (fw_log->vaddr) {
		memcpy(fw_log->vaddr, ivpu_bo_vaddr(vdev->fw->mem_log_crit), log_crit_size);
		fw_log->size  = log_crit_size;
		fw_log->name = "VPU critical";
		dump->num_logs++;
	}

	fw_log = &dump->fw_log[dump->num_logs];
	fw_log->vaddr = vmalloc(log_verb_size);
	if (fw_log->vaddr) {
		memcpy(fw_log->vaddr, ivpu_bo_vaddr(vdev->fw->mem_log_verb), log_verb_size);
		fw_log->size = log_verb_size;
		fw_log->name = "VPU verbose";
		dump->num_logs++;
	}

	if (!dump->num_logs) {
		kfree(dump);
		return;
	}

	dev_coredumpm(vdev->drm.dev, THIS_MODULE, dump, 0, GFP_KERNEL,
		      ivpu_coredump_read, ivpu_coredump_free);
}
