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
#define main mega_format512_main
#endif

int main(int argc, char *argv[]) {
    int fd_dev, fd_mega, bus_no = 0, target;
    u8 inq_data[96];

    /* FORMAT UNIT short parameter list header (FMTDATA=1, LONGLIST=0).
       byte 0: PROTECTION FIELD USAGE = 0
       byte 1: FOV=0 (use drive defaults), IMMED=0
       bytes 2-3: DEFECT LIST LENGTH = 0 (we supply no defect descriptors)

       This used to be a 12-byte MODE SELECT block descriptor, which the drive
       parsed as "defect list length = 8" followed by two short-block defect
       descriptors - silently adding LBA 0 and LBA 512 to the grown defect list
       on every run. FORMAT UNIT has no block-size field; the sector size comes
       from a preceding MODE SELECT (see mega_modesel.c / mega_format_immed.c). */
    u8 format_param[4] = {0x00, 0x00, 0x00, 0x00};

    u8 inq_cdb[6] = {0x12, 0, 0, 0, 96, 0};
    u8 format_cdb[6] = {0x04, 0x10, 0, 0, 0, 0};

    if (argc < 3) {
        printf("MegaRAID FORMAT UNIT (uses the drive's CURRENT sector size)\n");
        printf("Usage: %s <block_device> <target_id>\n", argv[0]);
        printf("\n");
        printf("This sends FORMAT UNIT only. FORMAT UNIT has no block-size field,\n");
        printf("so it reformats at whatever size the drive's mode page already\n");
        printf("holds - on a 520-byte drive it destroys the data and leaves it at\n");
        printf("520. To change the sector size use mega_modesel (SSD) or\n");
        printf("mega_format_immed (HDD, and the only safe choice on slow drives).\n");
        return 1;
    }
    target = parse_target(argv[2]);
    if (target < 0) {
        fprintf(stderr, "Invalid target id '%s' - expected 0-255\n", argv[2]);
        return 1;
    }

    /* Line-buffer stdout so the destructive warning and countdown below are
       actually visible when stdout is a pipe (tee, script, ...) rather than
       sitting in a block buffer until the process exits. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    fd_dev = open(argv[1], O_RDWR | O_NONBLOCK);
    if (fd_dev < 0) { perror("open"); return 1; }
    if (ioctl(fd_dev, SCSI_IOCTL_GET_BUS_NUMBER, &bus_no) < 0) {
        perror("SCSI_IOCTL_GET_BUS_NUMBER");
        close(fd_dev);
        return 1;
    }
    close(fd_dev);

    fd_mega = open("/dev/megaraid_sas_ioctl_node", O_RDWR);
    if (fd_mega < 0) { perror("open megaraid"); return 1; }

    printf("Checking target %d on bus %d...\n", target, bus_no);
    memset(inq_data, 0, sizeof(inq_data));
    if (send_cmd(fd_mega, bus_no, target, inq_cdb, 6, inq_data, 96, MFI_FRAME_DIR_READ, NULL)) {
        printf("INQUIRY failed - wrong target?\n");
        close(fd_mega);
        return 1;
    }
    printf("Found: ");
    print_ascii(inq_data + 8, 8);
    putchar(' ');
    print_ascii(inq_data + 16, 16);
    printf("\n\n");

    /* Deliberately does NOT claim "to 512-byte sectors": no MODE SELECT is sent,
       so this reformats at whatever size the mode page already holds. */
    printf("*** FORMAT UNIT AT THE DRIVE'S CURRENT SECTOR SIZE IN 5 SECONDS ***\n");
    printf("*** This does NOT change 520 -> 512; use mega_format_immed for that ***\n");
    printf("*** ALL DATA WILL BE DESTROYED - Ctrl+C to abort ***\n\n");
    for (int i = 5; i > 0; i--) {
        printf("%d...\n", i);
        sleep(1);
    }

    printf("\nSending FORMAT UNIT command...\n");
    int rc = send_cmd(fd_mega, bus_no, target, format_cdb, 6, format_param, sizeof(format_param), MFI_FRAME_DIR_WRITE, NULL);

    if (rc == 0) {
        printf("\nFORMAT command accepted!\n");
        printf("Format in progress - this will take 30-60 minutes.\n");
        printf("Monitor with: smartctl -d megaraid,%d -a /dev/sda\n", target);
    } else if (rc < 0) {
        printf("\nFORMAT failed: the ioctl itself did not complete.\n");
    } else {
        printf("\nFORMAT failed with status: 0x%02x\n", rc);
        if (rc == 0x45)
            printf("(0x45 = MFI_STAT_SCSI_IO_FAILED - on a slow HDD this usually means the\n"
                   " controller timed out a blocking FORMAT UNIT. Use mega_format_immed\n"
                   " instead, which sets the IMMED bit. See README.)\n");
    }

    close(fd_mega);
    /* Exit status is the SCSI status, so it must survive truncation mod 256:
       returning a raw -1 would surface as 255 and collide with a real status. */
    return (rc == 0) ? 0 : 1;
}
