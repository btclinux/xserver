/*
 * Copyright 2008 Tungsten Graphics, Inc., Cedar Park, Texas.
 * Copyright 2019 NVIDIA CORPORATION
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL TUNGSTEN GRAPHICS AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 *
 * Author: Alan Hourihane <alanh@tungstengraphics.com>
 * Additional contributors:
 *   Aaron Plattner <aplattner@nvidia.com>
 */

#include <errno.h>
#include <drm.h>
#include <xf86drm.h>
#include <xf86Crtc.h>
#include <damage.h>
#include <X11/extensions/dpmsconst.h>
#include <shadow.h>
#ifdef GLAMOR_HAS_GBM
#define GLAMOR_FOR_XORG 1
#include "glamor.h"
#include <gbm.h>
#endif

#include "drmmode_display.h"
#define MS_LOGLEVEL_DEBUG 4

struct ms_vrr_priv {
    Bool variable_refresh;
};

typedef enum {
    OPTION_SW_CURSOR,
    OPTION_DEVICE_PATH,
    OPTION_SHADOW_FB,
    OPTION_ACCEL_METHOD,
    OPTION_PAGEFLIP,
    OPTION_ZAPHOD_HEADS,
    OPTION_DOUBLE_SHADOW,
    OPTION_ATOMIC,
    OPTION_VARIABLE_REFRESH,
    OPTION_USE_GAMMA_LUT,
} modesettingOpts;

typedef struct
{
    int fd;
    int fd_ref;
    unsigned long fd_wakeup_registered; /* server generation for which fd has been registered for wakeup handling */
    int fd_wakeup_ref;
    unsigned int assigned_crtcs;
} modesettingEntRec, *modesettingEntPtr;

typedef void (*ms_drm_handler_proc)(uint64_t frame,
                                    uint64_t usec,
                                    void *data);

typedef void (*ms_drm_abort_proc)(void *data);

/**
 * A tracked handler for an event that will hopefully be generated by
 * the kernel, and what to do when it is encountered.
 */
struct ms_drm_queue {
    struct xorg_list list;
    xf86CrtcPtr crtc;
    uint32_t seq;
    void *data;
    ScrnInfoPtr scrn;
    ms_drm_handler_proc handler;
    ms_drm_abort_proc abort;
};

typedef struct _modesettingRec {
    int fd;
    Bool fd_passed;

    int Chipset;
    EntityInfoPtr pEnt;

    Bool noAccel;
    CloseScreenProcPtr CloseScreen;
    CreateWindowProcPtr CreateWindow;
    unsigned int SaveGeneration;

    CreateScreenResourcesProcPtr createScreenResources;
    ScreenBlockHandlerProcPtr BlockHandler;
    miPointerSpriteFuncPtr SpriteFuncs;
    void *driver;

    drmmode_rec drmmode;

    drmEventContext event_context;

    /**
     * Page flipping stuff.
     *  @{
     */
    Bool atomic_modeset;
    Bool pending_modeset;
    /** @} */

    DamagePtr damage;
    Bool dirty_enabled;

    uint32_t cursor_width, cursor_height;

    Bool has_queue_sequence;
    Bool tried_queue_sequence;

    Bool kms_has_modifiers;

    /* VRR support */
    Bool vrr_support;
    WindowPtr flip_window;

    Bool is_connector_vrr_capable;
    uint32_t connector_prop_id;

    /* shadow API */
    struct {
        Bool (*Setup)(ScreenPtr);
        Bool (*Add)(ScreenPtr, PixmapPtr, ShadowUpdateProc, ShadowWindowProc,
                    int, void *);
        void (*Remove)(ScreenPtr, PixmapPtr);
        void (*Update32to24)(ScreenPtr, shadowBufPtr);
        void (*UpdatePacked)(ScreenPtr, shadowBufPtr);
    } shadow;

#ifdef GLAMOR_HAS_GBM
    /* glamor API */
    struct {
        Bool (*back_pixmap_from_fd)(PixmapPtr, int, CARD16, CARD16, CARD16,
                                    CARD8, CARD8);
        void (*block_handler)(ScreenPtr);
        void (*clear_pixmap)(PixmapPtr);
        Bool (*egl_create_textured_pixmap)(PixmapPtr, int, int);
        Bool (*egl_create_textured_pixmap_from_gbm_bo)(PixmapPtr,
                                                       struct gbm_bo *,
                                                       Bool);
        void (*egl_exchange_buffers)(PixmapPtr, PixmapPtr);
        struct gbm_device *(*egl_get_gbm_device)(ScreenPtr);
        Bool (*egl_init)(ScrnInfoPtr, int);
        void (*finish)(ScreenPtr);
        struct gbm_bo *(*gbm_bo_from_pixmap)(ScreenPtr, PixmapPtr);
        Bool (*init)(ScreenPtr, unsigned int);
        int (*name_from_pixmap)(PixmapPtr, CARD16 *, CARD32 *);
        void (*set_drawable_modifiers_func)(ScreenPtr,
                                            GetDrawableModifiersFuncPtr);
        int (*shareable_fd_from_pixmap)(ScreenPtr, PixmapPtr, CARD16 *,
                                        CARD32 *);
        Bool (*supports_pixmap_import_export)(ScreenPtr);
        XF86VideoAdaptorPtr (*xv_init)(ScreenPtr, int);
        const char *(*egl_get_driver_name)(ScreenPtr);
    } glamor;
#endif
} modesettingRec, *modesettingPtr;

#define glamor_finish(screen) ms->glamor.finish(screen)

#define modesettingPTR(p) ((modesettingPtr)((p)->driverPrivate))
modesettingEntPtr ms_ent_priv(ScrnInfoPtr scrn);

uint32_t ms_drm_queue_alloc(xf86CrtcPtr crtc,
                            void *data,
                            ms_drm_handler_proc handler,
                            ms_drm_abort_proc abort);

typedef enum ms_queue_flag {
    MS_QUEUE_ABSOLUTE = 0,
    MS_QUEUE_RELATIVE = 1,
    MS_QUEUE_NEXT_ON_MISS = 2
} ms_queue_flag;

Bool ms_queue_vblank(xf86CrtcPtr crtc, ms_queue_flag flags,
                     uint64_t msc, uint64_t *msc_queued, uint32_t seq);

void ms_drm_abort(ScrnInfoPtr scrn,
                  Bool (*match)(void *data, void *match_data),
                  void *match_data);
void ms_drm_abort_seq(ScrnInfoPtr scrn, uint32_t seq);

Bool xf86_crtc_on(xf86CrtcPtr crtc);

xf86CrtcPtr ms_dri2_crtc_covering_drawable(DrawablePtr pDraw);
RRCrtcPtr   ms_randr_crtc_covering_drawable(DrawablePtr pDraw);

int ms_get_crtc_ust_msc(xf86CrtcPtr crtc, CARD64 *ust, CARD64 *msc);

uint64_t ms_kernel_msc_to_crtc_msc(xf86CrtcPtr crtc, uint64_t sequence, Bool is64bit);


Bool ms_dri2_screen_init(ScreenPtr screen);
void ms_dri2_close_screen(ScreenPtr screen);

Bool ms_vblank_screen_init(ScreenPtr screen);
void ms_vblank_close_screen(ScreenPtr screen);

Bool ms_present_screen_init(ScreenPtr screen);

#ifdef GLAMOR_HAS_GBM

typedef void (*ms_pageflip_handler_proc)(modesettingPtr ms,
                                         uint64_t frame,
                                         uint64_t usec,
                                         void *data);

typedef void (*ms_pageflip_abort_proc)(modesettingPtr ms, void *data);

Bool ms_do_pageflip(ScreenPtr screen,
                    PixmapPtr new_front,
                    void *event,
                    int ref_crtc_vblank_pipe,
                    Bool async,
                    ms_pageflip_handler_proc pageflip_handler,
                    ms_pageflip_abort_proc pageflip_abort,
                    const char *log_prefix);

#endif

int ms_flush_drm_events(ScreenPtr screen);
Bool ms_window_has_variable_refresh(modesettingPtr ms, WindowPtr win);
void ms_present_set_screen_vrr(ScrnInfoPtr scrn, Bool vrr_enabled);