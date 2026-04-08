/*
 *  Standalone benchmark for inspectrum's hot paths.
 *
 *  Exercises FFT, windowing+FFT+log-power (getLine), and the colormap
 *  pixel-conversion loop (getPixmapTile) without any GUI or input file.
 *
 *  Includes a built-in sampling profiler (Windows only) that suspends the
 *  benchmark thread at ~10 kHz, reads the instruction pointer, and resolves
 *  symbols via DbgHelp — the same technique VTune / Very Sleepy use.
 *  No admin privileges required (we sample our own process).
 *
 *  Build:  cmake --preset windows && cmake --build build --config RelWithDebInfo --target inspectrum_bench
 *  Run:    build\src\RelWithDebInfo\inspectrum_bench.exe [--fft-size N] [--iterations N] [--profile]
 */

#include "fft.h"
#include "util.h"

#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <numeric>
#include <random>
#include <vector>
#include <algorithm>

#ifdef _MSC_VER
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <atomic>
#include <thread>
#include <map>
#include <string>
#pragma comment(lib, "dbghelp.lib")
#endif

// ---------------------------------------------------------------------------
// Timing helpers
// ---------------------------------------------------------------------------
using Clock = std::chrono::high_resolution_clock;

struct BenchResult {
    double totalMs;
    double meanUs;
    double minUs;
    double maxUs;
    int    iterations;
};

template <typename Fn>
BenchResult bench(const char *label, int iterations, Fn &&fn)
{
    fn(); // warm-up

    std::vector<double> timings(iterations);
    auto wallStart = Clock::now();

    for (int i = 0; i < iterations; i++) {
        auto t0 = Clock::now();
        fn();
        auto t1 = Clock::now();
        timings[i] = std::chrono::duration<double, std::micro>(t1 - t0).count();
    }

    auto wallEnd = Clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(wallEnd - wallStart).count();
    double sum  = std::accumulate(timings.begin(), timings.end(), 0.0);
    double mean = sum / iterations;
    double mn   = *std::min_element(timings.begin(), timings.end());
    double mx   = *std::max_element(timings.begin(), timings.end());

    std::printf("  %-36s  %8.1f us/call   (min %7.1f  max %7.1f)   [%d iters, %.1f ms total]\n",
                label, mean, mn, mx, iterations, totalMs);

    return {totalMs, mean, mn, mx, iterations};
}

// ---------------------------------------------------------------------------
// Self-sampling profiler (Windows only)
// ---------------------------------------------------------------------------
#ifdef _MSC_VER

class Sampler {
public:
    Sampler() : running_(false), totalSamples_(0), failedSamples_(0) {}

    // Call from the thread you want to profile, before the workload starts.
    void start() {
        // Get a real handle to the calling thread (not the pseudo-handle).
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                        GetCurrentProcess(), &targetThread_,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                        FALSE, 0);

        totalSamples_ = 0;
        failedSamples_ = 0;
        samples_.clear();
        running_.store(true, std::memory_order_release);
        samplerThread_ = std::thread(&Sampler::sampleLoop, this);
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        if (samplerThread_.joinable())
            samplerThread_.join();
        CloseHandle(targetThread_);
    }

    void report(int topN = 30) {
        if (totalSamples_ == 0) {
            std::printf("  (no samples collected)\n");
            return;
        }

        // Initialise DbgHelp for symbol resolution.
        HANDLE proc = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        SymInitialize(proc, NULL, TRUE);

        // Aggregate samples by resolved function name.
        struct FuncInfo {
            std::string name;
            std::string module;
            std::string sourceFile;
            DWORD       sourceLine;
            int         count;
        };
        std::map<std::string, FuncInfo> byFunc;

        char symBuf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;

        IMAGEHLP_LINE64 lineInfo;
        lineInfo.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

        for (auto &kv : samples_) {
            DWORD64 addr = kv.first;
            int count    = kv.second;

            std::string funcName = "???";
            std::string modName  = "";
            std::string srcFile  = "";
            DWORD       srcLine  = 0;

            DWORD64 displacement64 = 0;
            if (SymFromAddr(proc, addr, &displacement64, sym)) {
                funcName = sym->Name;
            }

            // Module name
            HMODULE hMod = NULL;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(addr), &hMod);
            if (hMod) {
                char modPath[MAX_PATH];
                if (GetModuleFileNameA(hMod, modPath, MAX_PATH)) {
                    const char *slash = strrchr(modPath, '\\');
                    modName = slash ? slash + 1 : modPath;
                }
            }

            // Source line info
            DWORD displacement32 = 0;
            if (SymGetLineFromAddr64(proc, addr, &displacement32, &lineInfo)) {
                srcFile = lineInfo.FileName;
                srcLine = lineInfo.LineNumber;
                // Trim to just the filename
                const char *slash = strrchr(srcFile.c_str(), '\\');
                if (slash) srcFile = slash + 1;
            }

            // Key by function name + module to merge samples
            std::string key = modName + "!" + funcName;
            auto &fi = byFunc[key];
            fi.name = funcName;
            fi.module = modName;
            fi.count += count;
            if (fi.sourceFile.empty() && !srcFile.empty()) {
                fi.sourceFile = srcFile;
                fi.sourceLine = srcLine;
            }
        }

        SymCleanup(proc);

        // Sort by sample count descending.
        std::vector<FuncInfo> sorted;
        sorted.reserve(byFunc.size());
        for (auto &kv : byFunc)
            sorted.push_back(kv.second);
        std::sort(sorted.begin(), sorted.end(),
                  [](const FuncInfo &a, const FuncInfo &b) { return a.count > b.count; });

        // Print flat profile.
        std::printf("\n=== Sampling profile (%d samples, %d failed) ===\n\n",
                    totalSamples_, failedSamples_);
        std::printf("  %-6s  %-6s  %-40s  %-20s  %s\n",
                    "Samp", "%", "Function", "Module", "Source");
        std::printf("  %-6s  %-6s  %-40s  %-20s  %s\n",
                    "------", "------", "----------------------------------------",
                    "--------------------", "--------------------");

        int shown = 0;
        for (auto &fi : sorted) {
            if (shown >= topN) break;
            double pct = 100.0 * fi.count / totalSamples_;
            if (pct < 0.5 && shown > 10) break; // skip noise

            char srcBuf[128] = "";
            if (!fi.sourceFile.empty())
                std::snprintf(srcBuf, sizeof(srcBuf), "%s:%u", fi.sourceFile.c_str(), fi.sourceLine);

            std::printf("  %6d  %5.1f%%  %-40.40s  %-20.20s  %s\n",
                        fi.count, pct, fi.name.c_str(), fi.module.c_str(), srcBuf);
            shown++;
        }
        std::printf("\n");
    }

private:
    HANDLE              targetThread_;
    std::atomic<bool>   running_;
    std::thread         samplerThread_;
    std::map<DWORD64, int> samples_;
    int                 totalSamples_;
    int                 failedSamples_;

    void sampleLoop() {
        // Target: ~10 kHz sampling (100 us interval).
        // Use spin-wait with QPC for microsecond-accurate intervals.
        LARGE_INTEGER freq, next, now;
        QueryPerformanceFrequency(&freq);
        double ticksPerInterval = freq.QuadPart * 100.0 / 1000000.0; // 100 us

        QueryPerformanceCounter(&next);

        while (running_.load(std::memory_order_acquire)) {
            // Wait until next sample time.
            next.QuadPart += static_cast<LONGLONG>(ticksPerInterval);
            do {
                QueryPerformanceCounter(&now);
            } while (now.QuadPart < next.QuadPart);

            // Suspend target, read RIP, resume.
            if (SuspendThread(targetThread_) != (DWORD)-1) {
                CONTEXT ctx;
                ctx.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(targetThread_, &ctx)) {
                    samples_[ctx.Rip]++;
                    totalSamples_++;
                } else {
                    failedSamples_++;
                }
                ResumeThread(targetThread_);
            } else {
                failedSamples_++;
            }
        }
    }
};

#endif // _MSC_VER

// ---------------------------------------------------------------------------
// Generate synthetic IQ noise
// ---------------------------------------------------------------------------
static std::unique_ptr<std::complex<float>[]> makeSyntheticIQ(int n)
{
    auto buf = std::make_unique<std::complex<float>[]>(n);
    std::mt19937 rng(42);
    std::normal_distribution<float> dist(0.0f, 1.0f);
    for (int i = 0; i < n; i++)
        buf[i] = {dist(rng), dist(rng)};
    return buf;
}

// ---------------------------------------------------------------------------
// Fast log2 approximation (must match spectrogramplot.cpp)
// ---------------------------------------------------------------------------
static inline float fast_log2(float x)
{
    static_assert(sizeof(float) == sizeof(uint32_t), "float must be 32 bits");
    uint32_t i;
    std::memcpy(&i, &x, sizeof(i));
    return static_cast<float>(static_cast<int>(i >> 23) - 127)
         + static_cast<float>(i & 0x7FFFFFu) * (1.0f / 0x7FFFFFu);
}

// ---------------------------------------------------------------------------
// Extracted hot-path kernels (mirrors spectrogramplot.cpp logic)
// ---------------------------------------------------------------------------

static std::unique_ptr<float[]> makeHannWindow(int fftSize)
{
    auto w = std::make_unique<float[]>(fftSize);
    for (int i = 0; i < fftSize; i++)
        w[i] = 0.5f * (1.0f - cosf(static_cast<float>(Tau) * i / (fftSize - 1)));
    return w;
}

// Window + FFT + log-power  (mirrors SpectrogramPlot::getLine)
static void getLine(float *dest, const std::complex<float> *src,
                    const float *window, FFT &fft, int fftSize)
{
    auto buffer = std::make_unique<std::complex<float>[]>(fftSize);
    for (int i = 0; i < fftSize; i++)
        buffer[i] = src[i] * window[i];

    fft.process(reinterpret_cast<void*>(buffer.get()),
                reinterpret_cast<void*>(buffer.get()));

    const float invFFTSize = 1.0f / fftSize;
    const float logMultiplier = 10.0f / fast_log2(10.0f);
    for (int i = 0; i < fftSize; i++) {
        int k = i ^ (fftSize >> 1);
        auto s = buffer[k] * invFFTSize;
        float power = s.real() * s.real() + s.imag() * s.imag();
        *dest++ = fast_log2(power) * logMultiplier;
    }
}

// Colormap conversion (mirrors SpectrogramPlot::getPixmapTile inner loop)
static void colormapConvert(uint32_t *pixels, const float *fftTile,
                            const uint32_t *colormap, int fftSize,
                            int linesPerTile, float powerMax, float powerMin)
{
    float powerRange = -1.0f / std::abs(static_cast<int>(powerMin - powerMax));
    for (int y = 0; y < fftSize; y++) {
        for (int x = 0; x < linesPerTile; x++) {
            const float *fftLine = &fftTile[x * fftSize];
            float normPower = (fftLine[y] - powerMax) * powerRange;
            normPower = clamp(normPower, 0.0f, 1.0f);
            pixels[(fftSize - y - 1) * linesPerTile + x] =
                colormap[static_cast<uint8_t>(normPower * 255)];
        }
    }
}

// Full tile: N x getLine  (mirrors SpectrogramPlot::getFFTTile)
static void getFFTTile(float *dest, const std::complex<float> *samples,
                       int sampleCount, const float *window, FFT &fft,
                       int fftSize, int stride, int linesPerTile)
{
    float *ptr = dest;
    int offset = 0;
    for (int line = 0; line < linesPerTile; line++) {
        int srcOffset = offset;
        if (srcOffset + fftSize > sampleCount)
            srcOffset = sampleCount - fftSize;
        getLine(ptr, samples + srcOffset, window, fft, fftSize);
        ptr += fftSize;
        offset += stride;
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char *argv[])
{
    int  fftSize    = 2048;
    int  iterations = 500;
    bool profile    = false;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--fft-size") == 0 && i + 1 < argc)
            fftSize = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--iterations") == 0 && i + 1 < argc)
            iterations = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--profile") == 0)
            profile = true;
        else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("Usage: inspectrum_bench [--fft-size N] [--iterations N] [--profile]\n");
            std::printf("  --profile   Enable built-in sampling profiler (~10 kHz)\n");
            return 0;
        }
    }

    // Force power-of-two
    { int p = 1; while (p < fftSize) p <<= 1; fftSize = p; }

    const int tileSize     = 65536;
    const int linesPerTile = tileSize / fftSize;
    const int stride       = fftSize;
    const int totalSamples = linesPerTile * stride + fftSize;

    std::printf("inspectrum benchmark\n");
    std::printf("  FFT size:       %d\n", fftSize);
    std::printf("  Lines/tile:     %d\n", linesPerTile);
    std::printf("  Iterations:     %d\n", iterations);
    std::printf("  Tile samples:   %d\n", tileSize);
    std::printf("  Profiling:      %s\n", profile ? "ON" : "off (use --profile to enable)");
    std::printf("\n");

    // Setup
    auto samples = makeSyntheticIQ(totalSamples);
    auto window  = makeHannWindow(fftSize);
    FFT fft(fftSize);
    auto fftOutput = std::make_unique<float[]>(tileSize);
    auto pixels    = std::make_unique<uint32_t[]>(tileSize);

    // Build colormap
    uint32_t colormap[256];
    for (int i = 0; i < 256; i++) {
        float p = static_cast<float>(i) / 256;
        float h = p * 0.83f * 6.0f;
        float v = 1.0f - p;
        int hi = static_cast<int>(h) % 6;
        float f = h - static_cast<int>(h);
        float q = v * (1.0f - f);
        float t = v * f;
        float r, g, b;
        switch (hi) {
            case 0: r = v; g = t; b = 0; break;
            case 1: r = q; g = v; b = 0; break;
            case 2: r = 0; g = v; b = t; break;
            case 3: r = 0; g = q; b = v; break;
            case 4: r = t; g = 0; b = v; break;
            default: r = v; g = 0; b = q; break;
        }
        colormap[i] = (0xFFu << 24) |
                       (static_cast<uint8_t>(r * 255) << 16) |
                       (static_cast<uint8_t>(g * 255) << 8) |
                        static_cast<uint8_t>(b * 255);
    }

    float powerMax = 0.0f;
    float powerMin = -50.0f;

#ifdef _MSC_VER
    Sampler sampler;
    if (profile)
        sampler.start();
#endif

    // -----------------------------------------------------------------------
    std::printf("--- Kernel benchmarks ---\n");

    // 1. Raw FFT only
    {
        auto in  = std::make_unique<std::complex<float>[]>(fftSize);
        auto out = std::make_unique<std::complex<float>[]>(fftSize);
        std::memcpy(in.get(), samples.get(), fftSize * sizeof(std::complex<float>));
        bench("FFT::process (raw FFTW)", iterations * 10, [&]() {
            fft.process(out.get(), in.get());
        });
    }

    // 2. Window + FFT + log-power (getLine)
    {
        auto lineBuf = std::make_unique<float[]>(fftSize);
        bench("getLine (window+FFT+log)", iterations * 10, [&]() {
            getLine(lineBuf.get(), samples.get(), window.get(), fft, fftSize);
        });
    }

    // 3. Colormap conversion only (full tile)
    {
        for (int i = 0; i < tileSize; i++)
            fftOutput[i] = -25.0f + 20.0f * (static_cast<float>(i % 256) / 256.0f);
        bench("colormap (full tile)", iterations, [&]() {
            colormapConvert(pixels.get(), fftOutput.get(), colormap,
                            fftSize, linesPerTile, powerMax, powerMin);
        });
    }

    std::printf("\n--- Full-pipeline benchmarks ---\n");

    // 4. Full FFT tile
    bench("getFFTTile (all lines)", std::max(1, iterations / 10), [&]() {
        getFFTTile(fftOutput.get(), samples.get(), totalSamples,
                   window.get(), fft, fftSize, stride, linesPerTile);
    });

    // 5. Full tile + colormap
    bench("full tile + colormap", std::max(1, iterations / 10), [&]() {
        getFFTTile(fftOutput.get(), samples.get(), totalSamples,
                   window.get(), fft, fftSize, stride, linesPerTile);
        colormapConvert(pixels.get(), fftOutput.get(), colormap,
                        fftSize, linesPerTile, powerMax, powerMin);
    });

    // -----------------------------------------------------------------------
    std::printf("\n--- FFT size sweep ---\n");
    for (int sz : {256, 512, 1024, 2048, 4096, 8192, 16384}) {
        FFT fftSweep(sz);
        auto win = makeHannWindow(sz);
        auto lineBuf = std::make_unique<float[]>(sz);
        int sweepIters = std::max(10, 50000 / sz);

        char label[64];
        std::snprintf(label, sizeof(label), "getLine  fftSize=%d", sz);
        bench(label, sweepIters, [&]() {
            getLine(lineBuf.get(), samples.get(), win.get(), fftSweep, sz);
        });
    }

#ifdef _MSC_VER
    if (profile) {
        sampler.stop();
        sampler.report();
    }
#endif

    std::printf("Done.\n");
    return 0;
}
