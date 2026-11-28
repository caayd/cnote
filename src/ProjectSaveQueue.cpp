#include "ProjectSaveQueue.h"

#include <QThread>

#include <exception>
#include <new>
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
    try {
        if (ProjectFile::Save(document, path)) {
            result.success = true;
            return result;
        }
        result.error = "cannot write " + PathToUtf8(path);
    } catch (const std::bad_alloc&) {
        // Serialization allocates multiples of the document size.  The atomic
        // QSaveFile still holds the previous destination, and the failure
        // reaches the same error path as an I/O failure instead of terminating
        // the process on this thread.
        result.error = "cannot write " + PathToUtf8(path) + ": out of memory";
    } catch (const std::exception& error) {
        result.error = "cannot write " + PathToUtf8(path) + ": " + error.what();
    } catch (...) {
        result.error = "cannot write " + PathToUtf8(path);
    }
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
    // One result at a time, not a batch: a completion can run a nested event
    // loop (ReportError shows a modal dialog on a failed save), whose
    // WaitForIdle or queued invocation re-enters here.  Every entry pops the
    // next written result, so the nested call makes progress instead of
    // waiting on a delivery the outer call has not finished.
    for (;;) {
        FinishedJob finished;
        {
            std::lock_guard lock(mutex_);
            if (finished_.empty()) {
                return;
            }
            finished = std::move(finished_.front());
            finished_.pop_front();
        }
        if (finished.completion) {
            finished.completion(finished.result);
        }
    }
}

}  // namespace cnote
