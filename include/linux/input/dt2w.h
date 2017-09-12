#include <linux/types.h>
#include <linux/fb.h>

bool detect_dt2w_event(int x, int y);

void fb_notifier_unregister(struct notifier_block *fb_notif);
bool fb_notifier_register(struct notifier_block *fb_notif);
