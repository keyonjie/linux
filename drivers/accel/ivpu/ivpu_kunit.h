/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2022-2023 Intel Corporation
 */

#ifndef __IVPU_KUNIT_H__
#define __IVPU_KUNIT_H__

#include "ivpu_drv.h"

#if IS_ENABLED(CONFIG_DRM_ACCEL_IVPU_KUNIT_TEST)

#include <kunit/static_stub.h>
#include <kunit/test.h>

#define IVPU_STUB_REDIRECT(real_fn_name, args...) \
	KUNIT_STATIC_STUB_REDIRECT(real_fn_name, args)

extern struct ivpu_device *ivpu_test_vpu;
extern struct kunit_suite ivpu_kunit_gem_test_suite;

int ivpu_kunit_run_suites(struct ivpu_device *vdev);
void ivpu_kunit_cleanup_suites(void);

#else

#define IVPU_STUB_REDIRECT(real_fn_name, args...) do {} while (0)

static inline int ivpu_kunit_run_suites(__always_unused struct ivpu_device *vdev)
{
	return 0;
}

static inline void ivpu_kunit_cleanup_suites(void)
{
}

#endif /* IS_ENABLED(CONFIG_DRM_ACCEL_IVPU_KUNIT_TEST) */

#endif /* __IVPU_KUNIT_H__ */
