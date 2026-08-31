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
 *               --player NAME [--session NAME] [--host | --join [ADDRESS]]
 *               [--sp tcpip|ipx|serial|modem] [--port N] [--max N] [--wait]
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
    int         Hosting  = -1, Wait = 0, i;
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
        if      (!strcmp(a, "--app"))     AppName  = NEXT();
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

    if (!AppName || !GuidText || !ExeFile || !Player || Hosting < 0)
    {
        fprintf(stderr,
            "usage: vglobby --app NAME --guid {GUID} --exe FILE.EXE [--dir DIR]\n"
            "               --player NAME [--session NAME] (--host | --join [ADDRESS])\n"
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
    pLobbyCreate = (HRESULT (WINAPI *)(LPGUID, LPDIRECTPLAYLOBBYA *, IUnknown *, LPVOID, DWORD))
                   GetProcAddress(Dll, "DirectPlayLobbyCreateA");
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

    Hr = IDirectPlayLobby_RunApplication(Lobby, 0, &AppId, &Conn, NULL);
    if (FAILED(Hr))
        Fail("RunApplication failed", Hr);

    printf("vglobby: launched '%s' (%s) as %s, appid %lu\n",
           AppName, Hosting ? "hosting" : "joining", Player, (unsigned long)AppId);

    if (Wait)
    {
        /* Lobby-launched children die with the lobby only if it exits first on some providers; holding here
         * keeps this process around as the lobby owner for the life of the game. */
        HANDLE Proc = OpenProcess(SYNCHRONIZE, FALSE, AppId);
        if (Proc)
        {
            WaitForSingleObject(Proc, INFINITE);
            CloseHandle(Proc);
        }
    }

    free(Addr);
    IDirectPlayLobby_Release(Lobby2);
    IDirectPlayLobby_Release(Lobby);
    return 0;
}
