/* vglobby — a headless DirectPlay lobby launcher.
 *
 * WHY THIS EXISTS
 * DirectPlay-era games are usually launched by a *lobby*: the launcher fills in a DPLCONNECTION (service
 * provider, session name, player name, host-or-join, host address) and calls IDirectPlayLobby::RunApplication.
 * DirectPlay then starts the game, and the game asks DirectPlay for those settings on startup. None of it
 * travels on the command line, which is why a lobby-aware game cannot be pointed at a session with argv.
 *
 * Wipeout XL is the worked example. NET-WOXL.EXE imports DirectPlayLobbyCreateA and, in WinMain, asks whether
 * it was lobby-launched; if it was it goes straight into the session, and if it was not it falls back to a
 * local dialog that only ever offers Single Player and IPX. Its shipped launcher, WOLOBBY.EXE, imports no
 * CreateProcess at all -- it does everything through RunApplication.
 *
 * So this is WOLOBBY with the dialog removed: the same DirectPlay calls, driven entirely by arguments, so a
 * launcher (VidyaGod) can put a player straight into a friend's session with nothing typed.
 *
 * It also REGISTERS the application itself. RunApplication resolves the game through
 * HKLM\SOFTWARE\Microsoft\DirectPlay\Applications\<name> (Guid/File/Path/CurrentDirectory); the shipped
 * launchers write that key at runtime, so in a fresh prefix it does not exist yet. Registering here keeps the
 * tool self-contained -- no package registry layer required, and it works in any prefix.
 *
 * Deliberately 32-bit: dplayx is a 32-bit component and the games are 32-bit.
 *
 * Usage:
 *   vglobby.exe --app NAME --guid {GUID} --exe FILE.EXE [--dir DIR]
 *               --player NAME [--session NAME]
 *               [--address=ADDRESS | --host | --join [ADDRESS]]
 *               [--sp tcpip|ipx|serial|modem] [--port N] [--max N] [--wait]
 *
 * Hosting is the default; supplying a non-empty address is what selects joining. Prefer --address=VALUE from a
 * launcher: it is one token, so an empty value collapses to "host" instead of leaving a dangling flag.
 *
 * --dir defaults to the directory vglobby.exe itself lives in, which is normally the game directory.
 */

#define INITGUID
#define COBJMACROS
#include <windows.h>
#include <dplay.h>
#include <dplobby.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>

static void Fail(const char *Msg, HRESULT Hr)
{
    if (Hr)
        fprintf(stderr, "vglobby: %s (hr=0x%08lx)\n", Msg, (unsigned long)Hr);
    else
        fprintf(stderr, "vglobby: %s\n", Msg);
    exit(1);
}

/* RunApplication finds the game through the registry, not through us handing it a path. The shipped launchers
 * write this key at runtime; do the same so a fresh prefix works. A 32-bit process writing HKLM\SOFTWARE is
 * redirected into Wow6432Node, which is exactly where DirectPlay looks. */
static void RegisterApplication(const char *AppName, const char *GuidText,
                                const char *ExeFile, const char *Dir)
{
    char  KeyPath[512];
    HKEY  Key;
    LONG  Rc;

    snprintf(KeyPath, sizeof(KeyPath),
              "SOFTWARE\\Microsoft\\DirectPlay\\Applications\\%s", AppName);
    KeyPath[sizeof(KeyPath) - 1] = 0;

    Rc = RegCreateKeyExA(HKEY_LOCAL_MACHINE, KeyPath, 0, NULL, 0, KEY_WRITE, NULL, &Key, NULL);
    if (Rc != ERROR_SUCCESS)
        Fail("cannot create the DirectPlay application key", (HRESULT)Rc);

    RegSetValueExA(Key, "Guid", 0, REG_SZ, (const BYTE *)GuidText, (DWORD)strlen(GuidText) + 1);
    RegSetValueExA(Key, "File", 0, REG_SZ, (const BYTE *)ExeFile, (DWORD)strlen(ExeFile) + 1);
    RegSetValueExA(Key, "Path", 0, REG_SZ, (const BYTE *)Dir, (DWORD)strlen(Dir) + 1);
    RegSetValueExA(Key, "CurrentDirectory", 0, REG_SZ, (const BYTE *)Dir, (DWORD)strlen(Dir) + 1);
    RegSetValueExA(Key, "CommandLine", 0, REG_SZ, (const BYTE *)"", 1);
    RegCloseKey(Key);
}

/* Is a process with this exe name running? */
static int GameIsRunning(const char *ExeFile)
{
    HANDLE          Snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32  Pe;
    int             Found = 0;

    if (Snap == INVALID_HANDLE_VALUE)
        return 0;
    Pe.dwSize = sizeof(Pe);
    if (Process32First(Snap, &Pe))
        do {
            if (_stricmp(Pe.szExeFile, ExeFile) == 0) { Found = 1; break; }
        } while (Process32Next(Snap, &Pe));
    CloseHandle(Snap);
    return Found;
}

/* Block until the game exits.
 *
 * This matters for a launcher like VidyaGod: RunApplication starts the game as OUR child, so if we returned
 * straight away the launcher would see its process tree end and tear the prefix down while the game was still
 * starting. DirectPlay hands back an "application id" which is the process id on implementations that bother,
 * so try that first -- and fall back to watching for the executable by name, because the id is not guaranteed
 * and a wrong PID must not silently degrade into "exit immediately". */
static void WaitForGame(DWORD AppId, const char *ExeFile)
{
    HANDLE Proc = AppId ? OpenProcess(SYNCHRONIZE, FALSE, AppId) : NULL;
    int    i;

    if (Proc)
    {
        WaitForSingleObject(Proc, INFINITE);
        CloseHandle(Proc);
        return;
    }
    /* Give the game time to appear before concluding it has already finished. */
    for (i = 0; i < 120 && !GameIsRunning(ExeFile); i++)
        Sleep(500);
    while (GameIsRunning(ExeFile))
        Sleep(500);
}

/* --- join pre-check ------------------------------------------------------------------------------------- */

/* EnumSessions callback: records that at least one session answered. */
static BOOL FAR PASCAL CountSession(LPCDPSESSIONDESC2 Desc, LPDWORD Timeout, DWORD Flags, LPVOID Ctx)
{
    (void)Desc; (void)Timeout;
    if (Flags & DPESC_TIMEDOUT)
        return FALSE;
    *(int *)Ctx = 1;
    return FALSE;   /* one is enough */
}

/* Verify the host's session actually EXISTS before handing the game a JOINSESSION connection.
 *
 * Field lesson (2026-09-01, the first real two-machine test): when the joiner was launched while the host was
 * still walking its menus, NET-WOXL took the DPLCONNECTION, found no session behind the address, and died with
 * an invisible NULL-deref ~40 seconds in -- no window, no message, nothing in any log. The failure belongs to
 * the LAUNCHER: enumerate first, wait for the host to become ready (people are slow; menus are slow), and if
 * nothing ever answers, say so in a way a human sees. MessageBox, not stderr: under wine a GUI-subsystem exe's
 * stderr reaches nobody. */
static void RequireSession(HMODULE Dll, const GUID *AppGuid, LPVOID Addr, DWORD AddrSize, const char *JoinAddr)
{
    HRESULT (WINAPI *pCreate)(LPGUID, LPDIRECTPLAY *, IUnknown *);
    LPDIRECTPLAY   Dp1 = NULL;
    LPDIRECTPLAY4A Dp  = NULL;
    DPSESSIONDESC2 Want;
    HRESULT        Hr;
    int            Try, Found = 0;
    char           Msg[512];

    pCreate = (HRESULT (WINAPI *)(LPGUID, LPDIRECTPLAY *, IUnknown *))
              (void *)GetProcAddress(Dll, "DirectPlayCreate");
    if (!pCreate)
        return;                                   /* can't pre-check -- fall through to the old behavior */
    Hr = pCreate(NULL, &Dp1, NULL);
    if (FAILED(Hr) || !Dp1)
        return;
    Hr = IDirectPlay_QueryInterface(Dp1, &IID_IDirectPlay4A, (void **)&Dp);
    IDirectPlay_Release(Dp1);
    if (FAILED(Hr) || !Dp)
        return;
    Hr = IDirectPlayX_InitializeConnection(Dp, Addr, 0);
    if (FAILED(Hr))
    {
        IDirectPlayX_Release(Dp);
        return;
    }

    ZeroMemory(&Want, sizeof(Want));
    Want.dwSize          = sizeof(Want);
    Want.guidApplication = *AppGuid;

    /* ~90s window: 30 tries x 3s enumeration timeout. Prints per try so a captured log shows the wait. */
    for (Try = 0; Try < 30 && !Found; Try++)
    {
        Hr = IDirectPlayX_EnumSessions(Dp, &Want, 3000, CountSession, &Found, DPENUMSESSIONS_AVAILABLE);
        if (FAILED(Hr) && Hr != DPERR_TIMEOUT && Hr != DPERR_NOSESSIONS && Hr != DPERR_USERCANCEL)
            fprintf(stderr, "vglobby: EnumSessions failed (hr=0x%08lx), retrying\n", (unsigned long)Hr);
        if (!Found)
        {
            fprintf(stderr, "vglobby: no session at %s yet (try %d/30) -- is the host in its multiplayer lobby?\n",
                    JoinAddr, Try + 1);
            Sleep(1000);
        }
    }
    IDirectPlayX_Release(Dp);

    if (!Found)
    {
        snprintf(Msg, sizeof(Msg),
                 "No game session found at %s after 90 seconds.\n\n"
                 "The HOST must be inside the game's MULTIPLAYER LOBBY screen\n"
                 "(past race customization) before you join.\n\n"
                 "Start or ready the host, then launch Join again.",
                 JoinAddr);
        MessageBoxA(NULL, Msg, "vglobby -- host not ready", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        Fail("no session found at the join address -- host not ready", 0);
    }
    fprintf(stderr, "vglobby: session found at %s -- joining\n", JoinAddr);
}

static int ParseGuid(const char *Text, GUID *Out)
{
    unsigned long D1;
    unsigned int  D2, D3, B[8];
    int           n;

    n = sscanf(Text, " {%8lx-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}",
               &D1, &D2, &D3, &B[0], &B[1], &B[2], &B[3], &B[4], &B[5], &B[6], &B[7]);
    if (n != 11)
        n = sscanf(Text, " %8lx-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x",
                   &D1, &D2, &D3, &B[0], &B[1], &B[2], &B[3], &B[4], &B[5], &B[6], &B[7]);
    if (n != 11)
        return 0;
    Out->Data1 = (unsigned long)D1;
    Out->Data2 = (unsigned short)D2;
    Out->Data3 = (unsigned short)D3;
    for (n = 0; n < 8; n++)
        Out->Data4[n] = (unsigned char)B[n];
    return 1;
}

int main(int argc, char **argv)
{
    const char *AppName  = NULL, *GuidText = NULL, *ExeFile = NULL, *Dir = NULL;
    const char *Player   = NULL, *Session  = NULL, *JoinAddr = NULL;
    const char *SpName   = "tcpip";
    int         Hosting  = 1, Wait = 0, i;   /* host unless an address says otherwise */
    DWORD       MaxPlayers = 0, Port = 0;

    HRESULT                     Hr;
    HMODULE                     Dll;
    HRESULT (WINAPI *pLobbyCreate)(LPGUID, LPDIRECTPLAYLOBBYA *, IUnknown *, LPVOID, DWORD);
    LPDIRECTPLAYLOBBYA          Lobby  = NULL;
    LPDIRECTPLAYLOBBY2A         Lobby2 = NULL;
    GUID                        AppGuid, SpGuid;
    DPCOMPOUNDADDRESSELEMENT    Elems[3];
    DWORD                       ElemCount = 0, AddrSize = 0, AppId = 0;
    LPVOID                      Addr = NULL;
    DPSESSIONDESC2              Desc;
    DPNAME                      Name;
    DPLCONNECTION               Conn;
    char                        DirBuf[MAX_PATH];

    for (i = 1; i < argc; i++)
    {
        const char *a = argv[i];
        #define NEXT() (i + 1 < argc ? argv[++i] : NULL)
        /* --address=VALUE is the form a launcher should generate: it is ONE token, so when the value substitutes
         * to empty the whole argument is dropped and we fall back to hosting. A "--address VALUE" pair cannot do
         * that -- the flag would survive alone and swallow the next argument as its value. An empty value is
         * explicitly allowed and means "no address", i.e. host. */
        if (!strncmp(a, "--address=", 10))
        {
            JoinAddr = a + 10;
            if (*JoinAddr) Hosting = 0;
            continue;
        }
        if      (!strcmp(a, "--address")) { JoinAddr = NEXT(); if (JoinAddr && *JoinAddr) Hosting = 0; }
        else if (!strcmp(a, "--app"))     AppName  = NEXT();
        else if (!strcmp(a, "--guid"))    GuidText = NEXT();
        else if (!strcmp(a, "--exe"))     ExeFile  = NEXT();
        else if (!strcmp(a, "--dir"))     Dir      = NEXT();
        else if (!strcmp(a, "--player"))  Player   = NEXT();
        else if (!strcmp(a, "--session")) Session  = NEXT();
        else if (!strcmp(a, "--sp"))      SpName   = NEXT();
        else if (!strcmp(a, "--max"))     MaxPlayers = (DWORD)atoi(NEXT() ? argv[i] : "0");
        else if (!strcmp(a, "--port"))    Port     = (DWORD)atoi(NEXT() ? argv[i] : "0");
        else if (!strcmp(a, "--host"))    Hosting  = 1;
        else if (!strcmp(a, "--wait"))    Wait     = 1;
        else if (!strcmp(a, "--join"))
        {
            Hosting = 0;
            /* --join may be bare (pick from the session list) or carry an address. */
            if (i + 1 < argc && strncmp(argv[i + 1], "--", 2) != 0)
                JoinAddr = argv[++i];
        }
        else
        {
            fprintf(stderr, "vglobby: unknown argument '%s'\n", a);
            return 2;
        }
        #undef NEXT
    }

    if (!AppName || !GuidText || !ExeFile || !Player)
    {
        fprintf(stderr,
            "usage: vglobby --app NAME --guid {GUID} --exe FILE.EXE [--dir DIR]\n"
            "               --player NAME [--session NAME] [--address=ADDR | --host | --join [ADDR]]\n"
            "               [--sp tcpip|ipx|serial|modem] [--port N] [--max N] [--wait]\n");
        return 2;
    }
    if (!ParseGuid(GuidText, &AppGuid))
        Fail("--guid is not a GUID", 0);

    if      (!strcmp(SpName, "tcpip"))  SpGuid = DPSPGUID_TCPIP;
    else if (!strcmp(SpName, "ipx"))    SpGuid = DPSPGUID_IPX;
    else if (!strcmp(SpName, "serial")) SpGuid = DPSPGUID_SERIAL;
    else if (!strcmp(SpName, "modem"))  SpGuid = DPSPGUID_MODEM;
    else Fail("--sp must be tcpip, ipx, serial or modem", 0);

    if (!Dir)
    {
        char *Slash;
        if (!GetModuleFileNameA(NULL, DirBuf, sizeof(DirBuf)))
            Fail("cannot determine my own directory", 0);
        Slash = strrchr(DirBuf, '\\');
        if (Slash) *Slash = 0;
        Dir = DirBuf;
    }
    RegisterApplication(AppName, GuidText, ExeFile, Dir);

    Dll = LoadLibraryA("dplayx.dll");
    if (!Dll)
        Fail("cannot load dplayx.dll", 0);
    /* Resolved by name rather than linked, so no DirectX import library is needed to build this. */
    /* via void*: the direct FARPROC->typed cast trips -Wcast-function-type, and this is the usual idiom. */
    pLobbyCreate = (HRESULT (WINAPI *)(LPGUID, LPDIRECTPLAYLOBBYA *, IUnknown *, LPVOID, DWORD))
                   (void *)GetProcAddress(Dll, "DirectPlayLobbyCreateA");
    if (!pLobbyCreate)
        Fail("dplayx.dll has no DirectPlayLobbyCreateA", 0);

    Hr = pLobbyCreate(NULL, &Lobby, NULL, NULL, 0);
    if (FAILED(Hr))
        Fail("DirectPlayLobbyCreateA failed", Hr);

    /* CreateCompoundAddress lives on IDirectPlayLobby2. */
    Hr = IDirectPlayLobby_QueryInterface(Lobby, &IID_IDirectPlayLobby2A, (void **)&Lobby2);
    if (FAILED(Hr))
        Fail("no IDirectPlayLobby2A", Hr);

    Elems[ElemCount].guidDataType = DPAID_ServiceProvider;
    Elems[ElemCount].dwDataSize   = sizeof(GUID);
    Elems[ElemCount].lpData       = &SpGuid;
    ElemCount++;
    if (JoinAddr)
    {
        Elems[ElemCount].guidDataType = DPAID_INet;
        Elems[ElemCount].dwDataSize   = (DWORD)strlen(JoinAddr) + 1;
        Elems[ElemCount].lpData       = (LPVOID)JoinAddr;
        ElemCount++;
    }
    if (Port)
    {
        Elems[ElemCount].guidDataType = DPAID_INetPort;
        Elems[ElemCount].dwDataSize   = sizeof(DWORD);
        Elems[ElemCount].lpData       = &Port;
        ElemCount++;
    }

    /* Two-call idiom: ask for the size, then fill. */
    Hr = IDirectPlayLobby_CreateCompoundAddress(Lobby2, Elems, ElemCount, NULL, &AddrSize);
    if (Hr != DPERR_BUFFERTOOSMALL && FAILED(Hr))
        Fail("CreateCompoundAddress (sizing) failed", Hr);
    Addr = malloc(AddrSize);
    if (!Addr)
        Fail("out of memory", 0);
    Hr = IDirectPlayLobby_CreateCompoundAddress(Lobby2, Elems, ElemCount, Addr, &AddrSize);
    if (FAILED(Hr))
        Fail("CreateCompoundAddress failed", Hr);

    ZeroMemory(&Desc, sizeof(Desc));
    Desc.dwSize           = sizeof(Desc);
    Desc.guidApplication  = AppGuid;
    Desc.dwMaxPlayers     = MaxPlayers;
    Desc.lpszSessionNameA = (LPSTR)(Session ? Session : "VidyaGod");
    if (Hosting)
        Desc.dwFlags = DPSESSION_KEEPALIVE | DPSESSION_MIGRATEHOST;

    ZeroMemory(&Name, sizeof(Name));
    Name.dwSize          = sizeof(Name);
    Name.lpszShortNameA  = (LPSTR)Player;
    Name.lpszLongNameA   = (LPSTR)Player;

    ZeroMemory(&Conn, sizeof(Conn));
    Conn.dwSize         = sizeof(Conn);
    Conn.dwFlags        = Hosting ? DPLCONNECTION_CREATESESSION : DPLCONNECTION_JOINSESSION;
    Conn.lpSessionDesc  = &Desc;
    Conn.lpPlayerName   = &Name;
    Conn.guidSP         = SpGuid;
    Conn.lpAddress      = Addr;
    Conn.dwAddressSize  = AddrSize;

    if (!Hosting)
        RequireSession(Dll, &AppGuid, Addr, AddrSize, JoinAddr ? JoinAddr : "?");

    Hr = IDirectPlayLobby_RunApplication(Lobby, 0, &AppId, &Conn, NULL);
    if (FAILED(Hr))
        Fail("RunApplication failed", Hr);

    printf("vglobby: launched '%s' (%s) as %s, appid %lu\n",
           AppName, Hosting ? "hosting" : "joining", Player, (unsigned long)AppId);

    if (Wait)
        WaitForGame(AppId, ExeFile);

    free(Addr);
    IDirectPlayLobby_Release(Lobby2);
    IDirectPlayLobby_Release(Lobby);
    return 0;
}
