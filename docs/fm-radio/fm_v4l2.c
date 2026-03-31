/*
 * fm_v4l2 — V4L2 FM radio control for /dev/radio0 (brcm_fmdrv)
 *
 * Build (static, ARM):
 *   arm-linux-gnueabihf-gcc -static -o fm_v4l2 fm_v4l2.c
 *
 * Usage:
 *   fm_v4l2 on [freq_mhz*10]     Turn on, tune to freq (default 1000=100.0MHz)
 *   fm_v4l2 off                   Turn off
 *   fm_v4l2 tune <freq_mhz*10>   Tune to frequency
 *   fm_v4l2 status                Show tuner info
 *   fm_v4l2 vol <0-65535>         Set volume
 *   fm_v4l2 mute                  Mute
 *   fm_v4l2 unmute                Unmute
 *   fm_v4l2 seek_up               Seek next station up
 *   fm_v4l2 seek_down             Seek next station down
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

#define RADIO_DEV "/dev/radio0"

static int radio_fd = -1;

static int radio_open(void)
{
    radio_fd = open(RADIO_DEV, O_RDWR);
    if (radio_fd < 0) {
        fprintf(stderr, "ERROR open(%s): %s\n", RADIO_DEV, strerror(errno));
        return -1;
    }
    return 0;
}

static void radio_close(void)
{
    if (radio_fd >= 0) {
        close(radio_fd);
        radio_fd = -1;
    }
}

/* V4L2 frequency is in units of 62.5 Hz when V4L2_TUNER_CAP_LOW is set.
 * freq_mhz10 = MHz * 10 (e.g. 1000 = 100.0 MHz)
 * V4L2 freq = freq_mhz10 * 100000 / 62.5 = freq_mhz10 * 16000
 */
static unsigned int freq_to_v4l2(int freq_mhz10)
{
    return (unsigned int)(freq_mhz10 * 16000);
}

static int v4l2_to_freq_mhz10(unsigned int v4l2_freq)
{
    return (int)(v4l2_freq / 16000);
}

static void print_freq(int freq_mhz10)
{
    printf("%d.%d MHz", freq_mhz10 / 10, freq_mhz10 % 10);
}

static int do_status(void)
{
    struct v4l2_tuner tuner;
    struct v4l2_frequency freq;
    struct v4l2_capability cap;
    int ret;

    memset(&cap, 0, sizeof(cap));
    ret = ioctl(radio_fd, VIDIOC_QUERYCAP, &cap);
    if (ret == 0) {
        printf("Driver:   %s\n", cap.driver);
        printf("Card:     %s\n", cap.card);
    }

    memset(&tuner, 0, sizeof(tuner));
    tuner.index = 0;
    ret = ioctl(radio_fd, VIDIOC_G_TUNER, &tuner);
    if (ret < 0) {
        fprintf(stderr, "ERROR VIDIOC_G_TUNER: %s\n", strerror(errno));
        return -1;
    }

    printf("Tuner:    %s\n", tuner.name);
    printf("Signal:   %d/65535\n", tuner.signal);
    printf("Stereo:   %s\n", (tuner.rxsubchans & V4L2_TUNER_SUB_STEREO) ? "yes" : "mono");
    printf("Range:    ");
    print_freq(v4l2_to_freq_mhz10(tuner.rangelow));
    printf(" - ");
    print_freq(v4l2_to_freq_mhz10(tuner.rangehigh));
    printf("\n");

    memset(&freq, 0, sizeof(freq));
    freq.tuner = 0;
    ret = ioctl(radio_fd, VIDIOC_G_FREQUENCY, &freq);
    if (ret < 0) {
        fprintf(stderr, "ERROR VIDIOC_G_FREQUENCY: %s\n", strerror(errno));
    } else {
        printf("Freq:     ");
        print_freq(v4l2_to_freq_mhz10(freq.frequency));
        printf(" (raw=%u)\n", freq.frequency);
    }

    return 0;
}

static int do_tune(int freq_mhz10)
{
    struct v4l2_frequency freq;
    int ret;

    memset(&freq, 0, sizeof(freq));
    freq.tuner = 0;
    freq.type = V4L2_TUNER_RADIO;
    freq.frequency = freq_to_v4l2(freq_mhz10);

    printf("Tuning to ");
    print_freq(freq_mhz10);
    printf(" (v4l2=%u)...\n", freq.frequency);

    ret = ioctl(radio_fd, VIDIOC_S_FREQUENCY, &freq);
    if (ret < 0) {
        fprintf(stderr, "ERROR VIDIOC_S_FREQUENCY: %s\n", strerror(errno));
        return -1;
    }

    printf("OK\n");
    return 0;
}

static int do_volume(int vol)
{
    struct v4l2_control ctrl;
    int ret;

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = V4L2_CID_AUDIO_VOLUME;
    ctrl.value = vol;

    ret = ioctl(radio_fd, VIDIOC_S_CTRL, &ctrl);
    if (ret < 0) {
        fprintf(stderr, "ERROR VIDIOC_S_CTRL(VOLUME): %s\n", strerror(errno));
        return -1;
    }
    printf("Volume: %d\n", vol);
    return 0;
}

static int do_mute(int mute)
{
    struct v4l2_control ctrl;
    int ret;

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = V4L2_CID_AUDIO_MUTE;
    ctrl.value = mute;

    ret = ioctl(radio_fd, VIDIOC_S_CTRL, &ctrl);
    if (ret < 0) {
        fprintf(stderr, "ERROR VIDIOC_S_CTRL(MUTE): %s\n", strerror(errno));
        return -1;
    }
    printf("%s\n", mute ? "Muted" : "Unmuted");
    return 0;
}

static int do_seek(int direction_up)
{
    struct v4l2_hw_freq_seek seek;
    int ret;

    memset(&seek, 0, sizeof(seek));
    seek.tuner = 0;
    seek.type = V4L2_TUNER_RADIO;
    seek.seek_upward = direction_up ? 1 : 0;
    seek.wrap_around = 1;

    printf("Seeking %s...\n", direction_up ? "UP" : "DOWN");

    ret = ioctl(radio_fd, VIDIOC_S_HW_FREQ_SEEK, &seek);
    if (ret < 0) {
        fprintf(stderr, "ERROR VIDIOC_S_HW_FREQ_SEEK: %s\n", strerror(errno));
        return -1;
    }

    /* Read current freq after seek */
    do_status();
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
        "fm_v4l2 — V4L2 FM radio control for /dev/radio0\n"
        "\n"
        "Usage:\n"
        "  fm_v4l2 on [freq]       Open radio, tune (default 1000=100.0MHz)\n"
        "  fm_v4l2 off             Close radio\n"
        "  fm_v4l2 tune <freq>     Tune to freq (MHz*10, e.g. 1000=100.0)\n"
        "  fm_v4l2 status          Show tuner status\n"
        "  fm_v4l2 vol <0-65535>   Set volume\n"
        "  fm_v4l2 mute            Mute\n"
        "  fm_v4l2 unmute          Unmute\n"
        "  fm_v4l2 seek_up         Seek next station up\n"
        "  fm_v4l2 seek_down       Seek next station down\n"
    );
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage();
        return 1;
    }

    if (radio_open() < 0)
        return 1;

    int ret = 0;

    if (strcmp(argv[1], "on") == 0) {
        int freq = (argc > 2) ? atoi(argv[2]) : 1000;
        printf("=== FM Radio ON ===\n");
        ret = do_tune(freq);
        if (ret == 0) {
            /* Set moderate volume (not loud!) */
            do_volume(8000);
            do_status();
        }
    }
    else if (strcmp(argv[1], "off") == 0) {
        printf("=== FM Radio OFF ===\n");
        do_mute(1);
        /* Just closing fd should be enough */
    }
    else if (strcmp(argv[1], "tune") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: fm_v4l2 tune <freq>\n");
            ret = 1;
        } else {
            ret = do_tune(atoi(argv[2]));
            if (ret == 0) do_status();
        }
    }
    else if (strcmp(argv[1], "status") == 0) {
        ret = do_status();
    }
    else if (strcmp(argv[1], "vol") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: fm_v4l2 vol <0-65535>\n");
            ret = 1;
        } else {
            ret = do_volume(atoi(argv[2]));
        }
    }
    else if (strcmp(argv[1], "mute") == 0) {
        ret = do_mute(1);
    }
    else if (strcmp(argv[1], "unmute") == 0) {
        ret = do_mute(0);
    }
    else if (strcmp(argv[1], "seek_up") == 0) {
        ret = do_seek(1);
    }
    else if (strcmp(argv[1], "seek_down") == 0) {
        ret = do_seek(0);
    }
    else {
        usage();
        ret = 1;
    }

    radio_close();
    return ret ? 1 : 0;
}
