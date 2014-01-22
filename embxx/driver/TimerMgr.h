//
// Copyright 2013 (C). Alex Robenko. All rights reserved.
//

// This library is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

/// @file embxx/driver/TimerMgr.h
/// Facilitates creation and measuing multiple timers using only single
/// timer device in bare metal platform.

#pragma once

#include <array>
#include <algorithm>
#include <limits>
#include <iterator>

#include "embxx/util/StaticFunction.h"
#include "embxx/util/Assert.h"
#include "embxx/util/ScopeGuard.h"
#include "embxx/error/ErrorStatus.h"

#include "embxx/device/context.h"

namespace embxx
{

namespace driver
{

/// @addtogroup driver
/// @{

/// @brief Timer Manager.
/// @details Manages allocated timers and their wait requests in a queue to
///          be able to use single timer peripheral for all requested time
///          measurements. It doesn't use any dynamic memory allocation to
///          store and manage timer related information.
/// @tparam TDevice Platform specific device (peripheral) control class. It
///         must expose the following interface:
///         @li Define type for wait time units.
///         @code
///         typedef unsigned WaitTimeType;
///         @endcode
///         @li Function to set timer interrupt callback handler. The function
///             is called diring the construction of TimerMgr object in
///             non-interrupt context.
///         @code
///         template <typename TFunc>
///         void setHandler(TFunc&& func);
///         @endcode
///         @li Functions to start timer countdown in both event loop
///             (non-interrupt) and interrupt contexts.
///         @code
///         void startWait(WaitTimeType waitTime, embxx::device::context::EventLoop context);
///         void startWait(WaitTimeType waitTime, embxx::device::context::Interrupt context);
///         @endcode
///         @li Function to cancel timer countdown in event loop (non-interrupt)
///             context. The function must return true in case the wait was
///             actually cancelled and false and false when there is no wait
///             in progress.
///         @code
///         bool cancelWait(embxx::device::context::EventLoop context);
///         @endcode
///         @li Function to suspend countdown in event loop (non-interrupt)
///             context. The function must return true in case the wait was
///             actually suspended and false when there is no wait in progress.
///             The call to this function will be followed either by
///             resumeWait() or by cancelWait().
///         @code
///         bool suspendWait(embxx::device::context::EventLoop context);
///         @endcode
///         @li Function to resume countdown in event loop (non-interrupt)
///             context.
///         @code
///         void resumeWait(embxx::device::context::EventLoop context);
///         @endcode
///         @li Function to retrieve elapsed time of the last executed wait. It
///             will be called right after the cancelWait().
///         @code
///         WaitTimeType getElapsed(embxx::device::context::EventLoop context) const;
///         @endcode
/// @tparam TEventLoop A variant of embxx::util::EventLoop object that is used
///         to execute posted handlers in regular thread context.
/// @tparam TMaxTimers A number of timers this Timer Manager is supposed to
///         be able to allocate.
/// @tparam TTimeoutHandler The handler type provided with every wait request.
///         It must be either std::function<void (const embxx::error::ErrorStatus&)>
///         or embxx::util::StaticFunction<void (const embxx::error::ErrorStatus&), ...>
///         if no dynamic memory allocation is allowed. Every provided handler
///         function must have the following signature:
///         @code
///         void timeoutHandler(const embxx::error::ErrorStatus& err);
///         @endcode
/// @headerfile embxx/driver/TimerMgr.h
/// @related TimerMgr::Timer
template <typename TDevice,
          typename TEventLoop,
          std::size_t TMaxTimers,
          typename TTimeoutHandler = embxx::util::StaticFunction<void (const embxx::error::ErrorStatus&), 20> >
class TimerMgr
{

public:

    /// @brief Timer device (peripheral) type.
    typedef TDevice Device;

    /// @brief Event loop type
    typedef TEventLoop EventLoop;

    /// @brief Wait time units type
    typedef typename Device::WaitTimeType WaitTimeType;

    /// @brief Max number of timers supported by this TimerMgr
    static const std::size_t MaxTimers = TMaxTimers;

    /// @brief Type of timeout handler
    typedef TTimeoutHandler TimeoutHandler;

    /// @brief Timer class
    /// @details Allocated and managed by embxx::driver::TimerMgr. It is used
    /// to issue new wait request to embxx::driver::TimerMgr.
    /// @related TimerMgr
    /// @headerfile embxx/driver/TimerMgr.h
    class Timer
    {
        friend class TimerMgr<TDevice, TEventLoop, TMaxTimers, TTimeoutHandler>;

    public:
        /// @brief Default constructor
        /// @details Creates invalid Timer object. It cannot be used to
        /// schedule a new wait request. Use embxx::driver::TimerMgr::allocTimer()
        /// function to allocate a valid Timer object.
        /// @post Call to isValid() will return false.
        Timer();

        /// @brief Copy constructor is deleted.
        /// @details Timer object cannot be copied.
        Timer(const Timer&) = delete;

        /// @brief Move constructor.
        /// @details Creates new Timer object out of r-value other Timer object.
        /// @param[in] other R-value of other Timer object.
        /// @post Call to isValid() will return true if and only if call to
        /// other.isValid() would return true prior to this construction.
        /// @post Call to other.isValid() after the construction will return false.
        Timer(Timer&& other);

        /// @brief Destructor
        /// @details Removes proper record from internal data structures of
        /// embxx::driver::TimerMgr.
        /// @pre This timer object mustn't have any pending unhandled
        ///      (timeout handler wasn't called) waits.
        ~Timer();

        /// @brief Copy assignment operator is deleted.
        /// @details Timer object cannot be copied.
        Timer& operator=(const Timer& other) = delete;

        /// @brief Move assignment operator.
        /// @details Similar to move constructor, but deletes its former
        ///          allocation record from internal data structures of
        ///          embxx::driver::TimerMgr.
        /// @param[in] other R-value of other Timer object.
        /// @pre This timer object mustn't have any pending unhandled
        ///      (timeout handler wasn't called) waits prior to the assignment.
        /// @post Call to isValid() will return true if and only if call to
        /// other.isValid() would return true prior to this assignment.
        /// @post Call to other.isValid() after the construction will return false.
        Timer& operator=(Timer&& other);

        /// @brief Check the validity of this timer object
        /// @return true in case timer object is valid.
        bool isValid() const;

        /// @brief Cancel current wait (if such exists).
        /// @details If there is no wait in progress, the call to this function
        ///          doesn't have any effect. Otherwise it will cause the
        ///          call to a callback function, provided with recent asyncWait(),
        ///          to be posted for future processing in event loop with
        ///          embxx::driver::ErrorStatus::Aborted as value of "status"
        ///          argument.
        /// @return true in case the wait was really cancelled, false in case
        ///         this operation had no effect.
        /// @pre Timer object is valid (isValid() return true).
        bool cancel();

        /// @brief Request for asynchronous wait.
        /// @details Will forward the request to schedule the call to provided
        ///          callback function after the requested timeout and exit
        ///          immediately.
        /// @param[in] timeUnits Time to wait in the units supported by the
        ///            timer device (peripheral) object provided in the
        ///            construction of TimerMgr. This value will be used in
        ///            call to configWait() member function of timer device
        ///            object.
        /// @param[in] func Callback function object to be executed when
        ///            requested wait is over. It must have the following
        ///            signature:
        ///            @code
        ///            void timeoutHandler(const embxx::error::ErrorStatus& status);
        ///            @endcode
        /// @pre Timer object is valid (isValid() return true).
        /// @pre The callback from the previous wait request must already be
        ///      called. It is possible to activate new wait by calling
        ///      asyncWait() from within the callback of previously called
        ///      asyncWait().
        template <typename TFunc>
        void asyncWait(
            WaitTimeType timeUnits,
            TFunc&& func);

    private:
        Timer(TimerMgr* mgr);
        void destroy();

        TimerMgr* mgr_;
        unsigned idx_;

        static const unsigned InvalidIdx = std::numeric_limits<unsigned>::max();
    };

    /// @brief Constructor
    /// @details Constructs the TimerMgr object
    /// @param[in] device Reference to timer device control object
    /// @param[in] eventLoop Reference to event loop (embxx::util::EventLoop) object.
    /// @pre Timer device peripheral is stopped and timer interrupts are
    ///      disabled upon construction of this object
    TimerMgr(Device& device, EventLoop& eventLoop);

    /// @brief Copy construction is deleted
    TimerMgr(const TimerMgr&) = delete;

    /// @brief Move construction is deleted
    TimerMgr(TimerMgr&&) = delete;

    /// @brief Destructor
    ~TimerMgr();

    /// @brief Copy assignment is deleted
    TimerMgr& operator=(const TimerMgr&) = delete;

    /// @brief Move assignment is deleted
    TimerMgr& operator=(TimerMgr&&) = delete;

    /// @brief Timer allocation function
    /// @details Allocates timer object to initiate wait requests. It is
    ///          a responsibility of the caller to check the validity of timer
    ///          object.
    /// @return Valid timer object in case number of allocated timers do not
    ///         exceed the capacity of TimerMgr defined with TMaxTimers template
    ///         parameter. Otherwise it will return invalid timer object.
    Timer allocTimer();

private:
    friend class TimerMgr::Timer;

    typedef WaitTimeType TimeCounterType;
    typedef unsigned EngagementIdType;

    /// @cond DOCUMENT_TIMER_MANAGER_INTERNALS
    struct TimerInfo
    {
        TimerInfo();

        bool isAllocated() const;
        void setAllocated(bool allocated);
        bool isWaitInProgress() const;
        void setWaitInProgress(bool inProgress);

        TimeCounterType targetTime_;
        EngagementIdType engagementId_;
        TimeoutHandler handler_;

    private:
        typedef unsigned FlagsType;
        void updateFlag(bool value, FlagsType mask);

        FlagsType flags_;

        static const FlagsType AllocatedFlagMask = 0x1;
        static const FlagsType WaitInProgressMask = 0x2;
    };

    struct ScheduledWaitInfo
    {
        ScheduledWaitInfo();

        TimerInfo* timerInfo_;
        EngagementIdType engagementId_;
        TimeCounterType targetTime_;
    };

    struct ScheduledWaitPriorityComp
    {
        bool operator()(const ScheduledWaitInfo& info1, const ScheduledWaitInfo& info2)
        {
            if (info1.targetTime_ < info2.targetTime_) {
                return false;
            }

            if (info2.targetTime_ < info1.targetTime_) {
                return true;
            }

            return (info2.engagementId_ < info1.engagementId_);
        }
    };
    /// @endcond

    static const std::size_t ScheduleQueueScale = 2;
    typedef std::array<ScheduledWaitInfo, MaxTimers * ScheduleQueueScale> WaitQueue;
    typedef std::array<TimerInfo, MaxTimers> Timers;
    typedef embxx::device::context::EventLoop EventLoopContext;
    typedef embxx::device::context::Interrupt InterruptContext;

    // Functions to be invoked by Timer object
    void deleteTimer(unsigned idx);

    bool cancelWait(unsigned idx);

    void scheduleWait(
        unsigned idx,
        WaitTimeType timeUnits,
        TimeoutHandler&& func);

    // Internal functions
    void addToScheduledWaits(TimerInfo& info);
    void pushToWaitQueue(TimerInfo& info);
    void recreateWaitQueue();
    void postHandler(const embxx::error::ErrorStatus& status, TimerInfo& info, bool interruptContext);
    void interruptHandler(const embxx::error::ErrorStatus& es);
    void postExpiredHandlers(bool interruptContext);

    Device& device_;
    EventLoop& eventLoop_;
    TimeCounterType timeBase_;
    WaitQueue waitQueue_;
    std::size_t waitQueueCount_;
    Timers timers_;
    std::size_t timersCount_;
    EngagementIdType nextEngagementId_;
};

/// @}

// Implementation
template <typename TDevice,
          typename TEventLoop,
          std::size_t TMaxTimers,
          typename TTimeoutHandler>
TimerMgr<TDevice, TEventLoop, TMaxTimers, TTimeoutHandler>::Timer::Timer()
    : mgr_(nullptr),
      idx_(InvalidIdx)
{
}

template <typename TDevice,
          typename TEventLoop,
          std::size_t TMaxTimers,
          typename TTimeoutHandler>
TimerMgr<TDevice, TEventLoop, TMaxTimers, TTimeoutHandler>::Timer::Timer(
    Timer&& other)
    : mgr_(other.mgr_),
      idx_(other.idx_)
{
    other.idx_ = InvalidIdx;
    GASSERT(!other.isValid());
}

template <typename TDevice,
          typename TEventLoop,
          std::size_t TMaxTimers,
          typename TTimeoutHandler>
TimerMgr<TDevice, TEventLoop, TMaxTimers, TTimeoutHandler>::Timer::~Timer()
{
    destroy();
}

template <typename TDevice,
          typename TEventLoop,
          std::size_t TMaxTimers,
          typename TTimeoutHandler>
typename TimerMgr<TDevice, TEventLoop, TMaxTimers, TTimeoutHandler>::Timer&
TimerMgr<TDevice, TEventLoop, TMaxTimers, TTimeoutHandler>::Timer::operator=(
    Timer&& other)
{
    if (this != &other) {
        std::swap(mgr_, other.mgr_);
        std::swap(idx_, other.idx_);
        other.destroy();
    }
    return *this;
}

template <typename TDevice,
          typename TEventLoop,
          std::size_t TMaxTimers,
          typename TTimeoutHandler>
bool
TimerMgr<TDevice, TEventLoop, TMaxTimers, TTimeoutHandler>::Timer::isValid() const
{
    return ((mgr_ != nullptr) && (idx_ != InvalidIdx));
}

template <typename TDevice,
          typename TEventLoop,
          std::size_t TMaxTimers,
          typename TTimeoutHandler>
bool TimerMgr<TDevice, TEventLoop, TMaxTimers, TTimeoutHandler>::Timer::cancel()
{
    GASSERT(isValid());
    return mgr_->cancelWait(idx_);
}

template <typename TDevice,
          typename TEventLoop,
          std::size_t TMaxTimers,
          typename TTimeoutHandler>
template <typename TFunc>
void TimerMgr<TDevice, TEventLoop, TMaxTimers, TTimeoutHandler>::Timer::asyncWait(
    WaitTimeType timeUnits,
    TFunc&& func)
{
    GASSERT(isValid());
    mgr_->scheduleWait(idx_, timeUnits, TimeoutHandler(std::forward<TFunc>(func)));
}


template <typename TDevice,
          typename TEventLoop,
          std::size_t TMaxTimers,
          typename TTimeoutHandler>
TimerMgr<TDevice, TEventLoop, TMaxTimers, TTimeoutHandler>::Timer::Timer(TimerMgr* mgr)
    : mgr_(mgr),
      idx_(InvalidIdx)
{
}

template <typename TDevice,
          typename TEventLoop,
          std::size_t TMaxTimers,
          typename TTimeoutHandler>
void TimerMgr<TDevice, TEventLoop, TMaxTimers, TTimeoutHandler>::Timer::destroy()
{
    if (!isValid()) {
        return;
    }

    mgr_->deleteTimer(idx_);
    idx_ = InvalidIdx;
}

template <typename TDevice,
          typename TEventLoop,
          std::size_t TMaxTimers,
          typename TTimeoutHandler>
TimerMgr<TDevice, TEventLoop, TMaxTimers, TTimeoutHandler>::TimerMgr(
    Device& device,
    EventLoop& eventLoop)
    : device_(device),
      eventLoop_(eventLoop),
      timeBase_(0),
      waitQueueCount_(0),
      timersCount_(0),
      nextEngagementId_(0)
{
    device_.setHandler(
        std::bind(&TimerMgr::interruptHandler, this, std::placeholders::_1));
}

template <typename TDevice,
          typename TEventLoop,
          std::size_t TMaxTimers,
          typename TTimeoutHandler>
TimerMgr<TDevice, TEventLoop, TMaxTimers, TTimeoutHandler>::~TimerMgr()
{
    device_.setHandler(nullptr);
}

template <typename TDevice,
          typename TEventLoop,
          std::size_t TMaxTimers,
          typename TTimeoutHandler>
typename TimerMgr<TDevice, TEventLoop, TMaxTimers, TTimeoutHandler>::Timer
TimerMgr<TDevice, TEventLoop, TMaxTimers, TTimeoutHandler>::allocTimer()
{
    Timer timer(this);
    if (timersCount_ < timers_.size()) {
        auto iter = std::find_if(timers_.begin(), timers_.end(),
            [](const TimerInfo& info) -> bool
            {
                return !info.isAllocated();
            });

        GASSERT(iter != timers_.end());
        timer.idx_ =
            static_cast<std::size_t>(
                std::distance(timers_.begin(), iter));
        iter->setAllocated(true);
        iter->setWaitInProgress(false);
        ++timersCount_;
    }

    return timer;
}

/// @cond DOCUMENT_TIMER_MANAGER_INTERNALS
template <typename TDevice,
          typename TEventLoop,
          std::size_t TMaxTimers,
          typename TTimeoutHandler>
TimerMgr<TDevice, TEventLoop, TMaxTimers, TTimeoutHandler>::TimerInfo::TimerInfo()
    : targetTime_(0),
      engagementId_(0),
      flags_(0)
{
}

template <typename TDevice,
          typename TEventLoop,
          std::size_t TMaxTimers,
          typename TTimeoutHandler>
bool
TimerMgr<TDevice, TEventLoop, TMaxTimers, TTimeoutHandler>::TimerInfo::isAllocated() const
{
    return (flags_ & AllocatedFlagMask) != 0;
}

template <typename TDevice,
          typename TEventLoop,
          std::size_t TMaxTimers,
          typename TTimeoutHandler>
void
TimerMgr<TDevice, TEventLoop, TMaxTimers, TTimeoutHandler>::TimerInfo::setAllocated(
    bool allocated)
{
    updateFlag(allocated, AllocatedFlagMask);
}

template <typename TDevice,
          typename TEventLoop,
          std::size_t TMaxTimers,
          typename TTimeoutHandler>
bool
TimerMgr<TDevice, TEventLoop, TMaxTimers, TTimeoutHandler>::TimerInfo::isWaitInProgress() const
{
    return (flags_ & WaitInProgressMask) != 0;
}

template <typename TDevice,
          typename TEventLoop,
          std::size_t TMaxTimers,
          typename TTimeoutHandler>
void
TimerMgr<TDevice, TEventLoop, TMaxTimers, TTimeoutHandler>::TimerInfo::setWaitInProgress(
    bool engaged)
{
    updateFlag(engaged, WaitInProgressMask);
}

template <typename TDevice,
          typename TEventLoop,
          std::size_t TMaxTimers,
          typename TTimeoutHandler>
void
TimerMgr<TDevice, TEventLoop, TMaxTimers, TTimeoutHandler>::TimerInfo::updateFlag(
    bool value,
    FlagsType mask)
{
    if (value) {
        flags_ |= mask;
    }
    else {
        flags_ &= (~mask);
    }
}

template <typename TDevice,
          typename TEventLoop,
          std::size_t TMaxTimers,
          typename TTimeoutHandler>
TimerMgr<TDevice, TEventLoop, TMaxTimers, TTimeoutHandler>::ScheduledWaitInfo::ScheduledWaitInfo()
    : timerInfo_(nullptr),
      engagementId_(0),
      targetTime_(0)
{
}
/// @endcond

template <typename TDevice,
          typename TEventLoop,
          std::size_t TMaxTimers,
          typename TTimeoutHandler>
void TimerMgr<TDevice, TEventLoop, TMaxTimers, TTimeoutHandler>::deleteTimer(
    unsigned idx)
{
    GASSERT(idx < timersCount_);
    auto& info = timers_[idx];
    GASSERT(info.isAllocated());
    GASSERT(!info.handler_); // Handler must be already invoked and cleared.
    GASSERT(!info.isWaitInProgress());
    info.setAllocated(false);
}

template <typename TDevice,
          typename TEventLoop,
          std::size_t TMaxTimers,
          typename TTimeoutHandler>
bool TimerMgr<TDevice, TEventLoop, TMaxTimers, TTimeoutHandler>::cancelWait(
    unsigned idx)
{
    GASSERT(idx < timersCount_);
    auto& info = timers_[idx];
    if (!device_.suspendWait(EventLoopContext())) {
        // No wait in progress at all
        GASSERT(!info.isWaitInProgress());
        GASSERT(waitQueueCount_ == 0);
        return false;
    }

    auto interruptsGuard = embxx::util::makeScopeGuard(
        [this]()
        {
            device_.resumeWait(EventLoopContext());
        });
    static_cast<void>(interruptsGuard);

    GASSERT(info.isAllocated());

    if (!info.isWaitInProgress()) {
        // No wait scheduled or callback already posted
        return false;
    }

    postHandler(embxx::error::ErrorCode::Aborted, info, false);
    return true;
}

template <typename TDevice,
          typename TEventLoop,
          std::size_t TMaxTimers,
          typename TTimeoutHandler>
void TimerMgr<TDevice, TEventLoop, TMaxTimers, TTimeoutHandler>::scheduleWait(
    unsigned idx,
    WaitTimeType timeUnits,
    TimeoutHandler&& func)
{
    if (device_.cancelWait(EventLoopContext())) {
        // Wait was in progress
        GASSERT(0 < waitQueueCount_);
        timeBase_ += device_.getElapsed(EventLoopContext());
    }

    auto startGuard = embxx::util::makeScopeGuard(
        [this]()
        {
            GASSERT(0 < waitQueueCount_);
            device_.startWait(waitQueue_[0].targetTime_ - timeBase_, EventLoopContext());
        });
    static_cast<void>(startGuard);

    ++nextEngagementId_;
    auto targetTime = timeBase_ + timeUnits;

    GASSERT(idx < timersCount_);
    auto& info = timers_[idx];
    GASSERT(info.isAllocated());
    GASSERT(!info.isWaitInProgress());
    GASSERT(!info.handler_); // Handler must be already invoked and cleared.

    info.targetTime_ = targetTime;
    info.engagementId_ = nextEngagementId_;
    info.handler_ = std::move(func);
    info.setWaitInProgress(true);

    if (waitQueue_.size() <= waitQueueCount_) {
        // Wait queue overflow, contains lots of invalid waits, clean required.
        recreateWaitQueue();
    }
    else {
        pushToWaitQueue(info);
    }

    postExpiredHandlers(false);
    // starts on exit
}

template <typename TDevice,
          typename TEventLoop,
          std::size_t TMaxTimers,
          typename TTimeoutHandler>
void TimerMgr<TDevice, TEventLoop, TMaxTimers, TTimeoutHandler>::addToScheduledWaits(
    TimerInfo& info)
{
    GASSERT(waitQueueCount_ < waitQueue_.size());
    auto& waitInfo = waitQueue_[waitQueueCount_];
    waitInfo.timerInfo_ = &info;
    waitInfo.engagementId_ = info.engagementId_;
    waitInfo.targetTime_ = info.targetTime_;
    ++waitQueueCount_;
}

template <typename TDevice,
          typename TEventLoop,
          std::size_t TMaxTimers,
          typename TTimeoutHandler>
void TimerMgr<TDevice, TEventLoop, TMaxTimers, TTimeoutHandler>::pushToWaitQueue(
    TimerInfo& info)
{
    addToScheduledWaits(info);

    std::push_heap(
        waitQueue_.begin(),
        waitQueue_.begin() + waitQueueCount_,
        ScheduledWaitPriorityComp());
}

template <typename TDevice,
          typename TEventLoop,
          std::size_t TMaxTimers,
          typename TTimeoutHandler>
void TimerMgr<TDevice, TEventLoop, TMaxTimers, TTimeoutHandler>::recreateWaitQueue()
{
    waitQueueCount_ = 0;
    std::for_each(timers_.begin(), timers_.end(),
        [this](TimerInfo& info)
        {
            if (info.isAllocated() && info.isWaitInProgress()) {
                addToScheduledWaits(info);
            }
        });

    std::make_heap(
        waitQueue_.begin(),
        waitQueue_.begin() + waitQueueCount_,
        ScheduledWaitPriorityComp());
}

template <typename TDevice,
          typename TEventLoop,
          std::size_t TMaxTimers,
          typename TTimeoutHandler>
void TimerMgr<TDevice, TEventLoop, TMaxTimers, TTimeoutHandler>::postHandler(
    const embxx::error::ErrorStatus& status,
    TimerInfo& info,
    bool interruptContext)
{
    GASSERT(info.handler_);
    GASSERT(info.isAllocated());
    GASSERT(info.isWaitInProgress());

    bool postResult = false;
    if (interruptContext) {
        postResult =
            eventLoop_.postInterruptCtx(
                std::bind(std::move(info.handler_), status));
    }
    else {
        postResult =
            eventLoop_.post(
                std::bind(std::move(info.handler_), status));
    }
    static_cast<void>(postResult);
    GASSERT(postResult);
    GASSERT(!info.handler_);
    info.setWaitInProgress(false);
}

template <typename TDevice,
          typename TEventLoop,
          std::size_t TMaxTimers,
          typename TTimeoutHandler>
void TimerMgr<TDevice, TEventLoop, TMaxTimers, TTimeoutHandler>::interruptHandler(
    const embxx::error::ErrorStatus& es)
{
    // Executed in interrupt context

    if (es) {
        auto beginIter = timers_.begin();
        auto endIter = beginIter + timersCount_;
        std::for_each(beginIter, endIter,
            [this, &es](TimerInfo& info)
            {
                if (info.handler_) {
                    postHandler(es, info, true);
                }
            });

        waitQueueCount_ = 0;
        return;
    }

    GASSERT(0 < waitQueueCount_);
    GASSERT(timeBase_ <= waitQueue_[0].targetTime_);
    timeBase_ = waitQueue_[0].targetTime_;

    postExpiredHandlers(true);

    if (0 < waitQueueCount_) {
        device_.startWait(waitQueue_[0].targetTime_ - timeBase_, InterruptContext());
    }
}

template <typename TDevice,
          typename TEventLoop,
          std::size_t TMaxTimers,
          typename TTimeoutHandler>
void TimerMgr<TDevice, TEventLoop, TMaxTimers, TTimeoutHandler>::postExpiredHandlers(
    bool interruptContext)
{
    while (0 < waitQueueCount_) {
        auto& waitInfo = waitQueue_[0];
        if (timeBase_ < waitInfo.targetTime_) {
            break;
        }

        auto timerInfoPtr = waitInfo.timerInfo_;
        if (timerInfoPtr->isAllocated() &&
            timerInfoPtr->isWaitInProgress() &&
            (timerInfoPtr->engagementId_ == waitInfo.engagementId_)) {
            GASSERT(timerInfoPtr->handler_);

            postHandler(embxx::error::ErrorCode::Success, *timerInfoPtr, interruptContext);
        }

        std::pop_heap(
            waitQueue_.begin(),
            waitQueue_.begin() + waitQueueCount_,
            ScheduledWaitPriorityComp());

        --waitQueueCount_;
    }

}

}  // namespace driver

}  // namespace embxx


