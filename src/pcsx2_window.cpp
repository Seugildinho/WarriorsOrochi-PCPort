// The child window PCSX2's GS presents to: it covers the client area of the
// game window (raylib's), which keeps keyboard input because the child is
// disabled. Win32 only, kept apart from raylib's headers.
// ==========================================================================
// JANELA DE DESENHO DO PCSX2 + TECLAS F9 E F12
// ==========================================================================
// Como a janela e montada:
//   - A janela principal do jogo e criada pelo raylib (ela recebe teclado,
//     controle, F10 de tela cheia etc.).
//   - Por cima da area interna dela, criamos uma janela FILHA, desabilitada
//     (WS_DISABLED), onde o Direct3D 11 do PCSX2 desenha. Por ser
//     desabilitada, cliques e teclas continuam indo para a janela do raylib.
//   - Trocamos o "procedimento de janela" do pai (parentProcedure) para:
//       * redimensionar a filha junto com o pai (WM_SIZE);
//       * tratar F9 (captura de diagnostico) e F12 (screenshot).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objidl.h>
#include <algorithm>
namespace Gdiplus
{
    using std::max; // gdiplus.h expects the min/max macros NOMINMAX removes
    using std::min;
}
#include <gdiplus.h>

#include "pcsx2_runtime.h"

#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

namespace
{
    // s_parentProcedure : procedimento original do raylib (chamado no final de
    //                     parentProcedure para tudo que nao tratamos).
    // s_child           : a janela filha onde o PCSX2 desenha.
    // kTitleTimer       : id do timer que restaura o titulo apos uma mensagem.
    // s_title           : titulo original, guardado antes de mostrar a mensagem.
    WNDPROC s_parentProcedure = nullptr;
    HWND s_child = nullptr;
    constexpr UINT_PTR kTitleTimer = 0xF12;
    wchar_t s_title[256]{};

    // raylib's own F12 screenshot reads its OpenGL frame, which PCSX2's
    // window covers (it came out magenta). This one saves what is on screen:
    // screenshot-<date>-<time>.png in the working folder (data).
    // saveScreenshot: salva a imagem que esta na tela como PNG.
    //   1. Copia a area interna da janela com PrintWindow (inclui o conteudo
    //      Direct3D, por isso PW_RENDERFULLCONTENT).
    //   2. Converte para PNG com GDI+.
    //   3. Grava na pasta atual (que e "data"), com data e hora no nome.
    // Exemplo: F12 em 18/11/2026 17:00:00 -> data\screenshot-20261118-170000.png
    // Retorna false se a janela estiver minimizada (tamanho 0) ou se falhar.
    bool saveScreenshot(HWND window)
    {
        RECT client{};
        GetClientRect(window, &client);
        const int width = client.right - client.left, height = client.bottom - client.top;
        if (width <= 0 || height <= 0)
            return false;
        HDC screen = GetDC(window);
        HDC memory = CreateCompatibleDC(screen);
        HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
        HGDIOBJ previous = SelectObject(memory, bitmap);
        constexpr UINT kClientOnlyFullContent = 0x1 | 0x2; // PW_CLIENTONLY | PW_RENDERFULLCONTENT
        bool ok = PrintWindow(window, memory, kClientOnlyFullContent) != FALSE;
        SelectObject(memory, previous);
        if (ok)
        {
            ULONG_PTR token = 0;
            Gdiplus::GdiplusStartupInput input;
            ok = Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
            if (ok)
            {
                CLSID png{};
                UINT count = 0, bytes = 0;
                Gdiplus::GetImageEncodersSize(&count, &bytes);
                std::vector<unsigned char> buffer(bytes);
                auto *encoders = reinterpret_cast<Gdiplus::ImageCodecInfo *>(buffer.data());
                ok = bytes && Gdiplus::GetImageEncoders(count, bytes, encoders) == Gdiplus::Ok;
                bool found = false;
                for (UINT i = 0; ok && i < count; ++i)
                    if (std::wstring(encoders[i].MimeType) == L"image/png")
                    {
                        png = encoders[i].Clsid;
                        found = true;
                    }
                SYSTEMTIME now{};
                GetLocalTime(&now);
                wchar_t name[64];
                swprintf(name, 64, L"screenshot-%04u%02u%02u-%02u%02u%02u.png", now.wYear, now.wMonth, now.wDay, now.wHour,
                         now.wMinute, now.wSecond);
                {
                    Gdiplus::Bitmap image(bitmap, nullptr);
                    ok = found && image.Save(name, &png, nullptr) == Gdiplus::Ok;
                }
                Gdiplus::GdiplusShutdown(token);
            }
        }
        DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(window, screen);
        return ok;
    }

    // showInTitle: mostra uma mensagem curta no titulo da janela por 2,5 s
    // (ex.: "screenshot salvo na pasta data") e depois o timer kTitleTimer
    // devolve o titulo original.
    void showInTitle(HWND window, const wchar_t *text)
    {
        if (!s_title[0])
            GetWindowTextW(window, s_title, 256);
        SetWindowTextW(window, text);
        SetTimer(window, kTitleTimer, 2500, nullptr);
    }

    // Follows the game window's size.
    LRESULT CALLBACK parentProcedure(HWND window, UINT message, WPARAM w, LPARAM l)
    {
        if (message == WM_SIZE && s_child)
        {
            const unsigned width = LOWORD(l), height = HIWORD(l);
            if (width && height)
            {
                MoveWindow(s_child, 0, 0, static_cast<int>(width), static_cast<int>(height), FALSE);
                OrochiPcsx2Window::Resized(width, height);
            }
        }
        // F9 saves the current frame for diagnostics, and the title says so.
        if (message == WM_KEYDOWN && w == VK_F9 && (l & (1 << 30)) == 0 && OrochiPcsx2Window::CaptureKey())
            SetWindowTextW(window, L"Warriors Orochi | captura salva em logs");
        if ((message == WM_KEYDOWN || message == WM_KEYUP) && w == VK_F12)
        {
            if (message == WM_KEYDOWN && (l & (1 << 30)) == 0)
                showInTitle(window, saveScreenshot(window) ? L"Warriors Orochi | screenshot salvo na pasta data"
                                                           : L"Warriors Orochi | o screenshot falhou");
            return 0; // raylib does not see F12
        }
        if (message == WM_TIMER && w == kTitleTimer)
        {
            KillTimer(window, kTitleTimer);
            SetWindowTextW(window, s_title);
            return 0;
        }
        return CallWindowProcW(s_parentProcedure, window, message, w, l);
    }
}

// Create: cria a janela filha do PCSX2 dentro da janela do jogo.
//   - width/height (saida): tamanho atual da area interna do pai, em pixels.
//   - Liga WS_CLIPCHILDREN no pai para que o raylib nao pinte por cima da
//     imagem do Direct3D (isso causava piscadas).
//   - Instala parentProcedure no pai.
// Retorna o HWND da filha, ou nullptr se nao conseguiu criar.
void *OrochiPcsx2Window::Create(void *parentWindow, unsigned &width, unsigned &height)
{
    HWND parent = static_cast<HWND>(parentWindow);
    RECT client{};
    GetClientRect(parent, &client);
    width = static_cast<unsigned>(client.right - client.left);
    height = static_cast<unsigned>(client.bottom - client.top);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"OrochiPcsx2Gs";
    RegisterClassW(&windowClass);
    // The parent must not paint over the swap chain.
    SetWindowLongPtrW(parent, GWL_STYLE, GetWindowLongPtrW(parent, GWL_STYLE) | WS_CLIPCHILDREN);
    s_child = CreateWindowExW(0, windowClass.lpszClassName, L"", WS_CHILD | WS_VISIBLE | WS_DISABLED, 0, 0,
                              static_cast<int>(width), static_cast<int>(height), parent, nullptr,
                              windowClass.hInstance, nullptr);
    if (s_child)
        s_parentProcedure = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(parent, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(parentProcedure)));
    return s_child;
}

// ExecutableDirectory: pasta onde esta o WarriorsOrochi.exe.
// Exemplo: "C:\WO\Warriors Orochi". Usada para achar pcsx2-resources.
std::string OrochiPcsx2Window::ExecutableDirectory()
{
    char path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string directory(path, length);
    const size_t slash = directory.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : directory.substr(0, slash);
}
