#include "worker.hpp"
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <memory>

namespace fff {
namespace {
using Microsoft::WRL::ComPtr;

CoverPixels decode(IWICImagingFactory* factory, const fs::path& root, CoverRequest request, std::stop_token stop) {
    CoverPixels result{std::move(request), 0, 0, {}};
    if (!factory) return result;
    const auto path = safe_existing(root, result.request.path, false);
    if (!path || stop.stop_requested()) return result;
    const auto handle = CreateFileW(io_path(*path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return result;
    const std::unique_ptr<void, decltype(&CloseHandle)> file(handle, CloseHandle);
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.get(), &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        info.nFileSizeHigh || info.nFileSizeLow > 24 * 1024 * 1024 || stop.stop_requested()) return result;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFileHandle(reinterpret_cast<ULONG_PTR>(file.get()), nullptr,
                                                   WICDecodeMetadataCacheOnDemand, &decoder))) return result;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (stop.stop_requested() || FAILED(decoder->GetFrame(0, &frame))) return result;
    UINT width = 0, height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || !width || !height ||
        width > 8192 || height > 8192 || width * height > 12'000'000) return result;
    const double ratio = std::min({1.0, 320.0 / width, 190.0 / height});
    const auto outWidth = std::max(1u, static_cast<UINT>(width * ratio));
    const auto outHeight = std::max(1u, static_cast<UINT>(height * ratio));
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(frame.Get(), outWidth, outHeight, WICBitmapInterpolationModeFant))) return result;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                     nullptr, 0, WICBitmapPaletteTypeCustom))) return result;
    const auto stride = outWidth * 4;
    const auto size = stride * outHeight;
    if (stop.stop_requested()) return result;
    result.bytes.resize(size);
    if (FAILED(converter->CopyPixels(nullptr, stride, size, result.bytes.data()))) {
        result.bytes.clear();
        return result;
    }
    result.width = outWidth;
    result.height = outHeight;
    return result;
}
}

Worker::Worker(HWND window, fs::path root) : window_(window), root_(std::move(root)),
    thread_([this](std::stop_token stop) { run(stop); }) {}

Worker::~Worker() {
    {
        std::scoped_lock lock(mutex_);
        thread_.request_stop();
    }
    cv_.notify_one();
    CancelSynchronousIo(thread_.native_handle());
}

std::uint64_t Worker::refresh(bool nested) {
    std::uint64_t id;
    {
        std::scoped_lock lock(mutex_);
        id = ++lastScanId_;
        pendingScan_ = ScanRequest{nested, id};
    }
    cv_.notify_one();
    return id;
}

void Worker::clear_cache() {
    {
        std::scoped_lock lock(mutex_);
        clearPending_ = true;
        skipCacheThrough_ = lastScanId_;
        requests_.clear();
    }
    cv_.notify_one();
}

void Worker::covers(std::vector<CoverRequest> requests) {
    {
        std::scoped_lock lock(mutex_);
        requests_ = std::move(requests);
    }
    cv_.notify_one();
}

std::vector<LibraryUpdate> Worker::take_library() {
    std::scoped_lock lock(mutex_);
    std::vector<LibraryUpdate> results;
    results.swap(libraryResults_);
    return results;
}

std::vector<CoverPixels> Worker::take_covers() {
    std::scoped_lock lock(mutex_);
    std::vector<CoverPixels> results;
    results.swap(coverResults_);
    return results;
}

void Worker::update(LibraryUpdate result) {
    {
        std::scoped_lock lock(mutex_);
        libraryResults_.push_back(std::move(result));
    }
    PostMessageW(window_, workerMessage, 0, 0);
}

void Worker::run(std::stop_token stop) {
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(initialized)) {
        struct ComScope { ~ComScope() { CoUninitialize(); } } com;
        ComPtr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
            factory.Reset();
        }
        try {
            const auto data = root_ / L"data";
            LibraryUpdate startup;
            startup.gamesReady = ensure_directory(root_, L"games");
            if (stop.stop_requested()) return;
            startup.dataReady = ensure_directory(root_, L"data");
            if (stop.stop_requested()) return;
            startup.runtimeReady = safe_existing(root_, L"runtime/ruffle.exe", false).has_value();
            if (stop.stop_requested()) return;
            startup.state = startup.dataReady ? load_state(data) : State{};
            const auto& prefs = startup.state->preferences;
            if (stop.stop_requested()) return;
            if (startup.dataReady) startup.cached = load_cache(data, prefs.scanNested);
            {
                std::scoped_lock lock(mutex_);
                if (prefs.autoScan && !pendingScan_) pendingScan_ = ScanRequest{prefs.scanNested, ++lastScanId_};
                startup.scanning = pendingScan_.has_value();
            }
            update(std::move(startup));

            while (!stop.stop_requested()) {
                std::optional<CoverRequest> cover;
                std::optional<ScanRequest> scan;
                bool clearing = false;
                {
                    std::unique_lock lock(mutex_);
                    cv_.wait(lock, [&] { return stop.stop_requested() || clearPending_ || pendingScan_ || !requests_.empty(); });
                    if (stop.stop_requested()) break;
                    if (clearPending_) {
                        clearPending_ = false;
                        clearing = true;
                    } else if (pendingScan_) {
                        scan.swap(pendingScan_);
                    } else {
                        cover = std::move(requests_.back());
                        requests_.pop_back();
                    }
                }
                if (clearing) {
                    LibraryUpdate result;
                    result.cacheCleared = clear_catalog_cache(root_);
                    update(std::move(result));
                    continue;
                }
                if (cover) {
                    auto pixels = decode(factory.Get(), root_, std::move(*cover), stop);
                    if (stop.stop_requested()) break;
                    {
                        std::scoped_lock lock(mutex_);
                        coverResults_.push_back(std::move(pixels));
                    }
                    PostMessageW(window_, coverMessage, 0, 0);
                    continue;
                }
                LibraryUpdate result;
                result.gamesReady = ensure_directory(root_, L"games");
                if (stop.stop_requested()) break;
                result.dataReady = ensure_directory(root_, L"data");
                if (stop.stop_requested()) break;
                result.runtimeReady = safe_existing(root_, L"runtime/ruffle.exe", false).has_value();
                if (stop.stop_requested()) break;
                const auto start = std::chrono::steady_clock::now();
                result.nested = scan->nested;
                result.scanId = scan->id;
                result.scanned = fff::scan(root_ / L"games", result.nested, stop);
                result.scanMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                if (stop.stop_requested()) break;
                bool writeCache;
                {
                    std::scoped_lock lock(mutex_);
                    writeCache = scan->id > skipCacheThrough_ && scan->id == lastScanId_;
                }
                if (result.scanned->complete && writeCache) {
                    const bool cacheReady = ensure_directory(root_, L"data/cache");
                    if (stop.stop_requested()) break;
                    result.cacheError = !cacheReady || !save_cache(data, result.scanned->games, result.nested);
                }
                update(std::move(result));
            }
            return;
        } catch (...) {
        }
    }
    LibraryUpdate failure;
    failure.failed = true;
    update(std::move(failure));
}
}
