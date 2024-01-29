// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2022-2023 Intel Corporation
 */
#if IS_ENABLED(CONFIG_DRM_ACCEL_IVPU_KUNIT_TEST)

#include "ivpu_kunit.h"

#include <kunit/test.h>
#include <linux/kernel.h>

struct ivpu_device *ivpu_test_vpu;

static struct kunit_suite *ivpu_kunit_suites[] = {
	&ivpu_kunit_gem_test_suite,
};

int ivpu_kunit_run_suites(struct ivpu_device *vdev)
{
	int ret;

	ivpu_test_vpu = vdev;

	ret = __kunit_test_suites_init(ivpu_kunit_suites, ARRAY_SIZE(ivpu_kunit_suites));
	if (ret < 0) {
		ivpu_err(ivpu_test_vpu, "Failed executing a VPU KUnit test suite: %d\n", ret);
		return ret;
	}
	return 0;
}

void ivpu_kunit_cleanup_suites(void)
{
	__kunit_test_suites_exit(ivpu_kunit_suites, ARRAY_SIZE(ivpu_kunit_suites));
}

#endif /* IS_ENABLED(CONFIG_DRM_ACCEL_IVPU_KUNIT_TEST) */
