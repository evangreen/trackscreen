#include <linux/uinput.h>
#include <linux/types.h>
#include <linux/input.h>
#include <linux/hidraw.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MAX_FINGERS 10
#define MAX_EVENTS_PER_REPORT 24

#define USAGE \
        "Usage: %s /path/to/touchscreen\n\n" \
        "Trackscreen converts an area of your touchscreen into a mouse,\n" \
        "so you can use it as a virtual trackpad. Supply it the path to\n" \
        "the touchscreen, something like /dev/input/XX. Use evtest to\n" \
        "figure out the value of XX that corresponds to your touchscreen.\n" \
        "Options:\n" \
        "  -d left,top,width,height -- Define the percentages along the \n" \
        "     touchpad screen where the virtual trackpad should be \n" \
        "     active. If not specified, the default is -d 33,67,33,33 \n" \
        "     for the center bottom tic-tac-toe square.\n" \
        "  -k keycode -- Create a fake keyboard and send keyboard events \n" \
        "     whenever there are touches to the side of the trackpad.\n" \
        "     See input-event-codes.h for KEY_* definitions." \
        "  -n -- Connect to the device by name instead of path Try evtest \n" \
        "     to get a list of names\n." \
        "  -h -- Show this help." \
        "  -v -- Verbose"

typedef struct trackscreen_context {
        int ts; /* Touchscreen file descriptor */
        int tp; /* Trackpad file descriptor */
        int kbd; /* Fake keyboard file descriptor */
        int keycode; /* Keyboard keycode for side palm touches. */
        int ts_min_x; /* Minimum touchscreen X coordinate */
        int ts_min_y; /* Minimum touchscreen Y coordinate */
        int ts_max_x; /* Maximum touchscreen X coordinate */
        int ts_max_y; /* Maximum touchscreen Y coordinate */
        int x_res; /* X axis resolution */
        int y_res; /* Y axis resolution */
        int tp_left_percent; /* Percent from the left trackpad should start */
        int tp_top_percent; /* Percent from the top trackpad should start */
        int tp_width_percent; /* Width of the trackpad as percent of TS. */
        int tp_height_percent; /* Height of tp as percent of touchscreen. */
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
        struct input_event input_event[MAX_EVENTS_PER_REPORT]; /* Events this report */
        int input_events; /* Valid events in this report */
        int pos_x; /* X position from last report */
        int pos_y; /* Y position from last report */
        unsigned int sidekey; /* Current sidekey state. */
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
        int fd;
        struct uinput_setup usetup;

        ctx->tp = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
        fd = ctx->tp;
        if (fd < 0) {
                perror("Cannot open /dev/uinput");
                return __LINE__;
        }

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

static int setup_keyboard(trackscreen_context *ctx) {
        int fd;
        struct uinput_setup usetup;

        ctx->kbd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
        fd = ctx->kbd;
        if (fd < 0) {
                perror("Cannot open /dev/uinput");
                return __LINE__;
        }

        CHECK_IOCTL(fd, UI_SET_EVBIT, EV_KEY);
        CHECK_IOCTL(fd, UI_SET_KEYBIT, ctx->keycode);
        memset(&usetup, 0, sizeof(usetup));
        usetup.id.bustype = BUS_VIRTUAL;
        usetup.id.vendor = 0x0650; /* sample vendor */
        usetup.id.product = 0x0912; /* sample product */
        strcpy(usetup.name, "Trackscreen Keyboard");
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
        ctx->tp_min_x = ctx->ts_min_x + (width * ctx->tp_left_percent / 100);
        ctx->tp_max_x = ctx->tp_min_x + (width * ctx->tp_width_percent / 100);
        ctx->tp_min_y = ctx->ts_min_y + (height * ctx->tp_top_percent / 100);
        ctx->tp_max_y = ctx->tp_min_y +
                        (height * ctx->tp_height_percent / 100);

        if (ctx->verbose) {
                printf("Trackpad X [%d - %d], Y [%d - %d]\n",
                       ctx->tp_min_x,
                       ctx->tp_max_x,
                       ctx->tp_min_y,
                       ctx->tp_max_y);
        }

        return;
}

static void queue_tp_event(trackscreen_context *ctx,
                           uint16_t type,
                           uint16_t code,
                           int32_t value) {

        struct input_event *ev;

        if (ctx->input_events >= MAX_EVENTS_PER_REPORT) {
                if (ctx->verbose) {
                        fprintf(stderr, "Lost event\n");
                }

                return;
        }

        ev = &(ctx->input_event[ctx->input_events]);
        ctx->input_events += 1;
        ev->type = type;
        ev->code = code;
        ev->value = value;
        return;
}

static void flush_tp_events(trackscreen_context *ctx,
                            struct input_event *report) {

        size_t size;

        size = ctx->input_events * sizeof(struct input_event);
        write(ctx->tp, &(ctx->input_event[0]), size);
        ctx->input_events = 0;
        write(ctx->tp, report, sizeof(*report));
        return;
}

static void emit_sidekey_event(trackscreen_context *ctx,
                               int32_t value) {

        struct input_event ev[2];

        ev[0].type = EV_KEY;
        ev[0].code = ctx->keycode;
        ev[0].value = value;
        ev[1].type = EV_SYN;
        ev[1].code = SYN_REPORT;
        ev[1].value = 0;
        write(ctx->kbd, ev, sizeof(ev));
        return;
}

static void check_bounds(trackscreen_context *ctx) {
        struct input_event *ev;
        int index;
        int side_touches;
        int x;
        int y;

        x = ctx->pos_x;
        y = ctx->pos_y;

        /*
         * Go through all the events in this report and pick out the X
         * and Y position if possible. Assume that ABS_* and ABS_MT_POSITION_*
         * will be the same.
         */
        ev = &(ctx->input_event[0]);
        for (index = 0; index < ctx->input_events; index += 1) {
                if (ev->type == EV_ABS) {
                        if ((ev->code == ABS_X) ||
                            (ev->code == ABS_MT_POSITION_X)) {

                                x = ev->value;

                        } else if ((ev->code == ABS_Y) ||
                                   (ev->code == ABS_MT_POSITION_Y)) {

                                y = ev->value;
                        }
                }

                ev += 1;
        }

        /* If X or Y cannot be found, do nothing. */
        if ((x == -1) || (y == -1)) {
                if (ctx->verbose) {
                        printf("Full point not found: %d %d\n", x, y);
                }

                return;
        }

        /* Figure out if there are touches on either side of the trackpad. */
        side_touches = 0;
        if ((y >= ctx->tp_min_y) && (y < ctx->tp_max_y)) {
                if ((x < ctx->tp_min_x) || (x >= ctx->tp_max_x)) {
                        side_touches = 1;
                }
        }

        if (side_touches != ctx->sidekey) {
                ctx->sidekey = side_touches;
                if (ctx->verbose) {
                        printf("Sidekey: %d\n", side_touches);
                }

                if (ctx->kbd > 0) {
                        emit_sidekey_event(ctx, side_touches);
                }
        }

        ctx->pos_x = x;
        ctx->pos_y = y;
        /* Clamp and adjust x and y. */
        if (x < ctx->tp_min_x) {
                x = ctx->tp_min_x;

        } else if (x >= ctx->tp_max_x) {
                x = ctx->tp_max_x - 1;
        }

        if (y < ctx->tp_min_y) {
                y = ctx->tp_min_y;

        } else if (y >= ctx->tp_max_y) {
                y = ctx->tp_max_y - 1;
        }

        x -= ctx->tp_min_x;
        y -= ctx->tp_min_y;

        /* Replace the positions */
        ev = &(ctx->input_event[0]);
        for (index = 0; index < ctx->input_events; index += 1) {
                if (ev->type == EV_ABS) {
                        if ((ev->code == ABS_X) ||
                            (ev->code == ABS_MT_POSITION_X)) {

                                ev->value = x;

                        } else if ((ev->code == ABS_Y) ||
                                   (ev->code == ABS_MT_POSITION_Y)) {

                                ev->value = y;
                        }
                }

                ev += 1;
        }

        return;
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
        queue_tp_event(ctx, EV_KEY, code, value);
        return;
}

static int handle_event(trackscreen_context *ctx) {
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

                check_bounds(ctx);

                /*
                 * If there are no more fingers down, release the
                 * sidekey key as well.
                 */
                if ((finger_count == 0) && (ctx->sidekey != 0)) {
                        ctx->sidekey = 0;
                        if (ctx->keycode > 0) {
                                emit_sidekey_event(ctx, 0);
                        }
                }

                flush_tp_events(ctx, &ev);
                return 0;
        }

        /* Send anything but EV_ABS down directly */
        if (ev.type != EV_ABS) {
                queue_tp_event(ctx, ev.type, ev.code, ev.value);
                return 0;
        }

        switch (ev.code) {
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

        queue_tp_event(ctx, ev.type, ev.code, ev.value);
        return 0;
}

static int read_trackpad_dimensions(trackscreen_context *ctx,
                                    char *arg) {

        int items;

        items = sscanf(arg,
                       "%d,%d,%d,%d",
                       &(ctx->tp_left_percent),
                       &(ctx->tp_top_percent),
                       &(ctx->tp_width_percent),
                       &(ctx->tp_height_percent));

        if (items != 4) {
                fprintf(stderr, "Scanned only %d items\n", items);
                return -1;
        }

        if ((ctx->tp_left_percent < 0) || (ctx->tp_left_percent >= 100) ||
            (ctx->tp_top_percent < 0) || (ctx->tp_top_percent >= 100)) {

                fprintf(stderr, "Top/left percents must be between 0-100.\n");
                return -1;
        }

        if ((ctx->tp_width_percent <= 0) || (ctx->tp_width_percent > 100) ||
            (ctx->tp_left_percent + ctx->tp_width_percent > 100) ||
            (ctx->tp_height_percent <= 0) || (ctx->tp_height_percent > 100) ||
            (ctx->tp_top_percent + ctx->tp_height_percent > 100)) {

                fprintf(stderr,
                        "Width/height must be between 1-100, and must not "
                        "add to >100 when offset by left/top.");

                return -1;
        }

        return 0;
}

static int has_abs_bit(int fd, int absbit) {
        unsigned char absbits[(ABS_MAX / 8) + 1];
        int rc;

        memset(absbits, 0, sizeof(absbits));
        rc = ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);
        if (rc < 0) {
                return 0;
        }

        if ((absbits[absbit / 8] & (1 << (absbit % 8))) != 0) {
                return 1;
        }

        return 0;
}

static int find_input_by_name(trackscreen_context *ctx,
                              const char *arg) {

        char devicename[256];
        DIR *dir;
        struct dirent *entry;
        unsigned long evbit;
        int fd;
        char fullpath[268];
        int rc;

        dir = opendir("/dev/input");
        if (dir == NULL) {
                perror("Cannot open /dev/input");
                return -1;
        }

        fd = -1;
        while (1) {
                entry = readdir(dir);
                if (entry == NULL) {
                        break;
                }

                if (strncmp(entry->d_name, "event", 5) != 0) {
                        if (ctx->verbose) {
                                printf("Skipping %s\n", entry->d_name);
                        }

                        continue;
                }

                snprintf(fullpath,
                         sizeof(fullpath),
                         "/dev/input/%s",
                         entry->d_name);

                fullpath[sizeof(fullpath) - 1] = '\0';
                fd = open(fullpath, O_RDONLY);
                if (fd < 0) {
                        if (ctx->verbose) {
                                fprintf(stderr,
                                        "Cannot open %s: %s\n",
                                        fullpath,
                                        strerror(errno));
                        }

                        continue;
                }

                rc = ioctl(fd, EVIOCGNAME(sizeof(devicename)), devicename);
                if (rc < 0) {
                        if (ctx->verbose) {
                                fprintf(stderr,
                                        "Could not get name for %s: %s\n",
                                        fullpath,
                                        strerror(errno));
                        }

                        close(fd);
                        continue;
                }

                if (strcmp(devicename, arg) != 0) {
                        if (ctx->verbose) {
                                fprintf(stderr,
                                        "Skip '%s' != '%s'\n",
                                        devicename,
                                        arg);
                        }

                        close(fd);
                        continue;
                }

                /* See if it has EV_ABS */
                evbit = 0;
                ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), &evbit);
                if ((evbit & (1 << EV_ABS)) == 0) {
                        if (ctx->verbose) {
                                fprintf(stderr,
                                        "Skip %s, missing EV_ABS\n",
                                        fullpath);
                        }

                        close(fd);
                        continue;
                }

                /* See if it has ABS_MT_POSITION_Y */
                if (!has_abs_bit(fd, ABS_MT_POSITION_Y)) {
                        if (ctx->verbose) {
                                fprintf(stderr,
                                        "Skip %s, missing ABS_MT_POSITION_Y\n",
                                        fullpath);
                        }

                        close(fd);
                        continue;
                }

                if (ctx->verbose) {
                        printf("Found %s matching '%s'\n", fullpath, arg);
                }

                goto end;
        }

        errno = ENOENT;
        fd = -1;

end:
        closedir(dir);
        return fd;
}

int main(int argc, char **argv) {
        int argument_count;
        trackscreen_context ctx;
        char *device_path = NULL;
        char *end;
        int finger;
        int option;
        int status;
        int use_name;

        use_name = 0;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ts = -1;
        ctx.tp = -1;
        ctx.kbd = -1;
        ctx.keycode = -1;
        ctx.scale = 1.0;
        /* Put trackpad in the bottom center tic-tac-toe square. */
        ctx.tp_left_percent = 33;
        ctx.tp_top_percent = 67;
        ctx.tp_width_percent = 33;
        ctx.tp_height_percent = 33;
        for (finger = 0; finger < MAX_FINGERS; finger += 1) {
                ctx.fingers[finger] = -1;
        }

        while (true) {
                option = getopt(argc, argv, "d:hk:ns:v");
                if (option == -1) {
                        break;
                }

                switch (option) {
                case 'd':
                        status = read_trackpad_dimensions(&ctx, optarg);
                        if (status != 0) {
                                fprintf(stderr, "Invalid dimensions\n");
                                return 1;
                        }

                        break;

                case 'k':
                        ctx.keycode = atoi(optarg);
                        if (ctx.keycode <= 0) {
                                fprintf(stderr, "Invalid keycode\n");
                                return 1;
                        }

                        break;

                case 'n':
                        use_name = 1;
                        break;

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
        if (use_name != 0) {
                ctx.ts = find_input_by_name(&ctx, device_path);

        } else {
                ctx.ts = open(device_path, O_RDONLY);
        }

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
        status = setup_trackpad(&ctx);
        if (status != 0) {
                fprintf(stderr,
                        "Failed trackpad setup, line %d: %s\n",
                        status,
                        strerror(errno));

                goto mainEnd;
        }

        if (ctx.keycode > 0) {
                status = setup_keyboard(&ctx);
                if (status != 0) {
                        fprintf(stderr,
                                "Failed keyboard setup, line %d: %s\n",
                                status,
                                strerror(errno));

                        goto mainEnd;
                }
        }
        while (true) {
                status = handle_event(&ctx);
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

        if (ctx.kbd >= 0) {
                close(ctx.kbd);
        }

        return status;
}
