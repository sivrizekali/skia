/*
 * Copyright 2015 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "GrBatchTest.h"
#include "GrColor.h"
#include "GrDrawContext.h"
#include "GrDrawContextPriv.h"
#include "GrDrawingManager.h"
#include "GrOvalRenderer.h"
#include "GrPathRenderer.h"
#include "GrRenderTarget.h"
#include "GrRenderTargetPriv.h"
#include "GrResourceProvider.h"
#include "SkSurfacePriv.h"

#include "batches/GrBatch.h"
#include "batches/GrClearBatch.h"
#include "batches/GrDrawAtlasBatch.h"
#include "batches/GrDrawVerticesBatch.h"
#include "batches/GrRectBatchFactory.h"
#include "batches/GrNinePatch.h" // TODO Factory

#include "effects/GrRRectEffect.h"

#include "instanced/InstancedRendering.h"

#include "text/GrAtlasTextContext.h"
#include "text/GrStencilAndCoverTextContext.h"

#include "../private/GrAuditTrail.h"

#define ASSERT_OWNED_RESOURCE(R) SkASSERT(!(R) || (R)->getContext() == fDrawingManager->getContext())
#define ASSERT_SINGLE_OWNER \
    SkDEBUGCODE(GrSingleOwner::AutoEnforce debug_SingleOwner(fSingleOwner);)
#define ASSERT_SINGLE_OWNER_PRIV \
    SkDEBUGCODE(GrSingleOwner::AutoEnforce debug_SingleOwner(fDrawContext->fSingleOwner);)
#define RETURN_IF_ABANDONED        if (fDrawingManager->wasAbandoned()) { return; }
#define RETURN_IF_ABANDONED_PRIV   if (fDrawContext->fDrawingManager->wasAbandoned()) { return; }
#define RETURN_FALSE_IF_ABANDONED  if (fDrawingManager->wasAbandoned()) { return false; }
#define RETURN_FALSE_IF_ABANDONED_PRIV  if (fDrawContext->fDrawingManager->wasAbandoned()) { return false; }
#define RETURN_NULL_IF_ABANDONED   if (fDrawingManager->wasAbandoned()) { return nullptr; }

using gr_instanced::InstancedRendering;

class AutoCheckFlush {
public:
    AutoCheckFlush(GrDrawingManager* drawingManager) : fDrawingManager(drawingManager) {
        SkASSERT(fDrawingManager);
    }
    ~AutoCheckFlush() { fDrawingManager->getContext()->flushIfNecessary(); }

private:
    GrDrawingManager* fDrawingManager;
};

bool GrDrawContext::wasAbandoned() const {
    return fDrawingManager->wasAbandoned();
}

// In MDB mode the reffing of the 'getLastDrawTarget' call's result allows in-progress
// drawTargets to be picked up and added to by drawContexts lower in the call
// stack. When this occurs with a closed drawTarget, a new one will be allocated
// when the drawContext attempts to use it (via getDrawTarget).
GrDrawContext::GrDrawContext(GrContext* context,
                             GrDrawingManager* drawingMgr,
                             sk_sp<GrRenderTarget> rt,
                             const SkSurfaceProps* surfaceProps,
                             GrAuditTrail* auditTrail,
                             GrSingleOwner* singleOwner)
    : fDrawingManager(drawingMgr)
    , fRenderTarget(std::move(rt))
    , fDrawTarget(SkSafeRef(fRenderTarget->getLastDrawTarget()))
    , fContext(context)
    , fInstancedPipelineInfo(fRenderTarget.get())
    , fSurfaceProps(SkSurfacePropsCopyOrDefault(surfaceProps))
    , fAuditTrail(auditTrail)
#ifdef SK_DEBUG
    , fSingleOwner(singleOwner)
#endif
{
    SkDEBUGCODE(this->validate();)
}

#ifdef SK_DEBUG
void GrDrawContext::validate() const {
    SkASSERT(fRenderTarget);
    ASSERT_OWNED_RESOURCE(fRenderTarget);

    if (fDrawTarget && !fDrawTarget->isClosed()) {
        SkASSERT(fRenderTarget->getLastDrawTarget() == fDrawTarget);
    }
}
#endif

GrDrawContext::~GrDrawContext() {
    ASSERT_SINGLE_OWNER
    SkSafeUnref(fDrawTarget);
}

GrDrawTarget* GrDrawContext::getDrawTarget() {
    ASSERT_SINGLE_OWNER
    SkDEBUGCODE(this->validate();)

    if (!fDrawTarget || fDrawTarget->isClosed()) {
        fDrawTarget = fDrawingManager->newDrawTarget(fRenderTarget.get());
    }

    return fDrawTarget;
}

bool GrDrawContext::copySurface(GrSurface* src, const SkIRect& srcRect, const SkIPoint& dstPoint) {
    ASSERT_SINGLE_OWNER
    RETURN_FALSE_IF_ABANDONED
    SkDEBUGCODE(this->validate();)
    GR_AUDIT_TRAIL_AUTO_FRAME(fAuditTrail, "GrDrawContext::copySurface");

    return this->getDrawTarget()->copySurface(fRenderTarget.get(), src, srcRect, dstPoint);
}

void GrDrawContext::drawText(const GrClip& clip, const GrPaint& grPaint,
                             const SkPaint& skPaint,
                             const SkMatrix& viewMatrix,
                             const char text[], size_t byteLength,
                             SkScalar x, SkScalar y, const SkIRect& clipBounds) {
    ASSERT_SINGLE_OWNER
    RETURN_IF_ABANDONED
    SkDEBUGCODE(this->validate();)
    GR_AUDIT_TRAIL_AUTO_FRAME(fAuditTrail, "GrDrawContext::drawText");

    if (!fAtlasTextContext) {
        fAtlasTextContext.reset(GrAtlasTextContext::Create());
    }

    fAtlasTextContext->drawText(fContext, this, clip, grPaint, skPaint, viewMatrix, fSurfaceProps,
                                text, byteLength, x, y, clipBounds);
}

void GrDrawContext::drawPosText(const GrClip& clip, const GrPaint& grPaint,
                                const SkPaint& skPaint,
                                const SkMatrix& viewMatrix,
                                const char text[], size_t byteLength,
                                const SkScalar pos[], int scalarsPerPosition,
                                const SkPoint& offset, const SkIRect& clipBounds) {
    ASSERT_SINGLE_OWNER
    RETURN_IF_ABANDONED
    SkDEBUGCODE(this->validate();)
    GR_AUDIT_TRAIL_AUTO_FRAME(fAuditTrail, "GrDrawContext::drawPosText");

    if (!fAtlasTextContext) {
        fAtlasTextContext.reset(GrAtlasTextContext::Create());
    }

    fAtlasTextContext->drawPosText(fContext, this, clip, grPaint, skPaint, viewMatrix,
                                   fSurfaceProps, text, byteLength, pos, scalarsPerPosition,
                                   offset, clipBounds);

}

void GrDrawContext::drawTextBlob(const GrClip& clip, const SkPaint& skPaint,
                                 const SkMatrix& viewMatrix, const SkTextBlob* blob,
                                 SkScalar x, SkScalar y,
                                 SkDrawFilter* filter, const SkIRect& clipBounds) {
    ASSERT_SINGLE_OWNER
    RETURN_IF_ABANDONED
    SkDEBUGCODE(this->validate();)
    GR_AUDIT_TRAIL_AUTO_FRAME(fAuditTrail, "GrDrawContext::drawTextBlob");

    if (!fAtlasTextContext) {
        fAtlasTextContext.reset(GrAtlasTextContext::Create());
    }

    fAtlasTextContext->drawTextBlob(fContext, this, clip, skPaint, viewMatrix, fSurfaceProps, blob,
                                    x, y, filter, clipBounds);
}

void GrDrawContext::discard() {
    ASSERT_SINGLE_OWNER
    RETURN_IF_ABANDONED
    SkDEBUGCODE(this->validate();)
    GR_AUDIT_TRAIL_AUTO_FRAME(fAuditTrail, "GrDrawContext::discard");

    AutoCheckFlush acf(fDrawingManager);
    this->getDrawTarget()->discard(fRenderTarget.get());
}

void GrDrawContext::clear(const SkIRect* rect,
                          const GrColor color,
                          bool canIgnoreRect) {
    ASSERT_SINGLE_OWNER
    RETURN_IF_ABANDONED
    SkDEBUGCODE(this->validate();)
    GR_AUDIT_TRAIL_AUTO_FRAME(fAuditTrail, "GrDrawContext::clear");

    AutoCheckFlush acf(fDrawingManager);

    const SkIRect rtRect = SkIRect::MakeWH(this->width(), this->height());
    SkIRect clippedRect;
    if (!rect ||
        (canIgnoreRect && fContext->caps()->fullClearIsFree()) ||
        rect->contains(rtRect)) {
        rect = &rtRect;
    } else {
        clippedRect = *rect;
        if (!clippedRect.intersect(rtRect)) {
            return;
        }
        rect = &clippedRect;
    }

    if (fContext->caps()->useDrawInsteadOfClear()) {
        // This works around a driver bug with clear by drawing a rect instead.
        // The driver will ignore a clear if it is the only thing rendered to a
        // target before the target is read.
        if (rect == &rtRect) {
            this->discard();
        }

        GrPaint paint;
        paint.setColor4f(GrColor4f::FromGrColor(color));
        paint.setXPFactory(GrPorterDuffXPFactory::Make(SkXfermode::kSrc_Mode));

        this->drawRect(GrNoClip(), paint, SkMatrix::I(), SkRect::Make(*rect));
    } else {
        sk_sp<GrBatch> batch = GrClearBatch::Make(*rect, color, this->accessRenderTarget());
        this->getDrawTarget()->addBatch(std::move(batch));
    }
}


void GrDrawContext::drawPaint(const GrClip& clip,
                              const GrPaint& origPaint,
                              const SkMatrix& viewMatrix) {
    ASSERT_SINGLE_OWNER
    RETURN_IF_ABANDONED
    SkDEBUGCODE(this->validate();)
    GR_AUDIT_TRAIL_AUTO_FRAME(fAuditTrail, "GrDrawContext::drawPaint");

    // set rect to be big enough to fill the space, but not super-huge, so we
    // don't overflow fixed-point implementations
    SkRect r;
    r.setLTRB(0, 0,
              SkIntToScalar(fRenderTarget->width()),
              SkIntToScalar(fRenderTarget->height()));
    SkTCopyOnFirstWrite<GrPaint> paint(origPaint);

    // by definition this fills the entire clip, no need for AA
    if (paint->isAntiAlias()) {
        paint.writable()->setAntiAlias(false);
    }

    bool isPerspective = viewMatrix.hasPerspective();

    // We attempt to map r by the inverse matrix and draw that. mapRect will
    // map the four corners and bound them with a new rect. This will not
    // produce a correct result for some perspective matrices.
    if (!isPerspective) {
        SkMatrix inverse;
        if (!viewMatrix.invert(&inverse)) {
            SkDebugf("Could not invert matrix\n");
            return;
        }
        inverse.mapRect(&r);
        this->drawRect(clip, *paint, viewMatrix, r);
    } else {
        SkMatrix localMatrix;
        if (!viewMatrix.invert(&localMatrix)) {
            SkDebugf("Could not invert matrix\n");
            return;
        }

        AutoCheckFlush acf(fDrawingManager);

        this->drawNonAAFilledRect(clip, *paint, SkMatrix::I(), r, nullptr, &localMatrix, nullptr);
    }
}

static inline bool rect_contains_inclusive(const SkRect& rect, const SkPoint& point) {
    return point.fX >= rect.fLeft && point.fX <= rect.fRight &&
           point.fY >= rect.fTop && point.fY <= rect.fBottom;
}

static bool view_matrix_ok_for_aa_fill_rect(const SkMatrix& viewMatrix) {
    return viewMatrix.preservesRightAngles();
}

static bool should_apply_coverage_aa(const GrPaint& paint, GrRenderTarget* rt,
                                     bool* useHWAA = nullptr) {
    if (!paint.isAntiAlias()) {
        if (useHWAA) {
            *useHWAA = false;
        }
        return false;
    } else {
        if (useHWAA) {
            *useHWAA = rt->isUnifiedMultisampled();
        }
        return !rt->isUnifiedMultisampled();
    }
}

// Attempts to crop a rect and optional local rect to the clip boundaries.
// Returns false if the draw can be skipped entirely.
static bool crop_filled_rect(const GrRenderTarget* rt, const GrClip& clip,
                             const SkMatrix& viewMatrix, SkRect* rect,
                             SkRect* localRect = nullptr) {
    if (!viewMatrix.rectStaysRect()) {
        return true;
    }

    SkMatrix inverseViewMatrix;
    if (!viewMatrix.invert(&inverseViewMatrix)) {
        return false;
    }

    SkIRect clipDevBounds;
    SkRect clipBounds;
    SkASSERT(inverseViewMatrix.rectStaysRect());

    clip.getConservativeBounds(rt->width(), rt->height(), &clipDevBounds);
    inverseViewMatrix.mapRect(&clipBounds, SkRect::Make(clipDevBounds));

    if (localRect) {
        if (!rect->intersects(clipBounds)) {
            return false;
        }
        const SkScalar dx = localRect->width() / rect->width();
        const SkScalar dy = localRect->height() / rect->height();
        if (clipBounds.fLeft > rect->fLeft) {
            localRect->fLeft += (clipBounds.fLeft - rect->fLeft) * dx;
            rect->fLeft = clipBounds.fLeft;
        }
        if (clipBounds.fTop > rect->fTop) {
            localRect->fTop += (clipBounds.fTop - rect->fTop) * dy;
            rect->fTop = clipBounds.fTop;
        }
        if (clipBounds.fRight < rect->fRight) {
            localRect->fRight -= (rect->fRight - clipBounds.fRight) * dx;
            rect->fRight = clipBounds.fRight;
        }
        if (clipBounds.fBottom < rect->fBottom) {
            localRect->fBottom -= (rect->fBottom - clipBounds.fBottom) * dy;
            rect->fBottom = clipBounds.fBottom;
        }
        return true;
    }

    return rect->intersect(clipBounds);
}

bool GrDrawContext::drawFilledRect(const GrClip& clip,
                                   const GrPaint& paint,
                                   const SkMatrix& viewMatrix,
                                   const SkRect& rect,
                                   const GrUserStencilSettings* ss) {
    SkRect croppedRect = rect;
    if (!crop_filled_rect(fRenderTarget.get(), clip, viewMatrix, &croppedRect)) {
        return true;
    }

    SkAutoTUnref<GrDrawBatch> batch;
    bool useHWAA;

    if (InstancedRendering* ir = this->getDrawTarget()->instancedRendering()) {
        batch.reset(ir->recordRect(croppedRect, viewMatrix, paint.getColor(),
                                   paint.isAntiAlias(), fInstancedPipelineInfo,
                                   &useHWAA));
        if (batch) {
            GrPipelineBuilder pipelineBuilder(paint, useHWAA);
            if (ss) {
                pipelineBuilder.setUserStencil(ss);
            }
            this->getDrawTarget()->drawBatch(pipelineBuilder, this, clip, batch);
            return true;
        }
    }

    if (should_apply_coverage_aa(paint, fRenderTarget.get(), &useHWAA)) {
        // The fill path can handle rotation but not skew.
        if (view_matrix_ok_for_aa_fill_rect(viewMatrix)) {
            SkRect devBoundRect;
            viewMatrix.mapRect(&devBoundRect, croppedRect);

            batch.reset(GrRectBatchFactory::CreateAAFill(paint.getColor(), viewMatrix,
                                                         croppedRect, devBoundRect));
            if (batch) {
                GrPipelineBuilder pipelineBuilder(paint, useHWAA);
                if (ss) {
                    pipelineBuilder.setUserStencil(ss);
                }
                this->getDrawTarget()->drawBatch(pipelineBuilder, this, clip, batch);
                return true;
            }
        }
    } else {
        this->drawNonAAFilledRect(clip, paint, viewMatrix, croppedRect, nullptr, nullptr, ss);
        return true;
    }

    return false;
}

void GrDrawContext::drawRect(const GrClip& clip,
                             const GrPaint& paint,
                             const SkMatrix& viewMatrix,
                             const SkRect& rect,
                             const GrStyle* style) {
    if (!style) {
        style = &GrStyle::SimpleFill();
    }
    ASSERT_SINGLE_OWNER
    RETURN_IF_ABANDONED
    SkDEBUGCODE(this->validate();)
    GR_AUDIT_TRAIL_AUTO_FRAME(fAuditTrail, "GrDrawContext::drawRect");

    // Path effects should've been devolved to a path in SkGpuDevice
    SkASSERT(!style->pathEffect());

    AutoCheckFlush acf(fDrawingManager);

    const SkStrokeRec& stroke = style->strokeRec();
    if (stroke.getStyle() == SkStrokeRec::kFill_Style) {
        
        if (!fContext->caps()->useDrawInsteadOfClear()) {
            // Check if this is a full RT draw and can be replaced with a clear. We don't bother
            // checking cases where the RT is fully inside a stroke.
            SkRect rtRect;
            fRenderTarget->getBoundsRect(&rtRect);
            // Does the clip contain the entire RT?
            if (clip.quickContains(rtRect)) {
                SkMatrix invM;
                if (!viewMatrix.invert(&invM)) {
                    return;
                }
                // Does the rect bound the RT?
                SkPoint srcSpaceRTQuad[4];
                invM.mapRectToQuad(srcSpaceRTQuad, rtRect);
                if (rect_contains_inclusive(rect, srcSpaceRTQuad[0]) &&
                    rect_contains_inclusive(rect, srcSpaceRTQuad[1]) &&
                    rect_contains_inclusive(rect, srcSpaceRTQuad[2]) &&
                    rect_contains_inclusive(rect, srcSpaceRTQuad[3])) {
                    // Will it blend?
                    GrColor clearColor;
                    if (paint.isConstantBlendedColor(&clearColor)) {
                        this->clear(nullptr, clearColor, true);
                        return;
                    }
                }
            }
        }

        if (this->drawFilledRect(clip, paint, viewMatrix, rect, nullptr)) {
            return;
        }
    } else if (stroke.getStyle() == SkStrokeRec::kStroke_Style ||
               stroke.getStyle() == SkStrokeRec::kHairline_Style) {
        if ((!rect.width() || !rect.height()) &&
            SkStrokeRec::kHairline_Style != stroke.getStyle()) {
            SkScalar r = stroke.getWidth() / 2;
            // TODO: Move these stroke->fill fallbacks to GrShape?
            switch (stroke.getJoin()) {
                case SkPaint::kMiter_Join:
                    this->drawRect(clip, paint, viewMatrix,
                                   {rect.fLeft - r, rect.fTop - r,
                                    rect.fRight + r, rect.fBottom + r},
                                   &GrStyle::SimpleFill());
                    return;
                case SkPaint::kRound_Join:
                    // Raster draws nothing when both dimensions are empty.
                    if (rect.width() || rect.height()){
                        SkRRect rrect = SkRRect::MakeRectXY(rect.makeOutset(r, r), r, r);
                        this->drawRRect(clip, paint, viewMatrix, rrect, GrStyle::SimpleFill());
                        return;
                    }
                case SkPaint::kBevel_Join:
                    if (!rect.width()) {
                        this->drawRect(clip, paint, viewMatrix,
                                       {rect.fLeft - r, rect.fTop, rect.fRight + r, rect.fBottom},
                                       &GrStyle::SimpleFill());
                    } else {
                        this->drawRect(clip, paint, viewMatrix,
                                       {rect.fLeft, rect.fTop - r, rect.fRight, rect.fBottom + r},
                                       &GrStyle::SimpleFill());
                    }
                    return;
                }
        }

        bool useHWAA;
        bool snapToPixelCenters = false;
        SkAutoTUnref<GrDrawBatch> batch;

        GrColor color = paint.getColor();
        if (should_apply_coverage_aa(paint, fRenderTarget.get(), &useHWAA)) {
            // The stroke path needs the rect to remain axis aligned (no rotation or skew).
            if (viewMatrix.rectStaysRect()) {
                batch.reset(GrRectBatchFactory::CreateAAStroke(color, viewMatrix, rect, stroke));
            }
        } else {
            // Depending on sub-pixel coordinates and the particular GPU, we may lose a corner of
            // hairline rects. We jam all the vertices to pixel centers to avoid this, but not
            // when MSAA is enabled because it can cause ugly artifacts.
            snapToPixelCenters = stroke.getStyle() == SkStrokeRec::kHairline_Style &&
                                 !fRenderTarget->isUnifiedMultisampled();
            batch.reset(GrRectBatchFactory::CreateNonAAStroke(color, viewMatrix, rect,
                                                              stroke, snapToPixelCenters));
        }

        if (batch) {
            GrPipelineBuilder pipelineBuilder(paint, useHWAA);

            if (snapToPixelCenters) {
                pipelineBuilder.setState(GrPipelineBuilder::kSnapVerticesToPixelCenters_Flag,
                                         snapToPixelCenters);
            }

            this->getDrawTarget()->drawBatch(pipelineBuilder, this, clip, batch);
            return;
        }
    }

    SkPath path;
    path.setIsVolatile(true);
    path.addRect(rect);
    this->internalDrawPath(clip, paint, viewMatrix, path, *style);
}

void GrDrawContextPriv::clearStencilClip(const SkIRect& rect, bool insideClip) {
    ASSERT_SINGLE_OWNER_PRIV
    RETURN_IF_ABANDONED_PRIV
    SkDEBUGCODE(fDrawContext->validate();)
    GR_AUDIT_TRAIL_AUTO_FRAME(fDrawContext->fAuditTrail, "GrDrawContextPriv::clearStencilClip");

    AutoCheckFlush acf(fDrawContext->fDrawingManager);
    fDrawContext->getDrawTarget()->clearStencilClip(rect, insideClip,
                                                    fDrawContext->accessRenderTarget());
}

void GrDrawContextPriv::stencilPath(const GrClip& clip, 
                                    const GrUserStencilSettings* ss,
                                    bool useHWAA,
                                    const SkMatrix& viewMatrix,
                                    const GrPath* path) {
    fDrawContext->getDrawTarget()->stencilPath(fDrawContext, clip, ss, useHWAA, viewMatrix, path);
}

void GrDrawContextPriv::stencilRect(const GrFixedClip& clip,
                                    const GrUserStencilSettings* ss,
                                    bool useHWAA,
                                    const SkMatrix& viewMatrix,
                                    const SkRect& rect) {
    ASSERT_SINGLE_OWNER_PRIV
    RETURN_IF_ABANDONED_PRIV
    SkDEBUGCODE(fDrawContext->validate();)
    GR_AUDIT_TRAIL_AUTO_FRAME(fDrawContext->fAuditTrail, "GrDrawContext::stencilRect");

    AutoCheckFlush acf(fDrawContext->fDrawingManager);

    GrPaint paint;
    paint.setAntiAlias(useHWAA);
    paint.setXPFactory(GrDisableColorXPFactory::Make());

    SkASSERT(!useHWAA || fDrawContext->isStencilBufferMultisampled());

    fDrawContext->drawFilledRect(clip, paint, viewMatrix, rect, ss);
}

bool GrDrawContextPriv::drawAndStencilRect(const GrFixedClip& clip,
                                           const GrUserStencilSettings* ss,
                                           SkRegion::Op op,
                                           bool invert,
                                           bool doAA,
                                           const SkMatrix& viewMatrix,
                                           const SkRect& rect) {
    ASSERT_SINGLE_OWNER_PRIV
    RETURN_FALSE_IF_ABANDONED_PRIV
    SkDEBUGCODE(fDrawContext->validate();)
    GR_AUDIT_TRAIL_AUTO_FRAME(fDrawContext->fAuditTrail, "GrDrawContext::drawAndStencilRect");

    AutoCheckFlush acf(fDrawContext->fDrawingManager);

    GrPaint paint;
    paint.setAntiAlias(doAA);
    paint.setCoverageSetOpXPFactory(op, invert);

    if (fDrawContext->drawFilledRect(clip, paint, viewMatrix, rect, ss)) {
        return true;
    }

    SkPath path;
    path.setIsVolatile(true);
    path.addRect(rect);
    return this->drawAndStencilPath(clip, ss, op, invert, doAA, viewMatrix, path);
}

void GrDrawContext::fillRectToRect(const GrClip& clip,
                                   const GrPaint& paint,
                                   const SkMatrix& viewMatrix,
                                   const SkRect& rectToDraw,
                                   const SkRect& localRect) {
    ASSERT_SINGLE_OWNER
    RETURN_IF_ABANDONED
    SkDEBUGCODE(this->validate();)
    GR_AUDIT_TRAIL_AUTO_FRAME(fAuditTrail, "GrDrawContext::fillRectToRect");

    SkRect croppedRect = rectToDraw;
    SkRect croppedLocalRect = localRect;
    if (!crop_filled_rect(fRenderTarget.get(), clip, viewMatrix, &croppedRect, &croppedLocalRect)) {
        return;
    }

    AutoCheckFlush acf(fDrawingManager);
    SkAutoTUnref<GrDrawBatch> batch;
    bool useHWAA;

    if (InstancedRendering* ir = this->getDrawTarget()->instancedRendering()) {
        batch.reset(ir->recordRect(croppedRect, viewMatrix, paint.getColor(), croppedLocalRect,
                                   paint.isAntiAlias(), fInstancedPipelineInfo, &useHWAA));
        if (batch) {
            GrPipelineBuilder pipelineBuilder(paint, useHWAA);
            this->getDrawTarget()->drawBatch(pipelineBuilder, this, clip, batch);
            return;
        }
    }

    if (should_apply_coverage_aa(paint, fRenderTarget.get(), &useHWAA) &&
        view_matrix_ok_for_aa_fill_rect(viewMatrix)) {
        batch.reset(GrAAFillRectBatch::CreateWithLocalRect(paint.getColor(), viewMatrix,
                                                           croppedRect, croppedLocalRect));
        if (batch) {
            GrPipelineBuilder pipelineBuilder(paint, useHWAA);
            this->drawBatch(pipelineBuilder, clip, batch);
            return;
        }
    } else {
        this->drawNonAAFilledRect(clip, paint, viewMatrix, croppedRect, &croppedLocalRect,
                                  nullptr, nullptr);
    }

}

void GrDrawContext::fillRectWithLocalMatrix(const GrClip& clip,
                                            const GrPaint& paint,
                                            const SkMatrix& viewMatrix,
                                            const SkRect& rectToDraw,
                                            const SkMatrix& localMatrix) {
    ASSERT_SINGLE_OWNER
    RETURN_IF_ABANDONED
    SkDEBUGCODE(this->validate();)
    GR_AUDIT_TRAIL_AUTO_FRAME(fAuditTrail, "GrDrawContext::fillRectWithLocalMatrix");

    SkRect croppedRect = rectToDraw;
    if (!crop_filled_rect(fRenderTarget.get(), clip, viewMatrix, &croppedRect)) {
        return;
    }

    AutoCheckFlush acf(fDrawingManager);
    SkAutoTUnref<GrDrawBatch> batch;
    bool useHWAA;

    if (InstancedRendering* ir = this->getDrawTarget()->instancedRendering()) {
        batch.reset(ir->recordRect(croppedRect, viewMatrix, paint.getColor(), localMatrix,
                                   paint.isAntiAlias(), fInstancedPipelineInfo, &useHWAA));
        if (batch) {
            GrPipelineBuilder pipelineBuilder(paint, useHWAA);
            this->getDrawTarget()->drawBatch(pipelineBuilder, this, clip, batch);
            return;
        }
    }

    if (should_apply_coverage_aa(paint, fRenderTarget.get(), &useHWAA) &&
        view_matrix_ok_for_aa_fill_rect(viewMatrix)) {
        batch.reset(GrAAFillRectBatch::Create(paint.getColor(), viewMatrix, localMatrix,
                                              croppedRect));
        GrPipelineBuilder pipelineBuilder(paint, useHWAA);
        this->getDrawTarget()->drawBatch(pipelineBuilder, this, clip, batch);
    } else {
        this->drawNonAAFilledRect(clip, paint, viewMatrix, croppedRect, nullptr,
                                  &localMatrix, nullptr);
    }

}

void GrDrawContext::drawVertices(const GrClip& clip,
                                 const GrPaint& paint,
                                 const SkMatrix& viewMatrix,
                                 GrPrimitiveType primitiveType,
                                 int vertexCount,
                                 const SkPoint positions[],
                                 const SkPoint texCoords[],
                                 const GrColor colors[],
                                 const uint16_t indices[],
                                 int indexCount) {
    ASSERT_SINGLE_OWNER
    RETURN_IF_ABANDONED
    SkDEBUGCODE(this->validate();)
    GR_AUDIT_TRAIL_AUTO_FRAME(fAuditTrail, "GrDrawContext::drawVertices");

    AutoCheckFlush acf(fDrawingManager);

    // TODO clients should give us bounds
    SkRect bounds;
    if (!bounds.setBoundsCheck(positions, vertexCount)) {
        SkDebugf("drawVertices call empty bounds\n");
        return;
    }

    viewMatrix.mapRect(&bounds);

    // If we don't have AA then we outset for a half pixel in each direction to account for
    // snapping. We also do this for the "hair" primitive types: lines and points since they have
    // a 1 pixel thickness in device space.
    if (!paint.isAntiAlias() || GrIsPrimTypeLines(primitiveType) ||
        kPoints_GrPrimitiveType == primitiveType) {
        bounds.outset(0.5f, 0.5f);
    }

    SkAutoTUnref<GrDrawBatch> batch(new GrDrawVerticesBatch(paint.getColor(),
                                                            primitiveType, viewMatrix, positions,
                                                            vertexCount, indices, indexCount,
                                                            colors, texCoords, bounds));

    GrPipelineBuilder pipelineBuilder(paint, this->mustUseHWAA(paint));
    this->getDrawTarget()->drawBatch(pipelineBuilder, this, clip, batch);
}

///////////////////////////////////////////////////////////////////////////////

void GrDrawContext::drawAtlas(const GrClip& clip,
                              const GrPaint& paint,
                              const SkMatrix& viewMatrix,
                              int spriteCount,
                              const SkRSXform xform[],
                              const SkRect texRect[],
                              const SkColor colors[]) {
    ASSERT_SINGLE_OWNER
    RETURN_IF_ABANDONED
    SkDEBUGCODE(this->validate();)
    GR_AUDIT_TRAIL_AUTO_FRAME(fAuditTrail, "GrDrawContext::drawAtlas");

    AutoCheckFlush acf(fDrawingManager);

    SkAutoTUnref<GrDrawBatch> batch(new GrDrawAtlasBatch(paint.getColor(), viewMatrix, spriteCount,
                                                         xform, texRect, colors));

    GrPipelineBuilder pipelineBuilder(paint, this->mustUseHWAA(paint));
    this->getDrawTarget()->drawBatch(pipelineBuilder, this, clip, batch);
}

///////////////////////////////////////////////////////////////////////////////

void GrDrawContext::drawRRect(const GrClip& clip,
                              const GrPaint& paint,
                              const SkMatrix& viewMatrix,
                              const SkRRect& rrect,
                              const GrStyle& style) {
    ASSERT_SINGLE_OWNER
    RETURN_IF_ABANDONED
    SkDEBUGCODE(this->validate();)
    GR_AUDIT_TRAIL_AUTO_FRAME(fAuditTrail, "GrDrawContext::drawRRect");

    if (rrect.isEmpty()) {
       return;
    }

    SkASSERT(!style.pathEffect()); // this should've been devolved to a path in SkGpuDevice

    AutoCheckFlush acf(fDrawingManager);
    const SkStrokeRec stroke = style.strokeRec();
    bool useHWAA;

    if (this->getDrawTarget()->instancedRendering() && stroke.isFillStyle()) {
        InstancedRendering* ir = this->getDrawTarget()->instancedRendering();
        SkAutoTUnref<GrDrawBatch> batch(ir->recordRRect(rrect, viewMatrix, paint.getColor(),
                                                        paint.isAntiAlias(), fInstancedPipelineInfo,
                                                        &useHWAA));
        if (batch) {
            GrPipelineBuilder pipelineBuilder(paint, useHWAA);
            this->getDrawTarget()->drawBatch(pipelineBuilder, this, clip, batch);
            return;
        }
    }

    if (should_apply_coverage_aa(paint, fRenderTarget.get(), &useHWAA)) {
        GrShaderCaps* shaderCaps = fContext->caps()->shaderCaps();
        SkAutoTUnref<GrDrawBatch> batch(GrOvalRenderer::CreateRRectBatch(paint.getColor(),
                                                                         viewMatrix,
                                                                         rrect,
                                                                         stroke,
                                                                         shaderCaps));
        if (batch) {
            GrPipelineBuilder pipelineBuilder(paint, useHWAA);
            this->getDrawTarget()->drawBatch(pipelineBuilder, this, clip, batch);
            return;
        }
    }

    SkPath path;
    path.setIsVolatile(true);
    path.addRRect(rrect);
    this->internalDrawPath(clip, paint, viewMatrix, path, style);
}

bool GrDrawContext::drawFilledDRRect(const GrClip& clip,
                                     const GrPaint& paintIn,
                                     const SkMatrix& viewMatrix,
                                     const SkRRect& origOuter,
                                     const SkRRect& origInner) {
    SkASSERT(!origInner.isEmpty());
    SkASSERT(!origOuter.isEmpty());

    if (InstancedRendering* ir = this->getDrawTarget()->instancedRendering()) {
        bool useHWAA;
        SkAutoTUnref<GrDrawBatch> batch(ir->recordDRRect(origOuter, origInner, viewMatrix,
                                                         paintIn.getColor(), paintIn.isAntiAlias(),
                                                         fInstancedPipelineInfo, &useHWAA));
        if (batch) {
            GrPipelineBuilder pipelineBuilder(paintIn, useHWAA);
            this->getDrawTarget()->drawBatch(pipelineBuilder, this, clip, batch);
            return true;
        }
    }

    bool applyAA = paintIn.isAntiAlias() && !fRenderTarget->isUnifiedMultisampled();

    GrPrimitiveEdgeType innerEdgeType = applyAA ? kInverseFillAA_GrProcessorEdgeType :
                                                  kInverseFillBW_GrProcessorEdgeType;
    GrPrimitiveEdgeType outerEdgeType = applyAA ? kFillAA_GrProcessorEdgeType :
                                                  kFillBW_GrProcessorEdgeType;

    SkTCopyOnFirstWrite<SkRRect> inner(origInner), outer(origOuter);
    SkMatrix inverseVM;
    if (!viewMatrix.isIdentity()) {
        if (!origInner.transform(viewMatrix, inner.writable())) {
            return false;
        }
        if (!origOuter.transform(viewMatrix, outer.writable())) {
            return false;
        }
        if (!viewMatrix.invert(&inverseVM)) {
            return false;
        }
    } else {
        inverseVM.reset();
    }

    GrPaint grPaint(paintIn);
    grPaint.setAntiAlias(false);

    // TODO these need to be a geometry processors
    sk_sp<GrFragmentProcessor> innerEffect(GrRRectEffect::Make(innerEdgeType, *inner));
    if (!innerEffect) {
        return false;
    }

    sk_sp<GrFragmentProcessor> outerEffect(GrRRectEffect::Make(outerEdgeType, *outer));
    if (!outerEffect) {
        return false;
    }

    grPaint.addCoverageFragmentProcessor(std::move(innerEffect));
    grPaint.addCoverageFragmentProcessor(std::move(outerEffect));

    SkRect bounds = outer->getBounds();
    if (applyAA) {
        bounds.outset(SK_ScalarHalf, SK_ScalarHalf);
    }

    this->fillRectWithLocalMatrix(clip, grPaint, SkMatrix::I(), bounds, inverseVM);
    return true;
}

void GrDrawContext::drawDRRect(const GrClip& clip,
                               const GrPaint& paint,
                               const SkMatrix& viewMatrix,
                               const SkRRect& outer,
                               const SkRRect& inner) {
    ASSERT_SINGLE_OWNER
    RETURN_IF_ABANDONED
    SkDEBUGCODE(this->validate();)
    GR_AUDIT_TRAIL_AUTO_FRAME(fAuditTrail, "GrDrawContext::drawDRRect");

    SkASSERT(!outer.isEmpty());
    SkASSERT(!inner.isEmpty());

    AutoCheckFlush acf(fDrawingManager);

    if (this->drawFilledDRRect(clip, paint, viewMatrix, outer, inner)) {
        return;
    }

    SkPath path;
    path.setIsVolatile(true);
    path.addRRect(inner);
    path.addRRect(outer);
    path.setFillType(SkPath::kEvenOdd_FillType);

    this->internalDrawPath(clip, paint, viewMatrix, path, GrStyle::SimpleFill());
}

///////////////////////////////////////////////////////////////////////////////

void GrDrawContext::drawOval(const GrClip& clip,
                             const GrPaint& paint,
                             const SkMatrix& viewMatrix,
                             const SkRect& oval,
                             const GrStyle& style) {
    ASSERT_SINGLE_OWNER
    RETURN_IF_ABANDONED
    SkDEBUGCODE(this->validate();)
    GR_AUDIT_TRAIL_AUTO_FRAME(fAuditTrail, "GrDrawContext::drawOval");

    if (oval.isEmpty()) {
       return;
    }

    SkASSERT(!style.pathEffect()); // this should've been devolved to a path in SkGpuDevice

    AutoCheckFlush acf(fDrawingManager);
    const SkStrokeRec& stroke = style.strokeRec();
    bool useHWAA;

    if (this->getDrawTarget()->instancedRendering() && stroke.isFillStyle()) {
        InstancedRendering* ir = this->getDrawTarget()->instancedRendering();
        SkAutoTUnref<GrDrawBatch> batch(ir->recordOval(oval, viewMatrix, paint.getColor(),
                                                       paint.isAntiAlias(), fInstancedPipelineInfo,
                                                       &useHWAA));
        if (batch) {
            GrPipelineBuilder pipelineBuilder(paint, useHWAA);
            this->getDrawTarget()->drawBatch(pipelineBuilder, this, clip, batch);
            return;
        }
    }

    if (should_apply_coverage_aa(paint, fRenderTarget.get(), &useHWAA)) {
        GrShaderCaps* shaderCaps = fContext->caps()->shaderCaps();
        SkAutoTUnref<GrDrawBatch> batch(GrOvalRenderer::CreateOvalBatch(paint.getColor(),
                                                                        viewMatrix,
                                                                        oval,
                                                                        stroke,
                                                                        shaderCaps));
        if (batch) {
            GrPipelineBuilder pipelineBuilder(paint, useHWAA);
            this->getDrawTarget()->drawBatch(pipelineBuilder, this, clip, batch);
            return;
        }
    }

    SkPath path;
    path.setIsVolatile(true);
    path.addOval(oval);
    this->internalDrawPath(clip, paint, viewMatrix, path, style);
}

void GrDrawContext::drawImageNine(const GrClip& clip,
                                  const GrPaint& paint,
                                  const SkMatrix& viewMatrix,
                                  int imageWidth,
                                  int imageHeight,
                                  const SkIRect& center,
                                  const SkRect& dst) {
    ASSERT_SINGLE_OWNER
    RETURN_IF_ABANDONED
    SkDEBUGCODE(this->validate();)
    GR_AUDIT_TRAIL_AUTO_FRAME(fAuditTrail, "GrDrawContext::drawImageNine");

    AutoCheckFlush acf(fDrawingManager);

    SkAutoTUnref<GrDrawBatch> batch(GrNinePatch::CreateNonAA(paint.getColor(), viewMatrix,
                                                             imageWidth, imageHeight,
                                                             center, dst));

    GrPipelineBuilder pipelineBuilder(paint, this->mustUseHWAA(paint));
    this->getDrawTarget()->drawBatch(pipelineBuilder, this, clip, batch);
}


void GrDrawContext::drawNonAAFilledRect(const GrClip& clip,
                                        const GrPaint& paint,
                                        const SkMatrix& viewMatrix,
                                        const SkRect& rect,
                                        const SkRect* localRect,
                                        const SkMatrix* localMatrix,
                                        const GrUserStencilSettings* ss) {
    SkAutoTUnref<GrDrawBatch> batch(
            GrRectBatchFactory::CreateNonAAFill(paint.getColor(), viewMatrix, rect, localRect,
                                                localMatrix));
    GrPipelineBuilder pipelineBuilder(paint, this->mustUseHWAA(paint));
    if (ss) {
        pipelineBuilder.setUserStencil(ss);
    }
    this->getDrawTarget()->drawBatch(pipelineBuilder, this, clip, batch);
}

// Can 'path' be drawn as a pair of filled nested rectangles?
static bool fills_as_nested_rects(const SkMatrix& viewMatrix, const SkPath& path, SkRect rects[2]) {

    if (path.isInverseFillType()) {
        return false;
    }

    // TODO: this restriction could be lifted if we were willing to apply
    // the matrix to all the points individually rather than just to the rect
    if (!viewMatrix.rectStaysRect()) {
        return false;
    }

    SkPath::Direction dirs[2];
    if (!path.isNestedFillRects(rects, dirs)) {
        return false;
    }

    if (SkPath::kWinding_FillType == path.getFillType() && dirs[0] == dirs[1]) {
        // The two rects need to be wound opposite to each other
        return false;
    }

    // Right now, nested rects where the margin is not the same width
    // all around do not render correctly
    const SkScalar* outer = rects[0].asScalars();
    const SkScalar* inner = rects[1].asScalars();

    bool allEq = true;

    SkScalar margin = SkScalarAbs(outer[0] - inner[0]);
    bool allGoE1 = margin >= SK_Scalar1;

    for (int i = 1; i < 4; ++i) {
        SkScalar temp = SkScalarAbs(outer[i] - inner[i]);
        if (temp < SK_Scalar1) {
            allGoE1 = false;
        }
        if (!SkScalarNearlyEqual(margin, temp)) {
            allEq = false;
        }
    }

    return allEq || allGoE1;
}

void GrDrawContext::drawPath(const GrClip& clip,
                             const GrPaint& paint,
                             const SkMatrix& viewMatrix,
                             const SkPath& path,
                             const GrStyle& style) {
    ASSERT_SINGLE_OWNER
    RETURN_IF_ABANDONED
    SkDEBUGCODE(this->validate();)
    GR_AUDIT_TRAIL_AUTO_FRAME(fAuditTrail, "GrDrawContext::drawPath");

    if (path.isEmpty()) {
       if (path.isInverseFillType()) {
           this->drawPaint(clip, paint, viewMatrix);
       }
       return;
    }

    AutoCheckFlush acf(fDrawingManager);

    bool useHWAA;
    if (should_apply_coverage_aa(paint, fRenderTarget.get(), &useHWAA) && !style.pathEffect()) {
        if (style.isSimpleFill() && !path.isConvex()) {
            // Concave AA paths are expensive - try to avoid them for special cases
            SkRect rects[2];

            if (fills_as_nested_rects(viewMatrix, path, rects)) {
                SkAutoTUnref<GrDrawBatch> batch(GrRectBatchFactory::CreateAAFillNestedRects(
                    paint.getColor(), viewMatrix, rects));
                if (batch) {
                    GrPipelineBuilder pipelineBuilder(paint, useHWAA);
                    this->getDrawTarget()->drawBatch(pipelineBuilder, this, clip, batch);
                }
                return;
            }
        }
        SkRect ovalRect;
        bool isOval = path.isOval(&ovalRect);

        if (isOval && !path.isInverseFillType()) {
            GrShaderCaps* shaderCaps = fContext->caps()->shaderCaps();
            SkAutoTUnref<GrDrawBatch> batch(GrOvalRenderer::CreateOvalBatch(paint.getColor(),
                                                                            viewMatrix,
                                                                            ovalRect,
                                                                            style.strokeRec(),
                                                                            shaderCaps));
            if (batch) {
                GrPipelineBuilder pipelineBuilder(paint, useHWAA);
                this->getDrawTarget()->drawBatch(pipelineBuilder, this, clip, batch);
                return;
            }
        }
    }

    // Note that internalDrawPath may sw-rasterize the path into a scratch texture.
    // Scratch textures can be recycled after they are returned to the texture
    // cache. This presents a potential hazard for buffered drawing. However,
    // the writePixels that uploads to the scratch will perform a flush so we're
    // OK.
    this->internalDrawPath(clip, paint, viewMatrix, path, style);
}

bool GrDrawContextPriv::drawAndStencilPath(const GrFixedClip& clip,
                                           const GrUserStencilSettings* ss,
                                           SkRegion::Op op,
                                           bool invert,
                                           bool doAA,
                                           const SkMatrix& viewMatrix,
                                           const SkPath& path) {
    ASSERT_SINGLE_OWNER_PRIV
    RETURN_FALSE_IF_ABANDONED_PRIV
    SkDEBUGCODE(fDrawContext->validate();)
    GR_AUDIT_TRAIL_AUTO_FRAME(fDrawContext->fAuditTrail, "GrDrawContext::drawPath");

    if (path.isEmpty() && path.isInverseFillType()) {
        this->drawAndStencilRect(clip, ss, op, invert, false, SkMatrix::I(),
                                 SkRect::MakeIWH(fDrawContext->width(),
                                                 fDrawContext->height()));
        return true;
    }

    AutoCheckFlush acf(fDrawContext->fDrawingManager);

    // An Assumption here is that path renderer would use some form of tweaking
    // the src color (either the input alpha or in the frag shader) to implement
    // aa. If we have some future driver-mojo path AA that can do the right
    // thing WRT to the blend then we'll need some query on the PR.
    bool useCoverageAA = doAA && !fDrawContext->fRenderTarget->isUnifiedMultisampled();
    bool hasUserStencilSettings = !ss->isUnused();
    bool isStencilBufferMSAA = fDrawContext->fRenderTarget->isStencilBufferMultisampled();

    const GrPathRendererChain::DrawType type =
        useCoverageAA ? GrPathRendererChain::kColorAntiAlias_DrawType
                      : GrPathRendererChain::kColor_DrawType;

    GrShape shape(path, GrStyle::SimpleFill());
    GrPathRenderer::CanDrawPathArgs canDrawArgs;
    canDrawArgs.fShaderCaps = fDrawContext->fDrawingManager->getContext()->caps()->shaderCaps();
    canDrawArgs.fViewMatrix = &viewMatrix;
    canDrawArgs.fShape = &shape;
    canDrawArgs.fAntiAlias = useCoverageAA;
    canDrawArgs.fHasUserStencilSettings = hasUserStencilSettings;
    canDrawArgs.fIsStencilBufferMSAA = isStencilBufferMSAA;

    // Don't allow the SW renderer
    GrPathRenderer* pr = fDrawContext->fDrawingManager->getPathRenderer(canDrawArgs, false, type);
    if (!pr) {
        return false;
    }

    GrPaint paint;
    paint.setCoverageSetOpXPFactory(op, invert);

    GrPathRenderer::DrawPathArgs args;
    args.fResourceProvider = fDrawContext->fDrawingManager->getContext()->resourceProvider();
    args.fPaint = &paint;
    args.fUserStencilSettings = ss;
    args.fDrawContext = fDrawContext;
    args.fClip = &clip;
    args.fViewMatrix = &viewMatrix;
    args.fShape = &shape;
    args.fAntiAlias = useCoverageAA;
    args.fGammaCorrect = fDrawContext->isGammaCorrect();
    pr->drawPath(args);
    return true;
}

void GrDrawContext::internalDrawPath(const GrClip& clip,
                                     const GrPaint& paint,
                                     const SkMatrix& viewMatrix,
                                     const SkPath& path,
                                     const GrStyle& style) {
    ASSERT_SINGLE_OWNER
    RETURN_IF_ABANDONED
    SkASSERT(!path.isEmpty());

    bool useCoverageAA = should_apply_coverage_aa(paint, fRenderTarget.get());
    constexpr bool kHasUserStencilSettings = false;
    bool isStencilBufferMSAA = fRenderTarget->isStencilBufferMultisampled();

    const GrPathRendererChain::DrawType type =
        useCoverageAA ? GrPathRendererChain::kColorAntiAlias_DrawType
                      : GrPathRendererChain::kColor_DrawType;

    GrShape shape(path, style);
    if (shape.isEmpty()) {
        return;
    }
    GrPathRenderer::CanDrawPathArgs canDrawArgs;
    canDrawArgs.fShaderCaps = fDrawingManager->getContext()->caps()->shaderCaps();
    canDrawArgs.fViewMatrix = &viewMatrix;
    canDrawArgs.fShape = &shape;
    canDrawArgs.fAntiAlias = useCoverageAA;
    canDrawArgs.fHasUserStencilSettings = kHasUserStencilSettings;
    canDrawArgs.fIsStencilBufferMSAA = isStencilBufferMSAA;

    // Try a 1st time without applying any of the style to the geometry (and barring sw)
    GrPathRenderer* pr = fDrawingManager->getPathRenderer(canDrawArgs, false, type);
    SkScalar styleScale =  GrStyle::MatrixToScaleFactor(viewMatrix);

    if (!pr && shape.style().pathEffect()) {
        // It didn't work above, so try again with the path effect applied.
        shape = shape.applyStyle(GrStyle::Apply::kPathEffectOnly, styleScale);
        if (shape.isEmpty()) {
            return;
        }
        pr = fDrawingManager->getPathRenderer(canDrawArgs, false, type);
    }
    if (!pr) {
        if (shape.style().applies()) {
            shape = shape.applyStyle(GrStyle::Apply::kPathEffectAndStrokeRec, styleScale);
            if (shape.isEmpty()) {
                return;
            }
        }
        // This time, allow SW renderer
        pr = fDrawingManager->getPathRenderer(canDrawArgs, true, type);
    }

    if (!pr) {
#ifdef SK_DEBUG
        SkDebugf("Unable to find path renderer compatible with path.\n");
#endif
        return;
    }

    GrPathRenderer::DrawPathArgs args;
    args.fResourceProvider = fDrawingManager->getContext()->resourceProvider();
    args.fPaint = &paint;
    args.fUserStencilSettings = &GrUserStencilSettings::kUnused;
    args.fDrawContext = this;
    args.fClip = &clip;
    args.fViewMatrix = &viewMatrix;
    args.fShape = canDrawArgs.fShape;
    args.fAntiAlias = useCoverageAA;
    args.fGammaCorrect = this->isGammaCorrect();
    pr->drawPath(args);
}

void GrDrawContext::drawBatch(const GrPipelineBuilder& pipelineBuilder, const GrClip& clip,
                              GrDrawBatch* batch) {
    ASSERT_SINGLE_OWNER
    RETURN_IF_ABANDONED
    SkDEBUGCODE(this->validate();)
    GR_AUDIT_TRAIL_AUTO_FRAME(fAuditTrail, "GrDrawContext::drawBatch");

    this->getDrawTarget()->drawBatch(pipelineBuilder, this, clip, batch);
}
