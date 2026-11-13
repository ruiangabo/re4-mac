/* Texture packs for the macOS front end: replacements in Dolphin's naming (Load/Textures packs work as they
 * are, e.g. the RE4 HD Project), and a dump of every texture the game uploads in that naming.
 *
 *   tex1_<w>x<h>[_m]_<XXH64 of level 0, 16 hex>[_<XXH64 of the palette range used>]_<format>.png
 *
 * (Dolphin's TextureInfo: the hash covers level 0 in whole tiles; for paletted formats the palette entries
 * between the smallest and the largest index the texels use; "_$" in place of the palette hash matches any.)
 * The pictures may be larger than the original (HD): the renderer samples by normalized coordinates. */
#include <climits>
#include "texture_pack.h"
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <list>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <thread>
#include <pthread.h>
#include <sys/qos.h>
#include "gx_texture.h"

namespace re4tex {
namespace {

// ---- XXH64 (seed 0 here), as Dolphin hashes ----
constexpr uint64_t P1 = 11400714785074694791ULL, P2 = 14029467366897019727ULL, P3 = 1609587929392839161ULL,
                   P4 = 9650029242287828579ULL, P5 = 2870177450012600261ULL;
inline uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
inline uint64_t rd64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }  // (arm64: little endian)
inline uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint64_t round1(uint64_t acc, uint64_t in) { acc += in * P2; acc = rotl(acc, 31); return acc * P1; }
inline uint64_t merge(uint64_t acc, uint64_t v) { acc ^= round1(0, v); return acc * P1 + P4; }

uint64_t xxh64(const uint8_t* p, size_t len, uint64_t seed = 0) {
    const uint8_t* end = p + len;
    uint64_t h;
    if (len >= 32) {
        uint64_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        const uint8_t* limit = end - 32;
        do {
            v1 = round1(v1, rd64(p)); v2 = round1(v2, rd64(p + 8));
            v3 = round1(v3, rd64(p + 16)); v4 = round1(v4, rd64(p + 24));
            p += 32;
        } while (p <= limit);
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        h = merge(h, v1); h = merge(h, v2); h = merge(h, v3); h = merge(h, v4);
    } else {
        h = seed + P5;
    }
    h += (uint64_t) len;
    while (p + 8 <= end) { h ^= round1(0, rd64(p)); h = rotl(h, 27) * P1 + P4; p += 8; }
    if (p + 4 <= end) { h ^= (uint64_t) rd32(p) * P1; h = rotl(h, 23) * P2 + P3; p += 4; }
    while (p < end) { h ^= (*p) * P5; h = rotl(h, 11) * P1; p++; }
    h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3; h ^= h >> 32;
    return h;
}

// ---- names ----
struct Names {
    std::string base, baseOther, hash, tlut, fmt;  // base with / without "_m" as the texture has levels
    bool paletted = false;
};

Names namesFor(const re4gx::GxTexHookInfo& t) {
    Names n;
    char buf[64];
    snprintf(buf, sizeof buf, "tex1_%ux%u", t.width, t.height);
    const std::string plain = buf;
    n.base = t.levels > 1 ? plain + "_m" : plain;
    n.baseOther = t.levels > 1 ? plain : plain + "_m";
    snprintf(buf, sizeof buf, "%016llx", (unsigned long long) xxh64(t.data, t.bytes));
    n.hash = buf;
    n.fmt = std::to_string(t.format);
    if (re4gx::GxIsPaletted(t.format) && t.tlut) {
        n.paletted = true;
        unsigned lo = 0xFFFF, hi = 0;
        if (t.format == re4gx::kTexC4) {
            for (size_t i = 0; i < t.bytes; i++) {
                const unsigned a = t.data[i] >> 4, b = t.data[i] & 15;
                lo = std::min(lo, std::min(a, b)); hi = std::max(hi, std::max(a, b));
            }
        } else if (t.format == re4gx::kTexC8) {
            for (size_t i = 0; i < t.bytes; i++) { lo = std::min<unsigned>(lo, t.data[i]); hi = std::max<unsigned>(hi, t.data[i]); }
        } else {
            for (size_t i = 0; i + 1 < t.bytes; i += 2) {
                const unsigned v = ((t.data[i] << 8) | t.data[i + 1]) & 0x3FFF;
                lo = std::min(lo, v); hi = std::max(hi, v);
            }
        }
        uint64_t th = 0;
        if (lo <= hi && hi < t.tlutEntries) th = xxh64(t.tlut + 2 * lo, 2 * (hi + 1 - lo));
        snprintf(buf, sizeof buf, "_%016llx", (unsigned long long) th);
        n.tlut = buf;
    }
    return n;
}

// ---- PNG in and out (ImageIO) ----
unsigned maxSize = 0;  // 0: as the picture is; else its longer side at most this (scaled while decoding)
bool loadPng(const std::string& path, std::vector<uint8_t>* rgba, unsigned* w, unsigned* h) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8*) path.c_str(), (CFIndex) path.size(), false);
    if (!url) return false;
    CGImageSourceRef src = CGImageSourceCreateWithURL(url, nullptr);
    CFRelease(url);
    if (!src) return false;
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    CFRelease(src);
    if (!img) return false;
    *w = (unsigned) CGImageGetWidth(img); *h = (unsigned) CGImageGetHeight(img);
    if (maxSize && std::max(*w, *h) > maxSize) {
        const double k = (double) maxSize / std::max(*w, *h);
        *w = std::max(1u, (unsigned) (*w * k + 0.5)); *h = std::max(1u, (unsigned) (*h * k + 0.5));
    }
    rgba->assign((size_t) *w * *h * 4, 0);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(rgba->data(), *w, *h, 8, *w * 4, cs, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    if (!ctx) { CGImageRelease(img); return false; }
    CGContextSetBlendMode(ctx, kCGBlendModeCopy);
    CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
    CGContextDrawImage(ctx, CGRectMake(0, 0, *w, *h), img);
    CGContextRelease(ctx);
    CGImageRelease(img);
    // CoreGraphics only draws premultiplied: back to straight alpha, as the game's textures are
    for (size_t i = 0; i < rgba->size(); i += 4) {
        const unsigned a = (*rgba)[i + 3];
        if (a && a < 255)
            for (int c = 0; c < 3; c++) (*rgba)[i + c] = (uint8_t) std::min(255u, ((*rgba)[i + c] * 255u + a / 2) / a);
    }
    return true;
}

bool savePng(const std::string& path, const uint8_t* rgba, unsigned w, unsigned h) {
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CFDataRef data = CFDataCreate(nullptr, rgba, (CFIndex) w * h * 4);
    CGDataProviderRef prov = CGDataProviderCreateWithCFData(data);
    CGImageRef img = CGImageCreate(w, h, 8, 32, w * 4, cs, kCGImageAlphaLast | kCGBitmapByteOrder32Big, prov, nullptr, false,
                                   kCGRenderingIntentDefault);
    CGDataProviderRelease(prov); CFRelease(data); CGColorSpaceRelease(cs);
    if (!img) return false;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8*) path.c_str(), (CFIndex) path.size(), false);
    CFStringRef type = CFSTR("public.png");
    CGImageDestinationRef dst = url ? CGImageDestinationCreateWithURL(url, type, 1, nullptr) : nullptr;
    bool ok = false;
    if (dst) { CGImageDestinationAddImage(dst, img, nullptr); ok = CGImageDestinationFinalize(dst); CFRelease(dst); }
    if (url) CFRelease(url);
    CGImageRelease(img);
    return ok;
}

// ---- the pack ----
struct Loaded {
    std::vector<std::vector<uint8_t>> levels;
    std::vector<std::pair<unsigned, unsigned>> sizes;
    size_t bytes = 0;
    unsigned compressed = 0;  // as GxTexReplacement
};

// ---- BC1 / BC3 (DXT1 / DXT5), for the large pictures: 4-8x less to upload and to hold ----
inline uint16_t to565(int r, int g, int b) { return (uint16_t) (((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255)); }
inline void from565(uint16_t c, int* rgb) {
    const int r = c >> 11, g = (c >> 5) & 63, b = c & 31;
    rgb[0] = (r << 3) | (r >> 2); rgb[1] = (g << 2) | (g >> 4); rgb[2] = (b << 3) | (b >> 2);
}

// the colour half: the box of the block's colours along its main diagonal, a little inset; 4 colours
void colourBlock(const uint8_t (*px)[4], uint8_t* out) {
    int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0}, mean[3] = {0, 0, 0};
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < 3; c++) { lo[c] = std::min(lo[c], (int) px[i][c]); hi[c] = std::max(hi[c], (int) px[i][c]); mean[c] += px[i][c]; }
    for (int c = 0; c < 3; c++) mean[c] = (mean[c] + 8) / 16;
    // which way green and blue lean against red (the diagonal of the box the colours lie along)
    int cg = 0, cb = 0;
    for (int i = 0; i < 16; i++) {
        const int dr = px[i][0] - mean[0];
        cg += dr * (px[i][1] - mean[1]); cb += dr * (px[i][2] - mean[2]);
    }
    if (hi[0] == lo[0]) {  // no red spread: green against blue
        cb = 0;
        for (int i = 0; i < 16; i++) cb += (px[i][1] - mean[1]) * (px[i][2] - mean[2]);
    }
    int a[3] = {hi[0], hi[1], hi[2]}, b[3] = {lo[0], lo[1], lo[2]};
    if (cg < 0) std::swap(a[1], b[1]);
    if (cb < 0) std::swap(a[2], b[2]);
    for (int c = 0; c < 3; c++) { const int in = (a[c] - b[c]) / 16; a[c] -= in; b[c] += in; }
    uint16_t c0 = to565(a[0], a[1], a[2]), c1 = to565(b[0], b[1], b[2]);
    if (c0 < c1) std::swap(c0, c1);
    uint32_t idx = 0;
    if (c0 != c1) {
        int p[4][3];
        from565(c0, p[0]); from565(c1, p[1]);
        for (int c = 0; c < 3; c++) { p[2][c] = (2 * p[0][c] + p[1][c]) / 3; p[3][c] = (p[0][c] + 2 * p[1][c]) / 3; }
        for (int i = 0; i < 16; i++) {
            int best = 0, bestD = INT_MAX;
            for (int k = 0; k < 4; k++) {
                const int dr = px[i][0] - p[k][0], dg = px[i][1] - p[k][1], db = px[i][2] - p[k][2];
                const int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
                if (d < bestD) { bestD = d; best = k; }
            }
            idx |= (uint32_t) best << (2 * i);
        }
    }
    out[0] = c0 & 255; out[1] = c0 >> 8; out[2] = c1 & 255; out[3] = c1 >> 8;
    out[4] = idx & 255; out[5] = (idx >> 8) & 255; out[6] = (idx >> 16) & 255; out[7] = idx >> 24;
}

// the alpha half of BC3: the extremes and the 6 values between
void alphaBlock(const uint8_t (*px)[4], uint8_t* out) {
    int lo = 255, hi = 0;
    for (int i = 0; i < 16; i++) { lo = std::min(lo, (int) px[i][3]); hi = std::max(hi, (int) px[i][3]); }
    out[0] = (uint8_t) hi; out[1] = (uint8_t) lo;
    uint64_t bits = 0;
    if (hi != lo) {
        int v[8] = {hi, lo};
        for (int k = 1; k < 7; k++) v[k + 1] = ((7 - k) * hi + k * lo) / 7;
        for (int i = 0; i < 16; i++) {
            int best = 0, bestD = INT_MAX;
            for (int k = 0; k < 8; k++) { const int d = std::abs(px[i][3] - v[k]); if (d < bestD) { bestD = d; best = k; } }
            bits |= (uint64_t) best << (3 * i);
        }
    }
    for (int k = 0; k < 6; k++) out[2 + k] = (uint8_t) (bits >> (8 * k));
}

std::vector<uint8_t> compressLevel(const std::vector<uint8_t>& rgba, unsigned w, unsigned h, bool alpha) {
    const unsigned bw = (w + 3) / 4, bh = (h + 3) / 4, blockBytes = alpha ? 16 : 8;
    std::vector<uint8_t> out((size_t) bw * bh * blockBytes);
    uint8_t px[16][4];
    for (unsigned by = 0; by < bh; by++)
        for (unsigned bx = 0; bx < bw; bx++) {
            for (unsigned y = 0; y < 4; y++)
                for (unsigned x = 0; x < 4; x++) {
                    const unsigned sx = std::min(w - 1, bx * 4 + x), sy = std::min(h - 1, by * 4 + y);
                    memcpy(px[y * 4 + x], &rgba[((size_t) sy * w + sx) * 4], 4);
                }
            uint8_t* o = &out[((size_t) by * bw + bx) * blockBytes];
            if (alpha) { alphaBlock(px, o); colourBlock(px, o + 8); }
            else colourBlock(px, o);
        }
    return out;
}

// the levels of a large picture into BC1 (all opaque) or BC3
void compress(Loaded* l) {
    if (l->levels.empty()) return;
    const auto& top = l->levels[0];
    bool alpha = false;
    for (size_t i = 3; i < top.size() && !alpha; i += 4) alpha = top[i] != 255;
    l->bytes = 0;
    for (size_t k = 0; k < l->levels.size(); k++) {
        l->levels[k] = compressLevel(l->levels[k], l->sizes[k].first, l->sizes[k].second, alpha);
        l->bytes += l->levels[k].size();
    }
    l->compressed = alpha ? 3 : 1;
}

std::mutex lock;
struct Entry { std::string path; unsigned folder; };
std::unordered_map<std::string, Entry> index;  // name (no .png) -> picture; the earlier folder wins
std::unordered_map<std::string, std::list<std::pair<std::string, Loaded>>::iterator> cacheAt;
std::list<std::pair<std::string, Loaded>> cache;      // most recent first
size_t cacheBytes = 0;
constexpr size_t kCacheBudget = 384u << 20;
std::string dumpDir;
std::unordered_set<std::string> dumped;
unsigned replacedCount = 0;

// A mip chain by box filtering, as many levels as the original had plus those the larger picture adds.
void makeLevels(std::vector<uint8_t> rgba, unsigned w, unsigned h, unsigned origW, unsigned origLevels, Loaded* out) {
    unsigned extra = 0;
    for (unsigned s = w; s > origW && s > 1; s /= 2) extra++;
    const unsigned want = std::max(1u, origLevels) + extra;
    out->levels.clear(); out->sizes.clear(); out->bytes = 0;
    for (unsigned l = 0; l < want; l++) {
        out->bytes += rgba.size();
        out->sizes.push_back({w, h});
        out->levels.push_back(rgba);
        if (w == 1 && h == 1) break;
        const unsigned nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
        std::vector<uint8_t> next((size_t) nw * nh * 4);
        for (unsigned y = 0; y < nh; y++)
            for (unsigned x = 0; x < nw; x++)
                for (int c = 0; c < 4; c++) {
                    const unsigned x0 = std::min(w - 1, 2 * x), x1 = std::min(w - 1, 2 * x + 1);
                    const unsigned y0 = std::min(h - 1, 2 * y), y1 = std::min(h - 1, 2 * y + 1);
                    const unsigned s = rgba[((size_t) y0 * w + x0) * 4 + c] + rgba[((size_t) y0 * w + x1) * 4 + c] +
                                       rgba[((size_t) y1 * w + x0) * 4 + c] + rgba[((size_t) y1 * w + x1) * 4 + c];
                    next[((size_t) y * nw + x) * 4 + c] = (uint8_t) ((s + 2) / 4);
                }
        rgba.swap(next); w = nw; h = nh;
    }
}

// ---- reading in the background ----
// A picture not in the cache is read by the workers (the game keeps its original texture until then, no stall);
// several uploads waiting for the same picture all get it. RE4_TEX_SYNC=1: read while uploading, as before.
struct Waiter { uint64_t key; unsigned ticket; };
struct Job { std::string path; unsigned origW, origLevels, generation; };
std::deque<Job> jobs;
std::unordered_map<std::string, std::vector<Waiter>> inFlight;  // path -> the uploads waiting for it
std::deque<re4gx::GxTexReady> ready;
std::atomic<unsigned> readyCount{0};
std::condition_variable wake;
unsigned generation = 0;  // Setup again: what was being read for the old folders is dropped
bool workersUp = false, syncReads = false;
std::unordered_set<std::string> unreadable;

void putInCache(const std::string& path, Loaded&& l) {
    cacheBytes += l.bytes;
    cache.emplace_front(path, std::move(l));
    cacheAt[path] = cache.begin();
    while (cacheBytes > kCacheBudget && cache.size() > 1) {
        cacheBytes -= cache.back().second.bytes;
        cacheAt.erase(cache.back().first);
        cache.pop_back();
    }
}

bool readPicture(const std::string& path, unsigned origW, unsigned origLevels, Loaded* l) {
    std::vector<uint8_t> rgba; unsigned w = 0, h = 0;
    if (!loadPng(path, &rgba, &w, &h) || !w || !h) return false;
    makeLevels(std::move(rgba), w, h, origW, origLevels, l);
    // the large ones compressed (the small, the HUD's and the fonts', stay exact)
    if (re4gx::gGxTextureBc && (size_t) w * h >= 512u * 512u) compress(l);
    return true;
}

void worker() {
    // the efficiency cores: the game's own process (Rosetta) keeps the performance ones
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> g(lock);
            wake.wait(g, [] { return !jobs.empty(); });
            j = std::move(jobs.front());
            jobs.pop_front();
        }
        Loaded l;
        const bool ok = readPicture(j.path, j.origW, j.origLevels, &l);
        std::lock_guard<std::mutex> g(lock);
        auto w = inFlight.find(j.path);
        std::vector<Waiter> waiters;
        if (w != inFlight.end()) { waiters = std::move(w->second); inFlight.erase(w); }
        if (j.generation != generation) continue;
        if (!ok) { fprintf(stderr, "[tex] cannot read %s\n", j.path.c_str()); unreadable.insert(j.path); continue; }
        for (const Waiter& x : waiters) {
            re4gx::GxTexReady r;
            r.key = x.key; r.ticket = x.ticket;
            r.rep.levels = l.levels; r.rep.sizes = l.sizes; r.rep.compressed = l.compressed;
            ready.push_back(std::move(r));
        }
        readyCount = (unsigned) ready.size();
        if (replacedCount++ < 20) printf("[tex] replaced %s\n", std::filesystem::path(j.path).filename().c_str());
        putInCache(j.path, std::move(l));
    }
}

bool takeReady(re4gx::GxTexReady* out) {
    if (!readyCount.load(std::memory_order_relaxed)) return false;
    std::lock_guard<std::mutex> g(lock);
    if (ready.empty()) return false;
    *out = std::move(ready.front());
    ready.pop_front();
    readyCount = (unsigned) ready.size();
    return true;
}

bool replace(const re4gx::GxTexHookInfo& t, re4gx::GxTexReplacement* out) {
    if (!t.data || !t.bytes) return false;
    const Names n = namesFor(t);
    std::lock_guard<std::mutex> g(lock);
    if (index.empty()) return false;
    // the exact name or the any-palette one, whichever comes from the earlier folder (the button icons over a pack)
    std::string found;
    unsigned best = UINT_MAX;
    for (const std::string* base : {&n.base, &n.baseOther}) {
        std::string names[2] = {*base + "_" + n.hash + n.tlut + "_" + n.fmt, *base + "_" + n.hash + "_$_" + n.fmt};
        for (int k = 0; k < (n.paletted ? 2 : 1); k++) {
            auto it = index.find(names[k]);
            if (it != index.end() && it->second.folder < best) { best = it->second.folder; found = it->second.path; }
        }
        if (!found.empty()) break;
    }
    if (found.empty() || unreadable.count(found)) return false;
    auto c = cacheAt.find(found);
    if (c == cacheAt.end() && syncReads) {
        Loaded l;
        if (!readPicture(found, t.width, t.levels, &l)) { fprintf(stderr, "[tex] cannot read %s\n", found.c_str()); unreadable.insert(found); return false; }
        if (replacedCount++ < 20) printf("[tex] replaced %s\n", std::filesystem::path(found).filename().c_str());
        putInCache(found, std::move(l));
        c = cacheAt.find(found);
    }
    if (c == cacheAt.end()) {
        if (!workersUp) {
            workersUp = true;
            for (int i = 0; i < 3; i++) std::thread(worker).detach();
        }
        auto& w = inFlight[found];
        if (w.empty()) { jobs.push_back(Job{found, t.width, t.levels, generation}); wake.notify_one(); }
        w.push_back(Waiter{t.key, t.ticket});
        out->pending = true;
        return false;
    }
    cache.splice(cache.begin(), cache, c->second);
    out->levels = c->second->second.levels;
    out->sizes = c->second->second.sizes;
    out->compressed = c->second->second.compressed;
    return true;
}

void dump(const re4gx::GxTexHookInfo& t, const uint8_t* rgba) {
    const Names n = namesFor(t);
    const std::string name = n.base + "_" + n.hash + n.tlut + "_" + n.fmt;
    if (!dumped.insert(name).second) return;
    savePng(dumpDir + "/" + name + ".png", rgba, t.width, t.height);
}

}  // namespace

unsigned Setup(const std::vector<std::string>& folders, const std::string& dumpFolder, unsigned maxTextureSize) {
    std::lock_guard<std::mutex> g(lock);
    maxSize = maxTextureSize;
    index.clear(); cache.clear(); cacheAt.clear(); cacheBytes = 0;
    generation++; jobs.clear(); inFlight.clear(); ready.clear(); readyCount = 0; unreadable.clear();
    syncReads = getenv("RE4_TEX_SYNC") && *getenv("RE4_TEX_SYNC") == '1';
    unsigned folder = 0;
    for (const std::string& f : folders) {
        folder++;
        std::error_code ec;
        if (f.empty() || !std::filesystem::is_directory(f, ec)) continue;
        unsigned n = 0;
        for (auto it = std::filesystem::recursive_directory_iterator(f, std::filesystem::directory_options::follow_directory_symlink, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            const auto& p = it->path();
            std::string ext = p.extension().string();
            for (char& ch : ext) ch = (char) tolower((unsigned char) ch);
            if (ext != ".png") continue;
            const std::string stem = p.stem().string();
            if (stem.rfind("tex1_", 0) != 0) continue;
            index.emplace(stem, Entry{p.string(), folder});
            n++;
        }
        printf("[tex] %u replacements in %s\n", n, f.c_str());
    }
    re4gx::gGxTextureReplace = index.empty() ? nullptr : replace;
    re4gx::gGxTextureReady = index.empty() ? nullptr : takeReady;
    dumpDir = dumpFolder;
    if (!dumpDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dumpDir, ec);
        printf("[tex] dumping textures to %s\n", dumpDir.c_str());
    }
    re4gx::gGxTextureDump = dumpDir.empty() ? nullptr : dump;
    return (unsigned) index.size();
}

std::string NameFor(const re4gx::GxTexHookInfo& t) {
    const Names n = namesFor(t);
    return n.base + "_" + n.hash + n.tlut + "_" + n.fmt;
}

bool SelfTest() {
    // XXH64 reference values
    return xxh64((const uint8_t*) "", 0) == 0xEF46DB3751D8E999ULL && xxh64((const uint8_t*) "a", 1) == 0xD24EC4F1A98C6E5BULL;
}

}  // namespace re4tex
