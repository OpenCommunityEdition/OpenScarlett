// SPDX-License-Identifier: MIT
// Synthetic Series X|S title: user sign-in and game saves through the SDK's thunks.
#include <windows.h>

#include <XGameRuntimeInit.h>
#include <XGameSave.h>
#include <XTaskQueue.h>
#include <XUser.h>

#include <cstdio>
#include <cstring>
#include <string>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); g_failures++; } else std::printf("ok   %s\n", #cond); } while (0)

int main() {
    CHECK(SUCCEEDED(XGameRuntimeInitialize()));
    XTaskQueueHandle q = nullptr;
    XTaskQueueCreate(XTaskQueueDispatchMode::ThreadPool, XTaskQueueDispatchMode::Manual, &q);

    XAsyncBlock ab{};
    ab.queue = q;
    CHECK(SUCCEEDED(XUserAddAsync(XUserAddOptions::AddDefaultUserSilently, &ab)));
    for (int i = 0; i < 200 && XAsyncGetStatus(&ab, false) == E_PENDING; i++) XTaskQueueDispatch(q, XTaskQueuePort::Completion, 10);
    XUserHandle user = nullptr;
    CHECK(SUCCEEDED(XUserAddResult(&ab, &user)) && user);
    char tag[XUserGamertagComponentModernMaxBytes] = {};
    size_t used = 0;
    CHECK(SUCCEEDED(XUserGetGamertag(user, XUserGamertagComponent::Modern, sizeof tag, tag, &used)) && tag[0]);
    std::printf("gamertag: %s\n", tag);

    XGameSaveProviderHandle prov = nullptr;
    CHECK(SUCCEEDED(XGameSaveInitializeProvider(user, "00000000-0000-0000-0000-000000000000", false, &prov)));
    XGameSaveDeleteContainer(prov, "slot1");  // clean start

    XGameSaveContainerHandle cont = nullptr;
    CHECK(SUCCEEDED(XGameSaveCreateContainer(prov, "slot1", &cont)));
    XGameSaveUpdateHandle upd = nullptr;
    CHECK(SUCCEEDED(XGameSaveCreateUpdate(cont, "Slot One", &upd)));
    const char a[] = "hello save", b[] = "second blob data";
    CHECK(SUCCEEDED(XGameSaveSubmitBlobWrite(upd, "progress", (const uint8_t*)a, sizeof a)));
    CHECK(SUCCEEDED(XGameSaveSubmitBlobWrite(upd, "settings/audio", (const uint8_t*)b, sizeof b)));
    CHECK(SUCCEEDED(XGameSaveSubmitUpdate(upd)));
    XGameSaveCloseUpdate(upd);

    struct Info { int containers = 0; std::string display; uint32_t blobs = 0; } info;
    CHECK(SUCCEEDED(XGameSaveEnumerateContainerInfo(prov, &info, [](const XGameSaveContainerInfo* ci, void* c) {
        auto* i = (Info*)c;
        i->containers++;
        i->display = ci->displayName;
        i->blobs = ci->blobCount;
        return true;
    })));
    CHECK(info.containers == 1 && info.display == "Slot One" && info.blobs == 2);

    const char* names[] = {"progress", "settings/audio"};
    uint32_t count = 2;
    alignas(16) uint8_t buf[4096];
    CHECK(SUCCEEDED(XGameSaveReadBlobData(cont, names, &count, sizeof buf, (XGameSaveBlob*)buf)) && count == 2);
    auto* blobs = (XGameSaveBlob*)buf;
    CHECK(strcmp((const char*)blobs[0].data, a) == 0 && strcmp(blobs[0].info.name, "progress") == 0);
    CHECK(strcmp((const char*)blobs[1].data, b) == 0 && blobs[1].info.size == sizeof b);

    // Async read path.
    XAsyncBlock rb{};
    rb.queue = q;
    CHECK(SUCCEEDED(XGameSaveReadBlobDataAsync(cont, names, 2, &rb)));
    for (int i = 0; i < 200 && XAsyncGetStatus(&rb, false) == E_PENDING; i++) XTaskQueueDispatch(q, XTaskQueuePort::Completion, 10);
    uint32_t n2 = 0;
    alignas(16) uint8_t buf2[4096];
    CHECK(SUCCEEDED(XGameSaveReadBlobDataResult(&rb, sizeof buf2, (XGameSaveBlob*)buf2, &n2)) && n2 == 2);
    CHECK(strcmp((const char*)((XGameSaveBlob*)buf2)[1].data, b) == 0);

    XGameSaveCloseContainer(cont);
    CHECK(SUCCEEDED(XGameSaveDeleteContainer(prov, "slot1")));
    XGameSaveCloseProvider(prov);
    XUserCloseHandle(user);
    XTaskQueueCloseHandle(q);
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures;
}
