#include "gateway_internal.h"

#include <stdarg.h>

uint64_t monotonic_milliseconds(void)
{
	return (uint64_t)GetTickCount64();
}

void gateway_log(const char* level, const char* format, ...)
{
	char message[4096];
	va_list arguments;
	va_start(arguments, format);
	(void)vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	(void)fprintf(stderr, "%s: %s: at_ms=%llu %s\n", TAG, level,
	              (unsigned long long)monotonic_milliseconds(), message);
}
