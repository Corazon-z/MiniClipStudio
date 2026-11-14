// S06命令行入口只解析选项，设备查询/采集统一交给CameraCatalog和CameraCapture。
#pragma once
namespace miniclip {
// 接收--cameras/--camera-modes/--camera-record/--camera-simulate；参数错误抛给统一错误边界。
int RunCameraCommand(int argc, wchar_t** argv);
}
