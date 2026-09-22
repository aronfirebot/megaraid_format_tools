/*
 * mega_progress.c - poll FORMAT UNIT progress via MegaRAID passthrough.
 *
 * When a drive is formatted with the IMMED bit set (see mega_format_immed.c),
 * FORMAT UNIT returns immediately and the drive formats in the background.
 * This tool sends REQUEST SENSE and reports progress.
 *
 * While a format runs the drive answers REQUEST SENSE with:
 *   sense key 0x2 (NOT READY), ASC 0x04, ASCQ 0x04
 *   ("LOGICAL UNIT NOT READY, FORMAT IN PROGRESS")
 * plus a progress indication in the sense-key-specific field, scaled to 65536.
 *
 * We send REQUEST SENSE with DESC=0, so a conformant target must answer in
 * fixed format (response code 0x70/0x71) - the descriptor-format (0x72/0x73)
 * branch below is defensive hardening against a non-conformant drive, not a
 * path that should ever execute here. The progress bytes are only read when
 * the drive sets SKSV, and only for the sense keys where those bytes actually
 * mean progress.
 *
 * Once no format is in progress the tool issues READ CAPACITY(10) and reports
 * the drive's actual block size, rather than inferring completion from a
 * sense key of 0x0 (which equally means "nothing has happened yet").
 *
 * Exit status:
 *   0 = drive ready and at the expected block size (512 unless overridden)
 *   2 = drive ready but NOT at the expected block size (reformat did not
 *       take effect)
 *   3 = not confirmed complete (format still running, drive not ready, or a
 *       UNIT ATTENTION got in the way) - notably this is NOT success, because
 *       the thing it gates is power-cycling a drive that must not lose power
 *       mid-format
 *   1 = error (bad target, drive stopped responding, undecodable sense)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <scsi/sg.h>

#include "megaraid_common.h"

/* Consecutive failed REQUEST SENSE polls tolerated before giving up. */
#define MAX_SOFT_ERRORS        5
/* READ CAPACITY attempts once no format is in progress. */
#define READ_CAP_TRIES         3

/*
 * Decode REQUEST SENSE data. Fixed format (response code 0x70/0x71) is what
 * this tool actually receives, because we send DESC=0; the descriptor format
 * (0x72/0x73) path is defensive hardening for a non-conformant drive. The two
 * layouts put the sense key, ASC and ASCQ at different offsets.
 *
 * Returns 0 on success, -1 if the buffer is too short to hold a sense header or
 * the response code is not one of the four above.
 *
 * *progress receives a 0-65535 progress indication, or is left at -1. It is set
 * only when the drive actually supplied one - SKSV=1 for the fixed layout and
 * for descriptor type 0x02, or the presence of descriptor type 0x0A, which
 * carries progress with no SKSV bit - AND only for sense keys NO SENSE and NOT
 * READY, the two where those bytes mean progress at all. See the comment above
 * the gate at the bottom of this function for why that matters.
 */
static int parse_sense(const u8 *sense, size_t len,
                       u8 *sense_key, u8 *asc, u8 *ascq, int *progress) {
    u8 resp;
    int prog = -1;

    *progress = -1;

    if (len < 8)                                    /* no usable sense header */
        return -1;

    resp = sense[0] & 0x7F;

    if (resp == 0x70 || resp == 0x71) {             /* fixed format */
        /* Bound by the drive's own ADDITIONAL SENSE LENGTH (byte 7), the same
           way the descriptor branch below does. Checking against len alone
           would never fire - every caller passes sizeof(sense) - so it looked
           like validation without being any. Fields past the reported length
           are absent rather than zero, so report them as absent. */
        size_t valid = 8 + (size_t)sense[7];
        if (valid > len) valid = len;

        *sense_key = sense[2] & 0x0F;               /* always within the header */
        *asc  = (valid > 12) ? sense[12] : 0;
        *ascq = (valid > 13) ? sense[13] : 0;
        if (valid > 17 && (sense[15] & 0x80))       /* SKSV */
            prog = (sense[16] << 8) | sense[17];

    } else if (resp == 0x72 || resp == 0x73) {      /* descriptor format */
        *sense_key = sense[1] & 0x0F;
        *asc  = sense[2];
        *ascq = sense[3];

        /* Walk the descriptor list. Type 0x02 (sense-key specific) carries at
           its bytes 4-6 the same SKSV + progress field that lives at bytes
           15-17 in fixed format. Type 0x0A ("another progress indication")
           carries a progress value at its bytes 6-7 with no SKSV bit, and is
           the fallback sg_get_sense_progress_fld checks second. */
        size_t end = 8 + (size_t)sense[7];
        if (end > len) end = len;
        for (size_t i = 8; i + 1 < end; ) {
            size_t dlen = 2 + (size_t)sense[i + 1];
            if (i + dlen > end) break;
            if (sense[i] == 0x02 && dlen >= 7 && (sense[i + 4] & 0x80))
                prog = (sense[i + 5] << 8) | sense[i + 6];
            else if (sense[i] == 0x0A && dlen >= 8 && prog < 0)
                prog = (sense[i + 6] << 8) | sense[i + 7];
            i += dlen;
        }

    } else {
        return -1;
    }

    /* The sense-key-specific bytes are only a PROGRESS INDICATION for NO SENSE
       and NOT READY. Under ILLEGAL REQUEST the very same bytes are a field
       pointer, and under MEDIUM/HARDWARE ERROR they are an actual retry count -
       rendering either of those as "40% complete" would be worse than reporting
       no progress at all. sg_get_sense_progress_fld applies the same gate. */
    if (prog >= 0 && (*sense_key == 0x00 || *sense_key == 0x02))
        *progress = prog;

    return 0;
}

enum drive_state {
    STATE_FORMATTING,       /* 04/04 FORMAT IN PROGRESS */
    STATE_NOT_READY,        /* NOT READY for some other reason */
    STATE_BACKGROUND_OP,    /* progress reported without the 04/04 pair */
    STATE_UNIT_ATTENTION,   /* says nothing about whether the format finished */
    STATE_IDLE              /* nothing in progress - go ask READ CAPACITY */
};

/*
 * Decide what the drive is doing. Everything except STATE_IDLE means "not
 * confirmed complete", which is what stops the poller telling an operator it is
 * safe to power cycle a drive that is still formatting.
 *
 * Split out from the poll loop so it can be tested without a controller - this
 * is where the completion decision actually lives.
 */
static enum drive_state classify(u8 sense_key, u8 asc, u8 ascq, int progress) {
    /* FORMAT IN PROGRESS is keyed off ASC/ASCQ rather than the sense key. Most
       drives pair 04/04 with NOT READY (0x2), but some report a background
       operation with NO SENSE (0x0) and a valid progress field; requiring the
       key here would make those drives fall through and be reported as a
       finished format. */
    if (asc == 0x04 && ascq == 0x04)
        return STATE_FORMATTING;

    /* NOT READY for some other reason, e.g. 04/01 "becoming ready". */
    if (sense_key == 0x02)
        return STATE_NOT_READY;

    /* A progress indication without the 04/04 pair. parse_sense only surfaces
       progress for NO SENSE / NOT READY, so this is a drive running a
       background operation while keeping the LU accessible - still running,
       and emphatically not a finished format. */
    if (progress >= 0)
        return STATE_BACKGROUND_OP;

    /* UNIT ATTENTION (bus reset, mode parameters changed, ...) is a one-shot
       condition that clears once reported. It says nothing about whether the
       format finished, so re-poll rather than declaring the drive done. */
    if (sense_key == 0x06)
        return STATE_UNIT_ATTENTION;

    return STATE_IDLE;
}

/*
 * READ CAPACITY(10).
 *
 * Returns 0 on success, -1 on failure. Both outputs are device-controlled, so
 * they are assembled in u32 and returned through parameters rather than being
 * squeezed into the return value: a block length of 0xFFFFFFFF would otherwise
 * be indistinguishable from a -1 failure, and shifting a byte >= 0x80 into the
 * sign bit of an int is undefined behaviour.
 *
 * *last_lba receives the address of the LAST block, so capacity is
 * (*last_lba + 1) * block size. The value 0xFFFFFFFF is a saturation marker
 * meaning the drive is bigger than READ CAPACITY(10) can express - real
 * capacity then needs READ CAPACITY(16), which we do not send.
 */
static int read_capacity10(int fd, int bus, int target, u32 *last_lba, u32 *block_size) {
    u8 cdb[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    u8 cap[8];

    memset(cap, 0, sizeof(cap));
    if (send_cmd(fd, bus, target, cdb, 10, cap, sizeof(cap), MFI_FRAME_DIR_READ, NULL) != 0)
        return -1;

    *last_lba   = ((u32)cap[0] << 24) | ((u32)cap[1] << 16) | ((u32)cap[2] << 8) | cap[3];
    *block_size = ((u32)cap[4] << 24) | ((u32)cap[5] << 16) | ((u32)cap[6] << 8) | cap[7];

    /* A drive that transferred nothing leaves cap[] zeroed; reporting "block
       size 0 bytes" as a ready state would be worse than reporting failure. */
    if (*block_size == 0)
        return -1;

    return 0;
}

/*
 * Parse the expected block size (the value READ CAPACITY must report for
 * exit status 0). Distinct from megaraid_common.h's parse_block_size, which
 * parses a MODE SELECT block-length field for a different tool and a
 * different valid range - returns -1 on anything that is not a clean
 * 1-0xFFFFFFFE (0xFFFFFFFF is READ CAPACITY(10)'s own saturation marker,
 * never a real block size).
 */
static int64_t parse_expected_block_size(const char *s) {
    char *end;
    long long v;

    errno = 0;
    v = strtoll(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 1 || v > 0xFFFFFFFELL)
        return -1;
    return (int64_t)v;
}

/*
 * Decide the final exit status once no format is in progress: 0 when the
 * drive's actual block size (from READ CAPACITY) matches the requested one,
 * 2 otherwise. Split out of main()'s poll loop so the comparison that gates
 * "the reformat actually took effect" can be tested without a controller,
 * the same way classify() is.
 */
static int check_expected_block_size(u32 actual, u32 expected) {
    return (actual == expected) ? 0 : 2;
}

/* Built alone, this file's main() is the program entry point as always. Built
   as part of megaraid_tool (MEGA_MULTICALL), it is renamed so it can be
   linked alongside the other tools' own main()s without colliding; see
   megaraid_tool.c. */
#ifdef MEGA_MULTICALL
#define main mega_progress_main
#endif

int main(int argc, char *argv[]) {
    int fd_dev, fd_mega, bus_no = 0, target, interval = 0;
    u32 expected_bs = 512;
    u8 sense[96];
    u8 req_sense_cdb[6] = {0x03, 0x00, 0x00, 0x00, sizeof(sense), 0x00};

    if (argc < 3) {
        printf("MegaRAID FORMAT UNIT progress poller\n");
        printf("Usage: %s <block_device> <target_id> [interval_seconds] [expected_block_size]\n", argv[0]);
        printf("  With no interval, reports once and exits.\n");
        printf("  With an interval, polls until the format completes.\n");
        printf("  expected_block_size defaults to 512; pass 4096 if that's the target size.\n\n");
        printf("Exit status: 0 = ready at the expected block size, 2 = ready but at a\n");
        printf("             different size, 3 = not confirmed complete, 1 = error.\n");
        return 1;
    }
    target = parse_target(argv[2]);
    if (target < 0) {
        fprintf(stderr, "Invalid target id '%s' - expected 0-255\n", argv[2]);
        return 1;
    }
    if (argc > 3) interval = atoi(argv[3]);
    if (argc > 4) {
        int64_t bs = parse_expected_block_size(argv[4]);
        if (bs < 0) {
            fprintf(stderr, "Invalid expected block size '%s' - expected 1-4294967294\n", argv[4]);
            return 1;
        }
        expected_bs = (u32)bs;
        if (is_unusual_block_size(expected_bs))
            fprintf(stderr,
                    "WARNING: %u is not 512 or 4096 - those are the only sizes this repo has\n"
                    "         confirmed a MegaRAID/PERC controller will accept (see README).\n"
                    "         Continuing anyway; Ctrl+C now if that was a typo.\n",
                    expected_bs);
    }

    fd_dev = open(argv[1], O_RDWR | O_NONBLOCK);
    if (fd_dev < 0) { perror("open dev"); return 1; }
    if (ioctl(fd_dev, SCSI_IOCTL_GET_BUS_NUMBER, &bus_no) < 0) {
        perror("SCSI_IOCTL_GET_BUS_NUMBER");
        close(fd_dev);
        return 1;
    }
    close(fd_dev);

    fd_mega = open("/dev/megaraid_sas_ioctl_node", O_RDWR);
    if (fd_mega < 0) { perror("open megaraid"); return 1; }

    int soft_errors = 0;

    for (;;) {
        u8 sense_key = 0, asc = 0, ascq = 0;
        int progress = -1;
        enum drive_state state;

        memset(sense, 0, sizeof(sense));
        int rc = send_cmd(fd_mega, bus_no, target, req_sense_cdb, 6, sense, sizeof(sense), MFI_FRAME_DIR_READ, NULL);

        if (rc != 0 || parse_sense(sense, sizeof(sense), &sense_key, &asc, &ascq, &progress) < 0) {
            if (rc != 0)
                printf("REQUEST SENSE failed (status 0x%02x)\n", rc);
            else
                printf("Unrecognized sense response code 0x%02x - cannot decode.\n", sense[0] & 0x7F);

            /* One failed poll is not evidence the format died - the controller
               can be momentarily busy, and giving up here would abandon a watch
               that may be hours old. Only conclude failure after several in a
               row, and only while polling; a one-shot run just reports. */
            if (interval > 0 && ++soft_errors < MAX_SOFT_ERRORS) {
                printf("  (transient? retry %d/%d in %ds)\n", soft_errors, MAX_SOFT_ERRORS, interval);
                sleep(interval);
                continue;
            }
            printf("Giving up - wrong target, or the drive has stopped responding.\n");
            close(fd_mega);
            return 1;
        }
        soft_errors = 0;

        state = classify(sense_key, asc, ascq, progress);

        switch (state) {
        case STATE_FORMATTING:
            if (progress >= 0)
                printf("FORMAT IN PROGRESS: %.1f%% (%d/65536)\n",
                       progress * 100.0 / 65536.0, progress);
            else
                printf("FORMAT IN PROGRESS (drive supplied no progress indication)\n");
            break;
        case STATE_NOT_READY:
            printf("Drive NOT READY (ASC=0x%02x ASCQ=0x%02x) - not finished\n", asc, ascq);
            break;
        case STATE_BACKGROUND_OP:
            printf("Background operation in progress: %.1f%% (%d/65536)\n",
                   progress * 100.0 / 65536.0, progress);
            break;
        case STATE_UNIT_ATTENTION:
            printf("UNIT ATTENTION (ASC=0x%02x ASCQ=0x%02x) - re-checking\n", asc, ascq);
            break;
        case STATE_IDLE:
            break;
        }

        if (state != STATE_IDLE) {
            if (interval <= 0) {
                close(fd_mega);
                return 3;       /* not confirmed complete - see exit status note */
            }
            sleep(interval);
            continue;
        }

        /* Nothing in progress. Do NOT infer "complete" from sense key 0x0 - NO
           SENSE equally means "nothing has happened yet", so a poll issued
           before the format starts would report success. Ask the drive what its
           block size actually is instead; that is the thing we care about. */
        if (sense_key != 0x00)
            printf("sense key=0x%x ASC=0x%02x ASCQ=0x%02x (no format in progress)\n",
                   sense_key, asc, ascq);

        u32 last_lba = 0, bs = 0;
        int cap_ok = -1;
        for (int attempt = 1; attempt <= READ_CAP_TRIES; attempt++) {
            cap_ok = read_capacity10(fd_mega, bus_no, target, &last_lba, &bs);
            if (cap_ok == 0)
                break;
            /* Format completion is exactly when a UNIT ATTENTION is most likely
               to be pending, and the first READ CAPACITY tends to absorb it.
               Retry before concluding anything - this point may be hours into
               an unattended watch. */
            if (attempt < READ_CAP_TRIES)
                sleep(1);
        }
        if (cap_ok != 0) {
            printf("No format in progress, but READ CAPACITY failed %d times - drive not ready.\n",
                   READ_CAP_TRIES);
            close(fd_mega);
            return 1;
        }

        printf("Drive ready: block size %u bytes", bs);
        if (last_lba == 0xFFFFFFFFu)
            printf(", capacity exceeds READ CAPACITY(10) range\n");
        else
            printf(", %u blocks (%.2f TB)\n", last_lba + 1,
                   (last_lba + 1.0) * bs / 1e12);

        int status = check_expected_block_size(bs, expected_bs);
        if (status != 0)
            printf("NOTE: block size is %u, not %u - the reformat has not taken effect.\n", bs, expected_bs);

        close(fd_mega);
        return status;
    }
}
