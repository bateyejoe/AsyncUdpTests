#include <unistd.h>
#include <cstdint>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <iostream>
#include <memory>
#include <list>
#include <system_error>

// Holds a payload and address info between receiving a packet, and re-transmitting it
class Buffer
{
public:
    Buffer(std::byte*   pBuffer,
           size_t       bufferCapacity) :
        m_bufferCapacity(bufferCapacity),
        m_pBuffer(pBuffer)
    {
    }

    // We should never need to copy a buffer
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    std::byte* payload()
    {
        return m_pBuffer;
    }
    size_t capacity() const
    {
        return m_bufferCapacity;
    }
    size_t size() const
    {
        return m_bufferSize;
    }
    void set_size(size_t bufferSize)
    {
        m_bufferSize = bufferSize;
    }
    sockaddr* address_buffer()
    {
        return (sockaddr*)&m_address;
    }
    socklen_t address_capacity() const
    {
        return sizeof(m_address);
    }
    socklen_t address_size() const
    {
        return m_addressLength;
    }
    socklen_t* address_size_ptr()
    {
        return &m_addressLength;
    }
    void reset_for_receive()
    {
        m_addressLength = sizeof(m_address);
        m_bufferSize = 0;
    }

private:
    const size_t        m_bufferCapacity;                       ///< Total buffer capacity
    std::byte*          m_pBuffer;                              ///< Pointer to buffer payload (lifetime maintained elsewhere)
    size_t              m_bufferSize = 0;                       ///< Size of valid buffer data in buffer
    sockaddr_storage    m_address;                              ///< Space for storing the remote address
    socklen_t           m_addressLength = sizeof(m_address);    ///< Size of address stored in m_address
};  // class Buffer

using BufferList_t = std::list<Buffer>;     // std::list of Buffer objects. Buffers can be "spliced" efficiently between list instances.

void epoll_reflect(int sockFd, size_t bufferSize, size_t numBuffers)
{
    // Set up the buffers
    BufferList_t freePool;      // Buffers to use for receives
    BufferList_t sendQueue;     // Received buffers, waiting to send

    // Allocate a bunch of space for buffer payloads
    std::unique_ptr<std::byte[]>    pBufferSpace(new std::byte[numBuffers*bufferSize]);

    // Fill up the free list pool
    for (size_t i = 0; i < numBuffers; ++i)
    {
        freePool.emplace_front(&pBufferSpace[i*bufferSize], bufferSize);
    }

    // Set up epoll
    int epollFd = epoll_create1(0);
    if (epollFd < 0)
    {
        int err = errno;
        throw std::system_error(err, std::system_category(), "Creating epoll file descriptor");
    }

    // Register the socket with epoll and enable receive events
    struct epoll_event sockEvents;
    sockEvents.events = EPOLLIN;
    sockEvents.data.fd = sockFd;
    if (epoll_ctl(epollFd, EPOLL_CTL_ADD, sockFd, &sockEvents) < 0)
    {
        int err = errno;
        close(epollFd);
        throw std::system_error(err, std::system_category(), "Registering socket with epoll");
    }

    // some stats
    std::uint64_t   packetsReceived = 0;
    std::uint64_t   packetsSent = 0;

    // Local method to add a received buffer to the send queue and, if necessary, call epoll to enable sends
    auto queue_send = [&](BufferList_t::iterator freeBuf)
    {
        bool epollChange = false;
        if (sendQueue.empty())
        {
            // enable sends
            sockEvents.events |= EPOLLOUT;
            epollChange = true;
        }
        sendQueue.splice(sendQueue.end(), freePool, freeBuf);
        if (freePool.empty())
        {
            // disable receives events until we get some more free buffers
            sockEvents.events &= ~EPOLLIN;
            epollChange = true;
        }
        if (epollChange)
        {
            if (epoll_ctl(epollFd, EPOLL_CTL_MOD, sockFd, &sockEvents) < 0)
            {
                int err = errno;
                close(epollFd);
                throw std::system_error(err, std::system_category(), "Error in epoll_ctl()");
            }
        }
    };

    // Local method to return a sent buffer to the free list to use for a future receive
    auto free_send_buffer = [&](BufferList_t::iterator sendBuf)
    {
        bool epollChange = false;
        if (freePool.empty())
        {
            // enable receives
            sockEvents.events |= EPOLLIN;
            epollChange = true;
        }
        freePool.splice(freePool.begin(), sendQueue, sendBuf);
        if (sendQueue.empty())
        {
            // disable send events until more send buffers are queued
            sockEvents.events &= ~EPOLLOUT;
            epollChange = true;
        }
        if (epollChange)
        {
            if (epoll_ctl(epollFd, EPOLL_CTL_MOD, sockFd, &sockEvents) < 0)
            {
                int err = errno;
                close(epollFd);
                throw std::system_error(err, std::system_category(), "Error in epoll_ctl()");
            }
        }
    };

    struct epoll_event  events[10]; // Room for 10 events, but we only have on fd, so probably will only ever use 1
    while (true)
    {
        // Wait indefinitely for events
        int nfds = epoll_wait(epollFd, events, sizeof(events)/sizeof(*events), -1);
        if (nfds == -1)
        {
            if (errno != EINTR) // Ignore EINTR errors, fail the test on any other errors
            {
                int err = errno;
                close(epollFd);
                throw std::system_error(err, std::system_category(), "Error in epoll_wait()");
            }
        }

        // Process the events
        for (int i = 0; i < nfds; ++i)
        {
            if (events[i].data.fd == sockFd)
            {
                if ((events[i].events & EPOLLIN) != 0)
                {
                    // Handle received packets
                    while (!freePool.empty())
                    {
                        BufferList_t::iterator pBuffer = freePool.begin();
                        Buffer& buffer = *pBuffer;
                        buffer.reset_for_receive();
                        int readRet = recvfrom( sockFd,
                                                buffer.payload(),
                                                buffer.capacity(),
                                                0,
                                                buffer.address_buffer(),
                                                buffer.address_size_ptr());
                        if (readRet >= 0)
                        {
                            // We've received a buffer, move it to the send queue
                            buffer.set_size(readRet);
                            queue_send(pBuffer);
                            ++packetsReceived;
                        }
                        else if (errno == EWOULDBLOCK)
                        {
                            // No more packets available to read
                            break;
                        }
                        else
                        {
                            // Some error
                            int err = errno;
                            close(epollFd);
                            throw std::system_error(err, std::system_category(), "Error in recvfrom()");
                        }
                    }
                }
                else if ((events[i].events & EPOLLOUT) != 0)
                {
                    while (!sendQueue.empty())
                    {

                        BufferList_t::iterator pBuffer = sendQueue.begin();
                        Buffer& buffer = *pBuffer;
                        int sendRet = sendto(sockFd,
                                            buffer.payload(),
                                            buffer.size(),
                                            0,
                                            buffer.address_buffer(),
                                            buffer.address_size());
                        if (sendRet >= 0)
                        {
                            // Return send buffer to free pool
                            free_send_buffer(pBuffer);
                            ++packetsSent;
                        }
                        else if (errno == EWOULDBLOCK)
                        {
                            break;
                        }
                        else
                        {
                            // Some error
                            int err = errno;
                            close(epollFd);
                            throw std::system_error(err, std::system_category(), "Error in sendto()");
                        }
                    }
                }
            }
        }
    }


    close(epollFd);
}

void usage(const char* msg = nullptr)
{
    if (msg != nullptr)
    {
        std::cerr << msg << std::endl;
    }
    std::cout << "usage: reflect <port> <buffers> [<buffer_size>]\n";
}

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

int main(int argc, const char* argv[])
{
    if (argc < 3)
    {
        usage();
        return 0;
    }

    std::uint16_t port = atoi(argv[1]);
    size_t numBuffers = atoi(argv[2]);
    size_t bufferSize = (argc > 3) ? atoi(argv[3]) : 1500;

    std::cout << "Listening on port " << port << ". Beffers=" << numBuffers << " Size=" << bufferSize << std::endl;

    int sockFd = -1;
    try
    {
        // Open socket
        sockFd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
        if (sockFd < 0)
        {
            int err = errno;
            std::cerr << "Error " << err << " creating socket.\n";
            return 0;
        }

        // Bind to local address
        sockaddr_in localAddress;
        localAddress.sin_family = AF_INET;
        localAddress.sin_port = htons(port);
        localAddress.sin_addr.s_addr = INADDR_ANY;
        if (bind(sockFd, (const sockaddr*)&localAddress, sizeof(localAddress)) != 0)
        {
            int bindErr = errno;
            std::cerr << "Error " << bindErr << " binding to destination address\n";
            return 0;
        }

        // Start the epoll loop
        epoll_reflect(sockFd, bufferSize, numBuffers);

        close(sockFd);
    }
    catch(const std::exception& e)
    {
        if (sockFd != -1)
            close(sockFd);
        std::cerr << e.what() << '\n';
    }
}
