/*
 * Copyright 2014 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkTypes.h"

#include "SkData.h"
#include "SkFixed.h"
#include "SkFontDescriptor.h"
#include "SkFontHost_FreeType_common.h"
#include "SkFontMgr.h"
#include "SkFontMgr_android.h"
#include "SkFontMgr_android_parser.h"
#include "SkFontStyle.h"
#include "SkOSFile.h"
#include "SkPaint.h"
#include "SkRefCnt.h"
#include "SkString.h"
#include "SkStream.h"
#include "SkTArray.h"
#include "SkTDArray.h"
#include "SkTSearch.h"
#include "SkTemplates.h"
#include "SkTypefaceCache.h"

#include <limits>

class SkData;

class SkTypeface_Android : public SkTypeface_FreeType {
public:
    SkTypeface_Android(const SkFontStyle& style,
                       bool isFixedPitch,
                       const SkString& familyName)
        : INHERITED(style, isFixedPitch)
        , fFamilyName(familyName)
        { }

protected:
    void onGetFamilyName(SkString* familyName) const override {
        *familyName = fFamilyName;
    }

    SkString fFamilyName;

private:
    typedef SkTypeface_FreeType INHERITED;
};

class SkTypeface_AndroidSystem : public SkTypeface_Android {
public:
    SkTypeface_AndroidSystem(const SkString& pathName,
                             const bool cacheFontFiles,
                             int index,
                             const SkFixed* axes, int axesCount,
                             const SkFontStyle& style,
                             bool isFixedPitch,
                             const SkString& familyName,
                             const SkLanguage& lang,
                             FontVariant variantStyle)
        : INHERITED(style, isFixedPitch, familyName)
        , fPathName(pathName)
        , fIndex(index)
        , fAxes(axes, axesCount)
        , fLang(lang)
        , fVariantStyle(variantStyle)
        , fFile(cacheFontFiles ? sk_fopen(fPathName.c_str(), kRead_SkFILE_Flag) : nullptr) {
        if (cacheFontFiles) {
            SkASSERT(fFile);
        }
    }

    SkStreamAsset* createStream() const {
        if (fFile) {
            SkData* data = SkData::NewFromFILE(fFile);
            return data ? new SkMemoryStream(data) : nullptr;
        }
        return SkStream::NewFromFile(fPathName.c_str());
    }

    virtual void onGetFontDescriptor(SkFontDescriptor* desc, bool* serialize) const override {
        SkASSERT(desc);
        SkASSERT(serialize);
        desc->setFamilyName(fFamilyName.c_str());
        *serialize = false;
    }
    SkStreamAsset* onOpenStream(int* ttcIndex) const override {
        *ttcIndex = fIndex;
        return this->createStream();
    }
    SkFontData* onCreateFontData() const override {
        return new SkFontData(this->createStream(), fIndex, fAxes.begin(), fAxes.count());
    }

    const SkString fPathName;
    int fIndex;
    const SkSTArray<4, SkFixed, true> fAxes;
    const SkLanguage fLang;
    const FontVariant fVariantStyle;
    SkAutoTCallVProc<FILE, sk_fclose> fFile;

    typedef SkTypeface_Android INHERITED;
};

class SkTypeface_AndroidStream : public SkTypeface_Android {
public:
    SkTypeface_AndroidStream(SkFontData* data,
                             const SkFontStyle& style,
                             bool isFixedPitch,
                             const SkString& familyName)
        : INHERITED(style, isFixedPitch, familyName)
        , fData(data)
    { }

    virtual void onGetFontDescriptor(SkFontDescriptor* desc,
                                     bool* serialize) const override {
        SkASSERT(desc);
        SkASSERT(serialize);
        desc->setFamilyName(fFamilyName.c_str());
        *serialize = true;
    }

    SkStreamAsset* onOpenStream(int* ttcIndex) const override {
        *ttcIndex = fData->getIndex();
        return fData->duplicateStream();
    }

    SkFontData* onCreateFontData() const override {
        return new SkFontData(*fData.get());
    }

private:
    const SkAutoTDelete<const SkFontData> fData;
    typedef SkTypeface_Android INHERITED;
};

class SkFontStyleSet_Android : public SkFontStyleSet {
    typedef SkTypeface_FreeType::Scanner Scanner;

public:
    explicit SkFontStyleSet_Android(const FontFamily& family, const Scanner& scanner,
                                    const bool cacheFontFiles) {
        const SkString* cannonicalFamilyName = nullptr;
        if (family.fNames.count() > 0) {
            cannonicalFamilyName = &family.fNames[0];
        }
        // TODO? make this lazy
        for (int i = 0; i < family.fFonts.count(); ++i) {
            const FontFileInfo& fontFile = family.fFonts[i];

            SkString pathName(family.fBasePath);
            pathName.append(fontFile.fFileName);

            SkAutoTDelete<SkStream> stream(SkStream::NewFromFile(pathName.c_str()));
            if (!stream.get()) {
                SkDEBUGF(("Requested font file %s does not exist or cannot be opened.\n",
                          pathName.c_str()));
                continue;
            }

            const int ttcIndex = fontFile.fIndex;
            SkString familyName;
            SkFontStyle style;
            bool isFixedWidth;
            Scanner::AxisDefinitions axisDefinitions;
            if (!scanner.scanFont(stream.get(), ttcIndex,
                                  &familyName, &style, &isFixedWidth, &axisDefinitions))
            {
                SkDEBUGF(("Requested font file %s exists, but is not a valid font.\n",
                          pathName.c_str()));
                continue;
            }

            int weight = fontFile.fWeight != 0 ? fontFile.fWeight : style.weight();
            SkFontStyle::Slant slant = style.slant();
            switch (fontFile.fStyle) {
                case FontFileInfo::Style::kAuto: slant = style.slant(); break;
                case FontFileInfo::Style::kNormal: slant = SkFontStyle::kUpright_Slant; break;
                case FontFileInfo::Style::kItalic: slant = SkFontStyle::kItalic_Slant; break;
                default: SkASSERT(false); break;
            }
            style = SkFontStyle(weight, style.width(), slant);

            const SkLanguage& lang = family.fLanguage;
            uint32_t variant = family.fVariant;
            if (kDefault_FontVariant == variant) {
                variant = kCompact_FontVariant | kElegant_FontVariant;
            }

            // The first specified family name overrides the family name found in the font.
            // TODO: SkTypeface_AndroidSystem::onCreateFamilyNameIterator should return
            // all of the specified family names in addition to the names found in the font.
            if (cannonicalFamilyName != nullptr) {
                familyName = *cannonicalFamilyName;
            }

            SkAutoSTMalloc<4, SkFixed> axisValues(axisDefinitions.count());
            Scanner::computeAxisValues(axisDefinitions,
                                       fontFile.fAxes.begin(), fontFile.fAxes.count(),
                                       axisValues, familyName);

            fStyles.push_back().reset(new SkTypeface_AndroidSystem(
                    pathName, cacheFontFiles, ttcIndex, axisValues.get(), axisDefinitions.count(),
                    style, isFixedWidth, familyName, lang, variant));
        }
    }

    int count() override {
        return fStyles.count();
    }
    void getStyle(int index, SkFontStyle* style, SkString* name) override {
        if (index < 0 || fStyles.count() <= index) {
            return;
        }
        if (style) {
            *style = this->style(index);
        }
        if (name) {
            name->reset();
        }
    }
    SkTypeface_AndroidSystem* createTypeface(int index) override {
        if (index < 0 || fStyles.count() <= index) {
            return nullptr;
        }
        return SkRef(fStyles[index].get());
    }

    /** Find the typeface in this style set that most closely matches the given pattern.
     *  TODO: consider replacing with SkStyleSet_Indirect::matchStyle();
     *  this simpler version using match_score() passes all our tests.
     */
    SkTypeface_AndroidSystem* matchStyle(const SkFontStyle& pattern) override {
        if (0 == fStyles.count()) {
            return nullptr;
        }
        SkTypeface_AndroidSystem* closest = fStyles[0];
        int minScore = std::numeric_limits<int>::max();
        for (int i = 0; i < fStyles.count(); ++i) {
            SkFontStyle style = this->style(i);
            int score = match_score(pattern, style);
            if (score < minScore) {
                closest = fStyles[i];
                minScore = score;
            }
        }
        return SkRef(closest);
    }

private:
    SkFontStyle style(int index) {
        return fStyles[index]->fontStyle();
    }
    static int match_score(const SkFontStyle& pattern, const SkFontStyle& candidate) {
        int score = 0;
        score += SkTAbs((pattern.width() - candidate.width()) * 100);
        score += SkTAbs((pattern.slant() == candidate.slant()) ? 0 : 1000);
        score += SkTAbs(pattern.weight() - candidate.weight());
        return score;
    }

    SkTArray<SkAutoTUnref<SkTypeface_AndroidSystem>, true> fStyles;

    friend struct NameToFamily;
    friend class SkFontMgr_Android;

    typedef SkFontStyleSet INHERITED;
};

/** On Android a single family can have many names, but our API assumes unique names.
 *  Map names to the back end so that all names for a given family refer to the same
 *  (non-replicated) set of typefaces.
 *  SkTDict<> doesn't let us do index-based lookup, so we write our own mapping.
 */
struct NameToFamily {
    SkString name;
    SkFontStyleSet_Android* styleSet;
};

class SkFontMgr_Android : public SkFontMgr {
public:
    SkFontMgr_Android(const SkFontMgr_Android_CustomFonts* custom) {
        SkTDArray<FontFamily*> families;
        if (custom && SkFontMgr_Android_CustomFonts::kPreferSystem != custom->fSystemFontUse) {
            SkString base(custom->fBasePath);
            SkFontMgr_Android_Parser::GetCustomFontFamilies(
                families, base, custom->fFontsXml, custom->fFallbackFontsXml);
        }
        if (!custom ||
            (custom && SkFontMgr_Android_CustomFonts::kOnlyCustom != custom->fSystemFontUse))
        {
            SkFontMgr_Android_Parser::GetSystemFontFamilies(families);
        }
        if (custom && SkFontMgr_Android_CustomFonts::kPreferSystem == custom->fSystemFontUse) {
            SkString base(custom->fBasePath);
            SkFontMgr_Android_Parser::GetCustomFontFamilies(
                families, base, custom->fFontsXml, custom->fFallbackFontsXml);
        }
        this->buildNameToFamilyMap(families, custom ? custom->fIsolated : false);
        this->findDefaultFont();
        families.deleteAll();
    }

protected:
    /** Returns not how many families we have, but how many unique names
     *  exist among the families.
     */
    int onCountFamilies() const override {
        return fNameToFamilyMap.count();
    }

    void onGetFamilyName(int index, SkString* familyName) const override {
        if (index < 0 || fNameToFamilyMap.count() <= index) {
            familyName->reset();
            return;
        }
        familyName->set(fNameToFamilyMap[index].name);
    }

    SkFontStyleSet* onCreateStyleSet(int index) const override {
        if (index < 0 || fNameToFamilyMap.count() <= index) {
            return nullptr;
        }
        return SkRef(fNameToFamilyMap[index].styleSet);
    }

    SkFontStyleSet* onMatchFamily(const char familyName[]) const override {
        if (!familyName) {
            return nullptr;
        }
        SkAutoAsciiToLC tolc(familyName);
        for (int i = 0; i < fNameToFamilyMap.count(); ++i) {
            if (fNameToFamilyMap[i].name.equals(tolc.lc())) {
                return SkRef(fNameToFamilyMap[i].styleSet);
            }
        }
        // TODO: eventually we should not need to name fallback families.
        for (int i = 0; i < fFallbackNameToFamilyMap.count(); ++i) {
            if (fFallbackNameToFamilyMap[i].name.equals(tolc.lc())) {
                return SkRef(fFallbackNameToFamilyMap[i].styleSet);
            }
        }
        return nullptr;
    }

    virtual SkTypeface* onMatchFamilyStyle(const char familyName[],
                                           const SkFontStyle& style) const override {
        SkAutoTUnref<SkFontStyleSet> sset(this->matchFamily(familyName));
        return sset->matchStyle(style);
    }

    virtual SkTypeface* onMatchFaceStyle(const SkTypeface* typeface,
                                         const SkFontStyle& style) const override {
        for (int i = 0; i < fFontStyleSets.count(); ++i) {
            for (int j = 0; j < fFontStyleSets[i]->fStyles.count(); ++j) {
                if (fFontStyleSets[i]->fStyles[j] == typeface) {
                    return fFontStyleSets[i]->matchStyle(style);
                }
            }
        }
        return nullptr;
    }

    static sk_sp<SkTypeface_AndroidSystem> find_family_style_character(
            const SkTDArray<NameToFamily>& fallbackNameToFamilyMap,
            const SkFontStyle& style, bool elegant,
            const SkString& langTag, SkUnichar character)
    {
        for (int i = 0; i < fallbackNameToFamilyMap.count(); ++i) {
            SkFontStyleSet_Android* family = fallbackNameToFamilyMap[i].styleSet;
            sk_sp<SkTypeface_AndroidSystem> face(family->matchStyle(style));

            if (!langTag.isEmpty() && !face->fLang.getTag().startsWith(langTag.c_str())) {
                continue;
            }

            if (SkToBool(face->fVariantStyle & kElegant_FontVariant) != elegant) {
                continue;
            }

            SkPaint paint;
            paint.setTypeface(face);
            paint.setTextEncoding(SkPaint::kUTF32_TextEncoding);

            uint16_t glyphID;
            paint.textToGlyphs(&character, sizeof(character), &glyphID);
            if (glyphID != 0) {
                return face;
            }
        }
        return nullptr;
    }

    virtual SkTypeface* onMatchFamilyStyleCharacter(const char familyName[],
                                                    const SkFontStyle& style,
                                                    const char* bcp47[],
                                                    int bcp47Count,
                                                    SkUnichar character) const override
    {
        // The variant 'elegant' is 'not squashed', 'compact' is 'stays in ascent/descent'.
        // The variant 'default' means 'compact and elegant'.
        // As a result, it is not possible to know the variant context from the font alone.
        // TODO: add 'is_elegant' and 'is_compact' bits to 'style' request.

        // The first time match anything elegant, second time anything not elegant.
        for (int elegant = 2; elegant --> 0;) {
            for (int bcp47Index = bcp47Count; bcp47Index --> 0;) {
                SkLanguage lang(bcp47[bcp47Index]);
                while (!lang.getTag().isEmpty()) {
                    sk_sp<SkTypeface_AndroidSystem> matchingTypeface =
                        find_family_style_character(fFallbackNameToFamilyMap,
                                                    style, SkToBool(elegant),
                                                    lang.getTag(), character);
                    if (matchingTypeface) {
                        return matchingTypeface.release();
                    }

                    lang = lang.getParent();
                }
            }
            sk_sp<SkTypeface_AndroidSystem> matchingTypeface =
                find_family_style_character(fFallbackNameToFamilyMap,
                                            style, SkToBool(elegant),
                                            SkString(), character);
            if (matchingTypeface) {
                return matchingTypeface.release();
            }
        }
        return nullptr;
    }

    SkTypeface* onCreateFromData(SkData* data, int ttcIndex) const override {
        return this->createFromStream(new SkMemoryStream(data), ttcIndex);
    }

    SkTypeface* onCreateFromFile(const char path[], int ttcIndex) const override {
        SkAutoTDelete<SkStreamAsset> stream(SkStream::NewFromFile(path));
        return stream.get() ? this->createFromStream(stream.release(), ttcIndex) : nullptr;
    }

    SkTypeface* onCreateFromStream(SkStreamAsset* bareStream, int ttcIndex) const override {
        SkAutoTDelete<SkStreamAsset> stream(bareStream);
        bool isFixedPitch;
        SkFontStyle style;
        SkString name;
        if (!fScanner.scanFont(stream, ttcIndex, &name, &style, &isFixedPitch, nullptr)) {
            return nullptr;
        }
        SkFontData* data(new SkFontData(stream.release(), ttcIndex, nullptr, 0));
        return new SkTypeface_AndroidStream(data, style, isFixedPitch, name);
    }

    SkTypeface* onCreateFromStream(SkStreamAsset* s, const FontParameters& params) const override {
        using Scanner = SkTypeface_FreeType::Scanner;
        SkAutoTDelete<SkStreamAsset> stream(s);
        bool isFixedPitch;
        SkFontStyle style;
        SkString name;
        Scanner::AxisDefinitions axisDefinitions;
        if (!fScanner.scanFont(stream, params.getCollectionIndex(), &name, &style, &isFixedPitch,
                               &axisDefinitions))
        {
            return nullptr;
        }

        int paramAxisCount;
        const FontParameters::Axis* paramAxes = params.getAxes(&paramAxisCount);
        SkAutoSTMalloc<4, SkFixed> axisValues(axisDefinitions.count());
        Scanner::computeAxisValues(axisDefinitions, paramAxes, paramAxisCount, axisValues, name);

        SkFontData* data(new SkFontData(stream.release(), params.getCollectionIndex(),
                                        axisValues.get(), axisDefinitions.count()));
        return new SkTypeface_AndroidStream(data, style, isFixedPitch, name);
    }

    SkTypeface* onCreateFromFontData(SkFontData* data) const override {
        SkStreamAsset* stream(data->getStream());
        bool isFixedPitch;
        SkFontStyle style;
        SkString name;
        if (!fScanner.scanFont(stream, data->getIndex(), &name, &style, &isFixedPitch, nullptr)) {
            return nullptr;
        }
        return new SkTypeface_AndroidStream(data, style, isFixedPitch, name);
    }

    SkTypeface* onLegacyCreateTypeface(const char familyName[], SkFontStyle style) const override {
        if (familyName) {
            // On Android, we must return nullptr when we can't find the requested
            // named typeface so that the system/app can provide their own recovery
            // mechanism. On other platforms we'd provide a typeface from the
            // default family instead.
            return this->onMatchFamilyStyle(familyName, style);
        }
        return fDefaultFamily->matchStyle(style);
    }


private:

    SkTypeface_FreeType::Scanner fScanner;

    SkTArray<SkAutoTUnref<SkFontStyleSet_Android>, true> fFontStyleSets;
    SkFontStyleSet* fDefaultFamily;
    SkTypeface* fDefaultTypeface;

    SkTDArray<NameToFamily> fNameToFamilyMap;
    SkTDArray<NameToFamily> fFallbackNameToFamilyMap;

    void buildNameToFamilyMap(SkTDArray<FontFamily*> families, const bool isolated) {
        for (int i = 0; i < families.count(); i++) {
            FontFamily& family = *families[i];

            SkTDArray<NameToFamily>* nameToFamily = &fNameToFamilyMap;
            if (family.fIsFallbackFont) {
                nameToFamily = &fFallbackNameToFamilyMap;

                if (0 == family.fNames.count()) {
                    SkString& fallbackName = family.fNames.push_back();
                    fallbackName.printf("%.2x##fallback", i);
                }
            }

            SkFontStyleSet_Android* newSet = new SkFontStyleSet_Android(family, fScanner, isolated);
            if (0 == newSet->count()) {
                delete newSet;
                continue;
            }
            fFontStyleSets.push_back().reset(newSet);

            for (int j = 0; j < family.fNames.count(); j++) {
                NameToFamily* nextEntry = nameToFamily->append();
                new (&nextEntry->name) SkString(family.fNames[j]);
                nextEntry->styleSet = newSet;
            }
        }
    }

    void findDefaultFont() {
        SkASSERT(!fFontStyleSets.empty());

        static const char* gDefaultNames[] = { "sans-serif" };
        for (size_t i = 0; i < SK_ARRAY_COUNT(gDefaultNames); ++i) {
            SkFontStyleSet* set = this->onMatchFamily(gDefaultNames[i]);
            if (nullptr == set) {
                continue;
            }
            SkTypeface* tf = set->matchStyle(SkFontStyle());
            if (nullptr == tf) {
                continue;
            }
            fDefaultFamily = set;
            fDefaultTypeface = tf;
            break;
        }
        if (nullptr == fDefaultTypeface) {
            fDefaultFamily = fFontStyleSets[0];
            fDefaultTypeface = fDefaultFamily->createTypeface(0);
        }
        SkASSERT(fDefaultFamily);
        SkASSERT(fDefaultTypeface);
    }

    typedef SkFontMgr INHERITED;
};

#ifdef SK_DEBUG
static char const * const gSystemFontUseStrings[] = {
    "OnlyCustom", "PreferCustom", "PreferSystem"
};
#endif
SkFontMgr* SkFontMgr_New_Android(const SkFontMgr_Android_CustomFonts* custom) {
    if (custom) {
        SkASSERT(0 <= custom->fSystemFontUse);
        SkASSERT(custom->fSystemFontUse < SK_ARRAY_COUNT(gSystemFontUseStrings));
        SkDEBUGF(("SystemFontUse: %s BasePath: %s Fonts: %s FallbackFonts: %s\n",
                  gSystemFontUseStrings[custom->fSystemFontUse],
                  custom->fBasePath,
                  custom->fFontsXml,
                  custom->fFallbackFontsXml));
    }

    return new SkFontMgr_Android(custom);
}
