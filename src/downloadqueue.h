#ifndef DOWNLOADQUEUE_H
#define DOWNLOADQUEUE_H

#include <string>
#include <utility>
#include <vector>

#include "ipfswrapper.h"   // IpfsWrapper::FetchTarget

// ---------------------------------------------------------------------------
// DownloadQueue — the single, CID-addressed content-fetch pump.
//
// Every batch download (game content, runner builds, covers) enqueues here instead of spawning its own thread-per-
// target. The queue keys jobs by CID and de-duplicates AT ENQUEUE against both in-flight jobs and already-seeded
// content, so a layer CID shared by several packages (or by a game and its runner) is fetched exactly ONCE and then
// materialized (hard-link / copy) to every destination that wants it. A single dispatcher drains the queue bounded by
// the existing DownloadSlot semaphore (MaxConcurrentDownloads), honouring per-CID priority. This replaces the old
// FetchTargetsConcurrent race surface (two workers writing the same file) with an explicit, controllable queue; the
// Go-side singleflight remains a primitive-level backstop for direct FetchToPath callers.
// ---------------------------------------------------------------------------
namespace IpfsWrapper {

// Opaque token naming the CIDs an Enqueue call added to (or joined in) the queue — each with THIS batch's own
// optional/required view (a CID one batch requires may be optional to another), so WaitBatch fails correctly per batch.
struct BatchHandle { std::vector<std::pair<std::string, bool>> Items; };   // {cid, optionalForThisBatch}

// Enqueue a batch of fetch targets. Dedups each by CID (merging destinations for cross-dest single-fetch) and skips
// anything already on disk / already seeded. Returns a handle to wait on. Non-blocking (the dispatcher does the work).
BatchHandle EnqueueBatch(const std::vector<FetchTarget> &Targets);

// Block until every job named by Handle is finished; returns false (with *Error) as soon as a REQUIRED job fails
// (optional-job failures are logged and tolerated) — same contract the old FetchTargetsConcurrent had.
bool WaitBatch(const BatchHandle &Handle, std::string *Error = nullptr);

// Cancel a CID's download: drop it if still queued, abort it (RequestCancel) if active. A batch waiting on a required
// cancelled CID sees the cancellation as a failure.
void CancelDownload(const std::string &Cid);

// Move a still-queued CID ahead of all other queued jobs so the dispatcher picks it next. No-op once it is active/done.
void PrioritizeDownload(const std::string &Cid);

} // namespace IpfsWrapper

#endif // DOWNLOADQUEUE_H
