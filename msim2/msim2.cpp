#include <stdio.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <iostream>
#include <fstream>
#include <list>
#include <vector>
#include <cassert>
#include <thread>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <system_error>
#include <sys/resource.h>
#include <sys/epoll.h>

#include <liburing.h>

#include "stat_set.h"

// Config
sockaddr_storage            g_destAddress;
socklen_t                   g_destAddressLen    = 0;        // size of the address stored in g_destAddress
unsigned short              g_port              = 0;        // UDP port
size_t                      g_udpPayloadSize    = 172;      // Payload size
unsigned int                g_rate              = 50;       // Packets per second
unsigned int                g_ringFlags         = IORING_SETUP_SUBMIT_ALL | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG;
std::uint64_t               g_packetsToSend     = 10000;
unsigned int                g_buffers           = 100;      // receive buffers (cmdline option)
unsigned int                g_sendBuffers       = 5000;     // send buffers, fixed, but only submit as many as needed to keep up with data rate
bool                        g_dumpSubmitTiming  = false;    // Dump timing and batch size for every calls to io_uring_submit()
std::string                 g_submitTimingFilename;         // Filename to write io_uring_submit() timing
bool                        g_dumpApiTiming     = false;    // Dump out profiling stats for calls to liburing or epoll, recvfrom, sendto
std::string                 g_apiTimingFilename;            // Filename for API stats
bool                        g_appendStatistics  = false;    // Append the statistics to an existing file
std::string                 g_statisticsFilename;     // File to append stats to. If file does not exist, it is created and a header line with column names is appended first
unsigned int                g_ringSize          = 8000;     // Ring size

// Special stats for io_uring_submit
std::uint64_t               g_batchSize                 = 0;    // # of unsubmitted SQEs
std::uint64_t               g_maxBatchSize              = 0;
std::uint64_t               g_uringSqes                 = 0;    // # of submitted SQEs

// For capturing, in memory, timing for every call to io_uring_submit()
struct SubmitCallDataPoint
{
    std::uint64_t       m_microseconds;
    std::uint64_t       m_batchSize;
};
static constexpr size_t     g_maxSubmitCallDataPoints   = 2000000;
SubmitCallDataPoint         g_submitCalls[g_maxSubmitCallDataPoints];
size_t                      g_nextSubmitDataPoint       = 0;

// I/O stats
std::uint64_t   g_sendsInitiated = 0;
std::uint64_t   g_sendsCompleted = 0;
std::uint64_t   g_recvsInitiated = 0;
std::uint64_t   g_recvsCompleted = 0;

// Wait stats
std::uint64_t               g_waitCalls         = 0;
std::uint64_t               g_totalMicroseconds = 0;
std::uint64_t               g_maxWait           = 0;
std::uint64_t               g_waitsOver500ms    = 0;

// Statistics for liburing calls
StatSet g_uringStatSet;
StatSet::CallStat s_io_uring_get_sqe("io_uring_get_sqe", g_uringStatSet);
StatSet::CallStat s_io_uring_prep_recvmsg("io_uring_prep_recvmsg", g_uringStatSet);
StatSet::CallStat s_io_uring_sqe_set_data("io_uring_sqe_set_data", g_uringStatSet);
StatSet::CallStat s_io_uring_prep_sendmsg("io_uring_prep_sendmsg", g_uringStatSet);
StatSet::CallStat s_io_uring_submit("io_uring_submit", g_uringStatSet);
StatSet::CallStat s_io_uring_cq_has_overflow("io_uring_cq_has_overflow", g_uringStatSet);
StatSet::CallStat s_io_uring_get_events("io_uring_get_events", g_uringStatSet);
StatSet::CallStat s_io_uring_wait_cqe_timeout("io_uring_wait_cqe_timeout", g_uringStatSet);
StatSet::CallStat s_io_uring_peek_cqe("io_uring_peek_cqe", g_uringStatSet);
StatSet::CallStat s_io_uring_cqe_get_data("io_uring_cqe_get_data", g_uringStatSet);
StatSet::CallStat s_io_uring_cqe_seen("io_uring_cqe_seen", g_uringStatSet);
StatSet::CallStat s_io_uring_prep_cancel_fd("io_uring_prep_cancel_fd", g_uringStatSet);

// Statistics for epoll and related system calls
StatSet g_epollStatSet;
StatSet::CallStat s_epoll_ctl("epoll_ctl", g_epollStatSet);
StatSet::CallStat s_epoll_wait("epoll_wait", g_epollStatSet);
StatSet::CallStat s_recvfrom("recvfrom", g_epollStatSet);
StatSet::CallStat s_sendto("sendto", g_epollStatSet);

// Fill a sockaddr_storage with the IP and port from two strings. Returns the size
// of the socket_address stored in the sockaddr_storage.
socklen_t resolve_address(sockaddr_storage* pDest, const char* address, const char* port)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_canonname = NULL;
    hints.ai_addr = NULL;
    hints.ai_next = NULL;

    struct addrinfo* pResult;

    int res = ::getaddrinfo(address, port, &hints, &pResult);
    if (res != 0)
    {
        throw std::system_error(res, std::generic_category(), std::string(gai_strerror(res)));
    }
    memcpy(pDest, pResult->ai_addr, pResult->ai_addrlen);
    freeaddrinfo(pResult);
    return pResult->ai_addrlen;
}

// Keep track of some global stats
void update_wait_stats(RelativeTime_t waitTime)
{
    std::uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(waitTime).count();
    ++g_waitCalls;
    if (us > 500000)
        ++g_waitsOver500ms;
    g_totalMicroseconds += us;
    g_maxWait = std::max(g_maxWait, us);
}

// Defined for the m_op field in the IoOp structure to differentiate between send buffers and receive buffers
unsigned int opSend = 0;
unsigned int opRecv = 1;

struct IoOp;
using OpList_t = std::list<IoOp*>;  // IoOp list type

struct IoOp
{
    unsigned int                m_op;
    std::byte*                  m_pPayload;
    size_t                      m_payloadSize;
    struct msghdr               m_msg;
    struct iovec                m_iov;
    unsigned char               m_sockAddrBuffer[64];   // should be big enough for ipv4 or ipv6
    socklen_t                   m_sockAddrLen;
    OpList_t::const_iterator    m_self;
};  // SendOp

// Allocate an SQE and prepare (but don't submit) a receive operation
bool queue_recv(struct io_uring* pRing, IoOp* pOp, int sockFd)
{
    // Get the SQE
    struct io_uring_sqe* pSqe;
    TIME_CALL(s_io_uring_get_sqe, pSqe = io_uring_get_sqe(pRing));
    ++g_batchSize;
    if (pSqe == nullptr)
        return false;  // can't submit any more

    // Prepare the receive
    pOp->m_iov.iov_base = pOp->m_pPayload;
    pOp->m_iov.iov_len = pOp->m_payloadSize;
    pOp->m_msg.msg_iov = &pOp->m_iov;
    pOp->m_msg.msg_iovlen = 1;
    pOp->m_msg.msg_name = &pOp->m_sockAddrBuffer;
    pOp->m_msg.msg_namelen = sizeof(pOp->m_sockAddrBuffer);
    pOp->m_msg.msg_control = nullptr;
    pOp->m_msg.msg_controllen = 0;
    pOp->m_msg.msg_flags = 0;
    TIME_CALL(s_io_uring_prep_recvmsg, io_uring_prep_recvmsg(pSqe, sockFd, &pOp->m_msg, 0u));
    TIME_CALL(s_io_uring_sqe_set_data, io_uring_sqe_set_data(pSqe, pOp));

    return true;
}

// Allocate an SQE and prepare (but don't submit) a send operation
bool queue_send(struct io_uring* pRing, IoOp* pOp, int sockFd)
{
    // Get the SQE
    struct io_uring_sqe* pSqe;
    TIME_CALL(s_io_uring_get_sqe , pSqe = io_uring_get_sqe(pRing));
    ++g_batchSize;
    if (pSqe == nullptr)
        return false;  // can't submit any more

    // Prepare the send
    pOp->m_iov.iov_base = pOp->m_pPayload;
    pOp->m_iov.iov_len = pOp->m_payloadSize;
    pOp->m_msg.msg_iov = &pOp->m_iov;
    pOp->m_msg.msg_iovlen = 1;
    pOp->m_msg.msg_name = &g_destAddress;
    pOp->m_msg.msg_namelen = g_destAddressLen;
    pOp->m_msg.msg_control = nullptr;
    pOp->m_msg.msg_controllen = 0;
    pOp->m_msg.msg_flags = 0;
    TIME_CALL(s_io_uring_prep_sendmsg, io_uring_prep_sendmsg(pSqe, sockFd, &pOp->m_msg, 0u));
    TIME_CALL(s_io_uring_sqe_set_data, io_uring_sqe_set_data(pSqe, pOp));

    return true;
}

// Consolodate the IoOp list operations. alloc_op moves an IoOp from the given free
// list to the given in-flight list.
IoOp* alloc_op(OpList_t& freeList, OpList_t& inFlightList)
{
    assert(!freeList.empty());
    // Get the operation struct
    IoOp* pOriginal = freeList.front();
    inFlightList.splice(inFlightList.begin(), freeList, freeList.begin());
    assert(!inFlightList.empty());
    IoOp* pOp = inFlightList.front();
    assert(pOp == pOriginal);
    pOp->m_self = inFlightList.begin();
    return pOp;
}

// Consolodate the IoOp list operations. free_op moves an IoOp from the given in-flight
// list to the given free list.
void free_op(IoOp* pOp, OpList_t& freeList, OpList_t& inFlightList)
{
    assert(!inFlightList.empty());
    freeList.splice(freeList.begin(), inFlightList, pOp->m_self);
}

// Performs the test using liburing to send and receive UDP packets at the given rate and duration
// send_uring allocates the configured number of receive buffers, and a fixed number of send
// buffers along with IoOp structures for each and populates free-lists for each. Also created
// are empty in-flight lists that hold the pointer to the buffer information while the operation
// is in the ring. When the operation completes, the buffer is returned to the free list (or
// re-posted to the ring as appropriate).
//
// At the start of the test, the configured number of receive buffers are submitted as
// UDP recvfrom operations to the ring. 
//
// The send rate is achieved by using a 1ms timeout on waiting for queue events. Each time
// the a wait completed (either for completions or a timeout), the code checks to see if there
// more send operations are due by comparing the packets that should have been sent by the
// current time with the actual number sent and submits as many of those sends as possible
// given the size of the free list.
void send_uring()
{
    // Open and bind the socket
    int sockFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockFd < 0)
    {
        int err = errno;
        std::cerr << "Error " << err << " creating socket.\n";
        exit(-1);
    }

    sockaddr_in localAddress;
    localAddress.sin_family = AF_INET;
    localAddress.sin_port = g_port; // Use same port on both side for code simplicity. Of course, this means it requires two machines (or at least two interfaces)
    localAddress.sin_addr.s_addr = INADDR_ANY;
    if (bind(sockFd, (const sockaddr*)&localAddress, sizeof(localAddress)) != 0)
    {
        int bindErr = errno;
        std::cerr << "Error " << bindErr << " binding to destination address\n";
    }

    // Allocate buffers
    size_t buffers = g_buffers;

    // Allocate memory for UDP payloads
    std::byte* pRecvBufferSpace = new std::byte[buffers*g_udpPayloadSize];
    std::byte* pSendBufferSpace = new std::byte[g_sendBuffers*g_udpPayloadSize];

    OpList_t sendFreeList;      // Holds un-used send ops
    OpList_t sendInFlightList;  // Holds send-ops that are submitted and waiting completion

    // Allocate the IoOp structures for sends
    IoOp* pSendBuffers = new IoOp[g_sendBuffers];
    IoOp* pSendBuffersEnd = pSendBuffers + g_sendBuffers;
    IoOp* pOp = pSendBuffers;
    std::byte* pNextPayload = pSendBufferSpace;
    // Populate some field in the IoOps
    for (; pOp != pSendBuffersEnd; ++pOp)
    {
        pOp->m_pPayload = pNextPayload;
        pNextPayload += g_udpPayloadSize;
        pOp->m_payloadSize = g_udpPayloadSize;
        sendFreeList.push_back(pOp);
        pOp->m_op = opSend;
    }

    OpList_t recvFreeList;      // Holds un-used receive ops
    OpList_t recvInFlightList;  // Holds receive-ops that are submitted and waiting completion

    // Allocate the IoOp structures for sends
    IoOp* pRecvBuffers = new IoOp[buffers];
    IoOp* pRecvBuffersEnd = pRecvBuffers + buffers;
    pOp = pRecvBuffers;
    pNextPayload = pRecvBufferSpace;
    // Populate some field in the IoOps
    for (; pOp != pRecvBuffersEnd; ++pOp)
    {
        pOp->m_pPayload = pNextPayload;
        pNextPayload += g_udpPayloadSize;
        pOp->m_payloadSize = g_udpPayloadSize;
        recvFreeList.push_back(pOp);
        pOp->m_op = opRecv;
    }

    // Set up the uring
    struct io_uring ring;
    std::cout << "Creating ring with " << g_ringSize << " entries.\n";
    int ret = io_uring_queue_init(g_ringSize, &ring, g_ringFlags);
    if (ret < 0)
    {
        std::cerr << "Error " << -ret << " creating ring.\n";
        throw std::system_error(-ret, std::generic_category());
    }
    std::cout << "Ring fd=" << ring.ring_fd << '\n';

    RelativeTime_t interPacketPeriod(1ms);    // At most, try to transmit packets every millisecond

    g_sendsInitiated = 0;
    g_sendsCompleted = 0;
    g_recvsInitiated = 0;
    g_recvsCompleted = 0;

    TimeAndResourceTracker resourceTracker;
    resourceTracker.begin();

    // Initiate I/O
    while (!recvFreeList.empty())
    {
        // Get the operation struct
        IoOp* pOp = alloc_op(recvFreeList, recvInFlightList);

        if (!queue_recv(&ring, pOp, sockFd))
        {
            free_op(pOp, recvFreeList, recvInFlightList);
            recvFreeList.splice(recvFreeList.begin(), recvInFlightList, pOp->m_self);
            break;
        }

        ++g_recvsInitiated;
    }

// Commented out so we don't collect max batch size on initial receives post
//    g_uringSqes += g_batchSize;
//    g_maxBatchSize = std::max(g_batchSize, g_maxBatchSize);
    std::uint64_t usSubmit = 0;
    TIME_CALL_WITH_RETURN(s_io_uring_submit, io_uring_submit(&ring), &usSubmit);
    g_submitCalls[g_nextSubmitDataPoint++] = { usSubmit, g_batchSize };
    g_batchSize = 0;

    // Loop on ring completions
    bool shutdown = false;
    bool shutdownComplete = false;
    std::uint64_t   ringOverflows = 0;
    while (!shutdownComplete)
    {
        // Check for overflow
        int ret = 0;
        TIME_CALL(s_io_uring_cq_has_overflow, ret = io_uring_cq_has_overflow(&ring));
        if (ret != 0)
        {
            ++ringOverflows;
            std::cout << "Ring (fd=" << ring.ring_fd << ") has overflowed\n";
            TIME_CALL(s_io_uring_get_events, ret = io_uring_get_events(&ring));
            if (ret != 0)
            {
                std::cerr << "io_uring_get_events() for overflow has failed with error" << -ret << '\n';
                shutdown = true;
                continue;
            }
        }

        // Reap completions until we're done
        struct __kernel_timespec timeout = { 0, 1000000 };  // 1ms timeout
        struct io_uring_cqe* pCqe = nullptr;
        AbsoluteTime_t beforeWait = Clock_t::now();
        TIME_CALL(s_io_uring_wait_cqe_timeout, ret = io_uring_wait_cqe_timeout(&ring, &pCqe, &timeout));    // Also submits because we have a timeout
        if (ret != 0 && ret != -ETIME)
        {
            std::cerr << "Peek CQE error " << -ret << std::endl;
        }
        AbsoluteTime_t now = Clock_t::now();
        update_wait_stats(now-beforeWait);

        // Dispatch completions
        TIME_CALL(s_io_uring_peek_cqe, ret = io_uring_peek_cqe(&ring, &pCqe));
        while (ret == 0)
        {
            void* pUserData;
            TIME_CALL(s_io_uring_cqe_get_data, pUserData = io_uring_cqe_get_data(pCqe));
            if (pUserData != nullptr)
            {
                IoOp* pOp = reinterpret_cast<IoOp*>(pUserData);
                if (pOp->m_op == opSend)
                {
                    // send completion, return the buffer to the free list and update the stats
                    free_op(pOp, sendFreeList, sendInFlightList);
                    ++g_sendsCompleted;
                }
                else if (pOp->m_op == opRecv)
                {
                    ++g_recvsCompleted;
                    // If it was a recv completion, re-queue the receive op
                    if (shutdown)
                    {
                        // Return the buffer to the free list
                        free_op(pOp, recvFreeList, recvInFlightList);
                    }
                    else
                    {
                        // Re-queue the receive
                        queue_recv(&ring, pOp, sockFd);
                        ++g_recvsInitiated;
                    }
                }
                else
                {
                    // Unexpected operation
                }
            }
            TIME_CALL(s_io_uring_cqe_seen, io_uring_cqe_seen(&ring, pCqe));
            TIME_CALL(s_io_uring_peek_cqe, ret = io_uring_peek_cqe(&ring, &pCqe));
        }

        if (ret != -EAGAIN && ret != 0)
        {
            std::cerr << "Peek CQE returned " << -ret << std::endl;
        }

        // Check to see if we've completed enough sends
        if (!shutdown && g_recvsCompleted >= g_packetsToSend)
        {
            // We need to begin shutting down
            shutdown = true;

            // Cancel outstanding receives
            struct io_uring_sqe* pSqe;
            TIME_CALL(s_io_uring_get_sqe, pSqe = io_uring_get_sqe(&ring));
            ++g_batchSize;
            if (pSqe == nullptr)
            {
                shutdown = false;
                continue;  // can't submit any more, so try to cancel receives on a later iteration
            }

            TIME_CALL(s_io_uring_prep_cancel_fd, io_uring_prep_cancel_fd(pSqe, sockFd, IORING_ASYNC_CANCEL_FD | IORING_ASYNC_CANCEL_ALL));
            TIME_CALL(s_io_uring_sqe_set_data, io_uring_sqe_set_data(pSqe, nullptr));
        }

        // Initiate sends if we are due for more
        if (!shutdown)
        {
            AbsoluteTime_t currentTime = Clock_t::now();
            RelativeTime_t elapsed = currentTime - resourceTracker.start_time();
            auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
            std::uint64_t packets = microseconds*g_rate/1000000;

            if (packets > g_sendsInitiated)
            {
                std::uint64_t packetsToSend = packets - g_sendsInitiated;

                while (packetsToSend != 0 && !sendFreeList.empty())
                {
                    // Get the operation struct
                    IoOp* pOp = alloc_op(sendFreeList, sendInFlightList);

                    if (!queue_send(&ring, pOp, sockFd))
                    {
                        free_op(pOp, sendFreeList, sendInFlightList);
                        break;
                    }

                    ++g_sendsInitiated;
                    --packetsToSend;
                }
            }
        }

        if (g_batchSize > 0)
        {
                g_uringSqes += g_batchSize;
                g_maxBatchSize = std::max(g_batchSize, g_maxBatchSize);
                TIME_CALL_WITH_RETURN(s_io_uring_submit, io_uring_submit(&ring), &usSubmit);
                g_submitCalls[g_nextSubmitDataPoint++] = { usSubmit, g_batchSize };
                g_batchSize = 0;
        }

        // Check for completed shutdown
        shutdownComplete = shutdown && sendInFlightList.empty() && recvInFlightList.empty();
    }

    resourceTracker.end();
    std::uint64_t loopDuration_us = static_cast<std::uint64_t>(resourceTracker.elapsed_wallclock_time() * 1000000);

    std::cout << "Uring loop executed for " << loopDuration_us/1000000.0 << "s with " << g_totalMicroseconds/1000000.0 << "s in uring wait.\n";
    std::cout << "Percentage of execution time spent in uring wait was " << 100.0*static_cast<double>(g_totalMicroseconds)/loopDuration_us << '%' << std::endl;

    if (g_dumpApiTiming)
    {
        std::ofstream apiStatsFile(g_apiTimingFilename);
        g_uringStatSet.stream(apiStatsFile, loopDuration_us);
        apiStatsFile.close();
    }

    if (g_appendStatistics)
    {
        struct stat statBuffer;
        bool writeHeader = stat(g_statisticsFilename.c_str(),&statBuffer) != 0;

        std::ofstream statsFile(g_statisticsFilename, std::ios_base::out | std::ios_base::app);

        if (writeHeader)
        {
            std::cout << "rate,recv_buffers,io_uring_wait_cqe,io_uring_wait_cqe_max,io_uring_wait_cqe_avg,io_uring_submit,io_uring_submit_max,io_uring_submit_avg,io_uring_submit_max_batch,io_uring_submit_avg_batch,overflows,waits_over_500ms,maxWait,user_time,%user,sys_time,%sys,total_time,%cpu\n";
        }

        statsFile   << g_rate << ','
                    << g_buffers << ','
                    << s_io_uring_wait_cqe_timeout.m_usAccumulated/1000000.0 << ','
                    << s_io_uring_wait_cqe_timeout.m_maxSingleCallTime/1000000.0 << ','
                    << s_io_uring_wait_cqe_timeout.m_usAccumulated/1000000.0/s_io_uring_wait_cqe_timeout.m_calls << ','
                    << s_io_uring_submit.m_usAccumulated/1000000.0 << ','
                    << s_io_uring_submit.m_maxSingleCallTime/1000000.0 << ','
                    << s_io_uring_submit.m_usAccumulated/1000000.0/s_io_uring_submit.m_calls << ','
                    << g_maxBatchSize << ','
                    << static_cast<double>(g_uringSqes)/s_io_uring_submit.m_calls << ','
                    << ringOverflows << ','
                    << g_waitsOver500ms << ','
                    << g_maxWait << ','
                    << resourceTracker.user_time() << ','
                    << resourceTracker.percent_user_time() << ','
                    << resourceTracker.sys_time() << ','
                    << resourceTracker.percent_sys_time() << ','
                    << resourceTracker.elapsed_wallclock_time() << ','
                    << resourceTracker.percent_cpu() << std::endl;
        statsFile.close();
    }

    close(sockFd);
    io_uring_queue_exit(&ring);

    if (g_dumpSubmitTiming)
    {
        // dump out timing for all stat calls
        FILE* pSubmit = fopen(g_submitTimingFilename.c_str(), "w");
        fprintf(pSubmit, "submit_time,batch_size\n");
        for (size_t i = 0; i < g_nextSubmitDataPoint; ++i)
        {
            fprintf(pSubmit, "%f,%lu\n",
                g_submitCalls[i].m_microseconds/1000000.0,
                g_submitCalls[i].m_batchSize);
        }
        fclose(pSubmit);
    }

    delete [] pSendBuffers;
    delete [] pRecvBuffers;
    delete [] pRecvBufferSpace;
    delete [] pSendBufferSpace;
}

void send_epoll()
{
    // Open a non-blocking socket
    int sockFd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sockFd < 0)
    {
        int err = errno;
        std::cerr << "Error " << err << " creating socket.\n";
        return;
    }

    // Bind to local address
    sockaddr_in localAddress;
    localAddress.sin_family = AF_INET;
    localAddress.sin_port = g_port; // Use same port on both side for code simplicity. Of course, this means it requires two machines (or at least two interfaces)
    localAddress.sin_addr.s_addr = INADDR_ANY;
    if (bind(sockFd, (const sockaddr*)&localAddress, sizeof(localAddress)) != 0)
    {
        int bindErr = errno;
        std::cerr << "Error " << bindErr << " binding to destination address\n";
        return;
    }

    int epollFd = epoll_create1(0);
    if (epollFd < 0)
    {
        int epollErr = errno;
        std::cerr << "Error " << epollErr << " creating epoll\n";
        return;
    }

    struct epoll_event sockEvents;
    sockEvents.events = EPOLLIN;    // we will leave receives enabled until shutdown, sends will get enabled as time passes and they are required
    sockEvents.data.fd = sockFd;

    int epollRet = 0;
    TIME_CALL(s_epoll_ctl, epollRet = epoll_ctl(epollFd, EPOLL_CTL_ADD, sockFd, &sockEvents));
    if (epollRet < 0)
    {
        int err = errno;
        // Handle epoll error
        std::cerr << "Error " << err << " registering epoll file descriptor and enabling receives.\n";
        return;
    }

    // Some additional stats to determine if epoll is falling behind
    std::uint64_t totalRecvEvents = 0;
    unsigned int maxRecvsPerEvent = 0;
    std::uint64_t totalSendEvents = 0;
    unsigned int maxSendsPerEvent = 0;

    TimeAndResourceTracker resourceTracker;
    resourceTracker.begin();

    // Loop on epoll ready events
    struct epoll_event events[10];
    bool shutdown = false;
    while (!shutdown)
    {
        AbsoluteTime_t beforeWait = Clock_t::now();
        int nfds = 0;
        TIME_CALL(s_epoll_wait, nfds = epoll_wait(epollFd, events, sizeof(events)/sizeof(*events), 1));
        if (nfds == -1 && errno != EINTR)
        {
            int epollErr = errno;
            std::cerr << "Error " << epollErr << " in epoll_wait\n";
            return;
        }
        AbsoluteTime_t now = Clock_t::now();
        update_wait_stats(now-beforeWait);

        for (int i = 0; i < nfds; ++i)
        {
            if (events[i].data.fd == sockFd)
            {
                if ((events[i].events & EPOLLIN) != 0)
                {
                    // Handle received packets
                    int readRet = 0;
                    int readErr = 0;
                    unsigned int recvsPerEvent = 0;
                    do
                    {
                        // read a buffer
                        std::byte           payload[g_udpPayloadSize];
                        sockaddr_storage    addressStorage;
                        socklen_t           addressLength = sizeof(addressStorage);

                        TIME_CALL(s_recvfrom, readRet = recvfrom(sockFd, payload, sizeof(payload), 0, (sockaddr*)&addressStorage, &addressLength));
                        if (readRet >= 0)
                        {
                            // Packet received.
                            ++g_recvsCompleted;
                            ++recvsPerEvent;

                            if (g_recvsCompleted >= g_packetsToSend)
                            {
                                shutdown = true;
                            }
                        }
                        else
                        {
                            readErr = errno;
                        }
                    }
                    while (readRet >= 0 && !shutdown);
                    if (readErr != EWOULDBLOCK)
                    {
                        // Either an error occurred, or we've read enough packets. Unregister the EPOLLIN event
                        sockEvents.events &= ~EPOLLIN;
                        TIME_CALL(s_epoll_ctl, epollRet = epoll_ctl(epollFd, EPOLL_CTL_MOD, sockFd, &sockEvents));
                        if (epollRet < 0)
                        {
                            int err = errno;
                            // Handle epoll error
                            std::cerr << "Error " << err << " disabling the receive event.\n";
                            shutdown = true;
                        }
                    }

                    ++totalRecvEvents;
                    maxRecvsPerEvent = std::max(maxRecvsPerEvent, recvsPerEvent);
                }
                else if ((events[i].events & EPOLLOUT) != 0)
                {
                    if (!shutdown)
                    {
                        // Handle sending packets
                        AbsoluteTime_t currentTime = Clock_t::now();
                        RelativeTime_t elapsed = currentTime - resourceTracker.start_time();
                        auto seconds = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
                        std::uint64_t packets = seconds*g_rate/1000000;
                        unsigned int sendsPerEvent = 0;

                        if (packets > g_sendsCompleted)
                        {
                            std::uint64_t packetsToSend = packets - g_sendsCompleted;

                            ssize_t sendRet = 0;
                            int sendErr = 0;

                            while (packetsToSend != 0 && sendRet >= 0)
                            {
                                std::byte           payload[g_udpPayloadSize];

                                TIME_CALL(s_sendto, sendRet = sendto(sockFd, payload, sizeof(payload), 0, (struct sockaddr *)&g_destAddress, g_destAddressLen));
                                if (sendRet < 0)
                                {
                                    sendErr = errno;
                                    break;
                                }

                                ++g_sendsCompleted;
                                ++sendsPerEvent;
                                --packetsToSend;
                            }
                            if ((sendRet >= 0 && packetsToSend == 0) || (sendRet < 0 && sendErr != EWOULDBLOCK))
                            {
                                // Either we sent all the packets we need to send right now or there was an error, so disable the send event
                                sockEvents.events &= ~EPOLLOUT;
                                TIME_CALL(s_epoll_ctl, epollRet = epoll_ctl(epollFd, EPOLL_CTL_MOD, sockFd, &sockEvents));
                                if (epollRet < 0)
                                {
                                    // Handle error
                                    int err = errno;
                                    std::cerr << "Error " << err << " disabling the send event.\n";
                                    shutdown = true;
                                }
                                if (sendRet < 0 && sendErr != EWOULDBLOCK)
                                {
                                    shutdown = true;
                                }
                            }
                        }
                        ++totalSendEvents;
                        maxSendsPerEvent = std::max(maxSendsPerEvent, sendsPerEvent);
                    }
                }
                else if ((events[i].events & EPOLLHUP) != 0)
                {
                    // Network lost, shutdown
                    shutdown = true;
                }

            }

        }

        if (!shutdown)
        {
            // Check to see if we need to re-enable sends because packets are scheduled to be sent
            AbsoluteTime_t currentTime = Clock_t::now();
            RelativeTime_t elapsed = currentTime - resourceTracker.start_time();
            auto seconds = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();

            std::uint64_t packets = seconds*g_rate/1000000;
            if (packets > g_sendsCompleted)
            {
                sockEvents.events |= EPOLLOUT;
                epollRet = epoll_ctl(epollFd, EPOLL_CTL_MOD, sockFd, &sockEvents);
                if (epollRet < 0)
                {
                    // Handle error
                    int err = errno;
                    std::cerr << "Error " << err << " enabling the send event.\n";
                    shutdown = true;
                }
            }
        }
    }

    resourceTracker.end();
    std::uint64_t loopDuration_us = static_cast<std::uint64_t>(resourceTracker.elapsed_wallclock_time()*1000000);


    if (g_dumpApiTiming)
    {
        std::ofstream apiStatsFile(g_apiTimingFilename);
        g_epollStatSet.stream(std::cout, loopDuration_us);
        apiStatsFile.close();
    }

    // print some stats
    std::cout << "Receive Events: " << totalRecvEvents << ", Recvs per recv event: max=" << maxRecvsPerEvent << " avg=" << g_recvsCompleted/totalRecvEvents << std::endl;
    std::cout << "Send Events: " << totalSendEvents << ", Sends per send event: max=" << maxSendsPerEvent << " avg=" << g_sendsCompleted/totalSendEvents << std::endl;


    if (g_appendStatistics)
    {
        struct stat statBuffer;
        bool writeHeader = stat(g_statisticsFilename.c_str(),&statBuffer) != 0;

        std::ofstream statsFile(g_statisticsFilename, std::ios_base::out | std::ios_base::app);

        if (writeHeader)
        {
            std::cout << "rate,waits_over_500ms,maxWait,user_time,%user,sys_time,%sys,total_time,%cpu\n";
        }

        statsFile   << g_rate << ','
                    << g_waitsOver500ms << ','
                    << g_maxWait << ','
                    << resourceTracker.user_time() << ','
                    << resourceTracker.percent_user_time() << ','
                    << resourceTracker.sys_time() << ','
                    << resourceTracker.percent_sys_time() << ','
                    << resourceTracker.elapsed_wallclock_time() << ','
                    << resourceTracker.percent_cpu() << std::endl;
        statsFile.close();
    }

    epoll_ctl(epollFd, EPOLL_CTL_DEL, sockFd, nullptr);

    // Free resources
    close(sockFd);

    // close epoll
    close(epollFd);
}

void usage(const char* pErrorMsg = nullptr)
{
    if (pErrorMsg != nullptr)
    {
        std::cerr << pErrorMsg << std::endl;
    }
    std::cout << "usage: msim2 command args\n"
                 "  Commands\n"
                 "    send_uring <dest_ip> [flags] <port> <packets_per_second> <duration_in_seconds> [<buffers> [<packet_size>]]\n"
                 "    send_epoll <dest_ip> [flags] <port> <packets_per_second> <duration_in_seconds> [<packet_size>]\n"
                 "  Flags                                                                         \n"
                 "    --dump_submit_timing <filename>    - Applies only to send_uring. Creates a\n"
                 "                                         csv file of all io_uring_submit()\n"
                 "                                         call timings including batch size.\n"
                 "    --dump_api_timing <filename>       - Creates a file containing a summary of\n"
                 "                                         of the timing for API calls (liburing\n"
                 "                                         or epoll/recvfrom/sendto).\n"
                 "    --append_statistics <filename>     - If <filename> doesn't exist, it's\n"
                 "                                         created and both a header line, and a\n"
                 "                                         statistics line are written. If\n"
                 "                                         <filename> already exists, the\n"
                 "                                         statistics line is appended.\n"
                 "    --ring_size <size>                 - Size passed to io_uring_queue_init().\n"
                 "                                         defaults to 8000\n"
                 "                                         only applies to send_uring\n"
                ;
}

using CmdLineParams_t = std::vector<char*>;

bool process_flag(CmdLineParams_t& cmdLineParams, size_t i)
{
    const char* pFlagName = cmdLineParams[i]+1;
    // --dump_submit_timing
    if (strcmp(pFlagName,"-dump_submit_timing") == 0)
    {
        cmdLineParams.erase(cmdLineParams.begin() + i);
        if (cmdLineParams.size() > i)
        {
            g_dumpSubmitTiming = true;
            g_submitTimingFilename = cmdLineParams[i];
            cmdLineParams.erase(cmdLineParams.begin() + i);
        }
        else
        {
            std::cerr << "No filename parameter specified for flag '--dump_submit_timing'\n";
            return false;
        }
    }
    // --dump_api_timing
    else if (strcmp(pFlagName, "-dump_api_timing") == 0)
    {
        cmdLineParams.erase(cmdLineParams.begin() + i);
        if (cmdLineParams.size() > i)
        {
            g_dumpApiTiming = true;
            g_apiTimingFilename = cmdLineParams[i];
            cmdLineParams.erase(cmdLineParams.begin() + i);
        }
        else
        {
            std::cerr << "No filename parameter specified for flag '--dump_api_timing'\n";
            return false;
        }
    }
    // --dump_statistics
    else if (strcmp(pFlagName, "-append_statistics") == 0)
    {
        cmdLineParams.erase(cmdLineParams.begin() + i);
        if (cmdLineParams.size() > i)
        {
            g_appendStatistics = true;
            g_statisticsFilename = cmdLineParams[i];
            cmdLineParams.erase(cmdLineParams.begin() + i);
        }
        else
        {
            std::cerr << "No filename parameter specified for flag '-" << pFlagName << "'\n";
            return false;
        }
    }
    else if (strcmp(pFlagName, "-ring_size") == 0)
    {
        cmdLineParams.erase(cmdLineParams.begin() + i);
        if (cmdLineParams.size() > i)
        {
            g_ringSize = atoi(cmdLineParams[i]);
            cmdLineParams.erase(cmdLineParams.begin() + i);
        }
        else
        {
            std::cerr << "No size parameter specified for flag '-" << pFlagName << "'\n";
            return false;
        }
    }
    else
    {
        std::cerr << "Unknown flag '-" << pFlagName << std::endl;
        return false;
    }
    return true;
}

// Handle and remove all optional parameters (eg. "-flag" or "-flag value") leaving only the positional arguments
bool process_cmdline_flags(CmdLineParams_t& cmdLineParams)
{
    for (size_t i = 0; i < cmdLineParams.size();)
    {
        const char* pParam = cmdLineParams[i];
        if (pParam != nullptr && *pParam == '-')
        {
            // Flag, and any options will be removed from the vector
            if (!process_flag(cmdLineParams, i))
            {
                return false;
            }
        }
        else
        {
            ++i;    // This was a positional param, move to the next param
        }
    }
    return true;
}

int main(int argc, char* argv[])
{
    CmdLineParams_t cmdLineParams(argv+1, argv+argc);
    if (!process_cmdline_flags(cmdLineParams))
    {
        usage();
        return 0;
    }

    unsigned int packetsPerSecond = 2000*50; // equivalent to 2000 streams at 50 packets/s per stream

    if (cmdLineParams.size() < 1)
    {
        usage();
        return 0;
    }

    std::string command(cmdLineParams[0]);

    if (command ==  "send_uring")
    {
        try
        {
            if (cmdLineParams.size() < 4)
            {
                usage();
                return 0;
            }
            g_destAddressLen = resolve_address(&g_destAddress, cmdLineParams[1], cmdLineParams[2]);
            g_port = ntohs(reinterpret_cast<const sockaddr_in*>(&g_destAddress)->sin_port);
            g_rate = atoi(cmdLineParams[3]);
            if (g_rate < 1)
            {
                usage("invalid packet rate");
                return 0;
            }
            g_packetsToSend = atoi(cmdLineParams[4])*g_rate;

            if (cmdLineParams.size() > 5)
            {
                g_buffers = atoi(cmdLineParams[5]);
                if (g_buffers < 1)
                {
                    usage("invalid number of receive buffers");
                    return 0;
                }
            }
            else
            {
                g_buffers = g_rate/50;  // Approx equivalent to 1 receive buffer per 50 packets/s media stream
            }

            if (cmdLineParams.size() > 6)
            {
                // override the default packet size
                g_udpPayloadSize = atoi(cmdLineParams[6]);
                if (g_udpPayloadSize < 1)
                {
                    usage("invalid UDP payload size");
                    return 0;
                }
            }

            std::cout << "Starting send_uring test at " << g_rate << " packets/s and " << g_buffers << " receive buffers\n";
            send_uring();

            std::cout << "send_uring test completed\n";

            std::cout << g_sendsInitiated << " sends initiated " << g_sendsCompleted << " completed successfully\n";
            std::cout << g_recvsInitiated << " receives initiated " << g_recvsCompleted << " completed sucessfully\n";
            std::cout << g_waitCalls << " total wait calls\n"
                    << g_waitsOver500ms << " waits over 500ms\n"
                    << "Maximum wait was " << g_maxWait/1000.0 << "ms\n"
                    << "Average wait was " << g_totalMicroseconds/1000.0/g_waitCalls << "ms\n";
        }
        catch(const std::exception& e)
        {
            std::cerr << e.what() << '\n';
        }
    }
    else if (command == "send_epoll")
    {
        try
        {
            if (cmdLineParams.size() < 4)
            {
                usage();
                return 0;
            }
            g_destAddressLen = resolve_address(&g_destAddress, cmdLineParams[1], cmdLineParams[2]);
            g_port = ntohs(reinterpret_cast<const sockaddr_in*>(&g_destAddress)->sin_port);
            g_rate = atoi(cmdLineParams[3]);
            if (g_rate < 1)
            {
                usage("invalid packet rate");
                return 0;
            }
            g_packetsToSend = atoi(cmdLineParams[4])*g_rate;
            if (cmdLineParams.size() > 5)
            {
                // override the default packet size
                g_udpPayloadSize = atoi(cmdLineParams[5]);
                if (g_udpPayloadSize < 1)
                {
                    usage("invalid UDP payload size");
                    return 0;
                }
            }

            std::cout << "Starting send_epoll test at " << g_rate << " packets/s\n";
            send_epoll();

            std::cout << "send_epoll test completed\n";

            std::cout << g_sendsCompleted << " sends completed successfully\n";
            std::cout << g_recvsCompleted << " receives completed sucessfully\n";
            std::cout << g_waitCalls << " total epoll_wait calls\n"
                        << g_waitsOver500ms << " waits over 500ms\n"
                        << "Maximum wait was " << g_maxWait/1000.0 << "ms\n"
                        << "Average wait was " << g_totalMicroseconds/1000.0/g_waitCalls << "ms\n";
        }
        catch(const std::exception& e)
        {
            std::cerr << e.what() << '\n';
        }
        
    }
    else
    {
        usage();
        return 0;
    }
}
