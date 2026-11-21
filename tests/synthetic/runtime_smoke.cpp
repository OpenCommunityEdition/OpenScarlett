// SPDX-License-Identifier: MIT
// Synthetic Series X|S title: exercises the Gaming Runtime through the SDK's own
// xgameruntime.lib thunks. Exit code 0 = every check passed; failures are printed.
#include <windows.h>

#include <XAsync.h>
#include <XGame.h>
#include <XGameRuntimeInit.h>
#include <XPackage.h>
#include <XSystem.h>
#include <XTaskQueue.h>

#include <cstdio>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); g_failures++; } else std::printf("ok   %s\n", #cond); } while (0)

int main() {
    CHECK(SUCCEEDED(XGameRuntimeInitialize()));

    XSystemDeviceType dt = XSystemGetDeviceType();
    std::printf("device type %u\n", (unsigned)dt);
    CHECK(dt == XSystemDeviceType::XboxScarlettLockhart || dt == XSystemDeviceType::XboxScarlettAnaconda);

    char id[XSystemConsoleIdBytes] = {};
    size_t used = 0;
    CHECK(SUCCEEDED(XSystemGetConsoleId(sizeof id, id, &used)) && used > 1);

    uint32_t title = 0;
    CHECK(SUCCEEDED(XGameGetXboxTitleId(&title)));
    CHECK(XPackageIsPackagedProcess());

    // Task queue with a thread-pool work port and a manual completion port.
    XTaskQueueHandle q = nullptr;
    CHECK(SUCCEEDED(XTaskQueueCreate(XTaskQueueDispatchMode::ThreadPool, XTaskQueueDispatchMode::Manual, &q)));

    // XAsyncRun: work runs on the pool, completion is dispatched manually below.
    struct Ctx { volatile LONG ran = 0, completed = 0; } ctx;
    XAsyncBlock block{};
    block.queue = q;
    block.context = &ctx;
    block.callback = [](XAsyncBlock* b) { InterlockedExchange(&((Ctx*)b->context)->completed, 1); };
    CHECK(SUCCEEDED(XAsyncRun(&block, [](XAsyncBlock* b) -> HRESULT {
        InterlockedExchange(&((Ctx*)b->context)->ran, 1);
        return S_OK;
    })));
    for (int i = 0; i < 200 && !ctx.completed; i++) {
        XTaskQueueDispatch(q, XTaskQueuePort::Completion, 10);
    }
    CHECK(ctx.ran == 1);
    CHECK(ctx.completed == 1);
    CHECK(XAsyncGetStatus(&block, false) == S_OK);

    // Delayed callback on the work port.
    volatile LONG fired = 0;
    CHECK(SUCCEEDED(XTaskQueueSubmitDelayedCallback(q, XTaskQueuePort::Work, 20, (void*)&fired,
                                                    [](void* c, bool) { InterlockedExchange((volatile LONG*)c, 1); })));
    for (int i = 0; i < 100 && !fired; i++) Sleep(5);
    CHECK(fired == 1);

    XTaskQueueCloseHandle(q);
    XGameRuntimeUninitialize();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures;
}
