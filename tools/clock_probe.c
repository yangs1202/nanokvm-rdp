/* Read-only clock offset probe. Each input line returns realtime and monotonic
 * microseconds. Keep one connection open to exclude SSH/kubectl startup cost. */
#include <stdio.h>
#include <stdint.h>
#include <time.h>

static uint64_t microseconds(clockid_t clock)
{
	struct timespec now;
	if (clock_gettime(clock, &now) != 0) return 0;
	return (uint64_t)now.tv_sec * 1000000U + (uint64_t)now.tv_nsec / 1000U;
}

int main(void)
{
	char line[32];
	while (fgets(line, sizeof(line), stdin))
	{
		const uint64_t wall = microseconds(CLOCK_REALTIME);
		const uint64_t mono = microseconds(CLOCK_MONOTONIC);
		if (!wall || !mono) return 1;
		printf("%llu %llu\n", (unsigned long long)wall, (unsigned long long)mono);
		if (fflush(stdout) != 0) return 1;
	}
	return ferror(stdin) ? 1 : 0;
}
