/*
 *  PCM - Rate conversion
 *  Copyright (c) 2000 by Abramo Bagnara <abramo@alsa-project.org>
 *
 *
 *   This library is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Library General Public License as
 *   published by the Free Software Foundation; either version 2 of
 *   the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Library General Public License for more details.
 *
 *   You should have received a copy of the GNU Library General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */
  
#include <limits.h>
#include <byteswap.h>
#include "pcm_local.h"
#include "pcm_plugin.h"
#include "interval.h"

#define DIV (1<<16)

typedef struct {
	int16_t sample;
	int sum;
	unsigned int pos;
} rate_state_t;
 
typedef snd_pcm_uframes_t (*rate_f)(const snd_pcm_channel_area_t *src_areas,
			 snd_pcm_uframes_t src_offset, snd_pcm_uframes_t src_frames,
			 const snd_pcm_channel_area_t *dst_areas,
			 snd_pcm_uframes_t dst_offset, snd_pcm_uframes_t *dst_framesp,
			 unsigned int channels,
			 int getidx, int putidx,
			 unsigned int arg,
			 rate_state_t *states);

typedef struct {
	/* This field need to be the first */
	snd_pcm_plugin_t plug;
	int get_idx;
	int put_idx;
	unsigned int pitch;
	rate_f func;
	int sformat;
	int srate;
	rate_state_t *states;
} snd_pcm_rate_t;

static snd_pcm_uframes_t resample_expand(const snd_pcm_channel_area_t *src_areas,
			      snd_pcm_uframes_t src_offset, snd_pcm_uframes_t src_frames,
			      const snd_pcm_channel_area_t *dst_areas,
			      snd_pcm_uframes_t dst_offset, snd_pcm_uframes_t *dst_framesp,
			      unsigned int channels,
			      int getidx, int putidx,
			      unsigned int get_threshold,
			      rate_state_t *states)
{
#define GET16_LABELS
#define PUT16_LABELS
#include "plugin_ops.h"
#undef GET16_LABELS
#undef PUT16_LABELS
	void *get = get16_labels[getidx];
	void *put = put16_labels[putidx];
	unsigned int channel;
	snd_pcm_uframes_t src_frames1 = 0;
	snd_pcm_uframes_t dst_frames1 = 0;
	snd_pcm_uframes_t dst_frames = *dst_framesp;
	int16_t sample = 0;
	
	if (src_frames == 0 ||
	    dst_frames == 0)
		return 0;
	for (channel = 0; channel < channels; ++channel) {
		const snd_pcm_channel_area_t *src_area = &src_areas[channel];
		const snd_pcm_channel_area_t *dst_area = &dst_areas[channel];
		char *src, *dst;
		int src_step, dst_step;
		int16_t old_sample = states->sample;
		unsigned int pos = states->pos;
#if 0
		if (!src_area->enabled) {
			if (dst_area->wanted)
				snd_pcm_area_silence(&dst_area->area, 0, dst_frames, plugin->dst_format);
			dst_area->enabled = 0;
			continue;
		}
		dst_area->enabled = 1;
#endif
		src = snd_pcm_channel_area_addr(src_area, src_offset);
		dst = snd_pcm_channel_area_addr(dst_area, dst_offset);
		src_step = snd_pcm_channel_area_step(src_area);
		dst_step = snd_pcm_channel_area_step(dst_area);
		src_frames1 = 0;
		dst_frames1 = 0;
		while (dst_frames1 < dst_frames) {
			if (pos >= get_threshold) {
				int16_t new_sample;
				if (src_frames1 == src_frames)
					break;
				pos -= get_threshold;
				goto *get;
#define GET16_END after_get
#include "plugin_ops.h"
#undef GET16_END
			after_get:
				src += src_step;
				src_frames1++;
				new_sample = sample;
				sample = (old_sample * (DIV - pos) + new_sample * pos) / DIV;
				old_sample = new_sample;
			} else
				sample = old_sample;
			goto *put;
#define PUT16_END after_put
#include "plugin_ops.h"
#undef PUT16_END
		after_put:
			dst += dst_step;
			dst_frames1++;
			pos += DIV;
		}
		states->sample = old_sample;
		states->pos = pos;
		states++;
	}
	*dst_framesp = dst_frames1;
	return src_frames1;
}

static snd_pcm_uframes_t resample_shrink(const snd_pcm_channel_area_t *src_areas,
			      snd_pcm_uframes_t src_offset, snd_pcm_uframes_t src_frames,
			      const snd_pcm_channel_area_t *dst_areas,
			      snd_pcm_uframes_t dst_offset, snd_pcm_uframes_t *dst_framesp,
			      unsigned int channels,
			      int getidx, int putidx,
			      unsigned int get_increment,
			      rate_state_t *states)
{
#define GET16_LABELS
#define PUT16_LABELS
#include "plugin_ops.h"
#undef GET16_LABELS
#undef PUT16_LABELS
	void *get = get16_labels[getidx];
	void *put = put16_labels[putidx];
	unsigned int channel;
	snd_pcm_uframes_t src_frames1 = 0;
	snd_pcm_uframes_t dst_frames1 = 0;
	snd_pcm_uframes_t dst_frames = *dst_framesp;
	int16_t sample = 0;

	if (src_frames == 0 ||
	    dst_frames == 0)
		return 0;
	for (channel = 0; channel < channels; ++channel) {
		const snd_pcm_channel_area_t *src_area = &src_areas[channel];
		const snd_pcm_channel_area_t *dst_area = &dst_areas[channel];
		unsigned int pos;
		int sum;
		char *src, *dst;
		int src_step, dst_step;
		sum = states->sum;
		pos = states->pos;
#if 0
		if (!src_area->enabled) {
			if (dst_area->wanted)
				snd_pcm_area_silence(&dst_area->area, 0, dst_frames, plugin->dst_format);
			dst_area->enabled = 0;
			continue;
		}
		dst_area->enabled = 1;
#endif
		src = snd_pcm_channel_area_addr(src_area, src_offset);
		dst = snd_pcm_channel_area_addr(dst_area, dst_offset);
		src_step = snd_pcm_channel_area_step(src_area);
		dst_step = snd_pcm_channel_area_step(dst_area);
		src_frames1 = 0;
		dst_frames1 = 0;
		while (src_frames1 < src_frames) {
			
			goto *get;
#define GET16_END after_get
#include "plugin_ops.h"
#undef GET16_END
		after_get:
			src += src_step;
			src_frames1++;
			pos += get_increment;
			if (pos >= DIV) {
				int s = sample;
				pos -= DIV;
				sum += s * (get_increment - pos);
				sum /= DIV;
				sample = sum;
				goto *put;
#define PUT16_END after_put
#include "plugin_ops.h"
#undef PUT16_END
			after_put:
				dst += dst_step;
				sum = s * pos;
				dst_frames1++;
				if (dst_frames1 == dst_frames)
					break;
			} else
				sum += sample * get_increment;
		}
		states->sum = sum;
		states->pos = pos;
		states++;
	}
	*dst_framesp = dst_frames1;
	return src_frames1;
}

static int snd_pcm_rate_hw_refine_cprepare(snd_pcm_t *pcm ATTRIBUTE_UNUSED, snd_pcm_hw_params_t *params)
{
	int err;
	mask_t *access_mask = alloca(mask_sizeof());
	mask_t *format_mask = alloca(mask_sizeof());
	mask_load(access_mask, SND_PCM_ACCBIT_PLUGIN);
	mask_load(format_mask, SND_PCM_FMTBIT_LINEAR);
	err = _snd_pcm_hw_param_mask(params, SND_PCM_HW_PARAM_ACCESS,
				     access_mask);
	if (err < 0)
		return err;
	err = _snd_pcm_hw_param_mask(params, SND_PCM_HW_PARAM_FORMAT,
				     format_mask);
	if (err < 0)
		return err;
	err = _snd_pcm_hw_param_set(params, SND_PCM_HW_PARAM_SUBFORMAT,
				     SND_PCM_SUBFORMAT_STD, 0);
	if (err < 0)
		return err;
	err = _snd_pcm_hw_param_min(params,
				     SND_PCM_HW_PARAM_RATE, RATE_MIN, 0);
	if (err < 0)
		return err;
	err = _snd_pcm_hw_param_max(params,
				     SND_PCM_HW_PARAM_RATE, RATE_MAX, 0);
	if (err < 0)
		return err;
	params->info &= ~(SND_PCM_INFO_MMAP | SND_PCM_INFO_MMAP_VALID);
	return 0;
}

static int snd_pcm_rate_hw_refine_sprepare(snd_pcm_t *pcm, snd_pcm_hw_params_t *sparams)
{
	snd_pcm_rate_t *rate = pcm->private;
	mask_t *saccess_mask = alloca(mask_sizeof());
	mask_load(saccess_mask, SND_PCM_ACCBIT_MMAP);
	_snd_pcm_hw_params_any(sparams);
	_snd_pcm_hw_param_mask(sparams, SND_PCM_HW_PARAM_ACCESS,
				saccess_mask);
	if (rate->sformat >= 0) {
		_snd_pcm_hw_param_set(sparams, SND_PCM_HW_PARAM_FORMAT,
				      rate->sformat, 0);
		_snd_pcm_hw_param_set(sparams, SND_PCM_HW_PARAM_SUBFORMAT,
				      SND_PCM_SUBFORMAT_STD, 0);
	}
	_snd_pcm_hw_param_minmax(sparams, SND_PCM_HW_PARAM_RATE,
				 rate->srate, 0, rate->srate + 1, -1);
	return 0;
}

static int snd_pcm_rate_hw_refine_schange(snd_pcm_t *pcm, snd_pcm_hw_params_t *params,
					  snd_pcm_hw_params_t *sparams)
{
	snd_pcm_rate_t *rate = pcm->private;
	interval_t t, buffer_size;
	const interval_t *srate, *crate;
	int err;
	unsigned int links = (SND_PCM_HW_PARBIT_CHANNELS |
			      SND_PCM_HW_PARBIT_PERIOD_TIME |
			      SND_PCM_HW_PARBIT_TICK_TIME);
	if (rate->sformat < 0)
		links |= (SND_PCM_HW_PARBIT_FORMAT |
			  SND_PCM_HW_PARBIT_SUBFORMAT |
			  SND_PCM_HW_PARBIT_SAMPLE_BITS |
			  SND_PCM_HW_PARBIT_FRAME_BITS);
	interval_copy(&buffer_size, snd_pcm_hw_param_value_interval(params, SND_PCM_HW_PARAM_BUFFER_SIZE));
	interval_unfloor(&buffer_size);
	crate = snd_pcm_hw_param_value_interval(params, SND_PCM_HW_PARAM_RATE);
	srate = snd_pcm_hw_param_value_interval(sparams, SND_PCM_HW_PARAM_RATE);
	interval_muldiv(&buffer_size, srate, crate, &t);
	err = _snd_pcm_hw_param_refine_interval(sparams, SND_PCM_HW_PARAM_BUFFER_SIZE, &t);
	if (err < 0)
		return err;
	err = _snd_pcm_hw_params_refine(sparams, links, params);
	if (err < 0)
		return err;
	return 0;
}
	
static int snd_pcm_rate_hw_refine_cchange(snd_pcm_t *pcm, snd_pcm_hw_params_t *params,
					  snd_pcm_hw_params_t *sparams)
{
	snd_pcm_rate_t *rate = pcm->private;
	interval_t t;
	const interval_t *sbuffer_size;
	const interval_t *srate, *crate;
	int err;
	unsigned int links = (SND_PCM_HW_PARBIT_CHANNELS |
			      SND_PCM_HW_PARBIT_PERIOD_TIME |
			      SND_PCM_HW_PARBIT_TICK_TIME);
	if (rate->sformat < 0)
		links |= (SND_PCM_HW_PARBIT_FORMAT |
			  SND_PCM_HW_PARBIT_SUBFORMAT |
			  SND_PCM_HW_PARBIT_SAMPLE_BITS |
			  SND_PCM_HW_PARBIT_FRAME_BITS);
	sbuffer_size = snd_pcm_hw_param_value_interval(sparams, SND_PCM_HW_PARAM_BUFFER_SIZE);
	crate = snd_pcm_hw_param_value_interval(params, SND_PCM_HW_PARAM_RATE);
	srate = snd_pcm_hw_param_value_interval(sparams, SND_PCM_HW_PARAM_RATE);
	interval_muldiv(sbuffer_size, crate, srate, &t);
	interval_floor(&t);
	err = _snd_pcm_hw_param_refine_interval(params, SND_PCM_HW_PARAM_BUFFER_SIZE, &t);
	if (err < 0)
		return err;
	err = _snd_pcm_hw_params_refine(params, links, sparams);
	if (err < 0)
		return err;
	return 0;
}

static int snd_pcm_rate_hw_refine(snd_pcm_t *pcm, 
				  snd_pcm_hw_params_t *params)
{
	return snd_pcm_hw_refine_slave(pcm, params,
				       snd_pcm_rate_hw_refine_cprepare,
				       snd_pcm_rate_hw_refine_cchange,
				       snd_pcm_rate_hw_refine_sprepare,
				       snd_pcm_rate_hw_refine_schange,
				       snd_pcm_plugin_hw_refine_slave);
}

static int snd_pcm_rate_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t * params)
{
	snd_pcm_rate_t *rate = pcm->private;
	snd_pcm_t *slave = rate->plug.slave;
	unsigned int src_format, dst_format;
	unsigned int src_rate, dst_rate;
	int err = snd_pcm_hw_params_slave(pcm, params,
					  snd_pcm_rate_hw_refine_cchange,
					  snd_pcm_rate_hw_refine_sprepare,
					  snd_pcm_rate_hw_refine_schange,
					  snd_pcm_plugin_hw_params_slave);
	if (err < 0)
		return err;

	if (pcm->stream == SND_PCM_STREAM_PLAYBACK) {
		src_format = snd_pcm_hw_param_value(params, SND_PCM_HW_PARAM_FORMAT, 0);
		dst_format = slave->format;
		src_rate = snd_pcm_hw_param_value(params, SND_PCM_HW_PARAM_RATE, 0);
		dst_rate = slave->rate;
	} else {
		src_format = slave->format;
		dst_format = snd_pcm_hw_param_value(params, SND_PCM_HW_PARAM_FORMAT, 0);
		src_rate = slave->rate;
		dst_rate = snd_pcm_hw_param_value(params, SND_PCM_HW_PARAM_RATE, 0);
	}
	rate->get_idx = get_index(src_format, SND_PCM_FORMAT_S16);
	rate->put_idx = put_index(SND_PCM_FORMAT_S16, dst_format);
	if (src_rate < dst_rate) {
		rate->func = resample_expand;
		/* pitch is get_threshold */
	} else {
		rate->func = resample_shrink;
		/* pitch is get_increment */
	}
	rate->pitch = (((u_int64_t)dst_rate * DIV) + src_rate / 2) / src_rate;
	assert(!rate->states);
	rate->states = malloc(snd_pcm_hw_param_value(params, SND_PCM_HW_PARAM_CHANNELS, 0) * sizeof(*rate->states));
	return 0;
}

static int snd_pcm_rate_hw_free(snd_pcm_t *pcm)
{
	snd_pcm_rate_t *rate = pcm->private;
	if (rate->states) {
		free(rate->states);
		rate->states = 0;
	}
	return snd_pcm_hw_free(rate->plug.slave);
}

static int snd_pcm_rate_sw_params(snd_pcm_t *pcm, snd_pcm_sw_params_t * params)
{
	snd_pcm_rate_t *rate = pcm->private;
	snd_pcm_t *slave = rate->plug.slave;
	snd_pcm_sw_params_t sparams;
	sparams = *params;
	sparams.avail_min = muldiv_near(sparams.avail_min, slave->rate, pcm->rate);
	sparams.xfer_align = muldiv_near(sparams.xfer_align, slave->rate, pcm->rate);
	sparams.silence_threshold = muldiv_near(sparams.silence_threshold, slave->rate, pcm->rate);
	sparams.silence_size = muldiv_near(sparams.silence_size, slave->rate, pcm->rate);
	return snd_pcm_sw_params(slave, &sparams);
}

static int snd_pcm_rate_init(snd_pcm_t *pcm)
{
	snd_pcm_rate_t *rate = pcm->private;
	unsigned int k;
	for (k = 0; k < pcm->channels; ++k) {
		rate->states[k].sum = 0;
		rate->states[k].sample = 0;
		if (rate->func == resample_expand) {
			/* Get a sample on entry */
			rate->states[k].pos = rate->pitch + DIV;
		} else {
			rate->states[k].pos = 0;
		}
	}
	return 0;
}

static snd_pcm_sframes_t snd_pcm_rate_write_areas(snd_pcm_t *pcm,
					const snd_pcm_channel_area_t *areas,
					snd_pcm_uframes_t client_offset,
					snd_pcm_uframes_t client_size,
					snd_pcm_uframes_t *slave_sizep)
{
	snd_pcm_rate_t *rate = pcm->private;
	snd_pcm_t *slave = rate->plug.slave;
	snd_pcm_uframes_t client_xfer = 0;
	snd_pcm_uframes_t slave_xfer = 0;
	snd_pcm_sframes_t err = 0;
	snd_pcm_uframes_t slave_size;
	if (slave_sizep)
		slave_size = *slave_sizep;
	else
		slave_size = INT_MAX;
	assert(client_size > 0 && slave_size > 0);
	while (client_xfer < client_size &&
	       slave_xfer < slave_size) {
		snd_pcm_uframes_t src_frames, dst_frames;
		src_frames = client_size - client_xfer;
		dst_frames = snd_pcm_mmap_playback_xfer(slave, slave_size - slave_xfer);
		src_frames = rate->func(areas, client_offset, src_frames,
					snd_pcm_mmap_areas(slave), snd_pcm_mmap_offset(slave),
					&dst_frames, 
					pcm->channels,
					rate->get_idx, rate->put_idx,
					rate->pitch, rate->states);
		assert(src_frames || dst_frames);
		if (dst_frames > 0) {
			err = snd_pcm_mmap_forward(slave, dst_frames);
			if (err < 0)
				break;
			assert((snd_pcm_uframes_t)err == dst_frames);
			slave_xfer += dst_frames;
		}

		if (src_frames > 0) {
			client_offset += src_frames;
			client_xfer += src_frames;
			snd_pcm_mmap_hw_forward(pcm, src_frames);
		}
	}
	if (client_xfer > 0 || slave_xfer > 0) {
		if (slave_sizep)
			*slave_sizep = slave_xfer;
		return client_xfer;
	}
	return err;
}

static snd_pcm_sframes_t snd_pcm_rate_read_areas(snd_pcm_t *pcm,
				       const snd_pcm_channel_area_t *areas,
				       snd_pcm_uframes_t client_offset,
				       snd_pcm_uframes_t client_size,
				       snd_pcm_uframes_t *slave_sizep)

{
	snd_pcm_rate_t *rate = pcm->private;
	snd_pcm_t *slave = rate->plug.slave;
	snd_pcm_uframes_t client_xfer = 0;
	snd_pcm_uframes_t slave_xfer = 0;
	snd_pcm_sframes_t err = 0;
	snd_pcm_uframes_t slave_size;
	if (slave_sizep)
		slave_size = *slave_sizep;
	else
		slave_size = INT_MAX;
	assert(client_size > 0 && slave_size > 0);
	while (client_xfer < client_size &&
	       slave_xfer < slave_size) {
		snd_pcm_uframes_t src_frames, dst_frames;
		dst_frames = client_size - client_xfer;
		src_frames = snd_pcm_mmap_capture_xfer(slave, slave_size - slave_xfer);
		src_frames = rate->func(snd_pcm_mmap_areas(slave), snd_pcm_mmap_offset(slave),
					src_frames,
					areas, client_offset, &dst_frames,
					pcm->channels,
					rate->get_idx, rate->put_idx,
					rate->pitch, rate->states);
		assert(src_frames || dst_frames);
		if (src_frames > 0) {
			err = snd_pcm_mmap_forward(slave, src_frames);
			if (err < 0)
				break;
			assert((snd_pcm_uframes_t)err == src_frames);
			slave_xfer += src_frames;
		}
		if (dst_frames > 0) {
			client_offset += dst_frames;
			client_xfer += dst_frames;
			snd_pcm_mmap_hw_forward(pcm, dst_frames);
		}
	}
	if (client_xfer > 0 || slave_xfer > 0) {
		if (slave_sizep)
			*slave_sizep = slave_xfer;
		return client_xfer;
	}
	return err;
}

snd_pcm_sframes_t snd_pcm_rate_client_frames(snd_pcm_t *pcm, snd_pcm_sframes_t frames)
{
	snd_pcm_rate_t *rate = pcm->private;
	/* Round toward zero */
	if (pcm->stream == SND_PCM_STREAM_PLAYBACK)
		return muldiv_down(frames, DIV, rate->pitch);
	else
		return muldiv_down(frames, rate->pitch, DIV);
}

snd_pcm_sframes_t snd_pcm_rate_slave_frames(snd_pcm_t *pcm, snd_pcm_sframes_t frames)
{
	snd_pcm_rate_t *rate = pcm->private;
	/* Round toward zero */
	if (pcm->stream == SND_PCM_STREAM_PLAYBACK)
		return muldiv_down(frames, rate->pitch, DIV);
	else
		return muldiv_down(frames, DIV, rate->pitch);
}

static void snd_pcm_rate_dump(snd_pcm_t *pcm, snd_output_t *out)
{
	snd_pcm_rate_t *rate = pcm->private;
	if (rate->sformat < 0)
		snd_output_printf(out, "Rate conversion PCM (%d)\n", 
			rate->srate);
	else
		snd_output_printf(out, "Rate conversion PCM (%d, sformat=%s)\n", 
			rate->srate,
			snd_pcm_format_name(rate->sformat));
	if (pcm->setup) {
		snd_output_printf(out, "Its setup is:\n");
		snd_pcm_dump_setup(pcm, out);
	}
	snd_output_printf(out, "Slave: ");
	snd_pcm_dump(rate->plug.slave, out);
}

snd_pcm_ops_t snd_pcm_rate_ops = {
	close: snd_pcm_plugin_close,
	card: snd_pcm_plugin_card,
	info: snd_pcm_plugin_info,
	hw_refine: snd_pcm_rate_hw_refine,
	hw_params: snd_pcm_rate_hw_params,
	hw_free: snd_pcm_rate_hw_free,
	sw_params: snd_pcm_rate_sw_params,
	channel_info: snd_pcm_plugin_channel_info,
	dump: snd_pcm_rate_dump,
	nonblock: snd_pcm_plugin_nonblock,
	async: snd_pcm_plugin_async,
	mmap: snd_pcm_plugin_mmap,
	munmap: snd_pcm_plugin_munmap,
};

int snd_pcm_rate_open(snd_pcm_t **pcmp, char *name, int sformat, int srate, snd_pcm_t *slave, int close_slave)
{
	snd_pcm_t *pcm;
	snd_pcm_rate_t *rate;
	assert(pcmp && slave);
	if (sformat >= 0 && snd_pcm_format_linear(sformat) != 1)
		return -EINVAL;
	rate = calloc(1, sizeof(snd_pcm_rate_t));
	if (!rate) {
		return -ENOMEM;
	}
	rate->srate = srate;
	rate->sformat = sformat;
	rate->plug.read = snd_pcm_rate_read_areas;
	rate->plug.write = snd_pcm_rate_write_areas;
	rate->plug.client_frames = snd_pcm_rate_client_frames;
	rate->plug.slave_frames = snd_pcm_rate_slave_frames;
	rate->plug.init = snd_pcm_rate_init;
	rate->plug.slave = slave;
	rate->plug.close_slave = close_slave;

	pcm = calloc(1, sizeof(snd_pcm_t));
	if (!pcm) {
		free(rate);
		return -ENOMEM;
	}
	if (name)
		pcm->name = strdup(name);
	pcm->type = SND_PCM_TYPE_RATE;
	pcm->stream = slave->stream;
	pcm->mode = slave->mode;
	pcm->ops = &snd_pcm_rate_ops;
	pcm->op_arg = pcm;
	pcm->fast_ops = &snd_pcm_plugin_fast_ops;
	pcm->fast_op_arg = pcm;
	pcm->private = rate;
	pcm->poll_fd = slave->poll_fd;
	pcm->hw_ptr = &rate->plug.hw_ptr;
	pcm->appl_ptr = &rate->plug.appl_ptr;
	*pcmp = pcm;

	return 0;
}

int _snd_pcm_rate_open(snd_pcm_t **pcmp, char *name,
			 snd_config_t *conf, 
			 int stream, int mode)
{
	snd_config_iterator_t i;
	char *sname = NULL;
	int err;
	snd_pcm_t *spcm;
	int sformat = -1;
	long srate = -1;
	snd_config_foreach(i, conf) {
		snd_config_t *n = snd_config_entry(i);
		if (strcmp(n->id, "comment") == 0)
			continue;
		if (strcmp(n->id, "type") == 0)
			continue;
		if (strcmp(n->id, "stream") == 0)
			continue;
		if (strcmp(n->id, "sname") == 0) { 
			err = snd_config_string_get(n, &sname);
			if (err < 0) {
				ERR("Invalid type for %s", n->id);
				return -EINVAL;
			}
			continue;
		}
		if (strcmp(n->id, "sformat") == 0) {
			char *f;
			err = snd_config_string_get(n, &f);
			if (err < 0) {
				ERR("Invalid type for %s", n->id);
				return -EINVAL;
			}
			sformat = snd_pcm_format_value(f);
			if (sformat < 0) {
				ERR("Unknown sformat");
				return -EINVAL;
			}
			if (snd_pcm_format_linear(sformat) != 1) {
				ERR("sformat is not linear");
				return -EINVAL;
			}
			continue;
		}
		if (strcmp(n->id, "srate") == 0) {
			err = snd_config_integer_get(n, &srate);
			if (err < 0) {
				ERR("Invalid type for %s", n->id);
				return -EINVAL;
			}
			continue;
		}
		ERR("Unknown field %s", n->id);
		return -EINVAL;
	}
	if (!sname) {
		ERR("sname is not defined");
		return -EINVAL;
	}
	if (srate < 0) {
		ERR("srate is not defined");
		return -EINVAL;
	}
	/* This is needed cause snd_config_update may destroy config */
	sname = strdup(sname);
	if (!sname)
		return  -ENOMEM;
	err = snd_pcm_open(&spcm, sname, stream, mode);
	free(sname);
	if (err < 0)
		return err;
	err = snd_pcm_rate_open(pcmp, name, sformat, srate, spcm, 1);
	if (err < 0)
		snd_pcm_close(spcm);
	return err;
}
				

