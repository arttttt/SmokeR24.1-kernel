/*
 * fm_v4l2 — V4L2 FM radio control for /dev/radio0 (brcm_fmdrv)
 *
 * Build (static, ARM):
 *   arm-linux-gnueabihf-gcc -static -o fm_v4l2 fm_v4l2.c
 *
 * Usage:
 *   fm_v4l2 bringup [fw_name] [bdaddr]  Power on BCM4354 via rfkill, program
 *                                       firmware name into bcm_ldisc sysfs,
 *                                       fork UIM-mock helper that attaches
 *                                       N_BRCM_HCI on demand. Requires
 *                                       bluedroid stopped and UART free.
 *                                       fw_name is looked up by request_firmware
 *                                       in /vendor/firmware,/etc/firmware,... —
 *                                       do not pass an absolute path.
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
#include <dirent.h>
#include <poll.h>
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
#define DEFAULT_FW_NAME         "mocha_bcm4350.hcd"
#define RFKILL_NAME             "bluedroid_pm"
#define BRINGUP_TIMEOUT_MS      10000
#define LDISC_ON                '1'  /* V4L2_STATUS_ON  */
#define LDISC_OFF               '0'  /* V4L2_STATUS_OFF */
#define LDISC_ERR               '2'  /* V4L2_STATUS_ERR */

/* Broadcom hci_uart proto id — see drivers/bluetooth/broadcom/
 * line_discipline_driver/brcm_hci_uart.h (HCI_UART_BRCM=0) and brcm_hci.c
 * where brcmp.id = HCI_UART_BRCM. After TIOCSETD installs the ldisc,
 * HCIUARTSETPROTO sets HCI_UART_PROTO_SET in hu->flags — without this
 * flag brcm_hci_uart_tty_receive() drops every byte from the chip,
 * wait_for_completion(cmd_rcvd) times out in download_patchram, and we
 * look like the chip never answered. */
#define HCI_UART_BRCM           0
#define HCIUARTSETPROTO         _IOW('U', 200, int)

#define RADIO_DEV "/dev/radio0"

static int radio_fd = -1;

/* ========================================================================
 * Broadcom ldisc bringup/teardown — UIM daemon behaviour
 *
 * The real sequence (see brcm_sh_ldisc_start() + brcm_hci_uart_tty_open() +
 * download_patchram() in drivers/bluetooth/broadcom/line_discipline_driver/
 * brcm_sh_ldisc.c) is driven by the kernel side, not by userspace:
 *
 *   1. Something opens /dev/radio0 (or /dev/brcm_bt_drv). The protocol driver
 *      calls fmc_prepare() / brcm_sh_ldisc_register(PROTO_SH_FM).
 *   2. brcm_sh_ldisc_start() sets ldisc_install = V4L2_STATUS_ON ('1') and
 *      fires sysfs_notify("install"), then wait_for_completion(ldisc_installed,
 *      1500 ms).
 *   3. A userspace daemon (classically UIM) must be polling the sysfs file
 *      /sys/.../install for POLLPRI; on seeing '1' it does:
 *         fd = open("/dev/ttyTHS2", ...); ioctl(fd, TIOCSETD, N_BRCM_HCI);
 *      and keeps the fd open.
 *   4. The kernel fires the ldisc ops->open callback
 *      (brcm_hci_uart_tty_open), which calls sh_ldisc_complete() — that
 *      wakes the wait_for_completion on the kernel side.
 *   5. brcm_sh_ldisc_start() proceeds: HCI reset, hci_download_minidriver,
 *      request_firmware(fw_name) — loads the patchram from the path we wrote
 *      to fw_patchfile — patchram upload, baudrate switch.
 *   6. Control returns to /dev/radio0 open(); V4L2 ioctls now work.
 *
 * So "bringup" here has two phases:
 *   (a) Program fw_patchfile and bdaddr into sysfs *before* anybody triggers
 *       step 1.
 *   (b) Fork a UIM-mock child that polls /sys/.../install indefinitely and
 *       attaches N_BRCM_HCI on demand. The child holds the tty fd for as
 *       long as install stays '1'; when install goes back to '0' (driver
 *       unregistered) it closes the tty, letting the kernel reset the ldisc.
 * "on" then opens /dev/radio0 and the whole dance runs synchronously.
 * "teardown" SIGTERMs the child; closing the tty tears the ldisc down
 * automatically (the close path at line 2044 of brcm_sh_ldisc.c).
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

/* Find /sys/class/rfkill/rfkillN whose "name" matches `name`.
 * Returns 0 on success and copies the dir name into `out` (caller buffer). */
static int rfkill_find(const char *name, char *out, size_t out_sz)
{
    DIR *d = opendir("/sys/class/rfkill");
    if (!d)
        return -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "rfkill", 6) != 0)
            continue;
        char namepath[256];
        snprintf(namepath, sizeof(namepath),
                 "/sys/class/rfkill/%s/name", e->d_name);
        FILE *f = fopen(namepath, "r");
        if (!f)
            continue;
        char buf[64] = {0};
        if (fgets(buf, sizeof(buf), f)) {
            char *nl = strchr(buf, '\n');
            if (nl)
                *nl = 0;
            if (strcmp(buf, name) == 0) {
                snprintf(out, out_sz, "%s", e->d_name);
                fclose(f);
                closedir(d);
                return 0;
            }
        }
        fclose(f);
    }
    closedir(d);
    return -1;
}

/* Ensure the named rfkill is unblocked (state=1), without cycling it.
 *
 * BCM4354 is a combo WiFi/BT/FM chip. On mocha the bluedroid_pm GPIO
 * (BT_REG_ON) resets the *whole* chip on every 0→1 edge, which takes WiFi
 * down with it. A previous version of this helper did an off→on toggle to
 * guarantee a clean POR; that killed the live WiFi link, netd/system_server
 * cascaded, and the UI crashed back to the bootanimation.
 *
 * Safe rule: only write '1' if the state currently reads '0'. If the chip is
 * already powered (BT on before or left on by a previous session), do nothing
 * — there is nothing to toggle. */
static int rfkill_power_on(const char *name)
{
    char rk[32];
    if (rfkill_find(name, rk, sizeof(rk)) < 0) {
        fprintf(stderr, "ERROR rfkill: no device named '%s'\n", name);
        return -1;
    }
    char path[256];
    snprintf(path, sizeof(path), "/sys/class/rfkill/%s/state", rk);

    /* Check current state first. */
    int rfd = open(path, O_RDONLY);
    if (rfd < 0) {
        fprintf(stderr, "ERROR open(%s) RO: %s\n", path, strerror(errno));
        return -1;
    }
    char cur = 0;
    (void)read(rfd, &cur, 1);
    close(rfd);

    if (cur == '1') {
        printf("rfkill %s already on, skipping\n", name);
        return 0;
    }

    /* state was '0' (or unknown) — power on. */
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "ERROR open(%s) WO: %s\n", path, strerror(errno));
        return -1;
    }
    if (write(fd, "1", 1) != 1) {
        fprintf(stderr, "ERROR write state=1: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);
    usleep(200 * 1000);
    return 0;
}

/* UIM mock — runs in child, never returns.
 *
 * Holds two fds: sysfs "install" (for POLLPRI notify) and, once the kernel
 * asks for attach by setting install='1', the tty fd with N_BRCM_HCI ldisc.
 * Closing the tty fd tears the ldisc down; we do that when install goes back
 * to '0'. Exits on SIGTERM (kernel auto-cleans fds on exit). */
static int ldisc_child(void)
{
    if (setsid() < 0) {
        /* non-fatal */
    }

    /* Detach stdio so parent's shell doesn't hang on us. */
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > 2)
            close(devnull);
    }

    char sysfs_path[256];
    snprintf(sysfs_path, sizeof(sysfs_path), "%s/install", SYSFS_BASE);
    int sfd = open(sysfs_path, O_RDONLY);
    if (sfd < 0)
        return 2;

    int tty = -1;
    /* First pass: consume the initial sysfs value so future POLLPRI edges fire. */
    for (;;) {
        char c = 0;
        if (lseek(sfd, 0, SEEK_SET) >= 0 && read(sfd, &c, 1) == 1) {
            if (c == LDISC_ON && tty < 0) {
                tty = open(TTY_DEV, O_RDWR | O_NOCTTY);
                if (tty >= 0) {
                    int ldisc = N_BRCM_HCI;
                    int proto = HCI_UART_BRCM;
                    if (ioctl(tty, TIOCSETD, &ldisc) < 0 ||
                        ioctl(tty, HCIUARTSETPROTO, proto) < 0) {
                        close(tty);
                        tty = -1;
                    }
                }
            } else if (c != LDISC_ON && tty >= 0) {
                /* Driver unregistered — let the kernel close the ldisc. */
                close(tty);
                tty = -1;
            }
        }

        /* Wait for the next sysfs_notify on install. POLLPRI fires on notify. */
        struct pollfd pfd;
        pfd.fd = sfd;
        pfd.events = POLLPRI | POLLERR;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, -1);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
    }

    if (tty >= 0)
        close(tty);
    close(sfd);
    return 0;
}

static int do_bringup(const char *fw_name, const char *bdaddr)
{
    printf("=== Broadcom ldisc bringup (UIM mock) ===\n");
    printf("Firmware: %s\n", fw_name);
    if (bdaddr)
        printf("BDA:      %s\n", bdaddr);

    /* Refuse to start a second helper. */
    FILE *pf = fopen(PIDFILE, "r");
    if (pf) {
        int existing = 0;
        if (fscanf(pf, "%d", &existing) == 1 && existing > 0 &&
            kill(existing, 0) == 0) {
            fclose(pf);
            printf("Helper already running (pid=%d), skipping\n", existing);
            return 0;
        }
        fclose(pf);
        unlink(PIDFILE);
    }

    /* Intentionally NOT calling rfkill_power_on() here. On mocha the
     * bluedroid_pm GPIO (R.1) is electrically entangled with the WiFi side
     * of the BCM4354 combo chip — every 0→1 edge we've done from here
     * glitches WiFi, brcmfmac re-inits, and netd/system_server fall over
     * during the reinit window, leaving Android in bootanimation. Stock
     * bluedroid can toggle the same rfkill without that fallout because
     * it starts banging HCI on the line immediately and holds the chip
     * busy through the transient. Replicating that timing from here is
     * brittle; it's simpler to require the chip to be powered by other
     * means (e.g. user toggles BT via Settings first) and make bringup a
     * pure ldisc-attach operation. The rfkill_power_on() helper is kept
     * in the tool for manual use when it's known to be safe.
     *
     * 1. Firmware name — download_patchram() loads it via request_firmware(),
     *    which searches /vendor/firmware, /etc/firmware, /lib/firmware. */
    if (sysfs_write("fw_patchfile", fw_name) < 0)
        return 1;

    /* 3. Optional BD address (only needed for BT, but harmless here). */
    if (bdaddr && sysfs_write("bdaddr", bdaddr) < 0)
        return 1;

    /* 4. Vendor parameters. brcm_sh_ldisc parses space-separated key=value
     *    pairs out of this attribute (parse_vendor_params). The one that
     *    matters for us is custom_baudrate — if left at its 0 default, the
     *    post-patchram HCI_VSC_UPDATE_BAUDRATE (FC18) command is sent with a
     *    baud of 0 ("Baudrate not supported!" + response timeout), which
     *    aborts the whole attach. Stock NVIDIA libbt-vendor uses 3 Mbaud;
     *    the kernel baud_rates[] table has B3000000 ready to go.
     *
     *    ldisc_dbg_param / fm_dbg_param enable BT_LDISC_DBG / V4L2_FM_DRV_DBG
     *    output in dmesg (bit mask over V4L2_DBG_INIT/TX/RX/...). 0x1F =
     *    INIT+OPEN+CLOSE+TX+RX, which is what we want while bringing FM up on
     *    new hardware. The logs are loud but harmless; can be revisited once
     *    FM is actually playing. */
    /* skip_patchram=1 tells the kernel download_patchram() to bypass the
     * HCI_VSC_DOWNLOAD_MINIDRV + FC4C patchram upload, just set host tty
     * to custom_baudrate, and let the rest of brcm_sh_ldisc_start run
     * against the already-initialised chip. This is what allows hooking
     * up on top of stock bluedroid's init without re-POR'ing the combo
     * chip (which would kill WiFi). */
    if (sysfs_write("vendor_params",
                    "custom_baudrate=3000000 "
                    "skip_patchram=1 "
                    "ldisc_dbg_param=31 "
                    "fm_dbg_param=31") < 0)
        return 1;

    /* 5. Fork the UIM-mock child. */
    fflush(stdout);
    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        return 1;
    }
    if (child == 0)
        _exit(ldisc_child());

    /* Parent: let child settle, then verify it's still alive. */
    usleep(200 * 1000);
    int status;
    pid_t w = waitpid(child, &status, WNOHANG);
    if (w == child) {
        fprintf(stderr, "ERROR helper exited during startup (status=0x%x)\n",
                status);
        return 1;
    }

    FILE *f = fopen(PIDFILE, "w");
    if (f) {
        fprintf(f, "%d\n", (int)child);
        fclose(f);
    }
    printf("UIM helper pid=%d running; attach triggers on /dev/radio0 open\n",
           (int)child);
    printf("OK — now run: fm_v4l2 on [freq]\n");
    return 0;
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
        "                              " DEFAULT_FW_NAME "\n"
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
        const char *fw  = (argc > 2) ? argv[2] : DEFAULT_FW_NAME;
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
