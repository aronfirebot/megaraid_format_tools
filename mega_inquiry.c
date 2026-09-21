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
#define main mega_inquiry_main
#endif

int main(int argc, char *argv[]) {
    struct megasas_iocpacket ioc;
    struct megasas_pthru_frame *pthru;
    u8 inq_data[96];
    int fd_dev, fd_mega, bus_no = 0, target;

    if (argc < 3) {
        printf("Usage: %s <block_device> <target_id>\n", argv[0]);
        return 1;
    }
    target = parse_target(argv[2]);
    if (target < 0) {
        fprintf(stderr, "Invalid target id '%s' - expected 0-255\n", argv[2]);
        return 1;
    }

    fd_dev = open(argv[1], O_RDWR | O_NONBLOCK);
    if (fd_dev < 0) { perror("open block device"); return 1; }

    if (ioctl(fd_dev, SCSI_IOCTL_GET_BUS_NUMBER, &bus_no) < 0) {
        perror("get bus number"); close(fd_dev); return 1;
    }
    printf("Bus: %d, Target: %d\n", bus_no, target);
    close(fd_dev);

    fd_mega = open("/dev/megaraid_sas_ioctl_node", O_RDWR);
    if (fd_mega < 0) { perror("open megaraid"); return 1; }

    memset(&ioc, 0, sizeof(ioc));
    memset(inq_data, 0, sizeof(inq_data));
    pthru = &ioc.frame.pthru;

    ioc.host_no = bus_no;
    ioc.sge_count = 1;
    ioc.sgl_off = offsetof(struct megasas_pthru_frame, sgl);
    ioc.sgl[0].iov_base = inq_data;
    ioc.sgl[0].iov_len = 96;

    pthru->cmd = MFI_CMD_PD_SCSI_IO;
    pthru->cmd_status = 0xFF;
    pthru->target_id = target;
    pthru->cdb_len = 6;
    pthru->sge_count = 1;
    pthru->flags = MFI_FRAME_DIR_READ;
    pthru->data_xfer_len = 96;
    pthru->sgl.sge32[0].phys_addr = (intptr_t)inq_data;
    pthru->sgl.sge32[0].length = 96;
    pthru->cdb[0] = 0x12;
    pthru->cdb[4] = 96;

    int rc = ioctl(fd_mega, MEGASAS_IOC_FIRMWARE, &ioc);
    /* scsi_status is deliberately not printed: the driver copies back only
       cmd_status (a single-byte copy_to_user of frame.hdr.cmd_status), so
       scsi_status still holds whatever our own memset left. On the tool people
       reach for when debugging, a fabricated 0x00 does the most damage.
       errno is only meaningful when the ioctl actually failed. */
    if (rc < 0)
        printf("ioctl=%d errno=%d (%s) cmd_status=0x%02x\n",
               rc, errno, strerror(errno), pthru->cmd_status);
    else
        printf("ioctl=%d cmd_status=0x%02x\n", rc, pthru->cmd_status);

    if (rc == 0 && pthru->cmd_status == 0) {
        printf("SUCCESS!\nVendor: ");
        print_ascii(inq_data + 8, 8);
        printf("\nProduct: ");
        print_ascii(inq_data + 16, 16);
        putchar('\n');
        close(fd_mega);
        return 0;
    }

    /* Used to return 0 unconditionally, so a failed INQUIRY against a bad
       target still looked like success to any script calling it. */
    printf("INQUIRY failed - wrong target, or passthrough not working.\n");
    close(fd_mega);
    return 1;
}
