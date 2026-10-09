// Adapted from the Dishonored VR launcher; see NOTICE.md for provenance and license.
// tools/installer/sys/process.cpp - see process.h.
#include "sys/process.h"
#include "sys/fs.h"
#include <tlhelp32.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shldisp.h>
#include <exdisp.h>
#include <string.h>
#include <stdio.h>
#include <servprov.h>

#ifdef ShellExecute
#undef ShellExecute   // windows.h maps it to ShellExecuteW, which breaks the COM method name below
#endif

namespace bvr::launcher::process {

bool write_shortcut(const std::wstring& linkPath, const std::wstring& target,
                    const std::wstring& args, DWORD* err)
{
    IShellLinkW* link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (SUCCEEDED(hr)) hr = link->SetPath(target.c_str());
    if (SUCCEEDED(hr)) hr = link->SetArguments(args.c_str());
    if (SUCCEEDED(hr)) hr = link->SetWorkingDirectory(fs::parent(target).c_str());
    if (SUCCEEDED(hr)) hr = link->SetDescription(L"Configure and launch BioShock Remastered VR");
    if (SUCCEEDED(hr)) hr = link->SetIconLocation(target.c_str(), 0);
    IPersistFile* file = nullptr;
    if (SUCCEEDED(hr)) hr = link->QueryInterface(IID_PPV_ARGS(&file));
    if (SUCCEEDED(hr)) hr = file->Save(linkPath.c_str(), TRUE);
    if (file) file->Release();
    if (link) link->Release();
    if (FAILED(hr) && err) *err = HRESULT_FACILITY(hr) == FACILITY_WIN32 ? HRESULT_CODE(hr) : ERROR_GEN_FAILURE;
    return SUCCEEDED(hr);
}

Running is_running(const wchar_t* exeName)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return Running::Unknown;
    PROCESSENTRY32W pe = {}; pe.dwSize = sizeof(pe);
    Running r = Running::No;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) == 0) { r = Running::Yes; break; }
        } while (Process32NextW(snap, &pe));
    } else {
        r = Running::Unknown;
    }
    CloseHandle(snap);
    return r;
}

bool is_elevated()
{
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION te = {}; DWORD n = 0;
    const bool ok = GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &n) && te.TokenIsElevated;
    CloseHandle(tok);
    return ok;
}

std::wstring quote_arg(const std::wstring& a)
{
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring r = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++bs; continue; }
        if (c == L'"') { r.append(bs * 2 + 1, L'\\'); r += c; bs = 0; continue; }
        r.append(bs, L'\\'); bs = 0; r += c;
    }
    r.append(bs * 2, L'\\');
    return r + L"\"";
}

bool run_self_elevated_wait(const std::wstring& args, DWORD* exitCode, DWORD* err)
{
    const std::wstring exe = fs::module_path();
    SHELLEXECUTEINFOW sei = {}; sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) { if (err) *err = GetLastError(); return false; }
    if (!sei.hProcess) { if (err) *err = ERROR_INVALID_HANDLE; return false; }
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    if (exitCode) *exitCode = code;
    return true;
}

bool run_wait(const std::wstring& cmdline, DWORD* exitCode, DWORD* err, DWORD timeoutMs, const std::wstring& stdoutFile)
{
    std::wstring cmd = cmdline;   // CreateProcess may write into it
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    HANDLE out = INVALID_HANDLE_VALUE;
    if (!stdoutFile.empty()) {
        SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
        out = CreateFileW(stdoutFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out == INVALID_HANDLE_VALUE) { if (err) *err = GetLastError(); return false; }
        si.dwFlags |= STARTF_USESTDHANDLES; si.hStdOutput = out; si.hStdError = out; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    PROCESS_INFORMATION pi = {};
    const BOOL made = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, out != INVALID_HANDLE_VALUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (!made) { if (err) *err = GetLastError(); return false; }
    CloseHandle(pi.hThread);
    const DWORD w = WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD code = 1;
    if (w == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else { TerminateProcess(pi.hProcess, 1); code = WAIT_TIMEOUT; }
    CloseHandle(pi.hProcess);
    if (exitCode) *exitCode = code;
    return true;
}

bool open(const std::wstring& target)
{
    return (INT_PTR)ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32;
}

// The documented de-elevation route: the desktop's shell view hands out an
// IShellDispatch2 living in explorer.exe (medium integrity), and its
// ShellExecute runs the target with that token, not ours.
bool open_unelevated(const std::wstring& target, const std::wstring& args, const std::wstring& directory)
{
    if (!is_elevated()) return (INT_PTR)ShellExecuteW(nullptr, L"open", target.c_str(), args.empty()?nullptr:args.c_str(), directory.empty()?nullptr:directory.c_str(), SW_SHOWNORMAL)>32;
    bool ok = false;
    IShellWindows* windows = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows)))) return false;
    VARIANT vEmpty; VariantInit(&vEmpty);
    VARIANT vLoc; vLoc.vt = VT_I4; vLoc.lVal = CSIDL_DESKTOP;
    long hwnd = 0;
    IDispatch* disp = nullptr;
    if (SUCCEEDED(windows->FindWindowSW(&vLoc, &vEmpty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &disp)) && disp) {
        IServiceProvider* sp = nullptr;
        if (SUCCEEDED(disp->QueryInterface(IID_PPV_ARGS(&sp)))) {
            IShellBrowser* browser = nullptr;
            if (SUCCEEDED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser)))) {
                IShellView* view = nullptr;
                if (SUCCEEDED(browser->QueryActiveShellView(&view))) {
                    IDispatch* viewDisp = nullptr;
                    if (SUCCEEDED(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&viewDisp)))) {
                        IShellFolderViewDual* folderView = nullptr;
                        if (SUCCEEDED(viewDisp->QueryInterface(IID_PPV_ARGS(&folderView)))) {
                            IDispatch* app = nullptr;
                            if (SUCCEEDED(folderView->get_Application(&app))) {
                                IShellDispatch2* shell = nullptr;
                                if (SUCCEEDED(app->QueryInterface(IID_PPV_ARGS(&shell)))) {
                                    BSTR file = SysAllocString(target.c_str());
                                    VARIANT vShow; vShow.vt = VT_I4; vShow.lVal = SW_SHOWNORMAL;
                                    VARIANT params, dir; VariantInit(&params); VariantInit(&dir);
                                    params.vt=VT_BSTR; params.bstrVal=SysAllocString(args.c_str());
                                    dir.vt=VT_BSTR; dir.bstrVal=SysAllocString(directory.c_str());
                                    ok = SUCCEEDED(shell->ShellExecute(file, params, dir, vEmpty, vShow));
                                    VariantClear(&params); VariantClear(&dir);
                                    SysFreeString(file);
                                    shell->Release();
                                }
                                app->Release();
                            }
                            folderView->Release();
                        }
                        viewDisp->Release();
                    }
                    view->Release();
                }
                browser->Release();
            }
            sp->Release();
        }
        disp->Release();
    }
    windows->Release();
    return ok;
}

bool attach_parent_console()
{
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return false;
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    return true;
}

} // namespace bvr::launcher::process
