/*
 * fm_v4l2 — V4L2 FM radio control for /dev/radio0 (brcm_fmdrv)
 *
 * Build (static, ARM):
 *   arm-linux-gnueabihf-gcc -static -o fm_v4l2 fm_v4l2.c
 *
 * Usage:
 *   fm_v4l2 bringup [fw_path] [bdaddr]  Attach N_BRCM_HCI ldisc to /dev/ttyTHS2,
 *                                       program firmware via sysfs, wait for install=1.
 *                                       Holds tty fd in a background child; pidfile
 *                                       written to /data/local/tmp/fm_v4l2_ldisc.pid.
 *                                       Requires bluedroid stopped and UART free.
 *   fm_v4l2 teardown                    Kill ldisc holder child (releases tty, ldisc resets).
 *   fm_v4l2 on [freq_mhz*10]            Turn on, tune to freq (default 1000=100.0MHz)
 *   fm_v4l2 off                         Turn off
 *   fm_v4l2 tune <freq_mhz*10>          Tune to frequency
 *   fm_v4l2 status                      Show tuner info
 *   fm_v4l2 vol <0-65535>               Set volume
 *   fm_v4l2 mute                        Mute
 *   fm_v4l2 unmute                      Unmute
 *   fm_v4l2 seek_up                     Seek next station up
 *   fm_v4l2 seek_down                   Seek next station down
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <linux/videodev2.h>

/* Broadcom shared transport ldisc — see drivers/bluetooth/broadcom/ */
#define TTY_DEV                 "/dev/ttyTHS2"
#define N_BRCM_HCI              26
#define SYSFS_BASE              "/sys/devices/platform/bcm_ldisc"
#define PIDFILE                 "/data/local/tmp/fm_v4l2_ldisc.pid"
#define DEFAULT_FW_PATH         "/vendor/firmware/mocha_bcm4350.hcd"
#define BRINGUP_TIMEOUT_MS      10000
#define LDISC_ON                '1'  /* V4L2_STATUS_ON  */
#define LDISC_OFF               '0'  /* V4L2_STATUS_OFF */
#define LDISC_ERR               '2'  /* V4L2_STATUS_ERR */

#define RADIO_DEV "/dev/radio0"

static int radio_fd = -1;

/* ========================================================================
 * Broadcom ldisc bringup/teardown
 *
 * Protocol (see drivers/bluetooth/broadcom/line_discipline_driver/brcm_sh_ldisc.c):
 *   1. Write fw_patchfile sysfs so ldisc knows which patchram to load.
 *   2. Optionally write bdaddr sysfs.
 *   3. open(ttyTHS2) + ioctl(TIOCSETD, N_BRCM_HCI=26) triggers ldisc open callback,
 *      which runs: HCI reset -> hci_download_minidriver -> request_firmware(fw_name)
 *      -> patchram upload -> baudrate switch. On success, ldisc_install becomes
 *      V4L2_STATUS_ON ('1') and sysfs_notify fires.
 *   4. Userspace polls /sys/.../install for '1'.
 *   5. The tty fd must stay open — when it closes, kernel tears the ldisc down.
 * ======================================================================== */

static int sysfs_write(const char *attr, const char *value)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", SYSFS_BASE, attr);
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "ERROR open(%s): %s\n", path, strerror(errno));
        return -1;
    }
    size_t len = strlen(value);
    ssize_t n = write(fd, value, len);
    int saved = errno;
    close(fd);
    if (n != (ssize_t)len) {
        fprintf(stderr, "ERROR write(%s): %s (wrote %zd/%zu)\n",
                path, strerror(saved), n, len);
        return -1;
    }
    return 0;
}

static int sysfs_read_install(char *out)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/install", SYSFS_BASE);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    int n = read(fd, out, 1);
    close(fd);
    return (n == 1) ? 0 : -1;
}

/* Child: takes ownership of ttyTHS2, installs N_BRCM_HCI ldisc, sleeps forever.
 * When this process exits (or is killed), the kernel closes the tty and ldisc
 * resets automatically — that is the teardown mechanism. */
static int ldisc_child(void)
{
    if (setsid() < 0) {
        /* non-fatal */
    }
    int tty = open(TTY_DEV, O_RDWR | O_NOCTTY);
    if (tty < 0) {
        fprintf(stderr, "ERROR child open(%s): %s\n", TTY_DEV, strerror(errno));
        return 2;
    }
    int ldisc = N_BRCM_HCI;
    if (ioctl(tty, TIOCSETD, &ldisc) < 0) {
        fprintf(stderr, "ERROR child TIOCSETD(%d): %s\n",
                N_BRCM_HCI, strerror(errno));
        close(tty);
        return 3;
    }
    /* Detach stdio so shell doesn't hang waiting on us. */
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > 2)
            close(devnull);
    }
    for (;;)
        pause();
    /* not reached */
    return 0;
}

static int do_bringup(const char *fw_path, const char *bdaddr)
{
    printf("=== Broadcom ldisc bringup ===\n");
    printf("Firmware: %s\n", fw_path);
    if (bdaddr)
        printf("BDA:      %s\n", bdaddr);

    /* Skip if already up. */
    char state = 0;
    if (sysfs_read_install(&state) == 0 && state == LDISC_ON) {
        printf("ldisc already attached (install='1'), skipping\n");
        return 0;
    }

    /* 1. Firmware path — ldisc callback reads this via request_firmware(). */
    if (sysfs_write("fw_patchfile", fw_path) < 0)
        return 1;

    /* 2. Optional BD address. */
    if (bdaddr && sysfs_write("bdaddr", bdaddr) < 0)
        return 1;

    /* 3. Fork helper that holds the tty fd open with N_BRCM_HCI ldisc. */
    fflush(stdout);
    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        return 1;
    }
    if (child == 0)
        _exit(ldisc_child());

    /* Parent: record pid for teardown, then poll install sysfs. */
    FILE *f = fopen(PIDFILE, "w");
    if (f) {
        fprintf(f, "%d\n", (int)child);
        fclose(f);
    }
    printf("ldisc helper pid=%d, polling install (timeout %dms)\n",
           (int)child, BRINGUP_TIMEOUT_MS);

    int elapsed;
    for (elapsed = 0; elapsed < BRINGUP_TIMEOUT_MS; elapsed += 100) {
        usleep(100 * 1000);
        char c = 0;
        if (sysfs_read_install(&c) == 0) {
            if (c == LDISC_ON) {
                printf("install='1' (ready) after %d ms\n", elapsed + 100);
                printf("OK — %s ready for V4L2 ioctls\n", RADIO_DEV);
                return 0;
            }
            if (c == LDISC_ERR) {
                fprintf(stderr, "ERROR install='2' — ldisc reported error\n");
                break;
            }
        }
        /* Did child die? */
        int status;
        pid_t w = waitpid(child, &status, WNOHANG);
        if (w == child) {
            fprintf(stderr, "ERROR helper exited early (status=0x%x)\n",
                    status);
            unlink(PIDFILE);
            return 1;
        }
    }

    fprintf(stderr, "ERROR bringup timed out, killing helper\n");
    kill(child, SIGTERM);
    unlink(PIDFILE);
    return 1;
}

static int do_teardown(void)
{
    FILE *f = fopen(PIDFILE, "r");
    if (!f) {
        fprintf(stderr, "No pidfile at %s — nothing to tear down\n", PIDFILE);
        return 1;
    }
    int pid = 0;
    int ok = (fscanf(f, "%d", &pid) == 1);
    fclose(f);
    if (!ok || pid <= 0) {
        fprintf(stderr, "Bad pidfile content\n");
        unlink(PIDFILE);
        return 1;
    }
    printf("Teardown: kill pid=%d\n", pid);
    if (kill(pid, SIGTERM) < 0 && errno != ESRCH) {
        perror("kill");
        return 1;
    }
    unlink(PIDFILE);
    printf("OK\n");
    return 0;
}

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
        "  fm_v4l2 bringup [fw] [bda]  Attach N_BRCM_HCI to /dev/ttyTHS2, load\n"
        "                              patchram, wait for ready. Requires bluedroid\n"
        "                              stopped (svc bluetooth disable). Default fw:\n"
        "                              " DEFAULT_FW_PATH "\n"
        "  fm_v4l2 teardown        Kill ldisc helper, release tty\n"
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

    /* Subcommands that don't need /dev/radio0 open. */
    if (strcmp(argv[1], "bringup") == 0) {
        const char *fw  = (argc > 2) ? argv[2] : DEFAULT_FW_PATH;
        const char *bda = (argc > 3) ? argv[3] : NULL;
        return do_bringup(fw, bda);
    }
    if (strcmp(argv[1], "teardown") == 0)
        return do_teardown();

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
