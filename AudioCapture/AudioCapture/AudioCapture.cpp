#include "AudioCapture.h"
#include <stdio.h>

extern "C" {    //支持函数重载
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
}

volatile bool g_isRecording = false;  //全局变量，表示是否正在采集音频
//volatile 表示该变量可能会被其他线程修改，防止编译器对其进行优化，每次访问都从内存中读取最新的值。

void StartAudioCapture() {
	//1.注册设备
	avdevice_register_all();

	//2.设置音频输入格式
	const AVInputFormat* inputFormat = av_find_input_format("dshow");
	if (!inputFormat) {
		printf("Failed to find input format\n");
		return;
	}



	//3.设置音频输入设备名称
	AVFormatContext* fmt_ctx = NULL;
	//电脑麦克风设备名称
	// 注意：C++ 里字符串中的反斜杠 \ 是转义符，所以要把原本的一个 \ 写成两个
	const char* device_name = "audio=@device_cm_{33D9A762-90C8-11D0-BD43-00A0C911CE86}\\wave_{65E2DDA4-38EC-4C08-959E-74FD22BFD10D}";

	printf("尝试打开设备： % s\n", device_name);
	int ret = avformat_open_input(&fmt_ctx, device_name, inputFormat, NULL);

	if (ret < 0) {
		char errbuf[128];
		av_strerror(ret, errbuf, sizeof(errbuf));//识别错误码并将其转换为可读的错误信息
		printf("Failed to open input device: %s\n", errbuf);
		return;
	}
	printf("成功打开设备： %s\n", device_name);
	g_isRecording = true;
	//标志位设为true，表示正在采集音频
	//4.准备数据包
	AVPacket* pkt = av_packet_alloc();

	//5.循环读数据
	while (g_isRecording) {
		ret = av_read_frame(fmt_ctx, pkt);
		if (ret < 0) {
			char errbuf[128];
			av_strerror(ret, errbuf, sizeof(errbuf));
			printf("Failed to read frame: %s\n", errbuf);
			break;
		}
		//处理音频数据包pkt->data，大小为pkt->size
		//打印音频数据包的大小
		printf("Read audio packet, size: %d\n", pkt->size);
		//释放数据包
		av_packet_unref(pkt);
	}

	//6.关闭输入设备
	avformat_close_input(&fmt_ctx);
	av_packet_free(&pkt);
	printf("Audio capture stopped.\n");
}

void StopAudioCapture() {
	g_isRecording = false;
}
