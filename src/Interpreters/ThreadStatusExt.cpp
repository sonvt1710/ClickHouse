#include <Common/ThreadStatus.h>

#include <Interpreters/Context.h>
#include <Interpreters/ProcessList.h>
#include <Interpreters/QueryThreadLog.h>
#include <Common/CurrentThread.h>
#include <Common/Exception.h>
#include <Common/QueryProfiler.h>
#include <Common/ThreadProfileEvents.h>
#include <Common/TraceCollector.h>

#if defined(OS_LINUX)
#   include <Common/hasLinuxCapability.h>

#   include <sys/time.h>
#   include <sys/resource.h>
#endif


/// Implement some methods of ThreadStatus and CurrentThread here to avoid extra linking dependencies in clickhouse_common_io
/// TODO It doesn't make sense.

namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int CANNOT_SET_THREAD_PRIORITY;
}


void ThreadStatus::attachQueryContext(Context & query_context_)
{
    query_context = &query_context_;
    query_id = query_context->getCurrentQueryId();
    if (!global_context)
        global_context = &query_context->getGlobalContext();

    if (thread_group)
    {
        std::lock_guard lock(thread_group->mutex);

        thread_group->query_context = query_context;
        if (!thread_group->global_context)
            thread_group->global_context = global_context;
    }

    initQueryProfiler();
}

void CurrentThread::defaultThreadDeleter()
{
    if (unlikely(!current_thread))
        return;
    current_thread->detachQuery(true, true);
}

void ThreadStatus::setupState(const ThreadGroupStatusPtr & thread_group_)
{
    assertState({ThreadState::DetachedFromQuery}, __PRETTY_FUNCTION__);

    /// Attach or init current thread to thread group and copy useful information from it
    thread_group = thread_group_;

    performance_counters.setParent(&thread_group->performance_counters);
    memory_tracker.setParent(&thread_group->memory_tracker);

    {
        std::lock_guard lock(thread_group->mutex);

        /// NOTE: thread may be attached multiple times if it is reused from a thread pool.
        thread_group->thread_ids.emplace_back(thread_id);

        logs_queue_ptr = thread_group->logs_queue_ptr;
        query_context = thread_group->query_context;

        if (!global_context)
            global_context = thread_group->global_context;
    }

    if (query_context)
    {
        query_id = query_context->getCurrentQueryId();
        initQueryProfiler();

#if defined(OS_LINUX)
        /// Set "nice" value if required.
        Int32 new_os_thread_priority = query_context->getSettingsRef().os_thread_priority;
        if (new_os_thread_priority && hasLinuxCapability(CAP_SYS_NICE))
        {
            LOG_TRACE(log, "Setting nice to " << new_os_thread_priority);

            if (0 != setpriority(PRIO_PROCESS, thread_id, new_os_thread_priority))
                throwFromErrno("Cannot 'setpriority'", ErrorCodes::CANNOT_SET_THREAD_PRIORITY);

            os_thread_priority = new_os_thread_priority;
        }
#endif
    }

    initPerformanceCounters();

    thread_state = ThreadState::AttachedToQuery;
}

void ThreadStatus::initializeQuery()
{
    setupState(std::make_shared<ThreadGroupStatus>());

    /// No need to lock on mutex here
    thread_group->memory_tracker.setDescription("(for query)");
    thread_group->master_thread_id = thread_id;
}

void ThreadStatus::attachQuery(const ThreadGroupStatusPtr & thread_group_, bool check_detached)
{
    if (thread_state == ThreadState::AttachedToQuery)
    {
        if (check_detached)
            throw Exception("Can't attach query to the thread, it is already attached", ErrorCodes::LOGICAL_ERROR);
        return;
    }

    if (!thread_group_)
        throw Exception("Attempt to attach to nullptr thread group", ErrorCodes::LOGICAL_ERROR);

    setupState(thread_group_);
}

void ThreadStatus::finalizePerformanceCounters()
{
    if (performance_counters_finalized)
        return;

    performance_counters_finalized = true;
    updatePerformanceCounters();

    try
    {
        if (global_context && query_context)
        {
            auto & settings = query_context->getSettingsRef();
            if (settings.log_queries && settings.log_query_threads)
                if (auto thread_log = global_context->getQueryThreadLog())
                    logToQueryThreadLog(*thread_log);
        }
    }
    catch (...)
    {
        tryLogCurrentException(log);
    }
}

void ThreadStatus::initQueryProfiler()
{
    /// query profilers are useless without trace collector
    if (!global_context || !global_context->hasTraceCollector())
        return;

    const auto & settings = query_context->getSettingsRef();

    try
    {
        if (settings.query_profiler_real_time_period_ns > 0)
            query_profiler_real = std::make_unique<QueryProfilerReal>(thread_id,
                /* period */ static_cast<UInt32>(settings.query_profiler_real_time_period_ns));

        if (settings.query_profiler_cpu_time_period_ns > 0)
            query_profiler_cpu = std::make_unique<QueryProfilerCpu>(thread_id,
                /* period */ static_cast<UInt32>(settings.query_profiler_cpu_time_period_ns));
    }
    catch (...)
    {
        /// QueryProfiler is optional.
        tryLogCurrentException("ThreadStatus", "Cannot initialize QueryProfiler");
    }
}

void ThreadStatus::finalizeQueryProfiler()
{
    query_profiler_real.reset();
    query_profiler_cpu.reset();
}

void ThreadStatus::detachQuery(bool exit_if_already_detached, bool thread_exits)
{
    if (exit_if_already_detached && thread_state == ThreadState::DetachedFromQuery)
    {
        thread_state = thread_exits ? ThreadState::Died : ThreadState::DetachedFromQuery;
        return;
    }

    assertState({ThreadState::AttachedToQuery}, __PRETTY_FUNCTION__);

    finalizeQueryProfiler();
    finalizePerformanceCounters();

    /// Detach from thread group
    performance_counters.setParent(&ProfileEvents::global_counters);
    memory_tracker.reset();

    /// Must reset pointer to thread_group's memory_tracker, because it will be destroyed two lines below.
    memory_tracker.setParent(nullptr);

    query_id.clear();
    query_context = nullptr;
    thread_group.reset();

    thread_state = thread_exits ? ThreadState::Died : ThreadState::DetachedFromQuery;

#if defined(__linux__)
    if (os_thread_priority)
    {
        LOG_TRACE(log, "Resetting nice");

        if (0 != setpriority(PRIO_PROCESS, thread_id, 0))
            LOG_ERROR(log, "Cannot 'setpriority' back to zero: " << errnoToString(ErrorCodes::CANNOT_SET_THREAD_PRIORITY, errno));

        os_thread_priority = 0;
    }
#endif
}

void ThreadStatus::logToQueryThreadLog(QueryThreadLog & thread_log)
{
    QueryThreadLogElement elem;

    elem.event_time = time(nullptr);
    elem.query_start_time = query_start_time;
    elem.query_duration_ms = (getCurrentTimeNanoseconds() - query_start_time_nanoseconds) / 1000000U;

    elem.read_rows = progress_in.read_rows.load(std::memory_order_relaxed);
    elem.read_bytes = progress_in.read_bytes.load(std::memory_order_relaxed);

    /// TODO: Use written_rows and written_bytes when run time progress is implemented
    elem.written_rows = progress_out.read_rows.load(std::memory_order_relaxed);
    elem.written_bytes = progress_out.read_bytes.load(std::memory_order_relaxed);
    elem.memory_usage = memory_tracker.get();
    elem.peak_memory_usage = memory_tracker.getPeak();

    elem.thread_name = getThreadName();
    elem.thread_id = thread_id;

    if (thread_group)
    {
        {
            std::lock_guard lock(thread_group->mutex);

            elem.master_thread_id = thread_group->master_thread_id;
            elem.query = thread_group->query;
        }
    }

    if (query_context)
    {
        elem.client_info = query_context->getClientInfo();

        if (query_context->getSettingsRef().log_profile_events != 0)
        {
            /// NOTE: Here we are in the same thread, so we can make memcpy()
            elem.profile_counters = std::make_shared<ProfileEvents::Counters>(performance_counters.getPartiallyAtomicSnapshot());
        }
    }

    thread_log.add(elem);
}

void CurrentThread::initializeQuery()
{
    if (unlikely(!current_thread))
        return;
    current_thread->initializeQuery();
    current_thread->deleter = CurrentThread::defaultThreadDeleter;
}

void CurrentThread::attachTo(const ThreadGroupStatusPtr & thread_group)
{
    if (unlikely(!current_thread))
        return;
    current_thread->attachQuery(thread_group, true);
    current_thread->deleter = CurrentThread::defaultThreadDeleter;
}

void CurrentThread::attachToIfDetached(const ThreadGroupStatusPtr & thread_group)
{
    if (unlikely(!current_thread))
        return;
    current_thread->attachQuery(thread_group, false);
    current_thread->deleter = CurrentThread::defaultThreadDeleter;
}

void CurrentThread::attachQueryContext(Context & query_context)
{
    if (unlikely(!current_thread))
        return;
    current_thread->attachQueryContext(query_context);
}

void CurrentThread::finalizePerformanceCounters()
{
    if (unlikely(!current_thread))
        return;
    current_thread->finalizePerformanceCounters();
}

void CurrentThread::detachQuery()
{
    if (unlikely(!current_thread))
        return;
    current_thread->detachQuery(false);
}

void CurrentThread::detachQueryIfNotDetached()
{
    if (unlikely(!current_thread))
        return;
    current_thread->detachQuery(true);
}


CurrentThread::QueryScope::QueryScope(Context & query_context)
{
    CurrentThread::initializeQuery();
    CurrentThread::attachQueryContext(query_context);
}

void CurrentThread::QueryScope::logPeakMemoryUsage()
{
    auto group = CurrentThread::getGroup();
    if (!group)
        return;

    log_peak_memory_usage_in_destructor = false;
    group->memory_tracker.logPeakMemoryUsage();
}

CurrentThread::QueryScope::~QueryScope()
{
    try
    {
        if (log_peak_memory_usage_in_destructor)
            logPeakMemoryUsage();

        CurrentThread::detachQueryIfNotDetached();
    }
    catch (...)
    {
        tryLogCurrentException("CurrentThread", __PRETTY_FUNCTION__);
    }
}

}
