#ifndef PAINTPROVIDER_PAINTFONTS_H__
#define PAINTPROVIDER_PAINTFONTS_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "PaintImage.h"   // in order: everything above this in the module is visible here

// Outline fonts for the text boxes (PaintFonts), compiled once in the .cc
// like the codecs (PaintImage.h).
#include "stb_truetype.h"

/*
 * ── FONTS FOR TEXT BOXES ─────────────────────────────────────────────────────
 *
 * A Glyphs provider with more than one face. Font 0 is the pixel font the rest
 * of the sheet is lettered in (RenderProvider::TextLabel, bound with
 * BindPixel), kept because it is the look this program already has; fonts 1
 * and up are TrueType files loaded by path (Load), measured and drawn with
 * stb_truetype at any size, antialiased.
 *
 * FILES, NOT THE BROWSER'S FONTS. A text box in a shared session wraps where
 * the box's width says, and every page in the room has to wrap it at the same
 * words -- which only holds if every page measures with the same outlines. A
 * font from the operating system or the browser would be a different font on
 * each machine. So the faces ship with the program (PaintProvider/fonts, each
 * beside its licence) and are staged like the scripts are.
 *
 * SIZE IS THE LINE: size_px is the height from the highest ascender to the
 * lowest descender, the same meaning the pixel font's size has, so switching a
 * box's font keeps its lines about as tall as they were.
 *
 * Glyph bitmaps are cached per font, size and character: a text box is drawn
 * again on every render of the view, which is every stroke's sample.
 */
class PaintFonts : public GlyphsBase<PaintFonts>,
                   public DeletableBase<PaintFonts>
{
public:
    WIRE_TYPE_IDENTITY(PaintFonts);

    PaintFonts() = default;
    bool DeleteConcrete() override { return true; }

    bool Create() { this->addTag("active"); return true; }

    // Font 0: whatever Glyphs leaf draws the sheet's own lettering.
    void BindPixel(ETCS::RID glyphs) { m_pixel = glyphs; }

    /*
     * A FONT THAT DID NOT LOAD STILL TAKES ITS NUMBER. The number is what a box
     * stores and what the bar's buttons name, so a missing file must not shift
     * every font after it onto the wrong face -- the slot is kept, and a box in
     * it is drawn in the pixel font until the file is there.
     */
    bool Load(const std::string& name, const std::string& path)
    {
        auto face = std::make_unique<Face>();
        face->name = name;
        std::ifstream in(path, std::ios::binary);
        if (!in)
            ETCS_LOG("PaintFonts", "Load " << name << ": cannot open '" << path
                     << "' -- font " << (m_faces.size() + 1) << " draws in the pixel font.");
        else
        {
            face->bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            const int offset = face->bytes.empty() ? -1 : stbtt_GetFontOffsetForIndex(face->bytes.data(), 0);
            face->loaded = offset >= 0 && stbtt_InitFont(&face->info, face->bytes.data(), offset);
            if (face->loaded) stbtt_GetFontVMetrics(&face->info, &face->ascent, &face->descent, &face->gap);
            else ETCS_LOG("PaintFonts", "Load " << name << ": '" << path << "' is not a font stb_truetype can read"
                          " -- font " << (m_faces.size() + 1) << " draws in the pixel font.");
        }
        const bool ok = face->loaded;
        std::lock_guard<std::mutex> g(m_mu);
        m_faces.push_back(std::move(face));
        if (ok) ETCS_LOG("PaintFonts", "font " << m_faces.size() << " '" << name << "' from '" << path << "'.");
        return ok;
    }

    uint32_t count() const { return static_cast<uint32_t>(m_faces.size()) + 1; }
    std::string nameOf(uint32_t font) const
    {
        if (font == 0 || font > m_faces.size()) return "pixel";
        return m_faces[font - 1]->name;
    }

    void Report() const
    {
        ETCS_LOG("PaintFonts", "0 pixel" << (m_pixel ? "" : " (unbound)"));
        for (size_t i = 0; i < m_faces.size(); ++i)
            ETCS_LOG("PaintFonts", (i + 1) << " " << m_faces[i]->name << (m_faces[i]->loaded ? "" : " (not loaded: pixel font)"));
    }

    TextExtent MeasureTextConcrete(const char* text, uint32_t font, uint32_t size_px) override
    {
        Face* f = face(font);
        if (!f) return pixel_measure(text, size_px);
        const float scale = stbtt_ScaleForPixelHeight(&f->info, static_cast<float>(std::max<uint32_t>(1, size_px)));
        float w = 0.0f;
        int prev = 0;
        for (const char* c = text ? text : ""; *c; ++c)
        {
            const int cp = static_cast<unsigned char>(*c);
            int adv = 0, lsb = 0;
            stbtt_GetCodepointHMetrics(&f->info, cp, &adv, &lsb);
            if (prev) w += scale * stbtt_GetCodepointKernAdvance(&f->info, prev, cp);
            w += scale * adv;
            prev = cp;
        }
        const float asc = f->ascent * scale, desc = -f->descent * scale;
        return TextExtent{ static_cast<uint32_t>(std::ceil(w)),
                           static_cast<uint32_t>(std::ceil(asc + desc)),
                           static_cast<uint32_t>(std::ceil(asc)) };
    }

    TextExtent RasterizeTextConcrete(ETCS::RID target, const char* text, uint32_t font, uint32_t size_px,
                                     int32_t x, int32_t y, float r, float g, float b, float a) override
    {
        Face* f = face(font);
        // A face blends coverage into host bytes, so a target whose bytes are
        // not its picture -- a surface drawing through a device -- takes the
        // pixel font, which draws through the Surface verbs.
        Pixels_* dst = etcs_direct_pixels(ETCS::resolve_in_family<Pixels_>("Pixels", target));
        if (!f || !dst)
        {
            ETCS::Held<Glyphs_> px = ETCS::resolve_held<Glyphs_>("Glyphs", m_pixel);
            if (!px) return f ? MeasureTextConcrete(text, font, size_px) : TextExtent{ 0, 0, 0 };
            return px->RasterizeText(target, text, 0, size_px, x, y, r, g, b, a);
        }
        const TextExtent e = MeasureTextConcrete(text, font, size_px);
        if (!dst->PixelData() || !text) return e;
        const float scale = stbtt_ScaleForPixelHeight(&f->info, static_cast<float>(std::max<uint32_t>(1, size_px)));
        const int32_t base = y + static_cast<int32_t>(e.baseline);
        float pen = static_cast<float>(x);
        int prev = 0;
        std::lock_guard<std::mutex> lock(m_mu);
        for (const char* c = text; *c; ++c)
        {
            const int cp = static_cast<unsigned char>(*c);
            if (prev) pen += scale * stbtt_GetCodepointKernAdvance(&f->info, prev, cp);
            const Glyph& gl = glyph(*f, font, size_px, scale, cp);
            blend(*dst, gl, static_cast<int32_t>(std::lround(pen)) + gl.xoff, base + gl.yoff, r, g, b, a);
            pen += gl.advance;
            prev = cp;
        }
        etcs_mark_observed(static_cast<ETCS::Entity*>(dst));
        return e;
    }

private:
    struct Face
    {
        std::string name;
        std::vector<unsigned char> bytes;     // stb_truetype reads the file in place
        bool loaded = false;                  // a kept slot for a file that was not there
        stbtt_fontinfo info{};
        int ascent = 0, descent = 0, gap = 0;
    };
    struct Glyph
    {
        int32_t w = 0, h = 0, xoff = 0, yoff = 0;
        float advance = 0.0f;
        std::vector<uint8_t> cover;
    };

    Face* face(uint32_t font) const
    {
        if (font == 0 || font > m_faces.size()) return nullptr;
        Face* f = m_faces[font - 1].get();
        return f->loaded ? f : nullptr;
    }

    TextExtent pixel_measure(const char* text, uint32_t size_px)
    {
        ETCS::Held<Glyphs_> px = ETCS::resolve_held<Glyphs_>("Glyphs", m_pixel);
        if (!px) return TextExtent{ 0, 0, 0 };
        return px->MeasureText(text, 0, size_px);
    }

    const Glyph& glyph(Face& f, uint32_t font, uint32_t size_px, float scale, int cp)
    {
        const uint64_t key = (static_cast<uint64_t>(font) << 48) | (static_cast<uint64_t>(size_px) << 24)
                           | static_cast<uint64_t>(cp);
        auto it = m_cache.find(key);
        if (it != m_cache.end()) return it->second;
        if (m_cache.size() > 8192) m_cache.clear();   // a bound, not a policy: sizes come and go
        Glyph gl;
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&f.info, cp, &adv, &lsb);
        gl.advance = adv * scale;
        int w = 0, h = 0, xo = 0, yo = 0;
        unsigned char* bm = stbtt_GetCodepointBitmap(&f.info, scale, scale, cp, &w, &h, &xo, &yo);
        if (bm)
        {
            gl.w = w; gl.h = h; gl.xoff = xo; gl.yoff = yo;
            gl.cover.assign(bm, bm + static_cast<size_t>(w) * h);
            stbtt_FreeBitmap(bm, nullptr);
        }
        return m_cache.emplace(key, std::move(gl)).first->second;
    }

    // Source-over, coverage times the colour's alpha, into straight RGBA --
    // the blend every raster in this module uses.
    static void blend(Pixels_& dst, const Glyph& gl, int32_t x0, int32_t y0, float r, float g, float b, float a)
    {
        uint8_t* px = dst.PixelData();
        const int32_t W = static_cast<int32_t>(dst.PixelWidth()), H = static_cast<int32_t>(dst.PixelHeight());
        for (int32_t gy = 0; gy < gl.h; ++gy)
        {
            const int32_t ty = y0 + gy;
            if (ty < 0 || ty >= H) continue;
            for (int32_t gx = 0; gx < gl.w; ++gx)
            {
                const int32_t tx = x0 + gx;
                if (tx < 0 || tx >= W) continue;
                const float sa = a * (gl.cover[static_cast<size_t>(gy) * gl.w + gx] / 255.0f);
                if (sa <= 0.0f) continue;
                uint8_t* d = px + (static_cast<size_t>(ty) * W + tx) * 4;
                const float da = d[3] / 255.0f;
                const float oa = sa + da * (1.0f - sa);
                auto mix = [&](float sc, uint8_t dc)
                { return paint_to_byte((sc * sa + (dc / 255.0f) * da * (1.0f - sa)) / oa); };
                d[0] = mix(r, d[0]); d[1] = mix(g, d[1]); d[2] = mix(b, d[2]);
                d[3] = paint_to_byte(oa);
            }
        }
    }

    ETCS::RID m_pixel = 0;
    std::vector<std::unique_ptr<Face>> m_faces;
    std::unordered_map<uint64_t, Glyph> m_cache;
    std::mutex m_mu;
};

#endif // PAINTPROVIDER_PAINTFONTS_H__
