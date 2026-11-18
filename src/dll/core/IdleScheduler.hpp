#pragma once

// Runs work on the game thread from cIGZFrameWork's idle loop.
//
// The scheduler is a cIGZSystemService. It is on the framework's OnIdle list only while it
// has work, so it costs nothing when idle. Each idle tick it runs every task posted before the
// tick started, then every recurring step once. Work posted during a tick runs on the next.
//
// Threads: Post may be called from any thread (the framework guards its idle list with a
// critical section). AddRecurring and Cancel are game thread only. Tasks always run on the
// game thread.

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "cIGZSystemService.h"

class cIGZFrameWork;

class IdleScheduler final : public cIGZSystemService
{
public:
    using Task = std::function<void()>;
    // Runs once per idle tick until it returns false or is cancelled.
    using Step = std::function<bool()>;

    IdleScheduler();
    ~IdleScheduler();

    // Adds the service to the framework. Call once at PostAppInit.
    bool Initialize(cIGZFrameWork* framework);
    // Drops all work, leaves the idle list and removes the service. Call at PostAppShutdown.
    void Terminate();

    // Runs task once on the next idle tick.
    void Post(Task task);
    // Runs step on every idle tick until it returns false. Returns an id for Cancel, never 0.
    // The step is stored once, so a running recurring step does not allocate per tick.
    uint32_t AddRecurring(Step step);
    // Stops a recurring step. Safe from inside any task or step, also for its own id.
    void Cancel(uint32_t id);

    [[nodiscard]] bool HasWork() const;

    // cIGZUnknown. The director owns this object, the reference count never deletes it.
    bool QueryInterface(uint32_t riid, void** ppvObj) override;
    uint32_t AddRef() override;
    uint32_t Release() override;

    // cIGZSystemService
    uint32_t GetServiceID() override;
    void SetServiceID(uint32_t id) override;
    int32_t GetServicePriority() override;
    bool IsServiceRunning() override;
    void SetServiceRunning(bool running) override;
    bool Init() override;
    bool Shutdown() override;
    bool OnTick(uint32_t unknown) override;
    bool OnIdle(uint32_t unknown) override;
    int32_t GetServiceTickPriority() override;

private:
    struct Recurring
    {
        uint32_t id;
        Step step;
        bool cancelled;
    };

    void EnsureRegistered_();
    void UnregisterIfIdle_();

    cIGZFrameWork* framework_ = nullptr;
    uint32_t serviceId_;
    uint32_t refCount_ = 0;
    bool running_ = false;
    bool added_ = false;

    mutable std::mutex mutex_;       // guards posted_ and onIdleList_
    bool onIdleList_ = false;
    std::vector<Task> posted_;
    std::vector<Task> runningTasks_; // swapped with posted_ each tick, keeps its capacity

    std::vector<Recurring> recurring_;
    std::vector<Recurring> pendingRecurring_; // added while the steps run
    bool inTick_ = false;
    uint32_t lastId_ = 0;
};
