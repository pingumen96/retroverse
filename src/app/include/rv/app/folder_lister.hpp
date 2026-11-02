#pragma once

// GPU-free (part of rv_app_catalog): no SDL, OpenGL or RmlUi header is included.
//
// The listings of the emulators screen's folder browser (ADR-0018 §5), on a thread each, as the wizard's are: a slow or
// vanished volume never blocks a frame, and an answer that comes late is dropped by the model's serial. The threads hold
// only a shared box; the destructor waits for them within a bound and leaves one a dead volume holds to end by itself.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

#include <rv/core/expected.hpp>
#include <rv/library/folder_listing.hpp>
#include <rv/ui/folder_browser_model.hpp>

namespace rv::app {

class FolderLister {
public:
    /// `wake`: called on a job's thread once it answered (thread-safe), never once the destructor returned.
    explicit FolderLister(std::function<void()> wake);
    ~FolderLister();
    FolderLister(const FolderLister&) = delete;
    FolderLister& operator=(const FolderLister&) = delete;

    /// Once per frame and per wake: the listing `browser` asks is started, the latest answer given to it.
    void poll(ui::FolderBrowserModel& browser);
    /// A listing is awaited (lockstep runs wait for it).
    [[nodiscard]] bool busy() const noexcept { return m_waiting; }

private:
    struct Box;

    std::shared_ptr<Box> m_box;
    bool m_waiting = false;
    std::uint64_t m_serial = 0;
};

} // namespace rv::app
