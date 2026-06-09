#ifndef VIDYAGODFS_ZIPLAYER_H
#define VIDYAGODFS_ZIPLAYER_H

#include <string>
#include <vector>
#include <unordered_map>
#include <set>
#include <memory>
#include <mutex>
#include <cstdint>
#include <ctime>
#include <zip.h>

//One file entry inside a zip layer, captured at index time.
struct ZipEntry {
    std::string name;            // normalized, no leading/trailing '/'
    uint64_t    index = 0;       // zip entry index, for zip_fopen_index
    uint64_t    size = 0;        // uncompressed size
    time_t      mtime = 0;
    bool        isDir = false;
};

//A whole zip archive indexed for the FS lifetime. Reads are extracted on open (see OpenZipEntry):
//libzip's per-entry decompression is sequential and not seekable for DEFLATE, so we materialize the
//full entry into memory or a temp file once per open; Wine keeps mmapped EXE/DLLs open for the
//process lifetime, so this is one extraction per open, not per page fault.
struct ZipIndex {
    std::string archivePath;
    zip_t*      archive = nullptr;
    std::unordered_map<std::string, ZipEntry>              byName;      // files + explicit dir entries
    std::unordered_map<std::string, std::set<std::string>> dirChildren; // parent vrel → child segment names
    std::mutex  mtx;             // guards zip_fopen_index extraction on `archive`
    ~ZipIndex();
};

//Opens ArchivePath and indexes every entry (synthesizing missing parent dirs). nullptr on failure.
std::shared_ptr<ZipIndex> BuildZipIndex(const std::string &ArchivePath);

//Per-open materialized reader: small entries live in `mem`; large ones in a temp file `tmpFd`.
struct ZipReader {
    std::vector<char> mem;
    int               tmpFd = -1;
    std::string       tmpPath;
    uint64_t          size = 0;
    ~ZipReader();
};

//Extracts entry E from Z into Out (memory if <= MemThreshold, else a temp file under TempDir).
//Returns 0 or -errno.
int OpenZipEntry(ZipIndex &Z, const ZipEntry &E, const std::string &TempDir, ZipReader &Out);

//Serves a read from an opened ZipReader. Returns bytes read (>=0) or -errno.
int ReadZipEntry(ZipReader &R, char *Buf, size_t Size, off_t Off);

#endif // VIDYAGODFS_ZIPLAYER_H
