/*
 * Copyright 2012 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkCanvasPriv.h"
#include "SkClipStack.h"
#include "SkDebugCanvas.h"
#include "SkDrawCommand.h"
#include "SkPaintFilterCanvas.h"
#include "SkOverdrawMode.h"

#if SK_SUPPORT_GPU
#include "GrAuditTrail.h"
#include "GrContext.h"
#include "GrRenderTarget.h"
#include "SkGpuDevice.h"
#endif

#define SKDEBUGCANVAS_VERSION                     1
#define SKDEBUGCANVAS_ATTRIBUTE_VERSION           "version"
#define SKDEBUGCANVAS_ATTRIBUTE_COMMANDS          "commands"
#define SKDEBUGCANVAS_ATTRIBUTE_AUDITTRAIL        "auditTrail"

class DebugPaintFilterCanvas : public SkPaintFilterCanvas {
public:
    DebugPaintFilterCanvas(int width,
                           int height,
                           bool overdrawViz,
                           bool overrideFilterQuality,
                           SkFilterQuality quality)
        : INHERITED(width, height)
        , fOverdrawXfermode(overdrawViz ? SkOverdrawMode::Make() : nullptr)
        , fOverrideFilterQuality(overrideFilterQuality)
        , fFilterQuality(quality) {}

protected:
    bool onFilter(SkTCopyOnFirstWrite<SkPaint>* paint, Type) const override {
        if (*paint) {
            if (nullptr != fOverdrawXfermode.get()) {
                paint->writable()->setAntiAlias(false);
                paint->writable()->setXfermode(fOverdrawXfermode);
            }

            if (fOverrideFilterQuality) {
                paint->writable()->setFilterQuality(fFilterQuality);
            }
        }
        return true;
    }

    void onDrawPicture(const SkPicture* picture,
                       const SkMatrix* matrix,
                       const SkPaint* paint) override {
        // We need to replay the picture onto this canvas in order to filter its internal paints.
        this->SkCanvas::onDrawPicture(picture, matrix, paint);
    }

private:
    sk_sp<SkXfermode> fOverdrawXfermode;

    bool fOverrideFilterQuality;
    SkFilterQuality fFilterQuality;

    typedef SkPaintFilterCanvas INHERITED;
};

SkDebugCanvas::SkDebugCanvas(int width, int height)
        : INHERITED(width, height)
        , fPicture(nullptr)
        , fFilter(false)
        , fMegaVizMode(false)
        , fOverdrawViz(false)
        , fOverrideFilterQuality(false)
        , fFilterQuality(kNone_SkFilterQuality)
        , fClipVizColor(SK_ColorTRANSPARENT)
        , fDrawGpuBatchBounds(false) {
    fUserMatrix.reset();

    // SkPicturePlayback uses the base-class' quickReject calls to cull clipped
    // operations. This can lead to problems in the debugger which expects all
    // the operations in the captured skp to appear in the debug canvas. To
    // circumvent this we create a wide open clip here (an empty clip rect
    // is not sufficient).
    // Internally, the SkRect passed to clipRect is converted to an SkIRect and
    // rounded out. The following code creates a nearly maximal rect that will
    // not get collapsed by the coming conversions (Due to precision loss the
    // inset has to be surprisingly large).
    SkIRect largeIRect = SkIRect::MakeLargest();
    largeIRect.inset(1024, 1024);
    SkRect large = SkRect::Make(largeIRect);
#ifdef SK_DEBUG
    SkASSERT(!large.roundOut().isEmpty());
#endif
    // call the base class' version to avoid adding a draw command
    this->INHERITED::onClipRect(large, SkRegion::kReplace_Op, kHard_ClipEdgeStyle);
}

SkDebugCanvas::~SkDebugCanvas() {
    fCommandVector.deleteAll();
}

void SkDebugCanvas::addDrawCommand(SkDrawCommand* command) {
    fCommandVector.push(command);
}

void SkDebugCanvas::draw(SkCanvas* canvas) {
    if (!fCommandVector.isEmpty()) {
        this->drawTo(canvas, fCommandVector.count() - 1);
    }
}

void SkDebugCanvas::applyUserTransform(SkCanvas* canvas) {
    canvas->concat(fUserMatrix);
}

int SkDebugCanvas::getCommandAtPoint(int x, int y, int index) {
    SkBitmap bitmap;
    bitmap.allocPixels(SkImageInfo::MakeN32Premul(1, 1));

    SkCanvas canvas(bitmap);
    canvas.translate(SkIntToScalar(-x), SkIntToScalar(-y));
    this->applyUserTransform(&canvas);

    int layer = 0;
    SkColor prev = bitmap.getColor(0,0);
    for (int i = 0; i < index; i++) {
        if (fCommandVector[i]->isVisible()) {
            fCommandVector[i]->setUserMatrix(fUserMatrix);
            fCommandVector[i]->execute(&canvas);
        }
        if (prev != bitmap.getColor(0,0)) {
            layer = i;
        }
        prev = bitmap.getColor(0,0);
    }
    return layer;
}

class SkDebugClipVisitor : public SkCanvas::ClipVisitor {
public:
    SkDebugClipVisitor(SkCanvas* canvas) : fCanvas(canvas) {}

    void clipRect(const SkRect& r, SkRegion::Op, bool doAA) override {
        SkPaint p;
        p.setColor(SK_ColorRED);
        p.setStyle(SkPaint::kStroke_Style);
        p.setAntiAlias(doAA);
        fCanvas->drawRect(r, p);
    }
    void clipRRect(const SkRRect& rr, SkRegion::Op, bool doAA) override {
        SkPaint p;
        p.setColor(SK_ColorGREEN);
        p.setStyle(SkPaint::kStroke_Style);
        p.setAntiAlias(doAA);
        fCanvas->drawRRect(rr, p);
    }
    void clipPath(const SkPath& path, SkRegion::Op, bool doAA) override {
        SkPaint p;
        p.setColor(SK_ColorBLUE);
        p.setStyle(SkPaint::kStroke_Style);
        p.setAntiAlias(doAA);
        fCanvas->drawPath(path, p);
    }

protected:
    SkCanvas* fCanvas;

private:
    typedef SkCanvas::ClipVisitor INHERITED;
};

// set up the saveLayer commands so that the active ones
// return true in their 'active' method
void SkDebugCanvas::markActiveCommands(int index) {
    fActiveLayers.rewind();

    for (int i = 0; i < fCommandVector.count(); ++i) {
        fCommandVector[i]->setActive(false);
    }

    for (int i = 0; i < index; ++i) {
        SkDrawCommand::Action result = fCommandVector[i]->action();
        if (SkDrawCommand::kPushLayer_Action == result) {
            fActiveLayers.push(fCommandVector[i]);
        } else if (SkDrawCommand::kPopLayer_Action == result) {
            fActiveLayers.pop();
        }
    }

    for (int i = 0; i < fActiveLayers.count(); ++i) {
        fActiveLayers[i]->setActive(true);
    }

}

void SkDebugCanvas::drawTo(SkCanvas* canvas, int index, int m) {
    SkASSERT(!fCommandVector.isEmpty());
    SkASSERT(index < fCommandVector.count());

    int saveCount = canvas->save();

    SkRect windowRect = SkRect::MakeWH(SkIntToScalar(canvas->getBaseLayerSize().width()),
                                       SkIntToScalar(canvas->getBaseLayerSize().height()));

    bool pathOpsMode = getAllowSimplifyClip();
    canvas->setAllowSimplifyClip(pathOpsMode);
    canvas->clear(SK_ColorWHITE);
    canvas->resetMatrix();
    if (!windowRect.isEmpty()) {
        canvas->clipRect(windowRect, SkRegion::kReplace_Op);
    }
    this->applyUserTransform(canvas);

    if (fPaintFilterCanvas) {
        fPaintFilterCanvas->addCanvas(canvas);
        canvas = fPaintFilterCanvas.get();

    }

    if (fMegaVizMode) {
        this->markActiveCommands(index);
    }

#if SK_SUPPORT_GPU
    // If we have a GPU backend we can also visualize the batching information
    GrAuditTrail* at = nullptr;
    if (fDrawGpuBatchBounds || m != -1) {
        at = this->getAuditTrail(canvas);
    }
#endif

    for (int i = 0; i <= index; i++) {
        if (i == index && fFilter) {
            canvas->clear(0xAAFFFFFF);
        }

#if SK_SUPPORT_GPU
        // We need to flush any pending operations, or they might batch with commands below.
        // Previous operations were not registered with the audit trail when they were
        // created, so if we allow them to combine, the audit trail will fail to find them.
        canvas->flush();

        GrAuditTrail::AutoCollectBatches* acb = nullptr;
        if (at) {
            acb = new GrAuditTrail::AutoCollectBatches(at, i);
        }
#endif

        if (fCommandVector[i]->isVisible()) {
            if (fMegaVizMode && fCommandVector[i]->active()) {
                // "active" commands execute their visualization behaviors:
                //     All active saveLayers get replaced with saves so all draws go to the
                //     visible canvas.
                //     All active culls draw their cull box
                fCommandVector[i]->vizExecute(canvas);
            } else {
                fCommandVector[i]->setUserMatrix(fUserMatrix);
                fCommandVector[i]->execute(canvas);
            }
        }
#if SK_SUPPORT_GPU
        if (at && acb) {
            delete acb;
        }
#endif
    }

    if (SkColorGetA(fClipVizColor) != 0) {
        canvas->save();
        #define LARGE_COORD 1000000000
        canvas->clipRect(SkRect::MakeLTRB(-LARGE_COORD, -LARGE_COORD, LARGE_COORD, LARGE_COORD),
                       SkRegion::kReverseDifference_Op);
        SkPaint clipPaint;
        clipPaint.setColor(fClipVizColor);
        canvas->drawPaint(clipPaint);
        canvas->restore();
    }

    if (fMegaVizMode) {
        canvas->save();
        // nuke the CTM
        canvas->resetMatrix();
        // turn off clipping
        if (!windowRect.isEmpty()) {
            SkRect r = windowRect;
            r.outset(SK_Scalar1, SK_Scalar1);
            canvas->clipRect(r, SkRegion::kReplace_Op);
        }
        // visualize existing clips
        SkDebugClipVisitor visitor(canvas);

        canvas->replayClips(&visitor);

        canvas->restore();
    }
    if (pathOpsMode) {
        this->resetClipStackData();
        const SkClipStack* clipStack = canvas->getClipStack();
        SkClipStack::Iter iter(*clipStack, SkClipStack::Iter::kBottom_IterStart);
        const SkClipStack::Element* element;
        SkPath devPath;
        while ((element = iter.next())) {
            SkClipStack::Element::Type type = element->getType();
            SkPath operand;
            if (type != SkClipStack::Element::kEmpty_Type) {
               element->asPath(&operand);
            }
            SkRegion::Op elementOp = element->getOp();
            this->addClipStackData(devPath, operand, elementOp);
            if (elementOp == SkRegion::kReplace_Op) {
                devPath = operand;
            } else {
                Op(devPath, operand, (SkPathOp) elementOp, &devPath);
            }
        }
        this->lastClipStackData(devPath);
    }
    fMatrix = canvas->getTotalMatrix();
    if (!canvas->getClipDeviceBounds(&fClip)) {
        fClip.setEmpty();
    }

    canvas->restoreToCount(saveCount);

    if (fPaintFilterCanvas) {
        fPaintFilterCanvas->removeAll();
    }

#if SK_SUPPORT_GPU
    // draw any batches if required and issue a full reset onto GrAuditTrail
    if (at) {
        // just in case there is global reordering, we flush the canvas before querying
        // GrAuditTrail
        GrAuditTrail::AutoEnable ae(at);
        canvas->flush();

        // we pick three colorblind-safe colors, 75% alpha
        static const SkColor kTotalBounds = SkColorSetARGB(0xC0, 0x6A, 0x3D, 0x9A);
        static const SkColor kOpBatchBounds = SkColorSetARGB(0xC0, 0xE3, 0x1A, 0x1C);
        static const SkColor kOtherBatchBounds = SkColorSetARGB(0xC0, 0xFF, 0x7F, 0x00);

        // get the render target of the top device so we can ignore batches drawn offscreen
        SkBaseDevice* bd = canvas->getDevice_just_for_deprecated_compatibility_testing();
        SkGpuDevice* gbd = reinterpret_cast<SkGpuDevice*>(bd);
        uint32_t rtID = gbd->accessRenderTarget()->getUniqueID();

        // get the bounding boxes to draw
        SkTArray<GrAuditTrail::BatchInfo> childrenBounds;
        if (m == -1) {
            at->getBoundsByClientID(&childrenBounds, index);
        } else {
            // the client wants us to draw the mth batch
            at->getBoundsByBatchListID(&childrenBounds.push_back(), m);
        }
        SkPaint paint;
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setStrokeWidth(1);
        for (int i = 0; i < childrenBounds.count(); i++) {
            if (childrenBounds[i].fRenderTargetUniqueID != rtID) {
                // offscreen draw, ignore for now
                continue;
            }
            paint.setColor(kTotalBounds);
            canvas->drawRect(childrenBounds[i].fBounds, paint);
            for (int j = 0; j < childrenBounds[i].fBatches.count(); j++) {
                const GrAuditTrail::BatchInfo::Batch& batch = childrenBounds[i].fBatches[j];
                if (batch.fClientID != index) {
                    paint.setColor(kOtherBatchBounds);
                } else {
                    paint.setColor(kOpBatchBounds);
                }
                canvas->drawRect(batch.fBounds, paint);
            }
        }
    }
#endif
    this->cleanupAuditTrail(canvas);
}

void SkDebugCanvas::deleteDrawCommandAt(int index) {
    SkASSERT(index < fCommandVector.count());
    delete fCommandVector[index];
    fCommandVector.remove(index);
}

SkDrawCommand* SkDebugCanvas::getDrawCommandAt(int index) {
    SkASSERT(index < fCommandVector.count());
    return fCommandVector[index];
}

void SkDebugCanvas::setDrawCommandAt(int index, SkDrawCommand* command) {
    SkASSERT(index < fCommandVector.count());
    delete fCommandVector[index];
    fCommandVector[index] = command;
}

const SkTDArray<SkString*>* SkDebugCanvas::getCommandInfo(int index) const {
    SkASSERT(index < fCommandVector.count());
    return fCommandVector[index]->Info();
}

bool SkDebugCanvas::getDrawCommandVisibilityAt(int index) {
    SkASSERT(index < fCommandVector.count());
    return fCommandVector[index]->isVisible();
}

const SkTDArray <SkDrawCommand*>& SkDebugCanvas::getDrawCommands() const {
    return fCommandVector;
}

SkTDArray <SkDrawCommand*>& SkDebugCanvas::getDrawCommands() {
    return fCommandVector;
}

GrAuditTrail* SkDebugCanvas::getAuditTrail(SkCanvas* canvas) {
    GrAuditTrail* at = nullptr;
#if SK_SUPPORT_GPU
    GrContext* ctx = canvas->getGrContext();
    if (ctx) {
        at = ctx->getAuditTrail();
    }
#endif
    return at;
}

void SkDebugCanvas::drawAndCollectBatches(int n, SkCanvas* canvas) {
#if SK_SUPPORT_GPU
    GrAuditTrail* at = this->getAuditTrail(canvas);
    if (at) {
        // loop over all of the commands and draw them, this is to collect reordering
        // information
        for (int i = 0; i < this->getSize() && i <= n; i++) {
            GrAuditTrail::AutoCollectBatches enable(at, i);
            fCommandVector[i]->execute(canvas);
        }

        // in case there is some kind of global reordering
        {
            GrAuditTrail::AutoEnable ae(at);
            canvas->flush();
        }
    }
#endif
}

void SkDebugCanvas::cleanupAuditTrail(SkCanvas* canvas) {
    GrAuditTrail* at = this->getAuditTrail(canvas);
    if (at) {
#if SK_SUPPORT_GPU
        GrAuditTrail::AutoEnable ae(at);
        at->fullReset();
#endif
    }
}

Json::Value SkDebugCanvas::toJSON(UrlDataManager& urlDataManager, int n, SkCanvas* canvas) {
    this->drawAndCollectBatches(n, canvas);

    // now collect json
#if SK_SUPPORT_GPU
    GrAuditTrail* at = this->getAuditTrail(canvas);
#endif
    Json::Value result = Json::Value(Json::objectValue);
    result[SKDEBUGCANVAS_ATTRIBUTE_VERSION] = Json::Value(SKDEBUGCANVAS_VERSION);
    Json::Value commands = Json::Value(Json::arrayValue);
    for (int i = 0; i < this->getSize() && i <= n; i++) {
        commands[i] = this->getDrawCommandAt(i)->toJSON(urlDataManager);
#if SK_SUPPORT_GPU
        if (at) {
            // TODO if this is inefficient we could add a method to GrAuditTrail which takes
            // a Json::Value and is only compiled in this file
            Json::Value parsedFromString;
            Json::Reader reader;
            SkAssertResult(reader.parse(at->toJson(i).c_str(), parsedFromString));

            commands[i][SKDEBUGCANVAS_ATTRIBUTE_AUDITTRAIL] = parsedFromString;
        }
#endif
    }
    this->cleanupAuditTrail(canvas);
    result[SKDEBUGCANVAS_ATTRIBUTE_COMMANDS] = commands;
    return result;
}

Json::Value SkDebugCanvas::toJSONBatchList(int n, SkCanvas* canvas) {
    this->drawAndCollectBatches(n, canvas);

    Json::Value parsedFromString;
#if SK_SUPPORT_GPU
    GrAuditTrail* at = this->getAuditTrail(canvas);
    if (at) {
        GrAuditTrail::AutoManageBatchList enable(at);
        Json::Reader reader;
        SkAssertResult(reader.parse(at->toJson().c_str(), parsedFromString));
    }
#endif
    this->cleanupAuditTrail(canvas);
    return parsedFromString;
}

void SkDebugCanvas::updatePaintFilterCanvas() {
    if (!fOverdrawViz && !fOverrideFilterQuality) {
        fPaintFilterCanvas.reset(nullptr);
        return;
    }

    const SkImageInfo info = this->imageInfo();
    fPaintFilterCanvas.reset(new DebugPaintFilterCanvas(info.width(), info.height(), fOverdrawViz,
                                                        fOverrideFilterQuality, fFilterQuality));
}

void SkDebugCanvas::setOverdrawViz(bool overdrawViz) {
    if (fOverdrawViz == overdrawViz) {
        return;
    }

    fOverdrawViz = overdrawViz;
    this->updatePaintFilterCanvas();
}

void SkDebugCanvas::overrideTexFiltering(bool overrideTexFiltering, SkFilterQuality quality) {
    if (fOverrideFilterQuality == overrideTexFiltering && fFilterQuality == quality) {
        return;
    }

    fOverrideFilterQuality = overrideTexFiltering;
    fFilterQuality = quality;
    this->updatePaintFilterCanvas();
}

void SkDebugCanvas::onClipPath(const SkPath& path, SkRegion::Op op, ClipEdgeStyle edgeStyle) {
    this->addDrawCommand(new SkClipPathCommand(path, op, kSoft_ClipEdgeStyle == edgeStyle));
}

void SkDebugCanvas::onClipRect(const SkRect& rect, SkRegion::Op op, ClipEdgeStyle edgeStyle) {
    this->addDrawCommand(new SkClipRectCommand(rect, op, kSoft_ClipEdgeStyle == edgeStyle));
}

void SkDebugCanvas::onClipRRect(const SkRRect& rrect, SkRegion::Op op, ClipEdgeStyle edgeStyle) {
    this->addDrawCommand(new SkClipRRectCommand(rrect, op, kSoft_ClipEdgeStyle == edgeStyle));
}

void SkDebugCanvas::onClipRegion(const SkRegion& region, SkRegion::Op op) {
    this->addDrawCommand(new SkClipRegionCommand(region, op));
}

void SkDebugCanvas::didConcat(const SkMatrix& matrix) {
    this->addDrawCommand(new SkConcatCommand(matrix));
    this->INHERITED::didConcat(matrix);
}

void SkDebugCanvas::onDrawAnnotation(const SkRect& rect, const char key[], SkData* value) {
    this->addDrawCommand(new SkDrawAnnotationCommand(rect, key, sk_ref_sp(value)));
}

void SkDebugCanvas::onDrawBitmap(const SkBitmap& bitmap, SkScalar left,
                                 SkScalar top, const SkPaint* paint) {
    this->addDrawCommand(new SkDrawBitmapCommand(bitmap, left, top, paint));
}

void SkDebugCanvas::onDrawBitmapRect(const SkBitmap& bitmap, const SkRect* src, const SkRect& dst,
                                     const SkPaint* paint, SrcRectConstraint constraint) {
    this->addDrawCommand(new SkDrawBitmapRectCommand(bitmap, src, dst, paint,
                                                     (SrcRectConstraint)constraint));
}

void SkDebugCanvas::onDrawBitmapNine(const SkBitmap& bitmap, const SkIRect& center,
                                     const SkRect& dst, const SkPaint* paint) {
    this->addDrawCommand(new SkDrawBitmapNineCommand(bitmap, center, dst, paint));
}

void SkDebugCanvas::onDrawImage(const SkImage* image, SkScalar left, SkScalar top,
                                const SkPaint* paint) {
    this->addDrawCommand(new SkDrawImageCommand(image, left, top, paint));
}

void SkDebugCanvas::onDrawImageRect(const SkImage* image, const SkRect* src, const SkRect& dst,
                                    const SkPaint* paint, SrcRectConstraint constraint) {
    this->addDrawCommand(new SkDrawImageRectCommand(image, src, dst, paint, constraint));
}

void SkDebugCanvas::onDrawOval(const SkRect& oval, const SkPaint& paint) {
    this->addDrawCommand(new SkDrawOvalCommand(oval, paint));
}

void SkDebugCanvas::onDrawPaint(const SkPaint& paint) {
    this->addDrawCommand(new SkDrawPaintCommand(paint));
}

void SkDebugCanvas::onDrawPath(const SkPath& path, const SkPaint& paint) {
    this->addDrawCommand(new SkDrawPathCommand(path, paint));
}

void SkDebugCanvas::onDrawPicture(const SkPicture* picture,
                                  const SkMatrix* matrix,
                                  const SkPaint* paint) {
    this->addDrawCommand(new SkBeginDrawPictureCommand(picture, matrix, paint));
    SkAutoCanvasMatrixPaint acmp(this, matrix, paint, picture->cullRect());
    picture->playback(this);
    this->addDrawCommand(new SkEndDrawPictureCommand(SkToBool(matrix) || SkToBool(paint)));
}

void SkDebugCanvas::onDrawPoints(PointMode mode, size_t count,
                                 const SkPoint pts[], const SkPaint& paint) {
    this->addDrawCommand(new SkDrawPointsCommand(mode, count, pts, paint));
}

void SkDebugCanvas::onDrawPosText(const void* text, size_t byteLength, const SkPoint pos[],
                                  const SkPaint& paint) {
    this->addDrawCommand(new SkDrawPosTextCommand(text, byteLength, pos, paint));
}

void SkDebugCanvas::onDrawPosTextH(const void* text, size_t byteLength, const SkScalar xpos[],
                                   SkScalar constY, const SkPaint& paint) {
    this->addDrawCommand(
        new SkDrawPosTextHCommand(text, byteLength, xpos, constY, paint));
}

void SkDebugCanvas::onDrawRect(const SkRect& rect, const SkPaint& paint) {
    // NOTE(chudy): Messing up when renamed to DrawRect... Why?
    addDrawCommand(new SkDrawRectCommand(rect, paint));
}

void SkDebugCanvas::onDrawRRect(const SkRRect& rrect, const SkPaint& paint) {
    this->addDrawCommand(new SkDrawRRectCommand(rrect, paint));
}

void SkDebugCanvas::onDrawDRRect(const SkRRect& outer, const SkRRect& inner,
                                 const SkPaint& paint) {
    this->addDrawCommand(new SkDrawDRRectCommand(outer, inner, paint));
}

void SkDebugCanvas::onDrawText(const void* text, size_t byteLength, SkScalar x, SkScalar y,
                               const SkPaint& paint) {
    this->addDrawCommand(new SkDrawTextCommand(text, byteLength, x, y, paint));
}

void SkDebugCanvas::onDrawTextOnPath(const void* text, size_t byteLength, const SkPath& path,
                                     const SkMatrix* matrix, const SkPaint& paint) {
    this->addDrawCommand(
        new SkDrawTextOnPathCommand(text, byteLength, path, matrix, paint));
}

void SkDebugCanvas::onDrawTextRSXform(const void* text, size_t byteLength, const SkRSXform xform[],
                                      const SkRect* cull, const SkPaint& paint) {
    this->addDrawCommand(new SkDrawTextRSXformCommand(text, byteLength, xform, cull, paint));
}

void SkDebugCanvas::onDrawTextBlob(const SkTextBlob* blob, SkScalar x, SkScalar y,
                                   const SkPaint& paint) {
    this->addDrawCommand(new SkDrawTextBlobCommand(blob, x, y, paint));
}

void SkDebugCanvas::onDrawPatch(const SkPoint cubics[12], const SkColor colors[4],
                                const SkPoint texCoords[4], SkXfermode* xmode,
                                const SkPaint& paint) {
    this->addDrawCommand(new SkDrawPatchCommand(cubics, colors, texCoords, xmode, paint));
}

void SkDebugCanvas::onDrawVertices(VertexMode vmode, int vertexCount, const SkPoint vertices[],
                                   const SkPoint texs[], const SkColor colors[],
                                   SkXfermode*, const uint16_t indices[], int indexCount,
                                   const SkPaint& paint) {
    this->addDrawCommand(new SkDrawVerticesCommand(vmode, vertexCount, vertices,
                         texs, colors, nullptr, indices, indexCount, paint));
}

void SkDebugCanvas::willRestore() {
    this->addDrawCommand(new SkRestoreCommand());
    this->INHERITED::willRestore();
}

void SkDebugCanvas::willSave() {
    this->addDrawCommand(new SkSaveCommand());
    this->INHERITED::willSave();
}

SkCanvas::SaveLayerStrategy SkDebugCanvas::getSaveLayerStrategy(const SaveLayerRec& rec) {
    this->addDrawCommand(new SkSaveLayerCommand(rec));
    (void)this->INHERITED::getSaveLayerStrategy(rec);
    // No need for a full layer.
    return kNoLayer_SaveLayerStrategy;
}

void SkDebugCanvas::didSetMatrix(const SkMatrix& matrix) {
    this->addDrawCommand(new SkSetMatrixCommand(matrix));
    this->INHERITED::didSetMatrix(matrix);
}

void SkDebugCanvas::didTranslateZ(SkScalar z) {
    this->addDrawCommand(new SkTranslateZCommand(z));
    this->INHERITED::didTranslateZ(z);
}

void SkDebugCanvas::toggleCommand(int index, bool toggle) {
    SkASSERT(index < fCommandVector.count());
    fCommandVector[index]->setVisible(toggle);
}

static const char* gFillTypeStrs[] = {
    "kWinding_FillType",
    "kEvenOdd_FillType",
    "kInverseWinding_FillType",
    "kInverseEvenOdd_FillType"
};

static const char* gOpStrs[] = {
    "kDifference_PathOp",
    "kIntersect_PathOp",
    "kUnion_PathOp",
    "kXor_PathOp",
    "kReverseDifference_PathOp",
};

static const char kHTML4SpaceIndent[] = "&nbsp;&nbsp;&nbsp;&nbsp;";

void SkDebugCanvas::outputScalar(SkScalar num) {
    if (num == (int) num) {
        fClipStackData.appendf("%d", (int) num);
    } else {
        SkString str;
        str.printf("%1.9g", num);
        int width = (int) str.size();
        const char* cStr = str.c_str();
        while (cStr[width - 1] == '0') {
            --width;
        }
        str.resize(width);
        fClipStackData.appendf("%sf", str.c_str());
    }
}

void SkDebugCanvas::outputPointsCommon(const SkPoint* pts, int count) {
    for (int index = 0; index < count; ++index) {
        this->outputScalar(pts[index].fX);
        fClipStackData.appendf(", ");
        this->outputScalar(pts[index].fY);
        if (index + 1 < count) {
            fClipStackData.appendf(", ");
        }
    }
}

void SkDebugCanvas::outputPoints(const SkPoint* pts, int count) {
    this->outputPointsCommon(pts, count);
    fClipStackData.appendf(");<br>");
}

void SkDebugCanvas::outputConicPoints(const SkPoint* pts, SkScalar weight) {
    this->outputPointsCommon(pts, 2);
    fClipStackData.appendf(", ");
    this->outputScalar(weight);
    fClipStackData.appendf(");<br>");
}

void SkDebugCanvas::addPathData(const SkPath& path, const char* pathName) {
    SkPath::RawIter iter(path);
    SkPath::FillType fillType = path.getFillType();
    fClipStackData.appendf("%sSkPath %s;<br>", kHTML4SpaceIndent, pathName);
    fClipStackData.appendf("%s%s.setFillType(SkPath::%s);<br>", kHTML4SpaceIndent, pathName,
            gFillTypeStrs[fillType]);
    iter.setPath(path);
    uint8_t verb;
    SkPoint pts[4];
    while ((verb = iter.next(pts)) != SkPath::kDone_Verb) {
        switch (verb) {
            case SkPath::kMove_Verb:
                fClipStackData.appendf("%s%s.moveTo(", kHTML4SpaceIndent, pathName);
                this->outputPoints(&pts[0], 1);
                continue;
            case SkPath::kLine_Verb:
                fClipStackData.appendf("%s%s.lineTo(", kHTML4SpaceIndent, pathName);
                this->outputPoints(&pts[1], 1);
                break;
            case SkPath::kQuad_Verb:
                fClipStackData.appendf("%s%s.quadTo(", kHTML4SpaceIndent, pathName);
                this->outputPoints(&pts[1], 2);
                break;
            case SkPath::kConic_Verb:
                fClipStackData.appendf("%s%s.conicTo(", kHTML4SpaceIndent, pathName);
                this->outputConicPoints(&pts[1], iter.conicWeight());
                break;
            case SkPath::kCubic_Verb:
                fClipStackData.appendf("%s%s.cubicTo(", kHTML4SpaceIndent, pathName);
                this->outputPoints(&pts[1], 3);
                break;
            case SkPath::kClose_Verb:
                fClipStackData.appendf("%s%s.close();<br>", kHTML4SpaceIndent, pathName);
                break;
            default:
                SkDEBUGFAIL("bad verb");
                return;
        }
    }
}

void SkDebugCanvas::addClipStackData(const SkPath& devPath, const SkPath& operand,
                                     SkRegion::Op elementOp) {
    if (elementOp == SkRegion::kReplace_Op) {
        if (!lastClipStackData(devPath)) {
            fSaveDevPath = operand;
        }
        fCalledAddStackData = false;
    } else {
        fClipStackData.appendf("<br>static void test(skiatest::Reporter* reporter,"
            " const char* filename) {<br>");
        addPathData(fCalledAddStackData ? devPath : fSaveDevPath, "path");
        addPathData(operand, "pathB");
        fClipStackData.appendf("%stestPathOp(reporter, path, pathB, %s, filename);<br>",
            kHTML4SpaceIndent, gOpStrs[elementOp]);
        fClipStackData.appendf("}<br>");
        fCalledAddStackData = true;
    }
}

bool SkDebugCanvas::lastClipStackData(const SkPath& devPath) {
    if (fCalledAddStackData) {
        fClipStackData.appendf("<br>");
        addPathData(devPath, "pathOut");
        return true;
    }
    return false;
}
