#ifndef PARALLEL_H
#define PARALLEL_H

#include "commonutils.h"   // LogBuffer — what the items log comes out in item order, on the calling thread

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

// ParallelFor(N, Fn) — Fn(I) for every I in [0, N), spread over the machine's cores, returning when all are done.
// For independent per-item work over read-only shared state (each I writes only its own slot): the node index's
// parse, hash and fold steps. Items are handed out one at a time, so uneven items balance themselves; a small N
// runs on the calling thread alone.
//
// It behaves as the plain loop it replaces: what an item logs is kept and logged, in item order, on the calling
// thread once all are done (so the log reads as one pass would have written it, and a LogRecorder there sees it); and
// an exception an item throws stops the hand-out and is rethrown on the calling thread after every thread has
// finished (the first one, by item) — never a terminate from a worker.
namespace Parallel
{

inline unsigned Threads(std::size_t N)
{
    const unsigned Hw = std::max(1u, std::thread::hardware_concurrency());
    return static_cast<unsigned>(std::min<std::size_t>({ N, std::size_t(Hw), std::size_t(16) }));
}

// The shared run: T threads (this one included) each make a State, then take items until none are left or one threw.
template <typename State, typename Fn>
void Run(std::size_t N, unsigned T, Fn &&F)
{
    std::atomic<std::size_t> Next{ 0 };
    std::atomic<bool> Stop{ false };
    std::vector<std::vector<Diagnostics::Entry>> Logs(N);
    std::mutex ErrMu;
    std::size_t ErrAt = N;
    std::exception_ptr Err;
    auto Work = [&] {
        State S;
        for (std::size_t I; !Stop && (I = Next.fetch_add(1)) < N;)
        {
            LogBuffer Said;
            try { F(S, I); }
            catch (...)
            {
                std::lock_guard<std::mutex> Lk(ErrMu);
                if (I < ErrAt) { ErrAt = I; Err = std::current_exception(); }
                Stop = true;
            }
            Logs[I] = Said.Take();
        }
    };
    std::vector<std::thread> Pool;
    for (unsigned K = 1; K < T; ++K)
    {
        try { Pool.emplace_back(Work); }
        catch (const std::system_error &) { break; }   // no more threads to be had: the started ones (and this) do it all
    }
    Work();
    for (auto &Th : Pool) Th.join();
    for (std::size_t I = 0; I < N && I <= ErrAt; ++I) LogReplay(Logs[I]);   // a single pass stops at the item that threw
    if (Err) std::rethrow_exception(Err);
}

struct NoState {};

template <typename Fn>
void For(std::size_t N, Fn &&F)
{
    Run<NoState>(N, N < 64 ? 1u : Threads(N), [&](NoState &, std::size_t I) { F(I); });
}

// ParallelFor with a State per thread: Fn(State &, I), where State (default-constructed once per thread) holds what a
// thread may keep across its items — read caches — so nothing is shared but the items' own slots.
template <typename State, typename Fn>
void ForWith(std::size_t N, Fn &&F)
{
    Run<State>(N, N < 2 ? 1u : Threads(N), F);
}

} // namespace Parallel

#endif // PARALLEL_H
