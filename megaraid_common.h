#ifndef MEGARAID_COMMON_H
#define MEGARAID_COMMON_H

/*
 * megaraid_common.h - single source of truth for the megasas ioctl ABI and
 * the handful of helpers every mega_*.c tool needs.
 *
 * Each tool remains a single translation unit compiled on its own exactly as
 * README.md shows (`gcc -o mega_inquiry mega_inquiry.c`, etc.) - this header
 * just needs to sit next to it, which it always does since it ships in the
 * same checkout. What changes from the old approach (each tool carrying its
 * own copy of these structs/helpers) is that the copies can no longer drift
 * apart: there is exactly one definition of each, so mismatched struct
 * layouts or divergent helper behavior between tools are compile-time
 * impossible rather than something CI has to hash-compare for.
 *
 * Every function below is `static inline`. Each mega_*.c / check_size.c is
 * still its own translation unit - including when linked together into the
 * megaraid_tool multicall binary, see megaraid_tool.c - so `static` keeps
 * each TU's copy private with internal linkage, exactly as the old per-file
 * copies did; `inline` additionally tells GCC not to warn under
 * -Wall -Wextra when a given tool doesn't call every helper (e.g.
 * check_size.c never calls send_cmd() or parse_target()).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <scsi/sg.h>

#define SCSI_IOCTL_GET_BUS_NUMBER 0x5386

#define u8  uint8_t
#define u16 uint16_t
#define u32 uint32_t

#define MEGASAS_MAGIC          'M'
#define MEGASAS_IOC_FIRMWARE   _IOWR(MEGASAS_MAGIC, 1, struct megasas_iocpacket)
#define MFI_CMD_PD_SCSI_IO     0x04
#define MFI_FRAME_DIR_WRITE    0x0008
#define MFI_FRAME_DIR_READ     0x0010
#define MAX_IOCTL_SGE          16

struct megasas_sge32 { u32 phys_addr; u32 length; } __attribute__((packed));
union megasas_sgl { struct megasas_sge32 sge32[1]; } __attribute__((packed));

struct megasas_pthru_frame {
  u8 cmd; u8 sense_len; u8 cmd_status; u8 scsi_status;
  u8 target_id; u8 lun; u8 cdb_len; u8 sge_count;
  u32 context; u32 pad_0;
  u16 flags; u16 timeout; u32 data_xfer_len;
  u32 sense_buf_phys_addr_lo; u32 sense_buf_phys_addr_hi;
  u8 cdb[16];
  union megasas_sgl sgl;
} __attribute__((packed));

struct megasas_iocpacket {
  u16 host_no; u16 __pad1;
  u32 sgl_off; u32 sge_count; u32 sense_off; u32 sense_len;
  union { u8 raw[128]; struct megasas_pthru_frame pthru; } frame;
  struct iovec sgl[MAX_IOCTL_SGE];
} __attribute__((packed));

/*
 * Send one SCSI passthrough command via the MegaRAID ioctl.
 *
 * On success (the ioctl itself completed) returns the device's cmd_status
 * (0 = good). Returns -1 only when the ioctl call itself failed (wrong
 * target, driver rejected the frame, ...) - the two must never be confused:
 * cmd_status is a device-reported byte pre-set to 0xFF before the call, so
 * treating an ioctl failure as if it were cmd_status would print a
 * fabricated "device status" of 0xFF that the drive never sent.
 *
 * `name`, if non-NULL, is logged with the resulting status so a multi-step
 * tool (MODE SELECT then FORMAT UNIT) can show which step reported what;
 * pass NULL for tools that don't want that line.
 */
static inline int send_cmd(int fd, int bus, int target, u8 *cdb, int cdblen,
                            void *data, int len, int dir, const char *name) {
    struct megasas_iocpacket ioc;
    struct megasas_pthru_frame *pthru = &ioc.frame.pthru;

    memset(&ioc, 0, sizeof(ioc));
    ioc.host_no = bus;

    if (len > 0) {
        ioc.sge_count = 1;
        ioc.sgl_off = offsetof(struct megasas_pthru_frame, sgl);
        ioc.sgl[0].iov_base = data;
        ioc.sgl[0].iov_len = len;
        pthru->sge_count = 1;
        pthru->data_xfer_len = len;
        pthru->sgl.sge32[0].phys_addr = (intptr_t)data;
        pthru->sgl.sge32[0].length = len;
    }

    pthru->cmd = MFI_CMD_PD_SCSI_IO;
    pthru->cmd_status = 0xFF;
    pthru->target_id = target;
    pthru->cdb_len = cdblen;
    pthru->flags = dir;
    pthru->timeout = 0;
    memcpy(pthru->cdb, cdb, cdblen);

    if (ioctl(fd, MEGASAS_IOC_FIRMWARE, &ioc) < 0) {
        if (name)
            printf("%s: ioctl failed: %s\n", name, strerror(errno));
        return -1;
    }

    /* Only cmd_status is copied back by the driver (a single-byte
       copy_to_user of frame.hdr.cmd_status); scsi_status stays whatever our
       own memset left, so it is deliberately never read or printed here. */
    if (name)
        printf("%-12s cmd_status=0x%02x\n", name, pthru->cmd_status);
    return pthru->cmd_status;
}

/* INQUIRY vendor/product are 24 bytes the drive chooses, printed to a root
   operator's terminal. Escape sequences in there could scroll away or
   overwrite a destructive warning, or forge another drive's identity, so
   emit printable ASCII only. */
static inline void print_ascii(const u8 *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        putchar((s[i] >= 0x20 && s[i] < 0x7f) ? s[i] : '?');
}

/*
 * Parse a MegaRAID target id.
 *
 * atoi() silently turns "4x", "abc" and "" into 0 and returns no error, and
 * target_id is a u8 so 256 wraps to 0 too - either way a mistyped argument
 * aims a command at target 0 instead of refusing. Returns -1 on anything that
 * is not a clean 0-255.
 */
static inline int parse_target(const char *s) {
    char *end;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 0 || v > 255)
        return -1;
    return (int)v;
}

/*
 * Parse a MODE SELECT block length. The field it goes into (mode_sel_data
 * bytes 9-11) is 3 bytes wide, so the limit is 0xFFFFFF, not just "positive".
 * Returns -1 on anything that is not a clean 1-16777215.
 */
static inline int parse_block_size(const char *s) {
    char *end;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 1 || v > 0xFFFFFF)
        return -1;
    return (int)v;
}

#endif /* MEGARAID_COMMON_H */
