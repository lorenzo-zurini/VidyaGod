#include "ziplayer.h"
#include "layerspec.h"   // NormalizeVPath

#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <iostream>

static constexpr uint64_t kMemThreshold = 64ull * 1024 * 1024; // entries above this go to a temp file

ZipIndex::~ZipIndex()
{
    if (archive) zip_close(archive);
}

ZipReader::~ZipReader()
{
    if (tmpFd >= 0) { ::close(tmpFd); }
    if (!tmpPath.empty()) { ::unlink(tmpPath.c_str()); }
}

//Records `vrel` as a child of each of its ancestor directories, synthesizing the dir chain so
//readdir works even when the zip omits explicit directory entries.
static void RegisterParents(ZipIndex &Z, const std::string &vrel)
{
    std::string cur = vrel;
    while (true)
    {
        size_t slash = cur.find_last_of('/');
        std::string parent = (slash == std::string::npos) ? std::string() : cur.substr(0, slash);
        std::string child  = (slash == std::string::npos) ? cur : cur.substr(slash + 1);
        if (child.empty()) break;
        Z.dirChildren[parent].insert(child);
        if (parent.empty()) break;
        cur = parent;
    }
}

std::shared_ptr<ZipIndex> BuildZipIndex(const std::string &ArchivePath)
{
    int err = 0;
    zip_t *za = zip_open(ArchivePath.c_str(), ZIP_RDONLY, &err);
    if (!za)
    {
        std::cerr << "[vidyagodfs] zip_open failed for " << ArchivePath << " (err " << err << ")\n";
        return nullptr;
    }

    auto Z = std::make_shared<ZipIndex>();
    Z->archivePath = ArchivePath;
    Z->archive = za;

    zip_int64_t n = zip_get_num_entries(za, 0);
    for (zip_int64_t i = 0; i < n; ++i)
    {
        zip_stat_t st;
        zip_stat_init(&st);
        if (zip_stat_index(za, i, 0, &st) != 0) continue;
        if (!(st.valid & ZIP_STAT_NAME) || st.name == nullptr) continue;

        std::string raw = st.name;
        bool isDir = (!raw.empty() && raw.back() == '/');
        std::string vrel = NormalizeVPath(raw);
        if (vrel.empty()) continue; // skip a root entry

        if (isDir)
        {
            ZipEntry e; e.name = vrel; e.index = (uint64_t)i; e.isDir = true;
            if (st.valid & ZIP_STAT_MTIME) e.mtime = st.mtime;
            Z->byName[vrel] = e;
            Z->dirChildren.try_emplace(vrel); // ensure the dir node exists even if empty
        }
        else
        {
            ZipEntry e; e.name = vrel; e.index = (uint64_t)i; e.isDir = false;
            if (st.valid & ZIP_STAT_SIZE)  e.size  = st.size;
            if (st.valid & ZIP_STAT_MTIME) e.mtime = st.mtime;
            Z->byName[vrel] = e;
        }
        RegisterParents(*Z, vrel);
    }
    return Z;
}

int OpenZipEntry(ZipIndex &Z, const ZipEntry &E, const std::string &TempDir, ZipReader &Out)
{
    Out.size = E.size;
    if (E.size == 0) { Out.mem.clear(); return 0; } // empty file

    std::lock_guard<std::mutex> Lock(Z.mtx); // zip_fopen_index on a shared zip_t is not concurrent-safe

    zip_file_t *zf = zip_fopen_index(Z.archive, E.index, 0);
    if (!zf) return -EIO;

    const bool large = E.size > kMemThreshold;
    if (large)
    {
        std::string tmpl = TempDir + "/zipcacheXXXXXX";
        std::vector<char> path(tmpl.begin(), tmpl.end()); path.push_back('\0');
        int fd = ::mkstemp(path.data());
        if (fd < 0) { zip_fclose(zf); return -errno; }
        Out.tmpFd = fd;
        Out.tmpPath = path.data();
    }

    char buf[256 * 1024];
    uint64_t total = 0;
    int rc = 0;
    while (total < E.size)
    {
        zip_int64_t got = zip_fread(zf, buf, sizeof buf);
        if (got < 0) { rc = -EIO; break; }
        if (got == 0) break;
        if (large)
        {
            ssize_t w = ::write(Out.tmpFd, buf, (size_t)got);
            if (w != got) { rc = -EIO; break; }
        }
        else
        {
            Out.mem.insert(Out.mem.end(), buf, buf + got);
        }
        total += (uint64_t)got;
    }
    zip_fclose(zf);
    if (rc != 0) return rc;
    return 0;
}

int ReadZipEntry(ZipReader &R, char *Buf, size_t Size, off_t Off)
{
    if (Off < 0) return -EINVAL;
    if ((uint64_t)Off >= R.size) return 0;
    size_t avail = (size_t)(R.size - (uint64_t)Off);
    size_t want = Size < avail ? Size : avail;

    if (R.tmpFd >= 0)
    {
        ssize_t got = ::pread(R.tmpFd, Buf, want, Off);
        return got < 0 ? -errno : (int)got;
    }
    std::memcpy(Buf, R.mem.data() + Off, want);
    return (int)want;
}
