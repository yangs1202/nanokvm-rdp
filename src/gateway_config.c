#include "gateway_config.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void set_error(char* error, size_t error_size, const char* format, ...)
{
	if (error && error_size > 0)
	{
		va_list arguments;
		va_start(arguments, format);
		(void)vsnprintf(error, error_size, format, arguments);
		va_end(arguments);
	}
}

static bool parse_decimal(const char* value, unsigned long minimum, unsigned long maximum,
		  unsigned long* parsed)
{
	if (!value || value[0] == '\0')
		return false;

	unsigned long result = 0;
	for (const unsigned char* character = (const unsigned char*)value; *character; character++)
	{
		if (*character < '0' || *character > '9')
			return false;
		const unsigned long digit = (unsigned long)(*character - '0');
		if (result > (maximum - digit) / 10UL)
			return false;
		result = result * 10UL + digit;
	}
	if (result < minimum || result > maximum)
		return false;
	*parsed = result;
	return true;
}

static bool parse_listen(GatewayConfig* config, const char* value)
{
	const char* separator = strrchr(value, ':');
	if (!separator || separator == value || separator[1] == '\0')
		return false;

	const size_t host_length = (size_t)(separator - value);
	if (host_length >= sizeof(config->bind_address))
		return false;
	unsigned long port = 0;
	if (!parse_decimal(separator + 1, 1UL, 65535UL, &port))
		return false;

	memcpy(config->bind_address, value, host_length);
	config->bind_address[host_length] = '\0';
	config->port = (uint16_t)port;
	return true;
}

static bool parse_option_number(const char* option, const char* value, unsigned long minimum,
				unsigned long maximum, unsigned long* parsed, char* error,
				size_t error_size)
{
	if (parse_decimal(value, minimum, maximum, parsed))
		return true;
	set_error(error, error_size, "Invalid value for %s: %s", option, value);
	return false;
}

static bool is_value_option(const char* option)
{
	return strcmp(option, "-listen") == 0 || strcmp(option, "-cert") == 0 ||
	       strcmp(option, "-key") == 0 || strcmp(option, "-width") == 0 ||
	       strcmp(option, "-height") == 0 || strcmp(option, "-bitrate") == 0 ||
	       strcmp(option, "-control-port") == 0 || strcmp(option, "-video-port") == 0 ||
	       strcmp(option, "-http-port") == 0 || strcmp(option, "-jpeg-quality") == 0;
}

void gateway_config_defaults(GatewayConfig* config)
{
	(void)memset(config, 0, sizeof(*config));
	(void)snprintf(config->bind_address, sizeof(config->bind_address), "%s", "0.0.0.0");
	config->port = 3389;
	config->certificate = "/root/nanokvm-rdp/cert.pem";
	config->private_key = "/root/nanokvm-rdp/key.pem";
	config->width = 1920;
	config->height = 1080;
	config->bitrate = 8000;
	config->control_port = 3390;
	config->video_port = 5004;
	config->http_port = 8080;
	config->jpeg_quality = 8;
}

bool gateway_config_parse(GatewayConfig* config, int argc, char* const argv[], char* error,
		  size_t error_size)
{
	if (error && error_size > 0)
		error[0] = '\0';
	for (int index = 1; index < argc; index++)
	{
		const char* option = argv[index];
		if (strcmp(option, "-direct-gfx") == 0)
			config->direct_gfx = true;
		else if (strcmp(option, "-swap-alt-command") == 0)
			config->swap_alt_command = true;
		else
		{
			if (!is_value_option(option))
			{
				set_error(error, error_size, "Unknown option: %s", option);
				return false;
			}
			if (index + 1 >= argc)
			{
				set_error(error, error_size, "Missing value for %s", option);
				return false;
			}
			const char* value = argv[++index];
			unsigned long parsed = 0;
			if (strcmp(option, "-listen") == 0)
			{
				if (!parse_listen(config, value))
				{
					set_error(error, error_size, "Invalid value for -listen: %s", value);
					return false;
				}
			}
			else if (strcmp(option, "-cert") == 0)
				config->certificate = value;
			else if (strcmp(option, "-key") == 0)
				config->private_key = value;
			else if (strcmp(option, "-width") == 0)
			{
				if (!parse_option_number(option, value, 1UL, 8192UL, &parsed, error, error_size))
					return false;
				config->width = (uint16_t)parsed;
			}
			else if (strcmp(option, "-height") == 0)
			{
				if (!parse_option_number(option, value, 1UL, 8192UL, &parsed, error, error_size))
					return false;
				config->height = (uint16_t)parsed;
			}
			else if (strcmp(option, "-bitrate") == 0)
			{
				if (!parse_option_number(option, value, 1UL, 65535UL, &parsed, error, error_size))
					return false;
				config->bitrate = (uint16_t)parsed;
			}
			else if (strcmp(option, "-control-port") == 0)
			{
				if (!parse_option_number(option, value, 1UL, 65535UL, &parsed, error, error_size))
					return false;
				config->control_port = (uint16_t)parsed;
			}
			else if (strcmp(option, "-video-port") == 0)
			{
				if (!parse_option_number(option, value, 1UL, 65535UL, &parsed, error, error_size))
					return false;
				config->video_port = (uint16_t)parsed;
			}
			else if (strcmp(option, "-http-port") == 0)
			{
				if (!parse_option_number(option, value, 1UL, 65535UL, &parsed, error, error_size))
					return false;
				config->http_port = (uint16_t)parsed;
			}
			else if (strcmp(option, "-jpeg-quality") == 0)
			{
				if (!parse_option_number(option, value, 2UL, 31UL, &parsed, error, error_size))
					return false;
				config->jpeg_quality = (int)parsed;
			}
			else
			{
				set_error(error, error_size, "Unknown option: %s", option);
				return false;
			}
		}
	}
	return true;
}

void gateway_config_print_usage(FILE* stream, const char* executable)
{
	(void)fprintf(stream,
	              "Usage: %s [-listen host:port] [-cert file] [-key file] [-width n] [-height n] "
	              "[-bitrate n] [-control-port n] [-video-port n] [-http-port n] "
	              "[-jpeg-quality n] [-direct-gfx] [-swap-alt-command]\n",
	              executable);
}
