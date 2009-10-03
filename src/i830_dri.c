/**************************************************************************

Copyright 2001 VA Linux Systems Inc., Fremont, California.
Copyright © 2002 by David Dawes

All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
on the rights to use, copy, modify, merge, publish, distribute, sub
license, and/or sell copies of the Software, and to permit persons to whom
the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice (including the next
paragraph) shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
ATI, VA LINUX SYSTEMS AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
USE OR OTHER DEALINGS IN THE SOFTWARE.

**************************************************************************/

/*
 * Authors: Jeff Hartmann <jhartmann@valinux.com>
 *          David Dawes <dawes@xfree86.org>
 *          Keith Whitwell <keith@tungstengraphics.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>

#include "xf86.h"
#include "xf86_OSproc.h"
#include "xf86Priv.h"

#include "xf86PciInfo.h"
#include "xf86Pci.h"

#include "windowstr.h"
#include "shadow.h"

#include "GL/glxtokens.h"

#include "i830.h"
#include "i830_dri.h"

#include "i915_drm.h"

#include "dri2.h"

#ifdef DRI2
#if DRI2INFOREC_VERSION >= 1
#define USE_DRI2_1_1_0
#endif

extern XF86ModuleData dri2ModuleData;
#endif

#ifndef USE_DRI2_1_1_0
static DRI2BufferPtr
I830DRI2CreateBuffers(DrawablePtr pDraw, unsigned int *attachments, int count)
{
    ScreenPtr pScreen = pDraw->pScreen;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    I830Ptr pI830 = I830PTR(pScrn);
    DRI2BufferPtr buffers;
    dri_bo *bo;
    int i;
    I830DRI2BufferPrivatePtr privates;
    PixmapPtr pPixmap, pDepthPixmap;

    buffers = xcalloc(count, sizeof *buffers);
    if (buffers == NULL)
	return NULL;
    privates = xcalloc(count, sizeof *privates);
    if (privates == NULL) {
	xfree(buffers);
	return NULL;
    }

    pDepthPixmap = NULL;
    for (i = 0; i < count; i++) {
	if (attachments[i] == DRI2BufferFrontLeft) {
	    pPixmap = get_drawable_pixmap(pDraw);
	    pPixmap->refcnt++;
	} else if (attachments[i] == DRI2BufferStencil && pDepthPixmap) {
	    pPixmap = pDepthPixmap;
	    pPixmap->refcnt++;
	} else {
	    unsigned int hint = 0;

	    switch (attachments[i]) {
	    case DRI2BufferDepth:
		if (SUPPORTS_YTILING(pI830))
		    hint = INTEL_CREATE_PIXMAP_TILING_Y;
		else
		    hint = INTEL_CREATE_PIXMAP_TILING_X;
		break;
	    case DRI2BufferFakeFrontLeft:
	    case DRI2BufferFakeFrontRight:
	    case DRI2BufferBackLeft:
	    case DRI2BufferBackRight:
		    hint = INTEL_CREATE_PIXMAP_TILING_X;
		break;
	    }

	    if (!pI830->tiling ||
		(!IS_I965G(pI830) && !pI830->kernel_exec_fencing))
		hint = 0;

	    pPixmap = (*pScreen->CreatePixmap)(pScreen,
					       pDraw->width,
					       pDraw->height,
					       pDraw->depth,
					       hint);

	}

	if (attachments[i] == DRI2BufferDepth)
	    pDepthPixmap = pPixmap;

	buffers[i].attachment = attachments[i];
	buffers[i].pitch = pPixmap->devKind;
	buffers[i].cpp = pPixmap->drawable.bitsPerPixel / 8;
	buffers[i].driverPrivate = &privates[i];
	buffers[i].flags = 0; /* not tiled */
	privates[i].pPixmap = pPixmap;
	privates[i].attachment = attachments[i];

	bo = i830_get_pixmap_bo (pPixmap);
	if (dri_bo_flink(bo, &buffers[i].name) != 0) {
	    /* failed to name buffer */
	}

    }

    return buffers;
}

#else

static DRI2Buffer2Ptr
I830DRI2CreateBuffer(DrawablePtr pDraw, unsigned int attachment,
		     unsigned int format)
{
    ScreenPtr pScreen = pDraw->pScreen;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    I830Ptr pI830 = I830PTR(pScrn);
    DRI2Buffer2Ptr buffer;
    dri_bo *bo;
    I830DRI2BufferPrivatePtr privates;
    PixmapPtr pPixmap;

    buffer = xcalloc(1, sizeof *buffer);
    if (buffer == NULL)
	return NULL;
    privates = xcalloc(1, sizeof *privates);
    if (privates == NULL) {
	xfree(buffer);
	return NULL;
    }

    if (attachment == DRI2BufferFrontLeft) {
	pPixmap = get_drawable_pixmap(pDraw);
	pPixmap->refcnt++;
    } else {
	unsigned int hint = 0;

	switch (attachment) {
	case DRI2BufferDepth:
	case DRI2BufferDepthStencil:
	    if (SUPPORTS_YTILING(pI830))
		hint = INTEL_CREATE_PIXMAP_TILING_Y;
	    else
		hint = INTEL_CREATE_PIXMAP_TILING_X;
	    break;
	case DRI2BufferFakeFrontLeft:
	case DRI2BufferFakeFrontRight:
	case DRI2BufferBackLeft:
	case DRI2BufferBackRight:
	    hint = INTEL_CREATE_PIXMAP_TILING_X;
	    break;
	}

	if (!pI830->tiling ||
	    (!IS_I965G(pI830) && !pI830->kernel_exec_fencing))
	    hint = 0;

	pPixmap = (*pScreen->CreatePixmap)(pScreen,
					   pDraw->width,
					   pDraw->height,
					   (format != 0)?format:pDraw->depth,
					   hint);

    }


    buffer->attachment = attachment;
    buffer->pitch = pPixmap->devKind;
    buffer->cpp = pPixmap->drawable.bitsPerPixel / 8;
    buffer->driverPrivate = privates;
    buffer->format = format;
    buffer->flags = 0; /* not tiled */
    privates->pPixmap = pPixmap;
    privates->attachment = attachment;

    bo = i830_get_pixmap_bo (pPixmap);
    if (dri_bo_flink(bo, &buffer->name) != 0) {
	/* failed to name buffer */
    }

    return buffer;
}

#endif

#ifndef USE_DRI2_1_1_0

static void
I830DRI2DestroyBuffers(DrawablePtr pDraw, DRI2BufferPtr buffers, int count)
{
    ScreenPtr pScreen = pDraw->pScreen;
    I830DRI2BufferPrivatePtr private;
    int i;

    for (i = 0; i < count; i++)
    {
	private = buffers[i].driverPrivate;
	(*pScreen->DestroyPixmap)(private->pPixmap);
    }

    if (buffers)
    {
	xfree(buffers[0].driverPrivate);
	xfree(buffers);
    }
}

#else

static void
I830DRI2DestroyBuffer(DrawablePtr pDraw, DRI2Buffer2Ptr buffer)
{
    if (buffer) {
	I830DRI2BufferPrivatePtr private = buffer->driverPrivate;
	ScreenPtr pScreen = pDraw->pScreen;

	(*pScreen->DestroyPixmap)(private->pPixmap);

	xfree(private);
	xfree(buffer);
    }
}

#endif

static int
I830DRI2DrawablePipe(DrawablePtr pDraw)
{
    ScreenPtr pScreen = pDraw->pScreen;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    BoxRec box, crtcbox;
    xf86CrtcPtr crtc;
    int pipe = -1;

    box.x1 = pDraw->x;
    box.y1 = pDraw->y;
    box.x2 = box.x1 + pDraw->width;
    box.y2 = box.y1 + pDraw->height;

    crtc = i830_covering_crtc(pScrn, &box, NULL, &crtcbox);

    /* Make sure the CRTC is valid and this is the real front buffer */
    if (crtc != NULL && !crtc->rotatedData)
	pipe = i830_crtc_to_pipe(crtc);

    return pipe;
}

static void
I830DRI2CopyRegion(DrawablePtr pDraw, RegionPtr pRegion,
		   DRI2BufferPtr pDstBuffer, DRI2BufferPtr pSrcBuffer)
{
    I830DRI2BufferPrivatePtr srcPrivate = pSrcBuffer->driverPrivate;
    I830DRI2BufferPrivatePtr dstPrivate = pDstBuffer->driverPrivate;
    ScreenPtr pScreen = pDraw->pScreen;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    I830Ptr pI830 = I830PTR(pScrn);
    DrawablePtr src = (srcPrivate->attachment == DRI2BufferFrontLeft)
	? pDraw : &srcPrivate->pPixmap->drawable;
    DrawablePtr dst = (dstPrivate->attachment == DRI2BufferFrontLeft)
	? pDraw : &dstPrivate->pPixmap->drawable;
    RegionPtr pCopyClip;
    GCPtr pGC;

    pGC = GetScratchGC(pDraw->depth, pScreen);
    pCopyClip = REGION_CREATE(pScreen, NULL, 0);
    REGION_COPY(pScreen, pCopyClip, pRegion);
    (*pGC->funcs->ChangeClip) (pGC, CT_REGION, pCopyClip, 0);
    ValidateGC(dst, pGC);

    /* Wait for the scanline to be outside the region to be copied */
    if (pixmap_is_scanout(get_drawable_pixmap(dst)) && pI830->swapbuffers_wait) {
	BoxPtr box;
	BoxRec crtcbox;
	int y1, y2;
	int pipe = -1, event, load_scan_lines_pipe;
	xf86CrtcPtr crtc;

	box = REGION_EXTENTS(unused, pGC->pCompositeClip);
	crtc = i830_covering_crtc(pScrn, box, NULL, &crtcbox);

	/* Make sure the CRTC is valid and this is the real front buffer */
	if (crtc != NULL && !crtc->rotatedData) {
	    pipe = i830_crtc_to_pipe(crtc);

	    if (pipe == 0) {
		event = MI_WAIT_FOR_PIPEA_SCAN_LINE_WINDOW;
		load_scan_lines_pipe = MI_LOAD_SCAN_LINES_DISPLAY_PIPEA;
	    } else {
		event = MI_WAIT_FOR_PIPEB_SCAN_LINE_WINDOW;
		load_scan_lines_pipe = MI_LOAD_SCAN_LINES_DISPLAY_PIPEB;
	    }

	    /* Make sure we don't wait for a scanline that will never occur */
	    y1 = (crtcbox.y1 <= box->y1) ? box->y1 - crtcbox.y1 : 0;
	    y2 = (box->y2 <= crtcbox.y2) ?
		box->y2 - crtcbox.y1 : crtcbox.y2 - crtcbox.y1;

	    BEGIN_BATCH(5);
	    /* The documentation says that the LOAD_SCAN_LINES command
	     * always comes in pairs. Don't ask me why. */
	    OUT_BATCH(MI_LOAD_SCAN_LINES_INCL | load_scan_lines_pipe);
	    OUT_BATCH((y1 << 16) | y2);
	    OUT_BATCH(MI_LOAD_SCAN_LINES_INCL | load_scan_lines_pipe);
	    OUT_BATCH((y1 << 16) | y2);
	    OUT_BATCH(MI_WAIT_FOR_EVENT | event);
	    ADVANCE_BATCH();
	}
    }

    (*pGC->ops->CopyArea)(src, dst,
			  pGC, 0, 0, pDraw->width, pDraw->height, 0, 0);
    FreeScratchGC(pGC);

    /* Emit a flush of the rendering cache, or on the 965 and beyond
     * rendering results may not hit the framebuffer until significantly
     * later.
     */
    I830EmitFlush(pScrn);
    pI830->need_mi_flush = FALSE;

    /* We can't rely on getting into the block handler before the DRI
     * client gets to run again so flush now. */
    intel_batch_flush(pScrn, TRUE);
#if ALWAYS_SYNC
    I830Sync(pScrn);
#endif
    drmCommandNone(pI830->drmSubFD, DRM_I915_GEM_THROTTLE);

}

#if DRI2INFOREC_VERSION >= 4

/*
 * Request a DRM event when the requested conditions will be satisfied.
 * Event will be handled by the server and wake up any clients waiting
 * on the specified frame count.
 */
static int
I830DRI2SetupSwap(DrawablePtr pDraw, CARD64 target_msc, CARD64 divisor,
		  CARD64 remainder, void *data, CARD64 *event_frame)
{
    ScreenPtr pScreen = pDraw->pScreen;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    I830Ptr pI830 = I830PTR(pScrn);
    drmVBlank vbl;
    int ret, pipe = I830DRI2DrawablePipe(pDraw);

    /* Get current count */
    vbl.request.type = DRM_VBLANK_RELATIVE;
    if (pipe > 0)
	vbl.request.type |= DRM_VBLANK_SECONDARY;
    vbl.request.sequence = 0;
    ret = drmWaitVBlank(pI830->drmSubFD, &vbl);
    if (ret) {
	xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		   "get vblank counter failed: %s\n", strerror(errno));
	return FALSE;
    }

    /*
     * If divisor is zero, we just need to make sure target_msc passes
     * before waking up the client.
     */
    if (divisor == 0) {
	vbl.request.type = DRM_VBLANK_ABSOLUTE | DRM_VBLANK_EVENT;
	if (pipe > 0)
	    vbl.request.type |= DRM_VBLANK_SECONDARY;
	vbl.request.sequence = target_msc;
	vbl.request.signal = (unsigned long)data;
	ret = drmWaitVBlank(pI830->drmSubFD, &vbl);
	if (ret) {
	    xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		       "get vblank counter failed: %s\n", strerror(errno));
	    return FALSE;
	}
	return TRUE;
    }

    /*
     * If we get here, target_msc has already passed or we don't have one,
     * so we queue an event that will satisfy the divisor/remainder equation.
     */
    if ((vbl.reply.sequence % divisor) == remainder)
	return FALSE;

    vbl.request.type = DRM_VBLANK_ABSOLUTE | DRM_VBLANK_EVENT;
    if (pipe > 0)
	vbl.request.type |= DRM_VBLANK_SECONDARY;

    /*
     * If we have no remainder, and the test above failed, it means we've
     * passed the last point where seq % divisor == remainder, so we need
     * to wait for the next time that will happen.
     */
    if (!remainder)
	vbl.request.sequence += divisor;

    vbl.request.sequence = vbl.reply.sequence - (vbl.reply.sequence % divisor) +
	remainder;
    vbl.request.signal = (unsigned long)data;
    ret = drmWaitVBlank(pI830->drmSubFD, &vbl);
    if (ret) {
	xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		   "get vblank counter failed: %s\n", strerror(errno));
	return FALSE;
    }

    *event_frame = vbl.reply.sequence;

    return TRUE;
}

/*
 * DRI2SwapBuffers should try to do a buffer swap if possible, however:
 *   - if we're swapping buffers smaller than the screen, we have to blit
 *   - if the back buffer doesn't match the screen depth, we have to blit
 *   - otherwise we try to swap, and return to the caller the new front
 *     and back buffers
 */
static Bool
I830DRI2SwapBuffers(ScreenPtr pScreen,
		    DRI2BufferPtr front, DRI2BufferPtr back, void *data)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    I830Ptr pI830 = I830PTR(pScrn);
    I830DRI2BufferPrivatePtr front_priv, back_priv;
    dri_bo *tmp_bo;
    int tmp;

    front_priv = front->driverPrivate;
    back_priv = back->driverPrivate;

    if (pI830->shadow_present)
	return FALSE;

    /* Swap BO names so DRI works */
    tmp = front->name;
    front->name = back->name;
    back->name = tmp;

    /* Swap pixmap bos */
    dri_bo_reference(i830_get_pixmap_bo(front_priv->pPixmap));

    tmp_bo = i830_get_pixmap_bo(front_priv->pPixmap);
    i830_set_pixmap_bo(front_priv->pPixmap,
		       i830_get_pixmap_bo(back_priv->pPixmap));
    i830_set_pixmap_bo(back_priv->pPixmap, tmp_bo); /* should be screen */

    if (front_priv->pPixmap != pScreen->GetScreenPixmap(pScreen))
	FatalError("swapbuffers with bad front\n");

    /* Page flip the full screen buffer */
    return drmmode_do_pageflip(pScreen, i830_get_pixmap_bo(front_priv->pPixmap),
			       i830_get_pixmap_bo(back_priv->pPixmap), data);
}

/*
 * Get current frame count and frame count timestamp, based on drawable's
 * crtc.
 */
static int
I830DRI2GetMSC(DrawablePtr pDraw, CARD64 *ust, CARD64 *msc)
{
    ScreenPtr pScreen = pDraw->pScreen;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    I830Ptr pI830 = I830PTR(pScrn);
    drmVBlank vbl;
    int ret, pipe = I830DRI2DrawablePipe(pDraw);

    /* FIXME: we shouldn't be called if the window isn't mapped or is
       redirected */
    if (pipe < 0) {
	*ust = 0;
	*msc = 0;
	return TRUE;
    }

    vbl.request.type = DRM_VBLANK_RELATIVE;
    if (pipe > 0)
	vbl.request.type |= DRM_VBLANK_SECONDARY;
    vbl.request.sequence = 0;

    ret = drmWaitVBlank(pI830->drmSubFD, &vbl);
    if (ret) {
	xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		   "get vblank counter failed: %s\n", strerror(errno));
	return FALSE;
    }

    *ust = ((CARD64)vbl.reply.tval_sec * 1000000) + vbl.reply.tval_usec;
    *msc = vbl.reply.sequence;

    return TRUE;
}

/*
 * Request a DRM event when the requested conditions will be satisfied.
 * Event will be handled by the server and wake up any clients waiting
 * on the specified frame count.
 */
static int
I830DRI2SetupWaitMSC(DrawablePtr pDraw, CARD64 target_msc, CARD64 divisor,
		     CARD64 remainder, void *data, CARD64 *event_frame)
{
    ScreenPtr pScreen = pDraw->pScreen;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    I830Ptr pI830 = I830PTR(pScrn);
    drmVBlank vbl;
    int ret, pipe = I830DRI2DrawablePipe(pDraw);

    /* Get current count */
    vbl.request.type = DRM_VBLANK_RELATIVE;
    if (pipe > 0)
	vbl.request.type |= DRM_VBLANK_SECONDARY;
    vbl.request.sequence = 0;
    ret = drmWaitVBlank(pI830->drmSubFD, &vbl);
    if (ret) {
	xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		   "get vblank counter failed: %s\n", strerror(errno));
	return FALSE;
    }

    /*
     * If divisor is zero, we just need to make sure target_msc passes
     * before waking up the client.
     */
    if (divisor == 0) {
	vbl.request.type = DRM_VBLANK_ABSOLUTE | DRM_VBLANK_EVENT;
	if (pipe > 0)
	    vbl.request.type |= DRM_VBLANK_SECONDARY;
	vbl.request.sequence = target_msc;
	vbl.request.signal = (unsigned long)data;
	ret = drmWaitVBlank(pI830->drmSubFD, &vbl);
	if (ret) {
	    xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		       "get vblank counter failed: %s\n", strerror(errno));
	    return FALSE;
	}
	return TRUE;
    }

    /*
     * If we get here, target_msc has already passed or we don't have one,
     * so we queue an event that will satisfy the divisor/remainder equation.
     */
    vbl.request.type = DRM_VBLANK_ABSOLUTE | DRM_VBLANK_EVENT;
    if (pipe > 0)
	vbl.request.type |= DRM_VBLANK_SECONDARY;

    /*
     * If we have no remainder and the condition isn't satisified, it means
     * we've passed the last point where seq % divisor == remainder, so we need
     * to wait for the next time that will happen.
     */
    if (((vbl.reply.sequence % divisor) != remainder) && !remainder)
	vbl.request.sequence += divisor;

    vbl.request.sequence = vbl.reply.sequence - (vbl.reply.sequence % divisor) +
	remainder;
    vbl.request.signal = (unsigned long)data;
    ret = drmWaitVBlank(pI830->drmSubFD, &vbl);
    if (ret) {
	xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		   "get vblank counter failed: %s\n", strerror(errno));
	return FALSE;
    }

    *event_frame = vbl.reply.sequence;

    return TRUE;
}
#endif

Bool I830DRI2ScreenInit(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    I830Ptr pI830 = I830PTR(pScrn);
    DRI2InfoRec info;
    char *p;
    int i;
    struct stat sbuf;
    dev_t d;
#ifdef USE_DRI2_1_1_0
    int dri2_major = 1;
    int dri2_minor = 0;
#endif

#ifdef USE_DRI2_1_1_0
    if (xf86LoaderCheckSymbol("DRI2Version")) {
	DRI2Version(& dri2_major, & dri2_minor);
    }

    if (dri2_minor < 1) {
	xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		   "DRI2 requires DRI2 module version 1.1.0 or later\n");
	return FALSE;
    }
#endif

    info.fd = pI830->drmSubFD;

    /* The whole drmOpen thing is a fiasco and we need to find a way
     * back to just using open(2).  For now, however, lets just make
     * things worse with even more ad hoc directory walking code to
     * discover the device file name. */

    fstat(info.fd, &sbuf);
    d = sbuf.st_rdev;

    p = pI830->deviceName;
    for (i = 0; i < DRM_MAX_MINOR; i++) {
	sprintf(p, DRM_DEV_NAME, DRM_DIR_NAME, i);
	if (stat(p, &sbuf) == 0 && sbuf.st_rdev == d)
	    break;
    }
    if (i == DRM_MAX_MINOR) {
	xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		   "DRI2: failed to open drm device\n");
	return FALSE;
    }

    info.driverName = IS_I965G(pI830) ? "i965" : "i915";
    info.deviceName = p;

#if DRI2INFOREC_VERSION >= 3
    info.version = 3;
    info.CreateBuffer = I830DRI2CreateBuffer;
    info.DestroyBuffer = I830DRI2DestroyBuffer;
#else
# ifdef USE_DRI2_1_1_0
    info.version = 2;
    info.CreateBuffers = NULL;
    info.DestroyBuffers = NULL;
    info.CreateBuffer = I830DRI2CreateBuffer;
    info.DestroyBuffer = I830DRI2DestroyBuffer;
# else
    info.version = 1;
    info.CreateBuffers = I830DRI2CreateBuffers;
    info.DestroyBuffers = I830DRI2DestroyBuffers;
# endif
#endif

#if DRI2INFOREC_VERSION >= 4
    info.version = 4;
    info.SetupSwap = I830DRI2SetupSwap;
    if (pI830->use_swap_buffers) {
	info.SwapBuffers = I830DRI2SwapBuffers;
    } else {
	info.SwapBuffers = NULL;
    }

    info.GetMSC = I830DRI2GetMSC;
    info.SetupWaitMSC = I830DRI2SetupWaitMSC;
#endif

    info.CopyRegion = I830DRI2CopyRegion;

    return DRI2ScreenInit(pScreen, &info);
}

void I830DRI2CloseScreen(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    I830Ptr pI830 = I830PTR(pScrn);

    DRI2CloseScreen(pScreen);
    pI830->directRenderingType = DRI_NONE;
}
