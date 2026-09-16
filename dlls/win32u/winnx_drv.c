/*
 * Minimal Nintendo Switch (Horizon) display driver for win32u.
 *
 * Windows render into ordinary DIB memory (handled by the GDI engine), and the
 * window surface's flush() copies the dirty pixels to the runtime's OpenGL
 * compositor, one layer per window surface (wine-nx-probe/source/compositor.c),
 * or, with switch/wine/framebuffer.txt, straight to the libnx framebuffer via
 * the runtime hooks wine_nx_fb_*().
 *
 * The driver reuses the null_user_driver for everything else; only window
 * creation and surface presentation are Switch-specific (see driver.c, which
 * plugs these in under __SWITCH__).
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "ntgdi_private.h"
#include "ntuser_private.h"
#include "win32u_private.h"
#include "wine/gdi_driver.h"
#include "../../wine-nx-probe/source/compositor.h"

/* Framebuffer hooks implemented in the runtime (wine-nx-probe/source/runtime.c). */
extern void *wine_nx_fb_lock( int *width, int *height, int *stride_px );
extern void  wine_nx_fb_unlock( void );
extern void  wine_nx_fb_present( void );
extern int   wine_nx_pointer_poll( int *x, int *y, unsigned int *buttons );
extern int   wine_nx_pointer_take( int *x, int *y, unsigned int *buttons, unsigned int *pressed,
                                   unsigned int *released );
extern void  wine_nx_pointer_set_pos( int x, int y );
extern void  wine_nx_cursor_show( int visible );
extern void  wine_nx_runtime_trace( const char *msg ) __attribute__((weak));
extern int   wine_nx_runtime_verbose __attribute__((weak));
/* Whether the runtime's OpenGL compositor presents the screen instead of the
 * framebuffer (starting it on the first call); each window surface then feeds
 * a layer of it (wine-nx-probe/source/compositor.h). */
extern int   wine_nx_compositor_enabled( void );

/* Buttons reported by wine_nx_pointer_poll(). */
#define WINE_NX_POINTER_LEFT  0x1
#define WINE_NX_POINTER_RIGHT 0x2

struct wine_nx_surface
{
    struct window_surface surface;
    BOOL initial_redraw_done;
    RECT present_rect;
    POINT screen_origin;
    RECT clip_rect;
    BOOL has_clip;
    struct wine_nx_layer *layer;  /* the compositor's copy of the pixels, when it presents */
};

struct wine_nx_surface_entry
{
    HWND hwnd;
    RECT screen_rect;
    BOOL visible;
    struct wine_nx_layer *layer;  /* of the window's current surface */
    struct wine_nx_surface_entry *next;
};

/* The entries are only ever added to, from the threads of their windows. */
static pthread_mutex_t wine_nx_surface_entries_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct wine_nx_surface_entry *wine_nx_surface_entries;
static volatile int wine_nx_input_thread_started;
static void nxdrv_trace( const char *fmt, int a, int b, int c, int d );
static void nxdrv_trace_hot( const char *fmt, int a, int b, int c, int d );

/* Moves the drawn cursor while the program is too busy to pump messages, such
 * as OpenTTD loading its sprites. This thread has no TEB, so it must not call
 * into Wine: the first server call dereferences a NULL TEB. It only polls the
 * controller and presents; ProcessEvents sends the input from a Wine thread. */
static void *wine_nx_input_thread( void *arg )
{
    (void)arg;
    for (;;)
    {
        unsigned int buttons;
        int x, y;

        wine_nx_pointer_poll( &x, &y, &buttons );
        wine_nx_fb_present();
        usleep( 16000 );
    }
    return NULL;
}

static void wine_nx_start_input_thread(void)
{
    pthread_t thread;

    if (__atomic_exchange_n( &wine_nx_input_thread_started, 1, __ATOMIC_ACQ_REL )) return;
    if (pthread_create( &thread, NULL, wine_nx_input_thread, NULL ))
    {
        __atomic_store_n( &wine_nx_input_thread_started, 0, __ATOMIC_RELEASE );
        nxdrv_trace( "[NXINPUT] thread create failed", 0, 0, 0, 0 );
        return;
    }
    pthread_detach( thread );
    nxdrv_trace( "[NXINPUT] background polling started", 0, 0, 0, 0 );
}

static struct wine_nx_surface *wine_nx_surface_from_base( struct window_surface *surface )
{
    return CONTAINING_RECORD( surface, struct wine_nx_surface, surface );
}

static void nxdrv_trace( const char *fmt, int a, int b, int c, int d )
{
    char buf[160];
    if (!&wine_nx_runtime_trace || !&wine_nx_runtime_verbose || !wine_nx_runtime_verbose) return;
    snprintf( buf, sizeof(buf), fmt, a, b, c, d );
    wine_nx_runtime_trace( buf );
}

static void nxdrv_trace_hot( const char *fmt, int a, int b, int c, int d )
{
    static unsigned int count;

    if (count++ >= 120) return;
    nxdrv_trace( fmt, a, b, c, d );
}

/* Rare, user-triggered events (the on-screen keyboard opening or failing to)
 * are worth keeping in the log even with Verbose traces off, unlike the
 * per-frame lines nxdrv_trace gates on it. */
static void nxdrv_trace_always( const char *fmt, int a, int b, int c, int d )
{
    char buf[160];
    if (!&wine_nx_runtime_trace) return;
    snprintf( buf, sizeof(buf), fmt, a, b, c, d );
    wine_nx_runtime_trace( buf );
}

/* get_dib_stride / get_dib_image_size come from the win32u private headers. */

static void trace_surface_samples( const BITMAPINFO *color_info, const void *color_bits )
{
    static int sample_count;
    const DWORD *bits = color_bits;
    int width = color_info->bmiHeader.biWidth;
    int height = abs( color_info->bmiHeader.biHeight );
    DWORD p0, pc;
    int nonwhite = 0;
    int x, y;

    if (sample_count >= 40 || width <= 0 || height <= 0 || !bits) return;

    p0 = bits[0];
    pc = bits[(size_t)(height / 2) * width + width / 2];
    for (y = 0; y < height; y += 64)
    {
        for (x = 0; x < width; x += 64)
        {
            if ((bits[(size_t)y * width + x] & 0x00ffffff) != 0x00ffffff) nonwhite++;
        }
    }

    nxdrv_trace( "[NXDRV] samples p0=%08x pc=%08x nonwhite=%d",
                 (int)p0, (int)pc, nonwhite, 0 );
    sample_count++;
}

static BOOL wine_nx_surface_flush( struct window_surface *surface, const RECT *rect, const RECT *dirty,
                                   const BITMAPINFO *color_info, const void *color_bits, BOOL shape_changed,
                                   const BITMAPINFO *shape_info, const void *shape_bits );
static const struct window_surface_funcs wine_nx_surface_funcs;

static void wine_nx_surface_mark_full_dirty( struct window_surface *surface )
{
    struct wine_nx_surface *nx_surface = wine_nx_surface_from_base( surface );
    RECT dirty = nx_surface->present_rect;

    if (dirty.right <= 0 || dirty.bottom <= 0) return;

    window_surface_lock( surface );
    surface->bounds = dirty;
    window_surface_unlock( surface );
}

static BOOL wine_nx_surface_present_full( struct window_surface *surface )
{
    struct wine_nx_surface *nx_surface = wine_nx_surface_from_base( surface );
    char color_buf[FIELD_OFFSET( BITMAPINFO, bmiColors[256] )];
    BITMAPINFO *color_info = (BITMAPINFO *)color_buf;
    RECT dirty = nx_surface->present_rect;
    void *color_bits;
    BOOL ret = FALSE;

    if (dirty.right <= 0 || dirty.bottom <= 0) return FALSE;

    window_surface_lock( surface );
    color_bits = window_surface_get_color( surface, color_info );
    nxdrv_trace_hot( "[NXDRV] present_full bits=%d %dx%d",
                     color_bits ? 1 : 0, dirty.right, dirty.bottom, 0 );
    if (color_bits)
        ret = wine_nx_surface_flush( surface, &surface->rect, &dirty, color_info, color_bits,
                                     FALSE, NULL, NULL );
    if (ret) SetRectEmpty( &surface->bounds );
    window_surface_unlock( surface );
    return ret;
}

static BOOL wine_nx_surface_present_screen_rect( struct window_surface *surface, const RECT *screen_rect )
{
    struct wine_nx_surface *nx_surface = wine_nx_surface_from_base( surface );
    char color_buf[FIELD_OFFSET( BITMAPINFO, bmiColors[256] )];
    BITMAPINFO *color_info = (BITMAPINFO *)color_buf;
    RECT dirty = *screen_rect;
    void *color_bits;
    BOOL ret = FALSE;

    OffsetRect( &dirty, -nx_surface->screen_origin.x, -nx_surface->screen_origin.y );
    if (!intersect_rect( &dirty, &dirty, &nx_surface->present_rect )) return FALSE;

    window_surface_lock( surface );
    color_bits = window_surface_get_color( surface, color_info );
    if (color_bits)
        ret = wine_nx_surface_flush( surface, &surface->rect, &dirty, color_info, color_bits,
                                     FALSE, NULL, NULL );
    window_surface_unlock( surface );
    return ret;
}

/* With wine_nx_surface_entries_mutex held. */
static struct wine_nx_surface_entry *find_surface_entry_locked( HWND hwnd, BOOL create )
{
    struct wine_nx_surface_entry *entry;

    for (entry = wine_nx_surface_entries; entry; entry = entry->next)
        if (entry->hwnd == hwnd) return entry;

    if (!create) return NULL;
    if (!(entry = calloc( 1, sizeof(*entry) ))) return NULL;
    entry->hwnd = hwnd;
    entry->next = wine_nx_surface_entries;
    wine_nx_surface_entries = entry;
    return entry;
}

static struct wine_nx_surface_entry *wine_nx_find_surface_entry( HWND hwnd, BOOL create )
{
    struct wine_nx_surface_entry *entry;

    pthread_mutex_lock( &wine_nx_surface_entries_mutex );
    entry = find_surface_entry_locked( hwnd, create );
    pthread_mutex_unlock( &wine_nx_surface_entries_mutex );
    return entry;
}

/* With the compositor, each window shows through the layer of its current
 * surface. A surface forgets its layer here before destroying it, so a layer
 * found under the entries mutex stays alive while the mutex is held. */
static void wine_nx_show_window_layer( HWND hwnd, struct wine_nx_layer *layer, const POINT *origin,
                                       const RECT *present )
{
    struct wine_nx_surface_entry *entry;

    pthread_mutex_lock( &wine_nx_surface_entries_mutex );
    if ((entry = find_surface_entry_locked( hwnd, TRUE )))
    {
        /* A resized window gets a new surface; the old one can outlive this. */
        if (entry->layer && entry->layer != layer) wine_nx_layer_place( entry->layer, 0, 0, 0, 0 );
        entry->layer = layer;
    }
    wine_nx_layer_place( layer, origin->x, origin->y, present->right, present->bottom );
    pthread_mutex_unlock( &wine_nx_surface_entries_mutex );
}

static BOOL wine_nx_hide_window_layer( HWND hwnd )
{
    struct wine_nx_surface_entry *entry;
    BOOL ret = FALSE;

    pthread_mutex_lock( &wine_nx_surface_entries_mutex );
    if ((entry = find_surface_entry_locked( hwnd, FALSE )) && entry->layer)
    {
        wine_nx_layer_place( entry->layer, 0, 0, 0, 0 );
        ret = TRUE;
    }
    pthread_mutex_unlock( &wine_nx_surface_entries_mutex );
    return ret;
}

static void wine_nx_forget_layer( struct wine_nx_layer *layer )
{
    struct wine_nx_surface_entry *entry;

    pthread_mutex_lock( &wine_nx_surface_entries_mutex );
    for (entry = wine_nx_surface_entries; entry; entry = entry->next)
        if (entry->layer == layer) entry->layer = NULL;
    pthread_mutex_unlock( &wine_nx_surface_entries_mutex );
}

/* Give the compositor the stacking order of the top-level windows, topmost first. */
static void wine_nx_restack_layers( void )
{
    struct wine_nx_layer *layers[256];
    struct wine_nx_surface_entry *entry;
    int count = 0;
    HWND hwnd;

    pthread_mutex_lock( &wine_nx_surface_entries_mutex );
    for (hwnd = get_window_relative( get_desktop_window(), GW_CHILD ); hwnd && count < (int)ARRAY_SIZE(layers);
         hwnd = get_window_relative( hwnd, GW_HWNDNEXT ))
    {
        if ((entry = find_surface_entry_locked( hwnd, FALSE )) && entry->layer) layers[count++] = entry->layer;
    }
    wine_nx_compositor_restack( layers, count );
    pthread_mutex_unlock( &wine_nx_surface_entries_mutex );
}

static BOOL wine_nx_get_cached_screen_rect( HWND hwnd, RECT *rect )
{
    struct wine_nx_surface_entry *entry = wine_nx_find_surface_entry( hwnd, FALSE );

    if (!entry || IsRectEmpty( &entry->screen_rect )) return FALSE;
    *rect = entry->screen_rect;
    return TRUE;
}

static void wine_nx_note_surface_present( HWND hwnd, const POINT *origin, const RECT *present_rect )
{
    struct wine_nx_surface_entry *entry = wine_nx_find_surface_entry( hwnd, TRUE );

    if (!entry) return;
    entry->screen_rect = *present_rect;
    OffsetRect( &entry->screen_rect, origin->x, origin->y );
    entry->visible = !IsRectEmpty( &entry->screen_rect );
}

static void wine_nx_note_surface_hidden( HWND hwnd )
{
    struct wine_nx_surface_entry *entry = wine_nx_find_surface_entry( hwnd, FALSE );

    if (entry) entry->visible = FALSE;
}

static BOOL wine_nx_present_hwnd_surface( HWND hwnd, const RECT *screen_rect )
{
    struct window_surface *surface, *driver_surface;
    UINT raw_dpi = 0;
    BOOL ret = FALSE;

    if (!(surface = window_surface_get( hwnd ))) return FALSE;
    get_win_monitor_dpi( hwnd, &raw_dpi );
    driver_surface = get_driver_window_surface( surface, raw_dpi );
    if (driver_surface && driver_surface->funcs == &wine_nx_surface_funcs)
        ret = screen_rect ? wine_nx_surface_present_screen_rect( driver_surface, screen_rect ) :
                            wine_nx_surface_present_full( driver_surface );
    window_surface_release( surface );
    return ret;
}

static void wine_nx_present_visible_popups( HWND except )
{
    struct wine_nx_surface_entry *entry;

    for (entry = wine_nx_surface_entries; entry; entry = entry->next)
    {
        if (!entry->visible || entry->hwnd == except) continue;
        if (!(get_window_long( entry->hwnd, GWL_STYLE ) & WS_POPUP)) continue;
        wine_nx_present_hwnd_surface( entry->hwnd, NULL );
    }
}

static HWND wine_nx_find_popup_restore_owner( HWND hwnd, HWND owner_hint, const RECT *old_rect )
{
    HWND owner = owner_hint;

    if (!owner) owner = get_window_relative( hwnd, GW_OWNER );
    if (!owner && old_rect && !IsRectEmpty( old_rect ))
        owner = NtUserWindowFromPoint( (old_rect->left + old_rect->right) / 2,
                                      (old_rect->top + old_rect->bottom) / 2 );
    if (!owner) owner = get_active_window();
    if (!owner) owner = get_focus();
    if (owner) owner = NtUserGetAncestor( owner, GA_ROOT );
    if (owner == hwnd) owner = 0;
    return owner;
}

static void wine_nx_restore_popup_owner( HWND hwnd, HWND owner_hint, const RECT *old_rect )
{
    HWND owner;
    BOOL presented;

    if (!(get_window_long( hwnd, GWL_STYLE ) & WS_POPUP)) return;
    wine_nx_note_surface_hidden( hwnd );
    owner = wine_nx_find_popup_restore_owner( hwnd, owner_hint, old_rect );
    if (!owner || owner == hwnd) return;

    presented = wine_nx_present_hwnd_surface( owner, old_rect );
    wine_nx_present_visible_popups( hwnd );
    nxdrv_trace( "[NXDRV] restore popup=%x owner=%x presented=%d",
                 (int)(ULONG_PTR)hwnd, (int)(ULONG_PTR)owner, presented, 0 );
    if (!presented)
        NtUserRedrawWindow( owner, NULL, 0, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME |
                           RDW_ALLCHILDREN | RDW_UPDATENOW );
}

static void wine_nx_surface_set_clip( struct window_surface *surface, const RECT *rects, UINT count )
{
    struct wine_nx_surface *nx_surface = wine_nx_surface_from_base( surface );
    RECT clip = {0};
    UINT i;

    nx_surface->has_clip = FALSE;
    if (!rects || !count) return;

    for (i = 0; i < count; i++)
    {
        RECT rect;

        if (!intersect_rect( &rect, &rects[i], &nx_surface->present_rect )) continue;
        if (!nx_surface->has_clip) clip = rect;
        else union_rect( &clip, &clip, &rect );
        nx_surface->has_clip = TRUE;
    }
    nx_surface->clip_rect = clip;
}

/* Blit the dirty region of the (BGRX, top-down) DIB to the linear RGBA8888
 * framebuffer at the surface's screen position, clipped to the screen. */
static BOOL wine_nx_surface_flush( struct window_surface *surface, const RECT *rect, const RECT *dirty,
                                   const BITMAPINFO *color_info, const void *color_bits, BOOL shape_changed,
                                   const BITMAPINFO *shape_info, const void *shape_bits )
{
    struct wine_nx_surface *nx_surface = wine_nx_surface_from_base( surface );
    struct wine_nx_surface_entry *entry = wine_nx_find_surface_entry( surface->hwnd, FALSE );
    RECT blit = *dirty;
    int sw = color_info->bmiHeader.biWidth;
    int sh = abs( color_info->bmiHeader.biHeight );
    int fbw = 0, fbh = 0, fbstride = 0;
    DWORD *fb;
    int sy, sx;

    (void)rect;

    /* The compositor keeps every pixel; where the window shows is its placement. */
    if (nx_surface->layer)
    {
        wine_nx_layer_update( nx_surface->layer, color_bits, sw, dirty->left, dirty->top, dirty->right, dirty->bottom );
        return TRUE;
    }

    /* A queued paint can flush after SWP_HIDEWINDOW.  Keep the DIB contents,
     * but do not put the closed popup back over its restored owner. */
    if (entry && !entry->visible) return TRUE;

    if (!intersect_rect( &blit, &blit, &nx_surface->present_rect )) return TRUE;
    if (nx_surface->has_clip && !intersect_rect( &blit, &blit, &nx_surface->clip_rect )) return TRUE;

    nxdrv_trace_hot( "[NXDRV] flush dirty=%d,%d-%d,%d", blit.left, blit.top, blit.right, blit.bottom );
    trace_surface_samples( color_info, color_bits );
    fb = wine_nx_fb_lock( &fbw, &fbh, &fbstride );
    nxdrv_trace_hot( "[NXDRV] fb_lock -> fb=%d fbw=%d fbh=%d stride=%d", fb ? 1 : 0, fbw, fbh, fbstride );
    if (!fb) return TRUE;

    for (sy = blit.top; sy < blit.bottom; sy++)
    {
        int v = sy;
        int dy = nx_surface->screen_origin.y + sy;
        const DWORD *src;
        DWORD *dst;

        if (v < 0 || v >= sh || dy < 0 || dy >= fbh) continue;
        src = (const DWORD *)color_bits + (size_t)v * sw;
        dst = fb + (size_t)dy * fbstride;
        for (sx = blit.left; sx < blit.right; sx++)
        {
            int u = sx;
            int dx = nx_surface->screen_origin.x + sx;
            DWORD p;
            if (u < 0 || u >= sw || dx < 0 || dx >= fbw) continue;
            p = src[u];
            /* BGRX (0x00RRGGBB) -> RGBA8888 (R,G,B,A bytes), opaque */
            dst[dx] = 0xff000000u | (p & 0x0000ff00u) | ((p >> 16) & 0xffu) | ((p & 0xffu) << 16);
        }
    }

    wine_nx_fb_unlock();
    /* Some applications (including OpenTTD's GDI backend) flush a surface
     * from an update path that does not pass through NtUserEndPaint.  The
     * dirty pixels are already copied above; publish that buffer here so the
     * Switch display cannot remain on the initial white frame until another
     * unrelated message arrives. */
    wine_nx_fb_present();
    return TRUE;
}

static void wine_nx_surface_destroy( struct window_surface *surface )
{
    struct wine_nx_surface *nx_surface = wine_nx_surface_from_base( surface );

    /* The generic window_surface_release() frees the header storage. */
    if (!nx_surface->layer) return;
    wine_nx_forget_layer( nx_surface->layer );
    wine_nx_layer_destroy( nx_surface->layer );
    nx_surface->layer = NULL;
}

static const struct window_surface_funcs wine_nx_surface_funcs =
{
    wine_nx_surface_set_clip,
    wine_nx_surface_flush,
    wine_nx_surface_destroy,
};

/**********************************************************************
 *           wine_nx_drv_UpdateDisplayDevices
 *
 * Report a single 1280x720 primary monitor so win32u has a real virtual
 * screen; without it the desktop is 0x0 and every window clips to nothing.
 */
#define WINE_NX_SCREEN_W 1280
#define WINE_NX_SCREEN_H 720

UINT wine_nx_drv_UpdateDisplayDevices( const struct gdi_device_manager *dm, void *param )
{
    static const DWORD source_flags = DISPLAY_DEVICE_ATTACHED_TO_DESKTOP |
                                      DISPLAY_DEVICE_PRIMARY_DEVICE | DISPLAY_DEVICE_VGA_COMPATIBLE;
    RECT rc = { 0, 0, WINE_NX_SCREEN_W, WINE_NX_SCREEN_H };
    struct pci_id pci_id = { 0 };
    struct gdi_monitor monitor = { .rc_monitor = rc, .rc_work = rc };
    DEVMODEW mode =
    {
        .dmSize   = sizeof(mode),
        .dmFields = DM_DISPLAYORIENTATION | DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL |
                    DM_DISPLAYFLAGS | DM_DISPLAYFREQUENCY,
        .dmBitsPerPel = 32, .dmPelsWidth = WINE_NX_SCREEN_W, .dmPelsHeight = WINE_NX_SCREEN_H,
        .dmDisplayFrequency = 60,
    };
    UINT dpi = NtUserGetSystemDpiForProcess( NULL );
    DEVMODEW current = mode;

    dm->add_gpu( "Wine NX GPU", &pci_id, NULL, param );
    dm->add_source( "Default", source_flags, dpi, param );
    dm->add_monitor( &monitor, param );
    current.dmFields |= DM_POSITION;
    dm->add_modes( &current, 1, &mode, param );
    nxdrv_trace( "[NXDRV] UpdateDisplayDevices -> %dx%d", WINE_NX_SCREEN_W, WINE_NX_SCREEN_H, 0, 0 );
    return STATUS_SUCCESS;
}

/**********************************************************************
 *           wine_nx_drv_ProcessEvents
 *
 * Expose the Switch pointer as an absolute mouse: the right analog stick
 * moves the cursor, A is the left button and B the right, and a touchscreen
 * contact acts as a left press under the finger.  This gives classic Win32
 * applications useful input immediately, including non-client hit testing,
 * menus and controls.
 */
/* The button flags that take the delivered buttons (last) to the held ones,
 * in two steps: a button pressed and released since the last delivery clicks
 * (down, then up), and one released and pressed again goes up, then down. */
static void wine_nx_pointer_flags( unsigned int last, unsigned int held, unsigned int pressed,
                                   unsigned int released, DWORD *first, DWORD *second )
{
    static const struct { unsigned int button; DWORD down, up; } map[] =
    {
        { WINE_NX_POINTER_LEFT, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP },
        { WINE_NX_POINTER_RIGHT, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP },
    };
    unsigned int i;

    *first = *second = 0;
    for (i = 0; i < ARRAY_SIZE(map); i++)
    {
        unsigned int button = map[i].button;

        if (last & button)
        {
            if ((held & button) && !(released & button)) continue;
            *first |= map[i].up;
            if (held & button) *second |= map[i].down;
        }
        else if ((held | pressed) & button)
        {
            *first |= map[i].down;
            if (!(held & button)) *second |= map[i].up;
        }
    }
}

/* The controller stands in for the keyboard the console does not have. The
 * runtime polls it and keeps the held controls in wine_nx_pad_key_state, with
 * the virtual-key code of each in wine_nx_pad_keys (wine-nx-probe/source/
 * runtime.c, overridable through switch/wine/keys.txt and a program's own
 * NAME.keys.txt). */
#define WINE_NX_PAD_KEY_COUNT 16
extern unsigned int wine_nx_pad_key_state __attribute__((weak));
extern unsigned short wine_nx_pad_keys[] __attribute__((weak));

static BOOL wine_nx_send_keys(void)
{
    static unsigned int delivered;
    unsigned int held, changed, i;

    if (!&wine_nx_pad_key_state || !wine_nx_pad_keys) return FALSE;
    held = __atomic_load_n( &wine_nx_pad_key_state, __ATOMIC_RELAXED );
    if (!(changed = held ^ delivered)) return FALSE;

    for (i = 0; i < WINE_NX_PAD_KEY_COUNT; i++)
    {
        INPUT input = {0};
        UINT scan;

        if (!(changed & (1u << i)) || !wine_nx_pad_keys[i]) continue;
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = wine_nx_pad_keys[i];
        input.ki.dwFlags = (held & (1u << i)) ? 0 : KEYEVENTF_KEYUP;
        /* DirectInput names keys by scan code, and an arrow is E0 48, not 48. */
        scan = NtUserMapVirtualKeyEx( input.ki.wVk, MAPVK_VK_TO_VSC_EX, NtUserGetKeyboardLayout( 0 ) );
        input.ki.wScan = scan & 0xff;
        if ((scan & 0xff00) == 0xe000) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        NtUserSendHardwareInput( 0, 0, &input, 0 );
    }
    nxdrv_trace( "[NXINPUT] keys held=%x changed=%x", held, changed, 0, 0 );
    delivered = held;
    return TRUE;
}

static void wine_nx_send_mouse( int x, int y, DWORD flags )
{
    INPUT input = {0};

    input.type = INPUT_MOUSE;
    input.mi.dx = x;
    input.mi.dy = y;
    input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | flags;
    NtUserSendHardwareInput( 0, 0, &input, 0 );
}

/* Minus + right stick click force the on-screen keyboard open regardless of
 * focus, for programs is_text_entry_window (input.c) never recognises as a
 * text control, or with no-swkbd-auto.txt turning the automatic trigger off.
 * Read from wine_nx_swkbd_hotkey_state, not wine_nx_pad_key_state: the latter
 * is zeroed while a program reads the controller through XInput (runtime.c),
 * which this system-level shortcut must keep working through regardless. */
extern const unsigned int wine_nx_pad_key_minus_bit __attribute__((weak));
extern const unsigned int wine_nx_pad_key_stickr_bit __attribute__((weak));
extern unsigned int wine_nx_swkbd_hotkey_state __attribute__((weak));

/* 0 = idle, 1 = combo held (or the keyboard is up because of it). More than
 * one thread can reach here through ProcessEvents at once (a loading screen
 * and the main render loop, for instance), so this is a lock, not a plain
 * bool: only the thread whose compare-exchange takes it from 0 to 1 opens
 * the keyboard, and it does not open again until the combo is released. */
static unsigned int wine_nx_swkbd_hotkey_held;

static void wine_nx_swkbd_hotkey(void)
{
    unsigned int keys, combo, expected;
    BOOL opened;

    if (!&wine_nx_pad_key_minus_bit || !&wine_nx_swkbd_hotkey_state) return;
    combo = wine_nx_pad_key_minus_bit | wine_nx_pad_key_stickr_bit;
    keys = __atomic_load_n( &wine_nx_swkbd_hotkey_state, __ATOMIC_RELAXED );
    if ((keys & combo) != combo)
    {
        __atomic_store_n( &wine_nx_swkbd_hotkey_held, 0, __ATOMIC_RELEASE );
        return;
    }
    expected = 0;
    if (!__atomic_compare_exchange_n( &wine_nx_swkbd_hotkey_held, &expected, 1, FALSE,
                                      __ATOMIC_ACQ_REL, __ATOMIC_RELAXED ))
        return;  /* already open, or another thread just won the race to open it */
    nxdrv_trace_always( "[NXDRV] swkbd hotkey: keys=%#x combo=%#x", keys, combo, 0, 0 );
    opened = NtUserShowSoftwareKeyboard( 0 );
    nxdrv_trace_always( "[NXDRV] swkbd hotkey opened=%d", opened, 0, 0, 0 );
}

/* Whether to draw the arrow, from the cursor the program set and its show
 * count. Until a program sets a cursor there is none and the arrow is shown; a
 * program that later sets none, over a cursor it draws itself, hides it, and
 * so does a negative show count (ShowCursor). */
static BOOL wine_nx_cursor_visible_for( HCURSOR cursor, int count, BOOL *seen )
{
    if (cursor) *seen = TRUE;
    return count >= 0 && (cursor || !*seen);
}

/* Wine tells the display driver about cursor changes with WM_WINE_SETCURSOR,
 * which the Horizon server does not queue. Read the state instead. */
static void wine_nx_update_cursor( void )
{
    static BOOL seen;
    struct object_lock lock = OBJECT_LOCK_INIT;
    const input_shm_t *input_shm;
    HCURSOR cursor = 0;
    int count = 0;
    NTSTATUS status;

    while ((status = get_shared_input( 0, &lock, &input_shm )) == STATUS_PENDING)
    {
        cursor = wine_server_ptr_handle( input_shm->cursor );
        count = input_shm->cursor_count;
    }
    if (status) return;
    wine_nx_cursor_show( wine_nx_cursor_visible_for( cursor, count, &seen ) );
}

BOOL wine_nx_drv_ProcessEvents( DWORD mask )
{
    static unsigned int last_buttons;
    unsigned int buttons, pressed, released;
    DWORD first, second;
    BOOL moved, keys;
    int x, y;

    (void)mask;
    wine_nx_fb_present();
    /* Poll here too, then deliver everything the polls saw since the last
     * call, including those of the background thread. */
    wine_nx_pointer_poll( &x, &y, &buttons );
    moved = wine_nx_pointer_take( &x, &y, &buttons, &pressed, &released );
    wine_nx_pointer_flags( last_buttons, buttons, pressed, released, &first, &second );
    if (moved || first) wine_nx_send_mouse( x, y, (moved ? MOUSEEVENTF_MOVE : 0) | first );
    if (second) wine_nx_send_mouse( x, y, second );
    if (first) nxdrv_trace( "[NXINPUT] buttons=%x flags=%x,%x x=%d", buttons, first, second, x );
    else if (moved) nxdrv_trace_hot( "[NXINPUT] move x=%d y=%d buttons=%x", x, y, buttons, 0 );
    last_buttons = buttons;
    keys = wine_nx_send_keys();
    wine_nx_swkbd_hotkey();
    wine_nx_update_cursor();
    wine_nx_fb_present();
    return moved || first || keys;
}

/**********************************************************************
 *           wine_nx_drv_SetCursor
 *
 * For a server that queues WM_WINE_SETCURSOR; the shared state is authoritative.
 */
void wine_nx_drv_SetCursor( HWND hwnd, HCURSOR cursor )
{
    (void)hwnd;
    (void)cursor;
    wine_nx_update_cursor();
}

/* Opens Horizon's on-screen keyboard, in the runtime (wine_nx_show_keyboard,
 * wine-nx-probe/source/runtime.c: swkbdShow, blocking this guest thread). */
extern int wine_nx_show_keyboard( const char *header, const char *initial, char *out, size_t out_size );

/* Minimal UTF-8 to UTF-16 decoder for swkbd's output, which libnx gives as
 * UTF-8. Malformed input becomes U+FFFD rather than stopping the conversion;
 * out_count is in WCHARs, including room left for the terminating 0. */
static int wine_nx_utf8_to_utf16( const char *utf8, WCHAR *out, int out_count )
{
    const unsigned char *p = (const unsigned char *)utf8;
    int n = 0;

    while (*p && n + 1 < out_count)
    {
        unsigned int cp;
        int extra;

        if (*p < 0x80) { cp = *p++; extra = 0; }
        else if ((*p & 0xe0) == 0xc0) { cp = *p++ & 0x1f; extra = 1; }
        else if ((*p & 0xf0) == 0xe0) { cp = *p++ & 0x0f; extra = 2; }
        else if ((*p & 0xf8) == 0xf0) { cp = *p++ & 0x07; extra = 3; }
        else { p++; out[n++] = 0xfffd; continue; }

        while (extra-- && (*p & 0xc0) == 0x80) cp = (cp << 6) | (*p++ & 0x3f);

        if (cp >= 0x10000 && n + 2 < out_count)
        {
            cp -= 0x10000;
            out[n++] = 0xd800 + (cp >> 10);
            out[n++] = 0xdc00 + (cp & 0x3ff);
        }
        else out[n++] = (WCHAR)cp;
    }
    out[n] = 0;
    return n;
}

/* One key, down then up, with a real scan code the way wine_nx_send_keys
 * derives it - so DirectInput's keyboard state buffer and GetAsyncKeyState
 * see it, not only whatever WM_CHAR a message loop's TranslateMessage makes
 * of it. */
static void wine_nx_send_vk( HKL layout, BYTE vk, BOOL up )
{
    INPUT input = {0};
    UINT scan = NtUserMapVirtualKeyEx( vk, MAPVK_VK_TO_VSC_EX, layout );

    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.wScan = scan & 0xff;
    input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    if ((scan & 0xff00) == 0xe000) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    NtUserSendHardwareInput( 0, 0, &input, 0 );
}

/* A character with no key on the current layout (most of Unicode) has no
 * scan code to send; KEYEVENTF_UNICODE at least reaches a WM_CHAR handler,
 * though not GetAsyncKeyState or DirectInput's keyboard buffer, same as
 * real Windows: typing such a character with a real keyboard is no
 * different. */
static void wine_nx_send_unicode_char( WCHAR ch )
{
    INPUT input = {0};

    input.type = INPUT_KEYBOARD;
    input.ki.wScan = ch;
    input.ki.dwFlags = KEYEVENTF_UNICODE;
    NtUserSendHardwareInput( 0, 0, &input, 0 );
    input.ki.dwFlags |= KEYEVENTF_KEYUP;
    NtUserSendHardwareInput( 0, 0, &input, 0 );
}

/**********************************************************************
 *           wine_nx_drv_ShowSoftwareKeyboard
 *
 * Delivered to whichever window has focus when the player accepts what they
 * typed, the same way wine_nx_send_keys delivers the controller's remapped
 * keys - hwnd only picked the automatic trigger's target class (input.c),
 * not where the text goes, so it is unused here.
 */
BOOL wine_nx_drv_ShowSoftwareKeyboard( HWND hwnd )
{
    char utf8[512];
    WCHAR utf16[512];
    HKL layout;
    int i, len;

    (void)hwnd;
    nxdrv_trace_always( "[NXDRV] ShowSoftwareKeyboard enter", 0, 0, 0, 0 );
    if (!wine_nx_show_keyboard( "Enter text", "", utf8, sizeof(utf8) ))
    {
        nxdrv_trace_always( "[NXDRV] ShowSoftwareKeyboard: swkbdShow failed or was cancelled", 0, 0, 0, 0 );
        return FALSE;
    }
    len = wine_nx_utf8_to_utf16( utf8, utf16, ARRAY_SIZE(utf16) );
    layout = NtUserGetKeyboardLayout( 0 );
    for (i = 0; i < len; i++)
    {
        WORD scan = NtUserVkKeyScanEx( utf16[i], layout );
        BYTE vk = LOBYTE( scan ), state = HIBYTE( scan );

        if (scan == 0xffff) { wine_nx_send_unicode_char( utf16[i] ); continue; }
        if (state & 1) wine_nx_send_vk( layout, VK_SHIFT, FALSE );
        if (state & 2) wine_nx_send_vk( layout, VK_CONTROL, FALSE );
        if (state & 4) wine_nx_send_vk( layout, VK_MENU, FALSE );
        wine_nx_send_vk( layout, vk, FALSE );
        wine_nx_send_vk( layout, vk, TRUE );
        if (state & 4) wine_nx_send_vk( layout, VK_MENU, TRUE );
        if (state & 2) wine_nx_send_vk( layout, VK_CONTROL, TRUE );
        if (state & 1) wine_nx_send_vk( layout, VK_SHIFT, TRUE );
    }
    /* Accepting the on-screen keyboard is the player confirming that text,
     * the way Enter confirms a line typed on a real keyboard: without it, a
     * field built to end editing on Enter (most of them, for a game with no
     * on-screen "done" button of its own) never leaves edit mode and the
     * program never sees a completed entry, even though it saw every
     * character. */
    wine_nx_send_vk( layout, VK_RETURN, FALSE );
    wine_nx_send_vk( layout, VK_RETURN, TRUE );
    nxdrv_trace_always( "[NXDRV] ShowSoftwareKeyboard chars=%d", len, 0, 0, 0 );
    return TRUE;
}

/**********************************************************************
 *           wine_nx_drv_SetCursorPos
 */
BOOL wine_nx_drv_SetCursorPos( INT x, INT y )
{
    wine_nx_pointer_set_pos( x, y );
    return TRUE;
}

/**********************************************************************
 *           wine_nx_drv_CreateWindow
 */
BOOL wine_nx_drv_CreateWindow( HWND hwnd )
{
    nxdrv_trace( "[NXDRV] CreateWindow hwnd=%p", (int)(ULONG_PTR)hwnd, 0, 0, 0 );
    return TRUE;
}

/**********************************************************************
 *           wine_nx_drv_CreateWindowSurface
 */
BOOL wine_nx_drv_CreateWindowSurface( HWND hwnd, BOOL layered, const RECT *surface_rect,
                                      struct window_surface **surface )
{
    char buffer[FIELD_OFFSET( BITMAPINFO, bmiColors[256] )];
    BITMAPINFO *info = (BITMAPINFO *)buffer;
    int width  = surface_rect->right - surface_rect->left;
    int height = surface_rect->bottom - surface_rect->top;

    if (*surface) window_surface_release( *surface );
    *surface = NULL;
    if (width <= 0 || height <= 0) return TRUE;

    memset( info, 0, sizeof(*info) );
    info->bmiHeader.biSize        = sizeof(info->bmiHeader);
    info->bmiHeader.biWidth       = width;
    info->bmiHeader.biHeight      = -height; /* top-down */
    info->bmiHeader.biPlanes      = 1;
    info->bmiHeader.biBitCount    = 32;
    info->bmiHeader.biSizeImage   = get_dib_image_size( info );
    info->bmiHeader.biCompression = BI_RGB;

    *surface = window_surface_create( sizeof(struct wine_nx_surface), &wine_nx_surface_funcs,
                                      hwnd, surface_rect, info, 0 );
    /* Made with the surface, the layer gets every flush of it. win32u fills a
     * new surface with white: a window that never paints with GDI, like a
     * Direct3D game's, would show that white whenever the compositor has the
     * screen, so it starts black, like the layer. */
    if (*surface && wine_nx_compositor_enabled())
    {
        memset( window_surface_get_color( *surface, info ), 0, info->bmiHeader.biSizeImage );
        wine_nx_surface_from_base( *surface )->layer = wine_nx_layer_create( width, height );
    }
    nxdrv_trace( "[NXDRV] CreateWindowSurface rect=%d,%d %dx%d", surface_rect->left, surface_rect->top,
                 width, height );
    nxdrv_trace( "[NXDRV] surface_create -> %d", *surface ? 1 : 0, 0, 0, 0 );
    wine_nx_start_input_thread();
    return TRUE;
}

/**********************************************************************
 *           wine_nx_drv_WindowPosChanged
 *
 * Present the current surface contents after a geometry/visibility change.
 */
void wine_nx_drv_WindowPosChanged( HWND hwnd, HWND insert_after, HWND owner_hint, UINT swp_flags,
                                   const struct window_rects *new_rects, struct window_surface *surface )
{
    BOOL initial_redraw = FALSE;
    RECT old_screen_rect;
    BOOL has_old_screen_rect = wine_nx_get_cached_screen_rect( hwnd, &old_screen_rect );

    nxdrv_trace( "[NXDRV] WindowPosChanged surface=%d swp=%x", surface ? 1 : 0, swp_flags, 0, 0 );
    if (swp_flags & SWP_HIDEWINDOW)
    {
        wine_nx_note_surface_hidden( hwnd );
        /* The compositor shows what was under the window by itself. */
        if (wine_nx_hide_window_layer( hwnd )) return;
        wine_nx_restore_popup_owner( hwnd, owner_hint, has_old_screen_rect ? &old_screen_rect : NULL );
        wine_nx_fb_present();
        return;
    }
    if (surface)
    {
        if (surface->funcs == &wine_nx_surface_funcs)
        {
            struct wine_nx_surface *nx_surface = wine_nx_surface_from_base( surface );
            RECT allocation = { 0, 0, surface->rect.right - surface->rect.left,
                                surface->rect.bottom - surface->rect.top };
            int new_width = new_rects->visible.right - new_rects->visible.left;
            int new_height = new_rects->visible.bottom - new_rects->visible.top;

            if (!nx_surface->layer && nx_surface->initial_redraw_done &&
                (nx_surface->screen_origin.x != new_rects->visible.left ||
                 nx_surface->screen_origin.y != new_rects->visible.top ||
                 nx_surface->present_rect.right != new_width ||
                 nx_surface->present_rect.bottom != new_height))
                wine_nx_restore_popup_owner( hwnd, owner_hint,
                                             has_old_screen_rect ? &old_screen_rect : NULL );

            initial_redraw = !nx_surface->initial_redraw_done;
            nx_surface->initial_redraw_done = TRUE;
            nx_surface->screen_origin.x = new_rects->visible.left;
            nx_surface->screen_origin.y = new_rects->visible.top;
            nx_surface->present_rect.left = nx_surface->present_rect.top = 0;
            nx_surface->present_rect.right = new_width;
            nx_surface->present_rect.bottom = new_height;
            intersect_rect( &nx_surface->present_rect, &nx_surface->present_rect, &allocation );
            wine_nx_note_surface_present( hwnd, &nx_surface->screen_origin, &nx_surface->present_rect );
            if (nx_surface->layer)
            {
                /* Only the visible part is drawn, never the rest of the allocation. */
                wine_nx_show_window_layer( hwnd, nx_surface->layer, &nx_surface->screen_origin,
                                           &nx_surface->present_rect );
                wine_nx_restack_layers();
            }
            nxdrv_trace( "[NXDRV] present rect=%d,%d %dx%d",
                         nx_surface->screen_origin.x, nx_surface->screen_origin.y,
                         nx_surface->present_rect.right, nx_surface->present_rect.bottom );

            wine_nx_surface_mark_full_dirty( surface );
            if (!wine_nx_surface_present_full( surface ))
                window_surface_flush( surface );

            /* Child controls can paint while the top-level still owns the dummy
             * surface during ShowWindow.  Repaint the complete hierarchy once
             * the real surface has been installed so those discarded pixels are
             * produced again on the drawable surface. */
            if (initial_redraw)
            {
                nxdrv_trace( "[NXDRV] initial redraw hwnd=%p", (int)(ULONG_PTR)hwnd, 0, 0, 0 );
                NtUserRedrawWindow( hwnd, NULL, 0, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME |
                                   RDW_ALLCHILDREN | RDW_UPDATENOW );
                wine_nx_surface_mark_full_dirty( surface );
                wine_nx_surface_present_full( surface );
            }
        }
        else window_surface_flush( surface );
        wine_nx_fb_present();
    }
}
