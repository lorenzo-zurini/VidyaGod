// Parallel::For / ForWith: the loops the index build, hydration and the validator run on behave as the plain loops they
// replaced — an item's exception reaches the caller (never a terminate on a worker), and what the items log comes out
// in item order on the calling thread, up to the item that threw.
#include "vgtest.h"
#include "parallel.h"
#include "commonutils.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<int> LoggedItems(const std::vector<Diagnostics::Entry> &Said)
{
    std::vector<int> Out;
    for (const auto &E : Said) Out.push_back(std::stoi(E.Message));
    return Out;
}

} // namespace

// Teeth: drop the catch in Parallel::Run and this test aborts the whole binary; replay the logs as the items finish
// (or all of them) and the order or the cut-off at the thrower is wrong.
TEST(parallel_for_rethrows_on_the_caller_and_logs_in_item_order)
{
    SetLogEcho(false);
    LogRecorder Rec({ "ParallelTest" });
    bool Caught = false;
    try
    {
        Parallel::For(2000, [](std::size_t I) {
            LogWarn("ParallelTest", std::to_string(I));
            if (I == 1500) throw std::runtime_error("item 1500");
        });
    }
    catch (const std::runtime_error &E) { Caught = std::string(E.what()) == "item 1500"; }
    CHECK(Caught);
    const std::vector<int> Items = LoggedItems(Rec.Take());
    // Every item up to the thrower, each once and in order — what a single pass that stopped there would have said.
    CHECK(!Items.empty() && Items.back() == 1500);
    bool InOrder = true;
    for (std::size_t K = 0; K < Items.size(); ++K) InOrder = InOrder && Items[K] == static_cast<int>(K);
    CHECK(InOrder);
    CHECK_EQ(Items.size(), static_cast<std::size_t>(1501));
    SetLogEcho(true);
}

TEST(parallel_for_with_state_rethrows_and_keeps_order)
{
    SetLogEcho(false);
    struct Count { int Seen = 0; };
    LogRecorder Rec({ "ParallelTest" });
    bool Caught = false;
    try
    {
        Parallel::ForWith<Count>(300, [](Count &C, std::size_t I) {
            ++C.Seen;
            LogWarn("ParallelTest", std::to_string(I));
            if (I == 299) throw std::logic_error("last");
        });
    }
    catch (const std::logic_error &) { Caught = true; }
    CHECK(Caught);
    const std::vector<int> Items = LoggedItems(Rec.Take());
    CHECK_EQ(Items.size(), static_cast<std::size_t>(300));
    bool InOrder = true;
    for (std::size_t K = 0; K < Items.size(); ++K) InOrder = InOrder && Items[K] == static_cast<int>(K);
    CHECK(InOrder);
    SetLogEcho(true);
}
