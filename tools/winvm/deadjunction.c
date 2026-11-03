/* deadjunction <folder>: makes <folder> a mount-point junction to a volume that does not exist, as WinFsp leaves its
   mount folder when the machine dies with the mount up (WinFsp removes it only when its own process exits).
   mklink /J refuses a volume target ("Local volumes are required"), so the reparse point is written directly. */
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

int wmain(int argc, wchar_t **argv)
{
    if (argc != 2) { fwprintf(stderr, L"usage: deadjunction <folder>\n"); return 2; }
    const wchar_t *target = L"\\Device\\Volume{00000000-0000-0000-0000-000000000000}\\";   /* as WinFsp writes it */
    if (!CreateDirectoryW(argv[1], NULL)) { fwprintf(stderr, L"cannot create %ls (%lu)\n", argv[1], GetLastError()); return 1; }
    HANDLE h = CreateFileW(argv[1], GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) { fwprintf(stderr, L"cannot open %ls (%lu)\n", argv[1], GetLastError()); return 1; }
    /* MountPointReparseBuffer: SubstituteName and PrintName, each NUL-terminated, in PathBuffer. */
    const USHORT n = (USHORT)(wcslen(target) * sizeof(wchar_t));
    BYTE buf[1024] = {0};
    DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
    USHORT dataLen = (USHORT)(8 + n + 2 + n + 2), zero = 0, subOff = 0, subLen = n, prOff = (USHORT)(n + 2), prLen = n;
    memcpy(buf, &tag, 4); memcpy(buf + 4, &dataLen, 2); memcpy(buf + 6, &zero, 2);
    memcpy(buf + 8, &subOff, 2); memcpy(buf + 10, &subLen, 2); memcpy(buf + 12, &prOff, 2); memcpy(buf + 14, &prLen, 2);
    memcpy(buf + 16, target, n); memcpy(buf + 16 + n + 2, target, n);
    DWORD got = 0;
    BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf, 8 + dataLen, NULL, 0, &got, NULL);
    CloseHandle(h);
    if (!ok) { fwprintf(stderr, L"FSCTL_SET_REPARSE_POINT failed (%lu)\n", GetLastError()); RemoveDirectoryW(argv[1]); return 1; }
    return 0;
}
