#include <linux/input/dt2w.h>

#define DT2W_MIN_TIME_BETWEEN_TOUCHES    20
#define DT2W_MAX_TIME_BETWEEN_TOUCHES    300
#define DT2W_SECOND_TOUCH_RADIUS         60

static bool screen_is_off;
static unsigned long long last_tap_time = 0;
static int last_x_coord = 0;
static int last_y_coord = 0;
static int touch_count = 0;

int fb_notifier_callback(struct notifier_block *this, unsigned long event, void *data)
{
 	struct fb_event *evdata = data;
  	int *blank;
  
  	if (evdata && evdata->data && event == FB_EVENT_BLANK) {
  		blank = evdata->data;
  		switch (*blank) {
  			case FB_BLANK_UNBLANK:
  				//display on
                                 screen_is_off = false;
  				break;
  			case FB_BLANK_POWERDOWN:
  			case FB_BLANK_HSYNC_SUSPEND:
  			case FB_BLANK_VSYNC_SUSPEND:
  			case FB_BLANK_NORMAL:
  				//display off
                                 screen_is_off = true;
  				break;
  		}
         }
 
 	return NOTIFY_OK;
}

void fb_notifier_unregister(struct notifier_block *fb_notif) {
        fb_unregister_client(fb_notif);
}

bool fb_notifier_register(struct notifier_block *fb_notif) {
	fb_notif->notifier_call = fb_notifier_callback;
  	if (fb_register_client(fb_notif) != 0)
  		return true;
	else
		return false;
}

static unsigned int calc_feather(int coord, int prev_coord)
{
	int calc_coord = 0;

	calc_coord = coord - prev_coord;
	if (calc_coord < 0)
		return -calc_coord;

	return calc_coord;
}

static void doubletap2wake_reset(void)
{
	touch_count = 0;
	last_tap_time = 0;
	last_x_coord = 0;
	last_y_coord = 0;
}

static void new_touch(int x, int y)
{
	last_tap_time = jiffies;
	last_x_coord = x;
	last_y_coord = y;
	touch_count += 1;
}

bool detect_dt2w_event(int x, int y) {
	if (!screen_is_off)
		return false;

	switch (touch_count) {
		case 0: {
			new_touch(x, y);
			break;
		}
		case 1: {
			if ((calc_feather(x, last_x_coord) < DT2W_SECOND_TOUCH_RADIUS) &&
			(calc_feather(y, last_y_coord) < DT2W_SECOND_TOUCH_RADIUS) &&
			((jiffies - last_tap_time) < DT2W_MAX_TIME_BETWEEN_TOUCHES) &&
			((jiffies - last_tap_time) > DT2W_MIN_TIME_BETWEEN_TOUCHES)) {
				pr_err("double click gesture triggerred !\n"
							"wakeup the tablet!\n");
				doubletap2wake_reset();
				return true;
			} else {
				doubletap2wake_reset();
				new_touch(x, y);
			}
			break;
		}
	}
	return false;
}
