#include "keymap.h"
#include <Psapi.h>
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mmsystem.h>
#include <mutex>
#include <optional>
#include <profileapi.h>
#include <queue>
#include <regex>
#include <stdio.h>
#include <string>
#include <tchar.h>
#include <thread>
#include <tlhelp32.h>
#include <vector>
#include <winnt.h>
#include <winuser.h>
#include "macros.h"
#include <iostream>

#pragma comment(lib, "winmm.lib")
using namespace std::chrono_literals;

static void CreateLogConsole() {
    AllocConsole();

    // Redirect stdout to the console
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    freopen_s(&fp, "CONIN$", "r", stdin);

    SetConsoleTitleA("Log Console");
}

std::string getActiveProcessName() {
  HWND foregroundWindow = GetForegroundWindow();
  if (foregroundWindow == NULL) {
    return "No active window";
  }

  DWORD processId;
  GetWindowThreadProcessId(foregroundWindow, &processId);

  HANDLE processHandle = OpenProcess(
      PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
  if (processHandle == NULL) {
    return "Failed to open process";
  }

  TCHAR processName[MAX_PATH];
  if (GetModuleFileNameEx(processHandle, NULL, processName, MAX_PATH) == 0) {
    CloseHandle(processHandle);
    return "Failed to get process name";
  }

  CloseHandle(processHandle);

  // To get just the executable name from the full path
  std::wstring fullPath(processName, processName + lstrlen(processName));
  size_t lastBackslash = fullPath.find_last_of(L"\\");
  if (lastBackslash != std::wstring::npos) {
    return std::string(fullPath.begin() + lastBackslash + 1, fullPath.end());
  }

  std::cout << "Full path: " << std::string(fullPath.begin(), fullPath.end()) << std::endl;
  return std::string(fullPath.begin(), fullPath.end());
}

namespace InputHandler {
void queueTask(int delay, std::optional<std::function<void()>> function,
               bool recursive);
std::optional<WORD> findKey(const std::string &keyToFind);

struct Task {
  int delay;
  std::optional<std::function<void()>> function;
  bool recursive;
};
void queueTask(Task task);
void queueInputs(std::vector<std::string> inputs,
                 std::function<void()> callback = nullptr);
extern std::queue<Task> queuedTasks;
} // namespace InputHandler

class Keybind {
public:
  Keybind(int keyCode, std::function<void()> function,
          std::vector<std::string> modifiers = {}) {
    this->keyCode = keyCode;
    this->isPressed = false;
    this->modifiers = modifiers;
    this->function = [function]() {
      if (InputHandler::queuedTasks.empty()) {
        InputHandler::queueTask(0, function, false);
      }
    };
    keybinds.push_back(*this);
  }

  Keybind(const std::string &key, std::function<void()> function,
          std::vector<std::string> modifiers = {})
      : Keybind(InputHandler::findKey(key).value(), function,
                modifiers) { // This should always have a value
  }

  static std::vector<Keybind> keybinds;
  bool isPressed;
  DWORD keyCode;
  std::function<void()> function;
  std::vector<std::string> modifiers;
};

std::vector<Keybind> Keybind::keybinds = {};

namespace InputHandler {

bool getPhysicalKeyState(WORD vkCode) {
  for (Keybind &keybind : Keybind::keybinds) {
    if (vkCode == keybind.keyCode) {
      return keybind.isPressed;
    }
  }
  // Return Windows API KeyState if there is no keybind with that keycode.
  return (GetAsyncKeyState(vkCode) & 0x8000) != 0;
}

std::optional<WORD> findKey(const std::string &keyToFind) {
  std::string lowerCaseKey = keyToFind;
  std::transform(lowerCaseKey.begin(), lowerCaseKey.end(), lowerCaseKey.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  std::optional<WORD> vkCode;
  for (size_t i = 0; i < g_key_to_vk_size; ++i) {
    if (g_key_to_vk[i].keyName == lowerCaseKey) {
      vkCode = g_key_to_vk[i].vkCode;
      break;
    }
  }

  if (!vkCode.has_value()) {
    SHORT vk = VkKeyScan(lowerCaseKey[0]);
    if (vk == -1) {
      printf("Failed to find keycode for: %s", lowerCaseKey.c_str());
      return std::nullopt;
    }
    vkCode = LOBYTE(vk);
  }
  return vkCode;
}

void sendKeyInput(WORD vkCode, bool pressDown) {
  INPUT input = {0};
  // add specific handling for mousewheel and mouse buttons cause i was really
  // lazy
  if (vkCode == VK_LBUTTON || vkCode == VK_RBUTTON || vkCode == VK_MBUTTON) {
    input.type = INPUT_MOUSE;
    input.mi.time = 0;
    input.mi.dwExtraInfo = 0;

    switch (vkCode) {
    case VK_LBUTTON:
      input.mi.dwFlags = pressDown ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
      break;
    case VK_RBUTTON:
      input.mi.dwFlags =
          pressDown ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
      break;
    case VK_MBUTTON:
      input.mi.dwFlags =
          pressDown ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
      break;
    }
    SendInput(1, &input, sizeof(INPUT));
  } else if (vkCode == 0x1001 || vkCode == 0x1000) {
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    input.mi.mouseData = (vkCode == 0x1001) ? WHEEL_DELTA : -WHEEL_DELTA;
    input.mi.time = 0;
    input.mi.dwExtraInfo = 0;
    SendInput(1, &input, sizeof(INPUT));
    input.mi.mouseData = 0;
    SendInput(1, &input, sizeof(INPUT));
  } else {
    input.type = INPUT_KEYBOARD;

    input.ki.wVk = vkCode;
    input.ki.wScan = MapVirtualKey(vkCode, MAPVK_VK_TO_VSC);
    input.ki.time = 0;
    input.ki.dwExtraInfo = 0;
    input.ki.dwFlags = KEYEVENTF_SCANCODE;

    switch (
        vkCode) { // For some keycodes you need to add this flag or something
    case VK_UP:
    case VK_DOWN:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_INSERT:
    case VK_DELETE:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_LMENU:
    case VK_RMENU:
    case VK_APPS:
      input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
      break;
    }

    if (!pressDown) {
      input.ki.dwFlags |= KEYEVENTF_KEYUP;
    }
    SendInput(1, &input, sizeof(INPUT));
  }
}

std::queue<Task> queuedTasks = {};
std::mutex queuedTasksMutex;

void queueTask(Task task) {
  std::lock_guard<std::mutex> lock(queuedTasksMutex);
  queuedTasks.push(task);
}

void queueTask(int delay, std::optional<std::function<void()>> function,
               bool recursive) {
  std::lock_guard<std::mutex> lock(queuedTasksMutex);
  queuedTasks.push({delay, function, recursive});
}

void queueInput(WORD vkCode, std::optional<bool> state, bool recursive) {
  auto enqueue = [&](bool press, bool recursiveInput) {
    queueTask(
        0,
        [vkCode, press]() {
          sendKeyInput(vkCode, press);
          printf("%.3f sending %hu, state: %d\n",
                 std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now().time_since_epoch())
                     .count(),
                 vkCode, press);
        },
        recursiveInput);
  };

  if (state.has_value()) {
    enqueue(state.value(), recursive);
  } else {
    enqueue(true, false);
    enqueue(false, recursive);
  }
}

std::regex inputPattern(R"((\w+?)(?:\s(down|up|\d+))?(R)?)");
void queueInputs(std::vector<std::string> inputs,
                 std::function<void()> callback) {
  for (size_t i = 0; i < inputs.size(); ++i) {
    const std::string &input = inputs[i];
    std::smatch matches;
    if (!std::regex_match(input, matches, inputPattern)) {
      return;
    }
    std::string inputName = matches[1];
    std::string secondArg = matches[2];
    bool isRecursive = matches[3].matched;

    std::optional<bool> state = std::nullopt;
    int amount = 1;
    if (matches[2].matched) {
      if (secondArg == "down") {
        state = true;
      } else if (secondArg == "up") {
        state = false;
      } else if (!secondArg.empty() &&
                 std::all_of(secondArg.begin(), secondArg.end(), ::isdigit)) {
        amount = std::stoi(secondArg);
      }
    }

    WORD vkCode;
    if (inputName == "sleep") {
      for (int i = 0; i < amount; i++) {
        queueTask(0, std::nullopt, isRecursive);
      }
      continue;
    } else {
      std::optional<WORD> keyOpt = findKey(inputName);
      if (!keyOpt.has_value()) {
        return;
      }
      vkCode = keyOpt.value();

      printf("Key code for '%s': %hd\n", input.c_str(), vkCode);
    }

    if (inputName == "wheelup" || inputName == "wheeldown") {
      queueInput(vkCode, true, false);
      queueTask(0, std::nullopt, false);
      continue;
    }

    // Schizo up and down logic because it is faster
    if ((inputName == "up" || inputName == "down") && amount != 1 &&
        !state.has_value()) {
      WORD wheelInput = findKey("wheel" + inputName).value();
      for (int i = 0; i < floor(amount / 2); i++) {
        queueInput(vkCode, true, false);
        queueInput(vkCode, false, true);
        queueInput(wheelInput, false, false);
        if (amount >= 3) {
          queueTask(0, std::nullopt, false);
        }
      }
      if (amount & 1) {
        queueInput(vkCode, true, false);
        queueInput(vkCode, false, true);
      }
      continue;
    }

    for (int i = 0; i < amount; i++) {
      queueInput(vkCode, state, isRecursive);
    }
  }
  if (callback) {
    queueTask(0, callback, true);
  }
}

void executeFirstQueuedTask() {
  while (true) {
    Task firstTaskCopy;
    {
      std::lock_guard<std::mutex> lock(queuedTasksMutex);
      if (queuedTasks.empty()) {
        break;
      }
      Task &firstTaskReference = queuedTasks.front();
      if (--firstTaskReference.delay < 0) {
        firstTaskCopy = firstTaskReference;
        queuedTasks.pop();
      } else {
        break;
      }
    }
    if (firstTaskCopy.function.has_value()) {
      firstTaskCopy.function.value()();
    }
    if (!firstTaskCopy.recursive) {
      break;
    }
  }
}

void prepareForIntMenu() { queueInputs({"lbutton upR", "rbutton upR"}); }

} // namespace InputHandler

bool inChat = false;

void addKeybinds() { // Add keybinds here
  // You can't type this keycode as a string so i just typed in the virtual
  // keycode of it instead
  new Keybind(220, []() {
    InputHandler::prepareForIntMenu();
    InputHandler::queueInputs({"mR", "enter down", "enter up", "enter downR",
                               "down 4", "enter up", "enter downR", "down down",
                               "enter up", "down up"});
  });

  new Keybind("F2", []() {
    InputHandler::prepareForIntMenu();
    InputHandler::queueInputs({"mR", "enter down", "down 4", "enter up",
                               "enter", "sleep", "enter", "enter downR",
                               "up down", "enter up", "up up", "m"});
  });

  new Keybind(221,
              []() {
                InputHandler::prepareForIntMenu();
                InputHandler::queueInputs(
                    {"mR", "enter down", "up 6", "enter up", "down downR",
                     "enter down", "down up", "enter upR", "sleep 2",
                     "space downR", "m down", "m upR", "space up"});
              },
              {"shift"});

  new Keybind(186,
              []() {
                InputHandler::prepareForIntMenu();
                InputHandler::queueInputs(
                    {"mR", "enter down", "up 7", "enter up", "down downR",
                     "enter down", "down up", "down", "enter up"});
              },
              {"shift"});

  new Keybind("1",
              []() { InputHandler::queueInputs({"1 down", "tabR", "1 up"}); });
  new Keybind("2",
              []() { InputHandler::queueInputs({"2 down", "tabR", "2 up"}); });
  new Keybind("3",
              []() { InputHandler::queueInputs({"3 down", "tabR", "3 up"}); });
  new Keybind("4",
              []() { InputHandler::queueInputs({"4 down", "tabR", "4 up"}); });
  new Keybind("5",
              []() { InputHandler::queueInputs({"5 down", "tabR", "5 up"}); });
  new Keybind("6",
              []() { InputHandler::queueInputs({"6 down", "tabR", "6 up"}); });
  new Keybind("7",
              []() { InputHandler::queueInputs({"7 down", "tabR", "7 up"}); });
  new Keybind("8",
              []() { InputHandler::queueInputs({"8 down", "tabR", "8 up"}); });

  new Keybind("q", []() {
    InputHandler::queueInputs(
        {"4 down", "sleep 2", "2 down", "sleep 2", "tabR", "2 upR", "4 up"});
  });

  new Keybind("F6", []() {
      InputHandler::queueInputs({ "enter downR", "t", "hR", "eR", "lR", "lR", "o", "enter up" });
  });
}

// Store the original game WndProc here so we can restore it or pass unused keys back
WNDPROC oWndProc;

static LRESULT APIENTRY WndProcHook(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {

    // WM_KEYDOWN and WM_SYSKEYDOWN (System keys like Alt)
    if (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) {
        DWORD vkCode = (DWORD)wParam;

        for (Keybind& keybind : Keybind::keybinds) {
            bool modifiersPressed =
                keybind.modifiers.size() != 0
                ? std::all_of(
                    keybind.modifiers.begin(), keybind.modifiers.end(),
                    [](std::string modifier) {
                        std::optional<WORD> key = InputHandler::findKey(modifier);
                        return InputHandler::getPhysicalKeyState(key.value());
                    })
                : true;

            if (vkCode == keybind.keyCode && !keybind.isPressed && modifiersPressed) {
                keybind.isPressed = true;

                if (!inChat) {
                    keybind.function();
                }

                return 0;
            }
        }

        printf("Key Down: %lu\n", vkCode);
    }

    else if (uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP) {
        DWORD vkCode = (DWORD)wParam;

        for (Keybind& keybind : Keybind::keybinds) {
            if (vkCode == keybind.keyCode) {
                keybind.isPressed = false;
                return 0;
            }
        }
        printf("Key Up: %lu\n", vkCode);
    }

    return CallWindowProc(oWndProc, hwnd, uMsg, wParam, lParam);
}

void initMacros() {
    CreateLogConsole();

    addKeybinds();

    HWND targetWindow = FindWindowA("sgaWindow", NULL);

    if (!targetWindow) {
        printf("Failed to find GTA 5 window. I'll just try again cause I'm nonchalant.\n");
        while (!targetWindow) {
            targetWindow = FindWindowA("sgaWindow", NULL);
            Sleep(1000);
        }
    }

    oWndProc = (WNDPROC)SetWindowLongPtr(targetWindow, GWLP_WNDPROC, (LONG_PTR)WndProcHook);

    if (!oWndProc) {
        printf("Failed to hook WndProc. Error: %lu\n", GetLastError());
        return;
    }

    printf("WndProc Hook Installed successfully.\n");

}