// Xbox 360 build of pc-recomp/src/kernel/files.cpp: the game's disc (D:, \Device\CdRom0) is the folder beside the
// XEX (game:\), the hard disk partitions are folders under game:\hdd. The XDK's Win32 file calls replace
// std::filesystem; 360 file systems ignore case, so names need no matching. No disc image support.
#include <algorithm>
#include <ctype.h>
#include <map>
#include <stdio.h>

#include "host.h"
#include "kernel/exports.h"
#include "kernel/kernel.h"

#ifndef INVALID_FILE_ATTRIBUTES
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFF
#endif

namespace kernel {

namespace {

// The WMV movies (d:\movie\*.wmv) are reported missing, as the game allows: its software decoder plays them at about
// 35% of their speed (v39's log). game/movie.cpp plays the converted ones.
bool IsMovie(const std::string& name) {
    return name.size() > 4 && _stricmp(name.c_str() + name.size() - 4, ".wmv") == 0;
}

std::mutex g_links_lock;
std::map<std::string, std::string> g_links;    // upper case "\??\D:" -> "\Device\CdRom0"
std::map<std::string, std::string> g_devices;  // upper case device -> host directory, no trailing backslash
const char kCdRom[] = "\\DEVICE\\CDROM0";

std::string Upper(std::string s) {
    for (size_t i = 0; i < s.size(); i++) s[i] = char(toupper(uint8_t(s[i])));
    return s;
}

std::string Normalize(std::string p) {
    std::replace(p.begin(), p.end(), '/', '\\');
    std::string out;
    for (size_t i = 0; i < p.size(); i++) {
        char c = p[i];
        if (c == '\\' && !out.empty() && out[out.size() - 1] == '\\') continue;
        out += c;
    }
    while (out.size() > 1 && out[out.size() - 1] == '\\') out.erase(out.size() - 1);
    return out;
}

bool Exists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
bool IsDir(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
std::string Parent(const std::string& p) {
    size_t s = p.find_last_of('\\');
    return s == std::string::npos ? std::string() : p.substr(0, s);
}
void MakeDirs(const std::string& p) {
    if (p.empty() || IsDir(p)) return;
    MakeDirs(Parent(p));
    CreateDirectoryA(p.c_str(), NULL);
}

struct HostInfo {
    uint64_t time, size;
    bool dir;
};
bool Stat(const std::string& p, HostInfo& out) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &d)) return false;
    out.time = (uint64_t(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
    out.size = (uint64_t(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    out.dir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return true;
}

struct DirEntry {
    std::string name;
    HostInfo info;
    bool operator<(const DirEntry& o) const { return name < o.name; }
};

struct File : Object {
    std::string host;
    std::string xbox;
    FILE* fp;
    bool dir;
    uint64_t pos;
    std::vector<DirEntry> entries;
    size_t next;
    bool listed;
    bool delete_on_close;
    std::mutex io;  // threads sharing a handle: seek and transfer stay together
    File() : Object(kObjFile), fp(NULL), dir(false), pos(0), next(0), listed(false), delete_on_close(false) {}
    ~File() {
        if (fp) fclose(fp);
        if (delete_on_close) DeleteFileA(host.c_str());
    }
};

struct Link : Object {
    std::string target;
    Link() : Object(kObjSymLink) {}
};

void FillNetworkInfo(uint32_t out, uint64_t t, uint64_t size, bool dir) {
    for (int i = 0; i < 4; i++) W64(out + i * 8, t);
    W64(out + 0x20, (size + 0xFFF) & ~0xFFFull);
    W64(out + 0x28, size);
    W32(out + 0x30, dir ? 0x10 : 0x80);  // FILE_ATTRIBUTE_DIRECTORY or NORMAL
}

void FillNetworkInfo(uint32_t out, const HostInfo& h) { FillNetworkInfo(out, h.time, h.dir ? 0 : h.size, h.dir); }

bool WildMatch(const std::string& pat, const std::string& s) {
    size_t p = 0, i = 0, star = std::string::npos, mark = 0;
    while (i < s.size()) {
        if (p < pat.size() && (pat[p] == '?' || toupper(uint8_t(pat[p])) == toupper(uint8_t(s[i])))) {
            p++;
            i++;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++;
            mark = i;
        } else if (star != std::string::npos) {
            p = star + 1;
            i = ++mark;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') p++;
    return p == pat.size();
}

void IoStatus(uint32_t iosb, uint32_t status, uint32_t info) {
    if (!iosb) return;
    W32(iosb, status);
    W32(iosb + 4, info);
}

std::shared_ptr<File> FileFromHandle(uint32_t h) {
    ObjectRef o = FromHandle(h);
    if (!o || o->type != kObjFile) return std::shared_ptr<File>();
    return std::static_pointer_cast<File>(o);
}

std::string ObjectName(uint32_t attrs, uint32_t* root = NULL) {
    if (root) *root = R32(attrs);
    return ReadAnsiString(R32(attrs + 4));
}

void Complete(uint32_t event, uint32_t apc, uint32_t apc_ctx, uint32_t iosb) {
    if (event) {
        ObjectRef e = FromHandle(event);
        if (e) SetEvent(*e);
    }
    if (apc && Current()) QueueApc(*Current(), apc, apc_ctx, iosb, 0);
}

uint64_t FileSize(File& f) {
    if (f.fp) {
        fflush(f.fp);
        int64_t at = _ftelli64(f.fp);
        _fseeki64(f.fp, 0, SEEK_END);
        int64_t end = _ftelli64(f.fp);
        _fseeki64(f.fp, at, SEEK_SET);
        return uint64_t(end);
    }
    HostInfo h;
    return Stat(f.host, h) && !h.dir ? h.size : 0;
}

}  // namespace

std::string ReadAnsiString(uint32_t str) {
    if (!str) return std::string();
    uint16_t len = R16(str);
    uint32_t buf = R32(str + 4);
    if (!buf) return std::string();
    std::string s(len, '\0');
    for (uint16_t i = 0; i < len; i++) s[i] = char(G8(buf + i));
    return s;
}

std::string ReadCString(uint32_t va, size_t max) {
    std::string s;
    if (!va) return s;
    for (size_t i = 0; i < max; i++) {
        char c = char(G8(va + uint32_t(i)));
        if (!c) break;
        s += c;
    }
    return s;
}

void MountDefaults(const std::string& game_dir, const std::string& data_dir) {
    std::lock_guard<std::mutex> lock(g_links_lock);
    g_devices[kCdRom] = Normalize(game_dir);
    g_devices["\\DEVICE\\HARDDISK0\\PARTITION1"] = Normalize(data_dir + "\\E");
    g_devices["\\DEVICE\\HARDDISK0\\PARTITION2"] = Normalize(data_dir + "\\C");
    g_devices["\\DEVICE\\HARDDISK0\\PARTITION3"] = Normalize(data_dir + "\\X");
    g_devices["\\DEVICE\\HARDDISK0\\PARTITION4"] = Normalize(data_dir + "\\Y");
    g_devices["\\DEVICE\\HARDDISK0\\PARTITION5"] = Normalize(data_dir + "\\Z");
    for (std::map<std::string, std::string>::iterator it = g_devices.begin(); it != g_devices.end(); ++it)
        if (it->first != kCdRom) MakeDirs(it->second);
    g_links["\\??\\D:"] = "\\Device\\CdRom0";
    LOGI("files: D: is %s, hard disk under %s", game_dir.c_str(), data_dir.c_str());
}

bool CreateSymbolicLink(const std::string& link, const std::string& target) {
    std::lock_guard<std::mutex> lock(g_links_lock);
    g_links[Upper(Normalize(link))] = Normalize(target);
    LOGI("files: link %s -> %s", link.c_str(), target.c_str());
    return true;
}

bool DeleteSymbolicLink(const std::string& link) {
    std::lock_guard<std::mutex> lock(g_links_lock);
    return g_links.erase(Upper(Normalize(link))) != 0;
}

std::string QuerySymbolicLink(const std::string& link) {
    std::lock_guard<std::mutex> lock(g_links_lock);
    std::map<std::string, std::string>::iterator it = g_links.find(Upper(Normalize(link)));
    return it == g_links.end() ? std::string() : it->second;
}

// the host path of an Xbox path, empty when no device holds it
std::string ResolvePath(const std::string& xbox_path, uint32_t root_handle) {
    std::string p = Normalize(xbox_path);
    if (root_handle) {
        std::shared_ptr<File> f = FileFromHandle(root_handle);
        if (f) p = Normalize(f->xbox + "\\" + p);
    }
    if (p.size() >= 2 && p[1] == ':') p = "\\??\\" + p;
    std::lock_guard<std::mutex> lock(g_links_lock);
    for (int depth = 0; depth < 8; depth++) {
        std::string up = Upper(p);
        bool changed = false;
        for (std::map<std::string, std::string>::iterator it = g_links.begin(); it != g_links.end(); ++it) {
            const std::string& k = it->first;
            if (up.compare(0, k.size(), k) == 0 && (up.size() == k.size() || up[k.size()] == '\\')) {
                p = it->second + p.substr(k.size());
                changed = true;
                break;
            }
        }
        if (!changed) break;
    }
    std::string up = Upper(p);
    std::map<std::string, std::string>::iterator best = g_devices.end();
    for (std::map<std::string, std::string>::iterator it = g_devices.begin(); it != g_devices.end(); ++it)
        if (up.compare(0, it->first.size(), it->first) == 0 && (up.size() == it->first.size() || up[it->first.size()] == '\\'))
            if (best == g_devices.end() || it->first.size() > best->first.size()) best = it;
    if (best == g_devices.end()) return std::string();
    std::string rest = p.substr(best->first.size());
    // no Xbox name holds ':', which would make a host drive
    if (rest.find(':') != std::string::npos) return std::string();
    // "." and ".." parts
    std::string out = best->second;
    size_t s = 0;
    while (s < rest.size()) {
        size_t e = rest.find('\\', s);
        if (e == std::string::npos) e = rest.size();
        std::string part = rest.substr(s, e - s);
        if (part == "..") {
            if (out.size() > best->second.size()) out = Parent(out);
        } else if (!part.empty() && part != ".") {
            out += "\\" + part;
        }
        s = e + 1;
    }
    return out;
}

bool ReadGameFile(const std::string& xbox_path, std::vector<uint8_t>& out) {
    std::string host = ResolvePath(xbox_path, 0);
    FILE* f = host.empty() ? NULL : fopen(host.c_str(), "rb");
    if (!f) return false;
    _fseeki64(f, 0, SEEK_END);
    int64_t n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    out.resize(size_t(n));
    bool ok = n == 0 || fread(&out[0], 1, size_t(n), f) == size_t(n);
    fclose(f);
    return ok;
}

// exports

// JSRF, diagnostico: operacao de arquivo lenta. O console parou 14 s no carregamento e o Xenia nao (2026-11-04).
struct SlowOp {
    const char* what;
    std::string name;
    int64_t t0;
    SlowOp(const char* w, const std::string& n) : what(w), name(n), t0(host::NowNs()) {}
    ~SlowOp() {
        int64_t ms = (host::NowNs() - t0) / 1000000;
        if (ms >= 100) LOGW("arquivo lento: %s %s levou %d ms", what, name.c_str(), int(ms));
    }
};

KERNEL_EXPORT(190, NtCreateFile, 9) {
    uint32_t handle_out = a[0], attrs = a[2], iosb = a[3], disposition = a[7], options = a[8];
    uint32_t root;
    std::string name = ObjectName(attrs, &root);
    if (IsMovie(name)) {
        LOGI("files: movie %s skipped", name.c_str());
        IoStatus(iosb, kObjectNameNotFound, 5);
        return kObjectNameNotFound;
    }
    std::string host = ResolvePath(name, root);
    if (host.empty()) {
        LOGW("NtCreateFile: no mapping for %s", name.c_str());
        IoStatus(iosb, kObjectPathNotFound, 0);
        return kObjectPathNotFound;
    }
    SlowOp slow("abrir", host);
    bool exists = Exists(host);
    bool is_dir = exists && IsDir(host);
    bool want_dir = (options & 1) != 0;
    uint32_t info = 1;  // FILE_OPENED
    if (want_dir || is_dir) {
        if (!exists) {
            if (disposition == 1 || disposition == 4) {
                IoStatus(iosb, kObjectNameNotFound, 5);
                return kObjectNameNotFound;
            }
            MakeDirs(host);
            info = 2;
        } else if (disposition == 2) {
            IoStatus(iosb, kObjectNameCollision, 4);
            return kObjectNameCollision;
        }
        if (!want_dir && (options & 0x40)) {
            IoStatus(iosb, kFileIsADirectory, 0);
            return kFileIsADirectory;
        }
        std::shared_ptr<File> f = std::make_shared<File>();
        f->host = host;
        f->xbox = Normalize(name);
        f->dir = true;
        W32(handle_out, NewHandle(f));
        IoStatus(iosb, kSuccess, info);
        return kSuccess;
    }
    const char* mode = NULL;
    switch (disposition) {
        case 0:  // supersede
        case 5:  // overwrite if
            mode = "w+b";
            info = exists ? 3 : 2;
            break;
        case 1:  // open
            if (!exists) {
                LOG_FIRST(200, "missing %s", name.c_str());
                IoStatus(iosb, kObjectNameNotFound, 5);
                return kObjectNameNotFound;
            }
            mode = "r+b";
            break;
        case 2:  // create
            if (exists) {
                IoStatus(iosb, kObjectNameCollision, 4);
                return kObjectNameCollision;
            }
            mode = "w+b";
            info = 2;
            break;
        case 3:  // open if
            mode = exists ? "r+b" : "w+b";
            info = exists ? 1 : 2;
            break;
        case 4:  // overwrite
            if (!exists) {
                IoStatus(iosb, kObjectNameNotFound, 5);
                return kObjectNameNotFound;
            }
            mode = "w+b";
            info = 3;
            break;
        default:
            return kInvalidParameter;
    }
    // a file in a missing folder fails as on the console: XAPI probes saves with OPEN_ALWAYS
    if (!exists && !IsDir(Parent(host))) {
        IoStatus(iosb, kObjectPathNotFound, 0);
        return kObjectPathNotFound;
    }
    FILE* fp = fopen(host.c_str(), mode);
    if (!fp && std::string(mode) == "r+b") fp = fopen(host.c_str(), "rb");  // read only media
    if (!fp) {
        LOGW("NtCreateFile: cannot open %s", host.c_str());
        IoStatus(iosb, kAccessDenied, 0);
        return kAccessDenied;
    }
    std::shared_ptr<File> f = std::make_shared<File>();
    f->host = host;
    f->xbox = Normalize(name);
    f->fp = fp;
    f->delete_on_close = (options & 0x1000) != 0;  // FILE_DELETE_ON_CLOSE
    W32(handle_out, NewHandle(f));
    IoStatus(iosb, kSuccess, info);
    LOG_FIRST(400, "open %s", name.c_str());
    return kSuccess;
}

KERNEL_EXPORT(202, NtOpenFile, 6) {
    // same as create with FILE_OPEN
    uint32_t handle_out = a[0], attrs = a[2], iosb = a[3];
    uint32_t root;
    std::string name = ObjectName(attrs, &root);
    std::string host = IsMovie(name) ? std::string() : ResolvePath(name, root);
    if (host.empty() || !Exists(host)) {
        LOG_FIRST(200, "missing %s", name.c_str());
        IoStatus(iosb, kObjectNameNotFound, 5);
        return host.empty() ? kObjectPathNotFound : kObjectNameNotFound;
    }
    std::shared_ptr<File> f = std::make_shared<File>();
    f->host = host;
    f->xbox = Normalize(name);
    f->dir = IsDir(host);
    if (!f->dir) {
        f->fp = fopen(host.c_str(), "r+b");
        if (!f->fp) f->fp = fopen(host.c_str(), "rb");
        if (!f->fp) return kAccessDenied;
    }
    W32(handle_out, NewHandle(f));
    IoStatus(iosb, kSuccess, 1);
    return kSuccess;
}

// I/O completes before the call returns: the status block is written, then the file object, the event and the
// APC are signalled (GetOverlappedResult without an event waits on the file handle)
static uint32_t Finish(File& f, uint32_t status, uint32_t n, uint32_t event, uint32_t apc, uint32_t apc_ctx, uint32_t iosb) {
    IoStatus(iosb, status, n);
    SetEvent(f);
    Complete(event, apc, apc_ctx, iosb);
    return status;
}

static uint64_t Offset(File& f, uint32_t off) {
    int64_t o = off ? int64_t(R64(off)) : -1;
    return o >= 0 ? uint64_t(o) : f.pos;
}

// pages is the FILE_SEGMENT_ELEMENT array for scatter and gather, 0 for one buffer. Guest buffers are bytes in
// the window: a transfer copies straight between the file and HostPtr.
static uint32_t Transfer(const Args& a, bool write, uint32_t pages) {
    uint32_t h = a[0], event = a[1], apc = a[2], apc_ctx = a[3], iosb = a[4], buf = a[5], len = a[6], off = a[7];
    std::shared_ptr<File> f = FileFromHandle(h);
    if (!f || !f->fp) return kInvalidHandle;
    SlowOp slow(write ? "escrita" : "leitura", f->host);
    ResetEvent(*f);
    uint32_t n = 0;
    {
        GuestUnlock unlocked;  // o disco do console e lento: as outras threads do jogo seguem
        std::lock_guard<std::mutex> lock(f->io);
        uint64_t pos = Offset(*f, off);
        _fseeki64(f->fp, int64_t(pos), SEEK_SET);
        while (n < len) {
            uint32_t at = pages ? R32(pages + n / 0x1000 * 8) + n % 0x1000 : buf + n;
            uint32_t chunk = pages ? std::min(len - n, 0x1000 - n % 0x1000) : len - n;
            // a buffer may cross the end of a window region only on a corrupt request
            size_t done = write ? fwrite(HostPtr(at), 1, chunk, f->fp) : fread(HostPtr(at), 1, chunk, f->fp);
            n += uint32_t(done);
            if (done < chunk) break;
        }
        f->pos = pos + n;
    }
    if (write && n < len) {
        LOGW("escrita curta em %s: %u de %u bytes", f->host.c_str(), n, len);
        return Finish(*f, 0xC000007F, n, event, apc, apc_ctx, iosb);  // disk full
    }
    if (!write && len && !n) return Finish(*f, kEndOfFile, 0, event, apc, apc_ctx, iosb);
    uint32_t st = Finish(*f, kSuccess, n, event, apc, apc_ctx, iosb);
    if (!write && st == kSuccess && (event || iosb || apc)) return kPending;
    return st;
}

KERNEL_EXPORT(219, NtReadFile, 8) { return Transfer(a, false, 0); }
KERNEL_EXPORT(236, NtWriteFile, 8) { return Transfer(a, true, 0); }
// the buffer argument is an array of one page per element
KERNEL_EXPORT(220, NtReadFileScatter, 8) { return Transfer(a, false, a[5]); }
KERNEL_EXPORT(237, NtWriteFileGather, 8) { return Transfer(a, true, a[5]); }

KERNEL_EXPORT(211, NtQueryInformationFile, 5) {
    uint32_t h = a[0], iosb = a[1], out = a[2], len = a[3], cls = a[4];
    std::shared_ptr<File> f = FileFromHandle(h);
    if (!f) return kInvalidHandle;
    uint64_t size = f->dir ? 0 : FileSize(*f);
    memset(HostPtr(out), 0, len);
    switch (cls) {
        case 4: {  // FileBasicInformation
            HostInfo hi;
            uint64_t t = Stat(f->host, hi) ? hi.time : 0;
            for (int i = 0; i < 4; i++) W64(out + i * 8, t);
            W32(out + 0x20, f->dir ? 0x10 : 0x80);
            IoStatus(iosb, kSuccess, 0x28);
            return kSuccess;
        }
        case 5:  // FileStandardInformation
            W64(out, (size + 0xFFF) & ~0xFFFull);
            W64(out + 8, size);
            W32(out + 0x10, 1);       // NumberOfLinks
            G8(out + 0x15) = f->dir;  // Directory, after DeletePending
            IoStatus(iosb, kSuccess, 0x18);
            return kSuccess;
        case 14:  // FilePositionInformation
            W64(out, f->pos);
            IoStatus(iosb, kSuccess, 8);
            return kSuccess;
        case 34: {  // FileNetworkOpenInformation
            HostInfo hi;
            if (!Stat(f->host, hi)) memset(&hi, 0, sizeof(hi));
            hi.size = size;
            FillNetworkInfo(out, hi);
            IoStatus(iosb, kSuccess, 0x38);
            return kSuccess;
        }
        case 22:  // FileModeInformation
        case 16:
            IoStatus(iosb, kSuccess, 4);
            return kSuccess;
        default:
            LOG_FIRST(8, "NtQueryInformationFile: class %u", cls);
            return kInvalidParameter;
    }
}

KERNEL_EXPORT(226, NtSetInformationFile, 5) {
    uint32_t h = a[0], iosb = a[1], in = a[2], cls = a[4];
    std::shared_ptr<File> f = FileFromHandle(h);
    if (!f) return kInvalidHandle;
    SlowOp slow("ajustar", f->host);
    switch (cls) {
        case 14:  // position
            f->pos = R64(in);
            break;
        case 19:  // allocation: only a size below the end of file cuts it
        case 20:  // end of file
            if (f->fp) {
                std::lock_guard<std::mutex> lock(f->io);
                uint64_t size = FileSize(*f);
                uint64_t want = R64(in);
                if (cls == 20 || want < size) {
                    if (want > size) {  // grow with zeros
                        // de uma vez: byte a byte, um arquivo de 2 MB faz 2 milhoes de chamadas
                        _fseeki64(f->fp, int64_t(want) - 1, SEEK_SET);
                        fputc(0, f->fp);
                    } else if (want < size) {
                        LOG_FIRST(8, "NtSetInformationFile: truncating %s is not supported", f->host.c_str());
                    }
                }
            }
            break;
        case 13:  // disposition
            f->delete_on_close = G8(in) != 0;
            break;
        case 10: {  // rename: ReplaceIfExists, RootDirectory, FileName (ANSI_STRING inline)
            uint32_t root = R32(in + 4);
            std::string target = ReadAnsiString(in + 8);
            std::string host = ResolvePath(target, root);
            if (host.empty()) return kObjectPathNotFound;
            if (f->fp) {
                fclose(f->fp);
                f->fp = NULL;
            }
            if (Exists(host)) DeleteFileA(host.c_str());
            if (!MoveFileA(f->host.c_str(), host.c_str())) {
                LOGW("renomear %s falhou, erro %u", host.c_str(), unsigned(GetLastError()));
                return kAccessDenied;
            }
            f->host = host;
            if (!f->dir) f->fp = fopen(host.c_str(), "r+b");
            break;
        }
        case 4:  // basic, times and attributes are not kept
            break;
        default:
            LOG_FIRST(8, "NtSetInformationFile: class %u", cls);
            return kInvalidParameter;
    }
    IoStatus(iosb, kSuccess, 0);
    return kSuccess;
}

KERNEL_EXPORT(218, NtQueryVolumeInformationFile, 5) {
    uint32_t iosb = a[1], out = a[2], len = a[3], cls = a[4];
    memset(HostPtr(out), 0, len);
    switch (cls) {
        case 3:  // FileFsSizeInformation: 16 GiB total, 8 GiB free in 16 KiB units
            W64(out, 0x100000);
            W64(out + 8, 0x80000);
            W32(out + 0x10, 32);
            W32(out + 0x14, 512);
            IoStatus(iosb, kSuccess, 0x18);
            return kSuccess;
        case 7:  // FileFsFullSizeInformation
            W64(out, 0x100000);
            W64(out + 8, 0x80000);
            W64(out + 0x10, 0x80000);
            W32(out + 0x18, 32);
            W32(out + 0x1C, 512);
            IoStatus(iosb, kSuccess, 0x20);
            return kSuccess;
        case 1:  // FileFsVolumeInformation
            W32(out + 8, 0x12345678);  // VolumeSerialNumber
            IoStatus(iosb, kSuccess, 0x18);
            return kSuccess;
        case 5:  // FileFsAttributeInformation: attributes, max name, name length, "FATX"
            W32(out, 0);
            W32(out + 4, 42);
            W32(out + 8, 4);
            memcpy(HostPtr(out + 12), "FATX", 4);
            IoStatus(iosb, kSuccess, 16);
            return kSuccess;
        default:
            LOG_FIRST(8, "NtQueryVolumeInformationFile: class %u", cls);
            return kInvalidParameter;
    }
}

KERNEL_EXPORT(207, NtQueryDirectoryFile, 10) {
    uint32_t h = a[0], event = a[1], apc = a[2], apc_ctx = a[3], iosb = a[4], out = a[5], len = a[6], cls = a[7],
             mask = a[8], restart = a[9];
    std::shared_ptr<File> f = FileFromHandle(h);
    if (!f || !f->dir) return kInvalidHandle;
    if (cls != 1) {
        LOG_FIRST(8, "NtQueryDirectoryFile: class %u", cls);
        return kInvalidParameter;
    }
    std::string pat = mask ? ReadAnsiString(mask) : "*";
    if (!f->listed || (restart & 0xFF)) {
        f->entries.clear();
        WIN32_FIND_DATAA fd;
        HANDLE fh = FindFirstFileA((f->host + "\\*").c_str(), &fd);
        if (fh != INVALID_HANDLE_VALUE) {
            do {
                std::string n = fd.cFileName;
                if (n == "." || n == ".." || !WildMatch(pat, n)) continue;
                DirEntry e;
                e.name = n;
                e.info.time = (uint64_t(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
                e.info.size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                e.info.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                f->entries.push_back(e);
            } while (FindNextFileA(fh, &fd));
            ::FindClose(fh);
        }
        std::sort(f->entries.begin(), f->entries.end());
        f->next = 0;
        f->listed = true;
    }
    if (f->next >= f->entries.size()) {
        IoStatus(iosb, kNoMoreFiles, 0);
        return f->next == 0 ? kNoSuchFile : kNoMoreFiles;
    }
    const DirEntry& e = f->entries[f->next++];
    uint32_t need = 0x40 + uint32_t(e.name.size());
    if (len < need) return kBufferTooSmall;
    memset(HostPtr(out), 0, need);
    // times, sizes and attributes sit at the same place
    FillNetworkInfo(out + 8, e.info);
    // FILE_DIRECTORY_INFORMATION has EndOfFile before AllocationSize, the reverse of the network info
    W32(out + 0x38, R32(out + 8 + 0x30));
    uint64_t alloc = R64(out + 8 + 0x20), eof = R64(out + 8 + 0x28);
    W64(out + 0x28, eof);
    W64(out + 0x30, alloc);
    W32(out + 0x3C, uint32_t(e.name.size()));
    memcpy(HostPtr(out + 0x40), e.name.data(), e.name.size());
    IoStatus(iosb, kSuccess, need);
    Complete(event, apc, apc_ctx, iosb);
    return kSuccess;
}

KERNEL_EXPORT(210, NtQueryFullAttributesFile, 2) {
    std::string name = ReadAnsiString(R32(a[0] + 4));
    std::string host = IsMovie(name) ? std::string() : ResolvePath(name, R32(a[0]));
    HostInfo hi;
    if (host.empty() || !Stat(host, hi)) {
        LOG_FIRST(200, "missing %s", name.c_str());
        return kObjectNameNotFound;
    }
    FillNetworkInfo(a[1], hi);
    return kSuccess;
}

KERNEL_EXPORT(195, NtDeleteFile, 1) {
    std::string host = ResolvePath(ReadAnsiString(R32(a[0] + 4)), R32(a[0]));
    if (host.empty() || !(IsDir(host) ? RemoveDirectoryA(host.c_str()) : DeleteFileA(host.c_str()))) return kObjectNameNotFound;
    return kSuccess;
}

KERNEL_EXPORT(198, NtFlushBuffersFile, 2) {
    std::shared_ptr<File> f = FileFromHandle(a[0]);
    if (f && f->fp) fflush(f->fp);
    IoStatus(a[1], kSuccess, 0);
    return kSuccess;
}

KERNEL_EXPORT(200, NtFsControlFile, 10) {
    IoStatus(a[4], kSuccess, 0);
    return kSuccess;
}

KERNEL_EXPORT(196, NtDeviceIoControlFile, 10) {
    uint32_t iosb = a[4], code = a[5], out = a[8], out_len = a[9];
    LOG_FIRST(16, "NtDeviceIoControlFile: code %08X", code);
    if (out && out_len) memset(HostPtr(out), 0, out_len);
    if (code == 0x70000 && out_len >= 0x18) {  // IOCTL_DISK_GET_DRIVE_GEOMETRY
        W64(out, 1000);
        W32(out + 8, 12);
        W32(out + 12, 255);
        W32(out + 16, 63);
        W32(out + 20, 512);
    }
    IoStatus(iosb, kSuccess, out_len);
    return kSuccess;
}

KERNEL_EXPORT(67, IoCreateSymbolicLink, 2) {
    CreateSymbolicLink(ReadAnsiString(a[0]), ReadAnsiString(a[1]));
    return kSuccess;
}

KERNEL_EXPORT(69, IoDeleteSymbolicLink, 1) {
    return DeleteSymbolicLink(ReadAnsiString(a[0])) ? kSuccess : kObjectNameNotFound;
}

KERNEL_EXPORT(203, NtOpenSymbolicLinkObject, 2) {
    std::string name = ObjectName(a[1]);
    std::string target = QuerySymbolicLink(name);
    if (target.empty()) return kObjectNameNotFound;
    std::shared_ptr<Link> l = std::make_shared<Link>();
    l->target = target;
    W32(a[0], NewHandle(l));
    return kSuccess;
}

KERNEL_EXPORT(215, NtQuerySymbolicLinkObject, 3) {
    ObjectRef o = FromHandle(a[0]);
    if (!o || o->type != kObjSymLink) return kInvalidHandle;
    const std::string& t = static_cast<Link&>(*o).target;
    uint32_t str = a[1];
    uint16_t maxlen = R16(str + 2);
    if (a[2]) W32(a[2], uint32_t(t.size()));
    if (t.size() > maxlen) return kBufferTooSmall;
    memcpy(HostPtr(R32(str + 4)), t.data(), t.size());
    W16(str, uint16_t(t.size()));
    return kSuccess;
}

}  // namespace kernel
