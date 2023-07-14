# io_uring vs epoll UDP Test
## Background
This is a comparison of epoll vs io_uring for a very specific network use case. My application, currently implemented using epoll for the asynchronous networking, is a media server that transmits and receives many small UDP packets (typically a 172 byte UDP payload). Each stream of packets is fairly lightweight, needing to transmit/receive only 50 packets per second.

The media server has other things to do (file I/O, audio transcoding, mixing, analysis etc), so the amount of CPU used by the I/O directly impacts the server’s capacity. In fact, the server’s capacity is still, typically, bound by the amount of CPU available rather than our network throughput. Because of this, the CPU usage of the networking is of particular importance to my application. For that reason, this test focuses on the CPU usage.

## My First Test App
My first attempt at observing the io_uring performance for my application was to port an existing test application over to io_uring. This test application simulated the network pattern of my server by sending and receiving these UDP packet streams. Each packet stream has its own file descriptor and uses it’s own unique UDP port. These sets of streams and file descriptors are distributed evenly among the set of io_urings (one for each CPU core, each having it’s own dedicated thread).

Where I could manage about 8000 simultaneous streams with the epoll version of this test app, the io_uring exhibited some pretty bad behaviors at around 1000 streams. In particular, io_uring_wait_cqe_timeout(), which I always pass in a 1ms timeout, would sometimes block for thousands of milliseconds, and on all rings/threads. For my application, this is a deal-killer as a 1-second block means every stream on that ring has fallen behind by 50 packets.

With all the cores/rings/thread/file descriptors, it was difficult to narrow down what was going on. To simplify things, I created a new test application that uses a single file descriptor on a single thread on single io_uring ring and varied the packet rate (and also the number of outstanding receive operations). For comparison, I also created an epoll implementation of this test in the same application.

## The New Test App
My test app creates a single socket, on a single ring using a single thread and sends and receives small (172-byte UDP payload) UDP packets. In the test runs below, the sending and receiving rates are 200,000 packets per second. The only thing that varies in these four runs is the number of receive buffers (or maximum amount of ring receive operations in flight). Upon handling a receive completion, a new receive operation is posted meaning the app tries to keep all receive buffers posted to the ring at all times.

In general, the io_uring implementation consumes far more CPU than the epoll implementation, but I also noticed that the CPU usage, for the same packet rate, generally goes up as the number of outstanding receive operations is increased.

This is important to my real-world application as I normally have many low packet rate (50 packets per second) streams, each on its own socket. This test app’s packet rate of 200,000 packets per second is equivalent to 4000 individual 50 packet per second streams. My application would typically have 8 receive buffers per stream meaning I would have, at any given time, around 32,000 receive operations pending in the ring. Even if I lowered this down to 2 buffers per stream, that would still be 8000 pending receives.

Interestingly, the more receive buffers I have greatly affect the number of times I have to call io_uring_submit() and io_uring_wait_cqe() (where the majority of this test app’s time is spent). For 1000 receive buffers, I call each of those methods about 40,000 times each whereas if I have 2000 receive buffers, I call those two methods only about 5000 times each. Even though io_uring_submit() is being called an order of magnitude less, the time spent in that call doubles with 2000 receive buffers vs 1000. This is likely due to simply running low on CPU. I call io_uring_submit(), it takes several milliseconds and when it returns, I’m now somewhat behind on sends, so the next io_uring_submit() will have a larger batch of new operations, which causes io_uring_submit() to take even longer, putting me behind again, etc.

The epoll version of this test app can essentially saturate the 1Gbps link at a packet rate of 500,000 packets per second and cpu core usage of 72%. The io_uring implementation doesn’t really get past 200,000 packets per second for any reasonable number of receive buffers.

The conclusion I draw from this is that, for an application like mine that requires a lot of outstanding I/O operations, the overhead of the internal ring management greatly exceeds any benefit gained by avoiding some system calls.
![Screenshot CPU and Network Utilization during test runs](https://github.com/bateyejoe/AsyncUdpTests/blob/main/images/io_uring_200000.png)

### 200 Pending Receives

/usr/bin/time stats:
10.5% user, 100.4% system 110% cpu usage
|Liburing Call               |time(s)      |Total calls     |% of run time     |Max Time(ms)     |
|----------------------------|-------------|----------------|------------------|-----------------|
|io_uring_submit             |4.50182      |193503          |44.7525           |0.387            |
|io_uring_wait_cqe_timeout   |4.32691      |241453          |43.0138           |1.123            |
Submit that included the cancel the entire file descriptor tool 56.047ms.

### 1000 Pending Receives

/usr/bin/time stats:
8.26% user, 73.4% system 81.7% cpu usage

|Liburing Call               |time(s)      |Total calls     |% of run time     |Max Time(ms)     |
|----------------------------|-------------|----------------|------------------|-----------------|
|io_uring_submit             |3.33066      |38048           |32.7988           |13.893           |
|io_uring_wait_cqe_timeout   |5.87482      |45588           |57.8524           |3.488            |

Submit that included the cancel the entire file descriptor tool 95.619ms.

### 2000 Pending Receives

/usr/bin/time stats:
8.38% user, 105.0% system 113.4% cpu usage

|Liburing Call               |time(s)      |Total calls     |% of run time     |Max Time(ms)     |
|----------------------------|-------------|----------------|------------------|-----------------|
|io_uring_submit             |8.20975      |5256            |81.0151           |3.066            |
|io_uring_wait_cqe_timeout   |0.089418     |5276            |0.882392          |3.474            |

Submit that included the cancel the entire file descriptor tool 113.716ms.

### 4000 Pending Receives

/usr/bin/time stats:
8.04% user, 102.5% system 110.6% cpu usage

|Liburing Call               |time(s)      |Total calls     |% of run time     |Max Time(ms)     |
|----------------------------|-------------|----------------|------------------|-----------------|
|io_uring_submit             |11.2935      |3426            |89.945            |13.741           |
|io_uring_wait_cqe_timeout   |0.210859     |3632            |1.67934           |11.454           |

Submit that included the cancel the entire file descriptor tool 135.215ms.

## Notes:
- The initial submit, and the submit that cancels all outstanding I/O for the file descriptor are not included in the Liburing Call stats above as they tended to be the largest.

- The cancel operation, for a file descriptor, seems to be proportional to the number of outstanding I/O operations and can get quite lengthy as noted above. For my real-world application, this isn’t a concern because each cancel would be only for a handful of I/O operations.

- I did not see the long-term blocking of io_uring_wait_cqe_timeout() in this single-thread, single-ring implementation, even at higher data rates. I suspect this is related to some interaction between multiple rings on multiple threads/cores under load. 

## Epoll Result
![Screenshot of CPU and Network Utilization for epoll test](https://github.com/bateyejoe/AsyncUdpTests/blob/main/images/epoll_200000.png)
Above is a run using epoll with the same 200,000 packets per second data rate.

## Test Setup
The tests were run on two systesm. The system running the msim2 test app was run on an Intel Core i7-9700K, 8-core CPU, 16GB RAM on Ubuntu 22.04 LTS with Kernal version 5.19.0-45-generic and the reflec application was run on an Intel Core i9-9900K 16 core CPU, 64GB RAM on Ubuntu 22.04.2 LTS with Kernal version 5.19.0-45-generic.
Each test was run 10 times and the average results are shown in the tables. The screenshots are of a typical, single run.
