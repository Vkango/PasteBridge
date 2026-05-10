#define UNICODE
#define _UNICODE

#include <windows.h>
#include <string>
#include <iostream>
#include <io.h>
#include <fcntl.h>
#include <cmath>

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

constexpr int HOTKEY_BIND_A = 1;
constexpr int HOTKEY_BIND_B = 2;
constexpr int HOTKEY_TRANSFER = 3;
constexpr int HOTKEY_TOGGLE_QUICK = 4;

constexpr UINT WM_COORD_UPDATED = WM_USER + 1;
constexpr UINT WM_QUICK_TRIGGER = WM_USER + 2;
constexpr int DRAG_THRESHOLD = 10; // 像素, 超过视为"选择文本"

struct WindowAnchor {
    HWND hwnd = nullptr;
    std::wstring title;
    std::wstring className;
    DWORD pid = 0;
};

struct AppState {
    WindowAnchor targetA;
    WindowAnchor sourceB;
    bool hasTarget = false;
    bool hasSource = false;
    bool hasTargetPoint = false;
    POINT targetClientPoint{ 0, 0 };
};

AppState g_state;
HHOOK g_mouseHook = nullptr;
HWND g_consoleHwnd = nullptr;
DWORD g_mainThreadId = 0;
bool g_quickMode = false;
bool g_isDragging = false;
POINT g_dragStart{ 0, 0 };

std::wstring GetWindowTitle(HWND hwnd) {
    wchar_t buffer[512]{};
    GetWindowTextW(hwnd, buffer, 512);
    return buffer;
}

std::wstring GetWindowClassName(HWND hwnd) {
    wchar_t buffer[256]{};
    GetClassNameW(hwnd, buffer, 256);
    return buffer;
}

WindowAnchor CaptureForegroundWindow() {
    HWND fg = GetForegroundWindow();
    WindowAnchor anchor{};
    if (!fg) return anchor;
    anchor.hwnd = fg;
    anchor.title = GetWindowTitle(fg);
    anchor.className = GetWindowClassName(fg);
    GetWindowThreadProcessId(fg, &anchor.pid);
    return anchor;
}

void PrintAnchor(const wchar_t* name, const WindowAnchor& a) {
    std::wcout << name << L": hwnd=" << a.hwnd
        << L", title=" << a.title
        << L", class=" << a.className
        << L", pid=" << a.pid << std::endl;
}

bool IsSameWindow(HWND hwnd, const WindowAnchor& anchor) {
    return hwnd && hwnd == anchor.hwnd;
}

void SendCtrlKey(WORD vk) {
    INPUT inputs[4]{};

    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;
    inputs[0].ki.wScan = static_cast<WORD>(MapVirtualKey(VK_CONTROL, MAPVK_VK_TO_VSC));
    inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE;

    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = vk;
    inputs[1].ki.wScan = static_cast<WORD>(MapVirtualKey(vk, MAPVK_VK_TO_VSC));
    inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE;

    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wVk = vk;
    inputs[2].ki.wScan = static_cast<WORD>(MapVirtualKey(vk, MAPVK_VK_TO_VSC));
    inputs[2].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;

    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = VK_CONTROL;
    inputs[3].ki.wScan = static_cast<WORD>(MapVirtualKey(VK_CONTROL, MAPVK_VK_TO_VSC));
    inputs[3].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;

    SendInput(4, inputs, sizeof(INPUT));
}

void MouseClickScreen(int x, int y) {
    SetCursorPos(x, y);
    INPUT inputs[2]{};
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, inputs, sizeof(INPUT));
}

bool ReadClipboardText(std::wstring& out, int maxRetries = 5, int delayMs = 100) {
    out.clear();
    for (int i = 0; i < maxRetries; ++i) {
        if (i > 0) Sleep(delayMs);
        if (!OpenClipboard(nullptr)) continue;
        HANDLE hData = GetClipboardData(CF_UNICODETEXT);
        if (!hData) { CloseClipboard(); continue; }
        wchar_t* pszText = static_cast<wchar_t*>(GlobalLock(hData));
        if (!pszText) { CloseClipboard(); continue; }
        out = pszText;
        GlobalUnlock(hData);
        CloseClipboard();
        if (!out.empty()) return true;
    }
    return false;
}


bool WriteClipboardText(const std::wstring& text) {
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hMem) { CloseClipboard(); return false; }
    void* ptr = GlobalLock(hMem);
    memcpy(ptr, text.c_str(), bytes);
    GlobalUnlock(hMem);
    SetClipboardData(CF_UNICODETEXT, hMem);
    CloseClipboard();
    return true;
}

// 轮询激活
void ActivateWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;

    DWORD targetThread = GetWindowThreadProcessId(hwnd, nullptr);
    DWORD foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);

    if (foregroundThread && foregroundThread != targetThread) {
        AttachThreadInput(foregroundThread, targetThread, TRUE);
    }

    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);

    // 轮询等待前台真正切换
    for (int i = 0; i < 50; ++i) {
        if (GetForegroundWindow() == hwnd) break;
        Sleep(5);
    }

    if (foregroundThread && foregroundThread != targetThread) {
        AttachThreadInput(foregroundThread, targetThread, FALSE);
    }
}

void TransferText(bool silent = false) {
    if (!g_state.hasTarget) {
        if (!silent) std::wcout << L"尚未绑定目标窗口 A." << std::endl;
        return;
    }
    if (!g_state.hasSource) {
        if (!silent) std::wcout << L"尚未绑定源窗口 B." << std::endl;
        return;
    }
    if (!g_state.hasTargetPoint) {
        if (!silent) std::wcout << L"尚未记录 A 的点击位置. 请在 A 窗口内点击一次鼠标左键." << std::endl;
        return;
    }

    HWND source = g_state.sourceB.hwnd;
    HWND target = g_state.targetA.hwnd;

    if (!IsWindow(source)) {
        if (!silent) std::wcout << L"源窗口 B 已失效." << std::endl;
        return;
    }
    if (!IsWindow(target)) {
        if (!silent) std::wcout << L"目标窗口 A 已失效." << std::endl;
        return;
    }

    // 1. 激活 B, 复制
    ActivateWindow(source);
    WriteClipboardText(L"");
    SendCtrlKey('C');

    std::wstring copied;
    if (!ReadClipboardText(copied, 200, 20) || copied.empty()) {
        if (!silent) std::wcout << L"没有复制到文本. 请确认 B 中已选中文本." << std::endl;
        return;
    }
    if (!silent) {
        std::wcout << L"复制文本: " << copied.substr(0, 60)
            << (copied.size() > 60 ? L"..." : L"") << std::endl;
    }

    // 2. 激活 A, 点击, 粘贴
    ActivateWindow(target);
    POINT pt = g_state.targetClientPoint;
    ClientToScreen(target, &pt);
    MouseClickScreen(pt.x, pt.y);
    Sleep(30); // 点击后仅需极短等待让目标应用获得焦点

    WriteClipboardText(copied);
    SendCtrlKey('V');

    if (!silent) std::wcout << L"已粘贴到目标窗口 A." << std::endl;
}

// 检测 A 窗口点击记录位置, 以及快速模式下 B 窗口拖拽触发
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode != HC_ACTION) {
        return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
    }

    if (wParam == WM_LBUTTONDOWN) {
        MSLLHOOKSTRUCT* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        g_dragStart = info->pt;
        g_isDragging = true;
    }
    else if (wParam == WM_LBUTTONUP) {
        if (!g_isDragging) return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
        g_isDragging = false;

        MSLLHOOKSTRUCT* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        int dx = std::abs(info->pt.x - g_dragStart.x);
        int dy = std::abs(info->pt.y - g_dragStart.y);

        HWND fg = GetForegroundWindow();

        // 快速模式：B 窗口发生拖拽（位移超过阈值）→ 自动触发
        if (g_quickMode && g_state.hasSource && IsSameWindow(fg, g_state.sourceB)) {
            if (dx > DRAG_THRESHOLD || dy > DRAG_THRESHOLD) {
                PostThreadMessage(g_mainThreadId, WM_QUICK_TRIGGER, 0, 0);
            }
        }

        // A 窗口点击：记录 client 坐标
        if (g_state.hasTarget && IsSameWindow(fg, g_state.targetA)) {
            POINT pt = info->pt;
            ScreenToClient(g_state.targetA.hwnd, &pt);
            if (pt.x != g_state.targetClientPoint.x || pt.y != g_state.targetClientPoint.y) {
                g_state.targetClientPoint = pt;
                g_state.hasTargetPoint = true;
                PostThreadMessage(g_mainThreadId, WM_COORD_UPDATED,
                    static_cast<WPARAM>(pt.x), static_cast<LPARAM>(pt.y));
            }
        }
    }

    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

int main() {
    // DPI 感知
    auto SetProcessDpiAwarenessContext = reinterpret_cast<BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT)>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")
        );
    if (SetProcessDpiAwarenessContext) {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
    else {
        SetProcessDPIAware();
    }

    _setmode(_fileno(stdout), _O_U16TEXT);
    SetConsoleOutputCP(CP_UTF8);
    g_consoleHwnd = GetConsoleWindow();
    g_mainThreadId = GetCurrentThreadId();

    // 强制创建消息队列, 使 PostThreadMessage 可投递
    MSG dummy;
    PeekMessageW(&dummy, nullptr, 0, 0, PM_NOREMOVE);

    std::wcout << L"PasteBridge 快速模式启动" << std::endl;
    std::wcout << L"Ctrl+Alt+9: 绑定目标窗口 A" << std::endl;
    std::wcout << L"Ctrl+Alt+0: 绑定源窗口 B" << std::endl;
    std::wcout << L"Ctrl+Alt+V: 手动传输（保留）" << std::endl;
    std::wcout << L"Ctrl+Alt+T: 切换快速模式 [当前: 关]" << std::endl;
    std::wcout << L"提示: 在 A 窗口点击记录位置; 快速模式下在 B 窗口拖拽选中文本即自动粘贴" << std::endl;
    std::wcout << L"----------------------------------------" << std::endl;

    if (!RegisterHotKey(nullptr, HOTKEY_BIND_A, MOD_CONTROL | MOD_ALT, '9') ||
        !RegisterHotKey(nullptr, HOTKEY_BIND_B, MOD_CONTROL | MOD_ALT, '0') ||
        !RegisterHotKey(nullptr, HOTKEY_TRANSFER, MOD_CONTROL | MOD_ALT, 'V') ||
        !RegisterHotKey(nullptr, HOTKEY_TOGGLE_QUICK, MOD_CONTROL | MOD_ALT, 'T')) {
        std::wcout << L"注册全局热键失败. 错误码: " << GetLastError() << std::endl;
        return 1;
    }

    g_mouseHook = SetWindowsHookExW(
        WH_MOUSE_LL,
        LowLevelMouseProc,
        GetModuleHandleW(nullptr),
        0
    );
    if (!g_mouseHook) {
        std::wcout << L"安装鼠标钩子失败. 错误码: " << GetLastError() << std::endl;
        return 1;
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_COORD_UPDATED) {
            int x = static_cast<int>(msg.wParam);
            int y = static_cast<int>(msg.lParam);
            std::wcout << L"[更新] 目标窗口 A 点击位置: client=(" << x << L", " << y << L")" << std::endl;
            continue;
        }

        if (msg.message == WM_QUICK_TRIGGER) {
            // 快速模式自动触发, 静默执行（不输出失败提示, 避免打扰）
            TransferText(true);
            continue;
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);

        if (msg.message == WM_HOTKEY) {
            switch (msg.wParam) {
            case HOTKEY_BIND_A: {
                Sleep(50);
                auto cap = CaptureForegroundWindow();
                if (cap.hwnd == g_consoleHwnd) {
                    std::wcout << L"[跳过] 前台是控制台自身" << std::endl;
                    break;
                }
                if (cap.hwnd) {
                    g_state.targetA = cap;
                    g_state.hasTarget = true;
                    g_state.hasTargetPoint = false;
                    PrintAnchor(L"目标窗口 A", g_state.targetA);
                }
                break;
            }
            case HOTKEY_BIND_B: {
                Sleep(50);
                auto cap = CaptureForegroundWindow();
                if (cap.hwnd == g_consoleHwnd) {
                    std::wcout << L"[跳过] 前台是控制台自身" << std::endl;
                    break;
                }
                if (cap.hwnd) {
                    g_state.sourceB = cap;
                    g_state.hasSource = true;
                    PrintAnchor(L"源窗口 B", g_state.sourceB);
                }
                break;
            }
            case HOTKEY_TRANSFER:
                TransferText(false);
                break;
            case HOTKEY_TOGGLE_QUICK: {
                g_quickMode = !g_quickMode;
                std::wcout << L"[快速模式] " << (g_quickMode ? L"已开启" : L"已关闭")
                    << L" — 在 B 窗口拖拽选中文本后自动粘贴到 A" << std::endl;
                break;
            }
            }
        }
    }

    if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
    UnregisterHotKey(nullptr, HOTKEY_BIND_A);
    UnregisterHotKey(nullptr, HOTKEY_BIND_B);
    UnregisterHotKey(nullptr, HOTKEY_TRANSFER);
    UnregisterHotKey(nullptr, HOTKEY_TOGGLE_QUICK);
    return 0;
}