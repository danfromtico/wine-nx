/*
 * The runtime's launcher, shown before Wine starts: the Windows programs on
 * the SD card as a grid of their icons, drawn with SDL2 (launcher_ui.c).
 *
 * - The library lists the programs within two folders of drive_c, plus those
 *   added from the file browser (launcher-library.txt), sorted by title.
 * - A program's menu (Y) changes its title, arguments and settings, which go
 *   next to it (launcher_settings.h) and apply whenever it is started.
 * - Settings (X) holds the launcher's look (launcher.txt) and the global
 *   verbose.txt, profile.txt and framebuffer.txt.
 * - The file browser (-) starts a program anywhere on the card: C: is drive_c
 *   and Z: the card's root.
 *
 * Icons are read and decoded on a worker thread and shown as they arrive.
 * When SDL cannot start, the text menu of launcher_console.c is shown instead.
 */
#include <dirent.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <png.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "launcher.h"
#include "launcher_list.h"
#include "launcher_pe.h"
#include "launcher_settings.h"
#include "launcher_ui.h"

#define SCAN_DEPTH     3      /* drive_c and two folder levels below it */
#define ICON_SIDE      128    /* icons are decoded no larger than this */
#define ICON_TEXTURES  48     /* decoded icons kept as textures */
#define ICON_JOBS      64
#define MAX_FILES      1024
#define FOOTER_SPACE   38

enum icon_state { ICON_UNKNOWN, ICON_QUEUED, ICON_READY, ICON_MISSING };

struct program
{
    char path[512];                    /* sdmc:/... */
    char dos[256];                     /* C:\... or Z:\... */
    char title[128];                   /* shown: its own title, the resources' or the file name */
    char resource_title[128];
    unsigned short machine;
    struct launcher_settings settings;
    int own_files;                     /* settings, arguments, controls or Box64 options beside it */
    int added;                         /* listed in launcher-library.txt */
    int removed;
    enum icon_state icon_state;
    SDL_Texture *icon;
    int icon_width, icon_height;
    Uint32 icon_time;
    unsigned int icon_use;
};

struct icon_job
{
    int index;
    char path[512];
};

struct icon_result
{
    int index;
    int width, height;
    unsigned char *rgba;
};

struct file_entry
{
    char name[256];
    int is_dir;
    int supported;
    unsigned short machine;
};

struct launcher
{
    struct wine_nx_launcher_options *options;
    struct ui ui;

    struct program programs[LAUNCHER_MAX_ENTRIES];
    int program_count;
    int visible[LAUNCHER_MAX_ENTRIES];
    int visible_count;
    int selection;

    struct launcher_kv look;
    int columns, rows, show_hidden;
    char browse_dir[512];

    SDL_Thread *thread;
    SDL_mutex *mutex;
    SDL_cond *cond;
    int stop;
    struct icon_job jobs[ICON_JOBS];
    int job_count;
    struct icon_result results[ICON_JOBS];
    int result_count;
    unsigned int icon_use;
    Uint32 icon_event;
};

static struct launcher launcher;
static struct file_entry files[MAX_FILES];
static struct ui_row file_rows[MAX_FILES + 1];

extern int wine_nx_launcher_console_run( const char *drive_c, const char *runtime_dir, const char *build,
                                         int (*machine_of)( const char *path, unsigned short *machine ),
                                         int *verbose, int *profile, char *target, size_t target_size );

/***********************************************************************
 * Platform
 */

static void launcher_log( const char *format, ... )
{
    char line[600];
    va_list args;

    va_start( args, format );
    vsnprintf( line, sizeof(line), format, args );
    va_end( args );
    wine_nx_runtime_trace( line );
}

#ifdef __SWITCH__
static int font_service;

int launcher_platform_font( const void **data, size_t *size )
{
    PlFontData font;

    if (R_FAILED( plInitialize( PlServiceType_User ) )) return 0;
    font_service = 1;
    if (R_FAILED( plGetSharedFontByType( &font, PlSharedFontType_Standard ) ) || !font.address) return 0;
    *data = font.address;
    *size = font.size;
    return 1;
}

static void launcher_platform_font_release(void)
{
    /* The fonts read the shared memory until they are closed, so this comes after ui_quit. */
    if (font_service) plExit();
    font_service = 0;
}

int launcher_platform_prompt( const char *header, const char *initial, char *out, size_t size )
{
    SwkbdConfig keyboard;
    Result rc;

    if (R_FAILED( swkbdCreate( &keyboard, 0 ) )) return 0;
    swkbdConfigMakePresetDefault( &keyboard );
    swkbdConfigSetHeaderText( &keyboard, header );
    swkbdConfigSetGuideText( &keyboard, header );
    swkbdConfigSetInitialText( &keyboard, initial );
    swkbdConfigSetStringLenMax( &keyboard, size - 1 < 500 ? size - 1 : 500 );
    rc = swkbdShow( &keyboard, out, size );
    swkbdClose( &keyboard );
    return R_SUCCEEDED( rc );
}
#else
static void launcher_platform_font_release(void) {}
#endif

/***********************************************************************
 * Files
 */

static int file_exists( const char *path )
{
    struct stat st;

    return !stat( path, &st ) && S_ISREG( st.st_mode );
}

static void runtime_file( const struct launcher *l, const char *name, char *out, size_t size )
{
    snprintf( out, size, "%s/%s", l->options->runtime_dir, name );
}

static void write_line( const char *path, const char *text )
{
    FILE *file = fopen( path, "w" );

    if (!file) return;
    fprintf( file, "%s\n", text );
    fclose( file );
}

static int read_line( const char *path, char *out, size_t size )
{
    FILE *file = fopen( path, "r" );
    int ok;

    out[0] = 0;
    if (!file) return 0;
    ok = fgets( out, size, file ) != NULL;
    fclose( file );
    out[strcspn( out, "\r\n" )] = 0;
    return ok;
}

static const char *file_name( const char *path )
{
    const char *slash = strrchr( path, '/' );

    return slash ? slash + 1 : path;
}

static void parent_dir( char *dir )
{
    char *slash = strrchr( dir, '/' );

    if (!slash) return;
    if (slash == strchr( dir, '/' )) slash[1] = 0;  /* sdmc:/ stays */
    else *slash = 0;
}

static int is_root( const char *dir )
{
    const char *slash = strchr( dir, '/' );

    return !slash || !slash[1];
}

/***********************************************************************
 * Programs
 */

static void load_program_settings( struct launcher *l, struct program *p )
{
    char path[520];
    struct launcher_kv kv;
    size_t len;

    (void)l;
    p->own_files = 0;
    memset( &p->settings, 0, sizeof(p->settings) );
    p->settings.verbose = p->settings.profile = p->settings.framebuffer = -1;
    if (launcher_settings_path( p->path, path, sizeof(path) ) && launcher_kv_load( &kv, path ))
    {
        launcher_settings_read( &kv, &p->settings );
        p->own_files |= kv.size && file_exists( path );
    }
    if (launcher_args_path( p->path, path, sizeof(path) )) p->own_files |= file_exists( path );
    if (launcher_keys_path( p->path, path, sizeof(path) )) p->own_files |= file_exists( path );
    if (launcher_sibling_path( p->path, ".box64.txt", path, sizeof(path) )) p->own_files |= file_exists( path );

    if (p->settings.title[0]) snprintf( p->title, sizeof(p->title), "%s", p->settings.title );
    else if (p->resource_title[0]) snprintf( p->title, sizeof(p->title), "%s", p->resource_title );
    else
    {
        snprintf( p->title, sizeof(p->title), "%s", file_name( p->path ) );
        if ((len = strlen( p->title )) > 4 && !strcasecmp( p->title + len - 4, ".exe" )) p->title[len - 4] = 0;
    }
}

/* Fill in a program from its file; returns 0 when this runtime cannot start it. */
static int describe_program( struct launcher *l, struct program *p, const char *path )
{
    memset( p, 0, sizeof(*p) );
    if ((size_t)snprintf( p->path, sizeof(p->path), "%s", path ) >= sizeof(p->path)) return 0;
    if (!launcher_dos_path( path, p->dos, sizeof(p->dos) )) return 0;
    if (l->options->machine_of( path, &p->machine )) return 0;
    launcher_pe_describe( path, 0, NULL, p->resource_title, sizeof(p->resource_title) );
    load_program_settings( l, p );
    return 1;
}

static int find_program( const struct launcher *l, const char *path )
{
    int i;

    for (i = 0; i < l->program_count; i++)
        if (!l->programs[i].removed && !strcasecmp( l->programs[i].path, path )) return i;
    return -1;
}

static int add_program( struct launcher *l, const char *path, int added )
{
    int index = find_program( l, path );

    if (index >= 0) return index;
    if (l->program_count >= LAUNCHER_MAX_ENTRIES) return -1;
    if (!describe_program( l, &l->programs[l->program_count], path )) return -1;
    l->programs[l->program_count].added = added;
    return l->program_count++;
}

static void draw_loading( struct launcher *l, int found )
{
    struct ui *ui = &l->ui;
    char text[64];

    ui_background( ui );
    ui_header( ui, "Library", NULL );
    ui_text_centered( ui, ui->large, ui->width / 2, ui->height / 2 - 48, "Looking for programs...", ui->value );
    snprintf( text, sizeof(text), found == 1 ? "%d program found" : "%d programs found", found );
    ui_text_centered( ui, ui->small, ui->width / 2, ui->height / 2 + 20, text, ui->dim );
    ui_present( ui );
}

static void scan_dir( struct launcher *l, const char *dir, int depth, Uint32 *next_draw )
{
    struct dirent *entry;
    DIR *handle;

    if (!(handle = opendir( dir ))) return;
    while (l->program_count < LAUNCHER_MAX_ENTRIES && (entry = readdir( handle )))
    {
        char path[512];
        struct stat st;
        int is_dir;

        if (entry->d_name[0] == '.') continue;
        if ((size_t)snprintf( path, sizeof(path), "%s/%s", dir, entry->d_name ) >= sizeof(path)) continue;
        if (entry->d_type == DT_DIR) is_dir = 1;
        else if (entry->d_type == DT_REG) is_dir = 0;
        else if (stat( path, &st )) continue;
        else is_dir = S_ISDIR( st.st_mode );

        if (is_dir)
        {
            /* Wine's own files are under windows. */
            if (!depth && !strcasecmp( entry->d_name, "windows" )) continue;
            if (depth + 1 < SCAN_DEPTH) scan_dir( l, path, depth + 1, next_draw );
        }
        else if (launcher_is_exe( entry->d_name ))
        {
            add_program( l, path, 0 );
            if (SDL_TICKS_PASSED( SDL_GetTicks(), *next_draw ))
            {
                draw_loading( l, l->program_count );
                *next_draw = SDL_GetTicks() + 100;
            }
        }
    }
    closedir( handle );
}

static void save_library( struct launcher *l )
{
    char path[512];
    FILE *file;
    int i, any = 0;

    runtime_file( l, "launcher-library.txt", path, sizeof(path) );
    for (i = 0; i < l->program_count; i++) any |= l->programs[i].added && !l->programs[i].removed;
    if (!any)
    {
        remove( path );
        return;
    }
    if (!(file = fopen( path, "w" ))) return;
    for (i = 0; i < l->program_count; i++)
        if (l->programs[i].added && !l->programs[i].removed) fprintf( file, "%s\n", l->programs[i].path );
    fclose( file );
}

static void load_library( struct launcher *l )
{
    char path[512], line[512];
    Uint32 next_draw = SDL_GetTicks();
    FILE *file;

    draw_loading( l, 0 );
    scan_dir( l, LAUNCHER_DRIVE_C, 0, &next_draw );
    runtime_file( l, "launcher-library.txt", path, sizeof(path) );
    if (!(file = fopen( path, "r" ))) return;
    while (fgets( line, sizeof(line), file ))
    {
        int index;

        line[strcspn( line, "\r\n" )] = 0;
        if (!line[0] || !file_exists( line )) continue;
        if ((index = find_program( l, line )) >= 0) l->programs[index].added = 1;
        else add_program( l, line, 1 );
    }
    fclose( file );
}

static struct launcher *sort_launcher;

static int compare_visible( const void *a, const void *b )
{
    const struct program *x = &sort_launcher->programs[*(const int *)a];
    const struct program *y = &sort_launcher->programs[*(const int *)b];
    int order = strcasecmp( x->title, y->title );

    return order ? order : strcasecmp( x->dos, y->dos );
}

/* The programs shown, sorted by title; keep_index stays selected when it is shown. */
static void rebuild_visible( struct launcher *l, int keep_index )
{
    int i;

    l->visible_count = 0;
    for (i = 0; i < l->program_count; i++)
    {
        const struct program *p = &l->programs[i];

        if (p->removed || (p->settings.hidden && !l->show_hidden)) continue;
        l->visible[l->visible_count++] = i;
    }
    sort_launcher = l;
    qsort( l->visible, l->visible_count, sizeof(l->visible[0]), compare_visible );
    for (i = 0; i < l->visible_count; i++)
        if (l->visible[i] == keep_index) l->selection = i;
    if (l->selection >= l->visible_count) l->selection = l->visible_count ? l->visible_count - 1 : 0;
}

static void save_program_settings( struct launcher *l, struct program *p )
{
    struct launcher_kv kv;
    char path[520];

    if (!launcher_settings_path( p->path, path, sizeof(path) ) || !launcher_kv_load( &kv, path ) ||
        !launcher_settings_write( &kv, &p->settings ) || !launcher_kv_save( &kv, path ))
        ui_toast( &l->ui, "Could not save the program's settings", 2500 );
    load_program_settings( l, p );
}

/***********************************************************************
 * Icons
 */

static int decode_png( struct launcher_icon *icon )
{
    png_image image;
    unsigned char *rgba;

    memset( &image, 0, sizeof(image) );
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory( &image, icon->data, icon->size )) return 0;
    if (image.width > LAUNCHER_ICON_MAX_SIDE || image.height > LAUNCHER_ICON_MAX_SIDE)
    {
        png_image_free( &image );
        return 0;
    }
    image.format = PNG_FORMAT_RGBA;
    if (!(rgba = malloc( PNG_IMAGE_SIZE( image ) )))
    {
        png_image_free( &image );
        return 0;
    }
    if (!png_image_finish_read( &image, NULL, rgba, 0, NULL ))
    {
        free( rgba );
        return 0;
    }
    free( icon->data );
    icon->kind = LAUNCHER_ICON_RGBA;
    icon->data = rgba;
    icon->width = image.width;
    icon->height = image.height;
    icon->size = PNG_IMAGE_SIZE( image );
    return 1;
}

static int icon_thread( void *arg )
{
    struct launcher *l = arg;

    SDL_LockMutex( l->mutex );
    while (!l->stop)
    {
        struct launcher_icon icon;
        struct icon_result result;
        struct icon_job job;
        SDL_Event event;

        if (!l->job_count || l->result_count == ICON_JOBS)
        {
            SDL_CondWait( l->cond, l->mutex );
            continue;
        }
        job = l->jobs[0];
        memmove( l->jobs, l->jobs + 1, --l->job_count * sizeof(l->jobs[0]) );
        SDL_UnlockMutex( l->mutex );

        launcher_pe_describe( job.path, ICON_SIDE, &icon, NULL, 0 );
        if (icon.kind == LAUNCHER_ICON_PNG && !decode_png( &icon )) launcher_icon_free( &icon );
        if (icon.kind == LAUNCHER_ICON_RGBA && !launcher_icon_fit( &icon, ICON_SIDE )) launcher_icon_free( &icon );
        result.index = job.index;
        result.width = icon.width;
        result.height = icon.height;
        result.rgba = icon.kind == LAUNCHER_ICON_RGBA ? icon.data : NULL;
        if (!result.rgba) launcher_icon_free( &icon );

        SDL_LockMutex( l->mutex );
        l->results[l->result_count++] = result;
        memset( &event, 0, sizeof(event) );
        event.type = l->icon_event;
        SDL_PushEvent( &event );
    }
    SDL_UnlockMutex( l->mutex );
    return 0;
}

static void start_icons( struct launcher *l )
{
    l->icon_event = SDL_RegisterEvents( 1 );
    if (l->icon_event == (Uint32)-1) l->icon_event = SDL_USEREVENT;
    l->mutex = SDL_CreateMutex();
    l->cond = SDL_CreateCond();
    if (l->mutex && l->cond) l->thread = SDL_CreateThreadWithStackSize( icon_thread, "launcher icons", 256 * 1024, l );
}

static void stop_icons( struct launcher *l )
{
    int i;

    if (l->thread)
    {
        SDL_LockMutex( l->mutex );
        l->stop = 1;
        SDL_CondSignal( l->cond );
        SDL_UnlockMutex( l->mutex );
        SDL_WaitThread( l->thread, NULL );
    }
    for (i = 0; i < l->result_count; i++) free( l->results[i].rgba );
    l->result_count = l->job_count = 0;
    for (i = 0; i < l->program_count; i++)
    {
        if (l->programs[i].icon) SDL_DestroyTexture( l->programs[i].icon );
        l->programs[i].icon = NULL;
    }
    if (l->cond) SDL_DestroyCond( l->cond );
    if (l->mutex) SDL_DestroyMutex( l->mutex );
    l->thread = NULL;
    l->cond = NULL;
    l->mutex = NULL;
}

static void pump_icons( struct launcher *l )
{
    struct icon_result results[ICON_JOBS];
    int count, i, loaded = 0, oldest;

    if (!l->thread) return;
    SDL_LockMutex( l->mutex );
    count = l->result_count;
    memcpy( results, l->results, count * sizeof(results[0]) );
    l->result_count = 0;
    SDL_CondSignal( l->cond );
    SDL_UnlockMutex( l->mutex );

    for (i = 0; i < count; i++)
    {
        struct program *p = &l->programs[results[i].index];
        SDL_Texture *texture = NULL;

        if (results[i].rgba &&
            (texture = SDL_CreateTexture( l->ui.renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                                          results[i].width, results[i].height )))
        {
            SDL_UpdateTexture( texture, NULL, results[i].rgba, results[i].width * 4 );
            SDL_SetTextureBlendMode( texture, SDL_BLENDMODE_BLEND );
        }
        free( results[i].rgba );
        p->icon = texture;
        p->icon_width = results[i].width;
        p->icon_height = results[i].height;
        p->icon_time = SDL_GetTicks();
        p->icon_state = texture ? ICON_READY : ICON_MISSING;
    }

    /* Keep the most recently shown icons. */
    for (i = 0; i < l->program_count; i++) loaded += l->programs[i].icon != NULL;
    while (loaded > ICON_TEXTURES)
    {
        oldest = -1;
        for (i = 0; i < l->program_count; i++)
            if (l->programs[i].icon && (oldest < 0 || l->programs[i].icon_use < l->programs[oldest].icon_use))
                oldest = i;
        SDL_DestroyTexture( l->programs[oldest].icon );
        l->programs[oldest].icon = NULL;
        l->programs[oldest].icon_state = ICON_UNKNOWN;
        loaded--;
    }
}

static void request_icon( struct launcher *l, int index )
{
    struct program *p = &l->programs[index];

    p->icon_use = ++l->icon_use;
    if (p->icon_state != ICON_UNKNOWN || !l->thread) return;
    SDL_LockMutex( l->mutex );
    if (l->job_count < ICON_JOBS)
    {
        l->jobs[l->job_count].index = index;
        memcpy( l->jobs[l->job_count].path, p->path, sizeof(p->path) );
        l->job_count++;
        p->icon_state = ICON_QUEUED;
        SDL_CondSignal( l->cond );
    }
    SDL_UnlockMutex( l->mutex );
}

/***********************************************************************
 * Library grid
 */

struct grid
{
    int card, gap_x, gap_y, caption, x0, y0;
};

static void grid_layout( const struct launcher *l, struct grid *g )
{
    const struct ui *ui = &l->ui;
    int available = ui->height - UI_HEADER_HEIGHT - FOOTER_SPACE, by_height, by_width, w, h;

    g->gap_x = 22;
    g->gap_y = 16;
    g->caption = 36;
    by_height = (available - (l->rows - 1) * g->gap_y - l->rows * g->caption - 24) / l->rows;
    by_width = (ui->width - 120 - (l->columns - 1) * g->gap_x) / l->columns;
    g->card = by_height < by_width ? by_height : by_width;
    if (g->card < 64) g->card = 64;
    w = l->columns * g->card + (l->columns - 1) * g->gap_x;
    h = l->rows * (g->card + g->caption) + (l->rows - 1) * g->gap_y;
    g->x0 = (ui->width - w) / 2;
    g->y0 = UI_HEADER_HEIGHT + (available - h) / 2 + 4;
}

static int grid_hit( const struct launcher *l, int x, int y )
{
    int per_page = l->columns * l->rows, page_start = l->selection / per_page * per_page, row, column;
    struct grid g;

    grid_layout( l, &g );
    if (x < g.x0 || y < g.y0) return -1;
    column = (x - g.x0) / (g.card + g.gap_x);
    row = (y - g.y0) / (g.card + g.caption + g.gap_y);
    if (column >= l->columns || row >= l->rows) return -1;
    if ((x - g.x0) % (g.card + g.gap_x) >= g.card || (y - g.y0) % (g.card + g.caption + g.gap_y) >= g.card + g.caption)
        return -1;
    return page_start + row * l->columns + column < l->visible_count ? page_start + row * l->columns + column : -1;
}

/* A window with a title bar and the program's first letter, for programs without an icon. */
static void draw_placeholder( struct launcher *l, const struct program *p, int cx, int cy, int size, int alpha )
{
    struct ui *ui = &l->ui;
    SDL_Color frame = ui->dim, fill = { 0, 0, 0, 60 * alpha / 255 };
    int w = size, h = size * 3 / 4, x = cx - w / 2, y = cy - h / 2;
    char letter[8] = {0};
    size_t len = 1;

    frame.a = 200 * alpha / 255;
    ui_rounded( ui, x, y, w, h, 8, fill );
    ui_border( ui, x, y, w, h, 2, frame );
    ui_fill( ui, x, y, w, h / 6, frame );
    while ((p->title[len] & 0xc0) == 0x80 && len < 4) len++;
    memcpy( letter, p->title, p->title[0] ? len : 0 );
    if (letter[0] >= 'a' && letter[0] <= 'z') letter[0] -= 'a' - 'A';
    frame.a = alpha;
    ui_text_centered( ui, ui->large, cx, y + h / 6 + (h * 5 / 6 - TTF_FontHeight( ui->large )) / 2, letter, frame );
}

static void draw_card( struct launcher *l, int index, int x, int y, const struct grid *g, int current )
{
    struct ui *ui = &l->ui;
    struct program *p = &l->programs[l->visible[index]];
    int cx = x + g->card / 2, cy = y + g->card / 2, target = g->card * 60 / 100, dim = current ? 255 : 165;
    const char *arch = p->machine == 0x014c ? "x86" : "ARM64";
    SDL_Color caption = current ? ui->value : ui->dim;
    int text_w;

    request_icon( l, l->visible[index] );
    if (current)
    {
        SDL_Color glow = ui->selection;

        glow.a = 110;
        if (ui->glow)
        {
            SDL_Rect rect = { x - g->card / 4, y - g->card / 4, g->card * 3 / 2, g->card * 3 / 2 };
            SDL_SetTextureColorMod( ui->glow, glow.r, glow.g, glow.b );
            SDL_SetTextureAlphaMod( ui->glow, glow.a );
            SDL_RenderCopy( ui->renderer, ui->glow, NULL, &rect );
        }
        ui_rounded( ui, x - 5, y - 5, g->card + 10, g->card + 10, 18, ui->selection );
    }
    else
    {
        ui_rounded( ui, x + 4, y + 6, g->card, g->card, 14, (SDL_Color){ 0, 0, 0, 55 } );
        ui_rounded( ui, x + 2, y + 3, g->card, g->card, 14, (SDL_Color){ 0, 0, 0, 70 } );
    }
    ui_rounded( ui, x, y, g->card, g->card, 14, current ? ui->focus : ui->card );
    ui_fill( ui, x + 14, y, g->card - 28, 1, (SDL_Color){ 255, 255, 255, 30 } );

    if (p->icon)
    {
        int side = p->icon_width > p->icon_height ? p->icon_width : p->icon_height, scale, w, h;
        Uint32 age = SDL_GetTicks() - p->icon_time;
        SDL_Rect dst;

        /* Small pixel-art icons grow by whole steps so their pixels stay square. */
        if (side * 2 <= target)
        {
            scale = target / side;
            SDL_SetTextureScaleMode( p->icon, SDL_ScaleModeNearest );
        }
        else
        {
            scale = 0;
            SDL_SetTextureScaleMode( p->icon, SDL_ScaleModeLinear );
        }
        w = scale ? p->icon_width * scale : p->icon_width * target / side;
        h = scale ? p->icon_height * scale : p->icon_height * target / side;
        dst = (SDL_Rect){ cx - w / 2, cy - h / 2 - 4, w, h };
        SDL_SetTextureColorMod( p->icon, dim, dim, dim );
        SDL_SetTextureAlphaMod( p->icon, ui->animations && age < 180 ? age * 255 / 180 : 255 );
        SDL_RenderCopy( ui->renderer, p->icon, NULL, &dst );
    }
    else draw_placeholder( l, p, cx, cy - 4, target, p->icon_state == ICON_MISSING ? dim : dim / 3 );

    /* The processor it runs on, and a mark for programs with their own settings. */
    text_w = ui_text_width( ui, ui->small, arch );
    ui_rounded( ui, x + 10, y + 10, text_w + 16, TTF_FontHeight( ui->small ) + 4, 10, (SDL_Color){ 0, 0, 0, 110 } );
    ui_text( ui, ui->small, x + 18, y + 12, arch, current ? ui->value : ui->dim );
    if (p->own_files)
    {
        ui_rounded( ui, x + g->card - 26, y + 12, 14, 14, 4, (SDL_Color){ 10, 12, 18, 255 } );
        ui_rounded( ui, x + g->card - 24, y + 14, 10, 10, 3, ui->selection );
    }
    if (p->settings.hidden)
    {
        text_w = ui_text_width( ui, ui->small, "Hidden" );
        ui_rounded( ui, x + g->card - text_w - 26, y + g->card - 32, text_w + 16, TTF_FontHeight( ui->small ) + 4, 10,
                    (SDL_Color){ 0, 0, 0, 140 } );
        ui_text( ui, ui->small, x + g->card - text_w - 18, y + g->card - 30, "Hidden", ui->dim );
    }

    text_w = ui_text_width( ui, ui->small, p->title );
    if (text_w > g->card + g->gap_x - 6) text_w = g->card + g->gap_x - 6;
    ui_text_fit( ui, ui->small, cx - text_w / 2, y + g->card + 8, g->card + g->gap_x - 6, p->title, caption, current );
}

static void draw_library( struct launcher *l )
{
    static const struct ui_hint hints[] =
    {
        { UI_A, "Start" }, { UI_Y, "Options" }, { UI_X, "Settings" }, { UI_MINUS, "Files" },
        { UI_L, NULL }, { UI_R, "Page" }, { UI_PLUS, "Quit" },
    };
    struct ui *ui = &l->ui;
    int per_page = l->columns * l->rows, page_start = l->selection / per_page * per_page, i;
    const int band = UI_HEADER_HEIGHT - 4;
    char status[96];
    struct grid g;

    ui_background( ui );
    grid_layout( l, &g );

    ui_fill( ui, 0, 0, ui->width, band, ui->panel );
    if (!ui_animated( ui )) ui_fill( ui, 0, band, ui->width, 2, ui->selection );
    ui_text( ui, ui->normal, 28, (band - TTF_FontHeight( ui->normal )) / 2, "Wine-NX", ui->value );
    if (l->visible_count)
    {
        const char *dos = l->programs[l->visible[l->selection]].dos;
        int status_w, max_w, dos_w;

        snprintf( status, sizeof(status), "%d / %d   \xc2\xb7   Page %d / %d", l->selection + 1, l->visible_count,
                  l->selection / per_page + 1, (l->visible_count + per_page - 1) / per_page );
        ui_text_centered( ui, ui->normal, ui->width / 2, (band - TTF_FontHeight( ui->normal )) / 2, status, ui->value );
        status_w = ui_text_width( ui, ui->normal, status );
        max_w = ui->width - 28 - (ui->width / 2 + status_w / 2) - 30;
        dos_w = ui_text_width( ui, ui->small, dos );
        if (dos_w > max_w) dos_w = max_w;
        ui_text_fit( ui, ui->small, ui->width - 28 - dos_w, (band - TTF_FontHeight( ui->small )) / 2, max_w, dos,
                     ui->dim, 1 );
    }
    for (i = page_start; i < l->visible_count && i < page_start + per_page; i++)
    {
        int column = (i - page_start) % l->columns, row = (i - page_start) / l->columns;

        if (i == l->selection) continue;
        draw_card( l, i, g.x0 + column * (g.card + g.gap_x), g.y0 + row * (g.card + g.caption + g.gap_y), &g, 0 );
    }
    if (l->visible_count)
    {
        int column = (l->selection - page_start) % l->columns, row = (l->selection - page_start) / l->columns;

        draw_card( l, l->selection, g.x0 + column * (g.card + g.gap_x),
                   g.y0 + row * (g.card + g.caption + g.gap_y), &g, 1 );
    }
    /* Decode the next page's icons too, so turning to it shows them at once. */
    for (i = page_start + per_page; i < l->visible_count && i < page_start + 2 * per_page; i++)
        request_icon( l, l->visible[i] );

    if (!l->visible_count)
    {
        ui_text_centered( ui, ui->large, ui->width / 2, ui->height / 2 - 70,
                          l->program_count ? "Every program is hidden" : "No Windows programs yet", ui->value );
        ui_text_wrapped( ui, ui->normal, ui->width / 2, ui->height / 2, 900, 3,
                         l->program_count ? "Turn on \"Show hidden programs\" in Settings (X)."
                                          : "Copy programs into sdmc:/switch/wine/drive_c,\nor press - to find one anywhere on the SD card.",
                         ui->dim, 1 );
    }
    ui_footer( ui, hints, sizeof(hints) / sizeof(hints[0]) );
    ui_fade( ui );
}

/***********************************************************************
 * A program's menu
 */

enum program_row
{
    ROW_START, ROW_TITLE, ROW_ARGS, ROW_VERBOSE, ROW_PROFILE, ROW_WINDOWS, ROW_D3D9, ROW_CONTROLS, ROW_BOX64,
    ROW_HIDE, ROW_LIBRARY, PROGRAM_ROWS
};

/* A setting that follows the global one (-1) or is on (1) or off (0) for this program. */
static const char *state_text( int state, int global, const char *on, const char *off, char *buffer, size_t size )
{
    if (state >= 0) return state ? on : off;
    snprintf( buffer, size, "Global (%s)", global ? on : off );
    return buffer;
}

static int next_state( int state, int direction )
{
    static const int order[3] = { -1, 1, 0 };
    int i;

    for (i = 0; i < 2 && order[i] != state; i++) {}
    return order[(i + (direction < 0 ? 2 : 1)) % 3];
}

static void show_file( struct launcher *l, const char *title, const char *path, const char *missing )
{
    char text[1024];
    FILE *file = fopen( path, "rb" );
    size_t size, i, j;

    if (!file)
    {
        ui_message( &l->ui, title, missing );
        return;
    }
    size = fread( text, 1, sizeof(text) - 1, file );
    fclose( file );
    for (i = j = 0; i < size; i++) if (text[i] != '\r') text[j++] = text[i];
    text[j] = 0;
    ui_message( &l->ui, title, j ? text : "The file is empty." );
}

static int start_program( struct launcher *l, const struct program *p, char *target, size_t size )
{
    struct ui *ui = &l->ui;
    char path[512], text[160];

    /* The last frame before Wine starts; the screen stays dark until it shows a window. */
    snprintf( text, sizeof(text), "Starting %s", p->title );
    ui_background( ui );
    ui_header( ui, "Library", p->dos );
    ui_text_fit( ui, ui->large, (ui->width - (ui_text_width( ui, ui->large, text ) < ui->width - 120 ?
                                             ui_text_width( ui, ui->large, text ) : ui->width - 120)) / 2,
                 ui->height / 2 - 40, ui->width - 120, text, ui->value, 0 );
    ui_text_centered( ui, ui->small, ui->width / 2, ui->height / 2 + 24, "Wine is getting ready...", ui->dim );
    ui_present( ui );

    snprintf( target, size, "%s", p->path );
    runtime_file( l, "target.txt", path, sizeof(path) );
    write_line( path, target );
    return 1;
}

static void edit_text( const char *header, char *value, size_t size )
{
    char edited[896];

    if (!launcher_platform_prompt( header, value, edited, size < sizeof(edited) ? size : sizeof(edited) )) return;
    snprintf( value, size, "%s", edited );
}

/* Returns 1 when the program is to be started. */
static int program_menu( struct launcher *l, struct program *p, char *target, size_t size )
{
    struct ui_row rows[PROGRAM_ROWS];
    int ids[PROGRAM_ROWS], count, id;
    struct ui_list list = {0};
    struct ui *ui = &l->ui;
    char path[520], dir[512], line[896], global_line[896], name[128], buffer[64];
    struct program copy;

    for (;;)
    {
        const char *base = file_name( p->path );
        int in_library = find_program( l, p->path ) >= 0;
        int x86 = p->machine == 0x014c, dxvk_beside, dxvk_installed;
        enum ui_action action;
        struct ui_row *row;

        snprintf( name, sizeof(name), "%.*s", (int)(strlen( base ) > 4 ? strlen( base ) - 4 : strlen( base )), base );
        snprintf( dir, sizeof(dir), "%s", p->path );
        parent_dir( dir );
        snprintf( path, sizeof(path), "%s/d3d9.dll", dir );
        dxvk_beside = file_exists( path );
        dxvk_installed = file_exists( LAUNCHER_DRIVE_C "/dxvk/d3d9.dll" );

        count = 0;
#define ADD_ROW(i, text, help_text) \
        do { row = rows + count; memset( row, 0, sizeof(*row) ); ids[count++] = (i); \
             snprintf( row->label, sizeof(row->label), "%s", (text) ); row->help = (help_text); } while (0)

        ADD_ROW( ROW_START, "Start", NULL );
        ADD_ROW( ROW_TITLE, "Title",
                 "The name shown in the library. Y goes back to the name in the program's own resources." );
        snprintf( row->value, sizeof(row->value), "%s", p->title );

        ADD_ROW( ROW_ARGS, "Arguments",
                 "The command line after the program's name, kept beside it as NAME.args.txt. "
                 "Leave it empty to remove them." );
        line[0] = 0;
        if (launcher_args_path( p->path, path, sizeof(path) )) read_line( path, line, sizeof(line) );
        runtime_file( l, "args.txt", path, sizeof(path) );
        if (line[0]) snprintf( row->value, sizeof(row->value), "%s", line );
        else if (read_line( path, global_line, sizeof(global_line) ) && launcher_args_match( global_line, p->dos ))
            snprintf( row->value, sizeof(row->value), "args.txt: %s", global_line );
        else snprintf( row->value, sizeof(row->value), "None" );

        ADD_ROW( ROW_VERBOSE, "Verbose traces",
                 "Writes Wine's traces to wine-nx-runtime.log, which slows the program down. "
                 "Global follows the setting in Settings (X on the library)." );
        row->adjustable = 1;
        snprintf( row->value, sizeof(row->value), "%s",
                  state_text( p->settings.verbose, l->options->verbose, "On", "Off", buffer, sizeof(buffer) ) );

        ADD_ROW( ROW_PROFILE, "Profiler",
                 "Samples where every thread spends its time and writes [PROF] lines to wine-nx-runtime.log." );
        row->adjustable = 1;
        snprintf( row->value, sizeof(row->value), "%s",
                  state_text( p->settings.profile, l->options->profile, "On", "Off", buffer, sizeof(buffer) ) );

        ADD_ROW( ROW_WINDOWS, "Windows shown by",
                 "The compositor draws every window through OpenGL. The framebuffer copies window pixels "
                 "straight to the screen, for when the compositor misbehaves." );
        row->adjustable = 1;
        snprintf( row->value, sizeof(row->value), "%s",
                  state_text( p->settings.framebuffer, l->options->framebuffer, "Framebuffer", "Compositor",
                              buffer, sizeof(buffer) ) );

        if (l->options->vulkan && x86)
        {
            ADD_ROW( ROW_D3D9, "Direct3D 9",
                     "Wine draws Direct3D 9 with OpenGL. DXVK draws it with Vulkan: C:\\dxvk\\d3d9.dll is "
                     "loaded instead of Wine's. A d3d9.dll next to the program is always loaded first." );
            if (dxvk_beside)
            {
                row->disabled = 1;
                snprintf( row->value, sizeof(row->value), "d3d9.dll next to the program" );
            }
            else if (!dxvk_installed && !p->settings.dxvk)
            {
                row->disabled = 1;
                snprintf( row->value, sizeof(row->value), "Wine (no C:\\dxvk\\d3d9.dll)" );
            }
            else
            {
                row->adjustable = 1;
                snprintf( row->value, sizeof(row->value), "%s", p->settings.dxvk ? "DXVK" : "Wine" );
            }
        }

        ADD_ROW( ROW_CONTROLS, "Controls",
                 "Keys the controller presses: NAME.keys.txt next to the program, applied over the "
                 "shared keys.txt, one NAME=code line each." );
        if (launcher_keys_path( p->path, path, sizeof(path) ) && file_exists( path ))
            snprintf( row->value, sizeof(row->value), "%s", file_name( path ) );
        else
        {
            runtime_file( l, "keys.txt", path, sizeof(path) );
            snprintf( row->value, sizeof(row->value), "%s", file_exists( path ) ? "keys.txt" : "Default" );
        }

        if (x86)
        {
            ADD_ROW( ROW_BOX64, "Box64 options",
                     "Options for the x86 translator, read from NAME.box64.txt next to the program." );
            launcher_sibling_path( p->path, ".box64.txt", path, sizeof(path) );
            snprintf( row->value, sizeof(row->value), "%s", file_exists( path ) ? file_name( path ) : "None" );
        }

        if (in_library)
        {
            ADD_ROW( ROW_HIDE, p->settings.hidden ? "Show in the library" : "Hide from the library",
                     "Hidden programs stay out of the library unless Settings shows them." );
        }
        if (!in_library || p->added)
        {
            ADD_ROW( ROW_LIBRARY, in_library ? "Remove from the library" : "Add to the library",
                     "The library finds programs within two folders of drive_c by itself; "
                     "others found with the file browser can be added." );
            row->destructive = in_library;
        }
#undef ADD_ROW

        action = ui_list_run( ui, &list, p->title, p->dos, rows, count, 1 );
        if (action == UI_ACTION_BACK || action == UI_ACTION_QUIT) return 0;
        id = ids[list.selection];
        switch (id)
        {
        case ROW_START:
            return start_program( l, p, target, size );

        case ROW_TITLE:
            if (action == UI_ACTION_RESET) p->settings.title[0] = 0;
            else if (action == UI_ACTION_CHOOSE) edit_text( "Title", p->settings.title, sizeof(p->settings.title) );
            else break;
            save_program_settings( l, p );
            break;

        case ROW_ARGS:
            if (action != UI_ACTION_CHOOSE || !launcher_args_path( p->path, path, sizeof(path) )) break;
            read_line( path, global_line, sizeof(global_line) );
            if (!launcher_platform_prompt( "Arguments", global_line, line, sizeof(line) )) break;
            if (line[0]) write_line( path, line );
            else remove( path );
            load_program_settings( l, p );
            ui_toast( ui, line[0] ? "Arguments saved" : "Arguments removed", 1500 );
            break;

        case ROW_VERBOSE:
        case ROW_PROFILE:
        case ROW_WINDOWS:
        {
            int *state = id == ROW_VERBOSE ? &p->settings.verbose :
                         id == ROW_PROFILE ? &p->settings.profile : &p->settings.framebuffer;

            *state = action == UI_ACTION_RESET ? -1 : next_state( *state, action == UI_ACTION_LEFT ? -1 : 1 );
            save_program_settings( l, p );
            break;
        }

        case ROW_D3D9:
            p->settings.dxvk = action == UI_ACTION_RESET ? 0 : !p->settings.dxvk;
            save_program_settings( l, p );
            break;

        case ROW_CONTROLS:
            if (action != UI_ACTION_CHOOSE) break;
            if (!launcher_keys_path( p->path, path, sizeof(path) ) || !file_exists( path ))
                runtime_file( l, "keys.txt", path, sizeof(path) );
            show_file( l, "Controls", path,
                       "No keys.txt: the controller uses the default keys. Put NAME=code lines in "
                       "NAME.keys.txt next to the program to change them." );
            break;

        case ROW_BOX64:
            if (action != UI_ACTION_CHOOSE) break;
            launcher_sibling_path( p->path, ".box64.txt", path, sizeof(path) );
            show_file( l, "Box64 options", path, "This program uses the default Box64 options." );
            break;

        case ROW_HIDE:
            if (action == UI_ACTION_RESET) p->settings.hidden = 0;
            else if (action == UI_ACTION_CHOOSE) p->settings.hidden = !p->settings.hidden;
            else break;
            save_program_settings( l, p );
            ui_toast( ui, p->settings.hidden ? "Hidden from the library" : "Shown in the library", 1500 );
            break;

        case ROW_LIBRARY:
            if (action != UI_ACTION_CHOOSE) break;
            if (in_library)
            {
                int index = find_program( l, p->path );

                l->programs[index].removed = 1;
                save_library( l );
                ui_toast( ui, "Removed from the library", 1500 );
                /* p may be the removed entry itself; the menu goes on with a copy. */
                copy = *p;
                copy.icon = NULL;
                copy.added = 0;
                p = &copy;
            }
            else
            {
                int index = add_program( l, p->path, 1 );

                if (index < 0) break;
                save_library( l );
                ui_toast( ui, "Added to the library", 1500 );
                p = &l->programs[index];
            }
            break;
        }
    }
}

/***********************************************************************
 * Settings
 */

enum settings_row
{
    SET_THEME, SET_ANIMATIONS, SET_COLUMNS, SET_ROWS, SET_HIDDEN, SET_VERBOSE, SET_PROFILE, SET_WINDOWS, SET_SWKBD,
    SET_VERSION, SET_CREDITS, SETTINGS_ROWS
};

static void save_look( struct launcher *l )
{
    char path[512], value[16];
    char theme[16];
    size_t i;

    snprintf( theme, sizeof(theme), "%s", ui_theme_name( l->ui.theme ) );
    for (i = 0; theme[i]; i++) theme[i] = tolower( (unsigned char)theme[i] );
    launcher_kv_set( &l->look, "theme", theme );
    launcher_kv_set( &l->look, "animations", l->ui.animations ? "1" : "0" );
    snprintf( value, sizeof(value), "%d", l->columns );
    launcher_kv_set( &l->look, "columns", value );
    snprintf( value, sizeof(value), "%d", l->rows );
    launcher_kv_set( &l->look, "rows", value );
    launcher_kv_set( &l->look, "show-hidden", l->show_hidden ? "1" : "0" );
    launcher_kv_set( &l->look, "browse", l->browse_dir );
    runtime_file( l, "launcher.txt", path, sizeof(path) );
    launcher_kv_save( &l->look, path );
}

/* What Wine-NX is built from and on; README.md's Credits section has the same list. */
static const struct { const char *name, *value, *help; } credits[] =
{
    { "Wine", "WineHQ, LGPL-2.1+",
      "https://www.winehq.org\nThe Windows API, the loader, WoW64 and the Direct3D, OpenGL and Vulkan layers." },
    { "Box64", "ptitSeb, MIT",
      "https://github.com/ptitSeb/box64\nRuns 32-bit x86 code: its interpreter and ARM64 dynarec are the WoW64 CPU." },
    { "DXVK", "Philip Rebohle, zlib",
      "https://github.com/doitsujin/dxvk\nDirect3D 9 over Vulkan, for programs set to d3d9=dxvk." },
    { "Mesa", "Mesa3D, MIT",
      "https://mesa3d.org\nOpenGL through nvc0 and Vulkan through NVK on the Switch GPU." },
    { "mesa-switch", "danfromtico, NaGaa95 and others",
      "https://github.com/danfromtico/mesa-switch\nThe Switch port of Mesa 26, with nvc0 and NVK, that the runtime links." },
    { "Switch Mesa and libdrm_nouveau", "fincs, Subv, Jules Blok, MIT",
      "devkitPro's Switch ports of Mesa 20.1 and libdrm_nouveau, the earlier OpenGL path." },
    { "libnx", "switchbrew, ISC",
      "https://github.com/switchbrew/libnx\nThe Horizon system library the runtime is written against." },
    { "devkitPro", "devkitA64 and portlibs",
      "https://devkitpro.org\nThe toolchain and the Switch builds of the libraries below." },
    { "SDL2 and SDL2_ttf", "Sam Lantinga, zlib",
      "https://www.libsdl.org\nThe launcher's drawing, input and text." },
    { "FreeType", "FreeType Project, FTL",
      "https://freetype.org\nFont rendering for the launcher." },
    { "HarfBuzz", "HarfBuzz authors, MIT",
      "https://harfbuzz.github.io\nText shaping for the launcher." },
    { "libpng, zlib, bzip2", "libpng, zlib and BSD licenses",
      "https://www.libpng.org  https://zlib.net  https://sourceware.org/bzip2\nProgram icons and compressed data." },
    { "llvm-mingw", "Martin Storsjo, Apache-2.0",
      "https://github.com/mstorsjo/llvm-mingw\nBuilds Wine's and DXVK's Windows DLLs (LLVM, libc++, mingw-w64)." },
    { "7-Zip", "Igor Pavlov, LGPL-2.1",
      "https://www.7-zip.org\n7zr.exe, the benchmark and archive test program on the card." },
    { "dolphin-nx", "NaGaa95, launcher design",
      "https://github.com/NaGaa95/dolphin-nx\nThis launcher's look follows dolphin-nx's launcher; its code is Wine-NX's own." },
    { "Atmosphere", "Atmosphere-NX, reference",
      "https://github.com/Atmosphere-NX/Atmosphere\nIts kernel source is how Wine-NX learns what Horizon's memory calls allow." },
    { "tico-dolphin", "ticohq, reference",
      "https://github.com/ticohq/tico-dolphin\nJIT and exception handling on Horizon." },
    { "WineBox64 NX", "Ibnuard, reference",
      "https://github.com/Ibnuard/winebox64_nx\nA proof of concept running x86-64 Wine under Box64 on Horizon; "
      "reference for Wine-NX's Box64 and libnx integration." },
    { "sphaira", "ITotalJustice, NaGaa95",
      "https://github.com/NaGaa95/sphaira\nForwarders that start Wine-NX with a 32-bit address space." },
};
#define CREDIT_COUNT (sizeof(credits) / sizeof(credits[0]))

static void credits_screen( struct launcher *l )
{
    struct ui_row rows[CREDIT_COUNT];
    struct ui_list list = {0};
    size_t i;

    memset( rows, 0, sizeof(rows) );
    for (i = 0; i < CREDIT_COUNT; i++)
    {
        snprintf( rows[i].label, sizeof(rows[i].label), "%s", credits[i].name );
        snprintf( rows[i].value, sizeof(rows[i].value), "%s", credits[i].value );
        rows[i].help = credits[i].help;
    }
    for (;;)
    {
        enum ui_action action = ui_list_run( &l->ui, &list, "Credits", NULL, rows, CREDIT_COUNT, 0 );

        if (action == UI_ACTION_BACK || action == UI_ACTION_QUIT) return;
        if (action == UI_ACTION_CHOOSE)
        {
            ui_message( &l->ui, rows[list.selection].label, rows[list.selection].help );
            ui_start_screen( &l->ui );
        }
    }
}

static void settings_menu( struct launcher *l )
{
    static const char *on_off[2] = { "Off", "On" };
    struct ui_row rows[SETTINGS_ROWS];
    struct ui_list list = {0};
    struct ui *ui = &l->ui;
    char path[512];

    for (;;)
    {
        enum ui_action action;
        int i, direction;

        memset( rows, 0, sizeof(rows) );
        for (i = 0; i < SETTINGS_ROWS; i++) rows[i].adjustable = 1;
        snprintf( rows[SET_THEME].label, sizeof(rows[0].label), "Theme" );
        snprintf( rows[SET_THEME].value, sizeof(rows[0].value), "%s", ui_theme_name( ui->theme ) );
        rows[SET_THEME].help = "Bubbles and Glow move; Classic and OLED stay still.";
        snprintf( rows[SET_ANIMATIONS].label, sizeof(rows[0].label), "Animations" );
        snprintf( rows[SET_ANIMATIONS].value, sizeof(rows[0].value), "%s", on_off[ui->animations] );
        rows[SET_ANIMATIONS].help = "Moving backgrounds, fades and the sliding highlight. Off draws only when something changes.";
        snprintf( rows[SET_COLUMNS].label, sizeof(rows[0].label), "Library columns" );
        snprintf( rows[SET_COLUMNS].value, sizeof(rows[0].value), "%d", l->columns );
        snprintf( rows[SET_ROWS].label, sizeof(rows[0].label), "Library rows" );
        snprintf( rows[SET_ROWS].value, sizeof(rows[0].value), "%d", l->rows );
        snprintf( rows[SET_HIDDEN].label, sizeof(rows[0].label), "Show hidden programs" );
        snprintf( rows[SET_HIDDEN].value, sizeof(rows[0].value), "%s", on_off[l->show_hidden] );
        snprintf( rows[SET_VERBOSE].label, sizeof(rows[0].label), "Verbose traces" );
        snprintf( rows[SET_VERBOSE].value, sizeof(rows[0].value), "%s", on_off[!!l->options->verbose] );
        rows[SET_VERBOSE].help = "verbose.txt: Wine's traces go to wine-nx-runtime.log for every program without its own setting.";
        snprintf( rows[SET_PROFILE].label, sizeof(rows[0].label), "Profiler" );
        snprintf( rows[SET_PROFILE].value, sizeof(rows[0].value), "%s", on_off[!!l->options->profile] );
        rows[SET_PROFILE].help = "profile.txt: [PROF] lines with where each thread spends its time.";
        snprintf( rows[SET_WINDOWS].label, sizeof(rows[0].label), "Windows shown by" );
        snprintf( rows[SET_WINDOWS].value, sizeof(rows[0].value), "%s",
                  l->options->framebuffer ? "Framebuffer" : "Compositor" );
        rows[SET_WINDOWS].help = "framebuffer.txt: the framebuffer copies window pixels straight to the screen, "
                                 "for when the OpenGL compositor misbehaves.";
        snprintf( rows[SET_SWKBD].label, sizeof(rows[0].label), "On-screen keyboard" );
        snprintf( rows[SET_SWKBD].value, sizeof(rows[0].value), "%s", on_off[!!l->options->swkbd_auto] );
        rows[SET_SWKBD].help = "no-swkbd-auto.txt: opens by itself when a text field gets focus. Off leaves it to "
                                "Minus + the right stick click, or a program asking for it directly.";
        snprintf( rows[SET_VERSION].label, sizeof(rows[0].label), "Runtime" );
        snprintf( rows[SET_VERSION].value, sizeof(rows[0].value), "%s", l->options->build );
        rows[SET_VERSION].disabled = 1;
        rows[SET_VERSION].adjustable = 0;
        snprintf( rows[SET_CREDITS].label, sizeof(rows[0].label), "Credits" );
        snprintf( rows[SET_CREDITS].value, sizeof(rows[0].value), "Wine, Box64, DXVK, Mesa..." );
        rows[SET_CREDITS].help = "The projects Wine-NX is built from, and its launcher's design by dolphin-nx.";
        rows[SET_CREDITS].adjustable = 0;

        action = ui_list_run( ui, &list, "Settings", NULL, rows, SETTINGS_ROWS, 0 );
        if (action == UI_ACTION_BACK || action == UI_ACTION_QUIT) return;
        direction = action == UI_ACTION_LEFT ? -1 : 1;
        switch (list.selection)
        {
        case SET_THEME:
            ui_set_theme( ui, (ui->theme + UI_THEME_COUNT + direction) % UI_THEME_COUNT );
            break;
        case SET_ANIMATIONS: ui->animations = !ui->animations; break;
        case SET_COLUMNS: l->columns = 3 + (l->columns - 3 + 6 + direction) % 6; break;
        case SET_ROWS: l->rows = 1 + (l->rows - 1 + 3 + direction) % 3; break;
        case SET_HIDDEN: l->show_hidden = !l->show_hidden; break;
        case SET_VERBOSE:
            l->options->verbose = !l->options->verbose;
            runtime_file( l, "verbose.txt", path, sizeof(path) );
            write_line( path, l->options->verbose ? "1" : "0" );
            break;
        case SET_PROFILE:
            l->options->profile = !l->options->profile;
            runtime_file( l, "profile.txt", path, sizeof(path) );
            write_line( path, l->options->profile ? "1" : "0" );
            break;
        case SET_WINDOWS:
            l->options->framebuffer = !l->options->framebuffer;
            runtime_file( l, "framebuffer.txt", path, sizeof(path) );
            write_line( path, l->options->framebuffer ? "1" : "0" );
            break;
        case SET_SWKBD:
            l->options->swkbd_auto = !l->options->swkbd_auto;
            runtime_file( l, "no-swkbd-auto.txt", path, sizeof(path) );
            write_line( path, l->options->swkbd_auto ? "0" : "1" );
            break;
        case SET_CREDITS:
            credits_screen( l );
            ui_start_screen( ui );
            continue;
        }
        save_look( l );
    }
}

/***********************************************************************
 * File browser
 */

static int compare_files( const void *a, const void *b )
{
    const struct file_entry *x = a, *y = b;

    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
    return strcasecmp( x->name, y->name );
}

static int read_dir( struct launcher *l, const char *dir, int *count )
{
    struct dirent *entry;
    DIR *handle;

    *count = 0;
    if (!(handle = opendir( dir ))) return 0;
    while (*count < MAX_FILES && (entry = readdir( handle )))
    {
        struct file_entry *file = files + *count;
        char path[512];
        struct stat st;

        if (entry->d_name[0] == '.') continue;
        if ((size_t)snprintf( path, sizeof(path), "%s%s%s", dir, is_root( dir ) && dir[strlen( dir ) - 1] == '/' ? "" : "/",
                              entry->d_name ) >= sizeof(path)) continue;
        if (entry->d_type == DT_DIR) file->is_dir = 1;
        else if (entry->d_type == DT_REG) file->is_dir = 0;
        else if (stat( path, &st )) continue;
        else file->is_dir = S_ISDIR( st.st_mode );
        if (!file->is_dir && !launcher_is_exe( entry->d_name )) continue;
        snprintf( file->name, sizeof(file->name), "%s", entry->d_name );
        file->supported = file->is_dir || !l->options->machine_of( path, &file->machine );
        (*count)++;
    }
    closedir( handle );
    qsort( files, *count, sizeof(files[0]), compare_files );
    return 1;
}

static void join_path( char *out, size_t size, const char *dir, const char *name )
{
    snprintf( out, size, "%s%s%s", dir, dir[strlen( dir ) - 1] == '/' ? "" : "/", name );
}

static int file_browser( struct launcher *l, char *target, size_t size )
{
    struct ui *ui = &l->ui;
    char dir[512], came_from[256] = "", dos[512], path[512];
    struct stat st;

    snprintf( dir, sizeof(dir), "%s", l->browse_dir );
    if (stat( dir, &st ) || !S_ISDIR( st.st_mode )) snprintf( dir, sizeof(dir), "%s", LAUNCHER_DRIVE_C );

    for (;;)
    {
        struct ui_list list = {0};
        int count, has_up, rows, i, reload = 0;

        while (!read_dir( l, dir, &count ) && !is_root( dir )) parent_dir( dir );
        snprintf( l->browse_dir, sizeof(l->browse_dir), "%s", dir );
        has_up = !is_root( dir );
        if (!launcher_dos_path( dir, dos, sizeof(dos) )) snprintf( dos, sizeof(dos), "%s", dir );

        rows = 0;
        if (has_up)
        {
            char parent[512], parent_dos[512];

            snprintf( parent, sizeof(parent), "%s", dir );
            parent_dir( parent );
            memset( file_rows, 0, sizeof(file_rows[0]) );
            snprintf( file_rows[0].label, sizeof(file_rows[0].label), "Up one folder" );
            if (launcher_dos_path( parent, parent_dos, sizeof(parent_dos) ))
                snprintf( file_rows[0].value, sizeof(file_rows[0].value), "%s", parent_dos );
            rows = 1;
        }
        for (i = 0; i < count; i++, rows++)
        {
            struct ui_row *row = file_rows + rows;

            memset( row, 0, sizeof(*row) );
            snprintf( row->label, sizeof(row->label), "%s", files[i].name );
            if (files[i].is_dir) snprintf( row->value, sizeof(row->value), "Folder" );
            else if (!files[i].supported)
            {
                snprintf( row->value, sizeof(row->value), "Cannot run here" );
                row->disabled = 1;
            }
            else snprintf( row->value, sizeof(row->value), "%s", files[i].machine == 0x014c ? "x86" : "ARM64" );
            if (came_from[0] && !strcasecmp( files[i].name, came_from )) list.selection = rows;
        }
        if (!rows)
        {
            memset( file_rows, 0, sizeof(file_rows[0]) );
            snprintf( file_rows[0].label, sizeof(file_rows[0].label), "No folders or programs here" );
            file_rows[0].disabled = 1;
            rows = 1;
        }
        /* A folder opens on its first entry; going up returns to the folder left. */
        if (!came_from[0] && has_up && rows > 1) list.selection = 1;
        came_from[0] = 0;

        while (!reload)
        {
            enum ui_action action = ui_list_run( ui, &list, "Files", dos, file_rows, rows, 0 );
            int index = list.selection - has_up;

            if (action == UI_ACTION_QUIT) return 0;
            if (action == UI_ACTION_BACK || (action == UI_ACTION_CHOOSE && index < 0))
            {
                if (!has_up)
                {
                    save_look( l );
                    return 0;
                }
                snprintf( came_from, sizeof(came_from), "%s", file_name( dir ) );
                parent_dir( dir );
                reload = 1;
                break;
            }
            if (action != UI_ACTION_CHOOSE || index >= count) continue;
            join_path( path, sizeof(path), dir, files[index].name );
            if (files[index].is_dir)
            {
                snprintf( dir, sizeof(dir), "%s", path );
                reload = 1;
            }
            else
            {
                struct program program, *p = &program;
                int library_index = find_program( l, path );

                if (library_index >= 0) p = &l->programs[library_index];
                else if (!describe_program( l, &program, path )) continue;
                if (program_menu( l, p, target, size ))
                {
                    save_look( l );
                    return 1;
                }
                ui_start_screen( ui );
            }
        }
    }
}

/***********************************************************************
 * The library and the entry point
 */

static int run_library( struct launcher *l, char *target, size_t size )
{
    struct ui *ui = &l->ui;
    struct ui_input input;

    ui_start_screen( ui );
    while (ui_begin_frame( ui ))
    {
        int per_page = l->columns * l->rows;

        pump_icons( l );
        while (ui_poll( ui, &input ))
        {
            struct program *p = l->visible_count ? &l->programs[l->visible[l->selection]] : NULL;
            int keep = p ? l->visible[l->selection] : -1, hit;

            switch (input.touch)
            {
            case UI_TOUCH_TAP:
                if ((hit = grid_hit( l, input.x, input.y )) < 0) break;
                if (hit == l->selection) return start_program( l, p, target, size );
                l->selection = hit;
                break;
            case UI_TOUCH_SWIPE_LEFT:
            case UI_TOUCH_SCROLL_UP:
                l->selection = launcher_grid_page( l->selection, l->visible_count, per_page, 1 );
                break;
            case UI_TOUCH_SWIPE_RIGHT:
            case UI_TOUCH_SCROLL_DOWN:
                l->selection = launcher_grid_page( l->selection, l->visible_count, per_page, -1 );
                break;
            default:
                break;
            }
            switch (input.button)
            {
            case UI_LEFT:
            case UI_RIGHT:
                l->selection = launcher_grid_move( l->selection, l->visible_count, l->columns, l->rows,
                                                   input.button == UI_LEFT ? -1 : 1, 0 );
                break;
            case UI_UP:
            case UI_DOWN:
                l->selection = launcher_grid_move( l->selection, l->visible_count, l->columns, l->rows,
                                                   0, input.button == UI_UP ? -1 : 1 );
                break;
            case UI_L:
            case UI_R:
                l->selection = launcher_grid_page( l->selection, l->visible_count, per_page, input.button == UI_L ? -1 : 1 );
                break;
            case UI_A:
                if (p) return start_program( l, p, target, size );
                break;
            case UI_Y:
                if (!p) break;
                if (program_menu( l, p, target, size )) return 1;
                rebuild_visible( l, keep );
                ui_start_screen( ui );
                break;
            case UI_X:
                settings_menu( l );
                rebuild_visible( l, keep );
                ui_start_screen( ui );
                break;
            case UI_MINUS:
                if (file_browser( l, target, size )) return 1;
                rebuild_visible( l, keep );
                ui_start_screen( ui );
                break;
            case UI_PLUS:
                return 0;
            case UI_B:
                if (ui_confirm( ui, "Quit", "Close Wine-NX and go back to the Homebrew Menu?", "Quit" )) return 0;
                ui_start_screen( ui );
                break;
            }
            if (!ui->running) return 0;
        }
        if (!ui->running) break;
        draw_library( l );
        ui_present( ui );
        ui_wait( ui );
    }
    return 0;
}

int wine_nx_launcher_run( struct wine_nx_launcher_options *options, char *target, size_t target_size )
{
    struct launcher *l = &launcher;
    const void *font = NULL;
    size_t font_size = 0;
    char path[512], value[32];
    enum ui_theme theme = UI_THEME_BUBBLES;
    int ret, i, added, missing;
    Uint32 started;

    memset( l, 0, sizeof(*l) );
    l->options = options;
    runtime_file( l, "launcher.txt", path, sizeof(path) );
    launcher_kv_load( &l->look, path );
    if (launcher_kv_get( &l->look, "theme", value, sizeof(value) ))
        for (i = 0; i < UI_THEME_COUNT; i++)
            if (!strcasecmp( value, ui_theme_name( i ) )) theme = i;
    l->columns = launcher_kv_get_int( &l->look, "columns", 5 );
    l->rows = launcher_kv_get_int( &l->look, "rows", 2 );
    if (l->columns < 3 || l->columns > 8) l->columns = 5;
    if (l->rows < 1 || l->rows > 3) l->rows = 2;
    l->show_hidden = launcher_kv_get_int( &l->look, "show-hidden", 0 ) == 1;
    if (!launcher_kv_get( &l->look, "browse", l->browse_dir, sizeof(l->browse_dir) ) || !l->browse_dir[0])
        snprintf( l->browse_dir, sizeof(l->browse_dir), "%s", LAUNCHER_DRIVE_C );

    started = SDL_GetTicks();
    if (!launcher_platform_font( &font, &font_size ) ||
        !ui_init( &l->ui, font, font_size, theme, launcher_kv_get_int( &l->look, "animations", 1 ) != 0 ))
    {
        launcher_platform_font_release();
        /* libnx's console cannot draw once EGL has had the screen. */
        if (ui_screen_used())
        {
            launcher_log( "[LAUNCHER] SDL could not start (%s) after taking the screen; closing", ui_error() );
            return 0;
        }
        launcher_log( "[LAUNCHER] SDL could not start (%s); showing the text menu",
                      font ? ui_error() : "no shared font from the pl service" );
#ifdef __SWITCH__
        consoleInit( NULL );
        ret = wine_nx_launcher_console_run( LAUNCHER_DRIVE_C, options->runtime_dir, options->build, options->machine_of,
                                            &options->verbose, &options->profile, target, target_size );
        consoleExit( NULL );
        return ret;
#else
        return 0;
#endif
    }

    {
        SDL_RendererInfo info;

        if (SDL_GetRendererInfo( l->ui.renderer, &info )) info.name = "unknown";
        launcher_log( "[LAUNCHER] SDL %s video, %s renderer, font %zu bytes, theme %s, ready in %u ms",
                      SDL_GetCurrentVideoDriver(), info.name, font_size, ui_theme_name( l->ui.theme ),
                      SDL_GetTicks() - started );
    }
    start_icons( l );
    started = SDL_GetTicks();
    load_library( l );
    rebuild_visible( l, -1 );
    for (i = 0, added = 0; i < l->program_count; i++) added += l->programs[i].added;
    launcher_log( "[LAUNCHER] %d programs (%d from launcher-library.txt, %d shown) found in %u ms; icons %s",
                  l->program_count, added, l->visible_count, SDL_GetTicks() - started,
                  l->thread ? "load on a worker thread" : "off: no worker thread" );
    for (i = 0; i < l->visible_count; i++)
    {
        const struct program *p = &l->programs[l->visible[i]];

        if (!strcasecmp( p->path, target ) || !strcasecmp( p->dos, target )) l->selection = i;
    }
    ret = run_library( l, target, target_size );
    for (i = 0, added = 0, missing = 0; i < l->program_count; i++)
    {
        added += l->programs[i].icon_state == ICON_READY;
        missing += l->programs[i].icon_state == ICON_MISSING;
    }
    launcher_log( "[LAUNCHER] %s; %d icons shown, %d programs without one", ret ? "starting a program" : "closed",
                  added, missing );
    stop_icons( l );
    ui_quit( &l->ui );
    launcher_platform_font_release();
    return ret;
}
