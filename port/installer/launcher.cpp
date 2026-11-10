// LEGO Dimensions.exe: starts ldport.exe from an installed layout without a console window.
//
//   <install>\ldport.exe, SDL3.dll, imports.tsv, fsr1\
//   <install>\data          (settings, saves, Toy Pad figures, shader cache, ldport.log)
//   and the game data, either copied:
//     <install>\game\eboot.elf, game\update (1.24 patch), game\base (1.00), game\addcont (add-on packs, optional)
//   or left where it is, named by <install>\game.txt (one line: an "extracted" folder holding
//     elf\eboot.elf, CUSA01176\uroot, CUSA01176-base\uroot, addcont).
//
// The log goes to data\ldport.log (the previous run's to ldport.prev.log). The LAN Toy Pad page follows its setting
// (Options > Gameplay); any extra arguments are passed through to ldport.exe.
//
// "LEGO Dimensions.exe" --create-shortcuts [--no-desktop] [--quiet] makes the Start Menu folder and desktop shortcut
// again (the same ones the installer makes, for this user), e.g. after they were deleted or the folder was moved.
//
// "LEGO Dimensions.exe" --add-dlc asks for a folder of DLC (add-on pack) .pkg files and unpacks them into the game.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <iterator>
#include <string>

static std::wstring Dir(const std::wstring& path) { return path.substr(0, path.find_last_of(L"\\/")); }
static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static int Fail(const std::wstring& msg) {
    MessageBoxW(nullptr, msg.c_str(), L"LEGO Dimensions", MB_ICONERROR | MB_OK);
    return 1;
}
static std::wstring Quote(const std::wstring& s) { return L"\"" + s + L"\""; }

// The first line of a UTF-8 text file, trimmed; empty when there is none.
static std::wstring ReadLine(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    char buf[4096] = {};
    DWORD n = 0;
    ReadFile(f, buf, sizeof buf - 1, &n, nullptr);
    CloseHandle(f);
    std::string line(buf, n);
    if (line.size() >= 3 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);  // BOM
    if (const size_t eol = line.find_first_of("\r\n"); eol != std::string::npos) line.resize(eol);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\\' || line.back() == '/')) line.pop_back();
    std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, line.data(), int(line.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, line.data(), int(line.size()), w.data(), int(w.size()));
    return w;
}

static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}

static bool MakeLink(const std::wstring& lnk, const std::wstring& target, const std::wstring& dir, const std::wstring& icon) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return false;
    link->SetPath(target.c_str());
    if (!dir.empty()) link->SetWorkingDirectory(dir.c_str());
    if (!icon.empty()) link->SetIconLocation(icon.c_str(), 0);
    IPersistFile* file = nullptr;
    bool ok = SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)));
    if (ok) {
        ok = SUCCEEDED(file->Save(lnk.c_str(), TRUE));
        file->Release();
    }
    link->Release();
    return ok;
}

// --create-shortcuts: the installer's Start Menu entries and desktop shortcut, for the current user.
static int CreateShortcuts(const std::wstring& root, const std::wstring& self, bool desktop, bool quiet) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const std::wstring programs = KnownFolder(FOLDERID_Programs), desk = KnownFolder(FOLDERID_Desktop);
    const std::wstring group = programs + L"\\LEGO Dimensions";
    CreateDirectoryW(group.c_str(), nullptr);
    std::wstring made, failed;
    auto link = [&](const std::wstring& lnk, const std::wstring& target, const std::wstring& dir, const std::wstring& icon) {
        (MakeLink(lnk, target, dir, icon) ? made : failed) += L"\n  " + lnk;
    };
    link(group + L"\\LEGO Dimensions.lnk", self, root, self);
    link(group + L"\\Game folder (saves and log).lnk", root + L"\\data", L"", L"");
    if (Exists(root + L"\\LegoToypad\\LegoToypad.exe"))
        link(group + L"\\LegoToypad.lnk", root + L"\\LegoToypad\\LegoToypad.exe", root + L"\\LegoToypad", L"");
    {
        const std::wstring lnk = group + L"\\Add DLC packs.lnk";
        IShellLinkW* l = nullptr;  // the launcher with --add-dlc
        bool ok = SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&l)));
        if (ok) {
            l->SetPath(self.c_str());
            l->SetArguments(L"--add-dlc");
            l->SetWorkingDirectory(root.c_str());
            l->SetIconLocation(self.c_str(), 0);
            IPersistFile* f = nullptr;
            ok = SUCCEEDED(l->QueryInterface(IID_PPV_ARGS(&f))) && SUCCEEDED(f->Save(lnk.c_str(), TRUE));
            if (f) f->Release();
            l->Release();
        }
        (ok ? made : failed) += L"\n  " + lnk;
    }
    if (Exists(root + L"\\unins000.exe"))
        link(group + L"\\Uninstall LEGO Dimensions.lnk", root + L"\\unins000.exe", root, L"");
    {
        const std::wstring url = group + L"\\Virtual Toy Pad (while the game runs).url";
        HANDLE f = CreateFileW(url.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        const char text[] = "[InternetShortcut]\r\nURL=http://127.0.0.1:8642/\r\n";
        DWORD n = 0;
        const bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, text, DWORD(sizeof text - 1), &n, nullptr);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        (ok ? made : failed) += L"\n  " + url;
    }
    if (desktop) link(desk + L"\\LEGO Dimensions.lnk", self, root, self);
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW, group.c_str(), nullptr);
    CoUninitialize();
    if (!failed.empty()) return Fail(L"These shortcuts could not be made:" + failed);
    if (!quiet) MessageBoxW(nullptr, (L"Shortcuts made:" + made).c_str(), L"LEGO Dimensions", MB_ICONINFORMATION | MB_OK);
    return 0;
}

// A folder, or a file, picked in the standard Windows dialog; empty when cancelled.
static std::wstring Pick(const wchar_t* title, bool folder) {
    IFileOpenDialog* dlg = nullptr;
    std::wstring r;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return r;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    dlg->SetTitle(title);
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->Show(nullptr)) && SUCCEEDED(dlg->GetResult(&item))) {
        PWSTR p = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) r = p;
        CoTaskMemFree(p);
        item->Release();
    }
    dlg->Release();
    return r;
}

// Runs a command line in its own console window (for ldpkg's progress) and returns its exit code, or -1.
static int RunConsole(const std::wstring& exe, std::wstring cmd, const std::wstring& dir) {
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, dir.c_str(), &si, &pi))
        return -1;
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return int(code);
}

// --add-dlc: unpacks the add-on pack .pkg files in a folder into the installed game (game\addcont, or the addcont of
// the game data folder named by game.txt) with ldpkg. It uses the package keys setup kept as data\pkgkeys.bin, and
// asks for a keys file (and keeps it there) when there are none.
static int AddDlc(const std::wstring& root) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const std::wstring ldpkg = root + L"\\ldpkg.exe", data = root + L"\\data", keys = data + L"\\pkgkeys.bin";
    if (!Exists(ldpkg)) return Fail(L"ldpkg.exe is missing from\n" + root);
    std::wstring game = root + L"\\game";
    if (const std::wstring dir = ReadLine(root + L"\\game.txt"); !dir.empty()) game = dir;
    CreateDirectoryW(data.c_str(), nullptr);
    if (!Exists(keys)) {
        MessageBoxW(nullptr,
                    L"Adding DLC needs the package keys file (the two fake-package RSA keys). Pick it in the next "
                    L"window; it is kept in the game's data folder for next time.",
                    L"Add DLC packs", MB_ICONINFORMATION | MB_OK);
        const std::wstring k = Pick(L"Package keys file", false);
        if (k.empty()) return 1;
        if (RunConsole(ldpkg, Quote(ldpkg) + L" --keys " + Quote(k), root) != 0)
            return Fail(L"Those keys don't work:\n" + k);
        if (!CopyFileW(k.c_str(), keys.c_str(), FALSE)) return Fail(L"Could not copy the keys to\n" + keys);
    }
    const std::wstring src = Pick(L"Folder with the DLC (add-on pack) .pkg files", true);
    if (src.empty()) return 1;
    const std::wstring log = data + L"\\ldpkg-dlc.log";
    const int code = RunConsole(ldpkg,
                                Quote(ldpkg) + L" --dlc --keys " + Quote(keys) + L" --log " + Quote(log) + L" -o " +
                                    Quote(game) + L" " + Quote(src),
                                root);
    CoUninitialize();
    if (code != 0)
        return Fail(L"No DLC was added (ldpkg code " + std::to_wstring(code) + L"). Details are in\n" + log +
                    L"\n\nThe folder needs add-on pack .pkg files for LEGO Dimensions (CUSA01176), fake-signed.");
    MessageBoxW(nullptr, (L"The DLC is added to\n" + game + L"\\addcont\n\nThe game picks it up the next time it starts.").c_str(),
                L"Add DLC packs", MB_ICONINFORMATION | MB_OK);
    return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR extra, int) {
    wchar_t self[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, self, DWORD(std::size(self)));
    const std::wstring root = Dir(self);
    {
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        bool shortcuts = false, desktop = true, quiet = false, add_dlc = false;
        for (int i = 1; argv && i < argc; i++) {
            const std::wstring a = argv[i];
            if (a == L"--create-shortcuts") shortcuts = true;
            else if (a == L"--add-dlc") add_dlc = true;
            else if (a == L"--no-desktop") desktop = false;
            else if (a == L"--quiet") quiet = true;
        }
        LocalFree(argv);
        if (add_dlc) return AddDlc(root);
        if (shortcuts) return CreateShortcuts(root, self, desktop, quiet);
    }
    const std::wstring exe = root + L"\\ldport.exe", data = root + L"\\data";
    if (!Exists(exe)) return Fail(L"ldport.exe is missing from\n" + root);

    std::wstring where = root + L"\\game";
    std::wstring eboot = where + L"\\eboot.elf", update = where + L"\\update", base = where + L"\\base",
                 addcont = where + L"\\addcont";
    if (const std::wstring dir = ReadLine(root + L"\\game.txt"); !dir.empty()) {
        where = dir;
        eboot = dir + L"\\elf\\eboot.elf";
        update = dir + L"\\CUSA01176\\uroot";
        base = dir + L"\\CUSA01176-base\\uroot";
        addcont = dir + L"\\addcont";
    }
    if (!Exists(eboot) || !Exists(update))
        return Fail(L"The game data is missing from\n" + where +
                    L"\n\nRun the installer again and point it at the game data folder.");
    CreateDirectoryW(data.c_str(), nullptr);

    std::wstring cmd = Quote(exe) + L" " + Quote(eboot) + L" --run --names " + Quote(root + L"\\imports.tsv") +
                       L" --app0 " + Quote(update);
    if (Exists(base)) cmd += L" --app0 " + Quote(base);
    cmd += L" --data " + Quote(data);
    if (Exists(addcont)) cmd += L" --addcont " + Quote(addcont);
    if (extra && *extra) cmd += L" " + std::wstring(extra);

    const std::wstring log = data + L"\\ldport.log", prev = data + L"\\ldport.prev.log";
    MoveFileExW(log.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE out = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE in = CreateFileW(L"NUL", GENERIC_READ, 0, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in;
    si.hStdOutput = out;
    si.hStdError = out;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, root.c_str(), &si, &pi))
        return Fail(L"Could not start ldport.exe (error " + std::to_wstring(GetLastError()) + L").");
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);  // so Steam and the like see the game as running
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    return int(code);
}
