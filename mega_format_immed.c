/*
 * mega_format_immed.c - reformat via MegaRAID passthrough, IMMED (e.g. a
 * 520-byte enterprise sector size to 512 or 4096).
 *
 * Like mega_modesel.c (MODE SELECT to the target block size, then FORMAT
 * UNIT) but the
 * FORMAT UNIT is sent with the IMMED bit set in the parameter-list header.
 *
 * Why IMMED matters: without it, FORMAT UNIT does not return until the whole
 * format finishes. On a fast SSD that can complete before the controller's
 * command timeout, so the older tools "work" (though they may still report
 * SCSI_IO_FAILED / status 45). On a slow multi-TB 7200rpm SAS HDD the format
 * takes hours, the RAID controller's command timeout fires, and the command is
 * aborted with SCSI_IO_FAILED - leaving the medium HALF-FORMATTED and invalid
 * (SMART self-test returns I/O error; controller reports 0 KB / UBad).
 *
 * With IMMED=1 the drive validates the request, returns immediately, and formats
 * in the BACKGROUND. Poll progress with mega_progress.c. This makes the reformat
 * deterministic across both SSDs and slow HDDs.
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

/* Built alone, this file's main() is the program entry point as always. Built
   as part of megaraid_tool (MEGA_MULTICALL), it is renamed so it can be
   linked alongside the other tools' own main()s without colliding; see
   megaraid_tool.c. */
#ifdef MEGA_MULTICALL
#define main mega_format_immed_main
/* Same binary also provides "progress" as a subcommand. Recommending that
   instead of the standalone ./mega_progress avoids pointing an operator at a
   tool that won't exist if only this unified binary was copied to a server. */
#define PROGRESS_CMD "./megaraid_tool progress"
#else
#define PROGRESS_CMD "./mega_progress"
#endif

int main(int argc, char *argv[]) {
    int fd_dev, fd_mega, bus_no = 0, target, block_size;
    u8 inq_data[96];

    /* MODE SELECT(6): header(4) + block descriptor(8); bytes 9-11 (block
       length) are filled in below once block_size is known. */
    u8 mode_sel_data[12] = {
        0x00, 0x00, 0x00, 0x08,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    };
    u8 mode_sel_cdb[6] = {0x15, 0x10, 0x00, 0x00, 12, 0x00};   /* PF=1, SP=0, param len 12 */

    /* FORMAT UNIT: FMTPINFO=00 (no protection info), FMTDATA=1 (byte1 = 0x10) */
    u8 format_cdb[6] = {0x04, 0x10, 0x00, 0x00, 0x00, 0x00};
    /* Parameter list header (short): byte1 IMMED=1 (0x02); defect list length 0 */
    u8 format_param[4] = {0x00, 0x02, 0x00, 0x00};

    u8 inq_cdb[6] = {0x12, 0, 0, 0, 96, 0};

    if (argc < 3) {
        printf("MegaRAID Drive Formatter, IMMED (520-byte -> 512 or 4096-byte sectors)\n");
        printf("Usage: %s <block_device> <target_id> [block_size]\n", argv[0]);
        printf("  <block_device> any drive on the same controller (e.g. /dev/sda);\n");
        printf("                 used only to find the host number, never written to.\n");
        printf("  <target_id>    MegaRAID target id of the drive to format.\n");
        printf("  [block_size]   sector size to set, e.g. 512 or 4096 (default 512).\n");
        return 1;
    }
    target = parse_target(argv[2]);
    if (target < 0) {
        fprintf(stderr, "Invalid target id '%s' - expected 0-255\n", argv[2]);
        return 1;
    }
    block_size = 512;
    if (argc > 3) {
        block_size = parse_block_size(argv[3]);
        if (block_size < 0) {
            fprintf(stderr, "Invalid block size '%s' - expected 1-16777215\n", argv[3]);
            return 1;
        }
    }
    if (is_unusual_block_size(block_size))
        fprintf(stderr,
                "WARNING: %d is not 512 or 4096 - those are the only sizes this repo has\n"
                "         confirmed a MegaRAID/PERC controller will accept via passthrough\n"
                "         (see README). Continuing anyway; Ctrl+C now if that was a typo.\n",
                block_size);
    set_mode_sel_block_length(mode_sel_data, block_size);

    /* Line-buffer stdout: the destructive warning and the abort countdown below
       are useless if they sit in a block buffer until exit, which is what
       happens whenever stdout is a pipe (running under tee, script, etc). */
    setvbuf(stdout, NULL, _IOLBF, 0);

    fd_dev = open(argv[1], O_RDWR | O_NONBLOCK);
    if (fd_dev < 0) { perror("open dev"); return 1; }
    /* Must be checked: bus_no defaults to 0, so a failure here would silently
       send a destructive FORMAT to target <target_id> on host 0 - potentially a
       different controller than the one the user named. */
    if (ioctl(fd_dev, SCSI_IOCTL_GET_BUS_NUMBER, &bus_no) < 0) {
        perror("SCSI_IOCTL_GET_BUS_NUMBER");
        close(fd_dev);
        return 1;
    }
    close(fd_dev);

    fd_mega = open("/dev/megaraid_sas_ioctl_node", O_RDWR);
    if (fd_mega < 0) { perror("open megaraid"); return 1; }

    printf("Target %d on bus %d\n", target, bus_no);
    memset(inq_data, 0, sizeof(inq_data));
    if (send_cmd(fd_mega, bus_no, target, inq_cdb, 6, inq_data, 96, MFI_FRAME_DIR_READ, "INQUIRY")) {
        printf("INQUIRY failed - wrong target?\n");
        close(fd_mega);
        return 1;
    }
    printf("Found: ");
    print_ascii(inq_data + 8, 8);
    putchar(' ');
    print_ascii(inq_data + 16, 16);
    printf("\n\n");

    /* MODE SELECT first. It is not the destructive step - FORMAT UNIT is - so
       run it before the abort countdown. That way the countdown is the LAST
       thing before data loss and the user can still bail out having seen
       whether the drive actually accepted 512-byte sectors. */
    printf("Step 1: MODE SELECT - set block size to %d\n", block_size);
    int rc = send_cmd(fd_mega, bus_no, target, mode_sel_cdb, 6, mode_sel_data, 12, MFI_FRAME_DIR_WRITE, "MODE SELECT");

    int countdown = 5;
    if (rc != 0) {
        printf("\n*** MODE SELECT FAILED (status 0x%02x) ***\n", rc);
        printf("The drive has NOT accepted %d-byte sectors. Formatting now will\n", block_size);
        printf("destroy all data and may still leave the drive at its old size.\n");
        printf("Some drives do take the new size from FORMAT UNIT anyway, so this\n");
        printf("is not always fatal - but continue only if that is what you want.\n");
        countdown = 15;
    }

    printf("\n*** FORMATTING TO %d-BYTE SECTORS IN %d SECONDS ***\n", block_size, countdown);
    printf("*** ALL DATA WILL BE DESTROYED - Ctrl+C to abort ***\n\n");
    for (int i = countdown; i > 0; i--) { printf("%d...\n", i); sleep(1); }

    printf("\nStep 2: FORMAT UNIT with IMMED=1 (returns immediately)\n");
    rc = send_cmd(fd_mega, bus_no, target, format_cdb, 6, format_param, sizeof(format_param), MFI_FRAME_DIR_WRITE, "FORMAT UNIT");

    if (rc == 0) {
        printf("\nAccepted. Drive is now formatting in the BACKGROUND (can take hours\n");
        printf("on a multi-TB HDD). Do NOT power off until it finishes.\n");
        printf("Monitor progress with:\n");
        printf("  %s %s %d 60 %d\n", PROGRESS_CMD, argv[1], target, block_size);
        printf("When done, clear the controller's stale cache (see README) and verify:\n");
        printf("  smartctl -d megaraid,%d -i /dev/sda | grep -i 'block size'\n", target);
    } else {
        printf("\nFORMAT UNIT not accepted (status 0x%02x). Inspect sense:\n", rc);
        /* interval 0 keeps this a one-shot check (report once and exit), same
           as before - just with the requested block size carried through so a
           copy-pasted command doesn't silently check against the 512 default. */
        printf("  %s %s %d 0 %d\n", PROGRESS_CMD, argv[1], target, block_size);
    }

    /* Exit status must survive truncation mod 256: returning a raw -1 would
       surface as 255, and a raw SCSI status could collide with mega_progress's
       exit codes (2 = wrong block size). Report success or failure only. */
    close(fd_mega);
    return (rc == 0) ? 0 : 1;
}
