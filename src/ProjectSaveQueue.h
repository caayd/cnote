#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <QObject>

namespace cnote {

class Document;

// Outcome of one project write, reported on the GUI thread.
struct ProjectSaveResult {
    bool success = false;
    // "cannot write <path>" when the destination could not be replaced.
    std::string error;
};

// Writes project files - serialization, XPRESS compression and the atomic
// QSaveFile replacement - on one background thread, in submission order.  The
// GUI thread only takes the immutable Document snapshot and enqueues it, so a
// save no longer blocks input, paint or the session's autosave timer.
//
// Lives on the GUI thread: Enqueue, WaitForIdle and the completions all run
// there.  One instance per AppManager serializes the writes of every view and
// the autosave timer, so two saves can never race for the same destination.
class ProjectSaveQueue final : public QObject {
public:
    struct Job {
        // Snapshot taken on the GUI thread; the worker never reads the live
        // Document.  Rasters and embedded PDF bytes stay shared with it.
        std::shared_ptr<const Document> document;
        std::filesystem::path path;
    };
    using Completion = std::function<void(const ProjectSaveResult&)>;

    explicit ProjectSaveQueue(QObject* parent = nullptr);
    ~ProjectSaveQueue() override;
    ProjectSaveQueue(const ProjectSaveQueue&) = delete;
    ProjectSaveQueue& operator=(const ProjectSaveQueue&) = delete;

    // Queues `job`. `completion` runs on the GUI thread after the write: from
    // the queued delivery of a finished job, or inline from WaitForIdle.
    // Results run in write order, but a completion may spin a nested event
    // loop (a modal error dialog) whose WaitForIdle delivers a later result
    // before that completion returns.  Each completion's own session
    // bookkeeping is already applied when it is called.
    void Enqueue(Job job, Completion completion);

    // Waits until every queued job has been written and every completion has
    // run, so a close cannot outrun its own save.  May be called from inside a
    // completion (its nested event loop can reach a second SaveTo/close).
    void WaitForIdle();

    // Jobs queued, writing, or written but whose completion has not run yet.
    [[nodiscard]] std::size_t PendingCount() const;

private:
    struct PendingJob {
        Job job;
        Completion completion;
    };
    struct FinishedJob {
        ProjectSaveResult result;
        Completion completion;
    };

    void StartWorker();
    void WorkerLoop();
    void DeliverPending();

    mutable std::mutex mutex_;
    std::condition_variable workAvailable_;
    std::condition_variable idle_;
    std::deque<PendingJob> jobs_;
    std::deque<FinishedJob> finished_;
    // Jobs the worker has taken but not finished yet.
    std::size_t active_ = 0;
    bool stopping_ = false;
    bool workerStarted_ = false;
    std::thread worker_;
};

}  // namespace cnote
