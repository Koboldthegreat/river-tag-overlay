#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pixman.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "river-status-unstable-v1.h"
#include "wlr-layer-shell-unstable-v1.h"

struct Buffer
{
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	size_t size;
	void *mmap;
	struct wl_buffer *wl_buffer;
	pixman_image_t *pixman_image;
	bool busy;
};

struct Surface
{
	struct wl_surface *wl_surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	struct Buffer buffer[2];
	struct timespec last_frame;
	bool configured;
};

struct Output
{
	struct wl_list link;
	struct wl_output *wl_output;
	uint32_t global_name;
	struct Surface *surface;
	struct zriver_output_status_v1 *river_status;
	uint32_t focused_tags, view_tags;
	uint32_t scale; // TODO
	bool configured;
};

int ret = EXIT_SUCCESS;
bool loop = true;
struct wl_display *wl_display = NULL;
struct wl_registry *wl_registry = NULL;
struct wl_callback *sync_callback = NULL;
struct wl_compositor *wl_compositor = NULL;
struct wl_shm *wl_shm = NULL;
struct zriver_status_manager_v1 *river_status_manager = NULL;
struct zwlr_layer_shell_v1 *layer_shell = NULL;
struct wl_list outputs;

uint32_t border_width = 2;
uint32_t tag_amount = 9;
uint32_t square_size = 40;
uint32_t square_padding = 15;
uint32_t square_border_width = 1;
uint32_t square_inner_padding = 10;

uint32_t surface_width;
uint32_t surface_height;

pixman_color_t background_colour;
pixman_color_t border_colour;
pixman_color_t inactive_square_background_colour;
pixman_color_t active_square_background_colour;
pixman_color_t inactive_square_border_colour;
pixman_color_t active_square_border_colour;
pixman_color_t inactive_square_occupied_colour;
pixman_color_t active_square_occupied_colour;

/************
 *          *
 *  Buffer  *
 *          *
 ************/
static void randomize_string (char *str, size_t len)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	long r = ts.tv_nsec;

	for (size_t i = 0; i < len; i++, str++)
	{
		/* Use two byte from the current nano-second to pseudo-randomly
		 * increase the ASCII character 'A' into another character,
		 * which will then subsitute the character at *str.
		 */
		*str = (char)('A' + (r&15) + (r&16));
		r >>= 5;
	}
}

/* Tries to create a shared memory object and returns its file descriptor if
 * successful.
 */
static bool get_shm_fd (int *fd, size_t size)
{
	char name[] = "/river-tag-overlay-RANDOM";
	char *rp    = name + strlen("/river-tag-overlay-");; /* Pointer to random part. */
	size_t rl   = strlen("RANDOM"); /* Length of random part. */

	/* Try a few times to get a unique name. */
	for (int tries = 100; tries > 0; tries--)
	{
		/* Make the name pseudo-random to not conflict with other
		 * running instances.
		 */
		randomize_string(rp, rl);

		/* Try to create a shared memory object. Returns -1 if the
		 * memory object already exists.
		 */
		*fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);

		/* If a shared memory object was created, set its size and
		 * return its file descriptor.
		 */
		if ( *fd >= 0 )
		{
			shm_unlink(name);
			if ( ftruncate(*fd, (off_t)size) < 0 )
			{
				fprintf(stderr, "ERROR: ftruncate: %s.\n", strerror(errno));
				close(*fd);
				return false;
			}
			return true;
		}

		/* The EEXIST error means that the name is not unique and we
		 * must try again.
		 */
		if ( errno != EEXIST )
		{
			fprintf(stderr, "ERROR: shm_open: %s.\n", strerror(errno));
			return false;
		}
	}

	return false;
}

static void buffer_handle_release (void *data, struct wl_buffer *wl_buffer)
{
	struct Buffer *buffer = (struct Buffer *)data;
	buffer->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_handle_release,
};

static void finish_buffer (struct Buffer *buffer)
{
	if ( buffer->wl_buffer != NULL )
		wl_buffer_destroy(buffer->wl_buffer);
	if ( buffer->pixman_image != NULL )
		pixman_image_unref(buffer->pixman_image);
	if ( buffer->mmap != NULL )
		munmap(buffer->mmap, buffer->size);
	memset(buffer, 0, sizeof(struct Buffer));
}

#define PIXMAN_STRIDE(A, B) (((PIXMAN_FORMAT_BPP(A) * B + 7) / 8 + 4 - 1) & -4)
static bool init_buffer (struct Buffer *buffer, uint32_t width, uint32_t height)
{
	bool ret = true;

	buffer->width  = width;
	buffer->height = height;
	buffer->stride = (uint32_t)PIXMAN_STRIDE(PIXMAN_a8r8g8b8, (int32_t)width);
	buffer->size   = (size_t)(buffer->stride * height);

	if ( buffer->size == 0 )
	{
		ret = false;
		goto cleanup;
	}

	int fd = -1;
	if (! get_shm_fd(&fd, buffer->size))
	{
		ret = false;
		goto cleanup;
	}

	buffer->mmap = mmap(NULL, buffer->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if ( buffer->mmap == MAP_FAILED )
	{
		fprintf(stderr, "ERROR: mmap: %s.\n", strerror(errno));
		ret = false;
		goto cleanup;
	}

	struct wl_shm_pool *shm_pool = wl_shm_create_pool(wl_shm, fd, (int32_t)buffer->size);
	if ( shm_pool == NULL )
	{
		ret = false;
		goto cleanup;
	}

	buffer->wl_buffer = wl_shm_pool_create_buffer(shm_pool, 0, (int32_t)width,
			(int32_t)height, (int32_t)buffer->stride, WL_SHM_FORMAT_ARGB8888);
	if ( buffer->wl_buffer == NULL )
	{
		ret = false;
		goto cleanup;
	}
	wl_buffer_add_listener(buffer->wl_buffer, &buffer_listener, buffer);

	buffer->pixman_image = pixman_image_create_bits_no_clear(PIXMAN_a8r8g8b8,
			(int32_t)width, (int32_t)height, buffer->mmap, (int32_t)buffer->stride);
	if ( buffer->pixman_image == NULL )
	{
		ret = false;
		goto cleanup;
	}

cleanup:
	if ( shm_pool != NULL )
		wl_shm_pool_destroy(shm_pool);
	if ( fd != -1 )
		close(fd);
	if (! ret)
		finish_buffer(buffer);

	return ret;
}
#undef PIXMAN_STRIDE

static struct Buffer *next_buffer (struct Surface *surface, uint32_t width, uint32_t height)
{
	int i;
	if (! surface->buffer[0].busy)
		i = 0;
	else if (! surface->buffer[1].busy)
		i = 1;
	else
		return NULL;

	if ( surface->buffer[i].width != width
			|| surface->buffer[i].height != height
			|| surface->buffer[i].wl_buffer == NULL )
	{
		finish_buffer(&surface->buffer[i]);
		if (! init_buffer(&surface->buffer[i], width, height))
			return NULL;
	}

	return &surface->buffer[i];
}

/*************
 *           *
 *  Surface  *
 *           *
 *************/
static void bordered_rectangle (pixman_image_t *image, uint32_t x, uint32_t y,
		uint32_t width, uint32_t height, uint32_t border, uint32_t scale,
		pixman_color_t *background_colour, pixman_color_t *border_colour)
{
	x *= scale, y *= scale, width *= scale, height *= scale, border *= scale;

	pixman_image_fill_rectangles(PIXMAN_OP_SRC, image, background_colour,
			1, &(pixman_rectangle16_t){
				(int16_t)x,
				(int16_t)y,
				(uint16_t)width,
				(uint16_t)height,
			});

	pixman_image_fill_rectangles(PIXMAN_OP_SRC, image, border_colour,
			4, (pixman_rectangle16_t[]){
				/* Top */
				{
					(int16_t)x,
					(int16_t)y,
					(uint16_t)width,
					(uint16_t)border,
				},

				/* Bottom */
				{
					(int16_t)x,
					(int16_t)(y + height - border),
					(uint16_t)width,
					(uint16_t)border,
				},

				/* Left */
				{
					(int16_t)x,
					(int16_t)(y + border),
					(uint16_t)border,
					(uint16_t)(height - 2 * border),
				},

				/* Right */
				{
					(int16_t)(x + width - border),
					(int16_t)(y + border),
					(uint16_t)border,
					(uint16_t)(height - 2 * border),
				},
			});
}

static void render_frame (struct Output *output)
{
	struct Surface *surface = output->surface;

	if (! surface->configured)
		return;

	struct Buffer *buffer = next_buffer(surface,
			surface_width * output->scale, surface_height * output->scale);
	if ( buffer == NULL )
		return;

	bordered_rectangle(buffer->pixman_image, 0, 0, buffer->width, buffer->height,
			border_width, output->scale, &background_colour, &border_colour);

	/* Tags. */
	#define TAG_ON(A, B) ( A & 1 << B )
	for (uint32_t i = 0; i < tag_amount; i++)
	{
		pixman_color_t *square_background_colour;
		pixman_color_t *square_border_colour;
		pixman_color_t *square_occupied_colour;
		if (TAG_ON(output->focused_tags, i))
		{
			square_background_colour = &active_square_background_colour;
			square_border_colour     = &active_square_border_colour;
			square_occupied_colour   = &active_square_occupied_colour;
		}
		else
		{
			square_background_colour = &inactive_square_background_colour;
			square_border_colour     = &inactive_square_border_colour;
			square_occupied_colour   = &inactive_square_occupied_colour;
		}

		const uint32_t x = border_width + ((i+1) * square_padding) + (i * square_size);
		const uint32_t y = border_width + square_padding;

		bordered_rectangle(buffer->pixman_image, x, y,
				square_size, square_size,
				square_border_width, output->scale,
				square_background_colour, square_border_colour);

		if (TAG_ON(output->view_tags, i))
		{
			bordered_rectangle(buffer->pixman_image,
					x + square_inner_padding,
					y + square_inner_padding,
					square_size - 2 * square_inner_padding,
					square_size - 2 * square_inner_padding,
					square_border_width, output->scale,
					square_occupied_colour, square_border_colour);
		}
	}
	#undef TAG_ON

	wl_surface_set_buffer_scale(surface->wl_surface, (int32_t)output->scale);
	wl_surface_attach(surface->wl_surface, buffer->wl_buffer, 0, 0);
	wl_surface_damage_buffer(surface->wl_surface, 0, 0,
			(int32_t)buffer->width, (int32_t)buffer->height);

	clock_gettime(CLOCK_MONOTONIC, &output->surface->last_frame);
}

static void layer_surface_handle_configure (void *data, struct zwlr_layer_surface_v1 *layer_surface,
		uint32_t serial, uint32_t width, uint32_t height)
{
	struct Output *output = (struct Output *)data;
	struct Surface *surface = output->surface;
	surface->configured = true;
	zwlr_layer_surface_v1_ack_configure(surface->layer_surface, serial);
	render_frame(output);
	wl_surface_commit(surface->wl_surface);
}

static void destroy_surface (struct Surface *surface)
{
	if ( surface->layer_surface != NULL )
		zwlr_layer_surface_v1_destroy(surface->layer_surface);
	if ( surface->wl_surface != NULL )
		wl_surface_destroy(surface->wl_surface );

	finish_buffer(&surface->buffer[0]);
	finish_buffer(&surface->buffer[1]);

	free(surface);
}

static void layer_surface_handle_closed (void *data, struct zwlr_layer_surface_v1 *layer_surface)
{
	struct Output *output = (struct Output *)data;
	destroy_surface(output->surface);
	output->surface = NULL;
}

const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_handle_configure,
	.closed    = layer_surface_handle_closed
};

static void update_surface (struct Output *output)
{
	if ( output->surface != NULL )
	{
		render_frame(output);
		wl_surface_commit(output->surface->wl_surface);
		return;
	}

	output->surface = calloc(1, sizeof(struct Surface));
	if ( output->surface == NULL )
	{
		fprintf(stderr, "ERROR: calloc: %s.\n", strerror(errno));
		return;
	}

	output->surface->wl_surface = wl_compositor_create_surface(wl_compositor);
	output->surface->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
			layer_shell, output->surface->wl_surface,
			output->wl_output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
			"river-tag-overlay");
	zwlr_layer_surface_v1_add_listener(output->surface->layer_surface,
			&layer_surface_listener, output);
	zwlr_layer_surface_v1_set_size(output->surface->layer_surface,
			surface_width, surface_height);

	struct wl_region *region = wl_compositor_create_region(wl_compositor);
	wl_surface_set_input_region(output->surface->wl_surface, region);
	wl_region_destroy(region);

	wl_surface_commit(output->surface->wl_surface);
}

/************
 *          *
 *  Output  *
 *          *
 ************/
static void river_status_handle_focused_tags (void *data, struct zriver_output_status_v1 *river_status,
		uint32_t tags)
{
	struct Output *output = (struct Output *)data;
	output->focused_tags = tags;
	update_surface(output);
}

static void river_status_handle_view_tags (void *data, struct zriver_output_status_v1 *river_status,
		struct wl_array *tags)
{
	struct Output *output = (struct Output *)data;
	uint32_t *i;
	output->view_tags = 0;
	wl_array_for_each(i, tags)
		output->view_tags |= *i;

	/* Only update the popup if it is already active. */
	if ( output->surface != NULL )
		update_surface(output);
}

static const struct zriver_output_status_v1_listener river_status_listener = {
	.focused_tags = river_status_handle_focused_tags,
	.view_tags    = river_status_handle_view_tags
};

static void destroy_output (struct Output *output)
{
	if ( output->surface != NULL )
		destroy_surface(output->surface);
	if ( output->river_status != NULL )
		zriver_output_status_v1_destroy(output->river_status);
	wl_output_destroy(output->wl_output);
	wl_list_remove(&output->link);
	free(output);
}

static struct Output *output_from_global_name (uint32_t name)
{
	struct Output *output;
	wl_list_for_each(output, &outputs, link)
		if ( output->global_name == name )
			return output;
	return NULL;
}

static void configure_output (struct Output *output)
{
	output->river_status = zriver_status_manager_v1_get_river_output_status(
			river_status_manager, output->wl_output);
	zriver_output_status_v1_add_listener(output->river_status,
			&river_status_listener, output);
	output->configured = true;
}

/**********
 *        *
 *  Main  *
 *        *
 **********/
static void registry_handle_global (void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version)
{
	if ( strcmp(interface, wl_output_interface.name) == 0 )
	{
		struct Output *output = calloc(1, sizeof(struct Output));
		if ( output == NULL )
		{
			fprintf(stderr, "ERROR: calloc: %s.\n", strerror(errno));
			return;
		}

		output->wl_output = wl_registry_bind(registry, name, &wl_output_interface, version);
		output->global_name = name;
		output->surface = NULL;
		wl_output_set_user_data(output->wl_output, output);
		wl_list_insert(&outputs, &output->link);
		output->scale = 1;

		if ( river_status_manager != NULL )
			configure_output(output);
	}
	else if ( strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0 )
		layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, version);
	else if ( strcmp(interface, zriver_status_manager_v1_interface.name) == 0 )
		river_status_manager = wl_registry_bind(registry, name, &zriver_status_manager_v1_interface, version);
	else if ( strcmp(interface, wl_compositor_interface.name) == 0 )
		wl_compositor = wl_registry_bind(registry, name, &wl_compositor_interface, version);
	else if ( strcmp(interface, wl_shm_interface.name) == 0 )
		wl_shm = wl_registry_bind(registry, name, &wl_shm_interface, version);
}

static void registry_handle_global_remove (void *data, struct wl_registry *registry, uint32_t name)
{
	struct Output *output = output_from_global_name(name);
	if ( output != NULL )
		destroy_output(output);
}

static const struct wl_registry_listener registry_listener = {
	.global        = registry_handle_global,
	.global_remove = registry_handle_global_remove,
};

static char *check_for_interfaces (void)
{
	if ( wl_compositor == NULL )
		return "wl_compositor";
	if ( wl_shm == NULL )
		return "wl_shm";
	if ( layer_shell == NULL )
		return "wlr_layershell_v1";
	if ( river_status_manager == NULL )
		return "river_status_v1";
	return NULL;
}

static void sync_handle_done (void *data, struct wl_callback *wl_callback, uint32_t other)
{
	wl_callback_destroy(wl_callback);
	sync_callback = NULL;

	const char *missing = check_for_interfaces();
	if ( missing != NULL )
	{
		fprintf(stderr, "ERROR, Wayland compositor does not support %s.\n", missing);
		loop = false;
		ret = EXIT_FAILURE;
		return;
	}

	struct Output *output;
	wl_list_for_each(output, &outputs, link)
		if (! output->configured)
			configure_output(output);
}

static const struct wl_callback_listener sync_callback_listener = {
	.done = sync_handle_done,
};

static void timespec_diff (struct timespec *a, struct timespec *b, struct timespec *result)
{
	result->tv_sec  = a->tv_sec  - b->tv_sec;
	result->tv_nsec = a->tv_nsec - b->tv_nsec;
	if ( result->tv_nsec < 0 )
	{
		result->tv_sec--;
		result->tv_nsec += 1000000000L;
	}
}

static bool colour_from_hex (pixman_color_t *colour, const char *hex)
{
	uint16_t r = 0, g = 0, b = 0, a = 255;

	if ( 4 != sscanf(hex, "0x%02hx%02hx%02hx%02hx", &r, &g, &b, &a)
			&& 3 != sscanf(hex, "0x%02hx%02hx%02hx", &r, &g, &b) )
		return false;

	colour->alpha = (uint16_t)(((double)a / 255.0) * 65535.0);
	colour->red   = (uint16_t)((((double)r / 255.0) * 65535.0) * colour->alpha / 0xffff);
	colour->green = (uint16_t)((((double)g / 255.0) * 65535.0) * colour->alpha / 0xffff);
	colour->blue  = (uint16_t)((((double)b / 255.0) * 65535.0) * colour->alpha / 0xffff);

	return true;
}

int main (int argc, char *argv[])
{
	// TODO handle opts

	surface_width = (tag_amount * (square_size + square_padding)) + square_padding + (2 * border_width);
	surface_height = square_size + (2 * square_padding) + (2 * border_width);

	colour_from_hex(&background_colour, "0x666666");
	colour_from_hex(&border_colour, "0x333333");
	colour_from_hex(&inactive_square_background_colour, "0x999999");
	colour_from_hex(&active_square_background_colour, "0xE6803A");
	colour_from_hex(&inactive_square_border_colour, "0x7F7F7F");
	colour_from_hex(&active_square_border_colour, "0xB24C21");
	colour_from_hex(&inactive_square_occupied_colour, "0xCCCCCC");
	colour_from_hex(&active_square_occupied_colour, "0xFFB277");

	/* We query the display name here instead of letting wl_display_connect()
	 * figure it out itself, because libwayland (for legacy reasons) falls
	 * back to using "wayland-0" when $WAYLAND_DISPLAY is not set, which is
	 * generally not desirable.
	 */
	const char *display_name = getenv("WAYLAND_DISPLAY");
	if ( display_name == NULL )
	{
		fputs("ERROR: WAYLAND_DISPLAY is not set.\n", stderr);
		return EXIT_FAILURE;
	}

	wl_display = wl_display_connect(display_name);
	if ( wl_display == NULL )
	{
		fputs("ERROR: Can not connect to wayland display.\n", stderr);
		return EXIT_FAILURE;
	}

	wl_list_init(&outputs);

	wl_registry = wl_display_get_registry(wl_display);
	wl_registry_add_listener(wl_registry, &registry_listener, NULL);

	sync_callback = wl_display_sync(wl_display);
	wl_callback_add_listener(sync_callback, &sync_callback_listener, NULL);

	struct pollfd pollfds[] = {
		{
			.fd = wl_display_get_fd(wl_display),
			.events = POLLIN,
		},
	};

	while (loop)
	{
		int timeout = -1;
		struct Output *output;
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		wl_list_for_each(output, &outputs, link)
		{
			if ( output->surface == NULL )
				continue;
			if (! output->surface->configured)
				continue;

			const long nsec_half_second = 500000000;
			const long epsilon = 1000000;
 			struct timespec time_since_last_frame;
			timespec_diff(&now, &output->surface->last_frame, &time_since_last_frame);
			if ( time_since_last_frame.tv_sec > 1 || time_since_last_frame.tv_nsec >= nsec_half_second - epsilon )
			{
				destroy_surface(output->surface);
				output->surface = NULL;
			}
			else
			{
				const int _timeout = (int)(nsec_half_second - time_since_last_frame.tv_nsec) / 1000000;
				if ( timeout == -1 || timeout > _timeout )
					timeout = _timeout;
			}
		}

		/* Flush wayland events. */
		do
		{
			if ( wl_display_flush(wl_display) == -1 && errno != EAGAIN)
			{
				fprintf(stderr, "ERROR: wl_display_flush: %s.\n", strerror(errno));
				break;
			}
		} while ( errno == EAGAIN );


		if ( poll(pollfds, 1, timeout) < 0 )
		{
			if ( errno == EINTR )
				continue;
			fprintf(stderr, "ERROR: poll: %s.\n", strerror(errno));
			ret = EXIT_FAILURE;
			break;
		}

		if ( (pollfds[0].revents & POLLIN) && wl_display_dispatch(wl_display) == -1 )
		{
			fprintf(stderr, "ERROR: wl_display_dispatch: %s.\n", strerror(errno));
			break;
		}
		if ( (pollfds[0].revents & POLLOUT) && wl_display_flush(wl_display) == -1 )
		{
			fprintf(stderr, "ERROR: wl_display_flush: %s.\n", strerror(errno));
			break;
		}
	}

	close(pollfds[0].fd);

	struct Output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &outputs, link)
		destroy_output(output);

	if ( wl_compositor != NULL )
		wl_compositor_destroy(wl_compositor);
	if ( wl_shm != NULL )
		wl_shm_destroy(wl_shm);
	if ( layer_shell != NULL )
		zwlr_layer_shell_v1_destroy(layer_shell);
	if ( river_status_manager != NULL )
		zriver_status_manager_v1_destroy(river_status_manager);
	if ( sync_callback != NULL )
		wl_callback_destroy(sync_callback);
	if ( wl_registry != NULL )
		wl_registry_destroy(wl_registry);
	wl_display_disconnect(wl_display);

	return ret;
}

