# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2022-2023 Intel Corporation

### REMOVE FOR UPSTREAMING
CONFIG_DRM_ACCEL_IVPU := m
VPU_VERSION ?= $(shell date +%Y%m%d)-0
VPU_GIT_REVISION ?= $(shell git -C $(src) rev-parse --short HEAD 2> /dev/null || echo 'out-of-tree')
DRIVER_VERSION_STR ?= ivpu-drv-$(VPU_VERSION) (git revision: $(VPU_GIT_REVISION))

NOSTDINC_FLAGS += -I$(src)/../../../include
subdir-ccflags-y += -DDRIVER_VERSION_STR="\"$(DRIVER_VERSION_STR)\""
subdir-ccflags-y += -DDEBUG
subdir-ccflags-y += -Werror

intel_vpu-y := \
	ivpu_drv.o \
	ivpu_fw.o \
	ivpu_fw_log.o \
	ivpu_gem.o \
	ivpu_hw_37xx.o \
	ivpu_hw_40xx.o \
	ivpu_ipc.o \
	ivpu_job.o \
	ivpu_jsm_msg.o \
	ivpu_mmu.o \
	ivpu_mmu_context.o \
	ivpu_pm.o \
	ivpu_profiling.o \
	ivpu_trace_points.o \
	ivpu_kunit.o

intel_vpu-$(CONFIG_DEBUG_FS) += ivpu_debugfs.o
intel_vpu-$(CONFIG_DEV_COREDUMP) += ivpu_coredump.o

obj-$(CONFIG_DRM_ACCEL_IVPU) += intel_vpu.o

CFLAGS_ivpu_trace_points.o = -I$(src)
