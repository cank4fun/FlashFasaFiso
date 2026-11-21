#pragma once
#include "library.hpp"
#include <condition_variable>
#include <mutex>
#include <thread>

namespace fff {
constexpr UINT workerMessage = WM_APP + 1;
constexpr UINT coverMessage = WM_APP + 2;

struct CoverRequest {
    std::wstring path;
    unsigned generation = 0;
};

struct CoverPixels {
    CoverRequest request;
    unsigned width = 0, height = 0;
    std::vector<BYTE> bytes;
};

struct LibraryUpdate {
    std::optional<State> state;
    std::optional<std::vector<Game>> cached;
    std::optional<ScanResult> scanned;
    std::optional<bool> cacheCleared;
    bool scanning = false, nested = true;
    bool gamesReady = false, dataReady = false, runtimeReady = false, cacheError = false, failed = false;
    std::uint64_t scanId = 0;
    double scanMs = 0;
};

class Worker {
public:
    Worker(HWND window, fs::path root);
    ~Worker();
    std::uint64_t refresh(bool nested);
    void clear_cache();
    void covers(std::vector<CoverRequest> requests);
    std::vector<LibraryUpdate> take_library();
    std::vector<CoverPixels> take_covers();

private:
    struct ScanRequest { bool nested; std::uint64_t id; };
    void run(std::stop_token stop);
    void update(LibraryUpdate update);
    HWND window_;
    fs::path root_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<ScanRequest> pendingScan_;
    std::uint64_t lastScanId_ = 0, skipCacheThrough_ = 0;
    bool clearPending_ = false;
    std::vector<CoverRequest> requests_;
    std::vector<LibraryUpdate> libraryResults_;
    std::vector<CoverPixels> coverResults_;
    std::jthread thread_;
};
}
