/*
 * The runtime's launcher (launcher.c), shown before Wine starts.
 */
#ifndef WINE_NX_LAUNCHER_H
#define WINE_NX_LAUNCHER_H

#include <stddef.h>

struct wine_nx_launcher_options
{
    const char *runtime_dir;   /* sdmc:/switch/wine: target.txt, args.txt, verbose.txt... */
    const char *build;
    /* 0 with the program's IMAGE_FILE_MACHINE_* when this runtime can start it. */
    int (*machine_of)( const char *path, unsigned short *machine );
    int vulkan;                /* the runtime has Vulkan, so DXVK's d3d9 can run */
    /* The global settings on entry, as the user left them on return. */
    int verbose;
    int profile;
    int framebuffer;
    int swkbd_auto;             /* no-swkbd-auto.txt: the on-screen keyboard opens on focus */
};

/* Show the launcher. Returns 1 with the chosen program's path in target, or 0
 * when the user quits. target on entry preselects a program. */
int wine_nx_launcher_run( struct wine_nx_launcher_options *options, char *target, size_t target_size );

/* A line in wine-nx-runtime.log (runtime.c). */
void wine_nx_runtime_trace( const char *msg );

#ifndef __SWITCH__
/* A host build (tests/launcher_host.c) supplies what the Switch build takes from libnx. */
int launcher_platform_font( const void **data, size_t *size );
int launcher_platform_prompt( const char *header, const char *initial, char *out, size_t size );
#endif

#endif
