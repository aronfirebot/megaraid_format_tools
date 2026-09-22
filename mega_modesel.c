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
#define main mega_modesel_main
#endif

int main(int argc, char *argv[]) {
    int fd_dev, fd_mega, bus_no = 0, target, block_size;
    u8 inq_data[96];
    u8 inq_cdb[6] = {0x12, 0, 0, 0, 96, 0};

    /* MODE SELECT(6) parameter: header(4) + block descriptor(8) = 12 bytes.
       Bytes 9-11 (block length) are filled in below once block_size is known. */
    u8 mode_sel_data[12] = {
        0x00,                   /* Mode data length (ignored for MODE SELECT) */
        0x00,                   /* Medium type */
        0x00,                   /* Device-specific parameter */
        0x08,                   /* Block descriptor length = 8 */
        0x00, 0x00, 0x00, 0x00, /* Number of blocks (0 = use drive default) */
        0x00,                   /* Reserved */
        0x00, 0x00, 0x00        /* Block length - set below */
    };

    /* MODE SELECT(6) CDB: PF=1 (page format), SP=0 */
    u8 mode_sel_cdb[6] = {0x15, 0x10, 0x00, 0x00, 12, 0x00};

    /* FORMAT UNIT CDB - no data, use mode page settings */
    u8 format_cdb[6] = {0x04, 0x00, 0x00, 0x00, 0x00, 0x00};

    if (argc < 3) {
        printf("MegaRAID MODE SELECT + FORMAT UNIT (520-byte -> 512 or 4096-byte sectors)\n");
        printf("Usage: %s <block_device> <target_id> [block_size]\n", argv[0]);
        printf("  <block_device> any drive on the same controller (e.g. /dev/sda);\n");
        printf("                 used only to find the host number, never written to.\n");
        printf("  <target_id>    MegaRAID target id of the drive to format.\n");
        printf("  [block_size]   sector size to set, e.g. 512 or 4096 (default 512).\n");
        printf("\n");
        printf("Sends a BLOCKING FORMAT UNIT. Safe on SSDs; on a slow or multi-TB\n");
        printf("HDD use mega_format_immed instead - see README, \"The IMMED bit\".\n");
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

    /* Line-buffer stdout so the warning and countdown below reach the terminal
       as they happen rather than at exit when stdout is a pipe (tee, script). */
    setvbuf(stdout, NULL, _IOLBF, 0);

    fd_dev = open(argv[1], O_RDWR | O_NONBLOCK);
    if (fd_dev < 0) { perror("open dev"); return 1; }
    if (ioctl(fd_dev, SCSI_IOCTL_GET_BUS_NUMBER, &bus_no) < 0) {
        perror("SCSI_IOCTL_GET_BUS_NUMBER");
        close(fd_dev);
        return 1;
    }
    close(fd_dev);

    fd_mega = open("/dev/megaraid_sas_ioctl_node", O_RDWR);
    if (fd_mega < 0) { perror("open"); return 1; }

    printf("Target %d bus %d\n\n", target, bus_no);

    /* Confirm which drive this actually is before destroying it. This tool used
       to go straight from argv to FORMAT UNIT with no identity check and no
       abort window, so a mistyped target formatted a different drive silently. */
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

    printf("Step 1: MODE SELECT - set block size to %d\n", block_size);
    int rc = send_cmd(fd_mega, bus_no, target, mode_sel_cdb, 6, mode_sel_data, 12, MFI_FRAME_DIR_WRITE, "MODE SELECT");

    if (rc != 0) {
        printf("\nMODE SELECT failed (status 0x%02x) - FORMAT UNIT not sent.\n", rc);
        close(fd_mega);
        /* Used to return 0 here, reporting success for a drive never touched. */
        return 1;
    }

    printf("\n*** FORMATTING TO %d-BYTE SECTORS IN 5 SECONDS ***\n", block_size);
    printf("*** ALL DATA WILL BE DESTROYED - Ctrl+C to abort ***\n\n");
    for (int i = 5; i > 0; i--) { printf("%d...\n", i); sleep(1); }

    printf("\nStep 2: FORMAT UNIT - apply new settings\n");
    rc = send_cmd(fd_mega, bus_no, target, format_cdb, 6, NULL, 0, 0, "FORMAT UNIT");

    /* Exit status must survive truncation mod 256: returning a raw -1 would
       surface as 255, and a raw SCSI status could collide with mega_progress's
       exit codes (2 = wrong block size). Report success or failure only. */
    close(fd_mega);
    return (rc == 0) ? 0 : 1;
}
