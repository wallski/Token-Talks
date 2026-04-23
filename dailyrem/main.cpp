#include <windows.h>
#include "gui.h"

int main() {

    HWND consoleWnd = GetConsoleWindow();
    if (consoleWnd) {
        ShowWindow(consoleWnd, SW_HIDE);
    }
    
    return RunGUI();
}