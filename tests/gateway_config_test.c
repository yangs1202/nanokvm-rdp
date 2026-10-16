#include "gateway_config.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool parse_args(GatewayConfig* config, int argc, char* argv[], char* error, size_t error_size)
{
	gateway_config_defaults(config);
	return gateway_config_parse(config, argc, argv, error, error_size);
}

static void test_defaults(void)
{
	GatewayConfig config;
	gateway_config_defaults(&config);
	assert(strcmp(config.bind_address, "0.0.0.0") == 0);
	assert(config.port == 3389);
	assert(strcmp(config.certificate, "/root/nanokvm-rdp/cert.pem") == 0);
	assert(strcmp(config.private_key, "/root/nanokvm-rdp/key.pem") == 0);
	assert(config.width == 1920);
	assert(config.height == 1080);
	assert(config.bitrate == 8000);
	assert(config.control_port == 3390);
	assert(config.video_port == 5004);
	assert(config.http_port == 8080);
	assert(config.jpeg_quality == 8);
	assert(!config.direct_gfx);
	assert(!config.swap_alt_command);
}

static void test_every_flag(void)
{
	char* arguments[] = { "gateway", "-listen", "192.168.1.10:3399", "-cert", "cert.crt",
			      "-key", "key.pem", "-width", "8192", "-height", "1", "-bitrate",
			      "65535", "-control-port", "1", "-video-port", "65535", "-http-port",
			      "8081", "-jpeg-quality", "31", "-direct-gfx", "-swap-alt-command" };
	GatewayConfig config;
	char error[128];
	assert(parse_args(&config, (int)(sizeof(arguments) / sizeof(arguments[0])), arguments, error,
			  sizeof(error)));
	assert(strcmp(config.bind_address, "192.168.1.10") == 0);
	assert(config.port == 3399);
	assert(strcmp(config.certificate, "cert.crt") == 0);
	assert(strcmp(config.private_key, "key.pem") == 0);
	assert(config.width == 8192);
	assert(config.height == 1);
	assert(config.bitrate == 65535);
	assert(config.control_port == 1);
	assert(config.video_port == 65535);
	assert(config.http_port == 8081);
	assert(config.jpeg_quality == 31);
	assert(config.direct_gfx);
	assert(config.swap_alt_command);
}

static void test_listen_forms(void)
{
	char* ipv4[] = { "gateway", "-listen", "127.0.0.1:1" };
	GatewayConfig config;
	char error[128];
	assert(parse_args(&config, 3, ipv4, error, sizeof(error)));
	assert(strcmp(config.bind_address, "127.0.0.1") == 0);
	assert(config.port == 1);

	char* ipv6[] = { "gateway", "-listen", "::1:65535" };
	assert(parse_args(&config, 3, ipv6, error, sizeof(error)));
	assert(strcmp(config.bind_address, "::1") == 0);
	assert(config.port == 65535);

	const char* invalid_values[] = { ":3389", "localhost", "host:", "host:0", "host:65536",
					 "host:abc", "host:+80", "host:-80", "host:80x",
					 "host:99999999999999999999999999999",
					 "1234567890123456789012345678901234567890123456789012345678901234:80" };
	for (size_t index = 0; index < sizeof(invalid_values) / sizeof(invalid_values[0]); index++)
	{
		char* arguments[] = { "gateway", "-listen", (char*)invalid_values[index] };
		assert(!parse_args(&config, 3, arguments, error, sizeof(error)));
		assert(strstr(error, "-listen") != NULL);
	}
}

static void test_numeric_rejections(void)
{
	const struct
	{
		const char* option;
		const char* value;
	} invalid[] = {
		{ "-width", "0" }, { "-width", "8193" }, { "-width", "70000" },
		{ "-width", "abc" }, { "-width", "+1" }, { "-height", "-1" },
		{ "-height", "8193" }, { "-height", "2px" }, { "-bitrate", "0" },
		{ "-bitrate", "65536" }, { "-bitrate", "8000kbps" }, { "-control-port", "0" },
		{ "-control-port", "65536" }, { "-control-port", "port" }, { "-video-port", "0" },
		{ "-video-port", "65536" }, { "-video-port", "abc" }, { "-http-port", "0" },
		{ "-http-port", "65536" }, { "-http-port", "80.8" }, { "-jpeg-quality", "1" },
		{ "-jpeg-quality", "32" }, { "-jpeg-quality", "high" },
	};
	for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index++)
	{
		char* arguments[] = { "gateway", (char*)invalid[index].option, (char*)invalid[index].value };
		GatewayConfig config;
		char error[128];
		assert(!parse_args(&config, 3, arguments, error, sizeof(error)));
		assert(strstr(error, invalid[index].option) != NULL);
	}
}

static void test_missing_and_unknown_options(void)
{
	const char* value_options[] = { "-listen", "-cert", "-key", "-width", "-height", "-bitrate",
					"-control-port", "-video-port", "-http-port", "-jpeg-quality" };
	for (size_t index = 0; index < sizeof(value_options) / sizeof(value_options[0]); index++)
	{
		char* arguments[] = { "gateway", (char*)value_options[index] };
		GatewayConfig config;
		char error[128];
		assert(!parse_args(&config, 2, arguments, error, sizeof(error)));
		assert(strstr(error, "Missing value") != NULL);
	}
	char* unknown[] = { "gateway", "-unknown" };
	GatewayConfig config;
	char error[128];
	assert(!parse_args(&config, 2, unknown, error, sizeof(error)));
	assert(strstr(error, "Unknown option") != NULL);
}

static void test_boolean_flags_and_usage(void)
{
	char* arguments[] = { "gateway", "-direct-gfx", "-swap-alt-command" };
	GatewayConfig config;
	char error[128];
	assert(parse_args(&config, 3, arguments, error, sizeof(error)));
	assert(config.direct_gfx);
	assert(config.swap_alt_command);

	FILE* output = tmpfile();
	assert(output != NULL);
	gateway_config_print_usage(output, "gateway");
	assert(fseek(output, 0, SEEK_SET) == 0);
	char usage[256];
	const size_t length = fread(usage, 1, sizeof(usage) - 1, output);
	usage[length] = '\0';
	assert(strcmp(usage, "Usage: gateway [-listen host:port] [-cert file] [-key file] [-width n] [-height n] "
			  "[-bitrate n] [-control-port n] [-video-port n] [-http-port n] "
			  "[-jpeg-quality n] [-direct-gfx] [-swap-alt-command]\n") == 0);
	assert(fclose(output) == 0);
}

int main(void)
{
	test_defaults();
	test_every_flag();
	test_listen_forms();
	test_numeric_rejections();
	test_missing_and_unknown_options();
	test_boolean_flags_and_usage();
	return 0;
}
