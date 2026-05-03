#include <windows.h>
#include <string>
#include <stdexcept>
#include <memory>
#include <functional>
#include <iostream>
#include <wchar.h>
#include <chrono>
#include <thread>
#include <codecvt>
#include <random>

constexpr int START = 0x76;
constexpr int END = 0x77;

constexpr double SINE_AMPLITUDE = 0.25;
constexpr double JUMP_PROBABILITY = 0.08;

class ScopeExit {
public:
	explicit ScopeExit(std::function<void()> f)
		: func(std::move(f)), active(true) {
	}

	~ScopeExit() {
		if (active) func();
	}

	void release() { active = false; }

private:
	std::function<void()> func;
	bool active;
};

class RAIIBrush {
public:
	explicit RAIIBrush(COLORREF color) :
		brush(CreateSolidBrush(color)) {
	}

	~RAIIBrush() {
		if (brush) DeleteObject(brush);
	}

	HBRUSH get() const { return brush; }

	RAIIBrush(const RAIIBrush&) = delete;
	RAIIBrush& operator=(const RAIIBrush&) = delete;

private:
	HBRUSH brush;
};

struct StateInfo {
	std::atomic<bool> running{ true };
	std::atomic<bool> active{ false };

	std::string clipboardText;

	int wpm = 60;

	RAIIBrush brush{ RGB(235, 235, 235) };


};
class MainWindow;
inline StateInfo* GetAppState(HWND hwnd);


std::string wstring_to_utf8(const std::wstring& wstr) {
	if (wstr.empty()) return {};

	int size_needed = WideCharToMultiByte(
		CP_UTF8, 0,
		wstr.data(), (int)wstr.size(),
		nullptr, 0,
		nullptr, nullptr
	);

	std::string result(size_needed, 0);

	WideCharToMultiByte(
		CP_UTF8, 0,
		wstr.data(), (int)wstr.size(),
		result.data(), size_needed,
		nullptr, nullptr
	);

	return result;
}

std::wstring GetClipboardText() {
	if (!OpenClipboard(nullptr))
		throw std::runtime_error("OpenClipboard failed.");

	ScopeExit clipboard([] { CloseClipboard(); });

	HANDLE hData = GetClipboardData(CF_UNICODETEXT);
	if (!hData)
		throw std::runtime_error("GetClipboardData failed.");

	wchar_t* text = static_cast<wchar_t*>(GlobalLock(hData));
	if (!text)
		throw std::runtime_error("GlobalLock failed");

	ScopeExit unlock([&] { GlobalUnlock(hData); });

	return std::wstring(text);
}

void KeyboardInput(char ch) {
	SHORT result = VkKeyScanA(ch);
	BYTE vk = LOBYTE(result);
	BYTE shift = HIBYTE(result);

	INPUT ip = { 0 };
	ip.type = INPUT_KEYBOARD;

	if (shift) {
		ip.ki.wVk = VK_SHIFT;
		ip.ki.dwFlags = 0;
		SendInput(1, &ip, sizeof(INPUT));
	}

	ip.ki.wScan = 0;
	ip.ki.time = 0;
	ip.ki.dwExtraInfo;

	ip.ki.wVk = vk;
	ip.ki.dwFlags = 0;
	SendInput(1, &ip, sizeof(INPUT));

	ip.ki.dwFlags = KEYEVENTF_KEYUP;
	SendInput(1, &ip, sizeof(INPUT));

	ip.ki.wVk = VK_SHIFT;
	ip.ki.dwFlags = KEYEVENTF_KEYUP;
	SendInput(1, &ip, sizeof(INPUT));
}



void checkInputs(std::atomic<bool>& active, HWND hwnd) {
	StateInfo* state = GetAppState(hwnd);
	if (!state) { return; }

	while (state->running) {
		if (GetAsyncKeyState(START) & 1) {
			active = true;

			SetWindowText(hwnd, L"Human Typer (F8 to stop)");
		}
		else if (GetAsyncKeyState(END) & 1) {
			active = false;

			SetWindowText(hwnd, L"Human Typer (F7 to start)");
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(3));
	}
}

void OnSize(HWND hwnd, UINT flag, int width, int height) {

}

template <class DERIVED_TYPE>
class BaseWindow {
public:
	static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
		DERIVED_TYPE* pThis = NULL;

		if (uMsg == WM_NCCREATE) {
			CREATESTRUCT* pCreate = (CREATESTRUCT*)lParam;
			pThis = (DERIVED_TYPE*)pCreate->lpCreateParams;

			SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)pThis);

			pThis->m_hwnd = hwnd;
		}
		else {
			pThis = (DERIVED_TYPE*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
		}

		if (pThis) {
			return pThis->HandleMessage(uMsg, wParam, lParam);
		}
		else {
			return DefWindowProc(hwnd, uMsg, wParam, lParam);
		}

		return DefWindowProc(hwnd, uMsg, wParam, lParam);
	}

	BaseWindow() : m_hwnd(NULL) {}

	BOOL Create(
		PCWSTR lpWindowName,
		DWORD dwStyle,
		DWORD dwExStyle = 0,
		int x = CW_USEDEFAULT,
		int y = CW_USEDEFAULT,
		int nWidth = CW_USEDEFAULT,
		int nHeight = CW_USEDEFAULT,
		HWND hWndParent = 0,
		HMENU hMenu = 0
	)
	{
		WNDCLASS wc = { 0 };

		wc.lpfnWndProc = DERIVED_TYPE::WindowProc;
		wc.hInstance = GetModuleHandle(NULL);
		wc.lpszClassName = ClassName();

		RegisterClass(&wc);

		m_hwnd = CreateWindowEx(
			dwExStyle, ClassName(), lpWindowName, dwStyle, x, y,
			nWidth, nHeight, hWndParent, hMenu, GetModuleHandle(NULL), this
		);

		return (m_hwnd ? TRUE : FALSE);
	}

	HWND Window() const { return m_hwnd; }

protected:
	virtual PCWSTR ClassName() const = 0;
	virtual LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) = 0;

	HWND m_hwnd;
};

class MainWindow : public BaseWindow<MainWindow> {
public:
	PCWSTR ClassName() const { return L"Default Window Class"; }
	LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);

	StateInfo state;

	HWND hEdit = nullptr;
	HWND jumpCheck = nullptr;
	HWND randomOffsetCheck = nullptr;
};

inline StateInfo* GetAppState(HWND hwnd) {
	auto pMain = reinterpret_cast<MainWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
	return pMain ? &pMain->state : nullptr;
}

LRESULT MainWindow::HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) {
	switch (uMsg) {
	case WM_CREATE: {
		HWND hLabel = CreateWindowEx(
			0,
			L"STATIC",
			L"WPM",
			WS_CHILD | WS_VISIBLE,
			120, 10, 200, 20,
			m_hwnd,
			NULL,
			GetModuleHandle(NULL),
			NULL
		);

		hEdit = CreateWindowEx(
			WS_EX_CLIENTEDGE,
			L"EDIT",
			L"60",
			WS_CHILD | WS_VISIBLE | WS_BORDER |
			ES_LEFT | ES_AUTOHSCROLL,
			10, 10, 100, 25,
			m_hwnd,
			(HMENU)1,
			GetModuleHandle(NULL),
			NULL

		);

		HWND hLabel2 = CreateWindowEx(
			0,
			L"STATIC",
			L"HUMANIZATION:",
			WS_CHILD | WS_VISIBLE,
			10, 50, 200, 20,
			m_hwnd,
			NULL,
			GetModuleHandle(NULL),
			NULL
		);

		jumpCheck = CreateWindowEx(
			0,
			L"BUTTON",
			L"Jump",
			WS_CHILD | WS_VISIBLE | BS_CHECKBOX,
			10, 70, 200, 25,
			m_hwnd,
			(HMENU)2,
			GetModuleHandle(NULL),
			NULL
		);

		randomOffsetCheck = CreateWindowEx(
			0,
			L"BUTTON",
			L"Sine Oscillation",
			WS_CHILD | WS_VISIBLE | BS_CHECKBOX,
			10, 95, 200, 25,
			m_hwnd,
			(HMENU)2,
			GetModuleHandle(NULL),
			NULL
		);

		SendMessage(randomOffsetCheck, BM_SETCHECK, BST_CHECKED, 0);

		return 0;
	}

	case WM_COMMAND: {
		if (LOWORD(wParam) == 2) {
			HWND hCheck = (HWND)lParam;

			LRESULT state = SendMessage(hCheck, BM_GETCHECK, 0, 0);

			SendMessage(hCheck, BM_SETCHECK,
				state == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED,
				0
			);
		}

		return 0;
	}

	case WM_SIZE: {
		int width = LOWORD(lParam);
		int height = HIWORD(lParam);

		OnSize(m_hwnd, (UINT)wParam, width, height);

		return 0;
	}

	case WM_DESTROY: {
		PostQuitMessage(0);
		return 0;
	}
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(m_hwnd, &ps);

		StateInfo* state = GetAppState(m_hwnd);

		if (state) {
			FillRect(hdc, &ps.rcPaint, state->brush.get());
		}

		EndPaint(m_hwnd, &ps);

		return 0;
	}
	}

	return DefWindowProc(m_hwnd, uMsg, wParam, lParam);
}



int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
	/*
	AllocConsole();

	FILE* f;
	freopen_s(&f, "CONOUT$", "w", stdout);
	freopen_s(&f, "CONOUT$", "w", stderr);
	freopen_s(&f, "CONIN$", "r", stdin);

	std::cout << "Console is active..." << std::endl;
	*/

	MainWindow win;


	if (!win.Create(
		L"Human Typer (F7 to start)",
		WS_OVERLAPPEDWINDOW,
		0,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		500,
		400
	)) {
		return 0;
	}

	HWND hwnd = win.Window();

	ShowWindow(hwnd, nCmdShow);
	UpdateWindow(hwnd);

	SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

	StateInfo* state = GetAppState(hwnd);
	if (!state) {
		MessageBox(hwnd, L"Failed to get application state data.", L"Error", MB_OK | MB_ICONERROR);
		return 0;
	}

	std::thread worker([&]() {

		std::random_device rd;
		std::mt19937 gen(rd());
		std::uniform_real_distribution<double> probDist(0.0, 1.0);
		std::uniform_int_distribution<int> delayDist(1, 4);

		while (state->running) {
			wchar_t buffer[512];
			GetWindowText(win.hEdit, buffer, 512);

			try {
				state->wpm = std::stoi(buffer);
			}
			catch (...) {

			}


			if (!state->active.load()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				continue;
			}

			for (char ch : wstring_to_utf8(GetClipboardText())) {
				if (!state->active.load() || !state->running.load())
					break;

				if (SendMessage(win.jumpCheck, BM_GETCHECK, 0, 0)) {



					double probability = probDist(gen);

					if (probability < JUMP_PROBABILITY) {
						std::this_thread::sleep_for(std::chrono::seconds(delayDist(gen)));
					}
				}

				KeyboardInput(ch);

				double offset = 1.0;

				if (SendMessage(win.randomOffsetCheck, BM_GETCHECK, 0, 0)) {
					auto now = std::chrono::steady_clock::now().time_since_epoch();
					double t = std::chrono::duration<double>(now).count();

					offset = 1.0 + SINE_AMPLITUDE * std::sin(t * 5.0);
				}

				std::this_thread::sleep_for(
					std::chrono::milliseconds(
						static_cast<int>(((1000 / (state->wpm / 12.0))) * offset)
					)
				);
			}

			state->active.store(false);
		}
		});

	std::thread inputThread(checkInputs, std::ref(state->active), hwnd);


	MSG msg;
	while (GetMessage(&msg, nullptr, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}


	state->running.store(false);
	worker.join();
	inputThread.join();

	return 0;
}
