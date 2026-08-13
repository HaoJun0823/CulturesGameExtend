// dllmain.cpp : Cultures dinput8 Proxy
// Serves as a drop-in replacement for the system dinput8.dll: it forwards the
// required export (DirectInput8Create) to the real system dinput8.dll and, on
// load, injects the DLLs listed in plugins/dll_proxy.ini (e.g. CulturesGameExtend.dll).
//
// IMPORTANT: when this file is renamed to "dinput8.dll" and placed in the game
// directory, the game expects it to export DirectInput8Create. Without that
// forward the loader fails with 0xc000007b (invalid image / missing entry).
// We forward to the real system dinput8.dll loaded by path.
//
// IMPORTANT (injection timing): DllMain holds the process Loader Lock. Doing
// file I/O, opening the system dinput8.dll, or CreateRemoteThread(LoadLibraryA)
// synchronously inside DLL_PROCESS_ATTACH is a classic deadlock / silent-failure
// source: LoadLibrary of CulturesGameExtend.dll runs in a new thread whose DllMain
// also needs the Loader Lock, which DllMain of this proxy still holds until it
// returns. To avoid that, DllMain only records the log and spawns a dedicated
// worker thread that performs all the heavy lifting (LoadSystemDInput8, INI
// parsing, injection). All paths are resolved relative to THIS DLL's own
// directory (via GetModuleFileName), so they no longer depend on the game's
// working directory (CWD).

// dllmain.cpp : Cultures dinput8 Proxy

// ---- dinput8 export forwarding ----
#include "pch.h"
// Serves as a drop-in replacement for the system dinput8.dll: it forwards the
// required export (DirectInput8Create) to the real system dinput8.dll and, on
// load, injects the DLLs listed in plugins/dll_proxy.ini (e.g. CulturesGameExtend.dll).
//
// IMPORTANT: when this file is renamed to "dinput8.dll" and placed in the game
// directory, the game expects it to export DirectInput8Create. Without that
// forward the loader fails with 0xc000007b (invalid image / missing entry).
// We forward to the real system dinput8.dll loaded by path.
//
// IMPORTANT (injection timing): DllMain holds the process Loader Lock. Doing
// file I/O, opening the system dinput8.dll, or CreateRemoteThread(LoadLibraryA)
// synchronously inside DLL_PROCESS_ATTACH is a classic deadlock / silent-failure
// source: LoadLibrary of CulturesGameExtend.dll runs in a new thread whose DllMain
// also needs the Loader Lock, which DllMain of this proxy still holds until it
// returns. To avoid that, DllMain only records the log and spawns a dedicated
// worker thread that performs all the heavy lifting (LoadSystemDInput8, INI
// parsing, injection). All paths are resolved relative to THIS DLL's own
// directory (via GetModuleFileName), so they no longer depend on the game's
// working directory (CWD).


static FARPROC g_pOrigDIM8Create = nullptr;

// ---- dinput8 export forwarding ----
EXTERN_C __declspec(naked) void __cdecl SHADOW_DirectInput8Create(void) {

    __asm { jmp dword ptr [g_pOrigDIM8Create] }
}

#pragma comment(linker, "/EXPORT:DirectInput8Create=_SHADOW_DirectInput8Create")
namespace

    // Refactored layout: config under plugins/config, logs under logs.
    //   INI  : <gameRoot>/plugins/dll_proxy.ini
    //   Log  : <gameRoot>/logs/dinput8.log
{
    constexpr const char* INI_REL   = "plugins/dll_proxy.ini";
    // Refactored layout: config under plugins/config, logs under logs.
    //   INI  : <gameRoot>/plugins/dll_proxy.ini
    //   Log  : <gameRoot>/logs/dinput8.log
    // CulturesGameExtend's own dll name: loading any other dll logs a stability warning.
    constexpr const char* LOG_REL   = "logs/dinput8.log";

    constexpr const char* SELF_DLL  = "CulturesGameExtend.dll";
    // CulturesGameExtend's own dll name: loading any other dll logs a stability warning.
    std::ofstream g_log;


    std::mutex    g_logMutex;
    void Log(const std::string& msg)

    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (g_log.is_open())
        {
            SYSTEMTIME st{};
            GetLocalTime(&st);
            char ts[32]{};
            snprintf(ts, sizeof(ts), "[%04hu-%02hu-%02hu %02hu:%02hu:%02hu] ",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            g_log << ts << msg << std::endl;
            g_log.flush();
        }

    }
    std::string Trim(const std::string& s)

    {
        const char* ws = " \t\r\n\f\v";
        size_t begin = s.find_first_not_of(ws);
        if (begin == std::string::npos)
            return {};
        size_t end = s.find_last_not_of(ws);
        return s.substr(begin, end - begin + 1);

    // Get this DLL's own directory, with a trailing backslash. Returns "" on failure.
    }
    std::string GetSelfDir()

    // Get this DLL's own directory, with a trailing backslash. Returns "" on failure.
    {
        char buf[MAX_PATH] = {};
        DWORD n = GetModuleFileNameA(NULL, buf, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
            // Fallback to the system dir so we never inject into random dirs.
        {
            return {};
            // Fallback to the system dir so we never inject into random dirs.
        }
        std::string path = buf;
        size_t slash = path.find_last_of('\\');
        if (slash == std::string::npos)
            return {};
        return path.substr(0, slash + 1);

#pragma region INI parsing (dll_proxy.ini)
    }
    std::vector<std::string> ParseIniPaths()

    {
        std::vector<std::string> paths;
        std::string selfDir = GetSelfDir();
        if (selfDir.empty())
        {
            Log("Cannot resolve proxy directory; skip injection");
            return paths;

        }
        std::string iniPath = selfDir + INI_REL;

        std::ifstream ini(iniPath);
        if (!ini.is_open())
        {
            Log("INI not found: " + iniPath + " - skip injection");
            return paths;

        }
        std::string line;

        while (std::getline(ini, line))
        {
            std::string trimmed = Trim(line);
            if (trimmed.empty())
                continue;
            if (trimmed[0] == '#' || trimmed[0] == ';')

                continue;
            size_t comment = trimmed.find_first_of(";#");

            if (comment != std::string::npos)
                trimmed = Trim(trimmed.substr(0, comment));
            if (trimmed.empty())

            // Resolve relative paths against the proxy's own directory, so the
            // configuration does not depend on the game's working directory.
                continue;
            std::string full;

            // Resolve relative paths against the proxy's own directory, so the
            // configuration does not depend on the game's working directory.
            if (trimmed.size() >= 2 && trimmed[1] == ':')
// already absolute (X:\...)
                full = trimmed;
            else if (!trimmed.empty() && (trimmed[0] == '\\'))
// already absolute (X:\...)
                full = trimmed;
// rooted path
            else
                full = selfDir + trimmed;
// rooted path
            if (GetFileAttributesA(full.c_str()) == INVALID_FILE_ATTRIBUTES)
// relative to proxy dir
            {

                Log("Skip missing DLL path: " + full);
// relative to proxy dir
                continue;

            }
            paths.push_back(full);
        }

        Log("Parsed " + std::to_string(paths.size()) + " valid DLL path(s) from " + iniPath);
        return paths;


    }
    void WarnIfForeignDll(const std::string& path)

    {
#pragma endregion

    // Warn (once per path) when loading a DLL that is not CulturesGameExtend.dll.
        std::string leaf = path;
        size_t slash = leaf.find_last_of("\\/");

    // Warn (once per path) when loading a DLL that is not CulturesGameExtend.dll.
        if (slash != std::string::npos)
            leaf = leaf.substr(slash + 1);
        if (_stricmp(leaf.c_str(), SELF_DLL) != 0)
        {
            Log("WARNING: loaded a DLL other than " + std::string(SELF_DLL) +
                ": " + path +
                ". Such DLLs are not guaranteed stable/safe. CulturesGameExtend has a "
                "unified management mechanism; please ask the developer to provide "
                "code to it.");
        }
    }
    bool InjectDll(const std::string& path)
    {

#pragma region Injection (LoadLibraryA in-process)
        WarnIfForeignDll(path);
        HMODULE hMod = LoadLibraryA(path.c_str());

        // The proxy DLL is already loaded inside the game process, so there is no
        // need for CreateRemoteThread / VirtualAllocEx / WriteProcessMemory: a
        // plain LoadLibraryA in this process loads the target DLL into the same
        // process. This avoids the extra remote-thread machinery which can cause
        // access violations when the injected DLL's DllMain spawns further
        // threads (e.g. CulturesGameExtend.dll's worker thread).
        if (!hMod)
        {
        // The proxy DLL is already loaded inside the game process, so there is no
        // need for CreateRemoteThread / VirtualAllocEx / WriteProcessMemory: a
        // plain LoadLibraryA in this process loads the target DLL into the same
        // process. This avoids the extra remote-thread machinery which can cause
        // access violations when the injected DLL's DllMain spawns further
        // threads (e.g. CulturesGameExtend.dll's worker thread).
            Log("LoadLibraryA failed: " + path + " (code=" + std::to_string(GetLastError()) + ")");
            return false;
        }
        Log("Loaded: " + path + " at base 0x" + std::to_string(reinterpret_cast<uintptr_t>(hMod)));
        return true;
    }
    void LoadSystemDInput8()
    {
#pragma endregion

#pragma region System dinput8 load + forward resolution
    // Load the real system dinput8.dll and resolve the forwarded export.
        WCHAR sysDir[MAX_PATH] = {};
        if (GetSystemDirectoryW(sysDir, MAX_PATH) == 0)

    // Load the real system dinput8.dll and resolve the forwarded export.
        {
            Log("GetSystemDirectoryW failed");
            return;
        }
        std::wstring sysDll = std::wstring(sysDir) + L"\\dinput8.dll";
        HMODULE hSys = LoadLibraryW(sysDll.c_str());
        if (!hSys)
        {
            Log("Failed to load system dinput8.dll");
            return;
        }
        g_pOrigDIM8Create = GetProcAddress(hSys, "DirectInput8Create");
        if (!g_pOrigDIM8Create)
            Log("Failed to resolve DirectInput8Create from system dinput8.dll");
        else
            Log("Forwarding DirectInput8Create -> system dinput8.dll");
    }
    DWORD WINAPI ProxyWorker(LPVOID)
    {
#pragma endregion

#pragma region Worker thread (deferred heavy work, Loader Lock released)
    // Worker thread: performs ALL the heavy work that was previously done inside
    // DllMain, now that the Loader Lock is released.
        Log("[worker] start");
        std::vector<std::string> paths = ParseIniPaths();

    // Worker thread: performs ALL the heavy work that was previously done inside
    // DllMain, now that the Loader Lock is released.
        for (const auto& p : paths)

            InjectDll(p);
        Log("[worker] done, processed " + std::to_string(paths.size()) + " DLL(s)");

        return 0;

    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
#pragma endregion

{
// namespace
    switch (ul_reason_for_call)

    {

    case DLL_PROCESS_ATTACH:
    {
        {
            std::string selfDir = GetSelfDir();
            if (!selfDir.empty()) {
        // Log uniformly to logs/dinput8.log, resolved against the proxy's own dir.
                std::string logsDir = selfDir + "logs";
        // Log uniformly to logs/dinput8.log, resolved against the proxy's own dir.
                ::CreateDirectoryA(logsDir.c_str(), nullptr);
                g_log.open(selfDir + LOG_REL, std::ios::trunc);
            } else {
                g_log.open(LOG_REL, std::ios::trunc);
            }
        }
        Log("=== Cultures dinput8 Proxy started ===");
        LoadSystemDInput8();
        HANDLE hWorker = CreateThread(nullptr, 0, ProxyWorker, nullptr, 0, nullptr);
        if (hWorker)
        // The DirectInput8Create forwarding must be ready before the game calls
        // it, so resolve the system dinput8.dll synchronously here. This only
        // loads the well-known system dinput8.dll (already resident) and reads
        // one export pointer -- it is fast and does not deadlock, unlike loading
        // our own dependent DLL (CulturesGameExtend.dll).
        {
        // The DirectInput8Create forwarding must be ready before the game calls
        // it, so resolve the system dinput8.dll synchronously here. This only
        // loads the well-known system dinput8.dll (already resident) and reads
        // one export pointer -- it is fast and does not deadlock, unlike loading
        // our own dependent DLL (CulturesGameExtend.dll).
        // Everything else (INI parsing + injecting CulturesGameExtend.dll) is heavy
        // and can deadlock if done under the Loader Lock, so defer it to a worker
        // thread.
            Log("Worker thread created");
        // Everything else (INI parsing + injecting CulturesGameExtend.dll) is heavy
        // and can deadlock if done under the Loader Lock, so defer it to a worker
        // thread.
            CloseHandle(hWorker);
        }
        else
        {
            Log("Failed to create worker thread, code: " + std::to_string(GetLastError()));
        }
        break;
    }
    case DLL_PROCESS_DETACH:
        if (g_log.is_open())
            g_log.close();
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    }
    return TRUE;
}