#ifndef __SOUND_PCM_REFINE_H
#define __SOUND_PCM_REFINE_H

/*
 *  Digital Audio (PCM) HW/SW params refinement layer
 *  Copyright (c) by Jaroslav Kysela <perex@perex.cz>
 *                   Abramo Bagnara <abramo@alsa-project.org>
 *                   Jie Yang <yang.jie@intel.com>
 *
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */

struct snd_pcm_file {
	struct snd_pcm_substream *substream;
	int no_compat_mmap;
};

struct snd_pcm_hw_rule;
typedef int (*snd_pcm_hw_rule_func_t)(struct snd_pcm_hw_params *params,
				      struct snd_pcm_hw_rule *rule);

struct snd_pcm_hw_rule {
	unsigned int cond;
	int var;
	int deps[4];

	snd_pcm_hw_rule_func_t func;
	void *private;
};

struct snd_pcm_hw_constraints {
	struct snd_mask masks[SNDRV_PCM_HW_PARAM_LAST_MASK -
			 SNDRV_PCM_HW_PARAM_FIRST_MASK + 1];
	struct snd_interval intervals[SNDRV_PCM_HW_PARAM_LAST_INTERVAL -
			     SNDRV_PCM_HW_PARAM_FIRST_INTERVAL + 1];
	unsigned int rules_num;
	unsigned int rules_all;
	struct snd_pcm_hw_rule *rules;
};

struct snd_ratnum {
	unsigned int num;
	unsigned int den_min, den_max, den_step;
};

struct snd_ratden {
	unsigned int num_min, num_max, num_step;
	unsigned int den;
};

struct snd_pcm_hw_constraint_ratnums {
	int nrats;
	struct snd_ratnum *rats;
};

struct snd_pcm_hw_constraint_ratdens {
	int nrats;
	struct snd_ratden *rats;
};

struct snd_pcm_hw_constraint_list {
	const unsigned int *list;
	unsigned int count;
	unsigned int mask;
};

struct snd_pcm_hw_constraint_ranges {
	unsigned int count;
	const struct snd_interval *ranges;
	unsigned int mask;
};

struct snd_pcm_runtime;

#ifdef CONFIG_SND_PARAMS_REFINEMENT
int snd_interval_refine(struct snd_interval *i, const struct snd_interval *v);
void snd_interval_mul(const struct snd_interval *a, const struct snd_interval *b, struct snd_interval *c);
void snd_interval_div(const struct snd_interval *a, const struct snd_interval *b, struct snd_interval *c);
void snd_interval_muldivk(const struct snd_interval *a, const struct snd_interval *b,
			  unsigned int k, struct snd_interval *c);
void snd_interval_mulkdiv(const struct snd_interval *a, unsigned int k,
			  const struct snd_interval *b, struct snd_interval *c);
int snd_interval_list(struct snd_interval *i, unsigned int count,
		      const unsigned int *list, unsigned int mask);
int snd_interval_ranges(struct snd_interval *i, unsigned int count,
			const struct snd_interval *list, unsigned int mask);
int snd_interval_ratnum(struct snd_interval *i,
			unsigned int rats_count, struct snd_ratnum *rats,
			unsigned int *nump, unsigned int *denp);


int snd_pcm_hw_refine(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params);

int snd_pcm_hw_constraints_init(struct snd_pcm_substream *substream);
int snd_pcm_hw_constraints_complete(struct snd_pcm_substream *substream);

int snd_pcm_hw_constraint_mask(struct snd_pcm_runtime *runtime, snd_pcm_hw_param_t var,
			       u_int32_t mask);
int snd_pcm_hw_constraint_mask64(struct snd_pcm_runtime *runtime, snd_pcm_hw_param_t var,
				 u_int64_t mask);
int snd_pcm_hw_constraint_minmax(struct snd_pcm_runtime *runtime, snd_pcm_hw_param_t var,
				 unsigned int min, unsigned int max);
int snd_pcm_hw_constraint_integer(struct snd_pcm_runtime *runtime, snd_pcm_hw_param_t var);
int snd_pcm_hw_constraint_list(struct snd_pcm_runtime *runtime,
			       unsigned int cond,
			       snd_pcm_hw_param_t var,
			       const struct snd_pcm_hw_constraint_list *l);
int snd_pcm_hw_constraint_ranges(struct snd_pcm_runtime *runtime,
				 unsigned int cond,
				 snd_pcm_hw_param_t var,
				 const struct snd_pcm_hw_constraint_ranges *r);
int snd_pcm_hw_constraint_ratnums(struct snd_pcm_runtime *runtime,
				  unsigned int cond,
				  snd_pcm_hw_param_t var,
				  struct snd_pcm_hw_constraint_ratnums *r);
int snd_pcm_hw_constraint_ratdens(struct snd_pcm_runtime *runtime,
				  unsigned int cond,
				  snd_pcm_hw_param_t var,
				  struct snd_pcm_hw_constraint_ratdens *r);
int snd_pcm_hw_constraint_msbits(struct snd_pcm_runtime *runtime,
				 unsigned int cond,
				 unsigned int width,
				 unsigned int msbits);
int snd_pcm_hw_constraint_step(struct snd_pcm_runtime *runtime,
			       unsigned int cond,
			       snd_pcm_hw_param_t var,
			       unsigned long step);
int snd_pcm_hw_constraint_pow2(struct snd_pcm_runtime *runtime,
			       unsigned int cond,
			       snd_pcm_hw_param_t var);
int snd_pcm_hw_rule_noresample(struct snd_pcm_runtime *runtime,
			       unsigned int base_rate);
int snd_pcm_hw_rule_add(struct snd_pcm_runtime *runtime,
			unsigned int cond,
			int var,
			snd_pcm_hw_rule_func_t func, void *private,
			int dep, ...);

#else
static inline int snd_interval_refine(struct snd_interval *i, const struct snd_interval *v)
{
	return 0;
}

static inline void snd_interval_mul(const struct snd_interval *a, const struct snd_interval *b, struct snd_interval *c)
{
}

static inline void snd_interval_div(const struct snd_interval *a, const struct snd_interval *b, struct snd_interval *c)
{
}

static inline void snd_interval_muldivk(const struct snd_interval *a, const struct snd_interval *b,
			  unsigned int k, struct snd_interval *c)
{
}

static inline void snd_interval_mulkdiv(const struct snd_interval *a, unsigned int k,
			  const struct snd_interval *b, struct snd_interval *c)
{
}

static inline int snd_interval_list(struct snd_interval *i, unsigned int count,
			  const unsigned int *list, unsigned int mask)
{
	return 0;
}

static inline int snd_interval_ranges(struct snd_interval *i, unsigned int count,
			const struct snd_interval *list, unsigned int mask)
{
	return 0;
}

static inline int snd_interval_ratnum(struct snd_interval *i,
			unsigned int rats_count, struct snd_ratnum *rats,
			unsigned int *nump, unsigned int *denp)
{
	return 0;
}

static inline int snd_pcm_hw_refine(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params)
{
	return 0;
}

static inline int snd_pcm_hw_constraints_init(struct snd_pcm_substream *substream)
{
	return 0;
}

static inline int snd_pcm_hw_constraints_complete(struct snd_pcm_substream *substream)
{
	return 0;
}

static inline int snd_pcm_hw_constraint_mask(struct snd_pcm_runtime *runtime, snd_pcm_hw_param_t var,
			       u_int32_t mask)
{
	return 0;
}

static inline int snd_pcm_hw_constraint_mask64(struct snd_pcm_runtime *runtime, snd_pcm_hw_param_t var,
				 u_int64_t mask)
{
	return 0;
}

static inline int snd_pcm_hw_constraint_minmax(struct snd_pcm_runtime *runtime, snd_pcm_hw_param_t var,
				 unsigned int min, unsigned int max)
{
	return 0;
}

static inline int snd_pcm_hw_constraint_integer(struct snd_pcm_runtime *runtime, snd_pcm_hw_param_t var)
{
	return 0;
}

static inline int snd_pcm_hw_constraint_list(struct snd_pcm_runtime *runtime,
			       unsigned int cond,
			       snd_pcm_hw_param_t var,
			       const struct snd_pcm_hw_constraint_list *l)
{
	return 0;
}

static inline int snd_pcm_hw_constraint_ranges(struct snd_pcm_runtime *runtime,
				 unsigned int cond,
				 snd_pcm_hw_param_t var,
				 const struct snd_pcm_hw_constraint_ranges *r)
{
	return 0;
}

static inline int snd_pcm_hw_constraint_ratnums(struct snd_pcm_runtime *runtime,
				  unsigned int cond,
				  snd_pcm_hw_param_t var,
				  struct snd_pcm_hw_constraint_ratnums *r)
{
	return 0;
}

static inline int snd_pcm_hw_constraint_ratdens(struct snd_pcm_runtime *runtime,
				  unsigned int cond,
				  snd_pcm_hw_param_t var,
				  struct snd_pcm_hw_constraint_ratdens *r)
{
	return 0;
}

static inline int snd_pcm_hw_constraint_msbits(struct snd_pcm_runtime *runtime,
				 unsigned int cond,
				 unsigned int width,
				 unsigned int msbits)
{
	return 0;
}

static inline int snd_pcm_hw_constraint_step(struct snd_pcm_runtime *runtime,
			       unsigned int cond,
			       snd_pcm_hw_param_t var,
			       unsigned long step)
{
	return 0;
}

static inline int snd_pcm_hw_constraint_pow2(struct snd_pcm_runtime *runtime,
			       unsigned int cond,
			       snd_pcm_hw_param_t var)
{
	return 0;
}

static inline int snd_pcm_hw_rule_noresample(struct snd_pcm_runtime *runtime,
			       unsigned int base_rate)
{
	return 0;
}

static inline int snd_pcm_hw_rule_add(struct snd_pcm_runtime *runtime,
			unsigned int cond,
			int var,
			snd_pcm_hw_rule_func_t func, void *private,
			int dep, ...)
{
	return 0;
}

#endif

#endif /* __SOUND_PCM_REFINE_H */

