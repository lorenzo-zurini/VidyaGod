/* VidyaGod windows-native probe: argv[1] = %GameDir%, argv[2] = %AppData% as the launch spelled them.
   Sandboxie swallows stdout, so the result is the EXIT CODE, a bitmask of failed checks:
     1 the game's own file is not at argv[1]\data.txt     2 the edit in %GameDir% is missing or names another place
     4 the edit in %AppData% is missing or names another place     8 argv[1] is the machine's own C:\990001
     16 wrong argument count                                      32 argv[2] is not the machine's own AppData
     64 a save in argv[1]\saves (a kept folder) could not be written and read back
     128 the game's own file is not in its working directory (data.txt, a relative path: old games load so)
     256 the package's registry value (HKCU\Software\VidyaGodProbe GameDir) is missing or names another place
     512 the DLL the package places in the system folder (%SysDir64%\vgsys.dll) does not load, or is not its own
     1024 a save under %AppData% (argv[2]\probe\saves\count.txt, kept) could not be counted up
     2048 a kept registry value (Runs, under HKCU\Software\VidyaGodProbe\Kept, Kept2 and Kept3) could not be counted up
   The counters go up by one each run: the harness reads them in the instance's store after the launches. Three kept
   keys, so every teardown reads them out of the box one after another (a reg.exe started while the box still winds down
   from the one before is killed: AoE2's second kept key was lost so). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int readall(const char *p, char *buf, int n)
{
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    int k = (int)fread(buf, 1, n - 1, f);
    buf[k] = 0;
    fclose(f);
    while (k && (buf[k - 1] == '\n' || buf[k - 1] == '\r')) buf[--k] = 0;
    return k;
}

static int check(const char *what, const char *path, const char *want)
{
    char b[4096];
    if (readall(path, b, sizeof b) < 0) { printf("PROBE FAIL %s: cannot read %s\n", what, path); return 0; }
    printf("PROBE %s = %s\n", what, b);
    if (want && strcmp(b, want) != 0) { printf("PROBE FAIL %s: names another place than %s\n", what, want); return 0; }
    return 1;
}

static int writeall(const char *p, const char *text)
{
    FILE *s = fopen(p, "wb");
    if (!s) return 0;
    int bad = fputs(text, s) < 0;
    if (fclose(s) != 0) bad = 1;
    return !bad;
}

int main(int argc, char **argv)
{
    char p[4096], b[4096];
    int bits = 0;
    if (argc < 3) return 16;
    for (int i = 0; i < argc; ++i) printf("PROBE argv[%d] = %s\n", i, argv[i]);
    snprintf(p, sizeof p, "%s\\data.txt", argv[1]);       if (!check("game file", p, NULL)) bits |= 1;
    snprintf(p, sizeof p, "%s\\cfg.txt", argv[1]);        if (!check("edit in GameDir", p, argv[1])) bits |= 2;
    snprintf(p, sizeof p, "%s\\probe\\cfg.txt", argv[2]); if (!check("edit in AppData", p, argv[1])) bits |= 4;
    snprintf(p, sizeof p, "%s\\saves\\slot.txt", argv[1]);
    if (!writeall(p, "saved") || !check("save", p, "saved")) bits |= 64;
    if (!check("game file in the working directory", "data.txt", NULL)) bits |= 128;
    if (strncmp(argv[1], "C:\\990001", 9) == 0) bits |= 8;
    const char *appdata = getenv("APPDATA");
    if (!appdata || _stricmp(appdata, argv[2]) != 0) bits |= 32;

    DWORD n = sizeof b;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\VidyaGodProbe", "GameDir", RRF_RT_REG_SZ, NULL, b, &n) != ERROR_SUCCESS
        || strcmp(b, argv[1]) != 0) bits |= 256;

    HMODULE dll = LoadLibraryA("vgsys.dll");   /* not beside the exe: only the system folder has it */
    int (*value)(void) = dll ? (int (*)(void))GetProcAddress(dll, "vgsys_value") : NULL;
    if (!value || value() != 4242) bits |= 512;

    snprintf(p, sizeof p, "%s\\probe\\saves\\count.txt", argv[2]);
    int count = readall(p, b, sizeof b) >= 0 ? atoi(b) : 0;
    snprintf(b, sizeof b, "%d", count + 1);
    snprintf(p, sizeof p, "%s\\probe\\saves", argv[2]);
    CreateDirectoryA(p, NULL);
    snprintf(p, sizeof p, "%s\\probe\\saves\\count.txt", argv[2]);
    if (!writeall(p, b)) bits |= 1024;

    for (int i = 0; i < 3; ++i)
    {
        static const char *const keys[] = {"Software\\VidyaGodProbe\\Kept", "Software\\VidyaGodProbe\\Kept2", "Software\\VidyaGodProbe\\Kept3"};
        HKEY k;
        DWORD runs = 0, size = sizeof runs;
        if (RegCreateKeyExA(HKEY_CURRENT_USER, keys[i], 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS)
        {
            if (RegQueryValueExA(k, "Runs", NULL, NULL, (BYTE *)&runs, &size) != ERROR_SUCCESS) runs = 0;
            ++runs;
            if (RegSetValueExA(k, "Runs", 0, REG_DWORD, (const BYTE *)&runs, sizeof runs) != ERROR_SUCCESS) bits |= 2048;
            RegCloseKey(k);
        }
        else bits |= 2048;
    }

    printf("PROBE result %d\n", bits);
    fflush(stdout);
    return bits;
}
