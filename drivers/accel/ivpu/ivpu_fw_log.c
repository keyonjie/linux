// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020-2023 Intel Corporation
 */

#include <linux/ctype.h>
#include <linux/highmem.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/moduleparam.h>

#include "vpu_boot_api.h"
#include "ivpu_drv.h"
#include "ivpu_fw.h"
#include "ivpu_fw_log.h"
#include "ivpu_gem.h"
#include "ivpu_jsm_msg.h"

#define IVPU_FW_LOG_LINE_LENGTH	256

unsigned int ivpu_log_level = IVPU_FW_LOG_ERROR;
module_param(ivpu_log_level, uint, 0444);
MODULE_PARM_DESC(ivpu_log_level,
		 "VPU firmware default trace level: debug=" __stringify(IVPU_FW_LOG_DEBUG)
		 " info=" __stringify(IVPU_FW_LOG_INFO)
		 " warn=" __stringify(IVPU_FW_LOG_WARN)
		 " error=" __stringify(IVPU_FW_LOG_ERROR)
		 " fatal=" __stringify(IVPU_FW_LOG_FATAL));

u64 ivpu_hws_log_size;
module_param(ivpu_hws_log_size, ullong, 0444);
MODULE_PARM_DESC(ivpu_hws_log_size, "HWS scheduling log size");

static int fw_log_ptr(struct ivpu_device *vdev, void *vaddr, size_t size, u32 *offset,
		      struct vpu_tracing_buffer_header **log_header)
{
	struct vpu_tracing_buffer_header *log;

	if ((*offset + sizeof(*log)) > size)
		return -EINVAL;

	log = vaddr + *offset;

	if (log->vpu_canary_start != VPU_TRACING_BUFFER_CANARY)
		return -EINVAL;

	if (log->header_size < sizeof(*log) || log->header_size > 1024) {
		ivpu_dbg(vdev, FW_BOOT, "Invalid header size 0x%x\n", log->header_size);
		return -EINVAL;
	}
	if ((char *)log + log->size > (char *)vaddr + size) {
		ivpu_dbg(vdev, FW_BOOT, "Invalid log size 0x%x\n", log->size);
		return -EINVAL;
	}

	*log_header = log;
	*offset += log->size;

	ivpu_dbg(vdev, FW_BOOT,
		 "FW log name \"%s\", write offset 0x%x size 0x%x, wrap count %d, hdr version %d size %d format %d, alignment %d",
		 log->name, log->write_index, log->size, log->wrap_count, log->header_version,
		 log->header_size, log->format, log->alignment);

	return 0;
}

static int fw_log_ptr_bo(struct ivpu_device *vdev, struct ivpu_bo *bo, u32 *offset,
			 struct vpu_tracing_buffer_header **log_header)
{
	void *vaddr = ivpu_bo_vaddr(bo);
	size_t size = ivpu_bo_size(bo);

	return fw_log_ptr(vdev, vaddr, size, offset, log_header);
}

static void buffer_print(char *buffer, u32 size, struct drm_printer *p)
{
	char line[IVPU_FW_LOG_LINE_LENGTH];
	u32 index = 0;

	if (!size || !buffer)
		return;

	while (size--) {
		if (*buffer == '\n' || *buffer == 0) {
			line[index] = 0;
			if (index != 0)
				drm_printf(p, "%s\n", line);
			index = 0;
			buffer++;
			continue;
		}
		if (index == IVPU_FW_LOG_LINE_LENGTH - 1) {
			line[index] = 0;
			index = 0;
			drm_printf(p, "%s\n", line);
		}
		if (*buffer != '\r' && (isprint(*buffer) || iscntrl(*buffer)))
			line[index++] = *buffer;
		buffer++;
	}
	line[index] = 0;
	if (index != 0)
		drm_printf(p, "%s\n", line);
}

static void fw_log_print_buffer(struct vpu_tracing_buffer_header *log, const char *prefix,
				bool only_new_msgs, struct drm_printer *p)
{
	char *log_buffer = (void *)log + log->header_size;
	u32 log_size = log->size - log->header_size;
	u32 log_start = log->read_index;
	u32 log_end = log->write_index;

	if (!(log->write_index || log->wrap_count) ||
	    (log->write_index == log->read_index && only_new_msgs)) {
		drm_printf(p, "==== %s \"%s\" log empty ====\n", prefix, log->name);
		return;
	}

	drm_printf(p, "==== %s \"%s\" log start ====\n", prefix, log->name);
	if (log->write_index > log->read_index) {
		buffer_print(log_buffer + log_start, log_end - log_start, p);
	} else {
		buffer_print(log_buffer + log_end, log_size - log_end, p);
		buffer_print(log_buffer, log_end, p);
	}
	drm_printf(p, "\x1b[0m");
	drm_printf(p, "==== %s \"%s\" log end   ====\n", prefix, log->name);
}

void ivpu_fw_log_print(struct ivpu_device *vdev, struct ivpu_fw_log *logs, int num,
		       bool only_new_msgs, struct drm_printer *p)
{
	struct vpu_tracing_buffer_header *log_header;
	int i;

	for (i = 0; i < num; i++) {
		struct ivpu_fw_log *log = &logs[i];
		u32 next = 0;

		while (fw_log_ptr(vdev, log->vaddr, log->size, &next, &log_header) == 0)
			fw_log_print_buffer(log_header, log->name, only_new_msgs, p);
	}
}

void ivpu_fw_log_direct_print(struct ivpu_device *vdev, bool only_new_msgs, struct drm_printer *p)
{
	struct ivpu_fw_log logs[] = {
		{
		   .vaddr = ivpu_bo_vaddr(vdev->fw->mem_log_crit),
		   .size = ivpu_bo_size(vdev->fw->mem_log_crit),
		   .name = "VPU critical",
		},
		{
		   .vaddr = ivpu_bo_vaddr(vdev->fw->mem_log_verb),
		   .size = ivpu_bo_size(vdev->fw->mem_log_verb),
		   .name = "VPU verbose",
		},
	};

	ivpu_fw_log_print(vdev, logs, ARRAY_SIZE(logs), only_new_msgs, p);
}

void ivpu_fw_log_clear(struct ivpu_device *vdev)
{
	struct vpu_tracing_buffer_header *log_header;
	u32 next;

	next = 0;
	while (fw_log_ptr_bo(vdev, vdev->fw->mem_log_crit, &next, &log_header) == 0)
		log_header->read_index = log_header->write_index;

	next = 0;
	while (fw_log_ptr_bo(vdev, vdev->fw->mem_log_verb, &next, &log_header) == 0)
		log_header->read_index = log_header->write_index;
}

static int ivpu_hws_log_enable(struct ivpu_device *vdev, u32 engine_idx)
{
	struct vpu_hws_log_buffer_header *hdr;
	struct ivpu_bo *buffer;
	int ret;

	buffer = vdev->fw->mem_log_hws[engine_idx];
	hdr = ivpu_bo_vaddr(buffer);
	hdr->num_of_entries = (ivpu_bo_size(buffer) - sizeof(struct vpu_hws_log_buffer_header)) /
			      sizeof(struct vpu_hws_log_buffer_entry);

	ret = ivpu_jsm_hws_set_scheduling_log(vdev, engine_idx, IVPU_GLOBAL_CONTEXT_MMU_SSID,
					      buffer->vpu_addr, 0);

	return ret;
}

int ivpu_hws_log_init(struct ivpu_device *vdev)
{
	int ret, i;

	if (!ivpu_hws_log_size)
		return 0;

	for (i = 0; i < VPU_ENGINE_NB; i++) {
		ret = ivpu_hws_log_enable(vdev, i);
		if (ret)
			return ret;
	}

	return 0;
}

int ivpu_hws_log_mem_init(struct ivpu_device *vdev)
{
	struct ivpu_fw_info *fw = vdev->fw;
	int ret;

	if (!ivpu_hws_log_size)
		return 0;

	fw->mem_log_hws[VPU_ENGINE_COMPUTE] = ivpu_bo_create_global(vdev,
								    PAGE_ALIGN(ivpu_hws_log_size),
								    DRM_IVPU_BO_CACHED |
								    DRM_IVPU_BO_MAPPABLE);
	if (!fw->mem_log_hws[VPU_ENGINE_COMPUTE]) {
		ivpu_err(vdev, "Failed to allocate HWS log buffer for the compute engine\n");
		return -ENOMEM;
	}

	fw->mem_log_hws[VPU_ENGINE_COPY] = ivpu_bo_create_global(vdev,
								 PAGE_ALIGN(ivpu_hws_log_size),
								 DRM_IVPU_BO_CACHED |
								 DRM_IVPU_BO_MAPPABLE);
	if (!fw->mem_log_hws[VPU_ENGINE_COPY]) {
		ivpu_err(vdev, "Failed to allocate HWS log buffer for the copy engine\n");
		ret = -ENOMEM;
		goto err_free_log_compute;
	}

	return 0;

err_free_log_compute:
	ivpu_bo_free(fw->mem_log_hws[VPU_ENGINE_COMPUTE]);
	return ret;
}

void ivpu_hws_log_mem_fini(struct ivpu_device *vdev)
{
	struct ivpu_fw_info *fw = vdev->fw;
	int i;

	for (i = 0; i < VPU_ENGINE_NB; i++) {
		if (!fw->mem_log_hws[i])
			continue;
		ivpu_bo_free(fw->mem_log_hws[i]);
		fw->mem_log_hws[i] = NULL;
	}
}

static void
hws_buffer_print(struct vpu_hws_log_buffer_entry *log, u64 num_of_entries,
		 u64 start_no, struct drm_printer *p)
{
	while (num_of_entries--) {
		drm_printf(p, "%6llu: %#018llx %#010x %#018llx %#018llx\n",
			   start_no++, log->vpu_timestamp, log->operation_type,
			   log->operation_data[0], log->operation_data[1]);
		log++;
	}
}

static void
hws_log_print_buffer(struct ivpu_bo *buffer, const char *prefix, struct drm_printer *p)
{
	struct vpu_hws_log_buffer_header *hdr = ivpu_bo_vaddr(buffer);
	struct vpu_hws_log_buffer_entry *log;
	u64 log_end = hdr->first_free_entry_index;

	if (!(hdr->first_free_entry_index || hdr->wraparound_count)) {
		drm_printf(p, "==== HWS %s log empty ====\n", prefix);
		return;
	}

	log = ivpu_bo_vaddr(buffer) + sizeof(struct vpu_hws_log_buffer_header);

	drm_printf(p, "==== HWS %s log start ====\n", prefix);
	drm_printf(p, "%5s | %17s | %8s | %16s| %s\n", "entry", "timestamp",
		   "op type", "operation data[0]", "operation data[1]");
	if (hdr->wraparound_count) {
		hws_buffer_print(log + log_end, hdr->num_of_entries - log_end,
				 (hdr->wraparound_count - 1) * hdr->num_of_entries + log_end, p);
	}
	hws_buffer_print(log, log_end, hdr->wraparound_count * hdr->num_of_entries, p);
	drm_printf(p, "==== HWS %s log end ====\n", prefix);
}

void ivpu_hws_log_print(struct ivpu_device *vdev, struct drm_printer *p)
{
	struct ivpu_fw_info *fw = vdev->fw;

	if (fw->mem_log_hws[VPU_ENGINE_COMPUTE])
		hws_log_print_buffer(fw->mem_log_hws[VPU_ENGINE_COMPUTE], "Compute Engine", p);
	if (fw->mem_log_hws[VPU_ENGINE_COPY])
		hws_log_print_buffer(fw->mem_log_hws[VPU_ENGINE_COPY], "Copy Engine", p);
}
