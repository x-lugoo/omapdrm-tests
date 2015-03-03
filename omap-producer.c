
#include <omap_drmif.h>
#include <pthread.h>
#include <sys/un.h>
#include <sys/socket.h>

#include "test.h"
#include "omap-prod-con.h"

static const int bar_width = 40;
static const int bar_speed = 8;

#define MAX_OUTPUTS 5
#define BUF_QUEUE_SIZE 15

static const bool always_create_new_bufs = false;

static struct {
	int drm_fd;
	struct omap_device *omap_dev;
	volatile struct shared_data *sdata;
	struct framebuffer bufs[MAX_OUTPUTS][BUF_QUEUE_SIZE];
	int buf_num[MAX_OUTPUTS];
} global;

static void create_omap_fb(int width, int height, int bpp, struct framebuffer *buf)
{
	struct omap_bo *bo;
	uint32_t flags;
	void *map;

	flags = 0
	| OMAP_BO_WC
	| OMAP_BO_SCANOUT
	//| OMAP_BO_TILED
	;

	if (flags & OMAP_BO_TILED) {
		flags &= ~OMAP_BO_TILED;
		if (bpp == 8) {
			flags |= OMAP_BO_TILED_8;
		} else if (bpp == 16) {
			flags |= OMAP_BO_TILED_16;
		} else if (bpp == 32) {
			flags |= OMAP_BO_TILED_32;
		}
	}

	if (flags & OMAP_BO_TILED) {
		bo = omap_bo_new_tiled(global.omap_dev, width, height, flags);
	} else {
		bo = omap_bo_new(global.omap_dev, width * height * bpp / 8, flags);
	}

	ASSERT(bo);

	map = omap_bo_map(bo);
	ASSERT(map);

	memset(buf, 0, sizeof(*buf));
	buf->width = width;
	buf->height = height;
	buf->stride = width * bpp / 8;
	buf->size = omap_bo_size(bo);
	buf->handle = omap_bo_handle(bo);
	buf->map = map;
	buf->omap_bo = bo;
}

static void destroy_omap_fb(struct framebuffer *fb)
{
	omap_bo_del(fb->omap_bo);
}

static void init_drm()
{
	const char *card = "/dev/dri/card0";

	global.drm_fd = drm_open_dev_dumb(card);

	global.omap_dev = omap_device_new(global.drm_fd);
	ASSERT(global.omap_dev);

	drmDropMaster(global.drm_fd);
}

static void uninit_drm()
{
	omap_device_del(global.omap_dev);
	close(global.drm_fd);
}

static void send_fb(int cfd, int output_id, struct framebuffer *fb)
{
	int prime_fd;
	int r;
	char buf[1];

	r = drmPrimeHandleToFD(global.drm_fd, fb->handle, DRM_CLOEXEC, &prime_fd);
	ASSERT(r == 0);

	buf[0] = output_id;

	size_t size = sock_fd_write(cfd, buf, 1, prime_fd);
	ASSERT(size == 1);

	//printf ("sent fb handle %x, output %d, prime %d\n", fb->handle, output_id, prime_fd);

	r = close(prime_fd);
	ASSERT(r == 0);
}

static void main_loop(int cfd)
{
	static int bar_xpos[10];
	int count = 0;

	fd_set fds;

	FD_ZERO(&fds);

	while (true) {
		int r;

		struct timeval tv = { .tv_usec = 1000 };

		FD_SET(0, &fds);
		FD_SET(cfd, &fds);

		volatile struct shared_data *sdata = global.sdata;

		//printf("C %d, %d\n",
		//	sdata->outputs[0].request_count,
		//	sdata->outputs[1].request_count);

		for (int i = 0; i < sdata->num_outputs; ++i) {
			volatile struct shared_output *output;

			output = &sdata->outputs[i];

			if (output->request_count > 0) {
				tv.tv_sec = 0;
				break;
			}
		}

		r = select(cfd + 1, &fds, NULL, NULL, &tv);
		ASSERT(r >= 0);

		if (FD_ISSET(0, &fds)) {
			fprintf(stderr, "exit due to user-input\n");
			return;
		}

		if (FD_ISSET(cfd, &fds)) {
			fprintf(stderr, "exit due to lost client\n");
			return;
		}

		for (int i = 0; i < sdata->num_outputs; ++i) {
			volatile struct shared_output *output;

			output = &sdata->outputs[i];

			if (output->request_count == 0)
				continue;

			output->request_count--;

			struct framebuffer *fb;

			const int width = output->width;
			const int height = output->height;
			const int bpp = 32;

			if (always_create_new_bufs) {
				fb = &global.bufs[i][0];

				create_omap_fb(width, height, bpp, fb);
			} else {

				fb = &global.bufs[i][global.buf_num[i]];
				global.buf_num[i] = (global.buf_num[i] + 1) % BUF_QUEUE_SIZE;

				drm_clear_fb(fb);
			}

			drm_draw_color_bar(fb, -1, bar_xpos[i], bar_width);

			bar_xpos[i] = (bar_xpos[i] + bar_speed) % (fb->width - bar_width);

			send_fb(cfd, output->output_id, fb);

			if (always_create_new_bufs)
				destroy_omap_fb(fb);

			//printf("sent fb %d, handle %x\n", count, fb.handle);

			//usleep(1000);

			count++;
		}
	}
}

static void open_shared_mem()
{
	int fd;
	int r;

	fd = shm_open(SHARENAME, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
	ASSERT(fd > 0);

	r = ftruncate(fd, sizeof(struct shared_data));
	ASSERT(r == 0);

	volatile struct shared_data *sdata = mmap(NULL, sizeof(struct shared_data),
		PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	ASSERT(sdata);

	global.sdata = sdata;
}

static void create_bufs()
{
	volatile struct shared_data *sdata = global.sdata;

	for (int i = 0; i < sdata->num_outputs; ++i) {
		volatile struct shared_output *output;

		output = &sdata->outputs[i];

		for (int n = 0; n < BUF_QUEUE_SIZE; ++n) {
			const int width = output->width;
			const int height = output->height;
			const int bpp = 32;

			struct framebuffer *fb;

			fb = &global.bufs[i][n];

			create_omap_fb(width, height, bpp, fb);
			drm_draw_test_pattern(fb, 0);
		}
	}
}

int main(int argc, char **argv)
{
	int r;
	struct sockaddr_un addr = { 0 };
	int sfd;

	open_shared_mem();

	init_drm();

	unlink(SOCKNAME);

	sfd = socket(AF_UNIX, SOCK_STREAM, 0);
	ASSERT(sfd >= 0);

	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, SOCKNAME);
	r = bind(sfd, (struct sockaddr *)&addr, sizeof(struct sockaddr_un));
	ASSERT(r == 0);

	r = listen(sfd, 5);
	ASSERT(r == 0);

	printf("listening...\n");

	int cfd = accept(sfd, NULL, NULL);
	ASSERT(cfd > 0);

	printf("accepted connection\n");

	if (!always_create_new_bufs)
		create_bufs();

	main_loop(cfd);

	r = close(cfd);
	ASSERT(r == 0);

	printf("done\n");

	r = close(sfd);
	ASSERT(r == 0);

	unlink(SOCKNAME);

	uninit_drm();

	return 0;
}
