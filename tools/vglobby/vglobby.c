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
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>

/* stderr of a GUI-subsystem exe under wine reaches nobody; the log file is the only observable trace.
 * Z: is wine's root-of-host mapping, so this lands in the host's /tmp (bound into the sandbox). */
static void DbgLog(const char *Fmt, ...)
{
    static FILE *F;
    va_list Ap;
    if (!F)
        F = fopen("Z:\\tmp\\vglobby_dbg.log", "a");
    if (!F)
        return;
    va_start(Ap, Fmt);
    vfprintf(F, Fmt, Ap);
    va_end(Ap);
    fputc('\n', F);
    fflush(F);
}

static void Fail(const char *Msg, HRESULT Hr)
{
    DbgLog("FAIL: %s (hr=0x%08lx)", Msg, (unsigned long)Hr);
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

/* --- launcher-dialog driver ------------------------------------------------------------------------------ */
/*
 * Both WOXL exes open a code-built launcher window ("Wipeout XL": Device / Resolution / Sound, a Play Game
 * button) on EVERY start -- the binaries import RegQueryValueExA but no RegSetValueExA, so the choices are
 * never persisted anywhere and no registry seeding can suppress the window. The only way to skip it is to
 * DRIVE it: find the window the instant it appears, hide it before it can paint, apply the requested combo
 * values, and press Play programmatically. The game's own fullscreen window carries the SAME caption, so the
 * launcher is identified by its "Play Game" child button, never by caption alone.
 */
/* WM_GETTEXT is a SENT message: plain GetWindowTextA on a foreign window BLOCKS until that window's thread
 * pumps -- and the launcher's thread spends seconds loading between pumps, which silently froze the whole
 * driver while the dialog sat on screen. Timeout + abort-if-hung keeps the watcher lively no matter what the
 * game thread is doing. */
static int SafeText(HWND w, char *Buf, int Cch)
{
    DWORD_PTR R = 0;
    Buf[0] = 0;
    if (!SendMessageTimeoutA(w, WM_GETTEXT, (WPARAM)Cch, (LPARAM)Buf, SMTO_ABORTIFHUNG, 100, &R))
        return 0;
    Buf[Cch - 1] = 0;
    return 1;
}

static int HasWordCi(const char *Hay, const char *Needle)
{
    size_t i, n = strlen(Needle), h = strlen(Hay);
    for (i = 0; n && i + n <= h; i++)
        if (!_strnicmp(Hay + i, Needle, n))
            return 1;
    return 0;
}

struct LauncherKids
{
    HWND Play;
    HWND Btn[8];
    HWND Combo[8];
    int  NBtn, NCombo;
};

/* Controls are classified by CLASS + STYLE + GEOMETRY, with text as a bonus. "Button" class covers groupboxes,
 * checkboxes and radios too -- the SP launcher's leftmost "Button" is the GROUPBOX around the combos, and the
 * driver once pressed IT for 30 straight seconds. Only push-button styles qualify. Text (SafeText, bounded
 * WM_GETTEXT) is tried first: a pumping dialog (SP) answers and "Play"/"OK"/"Start" wins outright; the busy
 * NET-WOXL launcher times out and falls back to the leftmost qualifying push button. */
static BOOL CALLBACK CollectKids(HWND w, LPARAM p)
{
    struct LauncherKids *K = (struct LauncherKids *)p;
    char Cls[64] = {0};

    GetClassNameA(w, Cls, sizeof(Cls));
    if (!lstrcmpiA(Cls, "Button") && K->NBtn < 8)
    {
        const LONG Type = GetWindowLongA(w, GWL_STYLE) & BS_TYPEMASK;
        if (Type == BS_PUSHBUTTON || Type == BS_DEFPUSHBUTTON || Type == BS_OWNERDRAW)
            K->Btn[K->NBtn++] = w;
    }
    else if (!lstrcmpiA(Cls, "ComboBox") && K->NCombo < 8)
        K->Combo[K->NCombo++] = w;
    return TRUE;
}

static void PickPlay(struct LauncherKids *K)
{
    int  i;
    RECT Best, R;
    char Txt[64];

    K->Play = NULL;
    for (i = 0; i < K->NBtn; i++)
        if (SafeText(K->Btn[i], Txt, sizeof(Txt)) &&
            (HasWordCi(Txt, "play") || HasWordCi(Txt, "start") || !lstrcmpiA(Txt, "ok")))
        {
            K->Play = K->Btn[i];
            return;
        }
    for (i = 0; i < K->NBtn; i++)
    {
        GetWindowRect(K->Btn[i], &R);
        if (!K->Play || R.left < Best.left)
        {
            K->Play = K->Btn[i];
            Best    = R;
        }
    }
}

struct LauncherFind
{
    DWORD Pid;                 /* 0 = any process */
    HWND  Win;
    struct LauncherKids Kids;
};

static int DbgEnum = 0;   /* dump the first few enumerations wholesale */

static BOOL CALLBACK FindLauncher(HWND w, LPARAM p)
{
    struct LauncherFind *F = (struct LauncherFind *)p;
    struct LauncherKids  K;
    char  Txt[64] = {0};
    RECT  R;
    DWORD Pid = 0;
    LONG  Style;

    GetWindowThreadProcessId(w, &Pid);
    if (F->Pid && Pid != F->Pid)
        return TRUE;
    /* GetWindowText on ANOTHER process's window reads the cached server-side title -- it does NOT send
     * WM_GETTEXT, so it cannot block on the game's busy thread. (The SendMessageTimeout variant used briefly
     * here COULD NOT read the title while the launcher loaded -- which is exactly when it sits on screen.) */
    GetWindowTextA(w, Txt, sizeof(Txt));
    if (DbgEnum > 0)
    {
        RECT DR; LONG DS = GetWindowLongA(w, GWL_STYLE);
        GetWindowRect(w, &DR);
        DbgLog("  enum hwnd=%p pid=%lu txt='%s' style=%08lx w=%ld vis=%d", (void *)w, (unsigned long)Pid,
               Txt, (unsigned long)DS, (long)(DR.right - DR.left), IsWindowVisible(w));
    }
    if (lstrcmpiA(Txt, "Wipeout XL") != 0)
        return TRUE;
    /* Both the launcher AND the game window are caption-less WS_POPUPs with this title (the title bar the
     * desktop shows is the window manager's decoration, not a win32 style — checking WS_CAPTION here rejected
     * the launcher on every tick). Size alone cannot fully discriminate either (the game's win32 window is the
     * render resolution). The DRIVER is therefore stateful: it only touches caption-matched popups BEFORE it
     * has pressed Play, and stands down permanently the moment its tracked window dies. */
    Style = GetWindowLongA(w, GWL_STYLE);
    (void)Style;
    GetWindowRect(w, &R);
    if ((R.right - R.left) > 800)
        return TRUE;
    F->Win = w;
    ZeroMemory(&K, sizeof(K));
    EnumChildWindows(w, CollectKids, (LPARAM)&K);
    PickPlay(&K);
    F->Kids = K;               /* Play may still be NULL — the shell precedes its controls */
    return FALSE;
}

static int ComboYCompare(HWND a, HWND b)
{
    RECT Ra, Rb;
    GetWindowRect(a, &Ra);
    GetWindowRect(b, &Rb);
    return Ra.top - Rb.top;
}

/* Watch for the launcher for up to TimeoutMs, and when it appears: hide, configure, press Play. Res/Snd select
 * combo entries by PREFIX (CB_SELECTSTRING semantics), so "640" matches "640x480". Returns 1 once driven. */
static int DriveLauncher(DWORD Pid, const char *Res, const char *Snd, DWORD TimeoutMs)
{
    DWORD Start = GetTickCount(), Waited = 0, PlaySeen = 0, ClickedAt = 0, LastTickLog = 0;
    int   Clicked = 0, ClickedCombos = 0;
    HWND  ClickedWin = NULL;

    DbgLog("driver enter (pid=%lu)", (unsigned long)Pid);
    /* Waited is WALL time: each iteration can cost far more than the Sleep below (SendMessageTimeout pays its
     * timeout per busy window), and counting iterations made the 30s budget mean nearly an hour. */
    while ((Waited = GetTickCount() - Start) < TimeoutMs)
    {
        struct LauncherFind F;
        int j, k;
        ZeroMemory(&F, sizeof(F));
        F.Pid = (Waited < 3000) ? Pid : 0;
        DbgEnum = 0;
        EnumWindows(FindLauncher, (LPARAM)&F);
        if (Waited - LastTickLog >= 2000 || !LastTickLog)
        {
            LastTickLog = Waited ? Waited : 1;
            DbgLog("tick t=%lu win=%p play=%p combos=%d", (unsigned long)Waited,
                   (void *)F.Win, (void *)F.Kids.Play, F.Kids.NCombo);
        }

        if (!Clicked && F.Win)
        {
            /* NO win32-side hiding here. An earlier version yanked the window offscreen+hidden every tick --
             * but Wipeout2 REUSES the dialog's window as the game window, so whichever async yanks were still
             * queued at click time hit the GAME, leaving it a tiny black rectangle on the desktop (and made
             * every run different: the bug was a race with the queue). Hiding is the X-side cloak's job now;
             * it needs no cooperation from this window's thread and never touches win32 state. */
            if (!PlaySeen)
                PlaySeen = Waited ? Waited : 1;

            /* NET-WOXL's launcher draws its whole UI ITSELF -- EnumChildWindows finds nothing, there is no
             * button to press. Enter is its confirm key (the dialog auto-continues on a timer, so it has a
             * default action): post the keystroke and it fires the instant the dialog starts pumping. The
             * SP launcher (Wipeout2.exe) has REAL controls and takes the combo+command route below. */
            if (!F.Kids.Play && F.Kids.NCombo == 0 && Waited - PlaySeen >= 2000)
            {
                PostMessageA(F.Win, WM_KEYDOWN, VK_RETURN, 0x001C0001);
                PostMessageA(F.Win, WM_KEYUP,   VK_RETURN, 0xC01C0001);
                Clicked    = 1;
                ClickedAt  = Waited;
                ClickedWin = F.Win;
                DbgLog("posted Enter (no controls found) at t=%lums", (unsigned long)Waited);
            }
            else if (F.Kids.Play && F.Kids.NCombo >= 2)
            {
                if (Waited - PlaySeen >= 300)
                {
                    DWORD_PTR Sel;
                    for (j = 1; j < F.Kids.NCombo; j++)
                        for (k = j; k > 0 && ComboYCompare(F.Kids.Combo[k - 1], F.Kids.Combo[k]) > 0; k--)
                        {
                            HWND T = F.Kids.Combo[k];
                            F.Kids.Combo[k] = F.Kids.Combo[k - 1];
                            F.Kids.Combo[k - 1] = T;
                        }
                    if (Res && *Res)
                        if (!SendMessageTimeoutA(F.Kids.Combo[0], CB_SELECTSTRING, (WPARAM)-1, (LPARAM)Res,
                                                 SMTO_ABORTIFHUNG, 500, &Sel) || (LRESULT)Sel == CB_ERR)
                            DbgLog("resolution '%s' not applied", Res);
                    if (Snd && *Snd)
                        if (!SendMessageTimeoutA(F.Kids.Combo[1], CB_SELECTSTRING, (WPARAM)-1, (LPARAM)Snd,
                                                 SMTO_ABORTIFHUNG, 500, &Sel) || (LRESULT)Sel == CB_ERR)
                            DbgLog("sound '%s' not applied", Snd);
                    /* BM_CLICK, POSTED: the button synthesizes a real press when its thread pumps. A posted
                     * WM_COMMAND was ignored here -- the geometric pick's control can report dialog id 0, and
                     * the game's handler drops id-0 commands (SP soft-locked exactly that way once). */
                    PostMessageA(F.Kids.Play, BM_CLICK, 0, 0);
                    Clicked       = 1;
                    ClickedAt     = Waited;
                    ClickedWin    = F.Win;
                    ClickedCombos = F.Kids.NCombo;
                    DbgLog("pressed Play at t=%lums (combos=%d)", (unsigned long)Waited, F.Kids.NCombo);
                }
            }
        }
        else if (Clicked)
        {
            /* After the click we take our hands off every window (the next caption-matched popup is the GAME).
             * Success = the DIALOG is gone, and caption alone cannot tell: Wipeout2 reuses the very same win32
             * window for the game (controls destroyed, no new handle), while NET-WOXL destroys its window.
             * So: handle gone, handle changed, or the controls we clicked through have vanished. */
            if (!IsWindow(ClickedWin) || F.Win != ClickedWin ||
                (ClickedCombos > 0 && F.Kids.NCombo == 0))
            {
                DbgLog("launcher gone after click -- driven");
                return 1;
            }
            if (Waited - ClickedAt >= 1500)
            {
                if (F.Kids.Play)
                {
                    PostMessageA(F.Kids.Play, BM_CLICK, 0, 0);
                    if (GetDlgCtrlID(F.Kids.Play))
                        PostMessageA(F.Win, WM_COMMAND,
                                     MAKEWPARAM(GetDlgCtrlID(F.Kids.Play), BN_CLICKED), (LPARAM)F.Kids.Play);
                }
                else
                {
                    PostMessageA(F.Win, WM_KEYDOWN, VK_RETURN, 0x001C0001);
                    PostMessageA(F.Win, WM_KEYUP,   VK_RETURN, 0xC01C0001);
                }
                ClickedAt = Waited;
                DbgLog("re-pressed at t=%lums", (unsigned long)Waited);
            }
        }
        Sleep(15);
    }
    DbgLog("driver timeout after %lums (clicked=%d)", (unsigned long)TimeoutMs, Clicked);
    return Clicked;
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
    const char *Res = NULL, *Snd = NULL;
    int         Hosting  = 1, Wait = 0, RunDirect = 0, i;   /* host unless an address says otherwise */
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
        else if (!strcmp(a, "--run"))     RunDirect = 1;   /* plain spawn, no DirectPlay -- the single-player path */
        else if (!strcmp(a, "--res"))     Res      = NEXT();
        else if (!strcmp(a, "--snd"))     Snd      = NEXT();
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

    if (RunDirect ? !ExeFile : (!AppName || !GuidText || !ExeFile || !Player))
    {
        fprintf(stderr,
            "usage: vglobby --app NAME --guid {GUID} --exe FILE.EXE [--dir DIR]\n"
            "               --player NAME [--session NAME] [--address=ADDR | --host | --join [ADDR]]\n"
            "               [--sp tcpip|ipx|serial|modem] [--port N] [--max N] [--wait]\n"
            "               [--run] [--res 640x480] [--snd Stereo]\n"
            "  --run spawns FILE.EXE directly (no DirectPlay) -- the single-player path.\n"
            "  --res/--snd preselect the launcher dialog combos; the dialog itself is always\n"
            "  hidden and auto-confirmed, so the game boots straight in.\n");
        return 2;
    }
    if (RunDirect)
    {
        /* Single player through the same launcher: spawn the exe, kill its config dialog, wait it out. */
        STARTUPINFOA        Si;
        PROCESS_INFORMATION Pi;
        char                Cmd[MAX_PATH + 2];

        ZeroMemory(&Si, sizeof(Si)); Si.cb = sizeof(Si);
        ZeroMemory(&Pi, sizeof(Pi));
        snprintf(Cmd, sizeof(Cmd), "%s", ExeFile);
        if (!CreateProcessA(NULL, Cmd, NULL, NULL, FALSE, 0, NULL, Dir, &Si, &Pi))
            Fail("CreateProcess failed for --run", (HRESULT)GetLastError());
        printf("vglobby: spawned %s (pid %lu)\n", ExeFile, (unsigned long)Pi.dwProcessId);
        DriveLauncher(Pi.dwProcessId, Res, Snd, 30000);
        if (Wait)
            WaitForSingleObject(Pi.hProcess, INFINITE);
        CloseHandle(Pi.hThread);
        CloseHandle(Pi.hProcess);
        return 0;
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

    /* The lobby-launched game opens the same config dialog; kill it here too. AppId is the pid on
     * implementations that bother (wine's does) -- 0 falls back to matching any process. */
    DbgLog("lobby path: appid=%lu res=%s snd=%s", (unsigned long)AppId, Res ? Res : "-", Snd ? Snd : "-");
    DriveLauncher(AppId, Res, Snd, 30000);

    if (Wait)
        WaitForGame(AppId, ExeFile);

    free(Addr);
    IDirectPlayLobby_Release(Lobby2);
    IDirectPlayLobby_Release(Lobby);
    return 0;
}
