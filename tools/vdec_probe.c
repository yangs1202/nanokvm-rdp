#include <dlfcn.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WIDTH 1920U
#define HEIGHT 1080U
#define BITRATE_KBPS 3000U
#define PROBE_SECONDS 10U

enum kvm_frame_kind
{
	KVM_FRAME_MJPEG = 0,
	KVM_FRAME_SPS = 1,
	KVM_FRAME_PPS = 2,
	KVM_FRAME_IDR = 3,
	KVM_FRAME_P = 4,
};

typedef uint8_t CVI_BOOL;

/* Minimal CV180x VDEC ABI declarations used by this diagnostic program. */
typedef struct
{
	uint32_t u32RefFrameNum;
	CVI_BOOL bTemporalMvpEnable;
	uint8_t reserved[3];
	uint32_t u32TmvBufSize;
} VdecAttrVideo;

typedef struct
{
	uint32_t enType;
	uint32_t enMode;
	uint32_t u32PicWidth;
	uint32_t u32PicHeight;
	uint32_t u32StreamBufSize;
	uint32_t u32FrameBufSize;
	uint32_t u32FrameBufCnt;
	VdecAttrVideo stVdecVideoAttr;
} VdecChannelAttr;

typedef struct
{
	uint32_t u32Len;
	uint64_t u64PTS;
	CVI_BOOL bEndOfFrame;
	CVI_BOOL bEndOfStream;
	CVI_BOOL bDisplay;
	uint8_t* pu8Addr;
} VdecStream;

typedef struct
{
	uint32_t u32Width;
	uint32_t u32Height;
	uint32_t enPixelFormat;
	uint32_t enBayerFormat;
	uint32_t enVideoFormat;
	uint32_t enCompressMode;
	uint32_t enDynamicRange;
	uint32_t enColorGamut;
	uint32_t u32Stride[3];
	uint64_t u64PhyAddr[3];
	uint8_t* pu8VirAddr[3];
	uint32_t u32Length[3];
	int16_t s16OffsetTop;
	int16_t s16OffsetBottom;
	int16_t s16OffsetLeft;
	int16_t s16OffsetRight;
	uint32_t u32TimeRef;
	uint64_t u64PTS;
	void* pPrivateData;
	uint32_t u32FrameFlag;
} VideoFrame;

typedef struct
{
	VideoFrame stVFrame;
	uint32_t u32PoolId;
} VideoFrameInfo;

typedef struct
{
	void* handle;
	void (*init)(uint8_t level);
	int (*read_image)(uint16_t width, uint16_t height, uint8_t type, uint16_t quality,
	                  uint8_t** data, uint32_t* length);
	int (*free_data)(uint8_t** data);
	void (*set_frame_detect)(uint8_t enabled);
} KvmApi;

typedef struct
{
	void* handle;
	int (*create_channel)(int channel, const VdecChannelAttr* attr);
	int (*destroy_channel)(int channel);
	int (*start_receiving)(int channel);
	int (*stop_receiving)(int channel);
	int (*send_stream)(int channel, const VdecStream* stream, int timeout_ms);
	int (*get_frame)(int channel, VideoFrameInfo* frame, int timeout_ms);
	int (*release_frame)(int channel, const VideoFrameInfo* frame);
} VdecApi;

static volatile sig_atomic_t stop_requested = 0;

static void on_signal(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

static uint64_t monotonic_milliseconds(void)
{
	struct timespec now = { 0 };
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return ((uint64_t)now.tv_sec * 1000U) + ((uint64_t)now.tv_nsec / 1000000U);
}

static bool load_kvm(KvmApi* api)
{
	const char* paths[] = {
		"/root/foldvnc/dl_lib/libkvm.so",
		"/kvmapp/server/dl_lib/libkvm.so",
		"/tmp/server/dl_lib/libkvm.so",
		"libkvm.so",
	};
	for (size_t index = 0; index < sizeof(paths) / sizeof(paths[0]); index++)
	{
		api->handle = dlopen(paths[index], RTLD_NOW | RTLD_LOCAL);
		if (api->handle)
			break;
	}
	if (!api->handle)
	{
		fprintf(stderr, "cannot open libkvm.so: %s\n", dlerror());
		return false;
	}
	*(void**)(&api->init) = dlsym(api->handle, "kvmv_init");
	*(void**)(&api->read_image) = dlsym(api->handle, "kvmv_read_img");
	*(void**)(&api->free_data) = dlsym(api->handle, "free_kvmv_data");
	*(void**)(&api->set_frame_detect) = dlsym(api->handle, "set_frame_detact");
	if (!api->init || !api->read_image || !api->free_data || !api->set_frame_detect)
	{
		fprintf(stderr, "libkvm.so is missing a required symbol\n");
		dlclose(api->handle);
		memset(api, 0, sizeof(*api));
		return false;
	}
	return true;
}

static bool load_vdec(VdecApi* api)
{
	const char* paths[] = {
		"/root/foldvnc/dl_lib/libvdec.so",
		"/kvmapp/server/dl_lib/libvdec.so",
		"libvdec.so",
	};
	for (size_t index = 0; index < sizeof(paths) / sizeof(paths[0]); index++)
	{
		api->handle = dlopen(paths[index], RTLD_NOW | RTLD_LOCAL);
		if (api->handle)
			break;
	}
	if (!api->handle)
	{
		fprintf(stderr, "cannot open libvdec.so: %s\n", dlerror());
		return false;
	}
	*(void**)(&api->create_channel) = dlsym(api->handle, "CVI_VDEC_CreateChn");
	*(void**)(&api->destroy_channel) = dlsym(api->handle, "CVI_VDEC_DestroyChn");
	*(void**)(&api->start_receiving) = dlsym(api->handle, "CVI_VDEC_StartRecvStream");
	*(void**)(&api->stop_receiving) = dlsym(api->handle, "CVI_VDEC_StopRecvStream");
	*(void**)(&api->send_stream) = dlsym(api->handle, "CVI_VDEC_SendStream");
	*(void**)(&api->get_frame) = dlsym(api->handle, "CVI_VDEC_GetFrame");
	*(void**)(&api->release_frame) = dlsym(api->handle, "CVI_VDEC_ReleaseFrame");
	if (!api->create_channel || !api->destroy_channel || !api->start_receiving ||
	    !api->stop_receiving || !api->send_stream || !api->get_frame || !api->release_frame)
	{
		fprintf(stderr, "libvdec.so is missing a required symbol\n");
		dlclose(api->handle);
		memset(api, 0, sizeof(*api));
		return false;
	}
	return true;
}

int main(void)
{
	KvmApi kvm = { 0 };
	VdecApi vdec = { 0 };
	bool channel_created = false;
	bool receiving = false;
	int result = EXIT_FAILURE;
	const int channel = 0;
	const uint64_t deadline = monotonic_milliseconds() + (PROBE_SECONDS * 1000U);
	uint32_t submitted = 0;
	uint32_t decoded = 0;

	if (sizeof(VdecChannelAttr) != 40 || sizeof(VdecStream) != 32 || sizeof(VideoFrameInfo) != 152)
	{
		fprintf(stderr, "unexpected VDEC ABI structure size: attr=%zu stream=%zu frame=%zu\n",
		        sizeof(VdecChannelAttr), sizeof(VdecStream), sizeof(VideoFrameInfo));
		return EXIT_FAILURE;
	}
	(void)signal(SIGINT, on_signal);
	(void)signal(SIGTERM, on_signal);
	if (!load_kvm(&kvm) || !load_vdec(&vdec))
		goto out;
	kvm.init(0);
	kvm.set_frame_detect(0);

	VdecChannelAttr attr = { 0 };
	attr.enType = 96; /* PT_H264 */
	attr.enMode = 1; /* VIDEO_MODE_FRAME */
	attr.u32PicWidth = WIDTH;
	attr.u32PicHeight = HEIGHT;
	attr.u32StreamBufSize = 2U * 1024U * 1024U;
	attr.u32FrameBufSize = WIDTH * HEIGHT * 3U / 2U;
	attr.u32FrameBufCnt = 3;
	attr.stVdecVideoAttr.u32RefFrameNum = 3;

	int status = vdec.create_channel(channel, &attr);
	if (status != 0)
	{
		fprintf(stderr, "CVI_VDEC_CreateChn=%d\n", status);
		goto out;
	}
	channel_created = true;
	status = vdec.start_receiving(channel);
	if (status != 0)
	{
		fprintf(stderr, "CVI_VDEC_StartRecvStream=%d\n", status);
		goto out;
	}
	receiving = true;
	fprintf(stderr, "VDEC probe started: %ux%u for %u seconds\n", WIDTH, HEIGHT, PROBE_SECONDS);

	while (!stop_requested && monotonic_milliseconds() < deadline)
	{
		uint8_t* data = NULL;
		uint32_t length = 0;
		const int kind = kvm.read_image(WIDTH, HEIGHT, 1, BITRATE_KBPS, &data, &length);
		if (kind < 0 || !data || length == 0)
		{
			if (data)
				(void)kvm.free_data(&data);
			continue;
		}

		if (kind >= KVM_FRAME_SPS && kind <= KVM_FRAME_P)
		{
			const VdecStream stream = {
				.u32Len = length,
				.u64PTS = monotonic_milliseconds(),
				.bEndOfFrame = (kind == KVM_FRAME_IDR || kind == KVM_FRAME_P),
				.bEndOfStream = 0,
				.bDisplay = 1,
				.pu8Addr = data,
			};
			status = vdec.send_stream(channel, &stream, 0);
			if (status == 0)
				submitted++;
			else
				fprintf(stderr, "CVI_VDEC_SendStream kind=%d length=%u status=%d\n", kind, length,
				        status);
		}

		(void)kvm.free_data(&data);
		for (;;)
		{
			VideoFrameInfo frame = { 0 };
			status = vdec.get_frame(channel, &frame, 0);
			if (status != 0)
				break;
			decoded++;
			fprintf(stderr,
			        "decoded=%u %ux%u format=%u stride=%u/%u bytes=%u/%u addr=%p\n", decoded,
			        frame.stVFrame.u32Width, frame.stVFrame.u32Height, frame.stVFrame.enPixelFormat,
			        frame.stVFrame.u32Stride[0], frame.stVFrame.u32Stride[1],
			        frame.stVFrame.u32Length[0], frame.stVFrame.u32Length[1],
			        (void*)frame.stVFrame.pu8VirAddr[0]);
			status = vdec.release_frame(channel, &frame);
			if (status != 0)
			{
				fprintf(stderr, "CVI_VDEC_ReleaseFrame=%d\n", status);
				goto out;
			}
		}
	}
	fprintf(stderr, "VDEC probe complete: submitted=%u decoded=%u\n", submitted, decoded);
	result = decoded > 0 ? EXIT_SUCCESS : EXIT_FAILURE;

out:
	if (receiving)
		(void)vdec.stop_receiving(channel);
	if (channel_created)
		(void)vdec.destroy_channel(channel);
	if (vdec.handle)
		dlclose(vdec.handle);
	if (kvm.handle)
		dlclose(kvm.handle);
	return result;
}
