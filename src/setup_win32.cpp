// Win32 helpers shared by the two programs, kept apart from raylib's headers.
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>

#include "orochi_settings.h"

#include <cstdio>
#include <string>

bool orochiRunProgram(const wchar_t *name)
{
    wchar_t self[MAX_PATH * 4];
    const DWORD length = GetModuleFileNameW(nullptr, self, MAX_PATH * 4);
    if (length == 0)
        return false;
    const auto program = std::filesystem::path(self).parent_path() / name;
    std::wstring commandLine = L"\"" + program.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(program.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        program.parent_path().c_str(), &startup, &process))
        return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool orochiPickDisc(void *ownerWindow, std::filesystem::path &disc)
{
    wchar_t file[MAX_PATH * 4] = L"";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = static_cast<HWND>(ownerWindow);
    dialog.lpstrFilter = L"Imagem de disco (*.iso)\0*.iso\0Todos os arquivos (*.*)\0*.*\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = MAX_PATH * 4;
    dialog.lpstrTitle = L"Selecione a ISO do Warriors Orochi";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog))
        return false;
    disc = file;
    return true;
}

void orochiRestoreWindow(void *window)
{
    const HWND handle = static_cast<HWND>(window);
    if (IsIconic(handle))
        ShowWindow(handle, SW_RESTORE);
    SetForegroundWindow(handle);
}

void orochiLogToFile(const std::filesystem::path &path)
{
    if (GetStdHandle(STD_ERROR_HANDLE) != nullptr)
        return;
    // Not the _s versions: they lock the file, and both streams share it.
    _wfreopen(path.c_str(), L"w", stdout);
    _wfreopen(path.c_str(), L"a", stderr);
}
