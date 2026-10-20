/* virtualcam.c
 *
 * Copyright 2021 columbarius <co1umbarius@protonmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* Publishes the OBS output as a PipeWire video source with the "Camera"
 * media role, so it shows up as a webcam for PipeWire aware applications
 * (Firefox, Chromium based browsers, GStreamer, ...) without needing the
 * v4l2loopback kernel module.
 *
 * The stream offers every size and format combination as a separate, fixed
 * EnumFormat entry because libwebrtc (used by both Firefox and Chromium)
 * ignores formats that use ranges. OBS hands us frames at the canvas size,
 * and when the consumer picks another size or format we convert them with
 * the libobs video scaler. Frames are written tightly packed in a single
 * data block, which is what libwebrtc expects. */

#include "virtualcam.h"
#include "pipewire.h"

#include <obs-module.h>
#include <media-io/video-io.h>
#include <media-io/video-scaler.h>
#include <util/threading.h>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#define VCAM_NODE_NAME "obs_virtual_camera"
#define VCAM_NODE_DESCRIPTION "OBS Virtual Camera"

#define MAX_SIZES 5
#define PARAMS_BUFFER_SIZE 8192

struct vcam_format {
	uint32_t spa_format;
	enum video_format video_format;
};

/* Formats understood by libwebrtc's PipeWire camera capturer */
static const struct vcam_format vcam_formats[] = {
	{SPA_VIDEO_FORMAT_I420, VIDEO_FORMAT_I420},
	{SPA_VIDEO_FORMAT_NV12, VIDEO_FORMAT_NV12},
	{SPA_VIDEO_FORMAT_YUY2, VIDEO_FORMAT_YUY2},
};

#define N_VCAM_FORMATS (sizeof(vcam_formats) / sizeof(vcam_formats[0]))

/* Downscaled sizes offered in addition to the canvas size */
static const uint32_t vcam_extra_heights[] = {1080, 720, 480, 360};

struct frame_layout {
	uint32_t n_planes;
	uint32_t offset[MAX_AV_PLANES];
	uint32_t linesize[MAX_AV_PLANES];
	uint32_t height[MAX_AV_PLANES];
	uint32_t size;
};

struct pipewire_virtualcam {
	obs_output_t *output;

	/* Protects obs_pw and stream, since raw_video can still be called
	 * for a short while after obs_output_end_data_capture() returns. */
	pthread_mutex_t mutex;
	obs_pipewire *obs_pw;
	struct pw_stream *stream;
	struct spa_hook stream_listener;

	/* Frames as delivered by OBS. Fixed while the output is active. */
	enum video_format src_format;
	uint32_t src_width;
	uint32_t src_height;
	enum video_colorspace colorspace;
	enum video_range_type range;
	struct spa_fraction framerate;

	struct spa_rectangle sizes[MAX_SIZES];
	size_t n_sizes;

	/* Format agreed with the consumer. Protected by the PipeWire thread
	 * loop lock, as it's written from the PipeWire thread. */
	struct {
		bool valid;
		enum video_format format;
		uint32_t width;
		uint32_t height;
		struct frame_layout layout;
		video_scaler_t *scaler;
	} negotiated;

	uint64_t seq;
};

/* ------------------------------------------------------------------------- */

static enum video_format video_format_from_spa(uint32_t spa_format)
{
	for (size_t i = 0; i < N_VCAM_FORMATS; i++) {
		if (vcam_formats[i].spa_format == spa_format)
			return vcam_formats[i].video_format;
	}
	return VIDEO_FORMAT_NONE;
}

static uint32_t spa_format_from_video(enum video_format format)
{
	for (size_t i = 0; i < N_VCAM_FORMATS; i++) {
		if (vcam_formats[i].video_format == format)
			return vcam_formats[i].spa_format;
	}
	return SPA_VIDEO_FORMAT_UNKNOWN;
}

/* Planes are stored back to back without any row padding */
static bool get_frame_layout(enum video_format format, uint32_t width, uint32_t height, struct frame_layout *layout)
{
	const uint32_t chroma_width = (width + 1) / 2;
	const uint32_t chroma_height = (height + 1) / 2;

	memset(layout, 0, sizeof(*layout));

	switch (format) {
	case VIDEO_FORMAT_I420:
		layout->n_planes = 3;
		layout->linesize[0] = width;
		layout->height[0] = height;
		layout->linesize[1] = chroma_width;
		layout->height[1] = chroma_height;
		layout->linesize[2] = chroma_width;
		layout->height[2] = chroma_height;
		break;
	case VIDEO_FORMAT_NV12:
		layout->n_planes = 2;
		layout->linesize[0] = width;
		layout->height[0] = height;
		layout->linesize[1] = chroma_width * 2;
		layout->height[1] = chroma_height;
		break;
	case VIDEO_FORMAT_YUY2:
		layout->n_planes = 1;
		layout->linesize[0] = chroma_width * 4;
		layout->height[0] = height;
		break;
	default:
		return false;
	}

	for (uint32_t i = 0; i < layout->n_planes; i++) {
		layout->offset[i] = layout->size;
		layout->size += layout->linesize[i] * layout->height[i];
	}

	return true;
}

static void build_sizes(struct pipewire_virtualcam *vcam)
{
	const uint32_t width = vcam->src_width;
	const uint32_t height = vcam->src_height;

	vcam->n_sizes = 0;
	vcam->sizes[vcam->n_sizes++] = SPA_RECTANGLE(width, height);

	for (size_t i = 0; i < SPA_N_ELEMENTS(vcam_extra_heights); i++) {
		uint32_t h = vcam_extra_heights[i];
		uint32_t w;

		if (vcam->n_sizes >= MAX_SIZES)
			break;
		if (h >= height)
			continue;

		/* Keep the aspect ratio of the canvas, rounded to even */
		w = (uint32_t)(((uint64_t)width * h + height / 2) / height);
		w = (w + 1) & ~1u;
		if (w < 2)
			continue;

		vcam->sizes[vcam->n_sizes++] = SPA_RECTANGLE(w, h);
	}
}

static const struct spa_pod *build_format(struct spa_pod_builder *b, uint32_t spa_format,
					  const struct spa_rectangle *size, const struct spa_fraction *framerate)
{
	struct spa_pod_frame format_frame;
	struct spa_pod_frame choice_frame;
	const uint32_t num = framerate->num;
	const uint32_t denom = framerate->denom;

	spa_pod_builder_push_object(b, &format_frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype,
			    SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format, SPA_POD_Id(spa_format),
			    SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(size), 0);

	/* libwebrtc requests the integer part of the highest framerate it
	 * finds, so also offer whole number framerates around fractional
	 * ones such as 30000/1001. The first value is the default. */
	spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_framerate, 0);
	spa_pod_builder_push_choice(b, &choice_frame, SPA_CHOICE_Enum, 0);
	spa_pod_builder_fraction(b, num, denom);
	spa_pod_builder_fraction(b, num, denom);
	if (num % denom != 0) {
		if (num / denom > 0)
			spa_pod_builder_fraction(b, num / denom, 1);
		spa_pod_builder_fraction(b, num / denom + 1, 1);
	}
	spa_pod_builder_pop(b, &choice_frame);

	return spa_pod_builder_pop(b, &format_frame);
}

static bool needs_conversion(const struct pipewire_virtualcam *vcam, enum video_format format, uint32_t width,
			     uint32_t height)
{
	return format != vcam->src_format || width != vcam->src_width || height != vcam->src_height;
}

static bool create_scaler(const struct pipewire_virtualcam *vcam, enum video_format format, uint32_t width,
			  uint32_t height, video_scaler_t **scaler)
{
	const struct video_scale_info src = {
		.format = vcam->src_format,
		.width = vcam->src_width,
		.height = vcam->src_height,
		.range = vcam->range,
		.colorspace = vcam->colorspace,
	};
	const struct video_scale_info dst = {
		.format = format,
		.width = width,
		.height = height,
		.range = vcam->range,
		.colorspace = vcam->colorspace,
	};

	/* VIDEO_SCALE_BILINEAR maps to a flag combination that FFmpeg 8 rejects */
	if (video_scaler_create(scaler, &dst, &src, VIDEO_SCALE_BICUBIC) != VIDEO_SCALER_SUCCESS) {
		*scaler = NULL;
		return false;
	}
	return true;
}

/* Must be called with the thread loop locked, or before connecting */
static void clear_negotiated_format(struct pipewire_virtualcam *vcam)
{
	video_scaler_destroy(vcam->negotiated.scaler);
	memset(&vcam->negotiated, 0, sizeof(vcam->negotiated));
}

/* ------------------------------------------------------------------------- */

static void on_param_changed_cb(void *user_data, uint32_t id, const struct spa_pod *param)
{
	struct pipewire_virtualcam *vcam = user_data;
	struct spa_video_info_raw info;
	struct frame_layout layout;
	struct spa_pod_builder pod_builder;
	const struct spa_pod *params[2];
	uint8_t params_buffer[1024];
	uint32_t media_type, media_subtype;
	enum video_format format;

	if (id != SPA_PARAM_Format)
		return;

	clear_negotiated_format(vcam);

	/* A NULL format means the consumer went away */
	if (!param)
		return;

	if (spa_format_parse(param, &media_type, &media_subtype) < 0 || media_type != SPA_MEDIA_TYPE_video ||
	    media_subtype != SPA_MEDIA_SUBTYPE_raw)
		return;

	if (spa_format_video_raw_parse(param, &info) < 0)
		return;

	format = video_format_from_spa(info.format);
	if (format == VIDEO_FORMAT_NONE || !get_frame_layout(format, info.size.width, info.size.height, &layout)) {
		blog(LOG_WARNING, "[pipewire-virtualcam] Unsupported format negotiated: %u", info.format);
		return;
	}

	/* Only combinations that passed this check in create_stream() are
	 * offered, so a failure here is unexpected. Don't put the stream in
	 * an error state, as that would break it for later consumers too. */
	if (needs_conversion(vcam, format, info.size.width, info.size.height) &&
	    !create_scaler(vcam, format, info.size.width, info.size.height, &vcam->negotiated.scaler)) {
		blog(LOG_WARNING, "[pipewire-virtualcam] Failed to create video scaler for %s %ux%u",
		     get_video_format_name(format), info.size.width, info.size.height);
		return;
	}

	vcam->negotiated.format = format;
	vcam->negotiated.width = info.size.width;
	vcam->negotiated.height = info.size.height;
	vcam->negotiated.layout = layout;
	vcam->negotiated.valid = true;

	blog(LOG_INFO, "[pipewire-virtualcam] Negotiated %s %ux%u @ %u/%u%s", get_video_format_name(format),
	     info.size.width, info.size.height, info.framerate.num, info.framerate.denom,
	     vcam->negotiated.scaler ? " (converted)" : "");

	pod_builder = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));

	params[0] = spa_pod_builder_add_object(
		&pod_builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers,
		SPA_POD_CHOICE_RANGE_Int(8, 2, 16), SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1), SPA_PARAM_BUFFERS_size,
		SPA_POD_Int(layout.size), SPA_PARAM_BUFFERS_stride, SPA_POD_Int(layout.linesize[0]),
		SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)));

	/* libwebrtc reads the header without checking that it exists */
	params[1] = spa_pod_builder_add_object(&pod_builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
					       SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header), SPA_PARAM_META_size,
					       SPA_POD_Int(sizeof(struct spa_meta_header)));

	pw_stream_update_params(vcam->stream, params, 2);
}

static void on_state_changed_cb(void *user_data, enum pw_stream_state old, enum pw_stream_state state,
				const char *error)
{
	UNUSED_PARAMETER(user_data);

	blog(LOG_INFO, "[pipewire-virtualcam] Stream state: \"%s\" -> \"%s\"%s%s", pw_stream_state_as_string(old),
	     pw_stream_state_as_string(state), error ? ", error: " : "", error ? error : "");
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state_changed_cb,
	.param_changed = on_param_changed_cb,
};

/* ------------------------------------------------------------------------- */

static void destroy_stream(struct pipewire_virtualcam *vcam)
{
	if (!vcam->obs_pw)
		return;

	struct pw_thread_loop *thread_loop = obs_pipewire_get_thread_loop(vcam->obs_pw);

	pw_thread_loop_lock(thread_loop);
	if (vcam->stream) {
		spa_hook_remove(&vcam->stream_listener);
		pw_stream_disconnect(vcam->stream);
		pw_stream_destroy(vcam->stream);
		vcam->stream = NULL;
	}
	clear_negotiated_format(vcam);
	pw_thread_loop_unlock(thread_loop);

	obs_pipewire_destroy(vcam->obs_pw);
	vcam->obs_pw = NULL;
}

static bool create_stream(struct pipewire_virtualcam *vcam)
{
	struct pw_thread_loop *thread_loop;
	struct pw_properties *props;
	struct spa_pod_builder pod_builder;
	const struct spa_pod *params[MAX_SIZES * N_VCAM_FORMATS];
	uint8_t *params_buffer;
	uint32_t n_params = 0;
	int res;

	vcam->obs_pw = obs_pipewire_connect(NULL, NULL);
	if (!vcam->obs_pw)
		return false;

	thread_loop = obs_pipewire_get_thread_loop(vcam->obs_pw);
	params_buffer = bzalloc(PARAMS_BUFFER_SIZE);
	pod_builder = SPA_POD_BUILDER_INIT(params_buffer, PARAMS_BUFFER_SIZE);

	/* Canvas size first, and the format OBS hands us first within each
	 * size, so consumers without a preference avoid any conversion.
	 * Combinations the scaler can't handle are left out. */
	for (size_t i = 0; i < vcam->n_sizes; i++) {
		const struct spa_rectangle *size = &vcam->sizes[i];

		for (size_t j = 0; j <= N_VCAM_FORMATS; j++) {
			enum video_format format;

			if (j == 0)
				format = vcam->src_format;
			else if (vcam_formats[j - 1].video_format != vcam->src_format)
				format = vcam_formats[j - 1].video_format;
			else
				continue;

			if (needs_conversion(vcam, format, size->width, size->height)) {
				video_scaler_t *scaler;

				if (!create_scaler(vcam, format, size->width, size->height, &scaler)) {
					blog(LOG_DEBUG, "[pipewire-virtualcam] Not offering %s %ux%u",
					     get_video_format_name(format), size->width, size->height);
					continue;
				}
				video_scaler_destroy(scaler);
			}

			params[n_params++] =
				build_format(&pod_builder, spa_format_from_video(format), size, &vcam->framerate);
		}
	}

	if (n_params == 0) {
		bfree(params_buffer);
		destroy_stream(vcam);
		return false;
	}

	props = pw_properties_new(PW_KEY_NODE_NAME, VCAM_NODE_NAME, PW_KEY_NODE_DESCRIPTION, VCAM_NODE_DESCRIPTION,
				  PW_KEY_MEDIA_CLASS, "Video/Source", PW_KEY_MEDIA_ROLE, "Camera", NULL);

	pw_thread_loop_lock(thread_loop);

	vcam->stream = pw_stream_new(obs_pipewire_get_core(vcam->obs_pw), VCAM_NODE_DESCRIPTION, props);
	if (!vcam->stream) {
		pw_thread_loop_unlock(thread_loop);
		bfree(params_buffer);
		blog(LOG_WARNING, "[pipewire-virtualcam] Failed to create stream: %m");
		destroy_stream(vcam);
		return false;
	}

	pw_stream_add_listener(vcam->stream, &vcam->stream_listener, &stream_events, vcam);

	/* No autoconnect: the node just waits for a camera consumer */
	res = pw_stream_connect(vcam->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
				PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS, params, n_params);

	pw_thread_loop_unlock(thread_loop);
	bfree(params_buffer);

	if (res < 0) {
		blog(LOG_WARNING, "[pipewire-virtualcam] Failed to connect stream: %s", spa_strerror(res));
		destroy_stream(vcam);
		return false;
	}

	blog(LOG_INFO, "[pipewire-virtualcam] Created stream %p offering %u formats", vcam->stream, n_params);
	return true;
}

/* ------------------------------------------------------------------------- */

static const char *virtualcam_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("PipeWireVirtualCamera");
}

static void *virtualcam_create(obs_data_t *settings, obs_output_t *output)
{
	UNUSED_PARAMETER(settings);

	struct pipewire_virtualcam *vcam = bzalloc(sizeof(struct pipewire_virtualcam));
	vcam->output = output;
	pthread_mutex_init(&vcam->mutex, NULL);

	return vcam;
}

static void virtualcam_destroy(void *data)
{
	struct pipewire_virtualcam *vcam = data;

	pthread_mutex_lock(&vcam->mutex);
	destroy_stream(vcam);
	pthread_mutex_unlock(&vcam->mutex);

	pthread_mutex_destroy(&vcam->mutex);
	bfree(vcam);
}

static bool virtualcam_start(void *data)
{
	struct pipewire_virtualcam *vcam = data;
	const struct video_output_info *voi;
	video_t *video;
	bool success;

	video = obs_output_video(vcam->output);
	if (!video)
		return false;

	voi = video_output_get_info(video);

	/* Get frames in a format we can pass through untouched when the
	 * consumer agrees with it, I420 otherwise */
	if (voi->format == VIDEO_FORMAT_NV12 || voi->format == VIDEO_FORMAT_I420)
		vcam->src_format = voi->format;
	else
		vcam->src_format = VIDEO_FORMAT_I420;

	vcam->src_width = voi->width & ~1u;
	vcam->src_height = voi->height & ~1u;
	/* Camera consumers expect limited range SDR video, like the v4l2
	 * virtual camera sends */
	vcam->colorspace = voi->colorspace == VIDEO_CS_601 ? VIDEO_CS_601 : VIDEO_CS_709;
	vcam->range = VIDEO_RANGE_PARTIAL;
	vcam->framerate = SPA_FRACTION(voi->fps_num, voi->fps_den);
	vcam->seq = 0;

	if (vcam->src_width < 2 || vcam->src_height < 2 || !voi->fps_num || !voi->fps_den)
		return false;

	build_sizes(vcam);

	pthread_mutex_lock(&vcam->mutex);
	success = create_stream(vcam);
	pthread_mutex_unlock(&vcam->mutex);

	if (!success) {
		obs_output_set_last_error(vcam->output, obs_module_text("PipeWireVirtualCamera.ConnectFailed"));
		return false;
	}

	struct video_scale_info vsi = {
		.format = vcam->src_format,
		.width = vcam->src_width,
		.height = vcam->src_height,
		.range = vcam->range,
		.colorspace = vcam->colorspace,
	};
	obs_output_set_video_conversion(vcam->output, &vsi);

	if (!obs_output_begin_data_capture(vcam->output, 0)) {
		pthread_mutex_lock(&vcam->mutex);
		destroy_stream(vcam);
		pthread_mutex_unlock(&vcam->mutex);
		return false;
	}

	blog(LOG_INFO, "[pipewire-virtualcam] Virtual camera started (%ux%u %s @ %u/%u)", vcam->src_width,
	     vcam->src_height, get_video_format_name(vcam->src_format), voi->fps_num, voi->fps_den);
	return true;
}

static void virtualcam_stop(void *data, uint64_t ts)
{
	UNUSED_PARAMETER(ts);

	struct pipewire_virtualcam *vcam = data;

	obs_output_end_data_capture(vcam->output);

	pthread_mutex_lock(&vcam->mutex);
	destroy_stream(vcam);
	pthread_mutex_unlock(&vcam->mutex);

	blog(LOG_INFO, "[pipewire-virtualcam] Virtual camera stopped");
}

static void copy_frame(const struct frame_layout *layout, uint8_t *const planes[], const struct video_data *frame)
{
	for (uint32_t p = 0; p < layout->n_planes; p++) {
		const uint32_t linesize = layout->linesize[p];
		const uint8_t *src = frame->data[p];
		uint8_t *dst = planes[p];

		if (frame->linesize[p] == linesize) {
			memcpy(dst, src, (size_t)linesize * layout->height[p]);
			continue;
		}

		for (uint32_t y = 0; y < layout->height[p]; y++) {
			memcpy(dst, src, linesize);
			dst += linesize;
			src += frame->linesize[p];
		}
	}
}

static void virtualcam_raw_video(void *data, struct video_data *frame)
{
	struct pipewire_virtualcam *vcam = data;
	struct pw_thread_loop *thread_loop;
	const struct frame_layout *layout;
	struct spa_meta_header *header;
	struct spa_buffer *spa_buf;
	struct pw_buffer *buffer;
	struct spa_data *d;
	bool ok = false;

	pthread_mutex_lock(&vcam->mutex);

	if (!vcam->stream)
		goto unlock;

	thread_loop = obs_pipewire_get_thread_loop(vcam->obs_pw);
	pw_thread_loop_lock(thread_loop);

	if (!vcam->negotiated.valid || pw_stream_get_state(vcam->stream, NULL) != PW_STREAM_STATE_STREAMING)
		goto unlock_loop;

	/* No free buffer means the consumer is behind; drop this frame */
	buffer = pw_stream_dequeue_buffer(vcam->stream);
	if (!buffer)
		goto unlock_loop;

	spa_buf = buffer->buffer;
	d = &spa_buf->datas[0];
	layout = &vcam->negotiated.layout;

	if (d->data && d->maxsize >= layout->size) {
		uint8_t *planes[MAX_AV_PLANES] = {0};

		for (uint32_t p = 0; p < layout->n_planes; p++)
			planes[p] = (uint8_t *)d->data + layout->offset[p];

		if (vcam->negotiated.scaler) {
			ok = video_scaler_scale(vcam->negotiated.scaler, planes, layout->linesize,
						(const uint8_t *const *)frame->data, frame->linesize);
		} else {
			copy_frame(layout, planes, frame);
			ok = true;
		}
	}

	if (d->chunk) {
		d->chunk->offset = 0;
		d->chunk->size = ok ? layout->size : 0;
		d->chunk->stride = layout->linesize[0];
		d->chunk->flags = ok ? 0 : SPA_CHUNK_FLAG_CORRUPTED;
	}

	header = spa_buffer_find_meta_data(spa_buf, SPA_META_Header, sizeof(*header));
	if (header) {
		header->pts = (int64_t)frame->timestamp;
		header->flags = ok ? 0 : SPA_META_HEADER_FLAG_CORRUPTED;
		header->seq = vcam->seq++;
		header->dts_offset = 0;
	}

	pw_stream_queue_buffer(vcam->stream, buffer);
	pw_stream_trigger_process(vcam->stream);

unlock_loop:
	pw_thread_loop_unlock(thread_loop);
unlock:
	pthread_mutex_unlock(&vcam->mutex);
}

static struct obs_output_info pipewire_virtualcam_info = {
	.id = PIPEWIRE_VIRTUALCAM_ID,
	.flags = OBS_OUTPUT_VIDEO,
	.get_name = virtualcam_get_name,
	.create = virtualcam_create,
	.destroy = virtualcam_destroy,
	.start = virtualcam_start,
	.stop = virtualcam_stop,
	.raw_video = virtualcam_raw_video,
};

void virtualcam_load(void)
{
	obs_register_output(&pipewire_virtualcam_info);
}
