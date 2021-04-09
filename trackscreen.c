#include <linux/uinput.h>
#include <linux/types.h>
#include <linux/input.h>
#include <linux/hidraw.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define USAGE \
        "Usage: %s /path/to/touchscreen\n\n" \
        "Trackscreen converts an area of your touchscreen into a mouse,\n" \
        "so you can use it as a virtual trackpad. Supply it the path to\n" \
        "the touchscreen, something like /dev/input/XX. Use evtest to\n" \
        "figure out the value of XX that corresponds to your touchscreen.\n"

bool verbose;

#define MAX_FINGERS 2

typedef struct finger {
        int prev_x; /* Previous x coordinate */
        int prev_y; /* Previous y coordinate */
        int prev_on; /* Previous on value */
        int x; /* Most recent x coordinate */
        int y; /* Most recent y coordinate */
        int on; /* Whether or not the finger is touching or not */
        struct timeval start_time; /* When the finger started touching */
        struct timeval end_time; /* When the finger lifted off */
} finger;

typedef struct trackscreen_context {
        int ts; /* Touchscreen file descriptor */
        int tp; /* Trackpad file descriptor */
        int ts_min_x; /* Minimum touchscreen X coordinate */
        int ts_min_y; /* Minimum touchscreen Y coordinate */
        int ts_max_x; /* Maximum touchscreen X coordinate */
        int ts_max_y; /* Maximum touchscreen Y coordinate */
        int tp_min_x; /* Minimum trackpad X coordinate */
        int tp_min_y; /* Minimum trackpad Y coordinate */
        int tp_max_x; /* Maximum trackpad X coordinate */
        int tp_max_y; /* Maximum trackpad Y coordinate */
        int current_slot; /* The most recently selected event finger slot. */
        finger fingers[MAX_FINGERS]; /* Finger state */
} trackscreen_context;

#define CHECK_IOCTL(args...) \
        if (ioctl(args) < 0) { \
                return __LINE__ - 1; \
        }

int setup_trackpad(int fd) {
        CHECK_IOCTL(fd, UI_SET_EVBIT, EV_KEY);
        CHECK_IOCTL(fd, UI_SET_KEYBIT, BTN_LEFT);
        CHECK_IOCTL(fd, UI_SET_KEYBIT, BTN_MIDDLE);
        CHECK_IOCTL(fd, UI_SET_KEYBIT, BTN_RIGHT);
        CHECK_IOCTL(fd, UI_SET_KEYBIT, BTN_TOUCH);
        CHECK_IOCTL(fd, UI_SET_KEYBIT, BTN_TOOL_FINGER);
        CHECK_IOCTL(fd, UI_SET_KEYBIT, BTN_TOOL_DOUBLETAP);
        CHECK_IOCTL(fd, UI_SET_KEYBIT, BTN_TOOL_TRIPLETAP);
        CHECK_IOCTL(fd, UI_SET_KEYBIT, BTN_TOOL_QUADTAP);
        CHECK_IOCTL(fd, UI_SET_EVBIT, EV_REL);
        CHECK_IOCTL(fd, UI_SET_KEYBIT, REL_X);
        CHECK_IOCTL(fd, UI_SET_KEYBIT, REL_Y);
        return 0;
}

int read_touchscreen_parameters(trackscreen_context *ctx) {
        struct input_absinfo abs;

        if (ioctl(ctx->ts, EVIOCGABS(ABS_X), &abs)) {
                perror("Cannot get touchscreen X info");
                return -1;
        }

        ctx->ts_min_x = abs.minimum;
        ctx->ts_max_x = abs.maximum;
        if (ioctl(ctx->ts, EVIOCGABS(ABS_Y), &abs)) {
                perror("Cannot get touchscreen Y info");
                return -1;
        }

        ctx->ts_min_y = abs.minimum;
        ctx->ts_max_y = abs.maximum;
        printf("Touchscreen X [%d - %d], Y [%d - %d]\n",
               ctx->ts_min_x,
               ctx->ts_max_x,
               ctx->ts_min_y,
               ctx->ts_max_y);

        return 0;
}

void compute_trackpad_bounds(trackscreen_context *ctx) {
        int height;
        int width;

        height = ctx->ts_max_y - ctx->ts_min_y;
        width = ctx->ts_max_x - ctx->ts_min_x;

        /* In a 3x3 grid, put the trackpad in the bottom middle. */
        ctx->tp_min_x = ctx->ts_min_x + (width / 3);
        ctx->tp_max_x = ctx->tp_min_x + (width / 3);
        ctx->tp_min_y = ctx->ts_min_y + (height * 2 / 3);
        ctx->tp_max_y = ctx->ts_max_y;
        printf("Trackpad X [%d - %d], Y [%d - %d]\n",
               ctx->tp_min_x,
               ctx->tp_max_x,
               ctx->tp_min_y,
               ctx->tp_max_y);

        return;
}

#define TAP_TIME_US 100000

int process_report(trackscreen_context *ctx) {
        int64_t delta;
        int delta_x;
        int delta_y;
        finger *f;
        int fingers_on;
        int i;

        fingers_on = 0;
        for (i = 0; i < MAX_FINGERS; i++) {
                f = &(ctx->fingers[i]);
                if (f->on > 0) {
                        fingers_on += 1;
                }
        }

        delta_x = 0;
        delta_y = 0;
        for (i = 0; i < MAX_FINGERS; i++) {
                f = &(ctx->fingers[i]);
                if (f->on != f->prev_on) {
                        if (!f->on) {
                                delta = (f->end_time.tv_sec -
                                         f->start_time.tv_sec) * 1000000;

                                delta += f->end_time.tv_usec -
                                         f->start_time.tv_usec;

                                printf("%d Off %ldms\n", i, delta / 1000);
                                if (delta <= TAP_TIME_US) {
                                        printf("Tap%d\n", i);
                                }
                        }
                }

                delta_x += f->x - f->prev_x;
                delta_y += f->y - f->prev_y;
                f->prev_x = f->x;
                f->prev_y = f->y;
                f->prev_on = f->on;
        }

        printf("(%d, %d)\n", delta_x, delta_y);
        return 0;
}

int convert_event(trackscreen_context *ctx) {
        struct input_event ev;
        finger *f;

        if (read(ctx->ts, &ev, sizeof(ev)) != sizeof(ev)) {
                return -1;
        }

        //printf("EV %x %x %x\n", ev.type, ev.code, ev.value);
        /* Handle a completed report coming in. */
        if ((ev.type == EV_SYN) && (ev.code == SYN_REPORT)) {
                return process_report(ctx);
        }

        /* Ignore anything but absolute events. */
        if (ev.type != EV_ABS) {
                return 0;
        }

        switch (ev.code) {
        case ABS_MT_SLOT:
                ctx->current_slot = ev.value;
                break;

        case ABS_MT_POSITION_X:
                if (ctx->current_slot < MAX_FINGERS) {
                        ctx->fingers[ctx->current_slot].x = ev.value;
                }

                break;

        case ABS_MT_POSITION_Y:
                if (ctx->current_slot < MAX_FINGERS) {
                        ctx->fingers[ctx->current_slot].y = ev.value;
                }

                break;

        case ABS_MT_TRACKING_ID:
                if (ctx->current_slot < MAX_FINGERS) {
                        f = &(ctx->fingers[ctx->current_slot]);
                        if (ev.value < 0) {
                                if (f->on > 0) {
                                        f->end_time = ev.time;
                                }

                                f->on = 0;

                        } else {
                                if (f->on == 0) {
                                        f->start_time = ev.time;
                                }

                                f->on = ev.value;
                        }
                }

                break;
        }

        return 0;
}

int main(int argc, char **argv) {
        int argument_count;
        char* device_path = NULL;
	int option;
        int status;
        trackscreen_context ctx;

        memset(&ctx, 0, sizeof(ctx));
        ctx.ts = -1;
        ctx.tp = -1;
        while (true) {
                option = getopt(argc, argv, "hv");
                if (option == -1) {
                        break;
                }

		switch (option) {
		case 'v':
			verbose = true;
			break;

                case 'h':
		default:
			printf(USAGE, argv[0]);
                        return 1;
		}
	}

	argument_count = argc - optind;
	if (argument_count != 1) {
		fprintf(stderr, "Expecting 1 argument. See -h for usage.\n");
                return 1;
        }

        device_path = argv[optind];
        ctx.tp = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
        if (ctx.tp < 0) {
                fprintf(stderr,
                        "Cannot open %s: %s\n",
                        device_path,
                        strerror(errno));

                status = 1;
                goto mainEnd;
        }

        status = setup_trackpad(ctx.tp);
        if (status != 0) {
                fprintf(stderr,
                        "Failed trackpad setup, line %d: %s\n",
                        status,
                        strerror(errno));
        }

        ctx.ts = open(device_path, O_RDONLY);
        if (ctx.ts < 0) {
                fprintf(stderr,
                        "Cannot open %s: %s\n",
                        device_path,
                        strerror(errno));

                status = 1;
                goto mainEnd;
        }

        if (read_touchscreen_parameters(&ctx)) {
                status = 1;
                goto mainEnd;
        }

        compute_trackpad_bounds(&ctx);
        while (true) {
                status = convert_event(&ctx);
                if (status != 0) {
                        goto mainEnd;
                }
        }

        status = 0;

mainEnd:
        if (ctx.ts >= 0) {
                close(ctx.ts);
        }

        if (ctx.tp >= 0) {
                close(ctx.tp);
        }

	return status;
}