/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2016 Intel Corporation
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/queue.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>

#include <rte_keepalive.h>

#include <shm.h>

#define MAX_TIMEOUTS 4
#define SEM_TIMEOUT_SECS 2

/*
shm_open 和 mmap 结合使用是一种常见的方法来创建和管理共享内存段，尤其是在 Linux 和其他类 Unix 操作系统中。这种操作的主要目的是允许多个进程共享内存区域，从而实现进程间通信（IPC）。

以下是使用 shm_open 和 mmap 创建并映射共享内存的操作流程：

创建或打开共享内存对象:

使用 shm_open 函数打开或创建一个共享内存对象。
通常会指定一个唯一的名称来标识这个共享内存对象，以便其他进程能够找到它。
设置共享内存对象的大小:

使用 ftruncate 或 fallocate 函数来设置共享内存对象的大小。
映射共享内存到进程地址空间:

使用 mmap 函数将共享内存对象映射到进程的地址空间。
这样就可以像操作普通的内存变量一样读写共享内存中的数据了。
使用共享内存:

多个进程可以通过映射同一共享内存对象来共享数据。
解除映射和关闭共享内存对象:

当不再需要使用共享内存时，使用 munmap 函数解除映射。
使用 close 函数关闭共享内存对象的文件描述符。
如果不再有进程需要使用该共享内存，可以使用 shm_unlink 函数删除共享内存对象。
在提供的 C 函数 ka_shm_create 中，正是实现了上述流程的一部分。函数首先尝试使用 shm_open 打开一个名为 RTE_KEEPALIVE_SHM_NAME 的共享内存对象，并以读写模式 (O_RDWR) 打开。接着，使用 mmap 将这个共享内存对象映射到当前进程的地址空间中。如果成功映射，函数返回映射的地址；如果失败，则打印错误信息并返回 NULL。

这种方法非常适合需要在多个进程间高效共享大量数据的应用场景。
*/
static struct rte_keepalive_shm *ka_shm_create(void)
{
	// 为什么shm_open不用指定共享内存大小？
	/*
	shm_open函数用于创建或打开一个共享内存对象，它并不直接指定共享内存的大小，而是返回一个文件描述符（fd），
	后续通过ftruncate或fallocate函数来设置共享内存的大小。
	这样设计是为了更好地控制内存分配，允许在创建共享内存对象后根据实际需求动态调整大小。
	*/
	int fd = shm_open(RTE_KEEPALIVE_SHM_NAME, O_RDWR, 0666);
	size_t size = sizeof(struct rte_keepalive_shm);
	struct rte_keepalive_shm *shm;

	if (fd < 0)
		printf("Failed to open %s as SHM:%s\n",
			RTE_KEEPALIVE_SHM_NAME,
		strerror(errno));
	else {
		shm = (struct rte_keepalive_shm *) mmap(
			0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		// 为什么需要close？会不会导致共享内存被释放？
		/*
		close(fd)的原因是避免文件描述符泄露，不会影响已映射的共享内存。
		mmap通过MAP_SHARED标志将共享内存绑定到进程地址空间，因此关闭文件描述符不影响映射的有效性。
		
		1。资源管理：确保文件描述符被正确关闭，防止资源泄露。
		2。可维护性：即使在使用mma印之后，显式关闭文件描述符可以提高代码的清晰度和可维护性。
		3。兼容性和安全性：在某些情况下，如系统资源紧张时，及时释放不再使用的文件描述符有助于提升程序的健壮性和安全性。
		总之，这是一种推荐的做法，有助于编写更可靠、更高效的代码。
		*/
		close(fd);
		if (shm == MAP_FAILED)
			printf("Failed to mmap SHM:%s\n", strerror(errno));
		else
			return shm;
	}

	/* Reset to zero, as it was set to MAP_FAILED aka: (void *)-1 */
	shm = 0;
	return NULL;
}

int main(void)
{
	// 获取在l2fwd-keepalive/main.rte_keepalive_shm_create创建的共享内存
	struct rte_keepalive_shm *shm = ka_shm_create();
	struct timespec timeout = { .tv_nsec = 0 };
	int idx_core;
	int cnt_cores;
	uint64_t last_seen_alive_time = 0;
	uint64_t most_recent_alive_time;
	int cnt_timeouts = 0;
	int sem_errno;

	if (shm == NULL) {
		printf("Unable to access shared core state\n");
		return 1;
	}
	while (1) {
		most_recent_alive_time = 0;
		for (idx_core = 0; idx_core < RTE_KEEPALIVE_MAXCORES;
				idx_core++)
			if (shm->core_last_seen_times[idx_core] >
					most_recent_alive_time)
				most_recent_alive_time =
					shm->core_last_seen_times[idx_core];

		timeout.tv_sec = time(NULL) + SEM_TIMEOUT_SECS;
		if (sem_timedwait(&shm->core_died, &timeout) == -1) {
			/* Assume no core death signals and no change in any
			 * last-seen times is the keepalive monitor itself
			 * failing.
			 */
			sem_errno = errno;
			last_seen_alive_time = most_recent_alive_time;
			if (sem_errno == ETIMEDOUT) {
				// sem_timedwait因等待超时而返回
				if (last_seen_alive_time ==
						most_recent_alive_time &&
						cnt_timeouts++ >
						MAX_TIMEOUTS) {
					printf("No updates. Exiting..\n");
					break;
					}
			} else
				printf("sem_timedwait() error (%s)\n",
					strerror(sem_errno));
			continue;
		}
		cnt_timeouts = 0;

		cnt_cores = 0;
		for (idx_core = 0; idx_core < RTE_KEEPALIVE_MAXCORES;
				idx_core++)
			if (shm->core_state[idx_core] == RTE_KA_STATE_DEAD)
				cnt_cores++;
		if (cnt_cores == 0) {
			/* Can happen if core was restarted since Semaphore
			 * was sent, due to agent being offline.
			 */
			printf("Warning: Empty dead core report\n");
			continue;
		}

		printf("%i dead cores: ", cnt_cores);
		for (idx_core = 0;
				idx_core < RTE_KEEPALIVE_MAXCORES;
				idx_core++)
			if (shm->core_state[idx_core] == RTE_KA_STATE_DEAD)
				printf("%d, ", idx_core);
		printf("\b\b\n");
	}
	if (munmap(shm, sizeof(struct rte_keepalive_shm)) != 0)
		printf("Warning: munmap() failed\n");
	return 0;
}
