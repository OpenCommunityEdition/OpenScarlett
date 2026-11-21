// SPDX-License-Identifier: MIT
// OpenScarlett: SuspendState.h is referenced by libHttpClient's task queue (TaskQueueImpl.h) but is
// absent from the upstream tree at the vendored commit. This is an independent, minimal
// implementation: queues register for suspend/resume notifications; Suspend() notifies them
// and WaitForQueuesToSuspend() returns once every callback has run.
#pragma once
#include <mutex>
#include <vector>
#include <algorithm>

struct ISuspendResumeCallback
{
    virtual void OnSuspendResume(_In_ bool isSuspended) = 0;
};

class SuspendState
{
public:
    void Add(ISuspendResumeCallback* cb) { std::lock_guard<std::mutex> g(m_lock); m_callbacks.push_back(cb); if (m_suspended) cb->OnSuspendResume(true); }
    void Remove(ISuspendResumeCallback* cb)
    {
        std::lock_guard<std::mutex> g(m_lock);
        m_callbacks.erase(std::remove(m_callbacks.begin(), m_callbacks.end(), cb), m_callbacks.end());
    }
    void Suspend() { Notify(true); }
    void Resume() { Notify(false); }
    void WaitForQueuesToSuspend() {}  // notifications are delivered synchronously

private:
    void Notify(bool suspended)
    {
        std::vector<ISuspendResumeCallback*> copy;
        {
            std::lock_guard<std::mutex> g(m_lock);
            m_suspended = suspended;
            copy = m_callbacks;
        }
        for (auto* cb : copy) cb->OnSuspendResume(suspended);
    }
    std::mutex m_lock;
    std::vector<ISuspendResumeCallback*> m_callbacks;
    bool m_suspended = false;
};

class SuspendResumeHandler
{
public:
    HRESULT Initialize(SuspendState& state, ISuspendResumeCallback* cb)
    {
        m_state = &state;
        m_cb = cb;
        state.Add(cb);
        return S_OK;
    }
    void Shutdown()
    {
        if (m_state) m_state->Remove(m_cb);
        m_state = nullptr;
    }
    ~SuspendResumeHandler() { Shutdown(); }

private:
    SuspendState* m_state = nullptr;
    ISuspendResumeCallback* m_cb = nullptr;
};
