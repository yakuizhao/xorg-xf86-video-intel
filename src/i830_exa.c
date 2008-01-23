/**************************************************************************

Copyright 1998-1999 Precision Insight, Inc., Cedar Park, Texas.
All Rights Reserved.
Copyright (c) 2005 Jesse Barnes <jbarnes@virtuousgeek.org>
  Based on code from i830_xaa.c.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sub license, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice (including the
next paragraph) shall be included in all copies or substantial portions
of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

**************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "xf86.h"
#include "xaarop.h"
#include "i830.h"
#include "i810_reg.h"
#include "intel_bufmgr_ttm.h"
#include <string.h>

#ifdef I830DEBUG
#define DEBUG_I830FALLBACK 1
#endif

#define ALWAYS_SYNC		0

#ifdef DEBUG_I830FALLBACK
#define I830FALLBACK(s, arg...)				\
do {							\
	DPRINTF(PFX, "EXA fallback: " s "\n", ##arg); 	\
	return FALSE;					\
} while(0)
#else
#define I830FALLBACK(s, arg...) 			\
do { 							\
	return FALSE;					\
} while(0)
#endif

const int I830CopyROP[16] =
{
   ROP_0,               /* GXclear */
   ROP_DSa,             /* GXand */
   ROP_SDna,            /* GXandReverse */
   ROP_S,               /* GXcopy */
   ROP_DSna,            /* GXandInverted */
   ROP_D,               /* GXnoop */
   ROP_DSx,             /* GXxor */
   ROP_DSo,             /* GXor */
   ROP_DSon,            /* GXnor */
   ROP_DSxn,            /* GXequiv */
   ROP_Dn,              /* GXinvert*/
   ROP_SDno,            /* GXorReverse */
   ROP_Sn,              /* GXcopyInverted */
   ROP_DSno,            /* GXorInverted */
   ROP_DSan,            /* GXnand */
   ROP_1                /* GXset */
};

const int I830PatternROP[16] =
{
    ROP_0,
    ROP_DPa,
    ROP_PDna,
    ROP_P,
    ROP_DPna,
    ROP_D,
    ROP_DPx,
    ROP_DPo,
    ROP_DPon,
    ROP_PDxn,
    ROP_Dn,
    ROP_PDno,
    ROP_Pn,
    ROP_DPno,
    ROP_DPan,
    ROP_1
};

/**
 * Returns whether a given pixmap is tiled or not.
 *
 * Currently, we only have one pixmap that might be tiled, which is the front
 * buffer.  At the point where we are tiling some pixmaps managed by the
 * general allocator, we should move this to using pixmap privates.
 */
Bool
i830_pixmap_tiled(PixmapPtr pPixmap)
{
    ScreenPtr pScreen = pPixmap->drawable.pScreen;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    I830Ptr pI830 = I830PTR(pScrn);
    unsigned long offset;

    if (pI830->use_ttm_batch) {
	struct i830_exa_pixmap_priv *driver_priv;
	driver_priv = exaGetPixmapDriverPrivate(pPixmap);
	
	if (driver_priv) {
	    if ((pI830->front_buffer->tiling != TILE_NONE) && (driver_priv->flags & I830_EXA_PIXMAP_IS_FRONTBUFFER))
		return TRUE;
	    else
		return FALSE;
	}
    }
      
    offset = intel_get_pixmap_offset(pPixmap);
    if (offset == pI830->front_buffer->offset &&
	pI830->front_buffer->tiling != TILE_NONE)
    {
	return TRUE;
    }

    return FALSE;
}

static Bool
i830_exa_pixmap_is_offscreen(PixmapPtr pPixmap)
{
    ScrnInfoPtr pScrn = xf86Screens[pPixmap->drawable.pScreen->myNum];
    I830Ptr pI830 = I830PTR(pScrn);

    if ((void *)pPixmap->devPrivate.ptr >= (void *)pI830->FbBase &&
	(void *)pPixmap->devPrivate.ptr <
	(void *)(pI830->FbBase + pI830->FbMapSize))
    {
	return TRUE;
    } else {
	return FALSE;
    }
}

/**
 * I830EXASync - wait for a command to finish
 * @pScreen: current screen
 * @marker: marker command to wait for
 *
 * Wait for the command specified by @marker to finish, then return.  We don't
 * actually do marker waits, though we might in the future.  For now, just
 * wait for a full idle.
 */
static void
I830EXASync(ScreenPtr pScreen, int marker)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];

    I830Sync(pScrn);
}

/**
 * I830EXAPrepareSolid - prepare for a Solid operation, if possible
 */
static Bool
I830EXAPrepareSolid(PixmapPtr pPixmap, int alu, Pixel planemask, Pixel fg)
{
    ScrnInfoPtr pScrn = xf86Screens[pPixmap->drawable.pScreen->myNum];
    I830Ptr pI830 = I830PTR(pScrn);
    unsigned long offset, pitch;

    if (!EXA_PM_IS_SOLID(&pPixmap->drawable, planemask))
	I830FALLBACK("planemask is not solid");

    if (pPixmap->drawable.bitsPerPixel == 24)
	I830FALLBACK("solid 24bpp unsupported!\n");

    offset = exaGetPixmapOffset(pPixmap);
    pitch = exaGetPixmapPitch(pPixmap);

    if (offset % pI830->EXADriverPtr->pixmapOffsetAlign != 0)
	I830FALLBACK("pixmap offset not aligned");
    if (pitch % pI830->EXADriverPtr->pixmapPitchAlign != 0)
	I830FALLBACK("pixmap pitch not aligned");

    pI830->BR[13] = (I830PatternROP[alu] & 0xff) << 16 ;
    switch (pPixmap->drawable.bitsPerPixel) {
	case 8:
	    break;
	case 16:
	    /* RGB565 */
	    pI830->BR[13] |= (1 << 24);
	    break;
	case 32:
	    /* RGB8888 */
	    pI830->BR[13] |= ((1 << 24) | (1 << 25));
	    break;
    }
    pI830->BR[16] = fg;
    return TRUE;
}

static void
I830EXASolid(PixmapPtr pPixmap, int x1, int y1, int x2, int y2)
{
    ScrnInfoPtr pScrn = xf86Screens[pPixmap->drawable.pScreen->myNum];
    I830Ptr pI830 = I830PTR(pScrn);
    unsigned long pitch;
    uint32_t cmd;

    pitch = exaGetPixmapPitch(pPixmap);

    {
	BEGIN_BATCH(6);

	cmd = XY_COLOR_BLT_CMD;

	if (pPixmap->drawable.bitsPerPixel == 32)
	    cmd |= XY_COLOR_BLT_WRITE_ALPHA | XY_COLOR_BLT_WRITE_RGB;

	if (IS_I965G(pI830) && i830_pixmap_tiled(pPixmap)) {
	    assert((pitch % 512) == 0);
	    pitch >>= 2;
	    cmd |= XY_COLOR_BLT_TILED;
	}

	OUT_BATCH(cmd);
	OUT_BATCH(pI830->BR[13] | pitch);
	OUT_BATCH((y1 << 16) | (x1 & 0xffff));
	OUT_BATCH((y2 << 16) | (x2 & 0xffff));
	OUT_PIXMAP_RELOC(pPixmap,
			 DRM_BO_FLAG_MEM_TT | DRM_BO_FLAG_WRITE,
			 0);
	OUT_BATCH(pI830->BR[16]);
	ADVANCE_BATCH();
    }
}

static void
I830EXADoneSolid(PixmapPtr pPixmap)
{
#if ALWAYS_SYNC
    ScrnInfoPtr pScrn = xf86Screens[pPixmap->drawable.pScreen->myNum];

    I830Sync(pScrn);
#endif
}

/**
 * TODO:
 *   - support planemask using FULL_BLT_CMD?
 */
static Bool
I830EXAPrepareCopy(PixmapPtr pSrcPixmap, PixmapPtr pDstPixmap, int xdir,
		   int ydir, int alu, Pixel planemask)
{
    ScrnInfoPtr pScrn = xf86Screens[pDstPixmap->drawable.pScreen->myNum];
    I830Ptr pI830 = I830PTR(pScrn);

    if (!EXA_PM_IS_SOLID(&pSrcPixmap->drawable, planemask))
	I830FALLBACK("planemask is not solid");

    pI830->pSrcPixmap = pSrcPixmap;

    pI830->BR[13] = I830CopyROP[alu] << 16;

    switch (pSrcPixmap->drawable.bitsPerPixel) {
    case 8:
	break;
    case 16:
	pI830->BR[13] |= (1 << 24);
	break;
    case 32:
	pI830->BR[13] |= ((1 << 25) | (1 << 24));
	break;
    }
    return TRUE;
}

static void
I830EXACopy(PixmapPtr pDstPixmap, int src_x1, int src_y1, int dst_x1,
	    int dst_y1, int w, int h)
{
    ScrnInfoPtr pScrn = xf86Screens[pDstPixmap->drawable.pScreen->myNum];
    I830Ptr pI830 = I830PTR(pScrn);
    uint32_t cmd;
    int dst_x2, dst_y2;
    unsigned int dst_pitch, src_pitch;

    dst_x2 = dst_x1 + w;
    dst_y2 = dst_y1 + h;

    dst_pitch = exaGetPixmapPitch(pDstPixmap);
    src_pitch = exaGetPixmapPitch(pI830->pSrcPixmap);

    {
	BEGIN_BATCH(8);

	cmd = XY_SRC_COPY_BLT_CMD;

	if (pDstPixmap->drawable.bitsPerPixel == 32)
	    cmd |= XY_SRC_COPY_BLT_WRITE_ALPHA | XY_SRC_COPY_BLT_WRITE_RGB;

	if (IS_I965G(pI830)) {
	    if (i830_pixmap_tiled(pDstPixmap)) {
		assert((dst_pitch % 512) == 0);
		dst_pitch >>= 2;
		cmd |= XY_SRC_COPY_BLT_DST_TILED;
	    }

	    if (i830_pixmap_tiled(pI830->pSrcPixmap)) {
		assert((src_pitch % 512) == 0);
		src_pitch >>= 2;
		cmd |= XY_SRC_COPY_BLT_SRC_TILED;
	    }
	}

	OUT_BATCH(cmd);

	OUT_BATCH(pI830->BR[13] | dst_pitch);
	OUT_BATCH((dst_y1 << 16) | (dst_x1 & 0xffff));
	OUT_BATCH((dst_y2 << 16) | (dst_x2 & 0xffff));
	OUT_PIXMAP_RELOC(pDstPixmap,
			 DRM_BO_FLAG_MEM_TT | DRM_BO_FLAG_WRITE,
			 0);
	OUT_BATCH((src_y1 << 16) | (src_x1 & 0xffff));
	OUT_BATCH(src_pitch);
	OUT_PIXMAP_RELOC(pI830->pSrcPixmap,
			 DRM_BO_FLAG_MEM_TT | DRM_BO_FLAG_READ,
			 0);

	ADVANCE_BATCH();
    }
}

static void
I830EXADoneCopy(PixmapPtr pDstPixmap)
{
#if ALWAYS_SYNC
    ScrnInfoPtr pScrn = xf86Screens[pDstPixmap->drawable.pScreen->myNum];

    I830Sync(pScrn);
#endif
}

#define xFixedToFloat(val) \
	((float)xFixedToInt(val) + ((float)xFixedFrac(val) / 65536.0))

/**
 * Returns the floating-point coordinates transformed by the given transform.
 *
 * transform may be null.
 */
void
i830_get_transformed_coordinates(int x, int y, PictTransformPtr transform,
				 float *x_out, float *y_out)
{
    if (transform == NULL) {
	*x_out = x;
	*y_out = y;
    } else {
	PictVector v;

        v.vector[0] = IntToxFixed(x);
        v.vector[1] = IntToxFixed(y);
        v.vector[2] = xFixed1;
        PictureTransformPoint(transform, &v);
	*x_out = xFixedToFloat(v.vector[0]);
	*y_out = xFixedToFloat(v.vector[1]);
    }
}

#ifdef MM_DEBUG
#define mmDebug ErrorF
#else
#define mmDebug(x, ...) /* */
#endif

static void *I830EXACreatePixmap(ScreenPtr pScreen, int size, int align)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    I830Ptr pI830 = I830PTR(pScrn);
    struct i830_exa_pixmap_priv *new_priv;

    new_priv = xcalloc(1, sizeof(struct i830_exa_pixmap_priv));
    if (!new_priv)
        return NULL;

    if (size == 0)
	return new_priv;

    new_priv->bo = dri_bo_alloc(pI830->bufmgr, "pixmap",
				size, 4096, DRM_BO_FLAG_MEM_LOCAL | DRM_BO_FLAG_CACHED | DRM_BO_FLAG_CACHED_MAPPED);

    return new_priv;
}

static void I830EXADestroyPixmap(ScreenPtr pScreen, void *driverPriv)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    I830Ptr pI830 = I830PTR(pScrn);
    struct i830_exa_pixmap_priv *driver_priv = driverPriv;

    if (driver_priv->flags & I830_EXA_PIXMAP_IS_MAPPED)
        dri_bo_unmap(driver_priv->bo);

    dri_bo_unreference(driver_priv->bo);
    xfree(driverPriv);
}

static Bool I830EXAPixmapIsOffscreen(PixmapPtr pPix)
{
    struct i830_exa_pixmap_priv *driver_priv;

    driver_priv = exaGetPixmapDriverPrivate(pPix);

    if (!driver_priv)
       return FALSE;
    if (driver_priv->bo)
       return TRUE;
    return FALSE;
}

static Bool I830EXAPrepareAccess(PixmapPtr pPix, int index)
{
    ScreenPtr pScreen = pPix->drawable.pScreen;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    I830Ptr pI830 = I830PTR(pScrn);
    struct i830_exa_pixmap_priv *driver_priv;
    int ret;

    driver_priv = exaGetPixmapDriverPrivate(pPix);

    if (!driver_priv)
	return FALSE;

    /* TODO : make this more conditional */
    intelddx_batchbuffer_flush(pI830->batch);
    dri_fence_wait(pI830->batch->last_fence);

    if (driver_priv->bo) {
	mmDebug("mapping %p %d %dx%d\n", pPix, driver_priv->flags, pPix->drawable.width, pPix->drawable.height);

	if ((driver_priv->flags & I830_EXA_PIXMAP_IS_MAPPED))
	    return TRUE;

	ret = dri_bo_map(driver_priv->bo, 1);
	if (ret)
	    return FALSE;

	driver_priv->flags |= I830_EXA_PIXMAP_IS_MAPPED;
	pPix->devPrivate.ptr = driver_priv->bo->virtual;
    }

    return TRUE;
}

static Bool I830EXAModifyPixmapHeader(PixmapPtr pPixmap, int width, int height,
				      int depth, int bitsPerPixel, int devKind,
				      pointer pPixData)
{
    ScreenPtr	pScreen = pPixmap->drawable.pScreen;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    I830Ptr pI830 = I830PTR(pScrn);
    struct i830_exa_pixmap_priv *driver_priv;

    driver_priv = exaGetPixmapDriverPrivate(pPixmap);

    if (pPixData == pI830->FbBase + pScrn->fbOffset) {
	/* this is the front buffer pixmap so set it up as such..*/
        driver_priv->flags |= I830_EXA_PIXMAP_IS_FRONTBUFFER;

	/* get a reference to the front buffer handle */
	driver_priv->bo =
	    intel_ttm_bo_create_from_handle(pI830->bufmgr, "front",
					    pI830->front_buffer->bo.handle);
	miModifyPixmapHeader(pPixmap, width, height, depth,
			     bitsPerPixel, devKind, NULL);


	return TRUE;
    }
    return FALSE;
}


/*
 * TODO:
 *   - Dual head?
 */
Bool
I830EXAInit(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    I830Ptr pI830 = I830PTR(pScrn);

    pI830->EXADriverPtr = exaDriverAlloc();
    if (pI830->EXADriverPtr == NULL) {
	pI830->noAccel = TRUE;
	return FALSE;
    }
    memset(pI830->EXADriverPtr, 0, sizeof(*pI830->EXADriverPtr));

    pI830->bufferOffset = 0;
    pI830->EXADriverPtr->exa_major = 2;
    /* If compiled against EXA 2.2, require 2.2 so we can use the
     * PixmapIsOffscreen hook.
     */
#if EXA_VERSION_MINOR >= 2
    pI830->EXADriverPtr->exa_minor = 2;
#else
    pI830->EXADriverPtr->exa_minor = 1;
    xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
	       "EXA compatibility mode.  Output rotation rendering "
	       "performance may suffer\n");
#endif

    if (!pI830->use_ttm_batch) {
	pI830->EXADriverPtr->memoryBase = pI830->FbBase;
	if (pI830->exa_offscreen) {
	    pI830->EXADriverPtr->offScreenBase = pI830->exa_offscreen->offset;
	    pI830->EXADriverPtr->memorySize = pI830->exa_offscreen->offset +
		pI830->exa_offscreen->size;
	} else {
	    pI830->EXADriverPtr->offScreenBase = pI830->FbMapSize;
	    pI830->EXADriverPtr->memorySize = pI830->FbMapSize;
	}
	pI830->EXADriverPtr->flags = EXA_OFFSCREEN_PIXMAPS;
    } else {
	pI830->EXADriverPtr->flags = EXA_OFFSCREEN_PIXMAPS | EXA_HANDLES_PIXMAPS;
	pI830->EXADriverPtr->PrepareAccess = I830EXAPrepareAccess;
    }

    DPRINTF(PFX, "EXA Mem: memoryBase 0x%x, end 0x%x, offscreen base 0x%x, "
	    "memorySize 0x%x\n",
	    pI830->EXADriverPtr->memoryBase,
	    pI830->EXADriverPtr->memoryBase + pI830->EXADriverPtr->memorySize,
	    pI830->EXADriverPtr->offScreenBase,
	    pI830->EXADriverPtr->memorySize);


    /* Limits are described in the BLT engine chapter under Graphics Data Size
     * Limitations, and the descriptions of SURFACE_STATE, 3DSTATE_BUFFER_INFO,
     * 3DSTATE_DRAWING_RECTANGLE, 3DSTATE_MAP_INFO, and 3DSTATE_MAP_INFO.
     *
     * i845 through i965 limits 2D rendering to 65536 lines and pitch of 32768.
     *
     * i965 limits 3D surface to (2*element size)-aligned offset if un-tiled.
     * i965 limits 3D surface to 4kB-aligned offset if tiled.
     * i965 limits 3D surfaces to w,h of ?,8192.
     * i965 limits 3D surface to pitch of 1B - 128kB.
     * i965 limits 3D surface pitch alignment to 1 or 2 times the element size.
     * i965 limits 3D surface pitch alignment to 512B if tiled.
     * i965 limits 3D destination drawing rect to w,h of 8192,8192.
     *
     * i915 limits 3D textures to 4B-aligned offset if un-tiled.
     * i915 limits 3D textures to ~4kB-aligned offset if tiled.
     * i915 limits 3D textures to width,height of 2048,2048.
     * i915 limits 3D textures to pitch of 16B - 8kB, in dwords.
     * i915 limits 3D destination to ~4kB-aligned offset if tiled.
     * i915 limits 3D destination to pitch of 16B - 8kB, in dwords, if un-tiled.
     * i915 limits 3D destination to pitch of 512B - 8kB, in tiles, if tiled.
     * i915 limits 3D destination to POT aligned pitch if tiled.
     * i915 limits 3D destination drawing rect to w,h of 2048,2048.
     *
     * i845 limits 3D textures to 4B-aligned offset if un-tiled.
     * i845 limits 3D textures to ~4kB-aligned offset if tiled.
     * i845 limits 3D textures to width,height of 2048,2048.
     * i845 limits 3D textures to pitch of 4B - 8kB, in dwords.
     * i845 limits 3D destination to 4B-aligned offset if un-tiled.
     * i845 limits 3D destination to ~4kB-aligned offset if tiled.
     * i845 limits 3D destination to pitch of 8B - 8kB, in dwords.
     * i845 limits 3D destination drawing rect to w,h of 2048,2048.
     *
     * For the tiled issues, the only tiled buffer we draw to should be
     * the front, which will have an appropriate pitch/offset already set up,
     * so EXA doesn't need to worry.
     */
    if (IS_I965G(pI830)) {
	pI830->EXADriverPtr->pixmapOffsetAlign = 4 * 2;
	pI830->EXADriverPtr->pixmapPitchAlign = 16;
	pI830->EXADriverPtr->maxX = 8192;
	pI830->EXADriverPtr->maxY = 8192;
    } else {
	pI830->EXADriverPtr->pixmapOffsetAlign = 4;
	pI830->EXADriverPtr->pixmapPitchAlign = 16;
	pI830->EXADriverPtr->maxX = 2048;
	pI830->EXADriverPtr->maxY = 2048;
    }

    /* Sync */
    pI830->EXADriverPtr->WaitMarker = I830EXASync;

    /* Solid fill */
    pI830->EXADriverPtr->PrepareSolid = I830EXAPrepareSolid;
    pI830->EXADriverPtr->Solid = I830EXASolid;
    pI830->EXADriverPtr->DoneSolid = I830EXADoneSolid;

    /* Copy */
    pI830->EXADriverPtr->PrepareCopy = I830EXAPrepareCopy;
    pI830->EXADriverPtr->Copy = I830EXACopy;
    pI830->EXADriverPtr->DoneCopy = I830EXADoneCopy;

    /* Composite */
    if (!IS_I9XX(pI830)) {
    	pI830->EXADriverPtr->CheckComposite = i830_check_composite;
    	pI830->EXADriverPtr->PrepareComposite = i830_prepare_composite;
    	pI830->EXADriverPtr->Composite = i830_composite;
    	pI830->EXADriverPtr->DoneComposite = i830_done_composite;
    } else if (IS_I915G(pI830) || IS_I915GM(pI830) ||
	       IS_I945G(pI830) || IS_I945GM(pI830) || IS_G33CLASS(pI830))
    {
	pI830->EXADriverPtr->CheckComposite = i915_check_composite;
   	pI830->EXADriverPtr->PrepareComposite = i915_prepare_composite;
    	pI830->EXADriverPtr->Composite = i830_composite;
    	pI830->EXADriverPtr->DoneComposite = i830_done_composite;
    } else {
 	pI830->EXADriverPtr->CheckComposite = i965_check_composite;
 	pI830->EXADriverPtr->PrepareComposite = i965_prepare_composite;
 	pI830->EXADriverPtr->Composite = i965_composite;
 	pI830->EXADriverPtr->DoneComposite = i965_done_composite;
    }
#if EXA_VERSION_MINOR >= 4
    if (pI830->use_ttm_batch) {
	pI830->EXADriverPtr->CreatePixmap = I830EXACreatePixmap;
	pI830->EXADriverPtr->DestroyPixmap = I830EXADestroyPixmap;
	pI830->EXADriverPtr->PixmapIsOffscreen = I830EXAPixmapIsOffscreen;
	pI830->EXADriverPtr->ModifyPixmapHeader = I830EXAModifyPixmapHeader;
    } else 
#endif
	{
#if EXA_VERSION_MINOR >= 2
	    pI830->EXADriverPtr->PixmapIsOffscreen = i830_exa_pixmap_is_offscreen;
#endif
	}

    if(!exaDriverInit(pScreen, pI830->EXADriverPtr)) {
	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "EXA initialization failed; trying older version\n");
	pI830->EXADriverPtr->exa_minor = 0;
	if(!exaDriverInit(pScreen, pI830->EXADriverPtr)) {
	    xfree(pI830->EXADriverPtr);
	    pI830->noAccel = TRUE;
	    return FALSE;
	}
    }

    I830SelectBuffer(pScrn, I830_SELECT_FRONT);

    return TRUE;
}

#ifdef XF86DRI

#ifndef ExaOffscreenMarkUsed
extern void ExaOffscreenMarkUsed(PixmapPtr);
#endif

unsigned long long
I830TexOffsetStart(PixmapPtr pPix)
{
    ScreenPtr pScreen = pPix->drawable.pScreen;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    I830Ptr pI830 = I830PTR(pScrn);

    if (pI830->use_ttm_batch) {
	struct i830_exa_pixmap_priv *driver_priv;
	driver_priv = exaGetPixmapDriverPrivate(pPix);

	return driver_priv->bo->offset;
    } else {
        exaMoveInPixmap(pPix);
        ExaOffscreenMarkUsed(pPix);

        return exaGetPixmapOffset(pPix);
    }
}
#endif
