#ifndef PAINTPROVIDER_PAINTIMAGE_H__
#define PAINTPROVIDER_PAINTIMAGE_H__

#include "../../../core_defs.h"
#include "../../../ontology.h"
#include "PaintLayer.h"   // in order: everything above this in the module is visible here

/*
 * THE CODECS. stb_image reads PNG, JPEG, BMP, GIF and TGA; stb_image_write
 * writes PNG. Two public-domain single headers, pinned as a vendor entry in
 * manifests/PaintProvider.json rather than copied into libs/, for the same
 * reason sqlite and glfw are fetched: a codec is somebody else's code at a
 * known revision, not this tree's. Declarations here; the implementation is
 * compiled ONCE, in PaintProvider.cc, which defines the *_IMPLEMENTATION macros
 * before including PaintProvider.h (and so, through the contract, this).
 *
 * PAM stays. It is what the page store keeps its rows in (PaintPages), and a
 * blob that is raw bytes behind a text header is the right thing for a
 * database to hold: no decode on load, and the same reader the export uses.
 * PNG is for files a person opens elsewhere.
 */
#include "stb_image.h"
#include "stb_image_write.h"

#if defined(__EMSCRIPTEN__)
// For MAIN_THREAD_EM_ASM: the canvas menu's save/load reach the page's own
// file controls by dispatching a DOM event (PaintCanvasMenu::page_event).
#include <emscripten.h>
#include <emscripten/em_asm.h>
// For paint_heap_headroom: the wasm heap is fixed (loaders/Makefile's memory
// note), so a picture has to ask before it allocates.
#include <emscripten/heap.h>
#include <malloc.h>
#include <unistd.h>
#endif

/*
 * ── RAW IMAGES: PAM IN, PAM OUT ──────────────────────────────────────────────
 *
 * A PATH IS THE INTERFACE, on both substrates, and that is the whole design.
 * A browser hands the page a File and a desktop hands the shell a path; those
 * are two ways of arriving at one thing, bytes at a name the process can open.
 * The page already stages every script it boots into the emscripten filesystem
 * (index.html's preRun: FS.mkdirTree, FS.writeFile), so an upload is one more
 * file written the same way and a download is one file read back. So there is
 * one import and one export with no #ifdef in either, and the substrate's part
 * is getting bytes to and from a name -- which it was already doing.
 *
 * PAM, BECAUSE IT IS THE PIXELS. P7 with TUPLTYPE RGB_ALPHA is exactly what
 * Pixels_ stores -- RGBA8, non-premultiplied, row-major, top-left -- behind a
 * seven-line text header, so a write is the header and one write of the buffer
 * and a read is a tokenizer and one copy. GIMP, ImageMagick and netpbm open it.
 * There is no image codec in libs/, and vendoring one is a decision about the
 * tree, not about this feature: PNG is one header (stb_image, lodepng) away and
 * it drops in HERE -- paint_pam_read fills a PaintImage, and a second reader
 * filling the same struct is all a second format costs. Until that call is
 * made this reads its own PAM plus P6 PPM (what most tools mean by "raw" when
 * alpha is not wanted) and writes PAM only.
 *
 * REFUSED WITH A REASON, never silently: a header this cannot read says what it
 * found and what it takes, because "nothing happened" from an upload button is
 * the failure nobody can act on.
 *
 * Free functions on std types only, so they can be checked in a test that has
 * no runtime under it.
 */
struct PaintImage
{
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;       // w*h*4, the Pixels_ format
};

// A side longer than this is not a picture anyone paints on here; it is a
// header that lies, and the allocation it asks for is the harm.
static constexpr uint32_t PAINT_IMAGE_MAX_SIDE = 16384;

/*
 * HOW MANY BYTES A PICTURE MAY STILL TAKE, asked before any raster the size of
 * a page is allocated -- because in the browser the answer to asking too late
 * is not an exception but `Aborted(OOM)` and a dead tab. The wasm heap is
 * fixed at whatever INITIAL_MEMORY the loader was built with (loaders/Makefile
 * explains why growth is off), and the runtime's own arenas take most of it --
 * at 256 MB a resize that looked modest, 1024x768 to 1216x960 with two layers,
 * was the one that went over. Natively the heap is the OS's and a failed `new`
 * throws, so the answer is "as much as you like" and the guard reduces to the
 * side limit above.
 *
 * What is free is what the break has not reached plus what malloc has given
 * back (dlmalloc's mallinfo counts the top chunk in fordblks, so the sum is a
 * slight over-estimate, which is what the margin below is for).
 */
#if defined(__EMSCRIPTEN__)
static inline size_t paint_heap_headroom()
{
    const size_t brk = reinterpret_cast<size_t>(sbrk(0));
    const size_t top = emscripten_get_heap_max();
    const struct mallinfo mi = mallinfo();
    return (top > brk ? top - brk : 0) + static_cast<size_t>(mi.fordblks);
}
#else
static inline size_t paint_heap_headroom() { return SIZE_MAX; }
#endif

// Room for `bytes` more, keeping a margin for everything that is not a raster.
// `why` is set to a sentence a log can end with.
static inline bool paint_heap_can_take(size_t bytes, std::string& why)
{
    static constexpr size_t MARGIN = 8u << 20;
    const size_t room = paint_heap_headroom();
    if (room == SIZE_MAX || bytes + MARGIN <= room) return true;
    why = "needs " + std::to_string(bytes >> 20) + " MB and the page has "
        + std::to_string(room > MARGIN ? (room - MARGIN) >> 20 : 0) + " MB to spare";
    return false;
}

// The header tokenizer both formats share: words split on whitespace, `#` to
// end of line a comment (PPM says so, PAM allows it). Stops on the byte after
// the word, which is how the caller finds where the raster starts.
struct PaintPamCursor
{
    const uint8_t* p = nullptr;
    size_t n = 0;
    size_t i = 0;

    static bool ws(uint8_t c)
    {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
    }
    bool word(std::string& out)
    {
        for (;;)
        {
            while (i < n && ws(p[i])) ++i;
            if (i < n && p[i] == '#') { while (i < n && p[i] != '\n') ++i; continue; }
            break;
        }
        if (i >= n) return false;
        const size_t s = i;
        while (i < n && !ws(p[i])) ++i;
        out.assign(reinterpret_cast<const char*>(p + s), i - s);
        return true;
    }
};

static inline bool paint_pam_number(const std::string& t, uint32_t& v)
{
    if (t.empty() || t.size() > 9) return false;
    v = 0;
    for (char ch : t)
    {
        if (ch < '0' || ch > '9') return false;
        v = v * 10 + static_cast<uint32_t>(ch - '0');
    }
    return true;
}

static inline bool paint_pam_parse(const uint8_t* p, size_t n, PaintImage& out, std::string& why)
{
    PaintPamCursor c{ p, n, 0 };
    std::string tok;
    if (!c.word(tok)) { why = "empty file"; return false; }

    uint32_t w = 0, h = 0, depth = 0, maxval = 0;
    if (tok == "P6")
    {
        std::string a, b, m;
        if (!c.word(a) || !c.word(b) || !c.word(m)
         || !paint_pam_number(a, w) || !paint_pam_number(b, h) || !paint_pam_number(m, maxval))
        { why = "P6 header is not 'width height maxval'"; return false; }
        depth = 3;
    }
    else if (tok == "P7")
    {
        std::string tupl;
        for (;;)
        {
            if (!c.word(tok)) { why = "P7 header has no ENDHDR"; return false; }
            if (tok == "ENDHDR") break;
            std::string v;
            if (!c.word(v)) { why = "P7 header ends inside " + tok; return false; }
            bool ok = true;
            if      (tok == "WIDTH")    ok = paint_pam_number(v, w);
            else if (tok == "HEIGHT")   ok = paint_pam_number(v, h);
            else if (tok == "DEPTH")    ok = paint_pam_number(v, depth);
            else if (tok == "MAXVAL")   ok = paint_pam_number(v, maxval);
            else if (tok == "TUPLTYPE") tupl = v;
            else { why = "P7 header has an unknown field '" + tok + "'"; return false; }
            if (!ok) { why = "P7 " + tok + " is not a number: '" + v + "'"; return false; }
        }
        if (depth != 3 && depth != 4)
        { why = "DEPTH " + std::to_string(depth) + " -- only 3 (RGB) and 4 (RGB_ALPHA) are read"; return false; }
        if (!tupl.empty() && tupl != "RGB_ALPHA" && tupl != "RGB")
        { why = "TUPLTYPE " + tupl + " -- only RGB_ALPHA and RGB are read"; return false; }
        if (!tupl.empty() && ((tupl == "RGB_ALPHA") != (depth == 4)))
        { why = "TUPLTYPE " + tupl + " does not match DEPTH " + std::to_string(depth); return false; }
    }
    else
    {
        why = "magic '" + tok + "' -- accepted: P7 (PAM, TUPLTYPE RGB_ALPHA or RGB) and P6 (PPM)";
        return false;
    }

    if (maxval != 255)
    { why = "MAXVAL " + std::to_string(maxval) + " -- only 8 bits per channel (255) is read"; return false; }
    if (w == 0 || h == 0 || w > PAINT_IMAGE_MAX_SIDE || h > PAINT_IMAGE_MAX_SIDE)
    {
        why = "size " + std::to_string(w) + "x" + std::to_string(h) + " -- 1.."
            + std::to_string(PAINT_IMAGE_MAX_SIDE) + " on a side";
        return false;
    }
    // The raster, plus the layer it becomes (ImportImage copies it in).
    if (!paint_heap_can_take(static_cast<size_t>(w) * h * 8, why)) return false;

    // Exactly one whitespace byte between the header and the raster (both
    // formats say so), and it is the byte the cursor stopped on.
    size_t at = c.i;
    if (at >= n || !PaintPamCursor::ws(p[at])) { why = "no raster after the header"; return false; }
    ++at;
    const size_t need = static_cast<size_t>(w) * h * depth;
    if (n - at < need)
    {
        why = "raster is short: " + std::to_string(n - at) + " bytes for "
            + std::to_string(w) + "x" + std::to_string(h) + "x" + std::to_string(depth);
        return false;
    }

    out.w = w; out.h = h;
    out.rgba.resize(static_cast<size_t>(w) * h * 4);
    if (depth == 4)
        ::std::memcpy(out.rgba.data(), p + at, need);
    else
        for (size_t s = 0, d = 0; s < need; s += 3, d += 4)
        {
            out.rgba[d + 0] = p[at + s + 0];
            out.rgba[d + 1] = p[at + s + 1];
            out.rgba[d + 2] = p[at + s + 2];
            out.rgba[d + 3] = 255;
        }
    return true;
}

// Which decoder a file wants, from its first bytes rather than its name: a
// ".png" that is a JPEG is common enough to have a name for.
static inline bool paint_image_is_pam(const uint8_t* p, size_t n)
{ return n >= 2 && p[0] == 'P' && (p[1] == '7' || p[1] == '6'); }

/*
 * Any picture the tree can read, into the one in-memory shape (PaintImage).
 * PAM/PPM through this file's own parser; everything else through stb_image,
 * forced to four channels so a greyscale JPEG and an RGB PNG land as the same
 * RGBA the layers hold. The size guard is applied to both paths: a decoder that
 * will happily allocate what a lying header asks for is the harm, and stb's
 * own limit is generous.
 */
static inline bool paint_image_parse(const uint8_t* p, size_t n, PaintImage& out, std::string& why)
{
    if (paint_image_is_pam(p, n)) return paint_pam_parse(p, n, out, why);
    int w = 0, h = 0, comps = 0;
    if (!stbi_info_from_memory(p, static_cast<int>(n), &w, &h, &comps))
    {
        why = std::string("not a picture this reader knows (") + stbi_failure_reason()
            + "); accepted: PNG, JPEG, BMP, GIF, TGA, PAM (P7), PPM (P6)";
        return false;
    }
    if (w <= 0 || h <= 0 || static_cast<uint32_t>(w) > PAINT_IMAGE_MAX_SIDE
        || static_cast<uint32_t>(h) > PAINT_IMAGE_MAX_SIDE)
    { why = "refusing " + std::to_string(w) + "x" + std::to_string(h) + " -- a side over "
          + std::to_string(PAINT_IMAGE_MAX_SIDE) + " is not a picture anyone paints on here"; return false; }
    // stb's decode, the copy into `out`, and the layer it becomes: three rasters
    // at once at the peak, and asked for before the first is allocated.
    if (!paint_heap_can_take(static_cast<size_t>(w) * h * 12, why)) return false;
    unsigned char* px = stbi_load_from_memory(p, static_cast<int>(n), &w, &h, &comps, 4);
    if (!px) { why = std::string("decode failed: ") + stbi_failure_reason(); return false; }
    out.w = static_cast<uint32_t>(w); out.h = static_cast<uint32_t>(h);
    out.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
    stbi_image_free(px);
    return true;
}

/*
 * ── GIF, both ways ───────────────────────────────────────────────────────
 *
 * IN through stb_image, which already reads a GIF's frames and their delays
 * (stbi_load_gif_from_memory) -- an animated GIF is the one file format that
 * carries a frame sequence and a rate, so it is how frames arrive from
 * outside (PaintAnimation::ImportGif). OUT through the encoder below, because
 * stb_image_write has none and a frame sequence that can be brought in but
 * not taken out is half a feature.
 *
 * THE ENCODER IS THE SMALL ONE: one global 256-colour table for the whole
 * sequence, found by median cut over a sample of every frame's pixels, and
 * plain LZW at up to 12 bits, which is the format's own ceiling. No per-frame
 * tables, no transparency, no inter-frame difference: every frame is written
 * whole. That is more bytes than a good encoder writes and a fraction of the
 * code, and what a paint program's sprite sheet needs is the file to be a
 * correct GIF that every viewer plays, not a small one.
 */
namespace paint_gif {

struct Box { uint8_t lo[3], hi[3]; std::vector<uint32_t> px; };

// Median cut: split the widest axis of the box with the most pixels until
// there are `want` boxes; each box's average is a palette entry.
static inline void median_cut(std::vector<uint32_t> sample, size_t want, std::vector<uint32_t>& palette)
{
    palette.clear();
    if (sample.empty()) { palette.push_back(0); return; }
    std::vector<Box> boxes(1);
    boxes[0].px = std::move(sample);
    auto bounds = [](Box& b)
    {
        b.lo[0] = b.lo[1] = b.lo[2] = 255; b.hi[0] = b.hi[1] = b.hi[2] = 0;
        for (uint32_t c : b.px)
            for (int k = 0; k < 3; ++k)
            {
                const uint8_t v = static_cast<uint8_t>(c >> (16 - 8 * k));
                b.lo[k] = std::min(b.lo[k], v); b.hi[k] = std::max(b.hi[k], v);
            }
    };
    bounds(boxes[0]);
    while (boxes.size() < want)
    {
        // The box to split: the most pixels among those that can still split.
        size_t at = boxes.size(); size_t most = 1;
        for (size_t i = 0; i < boxes.size(); ++i)
        {
            const Box& b = boxes[i];
            const int span = std::max({ b.hi[0] - b.lo[0], b.hi[1] - b.lo[1], b.hi[2] - b.lo[2] });
            if (span > 0 && b.px.size() > most) { most = b.px.size(); at = i; }
        }
        if (at == boxes.size()) break;
        Box& b = boxes[at];
        int axis = 0; int span = b.hi[0] - b.lo[0];
        for (int k = 1; k < 3; ++k) if (b.hi[k] - b.lo[k] > span) { span = b.hi[k] - b.lo[k]; axis = k; }
        const int shift = 16 - 8 * axis;
        std::sort(b.px.begin(), b.px.end(), [shift](uint32_t a, uint32_t c)
                  { return ((a >> shift) & 255) < ((c >> shift) & 255); });
        Box other;
        const size_t mid = b.px.size() / 2;
        other.px.assign(b.px.begin() + mid, b.px.end());
        b.px.resize(mid);
        bounds(b); bounds(other);
        boxes.push_back(std::move(other));
    }
    for (const Box& b : boxes)
    {
        uint64_t r = 0, g = 0, bl = 0;
        for (uint32_t c : b.px) { r += (c >> 16) & 255; g += (c >> 8) & 255; bl += c & 255; }
        const size_t n = std::max<size_t>(1, b.px.size());
        palette.push_back((static_cast<uint32_t>(r / n) << 16) | (static_cast<uint32_t>(g / n) << 8)
                          | static_cast<uint32_t>(bl / n));
    }
}

// Nearest palette entry, cached on the top five bits of each channel: a
// 1024x768 frame asks this 786k times and the cache answers most of them.
struct Mapper
{
    const std::vector<uint32_t>& pal;
    std::vector<int16_t> cache;
    explicit Mapper(const std::vector<uint32_t>& p) : pal(p), cache(32 * 32 * 32, -1) {}
    uint8_t operator()(uint8_t r, uint8_t g, uint8_t b)
    {
        const size_t key = (static_cast<size_t>(r >> 3) << 10) | (static_cast<size_t>(g >> 3) << 5) | (b >> 3);
        if (cache[key] >= 0) return static_cast<uint8_t>(cache[key]);
        int best = 0; int64_t bd = INT64_MAX;
        for (size_t i = 0; i < pal.size(); ++i)
        {
            const int dr = static_cast<int>((pal[i] >> 16) & 255) - r;
            const int dg = static_cast<int>((pal[i] >> 8) & 255) - g;
            const int db = static_cast<int>(pal[i] & 255) - b;
            const int64_t d = static_cast<int64_t>(dr) * dr + static_cast<int64_t>(dg) * dg + static_cast<int64_t>(db) * db;
            if (d < bd) { bd = d; best = static_cast<int>(i); }
        }
        cache[key] = static_cast<int16_t>(best);
        return static_cast<uint8_t>(best);
    }
};

// LZW with a variable code width, the GIF flavour: clear and end codes after
// the alphabet, the table reset at 4096. Bits are packed least-significant
// first and the stream is cut into sub-blocks of at most 255 bytes.
struct LzwOut
{
    std::vector<uint8_t>& out;
    std::vector<uint8_t> block;
    uint32_t acc = 0; int bits = 0;
    explicit LzwOut(std::vector<uint8_t>& o) : out(o) {}
    void code(uint32_t c, int width)
    {
        acc |= c << bits; bits += width;
        while (bits >= 8) { byte(static_cast<uint8_t>(acc & 255)); acc >>= 8; bits -= 8; }
    }
    void byte(uint8_t b) { block.push_back(b); if (block.size() == 255) flush(); }
    void flush()
    {
        if (block.empty()) return;
        out.push_back(static_cast<uint8_t>(block.size()));
        out.insert(out.end(), block.begin(), block.end());
        block.clear();
    }
    void finish() { if (bits > 0) byte(static_cast<uint8_t>(acc & 255)); flush(); out.push_back(0); }
};

static inline void lzw_encode(const std::vector<uint8_t>& idx, std::vector<uint8_t>& out)
{
    constexpr int MIN = 8;
    constexpr uint32_t CLEAR = 1u << MIN, END = CLEAR + 1;
    out.push_back(MIN);
    LzwOut w(out);
    // prefix code * 256 + next byte -> code; 4096 * 256 entries of int16.
    std::vector<int16_t> table(4096 * 256);
    auto reset = [&]() { std::fill(table.begin(), table.end(), -1); };
    reset();
    uint32_t next = END + 1; int width = MIN + 1;
    w.code(CLEAR, width);
    if (idx.empty()) { w.code(END, width); w.finish(); return; }
    uint32_t prefix = idx[0];
    for (size_t i = 1; i < idx.size(); ++i)
    {
        const uint8_t c = idx[i];
        const size_t key = static_cast<size_t>(prefix) * 256 + c;
        if (table[key] >= 0) { prefix = static_cast<uint32_t>(table[key]); continue; }
        w.code(prefix, width);
        if (next < 4096)
        {
            table[key] = static_cast<int16_t>(next);
            if (next == (1u << width) && width < 12) ++width;
            ++next;
        }
        else
        {
            w.code(CLEAR, width);
            reset();
            next = END + 1; width = MIN + 1;
        }
        prefix = c;
    }
    w.code(prefix, width);
    w.code(END, width);
    w.finish();
}

// The whole file: frames of one size, a delay per frame in hundredths of a
// second, looping forever.
static inline bool encode(const std::vector<PaintImage>& frames, uint32_t delay_cs,
                          std::vector<uint8_t>& out, std::string& why)
{
    if (frames.empty()) { why = "no frames"; return false; }
    const uint32_t w = frames[0].w, h = frames[0].h;
    if (w == 0 || h == 0 || w > 65535 || h > 65535) { why = "bad frame size"; return false; }
    for (const PaintImage& f : frames)
        if (f.w != w || f.h != h || f.rgba.size() < static_cast<size_t>(w) * h * 4)
        { why = "frames differ in size"; return false; }

    // The palette, from every frame, sampled so a long sequence does not
    // cost a sort of every pixel it has.
    std::vector<uint32_t> sample;
    {
        const size_t total = static_cast<size_t>(w) * h * frames.size();
        const size_t step = std::max<size_t>(1, total / 65536);
        size_t k = 0;
        for (const PaintImage& f : frames)
            for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i, ++k)
                if (k % step == 0)
                {
                    const uint8_t* p = f.rgba.data() + i * 4;
                    sample.push_back((static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2]);
                }
    }
    std::vector<uint32_t> palette;
    median_cut(std::move(sample), 256, palette);
    Mapper map(palette);

    out.clear();
    auto u16 = [&](uint32_t v) { out.push_back(static_cast<uint8_t>(v & 255)); out.push_back(static_cast<uint8_t>((v >> 8) & 255)); };
    const char* sig = "GIF89a";
    out.insert(out.end(), sig, sig + 6);
    u16(w); u16(h);
    out.push_back(0xF7);            // global table, 8 bits per colour, 256 entries
    out.push_back(0); out.push_back(0);
    for (size_t i = 0; i < 256; ++i)
    {
        const uint32_t c = i < palette.size() ? palette[i] : 0;
        out.push_back(static_cast<uint8_t>((c >> 16) & 255));
        out.push_back(static_cast<uint8_t>((c >> 8) & 255));
        out.push_back(static_cast<uint8_t>(c & 255));
    }
    // Loop forever (the Netscape extension every viewer honours).
    const uint8_t loop[] = { 0x21, 0xFF, 0x0B, 'N','E','T','S','C','A','P','E','2','.','0', 0x03, 0x01, 0x00, 0x00, 0x00 };
    out.insert(out.end(), loop, loop + sizeof(loop));

    std::vector<uint8_t> idx(static_cast<size_t>(w) * h);
    for (const PaintImage& f : frames)
    {
        const uint8_t gce[] = { 0x21, 0xF9, 0x04, 0x00 };
        out.insert(out.end(), gce, gce + 4);
        u16(delay_cs); out.push_back(0); out.push_back(0);
        out.push_back(0x2C); u16(0); u16(0); u16(w); u16(h); out.push_back(0);
        for (size_t i = 0; i < idx.size(); ++i)
        {
            const uint8_t* p = f.rgba.data() + i * 4;
            idx[i] = map(p[0], p[1], p[2]);
        }
        lzw_encode(idx, out);
    }
    out.push_back(0x3B);
    return true;
}

// The frames of a GIF, and its rate as the mean delay in milliseconds. A
// still GIF is one frame; a file that is not a GIF at all is refused with the
// decoder's reason.
static inline bool decode(const uint8_t* bytes, size_t n, std::vector<PaintImage>& frames,
                          int& delay_ms, std::string& why)
{
    int* delays = nullptr; int w = 0, h = 0, z = 0, comp = 0;
    stbi_uc* px = stbi_load_gif_from_memory(bytes, static_cast<int>(n), &delays, &w, &h, &z, &comp, 4);
    if (!px) { why = std::string("decode failed: ") + stbi_failure_reason(); return false; }
    frames.clear();
    const size_t per = static_cast<size_t>(w) * h * 4;
    int64_t sum = 0;
    for (int i = 0; i < z; ++i)
    {
        PaintImage f; f.w = static_cast<uint32_t>(w); f.h = static_cast<uint32_t>(h);
        f.rgba.assign(px + per * i, px + per * (i + 1));
        frames.push_back(std::move(f));
        if (delays) sum += delays[i];
    }
    delay_ms = (z > 0 && sum > 0) ? static_cast<int>(sum / z) : 100;
    stbi_image_free(px);
    if (delays) stbi_image_free(delays);
    return !frames.empty();
}

} // namespace paint_gif

// How many frames a file has as a GIF: 0 for a file that is not one (by its
// magic, so nothing else is decoded), else the count. Decodes the whole GIF
// to answer, which is the price of stb's one entry point; a reel is decoded
// again on import, and a sprite sheet is small.
static inline size_t paint_gif_frames_in(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return 0;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 6 || std::memcmp(bytes.data(), "GIF8", 4) != 0) return 0;
    std::vector<PaintImage> frames; int delay = 0; std::string why;
    if (!paint_gif::decode(bytes.data(), bytes.size(), frames, delay, why)) return 0;
    return frames.size();
}

static inline bool paint_image_read(const std::string& path, PaintImage& out, std::string& why)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { why = "cannot open " + path; return false; }
    in.seekg(0, std::ios::end);
    const std::streamoff len = in.tellg();
    if (len < 0) { why = "cannot size " + path; return false; }
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(len));
    if (len > 0 && !in.read(reinterpret_cast<char*>(bytes.data()), len))
    { why = "short read on " + path; return false; }
    return paint_image_parse(bytes.data(), bytes.size(), out, why);
}

// By EXTENSION on the way out, because a file being written has no bytes to
// sniff and its name is the one thing the caller said. ".png" is PNG; anything
// else is PAM, which is what the export always was.
static inline bool paint_image_write(const std::string& path,
                                     const uint8_t* rgba, uint32_t w, uint32_t h, std::string& why);

static inline bool paint_pam_read(const std::string& path, PaintImage& out, std::string& why)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { why = "cannot open " + path; return false; }
    in.seekg(0, std::ios::end);
    const std::streamoff len = in.tellg();
    if (len < 0) { why = "cannot size " + path; return false; }
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(len));
    if (len > 0 && !in.read(reinterpret_cast<char*>(bytes.data()), len))
    { why = "short read on " + path; return false; }
    return paint_pam_parse(bytes.data(), bytes.size(), out, why);
}

// The file's bytes, in memory: the header and the raster, nothing else. Split
// from the writer so a PAM can go somewhere that is not a path -- a database
// row (PaintPages) -- and still be the same bytes the export produces, which
// is what lets one reader (paint_pam_parse) serve both.
static inline bool paint_pam_encode(const uint8_t* rgba, uint32_t w, uint32_t h,
                                    std::vector<uint8_t>& out, std::string& why)
{
    if (!rgba || w == 0 || h == 0) { why = "nothing to write"; return false; }
    const std::string header = "P7\nWIDTH " + std::to_string(w) + "\nHEIGHT " + std::to_string(h)
                             + "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
    const size_t raster = static_cast<size_t>(w) * h * 4;
    out.resize(header.size() + raster);
    ::std::memcpy(out.data(), header.data(), header.size());
    ::std::memcpy(out.data() + header.size(), rgba, raster);
    return true;
}

static inline bool paint_pam_write(const std::string& path,
                                   const uint8_t* rgba, uint32_t w, uint32_t h, std::string& why)
{
    std::vector<uint8_t> bytes;
    if (!paint_pam_encode(rgba, w, h, bytes, why)) return false;
    std::ofstream o(path, std::ios::binary | std::ios::trunc);
    if (!o) { why = "cannot open " + path + " for writing"; return false; }
    o.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!o) { why = "write to " + path + " failed"; return false; }
    return true;
}

static inline bool paint_image_write(const std::string& path,
                                     const uint8_t* rgba, uint32_t w, uint32_t h, std::string& why)
{
    const size_t dot = path.rfind('.');
    std::string ext = (dot == std::string::npos) ? "" : path.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext != "png") return paint_pam_write(path, rgba, w, h, why);
    if (!rgba || w == 0 || h == 0) { why = "nothing to write"; return false; }
    // Stride w*4: the layers are packed. Not to a memory buffer first -- the
    // file IS the destination on both substrates.
    if (!stbi_write_png(path.c_str(), static_cast<int>(w), static_cast<int>(h), 4, rgba,
                        static_cast<int>(w) * 4))
    { why = "PNG write to " + path + " failed"; return false; }
    return true;
}

// "photo", from "/uploads/photo.pam" -- what an imported layer is called. The
// directory is where it came from and the extension is how it was carried;
// neither is the picture's name.
static inline std::string paint_path_stem(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0) base.erase(dot);
    return base.empty() ? std::string("Image") : base;
}

#endif // PAINTPROVIDER_PAINTIMAGE_H__
