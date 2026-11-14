"""在独立控制台向本脚本启动的进程发真实Ctrl+C/Break；不向用户控制台广播。"""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time


def send_to_private_console(pid, control_break):
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.FreeConsole()
    if not kernel.AttachConsole(int(pid)):
        raise ctypes.WinError(ctypes.get_last_error())
    handler_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.DWORD)
    handler = handler_type(lambda event: True)
    if not kernel.SetConsoleCtrlHandler(handler, True):
        raise ctypes.WinError(ctypes.get_last_error())
    ids = (wintypes.DWORD * 4)()
    count = kernel.GetConsoleProcessList(ids, 4)
    if count != 2 or set(ids[:count]) != {int(pid), kernel.GetCurrentProcessId()}:
        raise RuntimeError('Signal target console is not isolated')
    if not kernel.GenerateConsoleCtrlEvent(1 if control_break else 0, 0):
        raise ctypes.WinError(ctypes.get_last_error())
    time.sleep(.3)  # 留出系统执行两个处理器的时间；本子进程的处理器仅保护自身。


if len(sys.argv) == 4 and sys.argv[1] == '--send-private':
    send_to_private_console(sys.argv[2], sys.argv[3] == 'ctrl-break')
    raise SystemExit(0)

parser = argparse.ArgumentParser()
parser.add_argument('--exe', required=True, type=Path)
parser.add_argument('--lifecycle', required=True, type=Path)
args = parser.parse_args()
args.exe, args.lifecycle = args.exe.resolve(), args.lifecycle.resolve()
folder = Path(__file__).resolve().parents[1] / 'workspace' / f'console-stop-check-{time.time_ns()}'
folder.mkdir(parents=True)
report = {'passed': False, 'exe_sha256': hashlib.sha256(args.exe.read_bytes()).hexdigest(), 'cases': []}
startup = subprocess.STARTUPINFO()
startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
startup.wShowWindow = 0


def run(command, private_console=False):
    result = subprocess.run(list(map(str, command)), capture_output=True, encoding='utf-8', errors='replace',
                            timeout=45, creationflags=subprocess.CREATE_NEW_CONSOLE if private_console else 0,
                            startupinfo=startup if private_console else None)
    report['cases'].append({'command': list(map(str, command)), 'exit': result.returncode,
                            'stdout': result.stdout, 'stderr': result.stderr})
    assert result.returncode == 0, report['cases'][-1]
    return result.stdout


try:
    source = folder / 'silent.nut'
    run([args.lifecycle, '--make-preview-fixture', 'silent', source])
    for event in ['ctrl-c', 'ctrl-break']:
        for mode in [[], ['--camera'], ['--preview', str(source)]]:
            log = folder / f'gui-{event}-{len(report["cases"])}.log'
            with log.open('w', encoding='utf-8') as stream:
                process = subprocess.Popen([str(args.exe), *mode], stdin=subprocess.DEVNULL, stdout=stream,
                    stderr=stream, creationflags=subprocess.CREATE_NEW_CONSOLE, startupinfo=startup)
                try:
                    user = ctypes.WinDLL('user32', use_last_error=True)
                    # 控制台子系统程序不能靠WaitForInputIdle证明建窗；只观察本测试PID的窗口。
                    enum_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
                    user.EnumWindows.argtypes = [enum_type, wintypes.LPARAM]
                    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
                    def owns_window(window, unused):
                        pid = wintypes.DWORD()
                        user.GetWindowThreadProcessId(window, ctypes.byref(pid))
                        if pid.value == process.pid:
                            found[0] = True
                        return True
                    callback = enum_type(owns_window)
                    deadline = time.perf_counter() + 15
                    found = [False]
                    while not found[0] and process.poll() is None and time.perf_counter() < deadline:
                        user.EnumWindows(callback, 0)
                        time.sleep(.05)
                    assert found[0], 'GUI did not create a window'
                    assert process.poll() is None, 'GUI exited before signal'
                    run([sys.executable, Path(__file__).resolve(), '--send-private', process.pid, event])
                    exit_code = process.wait(timeout=25)
                    report['cases'].append({'gui_arguments': mode, 'signal': event, 'exit': exit_code, 'log': str(log)})
                    assert exit_code == 0, report['cases'][-1]
                finally:
                    if process.poll() is None:
                        process.terminate()  # 仅清理本测试新建的子进程；超时仍是失败。
                        process.wait(timeout=10)
        output = folder / f'{event}.mp4'
        run([args.lifecycle, '--test-console-recording-stop', source, output, event], private_console=True)
        inspection = json.loads(run([args.lifecycle, '--inspect-export', output]))
        assert 1 <= inspection['video_frames'] < 100 and 0 < inspection['video_end'] < 4, inspection
        report['cases'].append({'signal': event, 'inspection': inspection})
    assert not list(folder.glob('*.part-*'))
    report['passed'] = True
finally:
    (folder / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
print('PASS GUI console stop and recording finalization:', folder / 'report.json')
