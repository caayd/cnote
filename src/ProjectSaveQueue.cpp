#include "ProjectSaveQueue.h"

#include <QThread>

#include <utility>

#include "ProjectFile.h"
#include "Unicode.h"

namespace cnote {

namespace {

// The worker's whole job: serialize the snapshot (including its XPRESS
// compression) and replace the destination atomically.  ProjectFile::Save owns
// that contract; QFile/QSaveFile are safe off the GUI thread.
ProjectSaveResult WriteProjectFile(const Document& document, const std::filesystem::path& path) {
    ProjectSaveResult result;
    if (ProjectFile::Save(document, path)) {
        result.success = true;
        return result;
    }
    result.error = "cannot write " + PathToUtf8(path);
    return result;
}

}  // namespace

ProjectSaveQueue::ProjectSaveQueue(QObject* parent) : QObject(parent) {}

ProjectSaveQueue::~ProjectSaveQueue() {
    if (!workerStarted_) {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        // Jobs that were already requested still have to reach the disk: the
        // worker drains the queue and only then exits.
        stopping_ = true;
    }
    workAvailable_.notify_all();
    worker_.join();
}

void ProjectSaveQueue::Enqueue(Job job, Completion completion) {
    Q_ASSERT(thread() == QThread::currentThread());
    StartWorker();
    std::lock_guard lock(mutex_);
    jobs_.push_back({ std::move(job), std::move(completion) });
    workAvailable_.notify_one();
}

void ProjectSaveQueue::WaitForIdle() {
    Q_ASSERT(thread() == QThread::currentThread());
    Q_ASSERT(!delivering_);
    for (;;) {
        {
            std::unique_lock lock(mutex_);
            idle_.wait(lock, [this] { return jobs_.empty() && active_ == 0; });
            if (finished_.empty()) {
                return;
            }
        }
        // Completions run inline on this thread; they are the only way the
        // caller learns the write committed.
        DeliverPending();
    }
}

std::size_t ProjectSaveQueue::PendingCount() const {
    std::lock_guard lock(mutex_);
    return jobs_.size() + active_ + finished_.size();
}

void ProjectSaveQueue::StartWorker() {
    if (workerStarted_) {
        return;
    }
    workerStarted_ = true;
    worker_ = std::thread([this] { WorkerLoop(); });
}

void ProjectSaveQueue::WorkerLoop() {
    for (;;) {
        PendingJob pending;
        {
            std::unique_lock lock(mutex_);
            workAvailable_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_ && jobs_.empty()) {
                return;
            }
            pending = std::move(jobs_.front());
            jobs_.pop_front();
            ++active_;
        }

        ProjectSaveResult result = WriteProjectFile(*pending.job.document, pending.job.path);

        {
            std::lock_guard lock(mutex_);
            --active_;
            finished_.push_back({ std::move(result), std::move(pending.completion) });
            idle_.notify_all();
        }
        // Deliver on the queue's thread.  A destroyed queue drops the event
        // with its pending calls; the write itself has already committed.
        QMetaObject::invokeMethod(this, [this] { DeliverPending(); }, Qt::QueuedConnection);
    }
}

void ProjectSaveQueue::DeliverPending() {
    Q_ASSERT(thread() == QThread::currentThread());
    if (delivering_) {
        // Re-entered from a completion (a modal error dialog can spin the
        // event loop); the outer loop below picks the new results up.
        return;
    }
    delivering_ = true;
    for (;;) {
        std::vector<FinishedJob> ready;
        {
            std::lock_guard lock(mutex_);
            ready.swap(finished_);
        }
        if (ready.empty()) {
            break;
        }
        for (FinishedJob& finished : ready) {
            if (finished.completion) {
                finished.completion(finished.result);
            }
        }
    }
    delivering_ = false;
}

}  // namespace cnote
