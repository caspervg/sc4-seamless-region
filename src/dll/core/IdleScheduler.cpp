#include "core/IdleScheduler.hpp"

#include <exception>

#include "cIGZFrameWork.h"
#include "utils/Logger.h"

namespace
{
    constexpr uint32_t kIdleSchedulerServiceId = 0x5E4A1D7C;

    template <typename Fn>
    bool RunGuarded(const char* what, Fn&& fn)
    {
        try {
            return fn();
        }
        catch (const std::exception& e) {
            LOG_ERROR("IdleScheduler: {} threw: {}", what, e.what());
        }
        catch (...) {
            LOG_ERROR("IdleScheduler: {} threw an unknown exception", what);
        }
        return false;
    }
}

IdleScheduler::IdleScheduler()
    : serviceId_(kIdleSchedulerServiceId)
{
}

IdleScheduler::~IdleScheduler() = default;

bool IdleScheduler::Initialize(cIGZFrameWork* framework)
{
    if (!framework) {
        LOG_ERROR("IdleScheduler: no framework");
        return false;
    }
    framework_ = framework;
    added_ = framework_->AddSystemService(this);
    if (!added_) {
        LOG_WARN("IdleScheduler: AddSystemService failed, the idle list still works");
    }
    running_ = true;
    return true;
}

void IdleScheduler::Terminate()
{
    {
        std::scoped_lock lock(mutex_);
        posted_.clear();
        if (framework_ && onIdleList_) {
            framework_->RemoveFromOnIdle(this);
        }
        onIdleList_ = false;
    }
    recurring_.clear();
    pendingRecurring_.clear();
    if (framework_ && added_) {
        framework_->RemoveSystemService(this);
    }
    added_ = false;
    running_ = false;
    framework_ = nullptr;
}

void IdleScheduler::Post(Task task)
{
    if (!task) {
        return;
    }
    std::scoped_lock lock(mutex_);
    if (!framework_) {
        return;
    }
    posted_.push_back(std::move(task));
    EnsureRegistered_();
}

uint32_t IdleScheduler::AddRecurring(Step step)
{
    if (!step || !framework_) {
        return 0;
    }
    const uint32_t id = ++lastId_;
    if (inTick_) {
        pendingRecurring_.push_back({id, std::move(step), false});
    }
    else {
        recurring_.push_back({id, std::move(step), false});
    }
    std::scoped_lock lock(mutex_);
    EnsureRegistered_();
    return id;
}

void IdleScheduler::Cancel(const uint32_t id)
{
    if (id == 0) {
        return;
    }
    for (auto& r : recurring_) {
        if (r.id == id) {
            r.cancelled = true;
        }
    }
    for (auto& r : pendingRecurring_) {
        if (r.id == id) {
            r.cancelled = true;
        }
    }
    if (!inTick_) {
        std::erase_if(recurring_, [](const Recurring& r) { return r.cancelled; });
        UnregisterIfIdle_();
    }
}

bool IdleScheduler::HasWork() const
{
    std::scoped_lock lock(mutex_);
    return !posted_.empty() || !recurring_.empty() || !pendingRecurring_.empty();
}

void IdleScheduler::EnsureRegistered_()
{
    // mutex_ is held.
    if (!onIdleList_ && framework_) {
        onIdleList_ = framework_->AddToOnIdle(this);
        if (!onIdleList_) {
            LOG_ERROR("IdleScheduler: AddToOnIdle failed");
        }
    }
}

void IdleScheduler::UnregisterIfIdle_()
{
    std::scoped_lock lock(mutex_);
    if (onIdleList_ && framework_ && posted_.empty() && recurring_.empty() && pendingRecurring_.empty()) {
        // The framework keeps removed services alive until its idle loop ends, so this is
        // safe from inside OnIdle.
        framework_->RemoveFromOnIdle(this);
        onIdleList_ = false;
    }
}

bool IdleScheduler::OnIdle(uint32_t)
{
    if (inTick_ || !framework_) {
        return true;
    }
    inTick_ = true;

    {
        std::scoped_lock lock(mutex_);
        posted_.swap(runningTasks_);
    }
    for (auto& task : runningTasks_) {
        RunGuarded("task", [&] {
            task();
            return true;
        });
    }
    runningTasks_.clear();

    // recurring_ does not change size while the steps run: new steps go to pendingRecurring_.
    for (auto& r : recurring_) {
        if (!r.cancelled && !RunGuarded("recurring step", r.step)) {
            r.cancelled = true;
        }
    }
    std::erase_if(recurring_, [](const Recurring& r) { return r.cancelled; });
    for (auto& r : pendingRecurring_) {
        if (!r.cancelled) {
            recurring_.push_back(std::move(r));
        }
    }
    pendingRecurring_.clear();

    inTick_ = false;
    UnregisterIfIdle_();
    return true;
}

bool IdleScheduler::QueryInterface(const uint32_t riid, void** ppvObj)
{
    if (!ppvObj) {
        return false;
    }
    if (riid == GZIID_cIGZSystemService) {
        *ppvObj = static_cast<cIGZSystemService*>(this);
        AddRef();
        return true;
    }
    if (riid == GZIID_cIGZUnknown) {
        *ppvObj = static_cast<cIGZUnknown*>(this);
        AddRef();
        return true;
    }
    return false;
}

uint32_t IdleScheduler::AddRef() { return ++refCount_; }

uint32_t IdleScheduler::Release()
{
    if (refCount_ > 0) {
        --refCount_;
    }
    return refCount_;
}

uint32_t IdleScheduler::GetServiceID() { return serviceId_; }
void IdleScheduler::SetServiceID(const uint32_t id) { serviceId_ = id; }
int32_t IdleScheduler::GetServicePriority() { return 0; }
bool IdleScheduler::IsServiceRunning() { return running_; }
void IdleScheduler::SetServiceRunning(const bool running) { running_ = running; }
bool IdleScheduler::Init() { return true; }
bool IdleScheduler::Shutdown() { return true; }
bool IdleScheduler::OnTick(uint32_t) { return true; }
int32_t IdleScheduler::GetServiceTickPriority() { return 0; }
