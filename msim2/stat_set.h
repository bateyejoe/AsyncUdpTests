/* stat_set.h
 *
 * Quick and dirty module to profile method calls and print the results
 *
 */

#include <stdio.h>
#include <iostream>
#include <chrono>
#include <iomanip>
#include <algorithm>

using namespace std::chrono_literals;
using Clock_t = std::chrono::steady_clock;
using AbsoluteTime_t = Clock_t::time_point;
using RelativeTime_t = Clock_t::duration;

// Quick and dirty class to track CPU resource usage for the duration of the test
class TimeAndResourceTracker
{
public:
    void begin()
    {
        m_start = Clock_t::now();
        getrusage(RUSAGE_SELF, &m_rusage);
    }
    void end()
    {
        RelativeTime_t wallClockDuration = Clock_t::now() - m_start;
        struct rusage end;
        getrusage(RUSAGE_SELF, &end);
        m_userTime = (end.ru_utime.tv_sec + end.ru_utime.tv_usec/1000000.0) - (m_rusage.ru_utime.tv_sec + m_rusage.ru_utime.tv_usec/1000000.0);
        m_sysTime = (end.ru_stime.tv_sec + end.ru_stime.tv_usec/1000000.0) - (m_rusage.ru_stime.tv_sec + m_rusage.ru_stime.tv_usec/1000000.0);
        m_rusage.ru_maxrss = end.ru_maxrss - m_rusage.ru_maxrss;
        m_rusage.ru_minflt = end.ru_minflt - m_rusage.ru_minflt;
        m_rusage.ru_majflt = end.ru_majflt - m_rusage.ru_majflt;
        m_rusage.ru_inblock = end.ru_inblock - m_rusage.ru_inblock;
        m_rusage.ru_oublock = end.ru_oublock - m_rusage.ru_oublock;
        m_rusage.ru_nvcsw = end.ru_nvcsw - m_rusage.ru_nvcsw;
        m_rusage.ru_nivcsw = end.ru_nivcsw - m_rusage.ru_nivcsw;
        m_wallClockTime = std::chrono::duration_cast<std::chrono::microseconds>(wallClockDuration).count()/1000000.0;
    }

    AbsoluteTime_t start_time() const
    {
        return m_start;
    }
    double elapsed_wallclock_time() const
    {
        return m_wallClockTime;
    }
    double process_time() const
    {
        return m_userTime + m_sysTime;
    }
    double user_time() const
    {
        return m_userTime;
    }
    double sys_time() const
    {
        return m_sysTime;
    }
    double percent_user_time() const
    {
        return m_userTime / m_wallClockTime;
    }
    double percent_sys_time() const
    {
        return m_sysTime / m_wallClockTime;
    }
    double percent_cpu() const
    {
        return (m_userTime + m_sysTime) / m_wallClockTime;
    }
private:
    AbsoluteTime_t  m_start;
    struct rusage   m_rusage;
    double          m_userTime      = 0.0;
    double          m_sysTime       = 0.0;
    double          m_wallClockTime = 0.0;
};  // class TimeAndResourceTracker

class StatSet
{
public:
    // uring call stats (accumulated execution time in microseconds)
    class CallStat
    {
    public:
        CallStat(const char* name, StatSet& statSet) :
            m_name(name)
        {
            statSet.add(this);
        }

        const char*     m_name;
        std::uint64_t   m_usAccumulated = 0;
        std::uint64_t   m_calls = 0;
        std::uint64_t   m_maxSingleCallTime = 0;
    };  // Uring stat
    friend class CallStat;

    class TraceStat
    {
    public:
        TraceStat(const CallStat& stat, std::uint64_t usTotalTime) :
            m_stat(stat), m_usTotalTime(usTotalTime)
        {}
        const CallStat&        m_stat;
        const std::uint64_t     m_usTotalTime;
    };

public:
    void stream(std::ostream& os, std::uint64_t totalTime);

private:
    void add(CallStat* pStat)
    {
        m_stats.push_back(pStat);
    }
    std::vector<CallStat*> m_stats;
};  // class StatSet


inline std::ostream& operator<<(std::ostream& os, const StatSet::TraceStat& ts)
{
    std::ios state(nullptr);
    state.copyfmt(os);
    os.setf(std::ios::right);
    os.precision(6);
    os << std::setw(12) << ts.m_stat.m_usAccumulated/1000000.0 << ' '
       << std::setw(12) << ts.m_stat.m_calls << ' '
       << std::setw(12) << (static_cast<double>(ts.m_stat.m_usAccumulated)/ts.m_usTotalTime)*100.0 << "% "
       << std::setw(12) << ts.m_stat.m_maxSingleCallTime/1000000.0;
    os.copyfmt(state);
    return os;
}

inline void StatSet::stream(std::ostream& os, std::uint64_t totalTime)
{
    // Sort the stats by time used
    std::sort(  m_stats.begin(),
                m_stats.end(),
                [](const CallStat* a, const CallStat* b)
                {
                    return a->m_usAccumulated > b->m_usAccumulated;
                });
    // dump them out
    size_t maxMethodNameLength = 0;
    std::for_each(m_stats.begin(), m_stats.end(), [&](const CallStat* pStat) { maxMethodNameLength = std::max(maxMethodNameLength, strlen(pStat->m_name)); });
    ++maxMethodNameLength;
    os.setf(std::ios::left);
    os << std::setw(maxMethodNameLength) << "Method" << "     time(s)        calls    % of total     max time\n";
    for (auto pStat : m_stats)
    {
        os.setf(std::ios::left);
        os << std::setw(maxMethodNameLength) << pStat->m_name << StatSet::TraceStat(*pStat, totalTime) << std::endl;
    }
}

template <StatSet::CallStat& STAT_T>
class CallStatUpdater
{
public:
    CallStatUpdater() :
        m_start(Clock_t::now())
    {
    }

    CallStatUpdater(std::uint64_t *pResult) :
        m_start(Clock_t::now()),
        m_pResult(pResult)
    {
    }

    ~CallStatUpdater()
    {
        ++STAT_T.m_calls;
        std::uint64_t thisCallTime = std::chrono::duration_cast<std::chrono::microseconds>(Clock_t::now()-m_start).count();
        STAT_T.m_usAccumulated += thisCallTime;
        STAT_T.m_maxSingleCallTime = std::max(STAT_T.m_maxSingleCallTime, thisCallTime);
        if (m_pResult != nullptr)
            *m_pResult = thisCallTime;
    }

private:
    AbsoluteTime_t  m_start;
    std::uint64_t*  m_pResult = nullptr;
};

#define TIME_CALL(a, b)\
{\
    CallStatUpdater<a> stat;\
    (b);\
}

#define TIME_CALL_WITH_RETURN(a, b, c)\
{\
    CallStatUpdater<a> stat(c);\
    (b);\
}
