#include <Windows.h>
#include <stdio.h>
#include "AudioCapture.h"


#define ID_BTN_RECORD 1001    //按钮ID,标识按钮的唯一标识符

HANDLE g_hThread = NULL;  //线程句柄
//HANDLE是Windows API中用于表示线程、进程、文件等对象的句柄类型。它是一个指向内核对象的指针，可以用于操作和管理这些对象。

DWORD WINAPI ThreadProc(LPVOID lpParam) {       //参数lpParam是传递给线程的参数，可以在创建线程时指定。这里没有使用该参数，所以可以忽略。
	StartAudioCapture();  //调用音频采集函数
	return 0;
}
//WINAPI中的线程函数必须符合DWORD WINAPI的签名，返回值为DWORD类型，表示线程的退出代码。
//按照约定调用结束被调用者清理栈

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	switch (msg) {
	case WM_CREATE: //v窗口创建新增一个按钮
		CreateWindowW(L"BUTTON", L"\u5F00\u59CB\u91C7\u96C6", WS_VISIBLE | WS_CHILD, 10, 10, 100, 30, hwnd, (HMENU)ID_BTN_RECORD, NULL, NULL);
		break;
	case WM_COMMAND:
		if (LOWORD(wParam) == ID_BTN_RECORD) {  //判断是否点击了开始采集按钮
			if (g_hThread == NULL) {
				//如果线程没跑，创建线程开始
				g_hThread = CreateThread(NULL, 0, ThreadProc, NULL, 0, NULL);  //创建线程
				SetDlgItemTextW(hwnd, ID_BTN_RECORD, L"\u505C\u6B62\u91C7\u96C6");  //修改按钮文本为“停止采集”)
			}
			else {
				StopAudioCapture();  //停止音频采集
				WaitForSingleObject(g_hThread, 1000);  //等待线程结束
				CloseHandle(g_hThread);  //关闭线程句柄
				g_hThread = NULL;  //重置线程句柄

				SetDlgItemTextW(hwnd, ID_BTN_RECORD, L"\u5F00\u59CB\u91C7\u96C6");  //修改按钮文本为“开始采集”)
			}
		}
		break;
	case WM_DESTROY:
		StopAudioCapture();  //停止音频采集
		if (g_hThread){
			WaitForSingleObject(g_hThread, 1000);  //等待线程结束
			CloseHandle(g_hThread);  //关闭线程句柄
			g_hThread = NULL;  //重置线程句柄
		}
		PostQuitMessage(0);
		break;
	default:
		return DefWindowProc(hwnd, msg, wParam, lParam);
		//不认识的消息交给默认的窗口过程处理
	}
	return 0;
}

//Winmain：
int main() {
	HINSTANCE hInstance = GetModuleHandleW(NULL);
	int nCmdShow = SW_SHOWDEFAULT;
	//UTF - 8编码
	SetConsoleOutputCP(65001);
	//1.注册窗口类
	const wchar_t CLASS_NAME[] = L"AudioCaptureWindowClass";
	WNDCLASSW wc = {};
	wc.lpfnWndProc = WndProc;  //窗口过程函数
	wc.hInstance = hInstance;  //应用程序实例句柄
	wc.lpszClassName = CLASS_NAME;  //窗口类名
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);  //窗口背景颜色
	RegisterClassW(&wc);
	//2.创建窗口
	HWND hwnd = CreateWindowW(L"AudioCaptureWindowClass", L"\u97F3\u9891\u91C7\u96C6", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 400, 300, NULL, NULL, hInstance, NULL);
	if (!hwnd) {
		return -1;
	}
	ShowWindow(hwnd, nCmdShow);  //显示窗口
	UpdateWindow(hwnd);  //更新窗口
	
	//3.消息循环
	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	return (int)msg.wParam;
}