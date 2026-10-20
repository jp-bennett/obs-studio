/* Minimal PipeWire camera consumer that mimics libwebrtc's
 * modules/video_capture/linux/{pipewire_session,video_capture_pipewire}.cc:
 *  - finds nodes with media.role == "Camera" and a node.description
 *  - enumerates EnumFormat params, accepting only fixed (CHOICE_None) sizes
 *  - computes maxFPS the same way libwebrtc does
 *  - connects requesting a fixed format/size/integer framerate
 *  - requests unpadded stride + Header/VideoTransform meta
 *  - reads datas[0] only, dereferencing the header without a NULL check
 * Usage: webrtc-like-consumer <FORMAT: I420|NV12|YUY2> <width> <height> <frames>
 */
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/iter.h>
#include <spa/debug/types.h>
#include <spa/param/video/type-info.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cap {
	uint32_t format, w, h;
	int max_fps;
};

struct data {
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_registry *registry;
	struct spa_hook registry_listener, core_listener, node_listener, stream_listener;
	struct pw_proxy *node;
	uint32_t node_id;
	char *description;
	int sync;
	struct cap caps[64];
	int n_caps;
	int phase;
	struct pw_stream *stream;
	uint32_t want_format, want_w, want_h;
	struct spa_video_info_raw negotiated;
	int frames, wanted_frames, bad;
	uint32_t expected_size;
};

static void on_node_param(void *d_, int seq, uint32_t id, uint32_t index, uint32_t next, const struct spa_pod *param)
{
	struct data *d = d_;
	const struct spa_pod_object *obj = (const struct spa_pod_object *)param;
	const struct spa_pod_prop *prop;
	struct spa_pod *val;
	uint32_t n_items, choice;
	struct cap cap = {0};

	prop = spa_pod_object_find_prop(obj, NULL, SPA_FORMAT_VIDEO_framerate);
	if (prop) {
		val = spa_pod_get_values(&prop->value, &n_items, &choice);
		if (val->type == SPA_TYPE_Fraction) {
			struct spa_fraction *f = SPA_POD_BODY(val);
			if (choice == SPA_CHOICE_None)
				cap.max_fps = 1.0 * f[0].num / f[0].denom;
			else if (choice == SPA_CHOICE_Enum) {
				for (uint32_t i = 1; i < n_items; i++) {
					int v = (int)(1.0 * f[i].num / f[i].denom);
					if (v > cap.max_fps)
						cap.max_fps = v;
				}
			} else if (choice == SPA_CHOICE_Range && f[1].num > 0)
				cap.max_fps = 1.0 * f[1].num / f[1].denom;
		}
	}
	prop = spa_pod_object_find_prop(obj, NULL, SPA_FORMAT_VIDEO_size);
	if (!prop)
		return;
	val = spa_pod_get_values(&prop->value, &n_items, &choice);
	if (val->type != SPA_TYPE_Rectangle || choice != SPA_CHOICE_None) {
		printf("  (ignored: size is not fixed, choice=%u)\n", choice);
		return;
	}
	prop = spa_pod_object_find_prop(obj, NULL, SPA_FORMAT_VIDEO_format);
	if (!prop)
		return;
	struct spa_pod *fv = spa_pod_get_values(&prop->value, &n_items, &choice);
	if (fv->type != SPA_TYPE_Id || choice != SPA_CHOICE_None) {
		printf("  (ignored: format is not fixed)\n");
		return;
	}
	cap.format = *(uint32_t *)SPA_POD_BODY(fv);
	struct spa_rectangle *r = SPA_POD_BODY(val);
	cap.w = r->width;
	cap.h = r->height;
	printf("  capability: %s %ux%u @ maxFPS %d\n", spa_debug_type_find_short_name(spa_type_video_format, cap.format),
	       cap.w, cap.h, cap.max_fps);
	if (d->n_caps < 64)
		d->caps[d->n_caps++] = cap;
}

static const struct pw_node_events node_events = {PW_VERSION_NODE_EVENTS, .param = on_node_param};

static void on_global(void *d_, uint32_t id, uint32_t perms, const char *type, uint32_t version,
		      const struct spa_dict *props)
{
	struct data *d = d_;
	if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
		return;
	const char *desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
	const char *role = spa_dict_lookup(props, PW_KEY_MEDIA_ROLE);
	if (!desc || !role || strcmp(role, "Camera"))
		return;
	printf("Found camera node %u: '%s' (node.name=%s, media.class=%s)\n", id, desc,
	       spa_dict_lookup(props, PW_KEY_NODE_NAME), spa_dict_lookup(props, PW_KEY_MEDIA_CLASS));
	if (d->node)
		return;
	d->node_id = id;
	d->description = strdup(desc);
	d->node = pw_registry_bind(d->registry, id, type, PW_VERSION_NODE, 0);
	pw_proxy_add_object_listener(d->node, &d->node_listener, &node_events, d);
	pw_node_enum_params((struct pw_node *)d->node, 0, SPA_PARAM_EnumFormat, 0, UINT32_MAX, NULL);
	d->sync = pw_core_sync(d->core, PW_ID_CORE, d->sync);
}

static const struct pw_registry_events registry_events = {PW_VERSION_REGISTRY_EVENTS, .global = on_global};

static void on_stream_param_changed(void *d_, uint32_t id, const struct spa_pod *param)
{
	struct data *d = d_;
	if (!param || id != SPA_PARAM_Format)
		return;
	spa_format_video_raw_parse(param, &d->negotiated);
	printf("Negotiated: %s %ux%u @ %u/%u\n",
	       spa_debug_type_find_short_name(spa_type_video_format, d->negotiated.format), d->negotiated.size.width,
	       d->negotiated.size.height, d->negotiated.framerate.num, d->negotiated.framerate.denom);
	uint32_t w = d->negotiated.size.width, h = d->negotiated.size.height, stride;
	switch (d->negotiated.format) {
	case SPA_VIDEO_FORMAT_I420:
		stride = w;
		d->expected_size = w * h + 2 * ((w + 1) / 2) * ((h + 1) / 2);
		break;
	case SPA_VIDEO_FORMAT_NV12:
		stride = w;
		d->expected_size = w * h + ((w + 1) / 2) * 2 * ((h + 1) / 2);
		break;
	default:
		stride = w * 2;
		d->expected_size = w * h * 2;
		break;
	}
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_pod *params[3];
	struct spa_pod_frame f;
	spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
	spa_pod_builder_add(&b, SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride), 0);
	spa_pod_builder_add(&b, SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 1, 32), SPA_PARAM_BUFFERS_dataType,
			    SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_DmaBuf) | (1 << SPA_DATA_MemFd) |
						     (1 << SPA_DATA_MemPtr)),
			    0);
	params[0] = spa_pod_builder_pop(&b, &f);
	params[1] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type,
					       SPA_POD_Id(SPA_META_Header), SPA_PARAM_META_size,
					       SPA_POD_Int(sizeof(struct spa_meta_header)));
	params[2] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type,
					       SPA_POD_Id(SPA_META_VideoTransform), SPA_PARAM_META_size,
					       SPA_POD_Int(sizeof(struct spa_meta_videotransform)));
	pw_stream_update_params(d->stream, params, 3);
}

static void on_stream_state(void *d_, enum pw_stream_state old, enum pw_stream_state state, const char *error)
{
	struct data *d = d_;
	printf("Stream state: %s -> %s %s\n", pw_stream_state_as_string(old), pw_stream_state_as_string(state),
	       error ? error : "");
	if (state == PW_STREAM_STATE_ERROR) {
		d->bad++;
		pw_main_loop_quit(d->loop);
	}
}

static void on_process(void *d_)
{
	struct data *d = d_;
	struct pw_buffer *b;
	while ((b = pw_stream_dequeue_buffer(d->stream))) {
		struct spa_buffer *sb = b->buffer;
		struct spa_meta_header *h = spa_buffer_find_meta_data(sb, SPA_META_Header, sizeof(*h));
		if (!h) {
			printf("FAIL: no header meta (libwebrtc would crash here)\n");
			d->bad++;
			pw_main_loop_quit(d->loop);
			return;
		}
		if (h->flags & SPA_META_HEADER_FLAG_CORRUPTED) {
			printf("frame flagged corrupted\n");
			d->bad++;
		}
		uint8_t *data = NULL;
		void *map = NULL;
		if (sb->datas[0].type == SPA_DATA_MemFd || sb->datas[0].type == SPA_DATA_DmaBuf) {
			map = mmap(NULL, sb->datas[0].maxsize + sb->datas[0].mapoffset, PROT_READ, MAP_SHARED,
				   sb->datas[0].fd, 0);
			data = (uint8_t *)map + sb->datas[0].mapoffset;
		} else {
			data = sb->datas[0].data;
		}
		uint32_t size = sb->datas[0].chunk->size;
		/* Average luma and first chroma sample as a content sanity check */
		uint64_t sum = 0;
		uint32_t w = d->negotiated.size.width, hgt = d->negotiated.size.height;
		uint32_t n = 0;
		if (d->negotiated.format == SPA_VIDEO_FORMAT_YUY2) {
			for (uint32_t i = 0; i < w * hgt * 2; i += 2, n++)
				sum += data[i];
		} else {
			for (uint32_t i = 0; i < w * hgt; i++, n++)
				sum += data[i];
		}
		uint8_t chroma0 = d->negotiated.format == SPA_VIDEO_FORMAT_YUY2 ? data[1] : data[w * hgt];
		uint8_t v0 = d->negotiated.format == SPA_VIDEO_FORMAT_YUY2   ? data[3]
			     : d->negotiated.format == SPA_VIDEO_FORMAT_NV12 ? data[w * hgt + 1]
									     : data[w * hgt + ((w + 1) / 2) * ((hgt + 1) / 2)];
		if (d->frames < 3 || d->frames + 1 == d->wanted_frames)
			printf("frame %d: type=%u chunk.size=%u (expected %u) stride=%d pts=%" PRId64
			       " seq=%" PRIu64 " avgY=%.1f U=%u V=%u\n",
			       d->frames, sb->datas[0].type, size, d->expected_size, sb->datas[0].chunk->stride, h->pts,
			       h->seq, n ? (double)sum / n : 0.0, chroma0, v0);
		if (size != d->expected_size) {
			printf("FAIL: unexpected chunk size\n");
			d->bad++;
		}
		if (map)
			munmap(map, sb->datas[0].maxsize + sb->datas[0].mapoffset);
		pw_stream_queue_buffer(d->stream, b);
		if (++d->frames >= d->wanted_frames)
			pw_main_loop_quit(d->loop);
	}
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_stream_state,
	.param_changed = on_stream_param_changed,
	.process = on_process,
};

static void start_capture(struct data *d)
{
	struct cap *c = NULL;
	for (int i = 0; i < d->n_caps; i++) {
		if (d->caps[i].format == d->want_format && d->caps[i].w == d->want_w && d->caps[i].h == d->want_h)
			c = &d->caps[i];
	}
	if (!c) {
		printf("FAIL: requested capability not offered\n");
		d->bad++;
		pw_main_loop_quit(d->loop);
		return;
	}
	printf("Requesting %s %ux%u @ %d/1 (as libwebrtc would)\n",
	       spa_debug_type_find_short_name(spa_type_video_format, c->format), c->w, c->h, c->max_fps);
	d->stream = pw_stream_new(d->core, "camera-stream", pw_properties_new_string("pipewire.client.reuse=1"));
	pw_stream_add_listener(d->stream, &d->stream_listener, &stream_events, d);
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct spa_rectangle res = SPA_RECTANGLE(c->w, c->h);
	struct spa_fraction fr = SPA_FRACTION((uint32_t)c->max_fps, 1);
	const struct spa_pod *params[1];
	params[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType,
					       SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype,
					       SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format,
					       SPA_POD_Id(c->format), SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&res),
					       SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&fr));
	pw_stream_connect(d->stream, PW_DIRECTION_INPUT, d->node_id,
			  PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_DONT_RECONNECT, params, 1);
}

static void on_core_done(void *d_, uint32_t id, int seq)
{
	struct data *d = d_;
	if (id != PW_ID_CORE || seq != d->sync)
		return;
	if (d->phase == 0 && d->node) {
		/* wait one more roundtrip so that all params arrived */
		d->phase = 1;
		d->sync = pw_core_sync(d->core, PW_ID_CORE, d->sync);
	} else if (d->phase == 0) {
		printf("FAIL: no camera node found\n");
		d->bad++;
		pw_main_loop_quit(d->loop);
	} else if (d->phase == 1) {
		d->phase = 2;
		printf("%d capabilities for '%s'\n", d->n_caps, d->description);
		start_capture(d);
	}
}

static const struct pw_core_events core_events = {PW_VERSION_CORE_EVENTS, .done = on_core_done};

static void on_timeout(void *d_, uint64_t exp)
{
	struct data *d = d_;
	printf("FAIL: timeout after %d frames\n", d->frames);
	d->bad++;
	pw_main_loop_quit(d->loop);
}

int main(int argc, char **argv)
{
	struct data d = {0};
	pw_init(&argc, &argv);
	d.want_format = argc > 1 && !strcmp(argv[1], "NV12")   ? SPA_VIDEO_FORMAT_NV12
			: argc > 1 && !strcmp(argv[1], "YUY2") ? SPA_VIDEO_FORMAT_YUY2
							       : SPA_VIDEO_FORMAT_I420;
	d.want_w = argc > 2 ? atoi(argv[2]) : 640;
	d.want_h = argc > 3 ? atoi(argv[3]) : 360;
	d.wanted_frames = argc > 4 ? atoi(argv[4]) : 30;
	d.loop = pw_main_loop_new(NULL);
	d.context = pw_context_new(pw_main_loop_get_loop(d.loop), NULL, 0);
	d.core = pw_context_connect(d.context, NULL, 0);
	if (!d.core) {
		printf("FAIL: cannot connect\n");
		return 2;
	}
	pw_core_add_listener(d.core, &d.core_listener, &core_events, &d);
	d.registry = pw_core_get_registry(d.core, PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(d.registry, &d.registry_listener, &registry_events, &d);
	d.sync = pw_core_sync(d.core, PW_ID_CORE, 0);
	struct spa_source *timer = pw_loop_add_timer(pw_main_loop_get_loop(d.loop), on_timeout, &d);
	struct timespec t = {.tv_sec = 15};
	pw_loop_update_timer(pw_main_loop_get_loop(d.loop), timer, &t, NULL, false);
	pw_main_loop_run(d.loop);
	if (d.stream)
		pw_stream_destroy(d.stream);
	printf("%s: %d frames, %d problems\n", d.bad ? "RESULT FAIL" : "RESULT OK", d.frames, d.bad);
	return d.bad ? 1 : 0;
}
