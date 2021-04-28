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

#define MAX_FINGERS 10

#define USAGE \
        "Usage: %s /path/to/touchscreen\n\n" \
        "Trackscreen converts an area of your touchscreen into a mouse,\n" \
        "so you can use it as a virtual trackpad. Supply it the path to\n" \
        "the touchscreen, something like /dev/input/XX. Use evtest to\n" \
        "figure out the value of XX that corresponds to your touchscreen.\n" \
        "Options:\n" \
        "  -s 1.0 -- Set a scaling factor on touchpad movement." \
        "  -h -- Show this help." \
        "  -v -- Verbose"

typedef struct trackscreen_context {
        int ts; /* Touchscreen file descriptor */
        int tp; /* Trackpad file descriptor */
        int ts_min_x; /* Minimum touchscreen X coordinate */
        int ts_min_y; /* Minimum touchscreen Y coordinate */
        int ts_max_x; /* Maximum touchscreen X coordinate */
        int ts_max_y; /* Maximum touchscreen Y coordinate */
        int x_res; /* X axis resolution */
        int y_res; /* Y axis resolution */
        int tp_min_x; /* Minimum trackpad X coordinate */
        int tp_min_y; /* Minimum trackpad Y coordinate */
        int tp_max_x; /* Maximum trackpad X coordinate */
        int tp_max_y; /* Maximum trackpad Y coordinate */
        int pressure_min; /* Minimum pressure */
        int pressure_max; /* Maximum pressure */
        int finger_count; /* Number of slots with a valid tracking ID */
        int fingers[MAX_FINGERS]; /* Tracking ID of all the fingers down */
        unsigned int slot; /* currently selected slot */
        double scale; /* touchpad_delta * scale = trackpad_delta */
        int verbose; /* Print stuff! */
} trackscreen_context;

#define CHECK_IOCTL(args...) \
        if (ioctl(args) < 0) { \
                return __LINE__ - 1; \
        }

static int setup_axis(trackscreen_context *ctx,
                      int axis_code,
                      int maximum,
		      int resolution) {

	struct input_absinfo info = {
		.value = 0,
		.minimum = 0,
		.maximum = maximum,
		.fuzz = 0,
		.flat = 0,
		.resolution = resolution,
	};

	struct uinput_abs_setup setup = {
		.code = axis_code,
		.absinfo = info,
	};

	CHECK_IOCTL(ctx->tp, UI_SET_ABSBIT, axis_code);
	CHECK_IOCTL(ctx->tp, UI_ABS_SETUP, &setup);
        return 0;
}

static int setup_pressure_axis(trackscreen_context *ctx,
                               int axis_code,
                               int minimum,
                               int maximum) {

	struct input_absinfo info = {
		.value = 0,
		.minimum = minimum,
		.maximum = maximum,
		.fuzz = 0,
		.flat = 0,
		.resolution = 0,
	};
	struct uinput_abs_setup setup = {
		.code = axis_code,
		.absinfo = info,
	};
	CHECK_IOCTL(ctx->tp, UI_SET_ABSBIT, axis_code);
	CHECK_IOCTL(ctx->tp, UI_ABS_SETUP, &setup);
        return 0;
}

static int setup_trackpad(trackscreen_context *ctx) {
        int fd = ctx->tp;
        struct uinput_setup usetup;

        CHECK_IOCTL(fd, UI_SET_EVBIT, EV_KEY);
        CHECK_IOCTL(fd, UI_SET_KEYBIT, BTN_TOOL_FINGER);
        CHECK_IOCTL(fd, UI_SET_KEYBIT, BTN_TOUCH);
        CHECK_IOCTL(fd, UI_SET_KEYBIT, BTN_TOOL_QUINTTAP);
        CHECK_IOCTL(fd, UI_SET_KEYBIT, BTN_TOOL_DOUBLETAP);
        CHECK_IOCTL(fd, UI_SET_KEYBIT, BTN_TOOL_TRIPLETAP);
        CHECK_IOCTL(fd, UI_SET_KEYBIT, BTN_TOOL_QUADTAP);
        CHECK_IOCTL(fd, UI_SET_EVBIT, EV_ABS);
        CHECK_IOCTL(fd, UI_SET_ABSBIT, ABS_X);
        CHECK_IOCTL(fd, UI_SET_ABSBIT, ABS_Y);
        CHECK_IOCTL(fd, UI_SET_ABSBIT, ABS_PRESSURE);
        CHECK_IOCTL(fd, UI_SET_ABSBIT, ABS_MT_SLOT);
        CHECK_IOCTL(fd, UI_SET_ABSBIT, ABS_MT_TOUCH_MAJOR);
        CHECK_IOCTL(fd, UI_SET_ABSBIT, ABS_MT_TOUCH_MINOR);
        CHECK_IOCTL(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
        CHECK_IOCTL(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
        CHECK_IOCTL(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
        CHECK_IOCTL(fd, UI_SET_ABSBIT, ABS_MT_PRESSURE);
        CHECK_IOCTL(fd, UI_SET_PROPBIT, INPUT_PROP_POINTER);
        CHECK_IOCTL(fd, UI_SET_PROPBIT, INPUT_PROP_BUTTONPAD);
	setup_axis(ctx, ABS_X, ctx->tp_max_x - ctx->tp_min_x, ctx->x_res);
	setup_axis(ctx, ABS_Y, ctx->tp_max_y - ctx->tp_min_y, ctx->y_res);
	setup_pressure_axis(ctx,
                            ABS_PRESSURE,
                            ctx->pressure_min,
                            ctx->pressure_max);

	setup_axis(ctx,
                   ABS_MT_POSITION_X,
                   ctx->tp_max_x - ctx->tp_min_x,
                   ctx->x_res);

	setup_axis(ctx,
                   ABS_MT_POSITION_Y,
                   ctx->tp_max_y - ctx->tp_min_y,
                   ctx->y_res);

	setup_pressure_axis(ctx,
                            ABS_MT_PRESSURE,
                            ctx->pressure_min,
                            ctx->pressure_max);

	setup_axis(ctx, ABS_MT_SLOT, 9, 0);
        memset(&usetup, 0, sizeof(usetup));
        usetup.id.bustype = BUS_VIRTUAL;
        usetup.id.vendor = 0x0650; /* sample vendor */
        usetup.id.product = 0x0911; /* sample product */
        strcpy(usetup.name, "Trackscreen");

        CHECK_IOCTL(fd, UI_DEV_SETUP, &usetup);
        CHECK_IOCTL(fd, UI_DEV_CREATE);
        return 0;
}

static int read_touchscreen_parameters(trackscreen_context *ctx) {
        struct input_absinfo abs;

        if (ioctl(ctx->ts, EVIOCGABS(ABS_X), &abs)) {
                perror("Cannot get touchscreen X info");
                return -1;
        }

        ctx->ts_min_x = abs.minimum;
        ctx->ts_max_x = abs.maximum;
        ctx->x_res =  abs.resolution;
        if (ioctl(ctx->ts, EVIOCGABS(ABS_Y), &abs)) {
                perror("Cannot get touchscreen Y info");
                return -1;
        }

        ctx->ts_min_y = abs.minimum;
        ctx->ts_max_y = abs.maximum;
        ctx->y_res = abs.resolution;
        if (ioctl(ctx->ts, EVIOCGABS(ABS_PRESSURE), &abs)) {
                perror("Cannot get touchscreen X info");
                return -1;
        }

        ctx->pressure_min = abs.minimum;
        ctx->pressure_max = abs.maximum;
        if (ctx->verbose) {
                printf("Touchscreen X [%d - %d], Y [%d - %d], "
                       "Pressure [%d - %d]\n",
                       ctx->ts_min_x,
                       ctx->ts_max_x,
                       ctx->ts_min_y,
                       ctx->ts_max_y,
                       ctx->pressure_min,
                       ctx->pressure_max);
        }

        return 0;
}

static void compute_trackpad_bounds(trackscreen_context *ctx) {
        int height;
        int width;

        height = ctx->ts_max_y - ctx->ts_min_y;
        width = ctx->ts_max_x - ctx->ts_min_x;

        /* In a 3x3 grid, put the trackpad in the bottom middle. */
        ctx->tp_min_x = ctx->ts_min_x + (width / 3);
        ctx->tp_max_x = ctx->tp_min_x + (width / 3);
        ctx->tp_min_y = ctx->ts_min_y + (height * 2 / 3);
        ctx->tp_max_y = ctx->ts_max_y;
        if (ctx->verbose) {
                printf("Trackpad X [%d - %d], Y [%d - %d]\n",
                       ctx->tp_min_x,
                       ctx->tp_max_x,
                       ctx->tp_min_y,
                       ctx->tp_max_y);
        }

        return;
}

static void emit_tp_event(trackscreen_context *ctx,
                          uint16_t type,
                          uint16_t code,
                          int32_t value) {

        struct input_event ev;

        ev.type = type;
        ev.code = code;
        ev.value = value;
        write(ctx->tp, &ev, sizeof(ev));
}

static const uint16_t finger_tap_codes[6] = {
        0,
        BTN_TOOL_FINGER,
        BTN_TOOL_DOUBLETAP,
        BTN_TOOL_TRIPLETAP,
        BTN_TOOL_QUADTAP,
        BTN_TOOL_QUINTTAP
};

static void emit_multitap(trackscreen_context *ctx,
                          int finger_count,
                          int32_t value) {

        uint16_t code;

        if ((finger_count <= 0) || (finger_count > 5)) {
                return;
        }

        if (ctx->verbose) {
                printf("Finger %d: %d\n", finger_count, value);
        }

        code = finger_tap_codes[finger_count];
        emit_tp_event(ctx, EV_KEY, code, value);
        return;
}

static int convert_event(trackscreen_context *ctx) {
        int finger_count;
        int i;
        struct input_event ev;

        if (read(ctx->ts, &ev, sizeof(ev)) != sizeof(ev)) {
                return -1;
        }

        if (ctx->verbose) {
                printf("RECV %x %x %x\n", ev.type, ev.code, ev.value);
        }

        if ((ev.type == EV_SYN) && (ev.code == SYN_REPORT)) {
                finger_count = 0;
                for (i = 0; i < MAX_FINGERS; i++) {
                        if (ctx->fingers[i] > 0) {
                                finger_count += 1;
                        }
                }

                if (finger_count != ctx->finger_count) {
                        emit_multitap(ctx, ctx->finger_count, 0);
                        emit_multitap(ctx, finger_count, 1);
                        ctx->finger_count = finger_count;
                }
        }

        /* Send anything but EV_ABS down directly */
        if (ev.type != EV_ABS) {
                emit_tp_event(ctx, ev.type, ev.code, ev.value);
                return 0;
        }

        switch (ev.code) {
        case ABS_X:
        case ABS_MT_POSITION_X:
                if (ev.value < ctx->tp_min_x) {
                        ev.value = ctx->tp_min_x;
                }

                if (ev.value >= ctx->tp_max_x) {
                        ev.value = ctx->tp_max_x - 1;
                }

                ev.value -= ctx->tp_min_x;
                break;

        case ABS_Y:
        case ABS_MT_POSITION_Y:
                if (ev.value < ctx->tp_min_y) {
                        ev.value = ctx->tp_min_y;
                }

                if (ev.value >= ctx->tp_max_y) {
                        ev.value = ctx->tp_max_y - 1;
                }

                ev.value -= ctx->tp_min_y;
                break;

        case ABS_MT_SLOT:
                ctx->slot = ev.value;
                break;

        case ABS_MT_TRACKING_ID:
                if (ctx->slot < MAX_FINGERS) {
                        ctx->fingers[ctx->slot] = ev.value;
                }

                break;

        default:
                break;
        }

        emit_tp_event(ctx, ev.type, ev.code, ev.value);
        return 0;
}

int main(int argc, char **argv) {
        int argument_count;
        char *device_path = NULL;
        char *end;
        int finger;
	int option;
        int status;
        trackscreen_context ctx;

        memset(&ctx, 0, sizeof(ctx));
        ctx.ts = -1;
        ctx.tp = -1;
        ctx.scale = 1.0;
        for (finger = 0; finger < MAX_FINGERS; finger += 1) {
                ctx.fingers[finger] = -1;
        }

        while (true) {
                option = getopt(argc, argv, "hs:v");
                if (option == -1) {
                        break;
                }

		switch (option) {
                case 's':
                        ctx.scale = strtod(optarg, &end);
                        if ((end == optarg) || (*end != '\0')) {
                                fprintf(stderr, "Invalid scale\n");
                                return 1;
                        }

                        break;

		case 'v':
			ctx.verbose = true;
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
        ctx.ts = open(device_path, O_RDONLY);
        if (ctx.ts < 0) {
                fprintf(stderr,
                        "Cannot open %s: %s\n",
                        device_path,
                        strerror(errno));

                status = 1;
                goto mainEnd;
        }

        if (ioctl(ctx.ts, EVIOCGRAB, 1) != 0) {
                fprintf(stderr,
                        "Warning: failed to grab %s exclusively.\n",
                        device_path);
        }

        if (read_touchscreen_parameters(&ctx)) {
                status = 1;
                goto mainEnd;
        }

        compute_trackpad_bounds(&ctx);
        ctx.tp = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
        if (ctx.tp < 0) {
                fprintf(stderr,
                        "Cannot open %s: %s\n",
                        device_path,
                        strerror(errno));

                status = 1;
                goto mainEnd;
        }

        status = setup_trackpad(&ctx);
        if (status != 0) {
                fprintf(stderr,
                        "Failed trackpad setup, line %d: %s\n",
                        status,
                        strerror(errno));
        }

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