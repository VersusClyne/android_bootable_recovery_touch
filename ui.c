/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <linux/spi/cpcap.h>

#include "common.h"
#include <cutils/android_reboot.h>
#include "minui/minui.h"
#include "recovery_ui.h"

extern int __system(const char *command);

#ifdef BOARD_HAS_NO_SELECT_BUTTON
static int gShowBackButton = 1;
#else
static int gShowBackButton = 0;
#endif

#define MAX_COLS 96
#define MAX_ROWS 32

#define MENU_MAX_COLS 64
#define MENU_MAX_ROWS 250

#define MIN_LOG_ROWS 3

#define CHAR_WIDTH BOARD_RECOVERY_CHAR_WIDTH
#define CHAR_HEIGHT BOARD_RECOVERY_CHAR_HEIGHT
#define CHAR_SPACE 48

#define UI_WAIT_KEY_TIMEOUT_SEC    3600

UIParameters ui_parameters = {
    6,       // indeterminate progress bar frames
    25,      // fps
    7,       // installation icon frames (0 == static image)
    23, 83, // installation icon overlay offset
};

static pthread_mutex_t gUpdateMutex = PTHREAD_MUTEX_INITIALIZER;
static gr_surface gBackgroundIcon[NUM_BACKGROUND_ICONS];
static gr_surface *gInstallationOverlay;
static gr_surface *gProgressBarIndeterminate;
static gr_surface gProgressBarEmpty;
static gr_surface gProgressBarFill;
static int ui_has_initialized = 0;
static int ui_log_stdout = 1;

static const struct { gr_surface* surface; const char *name; } BITMAPS[] = {
    { &gBackgroundIcon[BACKGROUND_ICON_INSTALLING], "icon_installing" },
    { &gBackgroundIcon[BACKGROUND_ICON_ERROR],      "icon_error" },
    { &gBackgroundIcon[BACKGROUND_ICON_CLOCKWORK],  "icon_clockwork" },
    { &gBackgroundIcon[BACKGROUND_ICON_FIRMWARE_INSTALLING], "icon_firmware_install" },
    { &gBackgroundIcon[BACKGROUND_ICON_FIRMWARE_ERROR], "icon_firmware_error" },
    { &gBackgroundIcon[BACKGROUND_ICON_ZIP], "icon_zip" },
    { &gBackgroundIcon[BACKGROUND_ICON_BACKUP], "icon_backup" },
    { &gBackgroundIcon[BACKGROUND_ICON_WIPE], "icon_wipe" },
    { &gBackgroundIcon[BACKGROUND_ICON_SAFETY], "icon_safety" },
    { &gBackgroundIcon[BACKGROUND_ICON_POWER], "icon_power" },
    { &gBackgroundIcon[BACKGROUND_ICON_ADVANCE], "icon_advance" },
    { &gBackgroundIcon[BACKGROUND_ICON_ABOUT], "icon_about" },
    { &gBackgroundIcon[BACKGROUND_ICON_USB], "icon_usb" },
    { &gBackgroundIcon[BACKGROUND_ICON_MOUNT], "icon_mount" },
    { &gProgressBarEmpty,               "progress_empty" },
    { &gProgressBarFill,                "progress_fill" },
    { NULL,                             NULL },
};

static int gInstallingFrame = 0;

static enum ProgressBarType {
    PROGRESSBAR_TYPE_NONE,
    PROGRESSBAR_TYPE_INDETERMINATE,
    PROGRESSBAR_TYPE_NORMAL,
} gProgressBarType = PROGRESSBAR_TYPE_NONE;

// Progress bar scope of current operation
static float gProgressScopeStart = 0, gProgressScopeSize = 0, gProgress = 0;
static double gProgressScopeTime, gProgressScopeDuration;

// Set to 1 when both graphics pages are the same (except for the progress bar)
static int gPagesIdentical = 0;

// Log text overlay, displayed when a magic key is pressed
static char text[MAX_ROWS][MAX_COLS];
static int text_cols = 0, text_rows = 0;
static int text_col = 0, text_row = 0, text_top = 0;
static int show_text = 0;
static int show_text_ever = 0;   // has show_text ever been 1?

static char menu[MENU_MAX_ROWS][MENU_MAX_COLS];
static int show_menu = 0;
static int menu_top = 0, menu_items = 0, menu_sel = 0;
static int menu_show_start = 0;             // this is line which menu display is starting at
static int max_menu_rows;

// Key event input queue
static pthread_mutex_t key_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t key_queue_cond = PTHREAD_COND_INITIALIZER;
static int key_queue[256], key_queue_len = 0;
static volatile char key_pressed[KEY_MAX + 1];

// Return the current time as a double (including fractions of a second).
static double now() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
}

// Draw the given frame over the installation overlay animation.  The
// background is not cleared or draw with the base icon first; we
// assume that the frame already contains some other frame of the
// animation.  Does nothing if no overlay animation is defined.
// Should only be called with gUpdateMutex locked.
static void draw_install_overlay_locked(int frame) {
    if (gInstallationOverlay == NULL) return;
    gr_surface surface = gInstallationOverlay[frame];
    int iconWidth = gr_get_width(surface);
    int iconHeight = gr_get_height(surface);
    gr_blit(surface, 0, 0, iconWidth, iconHeight,
            ui_parameters.install_overlay_offset_x,
            ui_parameters.install_overlay_offset_y);
}

// Clear the screen and draw the currently selected background icon (if any).
// Should only be called with gUpdateMutex locked.
static void draw_background_locked(int icon)
{
    gPagesIdentical = 0;
    gr_color(0, 0, 0, 255);
    gr_fill(0, 0, gr_fb_width(), gr_fb_height());

    if (icon) {
        gr_surface surface = gBackgroundIcon[icon];
        int iconWidth = gr_get_width(surface);
        int iconHeight = gr_get_height(surface);
        int iconX = (gr_fb_width() - iconWidth) / 2;
        int iconY = (gr_fb_height() - iconHeight) / 2;
        gr_blit(surface, 0, 0, iconWidth, iconHeight, iconX, iconY);
        if (icon == BACKGROUND_ICON_INSTALLING) {
            draw_install_overlay_locked(gInstallingFrame);
        }
    }
}

// Draw the progress bar (if any) on the screen.  Does not flip pages.
// Should only be called with gUpdateMutex locked.
static void draw_progress_locked()
{
    if (gCurrentIcon == BACKGROUND_ICON_INSTALLING) {
		gInstallingFrame = (gInstallingFrame + 1) % ui_parameters.installing_frames;
        draw_install_overlay_locked(gInstallingFrame);
    }

    if (gProgressBarType != PROGRESSBAR_TYPE_NONE) {
        int iconHeight = gr_get_height(gBackgroundIcon[BACKGROUND_ICON_INSTALLING]);
        int width = gr_get_width(gProgressBarEmpty);
        int height = gr_get_height(gProgressBarEmpty);

        int dx = (gr_fb_width() - width)/2;
        int dy = (3*gr_fb_height() + iconHeight - 2*height)/4;

        // Erase behind the progress bar (in case this was a progress-only update)
        gr_color(0, 0, 0, 255);
        gr_fill(dx, dy, width, height);

        if (gProgressBarType == PROGRESSBAR_TYPE_NORMAL) {
            float progress = gProgressScopeStart + gProgress * gProgressScopeSize;
            int pos = (int) (progress * width);

            if (pos > 0) {
                gr_blit(gProgressBarFill, 0, 0, pos, height, dx, dy);
            }
            if (pos < width-1) {
                gr_blit(gProgressBarEmpty, pos, 0, width-pos, height, dx+pos, dy);
            }
        }

        if (gProgressBarType == PROGRESSBAR_TYPE_INDETERMINATE) {
            static int frame = 0;
            gr_blit(gProgressBarIndeterminate[frame], 0, 0, width, height, dx, dy);
            frame = (frame + 1) % ui_parameters.indeterminate_frames;
        }
    }
}

static void draw_text_line(int row, const char* t, int space) {
  if (t[0] != '\0') {
    gr_text(0, (row+1)*space-1, t);
  }
}

#define GREEN 85, 170, 56, 255
#define WHITE 200, 200, 200, 255
#define YELLOW 255, 255, 0, 255
#define YELLOW2 220, 220, 0, 255
#define BLACK 0, 0, 0, 255
#define GREY 100, 100, 100, 255
#define GREY2 85, 85, 85, 255
#define GREY3 70, 70, 70, 255

// Redraw everything on the screen.  Does not flip pages.
// Should only be called with gUpdateMutex locked.
static void draw_screen_locked(void)
{
	bool fm = false;
    if (!ui_has_initialized) return;

	draw_background_locked(gCurrentIcon);
    draw_progress_locked();

    if (show_text) {
        int total_rows = gr_fb_height() / CHAR_HEIGHT;
        int i = 0;
        int j = 0;
        int offset = 0;         // offset of separating bar under menus
        int row = 0;            // current row that we are drawing on
        if (show_menu) {
			gr_color(0, 0, 0, 160);
	        gr_fill(0, 0, gr_fb_width(), gr_fb_height());
            gr_color(YELLOW);

            for (i = 0; i < menu_top; ++i) {
				draw_text_line(i+1, menu[i],CHAR_HEIGHT);
				row+=2;
			}

			if (strncasecmp(menu[1], "choose", 6) == 0) { fm = true; }
			//highlight
            gr_color(GREEN);
            gr_fill(0, ((menu_top + menu_sel - menu_show_start) * CHAR_SPACE)+6, gr_fb_width(), ((menu_top + menu_sel - menu_show_start + 1)*CHAR_SPACE+CHAR_HEIGHT)-6);

            if (menu_items - menu_show_start + menu_top >= max_menu_rows)
                j = max_menu_rows - menu_top;
            else
                j = menu_items - menu_show_start;

            for (i = menu_show_start + menu_top; i < (menu_show_start + menu_top + j); ++i) {
                if (i == menu_top + menu_sel) {
                    gr_color(WHITE);
                    draw_text_line(i - menu_show_start , menu[i],CHAR_SPACE);
                } else {
                    gr_color(GREEN);
                    draw_text_line(i - menu_show_start, menu[i],CHAR_SPACE);
                }
				if ((fm == true) && (strncasecmp(menu[i], " +++", 4) != 0) && (strlen(menu[i]) > 3)) {
					if (strncasecmp(menu[i] + (strlen(menu[i])-4), ".zip", 4) == 0) {
						gr_color(WHITE);
						gr_fill(10, (row/2)*CHAR_SPACE+CHAR_HEIGHT+3, 25, (row/2)*CHAR_SPACE+CHAR_HEIGHT+22);
						gr_color(BLACK);
						gr_fill(10, (row/2)*CHAR_SPACE+CHAR_HEIGHT+3, 12, (row/2)*CHAR_SPACE+CHAR_HEIGHT+5);
					} else {
						gr_color(YELLOW2);	
						gr_fill(5, (row/2)*CHAR_SPACE+CHAR_HEIGHT+3, 30, (row/2)*CHAR_SPACE+CHAR_HEIGHT+22);
						gr_color(GREY);
						gr_fill(15, (row/2)*CHAR_SPACE+CHAR_HEIGHT+3, 30, (row/2)*CHAR_SPACE+CHAR_HEIGHT+5);
					}
				}
                row +=2;
                if (row/2 >= max_menu_rows) {
                    break;
				}
            }
            if (menu_items <= max_menu_rows)
                offset = 1;

			//Hline
			gr_color(GREEN);
			gr_fill(0, (menu_top*CHAR_SPACE)+4, gr_fb_width(), (menu_top*CHAR_SPACE)+6);
            gr_fill(0, (row/2-offset)*CHAR_SPACE+CHAR_HEIGHT-6, gr_fb_width(), (row/2-offset)*CHAR_SPACE+CHAR_HEIGHT-4);
        }

        gr_color(WHITE);
        int cur_row = text_row;
        int available_rows = total_rows - row - 1;
        int start_row = row + 1;
        if (available_rows < MAX_ROWS)
            cur_row = (cur_row + (MAX_ROWS - available_rows)) % MAX_ROWS;
        else
            start_row = total_rows - MAX_ROWS;

        int r;
        for (r = 0; r < (available_rows < MAX_ROWS ? available_rows : MAX_ROWS) ; r++) {
            draw_text_line(start_row + r, text[(cur_row + r) % MAX_ROWS],CHAR_HEIGHT);
        }

		gr_color(BLACK);
		gr_fill(0, 0, gr_fb_width(), 24);

		//Text
		char clock[MAX_COLS];
		gr_color(GREEN);
		sprintf(clock,"TOUCH:  | MODE:%s | BATT:%d mV", recovery_mode, battlevel);
		draw_text_line(0, clock,CHAR_HEIGHT);

		//Touch icon
		if (touch_select) gr_color(0, 255, 0, 255);
		else gr_color(255, 0, 0, 255);
		gr_fill(90, 2,110, 22);

    }
}

// Redraw everything on the screen and flip the screen (make it visible).
// Should only be called with gUpdateMutex locked.
static void update_screen_locked(void)
{
    if (!ui_has_initialized) return;
    draw_screen_locked();
    gr_flip();
}

// Updates only the progress bar, if possible, otherwise redraws the screen.
// Should only be called with gUpdateMutex locked.
static void update_progress_locked(void)
{
    if (!ui_has_initialized) return;
    if (show_text || !gPagesIdentical) {
        draw_screen_locked();    // Must redraw the whole screen
        gPagesIdentical = 1;
    } else {
        draw_progress_locked();  // Draw only the progress bar and overlays
    }
    gr_flip();
}

// Keeps the progress bar updated, even when the process is otherwise busy.
static void *progress_thread(void *cookie)
{
    double interval = 1.0 / ui_parameters.update_fps;
    for (;;) {
        double start = now();
        pthread_mutex_lock(&gUpdateMutex);

        int redraw = 0;

        // update the installation animation, if active
        // skip this if we have a text overlay (too expensive to update)
        if (gCurrentIcon == BACKGROUND_ICON_INSTALLING &&
            ui_parameters.installing_frames > 0 &&
            !show_text) {
            gInstallingFrame =
                (gInstallingFrame + 1) % ui_parameters.installing_frames;
            redraw = 1;
        }

        // update the progress bar animation, if active
        // skip this if we have a text overlay (too expensive to update)
        if (gProgressBarType == PROGRESSBAR_TYPE_INDETERMINATE && !show_text) {
            redraw = 1;
        }

        // move the progress bar forward on timed intervals, if configured
        int duration = gProgressScopeDuration;
        if (gProgressBarType == PROGRESSBAR_TYPE_NORMAL && duration > 0) {
            double elapsed = now() - gProgressScopeTime;
            float progress = 1.0 * elapsed / duration;
            if (progress > 1.0) progress = 1.0;
            if (progress > gProgress) {
                gProgress = progress;
                redraw = 1;
            }
        }

        if (redraw) update_progress_locked();

        pthread_mutex_unlock(&gUpdateMutex);
        double end = now();
        // minimum of 20ms delay between frames
        double delay = interval - (end-start);
        if (delay < 0.02) delay = 0.02;
        usleep((long)(delay * 1000000));
    }
    return NULL;
}

static int rel_sum = 0;
static int in_touch = 0;
static int touch_x = 0;
static int touch_y = 0;
static int old_x = 0;
static int old_y = 0;
static int diff_x = 0;
static int diff_y = 0;

static void reset_gestures() {
    diff_x = 0;
    diff_y = 0;
    old_x = 0;
    old_y = 0;
    touch_x = 0;
    touch_y = 0;
}

static int input_callback(int fd, short revents, void *data)
{
    struct input_event ev;
    int ret;
    int fake_key = 0;

    ret = ev_get_input(fd, revents, &ev);
    if (ret)
        return -1;

    if (ev.type == EV_SYN) {
        return 0;
    } else if (ev.type == EV_REL) {
        if (ev.code == REL_Y) {
            // accumulate the up or down motion reported by
            // the trackball.  When it exceeds a threshold
            // (positive or negative), fake an up/down
            // key event.
            rel_sum += ev.value;
            if (rel_sum > 3) {
                fake_key = 1;
                ev.type = EV_KEY;
                ev.code = KEY_DOWN;
                ev.value = 1;
                rel_sum = 0;
            } else if (rel_sum < -3) {
                fake_key = 1;
                ev.type = EV_KEY;
                ev.code = KEY_UP;
                ev.value = 1;
                rel_sum = 0;
            }
        }
    }  else if (ev.type == EV_ABS) {

//ABS_MT_TOUCH_MAJOR	0x30(48)	/* Major axis of touching ellipse */
//ABS_MT_POSITION_X		0x35(53)	/* Center X ellipse position */
//ABS_MT_POSITION_Y		0x36(54)	/* Center Y ellipse position */
//ABS_MT_PRESSURE		0x3a(58)	/* Pressure on contact area */

		//touch event tracker
		if (ev.code == ABS_MT_PRESSURE) {
            if (ev.value == 0) {
				reset_gestures();
			}
        }

		if (ev.code == ABS_MT_TOUCH_MAJOR) {
            if (ev.value != 0) {
				if (in_touch == 0) {
            		in_touch = 1;
            		reset_gestures();
        		}
			} else {
				in_touch = 0;
				reset_gestures();
			}
        }

			//slide left-right
		if ((ev.code == ABS_MT_POSITION_X) && (touch_select == 1)) {
			old_x = touch_x;
        	touch_x = ev.value;
			if (old_x != 0) diff_x += touch_x - old_x;

			if ((diff_y < touch_y_sen) && (diff_y > (-1*touch_y_sen))) {
            	if (diff_x > touch_x_sen) {
					//ui_print("%d %d\n", diff_x, diff_y);
					fake_key = 1;
            	    ev.type = EV_KEY;
            	    ev.code = KEY_SEARCH;
            	    ev.value = 1;
            	    reset_gestures();
				} else if(diff_x < (-1*touch_x_sen)) {
					//ui_print("%d %d\n", diff_x, diff_y);
					fake_key = 1;
            	    ev.type = EV_KEY;
            	    ev.code = KEY_BACK;
            	    ev.value = 1;
            	    reset_gestures();
            	}
			}
		}

		//slide up-down
		if (ev.code == ABS_MT_POSITION_Y) {
			old_y = touch_y;
        	touch_y = ev.value;
			if (old_y != 0) diff_y += touch_y - old_y;
			if ((diff_x < touch_y_sen) && (diff_x > (-1*touch_y_sen))) {
            	if (diff_y > touch_y_sen) {
					//ui_print("%d %d\n", diff_x, diff_y);
					fake_key = 1;
                	ev.type = EV_KEY;
                	ev.code = KEY_MENU;
                	ev.value = 1;
                	reset_gestures();
				} else if(diff_y < (-1*touch_y_sen)) {
					//ui_print("%d %d\n", diff_x, diff_y);
					fake_key = 1;
               	 	ev.type = EV_KEY;
               	 	ev.code = KEY_HOME;
                	ev.value = 1;
                	reset_gestures();
           		}
			}
		}
	} else {
        rel_sum = 0;
    }

    if (ev.type != EV_KEY || ev.code > KEY_MAX)
        return 0;

    pthread_mutex_lock(&key_queue_mutex);
    if (!fake_key) {
        // our "fake" keys only report a key-down event (no
        // key-up), so don't record them in the key_pressed
        // table.
        key_pressed[ev.code] = ev.value;
    }
    const int queue_max = sizeof(key_queue) / sizeof(key_queue[0]);
    if (ev.value > 0 && key_queue_len < queue_max) {
        key_queue[key_queue_len++] = ev.code;
        pthread_cond_signal(&key_queue_cond);
    }
    pthread_mutex_unlock(&key_queue_mutex);

    if (ev.value > 0 && device_toggle_display(key_pressed, ev.code)) {
        pthread_mutex_lock(&gUpdateMutex);
        show_text = !show_text;
        if (show_text) show_text_ever = 1;
        update_screen_locked();
        pthread_mutex_unlock(&gUpdateMutex);
    }

    if (ev.value > 0 && device_reboot_now(key_pressed, ev.code)) {
        android_reboot(ANDROID_RB_RESTART, 0, 0);
    }

    return 0;
}

// Reads input events, handles special hot keys, and adds to the key queue.
static void *input_thread(void *cookie)
{
    for (;;) {
        if (!ev_wait(-1))
            ev_dispatch();
    }
    return NULL;
}

void ui_init(void)
{
    ui_has_initialized = 1;
    gr_init();
    ev_init(input_callback, NULL);

    text_col = text_row = 0;
    text_rows = gr_fb_height() / CHAR_HEIGHT;
    max_menu_rows = (text_rows - MIN_LOG_ROWS)/2;
    if (max_menu_rows > MENU_MAX_ROWS)
        max_menu_rows = MENU_MAX_ROWS;
    if (text_rows > MAX_ROWS) text_rows = MAX_ROWS;
    text_top = 1;

    text_cols = gr_fb_width() / CHAR_WIDTH;
    if (text_cols > MAX_COLS - 1) text_cols = MAX_COLS - 1;

    int i;
    for (i = 0; BITMAPS[i].name != NULL; ++i) {
        int result = res_create_surface(BITMAPS[i].name, BITMAPS[i].surface);
        if (result < 0) {
            LOGE("Missing bitmap %s\n(Code %d)\n", BITMAPS[i].name, result);
        }
    }

    gProgressBarIndeterminate = malloc(ui_parameters.indeterminate_frames *
                                       sizeof(gr_surface));
    for (i = 0; i < ui_parameters.indeterminate_frames; ++i) {
        char filename[40];
        // "indeterminate01.png", "indeterminate02.png", ...
        sprintf(filename, "indeterminate%02d", i+1);
        int result = res_create_surface(filename, gProgressBarIndeterminate+i);
        if (result < 0) {
            LOGE("Missing bitmap %s\n(Code %d)\n", filename, result);
        }
    }

    if (ui_parameters.installing_frames > 0) {
        gInstallationOverlay = malloc(ui_parameters.installing_frames *
                                      sizeof(gr_surface));
        for (i = 0; i < ui_parameters.installing_frames; ++i) {
            char filename[40];
            // "icon_installing_overlay01.png",
            // "icon_installing_overlay02.png", ...
            sprintf(filename, "icon_installing_overlay%02d", i+1);
            int result = res_create_surface(filename, gInstallationOverlay+i);
            if (result < 0) {
                LOGE("Missing bitmap %s\n(Code %d)\n", filename, result);
            }
        }

        // Adjust the offset to account for the positioning of the
        // base image on the screen.
        if (gBackgroundIcon[BACKGROUND_ICON_INSTALLING] != NULL) {
            gr_surface bg = gBackgroundIcon[BACKGROUND_ICON_INSTALLING];
            ui_parameters.install_overlay_offset_x +=
                (gr_fb_width() - gr_get_width(bg)) / 2;
            ui_parameters.install_overlay_offset_y +=
                (gr_fb_height() - gr_get_height(bg)) / 2;
        }
    } else {
        gInstallationOverlay = NULL;
    }

    pthread_t t;
    pthread_create(&t, NULL, progress_thread, NULL);
    pthread_create(&t, NULL, input_thread, NULL);
}

char *ui_copy_image(int icon, int *width, int *height, int *bpp) {
    pthread_mutex_lock(&gUpdateMutex);
    draw_background_locked(gBackgroundIcon[icon]);
    *width = gr_fb_width();
    *height = gr_fb_height();
    *bpp = sizeof(gr_pixel) * 8;
    int size = *width * *height * sizeof(gr_pixel);
    char *ret = malloc(size);
    if (ret == NULL) {
        LOGE("Can't allocate %d bytes for image\n", size);
    } else {
        memcpy(ret, gr_fb_data(), size);
    }
    pthread_mutex_unlock(&gUpdateMutex);
    return ret;
}

void ui_set_background(int icon)
{
    pthread_mutex_lock(&gUpdateMutex);
    gCurrentIcon = icon;
    update_screen_locked();
    pthread_mutex_unlock(&gUpdateMutex);
}

void ui_show_indeterminate_progress()
{
    pthread_mutex_lock(&gUpdateMutex);
    if (gProgressBarType != PROGRESSBAR_TYPE_INDETERMINATE) {
        gProgressBarType = PROGRESSBAR_TYPE_INDETERMINATE;
        update_progress_locked();
    }
    pthread_mutex_unlock(&gUpdateMutex);
}

void ui_show_progress(float portion, int seconds)
{
    pthread_mutex_lock(&gUpdateMutex);
    gProgressBarType = PROGRESSBAR_TYPE_NORMAL;
    gProgressScopeStart += gProgressScopeSize;
    gProgressScopeSize = portion;
    gProgressScopeTime = now();
    gProgressScopeDuration = seconds;
    gProgress = 0;
    update_progress_locked();
    pthread_mutex_unlock(&gUpdateMutex);
}

void ui_set_progress(float fraction)
{
    pthread_mutex_lock(&gUpdateMutex);
    if (fraction < 0.0) fraction = 0.0;
    if (fraction > 1.0) fraction = 1.0;
    if (gProgressBarType == PROGRESSBAR_TYPE_NORMAL && fraction > gProgress) {
        // Skip updates that aren't visibly different.
        int width = gr_get_width(gProgressBarIndeterminate[0]);
        float scale = width * gProgressScopeSize;
        if ((int) (gProgress * scale) != (int) (fraction * scale)) {
            gProgress = fraction;
            update_progress_locked();
        }
    }
    pthread_mutex_unlock(&gUpdateMutex);
}

void ui_reset_progress()
{
    pthread_mutex_lock(&gUpdateMutex);
    gProgressBarType = PROGRESSBAR_TYPE_NONE;
    gProgressScopeStart = gProgressScopeSize = 0;
    gProgressScopeTime = gProgressScopeDuration = 0;
    gProgress = 0;
    update_screen_locked();
    pthread_mutex_unlock(&gUpdateMutex);
}

void ui_print(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, 256, fmt, ap);
    va_end(ap);

    if (ui_log_stdout)
        fputs(buf, stdout);

    // This can get called before ui_init(), so be careful.
    pthread_mutex_lock(&gUpdateMutex);
    if (text_rows > 0 && text_cols > 0) {
        char *ptr;
        for (ptr = buf; *ptr != '\0'; ++ptr) {
            if (*ptr == '\n' || text_col >= text_cols) {
                text[text_row][text_col] = '\0';
                text_col = 0;
                text_row = (text_row + 1) % text_rows;
                if (text_row == text_top) text_top = (text_top + 1) % text_rows;
            }
            if (*ptr != '\n') text[text_row][text_col++] = *ptr;
        }
        text[text_row][text_col] = '\0';
        update_screen_locked();
    }
    pthread_mutex_unlock(&gUpdateMutex);
}

void ui_printlogtail(int nb_lines) {
    char * log_data;
    char tmp[PATH_MAX];
    FILE * f;
    int line=0;
    //don't log output to recovery.log
    ui_log_stdout=0;
    sprintf(tmp, "tail -n %d /tmp/recovery.log > /tmp/tail.log", nb_lines);
    __system(tmp);
    f = fopen("/tmp/tail.log", "rb");
    if (f != NULL) {
        while (line < nb_lines) {
            log_data = fgets(tmp, PATH_MAX, f);
            if (log_data == NULL) break;
            ui_print("%s", tmp);
            line++;
        }
        fclose(f);
    }
    ui_log_stdout=1;
}

void ui_reset_text_col()
{
    pthread_mutex_lock(&gUpdateMutex);
    text_col = 0;
    pthread_mutex_unlock(&gUpdateMutex);
}

#define MENU_ITEM_HEADER " - "
#define MENU_ITEM_HEADER_LENGTH strlen(MENU_ITEM_HEADER)

int ui_start_menu(char** headers, char** items, int initial_selection) {
    int i;
    pthread_mutex_lock(&gUpdateMutex);
    if (text_rows > 0 && text_cols > 0) {
        for (i = 0; i < text_rows; ++i) {
            if (headers[i] == NULL) break;
            strncpy(menu[i], headers[i], text_cols-1);
            menu[i][text_cols-1] = '\0';
        }
        menu_top = i;
        for (; i < MENU_MAX_ROWS; i++) {
            if (items[i-menu_top] == NULL) break;
           	strcpy(menu[i], MENU_ITEM_HEADER);
            strncpy(menu[i] + MENU_ITEM_HEADER_LENGTH, items[i-menu_top], MENU_MAX_COLS - 1 - MENU_ITEM_HEADER_LENGTH);
            menu[i][MENU_MAX_COLS-1] = '\0';
        }

        if (gShowBackButton && ui_menu_level > 0) {
            strcpy(menu[i], " +++++++++++  Go Back  +++++++++++");
            ++i;
        }
        
        strcpy(menu[i], " ");
        ++i;

        menu_items = i - menu_top;
        show_menu = 1;
        menu_sel = menu_show_start = initial_selection;
        update_screen_locked();
    }
    pthread_mutex_unlock(&gUpdateMutex);
    if (gShowBackButton && ui_menu_level > 0) {
        return menu_items - 1;
    }
    return menu_items;
}

int ui_menu_select(int sel) {
    int old_sel;
    pthread_mutex_lock(&gUpdateMutex);
    if (show_menu > 0) {
        old_sel = menu_sel;
        menu_sel = sel;

        if (menu_sel < 0) menu_sel = menu_items-1 + menu_sel;
        if (menu_sel >= menu_items-1) menu_sel = menu_sel - menu_items+1;


        if (menu_sel < menu_show_start && menu_show_start > 0) {
            menu_show_start = menu_sel;
        }

        if (menu_sel - menu_show_start + menu_top >= max_menu_rows) {
            menu_show_start = menu_sel + menu_top - max_menu_rows + 1;
        }

        sel = menu_sel;

        if (menu_sel != old_sel) update_screen_locked();
    }
    pthread_mutex_unlock(&gUpdateMutex);
    return sel;
}

void ui_end_menu() {
    int i;
    pthread_mutex_lock(&gUpdateMutex);
    if (show_menu > 0 && text_rows > 0 && text_cols > 0) {
        show_menu = 0;
        update_screen_locked();
    }
    pthread_mutex_unlock(&gUpdateMutex);
}

int ui_text_visible()
{
    pthread_mutex_lock(&gUpdateMutex);
    int visible = show_text;
    pthread_mutex_unlock(&gUpdateMutex);
    return visible;
}

int ui_text_ever_visible()
{
    pthread_mutex_lock(&gUpdateMutex);
    int ever_visible = show_text_ever;
    pthread_mutex_unlock(&gUpdateMutex);
    return ever_visible;
}

void ui_show_text(int visible)
{
    pthread_mutex_lock(&gUpdateMutex);
    show_text = visible;
    if (show_text) show_text_ever = 1;
    update_screen_locked();
    pthread_mutex_unlock(&gUpdateMutex);
}

// Return true if USB is connected.
static int usb_connected() {
    int fd = open("/sys/class/android_usb/android0/state", O_RDONLY);
    if (fd < 0) {
        printf("failed to open /sys/class/android_usb/android0/state: %s\n",
               strerror(errno));
        return 0;
    }

    char buf;
    /* USB is connected if android_usb state is CONNECTED or CONFIGURED */
    int connected = (read(fd, &buf, 1) == 1) && (buf == 'C');
    if (close(fd) < 0) {
        printf("failed to close /sys/class/android_usb/android0/state: %s\n",
               strerror(errno));
    }
    return connected;
}

int ui_wait_key()
{
    pthread_mutex_lock(&key_queue_mutex);

    // Time out after UI_WAIT_KEY_TIMEOUT_SEC, unless a USB cable is
    // plugged in.
    do {
        struct timeval now;
        struct timespec timeout;
        gettimeofday(&now, NULL);
        timeout.tv_sec = now.tv_sec;
        timeout.tv_nsec = now.tv_usec * 1000;
        timeout.tv_sec += UI_WAIT_KEY_TIMEOUT_SEC;

        int rc = 0;
        while (key_queue_len == 0 && rc != ETIMEDOUT) {
            rc = pthread_cond_timedwait(&key_queue_cond, &key_queue_mutex,
                                        &timeout);
        }
    } while (0 && key_queue_len == 0);

    int key = -1;
    if (key_queue_len > 0) {
        key = key_queue[0];
        memcpy(&key_queue[0], &key_queue[1], sizeof(int) * --key_queue_len);
    }
    pthread_mutex_unlock(&key_queue_mutex);
	//update battery stat on select
	if ((key == KEY_END) || (key == KEY_SEARCH)) battlevel = cpcap_batt_percent();
    return key;
}

int ui_key_pressed(int key)
{
    // This is a volatile static array, don't bother locking
    return key_pressed[key];
}

void ui_clear_key_queue() {
    pthread_mutex_lock(&key_queue_mutex);
    key_queue_len = 0;
    pthread_mutex_unlock(&key_queue_mutex);
}

void ui_set_show_text(int value) {
    show_text = value;
}

void ui_set_showing_back_button(int showBackButton) {
    gShowBackButton = showBackButton;
}

int ui_get_showing_back_button() {
    return gShowBackButton;
}


/*
 * codes from batt_cpcap.c tpruvot@CyanogenDefy/android_external_bootmenu
 * 
 * Read battery voltage. an idea of what battery level is.
 *
 */
int cpcap_batt_percent(void) {
	int cpcap_fd = -1;
    struct cpcap_adc_us_request req_us;
    int ret, volt_batt;
    float range, mini;

    cpcap_fd = open("/dev/cpcap_batt", O_RDONLY | O_NONBLOCK);
    if (cpcap_fd <= 0) {
        return -1;
    }
	
	memset(&req_us, 0, sizeof(struct cpcap_adc_us_request));
    req_us.timing = CPCAP_ADC_TIMING_IMM;
    req_us.format = CPCAP_ADC_FORMAT_CONVERTED;
    req_us.type = CPCAP_ADC_TYPE_BANK_0;

	ret = ioctl(cpcap_fd, CPCAP_IOCTL_BATT_ATOD_SYNC, &req_us);
    if (ret != 0) {
        close(cpcap_fd);
		return -1;
    }

    volt_batt = req_us.result[CPCAP_ADC_BATTP];
	close(cpcap_fd);
    return volt_batt;
}
