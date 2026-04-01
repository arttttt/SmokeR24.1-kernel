/*
 * hci_fm — Minimal HCI VSC tool for BCM FM radio on Android
 *
 * Sends HCI Vendor-Specific Commands (opcode 0xFC15) to Broadcom FM receiver
 * via raw Bluetooth HCI socket. Works alongside running Android BT stack.
 *
 * Build (static, for Android ARM):
 *   arm-linux-gnueabihf-gcc -static -o hci_fm hci_fm.c
 *   or with Android NDK:
 *   $NDK/toolchains/arm-linux-androideabi-4.9/prebuilt/linux-x86_64/bin/arm-linux-androideabi-gcc \
 *       --sysroot=$NDK/platforms/android-21/arch-arm -static -o hci_fm hci_fm.c
 *
 * Usage:
 *   hci_fm write <reg_hex> <val16_dec>   Write 16-bit value to FM register
 *   hci_fm write8 <reg_hex> <val8_dec>   Write 8-bit value to FM register
 *   hci_fm read <reg_hex> [1|2]          Read FM register (default: 2 bytes)
 *   hci_fm raw <byte0> <byte1> ...       Send raw HCI VSC 0xFC15 payload
 *   hci_fm vsc <ocf_hex> <byte0> ...     Send arbitrary VSC with custom OCF
 *
 * Output:
 *   OK [hex bytes...]                    Success + response data
 *   ERROR <message>                      Failure
 *
 * Copyright (c) 2026 arttttt
 * License: GPL-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/poll.h>

/* ========================================================================
 * Bluetooth HCI definitions (inline to avoid header dependency)
 * ======================================================================== */

#define AF_BLUETOOTH    31
#define BTPROTO_HCI     1

#define SOL_HCI         0
#define HCI_FILTER      2

/* HCI packet types */
#define HCI_COMMAND_PKT 0x01
#define HCI_EVENT_PKT   0x04

/* HCI events */
#define HCI_EV_CMD_COMPLETE 0x0E
#define HCI_EV_VENDOR       0xFF

/* Broadcom FM constants */
#define OGF_VENDOR      0x3F
#define OCF_FM          0x0015
#define FM_OPCODE       ((OGF_VENDOR << 10) | OCF_FM)  /* 0xFC15 */

/* HCI socket address */
struct sockaddr_hci {
    unsigned short hci_family;
    unsigned short hci_dev;
    unsigned short hci_channel;
};

/* HCI filter */
struct hci_filter {
    unsigned int type_mask;
    unsigned int event_mask[2];
    unsigned short opcode;
};

static inline void hci_filter_clear(struct hci_filter *f) {
    memset(f, 0, sizeof(*f));
}
static inline void hci_filter_set_ptype(int t, struct hci_filter *f) {
    f->type_mask |= (1 << t);
}
static inline void hci_filter_set_event(int e, struct hci_filter *f) {
    int byte = e >> 5;
    f->event_mask[byte] |= (1 << (e & 31));
}

/* ========================================================================
 * HCI communication
 * ======================================================================== */

#define HCI_DEV         0       /* hci0 */
#define RESP_TIMEOUT_MS 3000
#define MAX_RESP_LEN    260

static int hci_fd = -1;

static int hci_open(void)
{
    struct sockaddr_hci addr;
    struct hci_filter filt;
    int fd;

    fd = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
    if (fd < 0) {
        fprintf(stderr, "ERROR socket(AF_BLUETOOTH): %s\n", strerror(errno));
        if (errno == EPERM)
            fprintf(stderr, "ERROR Need root. Run with su.\n");
        if (errno == EAFNOSUPPORT)
            fprintf(stderr, "ERROR Kernel has no AF_BLUETOOTH. Is CONFIG_BT=y?\n");
        return -1;
    }

    /* Bind to hci0 */
    memset(&addr, 0, sizeof(addr));
    addr.hci_family = AF_BLUETOOTH;
    addr.hci_dev = HCI_DEV;
    addr.hci_channel = 0; /* RAW channel */

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "ERROR bind(hci%d): %s\n", HCI_DEV, strerror(errno));
        close(fd);
        return -1;
    }

    /* Set filter: only command complete (0x0E) and vendor events (0xFF) */
    hci_filter_clear(&filt);
    hci_filter_set_ptype(HCI_EVENT_PKT, &filt);
    hci_filter_set_event(HCI_EV_CMD_COMPLETE, &filt);
    hci_filter_set_event(HCI_EV_VENDOR, &filt);

    if (setsockopt(fd, SOL_HCI, HCI_FILTER, &filt, sizeof(filt)) < 0) {
        fprintf(stderr, "ERROR setsockopt(HCI_FILTER): %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    hci_fd = fd;
    return 0;
}

static void hci_close(void)
{
    if (hci_fd >= 0) {
        close(hci_fd);
        hci_fd = -1;
    }
}

/* Send raw bytes and wait for response.
 * cmd_buf includes the HCI packet type byte (0x01) at offset 0.
 * Returns response length or -1 on error. */
static int hci_send_recv(const unsigned char *cmd_buf, int cmd_len,
                         unsigned char *resp_buf, int resp_max)
{
    struct pollfd pfd;
    int n;

    /* Send */
    n = write(hci_fd, cmd_buf, cmd_len);
    if (n < 0) {
        fprintf(stderr, "ERROR write: %s\n", strerror(errno));
        return -1;
    }
    if (n != cmd_len) {
        fprintf(stderr, "ERROR short write: %d/%d\n", n, cmd_len);
        return -1;
    }

    /* Wait for response */
    pfd.fd = hci_fd;
    pfd.events = POLLIN;

    n = poll(&pfd, 1, RESP_TIMEOUT_MS);
    if (n < 0) {
        fprintf(stderr, "ERROR poll: %s\n", strerror(errno));
        return -1;
    }
    if (n == 0) {
        fprintf(stderr, "ERROR timeout waiting for HCI response (%dms)\n", RESP_TIMEOUT_MS);
        return -1;
    }

    /* Read response */
    n = read(hci_fd, resp_buf, resp_max);
    if (n < 0) {
        fprintf(stderr, "ERROR read: %s\n", strerror(errno));
        return -1;
    }

    return n;
}

/* ========================================================================
 * FM command builders
 * ======================================================================== */

/* Build and send FM register write/read command.
 * Returns: 0 on success, -1 on error.
 * On success, resp_data/resp_len contain response payload (after headers). */
static int fm_reg_cmd(unsigned char reg, int is_read,
                      const unsigned char *data, int data_len,
                      unsigned char *resp_data, int *resp_len)
{
    unsigned char cmd[32];
    unsigned char resp[MAX_RESP_LEN];
    int cmd_len, n;

    /* Build HCI command:
     *   [0]     = 0x01 (HCI command pkt type)
     *   [1..2]  = opcode 0xFC15 (LE: 0x15, 0xFC)
     *   [3]     = parameter length = data_len + 2
     *   [4]     = FM register address
     *   [5]     = 0x00=write, 0x01=read
     *   [6..]   = data bytes
     */
    cmd[0] = HCI_COMMAND_PKT;
    cmd[1] = FM_OPCODE & 0xFF;         /* 0x15 */
    cmd[2] = (FM_OPCODE >> 8) & 0xFF;  /* 0xFC */
    cmd[3] = data_len + 2;             /* param total length */
    cmd[4] = reg;                      /* FM register address */
    cmd[5] = is_read ? 0x01 : 0x00;   /* read/write */

    if (data_len > 0 && data != NULL)
        memcpy(&cmd[6], data, data_len);

    cmd_len = 6 + data_len;

    n = hci_send_recv(cmd, cmd_len, resp, sizeof(resp));
    if (n < 0)
        return -1;

    /* Parse response.
     * For cmd complete (0x0E):
     *   [0]  = 0x04 (HCI event pkt type — may or may not be present depending on kernel)
     *   Then: event_code(1) + plen(1) + num_pkts(1) + opcode(2) + status(1) + fm_reg(1) + rdwr(1) + data...
     *
     * We search for the opcode 0x15 0xFC in the response to find our data.
     */
    int offset = -1;
    int i;
    for (i = 0; i < n - 1; i++) {
        if (resp[i] == 0x15 && resp[i+1] == 0xFC) {
            offset = i + 2; /* skip opcode, now at status byte */
            break;
        }
    }

    if (offset < 0 || offset >= n) {
        /* Maybe it's a vendor event (0xFF) */
        for (i = 0; i < n; i++) {
            if (resp[i] == 0xFF && i + 2 < n && resp[i+2] == 0x08) {
                /* FM interrupt event */
                printf("EVENT FM_INTERRUPT\n");
                return 0;
            }
        }
        fprintf(stderr, "ERROR could not parse HCI response (%d bytes):", n);
        for (i = 0; i < n; i++)
            fprintf(stderr, " %02X", resp[i]);
        fprintf(stderr, "\n");
        return -1;
    }

    unsigned char status = resp[offset];
    if (status != 0x00) {
        fprintf(stderr, "ERROR FM command failed, status=0x%02X\n", status);
        return -1;
    }

    /* Skip status(1) + fm_reg(1) + rdwr(1) = 3 bytes to get to data */
    int data_offset = offset + 3;
    int data_avail = n - data_offset;

    if (data_avail < 0)
        data_avail = 0;

    if (resp_data && resp_len) {
        int copy = data_avail;
        if (copy > 16) copy = 16; /* safety limit */
        memcpy(resp_data, &resp[data_offset], copy);
        *resp_len = copy;
    }

    return 0;
}

/* Send arbitrary VSC with custom OCF */
static int vsc_cmd(unsigned short ocf,
                   const unsigned char *data, int data_len,
                   unsigned char *resp_data, int *resp_len)
{
    unsigned char cmd[64];
    unsigned char resp[MAX_RESP_LEN];
    unsigned short opcode = (OGF_VENDOR << 10) | ocf;
    int n;

    cmd[0] = HCI_COMMAND_PKT;
    cmd[1] = opcode & 0xFF;
    cmd[2] = (opcode >> 8) & 0xFF;
    cmd[3] = data_len;
    if (data_len > 0)
        memcpy(&cmd[4], data, data_len);

    n = hci_send_recv(cmd, 4 + data_len, resp, sizeof(resp));
    if (n < 0)
        return -1;

    /* Print all response bytes */
    printf("OK");
    int i;
    for (i = 0; i < n; i++)
        printf(" %02X", resp[i]);
    printf("\n");

    return 0;
}

/* ========================================================================
 * CLI
 * ======================================================================== */

static unsigned long parse_hex(const char *s)
{
    return strtoul(s, NULL, 0); /* handles 0x prefix automatically */
}

static void usage(void)
{
    fprintf(stderr,
        "hci_fm — BCM FM radio HCI tool\n"
        "\n"
        "Usage:\n"
        "  hci_fm write <reg> <val16>     Write 16-bit to FM register\n"
        "  hci_fm write8 <reg> <val8>     Write 8-bit to FM register\n"
        "  hci_fm read <reg> [1|2]        Read FM register (default 2 bytes)\n"
        "  hci_fm raw <b0> <b1> ...       Send raw 0xFC15 payload bytes\n"
        "  hci_fm vsc <ocf> <b0> ...      Send arbitrary VSC\n"
        "\n"
        "  <reg> is hex (e.g., 0x0A for FM_FREQ)\n"
        "  <val> is decimal or hex with 0x prefix\n"
        "\n"
        "Output:\n"
        "  OK [hex bytes]    Success\n"
        "  ERROR message     Failure\n"
        "\n"
        "Examples:\n"
        "  hci_fm write 0x00 1           # FM ON (RDS_SYS=1)\n"
        "  hci_fm write 0x0A 37000       # Set freq 101.0 MHz\n"
        "  hci_fm write 0x09 1           # Trigger tune\n"
        "  hci_fm read 0x0F              # Read RSSI\n"
        "  hci_fm read 0x0A              # Read current frequency\n"
        "  hci_fm write 0xF8 180         # Set volume to 180\n"
        "  hci_fm write 0x00 0           # FM OFF\n"
    );
}

int main(int argc, char *argv[])
{
    unsigned char resp_data[16];
    int resp_len = 0;
    int ret;

    if (argc < 2) {
        usage();
        return 1;
    }

    if (hci_open() < 0)
        return 1;

    if (strcmp(argv[1], "write") == 0) {
        if (argc < 4) {
            fprintf(stderr, "ERROR usage: hci_fm write <reg> <val16>\n");
            ret = 1; goto out;
        }
        unsigned char reg = parse_hex(argv[2]);
        unsigned short val = (unsigned short)parse_hex(argv[3]);
        unsigned char data[2] = { val & 0xFF, (val >> 8) & 0xFF };

        ret = fm_reg_cmd(reg, 0, data, 2, resp_data, &resp_len);
        if (ret == 0) {
            printf("OK");
            int i;
            for (i = 0; i < resp_len; i++)
                printf(" %02X", resp_data[i]);
            printf("\n");
        }
    }
    else if (strcmp(argv[1], "write8") == 0) {
        if (argc < 4) {
            fprintf(stderr, "ERROR usage: hci_fm write8 <reg> <val8>\n");
            ret = 1; goto out;
        }
        unsigned char reg = parse_hex(argv[2]);
        unsigned char val = (unsigned char)parse_hex(argv[3]);
        unsigned char data[2] = { val, 0x00 };

        ret = fm_reg_cmd(reg, 0, data, 2, resp_data, &resp_len);
        if (ret == 0) {
            printf("OK");
            int i;
            for (i = 0; i < resp_len; i++)
                printf(" %02X", resp_data[i]);
            printf("\n");
        }
    }
    else if (strcmp(argv[1], "read") == 0) {
        if (argc < 3) {
            fprintf(stderr, "ERROR usage: hci_fm read <reg> [1|2]\n");
            ret = 1; goto out;
        }
        unsigned char reg = parse_hex(argv[2]);
        unsigned char read_len = (argc > 3) ? atoi(argv[3]) : 2;
        unsigned char data[2] = { read_len, 0x00 };

        ret = fm_reg_cmd(reg, 1, data, 1, resp_data, &resp_len);
        if (ret == 0) {
            printf("OK");
            int i;
            for (i = 0; i < resp_len; i++)
                printf(" %02X", resp_data[i]);
            printf("\n");
        }
    }
    else if (strcmp(argv[1], "raw") == 0) {
        if (argc < 3) {
            fprintf(stderr, "ERROR usage: hci_fm raw <byte0> <byte1> ...\n");
            ret = 1; goto out;
        }
        unsigned char payload[32];
        int plen = 0;
        int i;
        for (i = 2; i < argc && plen < (int)sizeof(payload); i++)
            payload[plen++] = (unsigned char)parse_hex(argv[i]);

        /* raw sends as 0xFC15 with given payload */
        unsigned char cmd[64];
        unsigned char resp[MAX_RESP_LEN];
        cmd[0] = HCI_COMMAND_PKT;
        cmd[1] = FM_OPCODE & 0xFF;
        cmd[2] = (FM_OPCODE >> 8) & 0xFF;
        cmd[3] = plen;
        memcpy(&cmd[4], payload, plen);

        int n = hci_send_recv(cmd, 4 + plen, resp, sizeof(resp));
        if (n < 0) { ret = 1; goto out; }
        printf("OK");
        for (i = 0; i < n; i++)
            printf(" %02X", resp[i]);
        printf("\n");
        ret = 0;
    }
    else if (strcmp(argv[1], "vsc") == 0) {
        if (argc < 3) {
            fprintf(stderr, "ERROR usage: hci_fm vsc <ocf> [bytes...]\n");
            ret = 1; goto out;
        }
        unsigned short ocf = (unsigned short)parse_hex(argv[2]);
        unsigned char payload[32];
        int plen = 0;
        int i;
        for (i = 3; i < argc && plen < (int)sizeof(payload); i++)
            payload[plen++] = (unsigned char)parse_hex(argv[i]);

        ret = vsc_cmd(ocf, payload, plen, resp_data, &resp_len);
    }
    else {
        usage();
        ret = 1;
    }

out:
    hci_close();
    return ret ? 1 : 0;
}
