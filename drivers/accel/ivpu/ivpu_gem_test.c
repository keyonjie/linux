// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2022-2023 Intel Corporation
 */

#if IS_ENABLED(CONFIG_DRM_ACCEL_IVPU_KUNIT_TEST)

#include <linux/errno.h>
#include <linux/sizes.h>

#include "ivpu_jsm_msg.h"
#include "ivpu_kunit.h"

struct test_bo_create_case {
	u64 size;
	u32 flags;
};

static const struct test_bo_create_case test_bo_create_cases[] = {
	{ SZ_16K,  DRM_IVPU_BO_CACHED | DRM_IVPU_BO_MAPPABLE },
	{ SZ_16K,  DRM_IVPU_BO_WC | DRM_IVPU_BO_MAPPABLE },
	{ SZ_32K,  DRM_IVPU_BO_CACHED | DRM_IVPU_BO_MAPPABLE },
	{ SZ_32K,  DRM_IVPU_BO_WC | DRM_IVPU_BO_MAPPABLE },
	{ SZ_64K,  DRM_IVPU_BO_CACHED | DRM_IVPU_BO_MAPPABLE },
	{ SZ_64K,  DRM_IVPU_BO_WC | DRM_IVPU_BO_MAPPABLE },
	{ SZ_128K, DRM_IVPU_BO_CACHED | DRM_IVPU_BO_MAPPABLE },
	{ SZ_128K, DRM_IVPU_BO_WC | DRM_IVPU_BO_MAPPABLE },
};

static void test_bo_create_case_desc(const struct test_bo_create_case *t, char *desc)
{
	sprintf(desc, "TEST_BO_ALLOC size: %lld cached: %s",
		t->size, t->flags & DRM_IVPU_BO_CACHED ? "y" : "n");
}

KUNIT_ARRAY_PARAM(test_bo_create, test_bo_create_cases, test_bo_create_case_desc);

static void ivpu_test_bo_create(struct kunit *test)
{
	struct ivpu_bo *bo;
	const struct test_bo_create_case *param = test->param_value;

	bo = ivpu_bo_create_global(ivpu_test_vpu, param->size, param->flags);
	KUNIT_EXPECT_NOT_NULL(test, bo);

	ivpu_bo_free(bo);
}

static int ivpu_bo_pin_broken(__always_unused struct ivpu_bo *bo)
{
	return -EFAULT;
}

static void ivpu_test_bo_create_fault_pin(struct kunit *test)
{
	struct ivpu_bo *bo;

	kunit_activate_static_stub(test, ivpu_bo_pin, ivpu_bo_pin_broken);
	bo = ivpu_bo_create_global(ivpu_test_vpu, SZ_16K, DRM_IVPU_BO_CACHED);
	kunit_deactivate_static_stub(test, ivpu_bo_pin);

	KUNIT_EXPECT_NULL(test, bo);
}

static struct kunit_case ivpu_gem_test_cases[] = {
	KUNIT_CASE_PARAM(ivpu_test_bo_create, test_bo_create_gen_params),
	KUNIT_CASE(ivpu_test_bo_create_fault_pin),
	{}
};

struct kunit_suite ivpu_kunit_gem_test_suite = {
	.name = "intel-vpu-gem-suite",
	.test_cases = ivpu_gem_test_cases,
};

#endif /* IS_ENABLED(CONFIG_DRM_ACCEL_IVPU_KUNIT_TEST) */
