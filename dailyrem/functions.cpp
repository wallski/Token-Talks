#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <iostream>
#include <string>
#include <cstdlib>
#include <limits>

void clearScreen() {
#ifdef _WIN32
    system("cls");
#endif
}

void pauseAndClear() {
    std::cout << "\nPress Enter to continue";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    std::cin.get(); 
    clearScreen();
}
