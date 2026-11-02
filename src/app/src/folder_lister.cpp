#include <rv/app/folder_lister.hpp>

#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

namespace rv::app {
namespace {

constexpr std::chrono::milliseconds kStopBound{5000};

[[nodiscard]] ui::FolderBrowserFailure failureOf(const Error& error) noexcept {
    switch (error.code) {
    case ErrorCode::NotFound:
        return ui::FolderBrowserFailure::Missing;
    case ErrorCode::Unavailable:
        return ui::FolderBrowserFailure::Unreachable;
    default:
        return ui::FolderBrowserFailure::Unreadable;
    }
}

} // namespace

struct FolderLister::Box {
    struct Listed {
        std::uint64_t serial = 0;
        std::vector<ui::FolderRow> rows;
        std::optional<ui::FolderBrowserFailure> failure;
    };

    std::mutex mutex;
    std::condition_variable ended;
    int running = 0;
    std::function<void()> wake;
    std::optional<Listed> listed;
};

FolderLister::FolderLister(std::function<void()> wake) : m_box(std::make_shared<Box>()) {
    m_box->wake = std::move(wake);
}

FolderLister::~FolderLister() {
    std::unique_lock lock(m_box->mutex);
    if (!m_box->ended.wait_for(lock, kStopBound, [this] { return m_box->running == 0; })) {
        spdlog::warn("A folder listing did not end within {} ms; it is left to end by itself", kStopBound.count());
    }
    m_box->wake = nullptr;
}

void FolderLister::poll(ui::FolderBrowserModel& browser) {
    if (const std::optional<ui::FolderListingRequest> request = browser.takeRequest()) {
        m_waiting = true;
        m_serial = request->serial;
        {
            const std::lock_guard lock(m_box->mutex);
            ++m_box->running;
        }
        std::thread([box = m_box, request = *request] {
            try {
                Box::Listed answer;
                answer.serial = request.serial;
                const auto toRows = [&](const std::vector<library::ListedFolder>& folders) {
                    answer.rows.reserve(folders.size());
                    for (const library::ListedFolder& folder : folders) {
                        answer.rows.push_back(ui::FolderRow{folder.path, folder.name, folder.program});
                    }
                };
                if (request.folder.empty()) {
                    toRows(library::listDrives());
                } else if (const auto folders = request.programs ? library::listPrograms(request.folder)
                                                                 : library::listFolders(request.folder)) {
                    toRows(*folders);
                } else {
                    answer.failure = failureOf(folders.error());
                }
                const std::lock_guard lock(box->mutex);
                if (!box->listed || box->listed->serial < answer.serial) {
                    box->listed = std::move(answer);
                }
            } catch (const std::exception& error) {
                spdlog::error("A folder listing failed: {}", error.what());
            }
            // The wake under the lock: once the destructor cleared it, none is on its way to a host that is gone.
            const std::lock_guard lock(box->mutex);
            --box->running;
            if (box->wake) {
                box->wake();
            }
            box->ended.notify_all();
        }).detach();
    }
    std::optional<Box::Listed> listed;
    {
        const std::lock_guard lock(m_box->mutex);
        listed = std::exchange(m_box->listed, std::nullopt);
    }
    if (listed) {
        if (listed->serial == m_serial) {
            m_waiting = false;
        }
        if (listed->failure) {
            browser.setFailure(listed->serial, *listed->failure);
        } else {
            browser.setListing(listed->serial, std::move(listed->rows));
        }
    }
    if (!browser.isOpen()) {
        m_waiting = false;
    }
}

} // namespace rv::app
