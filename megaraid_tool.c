/*
 * megaraid_tool.c - single-binary multicall wrapper around the standalone
 * mega_*.c / check_size.c tools in this repo.
 *
 * Every tool file remains fully self-contained and independently buildable
 * exactly as documented in README.md - `gcc -o mega_inquiry mega_inquiry.c`
 * still works unmodified. The ioctl structs and shared helpers (send_cmd,
 * parse_target, print_ascii, ...) live in one place, megaraid_common.h,
 * which every tool file includes - see that header for why a single
 * `static inline` copy is both correctness (no drift between tools) and
 * self-containment (each .c file still compiles alone with the header
 * sitting next to it). This file is compiled together with those same
 * source files, with -DMEGA_MULTICALL defined, into one binary:
 *
 *   cc -DMEGA_MULTICALL -o megaraid_tool megaraid_tool.c mega_inquiry.c \
 *      mega_format512.c mega_modesel.c mega_format_immed.c mega_progress.c \
 *      check_size.c
 *
 * (or just `make megaraid_tool` / `make` - see Makefile).
 *
 * Under MEGA_MULTICALL each tool's own main() is renamed to a unique
 * `<tool>_main` (see the #ifdef MEGA_MULTICALL block near the bottom of each
 * tool file), so linking them all into one binary does not produce multiple
 * definitions of `main`. Every helper from megaraid_common.h is `static
 * inline`, giving each tool's own translation unit its own private copy with
 * internal linkage - each tool .c file is still compiled as its own
 * translation unit here, just linked into a shared executable.
 *
 * Usage:
 *   megaraid_tool <command> [args...]
 *
 * Each tool is also reachable by its original name if this binary is
 * invoked (directly or via a symlink/copy) under that name, e.g.:
 *   ln -s megaraid_tool mega_inquiry
 *   ./mega_inquiry /dev/sda 4
 * which dispatches exactly like running `megaraid_tool inquiry /dev/sda 4`.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

int mega_inquiry_main(int argc, char *argv[]);
int mega_format512_main(int argc, char *argv[]);
int mega_modesel_main(int argc, char *argv[]);
int mega_format_immed_main(int argc, char *argv[]);
int mega_progress_main(int argc, char *argv[]);
int check_size_main(void);

/* check_size's tool main() takes no arguments; adapt it to the common
   (argc, argv) applet signature so it can sit in the same dispatch table. */
static int check_size_dispatch(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    return check_size_main();
}

struct applet {
    const char *name;          /* subcommand name, and original binary name
                                   without the "mega_" prefix (if any) */
    int (*run)(int, char **);
    const char *summary;
};

static const struct applet applets[] = {
    { "inquiry",      mega_inquiry_main,      "identify a drive via SCSI INQUIRY" },
    { "format512",    mega_format512_main,    "FORMAT UNIT at the drive's current sector size" },
    { "modesel",      mega_modesel_main,      "MODE SELECT + FORMAT UNIT (blocking - SSDs only)" },
    { "format_immed", mega_format_immed_main, "MODE SELECT + FORMAT UNIT with IMMED (background, HDD-safe)" },
    { "progress",     mega_progress_main,     "poll background FORMAT UNIT progress" },
    { "check_size",   check_size_dispatch,    "validate the MegaRAID ioctl struct layout" },
};
#define NUM_APPLETS (sizeof(applets) / sizeof(applets[0]))

static const struct applet *find_by_name(const char *name) {
    for (size_t i = 0; i < NUM_APPLETS; i++)
        if (strcmp(name, applets[i].name) == 0)
            return &applets[i];
    return NULL;
}

/* Match this binary's own invocation name (argv[0], with any directory
   component and a leading "mega_" stripped) against an applet name. This is
   the same convention BusyBox uses for multicall binaries, and it means a
   symlink or copy named after one of the original tools dispatches directly
   without needing the "<command>" argument. */
static const struct applet *find_by_progname(const char *argv0) {
    const char *base = strrchr(argv0, '/');
    base = base ? base + 1 : argv0;
    if (strncmp(base, "mega_", 5) == 0)
        base += 5;
    return find_by_name(base);
}

static int usage(const char *prog) {
    printf("MegaRAID 520-to-512/4096 byte sector format tools (unified binary)\n");
    printf("Usage: %s <command> [args...]\n\n", prog);
    printf("Commands:\n");
    for (size_t i = 0; i < NUM_APPLETS; i++)
        printf("  %-14s %s\n", applets[i].name, applets[i].summary);
    printf("\nRun \"%s <command>\" with no further arguments for that command's own\n", prog);
    printf("usage/help text. Each command is also its own standalone tool (see\n");
    printf("README.md) - this binary just links the same source files together.\n");
    return 1;
}

int main(int argc, char *argv[]) {
    const struct applet *a = find_by_progname(argv[0]);

    if (a)
        return a->run(argc, argv);

    if (argc < 2)
        return usage(argv[0]);

    a = find_by_name(argv[1]);
    if (!a) {
        fprintf(stderr, "Unknown command '%s'\n\n", argv[1]);
        return usage(argv[0]);
    }
    /* Shift argv so the applet sees its own name (the subcommand) as argv[0],
       just as it would if invoked as its own standalone binary. */
    return a->run(argc - 1, argv + 1);
}
