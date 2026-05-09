#include <windows.h>
#include <commdlg.h>
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
#include <fstream>
#include <sstream>
#include <mutex>
#include <commctrl.h>

#define ID_FILE_OPEN   0x1
#define ID_FILE_EXIT   0x2

#define ID_CHECK_JUMP  0x64
#define ID_CHECK_SINE  0x65
#define ID_CHECK_TYPOS 0x66

#define ID_WPM_EDIT 0x67
#define ID_CLIPBOARD_EDIT 0x68

#define ID_LOAD_CLIPBOARD_BUTTON 0x69
#define ID_CHAR_POSITION 0x70

constexpr int START = 0x76;
constexpr int PAUSE = 0x77;
constexpr int END = 0x78;

constexpr double SINE_AMPLITUDE = 0.25;
constexpr double JUMP_PROBABILITY = 0.03;
constexpr double TYPO_PROBABILITY = 0.03;

std::mutex clipboardTextMutex;
std::string clipboardTextboxText;

namespace KeyboardLayout {
	inline const std::vector<std::string> QWERTY = {
		"1234567890-=",
		"qwertyuiop",
		"asdfghjkl",
		"zxcvbnm"
	};
}

enum class ProgramState {
	Stopped = 0x0,
	Paused = 0x1,
	Active = 0x2
};

bool OpenFileDialog(HWND hwnd, wchar_t* outFile, DWORD size) {
	OPENFILENAME ofn = {0 };
	wchar_t fileName[MAX_PATH] = L"";

	ZeroMemory(&ofn, sizeof(ofn));

	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = hwnd;
	ofn.lpstrFile = outFile;
	ofn.nMaxFile = size;

	ofn.lpstrFilter = L"All Files\0*.*\0Text Files\0*.txt\0\0";
	ofn.nFilterIndex = 1;

	ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_EXPLORER;

	BOOL ok = GetOpenFileName(&ofn);

	if (!ok) {
		DWORD err = CommDlgExtendedError();

		std::wstring msg = L"Open dialog failed. Error code: " + std::to_wstring(err);

		MessageBox(hwnd, msg.c_str(), L"Error", MB_OK);
	}

	return ok;

}

std::vector<char> getAdjacentKeys(char ch, const std::vector<std::string>& keyboardLayout) {
	std::vector<char> adjacent;

	ch = tolower(ch);
	for (size_t r = 0; r < keyboardLayout.size(); ++r) {
		const std::string& row = keyboardLayout[r];
		const size_t c = row.find(ch);

		if (c != std::string::npos) {
			for (int dR = -1; dR <= 1; ++dR) {
				for (int dC = -1; dC <= 1; ++dC) {
					if (dR == 0 && dC == 0) continue;

					int newRow = r + dR;
					int newCol = c + dC;

					if (newRow >= 0 && newRow < keyboardLayout.size()
						&& newCol >= 0 && newCol < keyboardLayout[newRow].size()) {
						adjacent.push_back(keyboardLayout[newRow][newCol]);
					}

				}
			}
		}
	}
	return adjacent;

}

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
	std::atomic<int> active{ (int)ProgramState::Stopped };

	std::string clipboardText;

	int wpm = 60;

	RAIIBrush brush{ RGB(240, 240, 240) };


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

std::wstring utf8_to_wstring(const std::string& str) {
	if (str.empty()) 
		return L"";

	int sizeNeeded = MultiByteToWideChar(
		CP_UTF8, 
		0, 
		str.c_str(), 
		-1, 
		nullptr, 
		0
	);

	if (sizeNeeded <= 0)
		return L"";

	std::wstring wtext(sizeNeeded, L'\0');

	MultiByteToWideChar(
		CP_UTF8, 
		0, 
		str.c_str(),
		-1, 
		&wtext[0], 
		sizeNeeded
	);

	if (!wtext.empty() && wtext.back() == L'\0')
		wtext.pop_back();

	return wtext;
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

	std::wstring result(text);

	ScopeExit unlock([&] { GlobalUnlock(hData); });

	return std::wstring(result);
}

void KeyboardInput(char ch) {
	SHORT result = VkKeyScanA(ch);

	if (result == -1)
		return;

	BYTE vk = LOBYTE(result);
	BYTE shift = HIBYTE(result);

	
	

	if (shift & 1) {
		INPUT ip = { 0 };
		ip.type = INPUT_KEYBOARD;
		ip.ki.wVk = VK_SHIFT;
		
		SendInput(1, &ip, sizeof(INPUT));
	}

	INPUT ip = { 0 };
	ip.type = INPUT_KEYBOARD;

	ip.ki.wScan = 0;
	ip.ki.time = 0;

	ip.ki.wVk = vk;
	ip.ki.dwFlags = 0;
	SendInput(1, &ip, sizeof(INPUT));

	ip.ki.dwFlags = KEYEVENTF_KEYUP;
	SendInput(1, &ip, sizeof(INPUT));

	if (shift & 1) {
		INPUT ip = { 0 };
		ip.type = INPUT_KEYBOARD;
		ip.ki.wVk = VK_SHIFT;
		ip.ki.dwFlags = KEYEVENTF_KEYUP;
		SendInput(1, &ip, sizeof(INPUT));
	}
}

void KeyboardInput(WORD vk) {
	INPUT ip = { 0 };
	ip.type = INPUT_KEYBOARD;

	ip.ki.wVk = vk;
	ip.ki.dwFlags = 0;
	SendInput(1, &ip, sizeof(INPUT));

	ip.ki.dwFlags = KEYEVENTF_KEYUP;
	SendInput(1, &ip, sizeof(INPUT));
}



void checkInputs(std::atomic<int>& active, HWND hwnd) {
	StateInfo* state = GetAppState(hwnd);
	if (!state) { return; }

	while (state->running) {
		if (GetAsyncKeyState(START) & 1) {
			active = (int)ProgramState::Active;

			SetWindowText(hwnd, L"Human Typer (F8 to pause, F9 to stop)");
		}
		else if (GetAsyncKeyState(END) & 1) {
			active = (int)ProgramState::Stopped;

			SetWindowText(hwnd, L"Human Typer (F7 to start)");
		}
		else if (GetAsyncKeyState(PAUSE) & 1) {
			active = (int)ProgramState::Paused;

			SetWindowText(hwnd, L"Human Typer (F7 to unpause)");
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

		INITCOMMONCONTROLSEX icc = {};
		icc.dwSize = sizeof(icc);
		icc.dwICC = ICC_PROGRESS_CLASS;
		InitCommonControlsEx(&icc);

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
	HWND hClipboardEdit = nullptr;
	HWND hJumpCheck = nullptr;
	HWND hRandomOffsetCheck = nullptr;
	HWND hTyposCheck = nullptr;
	HWND hLoadClipboardButton = nullptr;
	HWND hProgressBar = nullptr;
	HWND hCharPosition = nullptr;
};

inline StateInfo* GetAppState(HWND hwnd) {
	auto pMain = reinterpret_cast<MainWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
	return pMain ? &pMain->state : nullptr;
}

class File {
public:
	explicit File(const std::string& path) :
		f(path, std::ios::binary) {

		if (!f.is_open()) {
			throw std::runtime_error(std::format("Failed to open file \"{}\"", path));
		}
	}
	~File() {
		f.close();
	}

	void Contents(std::string& buffer) {
		std::ostringstream ss;
		ss << f.rdbuf();
		buffer = ss.str();
	}
private:
	std::ifstream f;
};

LRESULT MainWindow::HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) {


	switch (uMsg) {
	case WM_CREATE: {
		HMENU hMenu = CreateMenu();

		HMENU hFileMenu = CreatePopupMenu();
		AppendMenu(hFileMenu, MF_STRING, ID_FILE_OPEN, L"Open");

		AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hFileMenu, L"File");

		SetMenu(m_hwnd, hMenu);
		DrawMenuBar(m_hwnd);

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

		hJumpCheck = CreateWindowEx(
			0,
			L"BUTTON",
			L"Jump",
			WS_CHILD | WS_VISIBLE | BS_CHECKBOX,
			10, 70, 200, 25,
			m_hwnd,
			(HMENU)ID_CHECK_JUMP,
			GetModuleHandle(NULL),
			NULL
		);

		hRandomOffsetCheck = CreateWindowEx(
			0,
			L"BUTTON",
			L"Sine Oscillation",
			WS_CHILD | WS_VISIBLE | BS_CHECKBOX,
			10, 95, 200, 25,
			m_hwnd,
			(HMENU)ID_CHECK_SINE,
			GetModuleHandle(NULL),
			NULL
		);

		hTyposCheck = CreateWindowEx(
			0,
			L"BUTTON",
			L"Typos (fixes them)",
			WS_CHILD | WS_VISIBLE | BS_CHECKBOX,
			10, 120, 200, 25,
			m_hwnd,
			(HMENU)ID_CHECK_TYPOS,
			GetModuleHandle(NULL),
			NULL
		);

		hClipboardEdit = CreateWindowEx(
			WS_EX_CLIENTEDGE,
			L"EDIT",
			L"Lorem ipsum dolor sit amet...",
			WS_CHILD | WS_VISIBLE | WS_BORDER |
			ES_LEFT | ES_AUTOHSCROLL | ES_MULTILINE,
			300, 60, 300, 200,
			m_hwnd,
			(HMENU)ID_CLIPBOARD_EDIT,
			GetModuleHandle(NULL),
			NULL

		);

		hLoadClipboardButton = CreateWindowEx(
			0,
			L"BUTTON",
			L"Load clipboard data",
			WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
			300, 270, 200, 30,
			m_hwnd,
			(HMENU)ID_LOAD_CLIPBOARD_BUTTON,
			GetModuleHandle(NULL),
			NULL
		);

		hCharPosition = CreateWindowEx(
			0,
			L"STATIC",
			L"Char position:",
			WS_CHILD | WS_VISIBLE | ES_READONLY | ES_LEFT,
			300, 10, 200, 20,
			m_hwnd,
			NULL,
			GetModuleHandle(NULL),
			NULL
		);

		hProgressBar = CreateWindowEx(
			0,
			PROGRESS_CLASS,
			NULL,
			WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
			300, 30, 300, 20,
			m_hwnd,
			NULL,
			GetModuleHandle(NULL),
			NULL
		);

		SendMessage(hProgressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

		//SendMessage(loadClipboardButton, WM_SETFONT, (WPARAM)hFont, TRUE);
		
		
		std::wstring clipboardText = GetClipboardText();
		SendMessage(hClipboardEdit, WM_SETTEXT, wParam, (LPARAM)clipboardText.c_str());

		wchar_t buffer[4096];
		SendMessageW(hClipboardEdit, WM_GETTEXT, 4096, (LPARAM)buffer);

		std::lock_guard<std::mutex> lock(clipboardTextMutex);
		clipboardTextboxText = wstring_to_utf8(buffer);

		SendMessage(hRandomOffsetCheck, BM_SETCHECK, BST_CHECKED, 0);

		return 0;
	}

	case WM_COMMAND: {
		int id = LOWORD(wParam);
		int code = HIWORD(wParam);

		HWND hCtrl = (HWND)lParam;


		if (id == ID_CLIPBOARD_EDIT && code == EN_CHANGE) {
			wchar_t buffer[4096];
			SendMessageW(hClipboardEdit, WM_GETTEXT, (WPARAM)4096, (LPARAM)buffer);

			{
				std::lock_guard<std::mutex> lock(clipboardTextMutex);
				clipboardTextboxText = wstring_to_utf8(buffer);
			}
			
		}

		//menu options
		if (hCtrl == nullptr) {
			switch (id) {
			case ID_FILE_OPEN: { // open
				wchar_t buffer[512] = { 0 };


				if (!OpenFileDialog(m_hwnd, buffer, 512) || buffer[0] == L'\0') {

					return 1;
				}


				std::wstring msg = std::wstring(L"Would you like to use \"") + buffer + std::wstring(L"\"?");
				int dialogResult = MessageBoxW(m_hwnd, msg.c_str(), L"Open File", MB_OKCANCEL);

				if (dialogResult == IDOK) {
					std::string path = wstring_to_utf8(buffer);

					File f(path);
					std::string fileText;
					f.Contents(fileText);

					SendMessage(hClipboardEdit, WM_SETTEXT, 0, (LPARAM)(utf8_to_wstring(fileText)).c_str());
					SendMessageW(hClipboardEdit, WM_GETTEXT, 4096, (LPARAM)buffer);

					std::lock_guard<std::mutex> lock(clipboardTextMutex);
					clipboardTextboxText = wstring_to_utf8(buffer);
					

				}

				break;
			}
			}
		}

		// checkboxes
		if (id == ID_CHECK_JUMP ||
			id == ID_CHECK_SINE ||
			id == ID_CHECK_TYPOS) {
			HWND hCheck = (HWND)lParam;

			LRESULT state = SendMessage(hCheck, BM_GETCHECK, 0, 0);

			SendMessage(hCheck, BM_SETCHECK,
				state == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED,
				0
			);
		}

		if (id == ID_LOAD_CLIPBOARD_BUTTON) {
			std::wstring clipboardText = GetClipboardText();
			SendMessage(hClipboardEdit, WM_SETTEXT, 0, (LPARAM)clipboardText.c_str());

			wchar_t buffer[4096];
			SendMessageW(hClipboardEdit, WM_GETTEXT, 4096, (LPARAM)buffer);

			std::lock_guard<std::mutex> lock(clipboardTextMutex);
			clipboardTextboxText = wstring_to_utf8(buffer);
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

	std::vector<char> adj = getAdjacentKeys('a', KeyboardLayout::QWERTY);
	std::cout << std::string(adj.begin(), adj.end()) << std::endl;
	
	*/
	

	MainWindow win;

	if (!win.Create(
		L"Human Typer (F7 to start)",
		WS_OVERLAPPEDWINDOW,
		0,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		800,
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
		std::uniform_int_distribution<int> delayDist(250, 2500);

		while (state->running) {
			wchar_t wpmBuffer[512];

			if (state->active.load() != (int)ProgramState::Active) {
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				continue;
			}

			std::string clipboardTextboxTextCopy;

			{
				std::lock_guard<std::mutex> lock(clipboardTextMutex);
				clipboardTextboxTextCopy = clipboardTextboxText;
			}


			for (int c = 0; c < clipboardTextboxTextCopy.size(); ++c) {
				
				if (clipboardTextboxTextCopy != clipboardTextboxText)
					break;

				SendMessageW(win.hEdit, WM_GETTEXT, (WPARAM)2048, (LPARAM)wpmBuffer);
				while (state->active.load() == (int)ProgramState::Paused) {
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
				}

				try {
					state->wpm = std::stoi(wpmBuffer);
				}
				catch (...) {

				}

				std::wstring charPositionText = L"Char position: " + std::to_wstring(c);
				SendMessage(win.hCharPosition, WM_SETTEXT, 0, (LPARAM)charPositionText.c_str());
				
				int percent = (int)((c * 100) / clipboardTextboxTextCopy.size());
				SendMessage(win.hProgressBar, PBM_SETPOS, percent, 0);
				

				double offset = 1.0;

				if (SendMessage(win.hRandomOffsetCheck, BM_GETCHECK, 0, 0)) {
					auto now = std::chrono::steady_clock::now().time_since_epoch();
					double t = std::chrono::duration<double>(now).count();

					offset = 1.0 + SINE_AMPLITUDE * std::sin(t * 5.0);
				}

				int miliOffset = static_cast<int>(((1000 / (state->wpm / 12.0))) * offset);

				char ch = clipboardTextboxTextCopy[c];

				if (state->active.load() == (int)ProgramState::Stopped || !state->running.load())
					break;

				if (SendMessage(win.hJumpCheck, BM_GETCHECK, 0, 0)) {



					double probability = probDist(gen);

					if (probability < JUMP_PROBABILITY) {
						std::this_thread::sleep_for(std::chrono::milliseconds(delayDist(gen)));
					}

				}

				if (SendMessage(win.hTyposCheck, BM_GETCHECK, 0, 0)) {
					double probability = probDist(gen);

					if (probability < TYPO_PROBABILITY) {
						std::vector<char> adj = getAdjacentKeys(ch, KeyboardLayout::QWERTY);

						if (adj.empty()) {
							KeyboardInput(ch);
							continue;
						}

						std::uniform_int_distribution<size_t> randomTypoDist(0, adj.size() - 1);
						

						std::uniform_int_distribution<size_t> typoAmountDist(1, 3);
						size_t typos = typoAmountDist(gen);

						for (int _ = 0; _ < typos; ++_) {
							KeyboardInput(adj[randomTypoDist(gen)]);

							std::this_thread::sleep_for(std::chrono::milliseconds(miliOffset));
							
						}

						std::this_thread::sleep_for(std::chrono::milliseconds(delayDist(gen)));

						for (int _ = 0; _ < typos; ++_) {
							KeyboardInput((WORD)VK_BACK);
							std::this_thread::sleep_for(std::chrono::milliseconds(miliOffset));
						}

					}
				}
				KeyboardInput(ch);

				std::this_thread::sleep_for(std::chrono::milliseconds(miliOffset));
			}

			SendMessage(win.hCharPosition, WM_SETTEXT, 0, (LPARAM)L"Char position: 0");
			SendMessage(win.hProgressBar, PBM_SETPOS, 0, 0);
			state->active.store((int)ProgramState::Stopped);
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
