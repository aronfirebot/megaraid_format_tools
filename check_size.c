#include <stdio.h>
#include <stddef.h>

#include "megaraid_common.h"

/* The whole point of this tool is that the layout matches what the megaraid_sas
   driver expects. Asserting at compile time makes a mismatch a build failure
   rather than a line of output nobody reads - main() used to return 0
   unconditionally, so running it in CI proved nothing. */
_Static_assert(sizeof(struct megasas_iocpacket) == 404,
               "megasas_iocpacket must be 404 bytes (0x194)");
_Static_assert(offsetof(struct megasas_iocpacket, frame) == 20,
               "megasas_iocpacket.frame must be at offset 20");
_Static_assert(offsetof(struct megasas_iocpacket, sgl) == 148,
               "megasas_iocpacket.sgl must be at offset 148");

/* Built alone, this file's main() is the program entry point as always. Built
   as part of megaraid_tool (MEGA_MULTICALL), it is renamed so it can be
   linked alongside the other tools' own main()s without colliding; see
   megaraid_tool.c. */
#ifdef MEGA_MULTICALL
#define main check_size_main
#endif

int main(void) {
    printf("sizeof(megasas_iocpacket) = %zu (expected 0x194 = 404)\n", sizeof(struct megasas_iocpacket));
    printf("sizeof(megasas_pthru_frame) = %zu\n", sizeof(struct megasas_pthru_frame));
    printf("sizeof(iovec) = %zu\n", sizeof(struct iovec));
    printf("offsetof sgl_off = %zu\n", offsetof(struct megasas_iocpacket, sgl_off));
    printf("offsetof frame = %zu\n", offsetof(struct megasas_iocpacket, frame));
    printf("offsetof sgl = %zu\n", offsetof(struct megasas_iocpacket, sgl));
    return 0;
}
