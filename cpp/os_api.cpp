#include "os_api.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>

#include "literals.h"

namespace lua::platform
{

void os_print_stdout(byte_view utf8)
{
  fwrite(utf8.data(), sizeof(char), utf8.size(), stdout);
}

void os_print_stderr(byte_view utf8)
{
  fwrite(utf8.data(), sizeof(char), utf8.size(), stderr);
}

void os_flush_stdout()
{
  fflush(stdout);
}

void os_flush_stderr()
{
  fflush(stderr);
}

std::optional<byte_view> os_getenv(byte_view name)
{
  std::string n(name.data(), name.size());
  const char* v = getenv(n.c_str());
  if (v == nullptr)
    return std::nullopt;
  byte_view svenv(v);
  return svenv;
}

byte_view os_strerror(int err)
{
  byte_view sverr(strerror(err));
  return sverr;
}

int os_strcoll(byte_view a, byte_view b)
{
  std::string sa(a.data(), a.size());
  std::string sb(b.data(), b.size());
  return strcoll(sa.c_str(), sb.c_str());
}

static_assert(sizeof(time_t) == sizeof(long long), "time_t must hold every Lua time value");

static struct tm totm(const OsDate& d)
{
  struct tm r = {};
  r.tm_year = d.year;
  r.tm_mon = d.month;
  r.tm_mday = d.day;
  r.tm_hour = d.hour;
  r.tm_min = d.min;
  r.tm_sec = d.sec;
  r.tm_yday = d.yday;
  r.tm_wday = d.wday;
  r.tm_isdst = d.isdst;
  r.tm_gmtoff = d.gmtoff;
  r.tm_zone = d.zone.has_value() ? d.zone->data() : nullptr;
  return r;
}

static OsDate fromtm(const struct tm& r)
{
  OsDate d;
  d.year = r.tm_year;
  d.month = r.tm_mon;
  d.day = r.tm_mday;
  d.hour = r.tm_hour;
  d.min = r.tm_min;
  d.sec = r.tm_sec;
  d.yday = r.tm_yday;
  d.wday = r.tm_wday;
  d.isdst = r.tm_isdst;
  d.gmtoff = r.tm_gmtoff;
  d.zone = (r.tm_zone != nullptr) ? std::optional<byte_view>(byte_view(r.tm_zone)) : std::nullopt;
  return d;
}

double os_cpuclock()
{
  return ((double) clock()) / (double) CLOCKS_PER_SEC;
}

long long os_now()
{
  return time(nullptr);
}

std::optional<OsDate> os_gmtime(long long t)
{
  time_t tt = t;
  struct tm r;
  if (gmtime_r(&tt, &r) == nullptr)
    return std::nullopt;
  return fromtm(r);
}

std::optional<OsDate> os_localtime(long long t)
{
  time_t tt = t;
  struct tm r;
  if (localtime_r(&tt, &r) == nullptr)
    return std::nullopt;
  return fromtm(r);
}

std::optional<long long> os_mktime(OsDate* d)
{
  struct tm r = totm(*d);
  r.tm_wday = -1; /* if call succeeds, it will set this value */
  time_t t = mktime(&r);
  *d = fromtm(r);
  if (r.tm_wday == -1) /* error if tm_wday was not set */
    return std::nullopt;
  return t;
}

double os_timediff(long long t1, long long t2)
{
  return difftime(t1, t2);
}

size_t os_strftime(byte_span out, size_t off, byte_view fmt, const OsDate& d)
{
  std::string f(fmt.data(), fmt.size());
  struct tm r = totm(d);
  return strftime(out.data() + off, out.size() - off, f.c_str(), &r);
}

OsResult os_remove(byte_view path)
{
  std::string p(path.data(), path.size());
  errno = 0;
  bool ok = (remove(p.c_str()) == 0);
  return {ok, errno};
}

OsResult os_rename(byte_view from, byte_view to)
{
  std::string f(from.data(), from.size());
  std::string t(to.data(), to.size());
  errno = 0;
  bool ok = (rename(f.c_str(), t.c_str()) == 0);
  return {ok, errno};
}

std::optional<InternalString> os_tmpname()
{
  char buff[] = "/tmp/lua_XXXXXX";
  int fd = mkstemp(buff);
  if (fd == -1)
    return std::nullopt;
  close(fd);
  byte_view svbuff(buff);
  return InternalString(svbuff);
}

bool os_shell_available()
{
  return system(nullptr) != 0;
}

OsSystemResult os_system(byte_view cmd)
{
  std::string c(cmd.data(), cmd.size());
  errno = 0;
  int stat = system(c.c_str());
  return {stat, errno};
}

OsExecStatus os_decode_exec(int stat)
{
  if (WIFEXITED(stat))
    return {true, WEXITSTATUS(stat)};
  if (WIFSIGNALED(stat))
    return {false, WTERMSIG(stat)};
  return {true, stat};
}

OsLib* os_dlopen(byte_view path, bool global)
{
  std::string p(path.data(), path.size());
  return reinterpret_cast<OsLib*>(dlopen(p.c_str(), RTLD_NOW | (global ? RTLD_GLOBAL : RTLD_LOCAL)));
}

void* os_dlsym(OsLib* lib, byte_view sym)
{
  std::string s(sym.data(), sym.size());
  return dlsym(reinterpret_cast<void*>(lib), s.c_str());
}

int (*os_dlfunc(OsLib* lib, byte_view sym))(struct lua_State*)
{
  return reinterpret_cast<int (*)(struct lua_State*)>(os_dlsym(lib, sym));
}

void os_dlclose(OsLib* lib)
{
  dlclose(reinterpret_cast<void*>(lib));
}

InternalString os_dlerror()
{
  const char* e = dlerror();
  byte_view sve(e != nullptr ? byte_view(e) : lt::unknown_dynamic_link_error);
  return InternalString(sve);
}

static FILE* asf(OsFile* f)
{
  return reinterpret_cast<FILE*>(f);
}

static OsFile* asof(FILE* f)
{
  return reinterpret_cast<OsFile*>(f);
}

OsOpenResult os_fopen(byte_view path, byte_view mode)
{
  std::string p(path.data(), path.size());
  std::string m(mode.data(), mode.size());
  errno = 0;
  FILE* f = fopen(p.c_str(), m.c_str());
  return {asof(f), f == nullptr ? errno : 0};
}

OsOpenResult os_popen(byte_view cmd, byte_view mode)
{
  std::string c(cmd.data(), cmd.size());
  std::string m(mode.data(), mode.size());
  errno = 0;
  FILE* f = popen(c.c_str(), m.c_str());
  return {asof(f), f == nullptr ? errno : 0};
}

OsOpenResult os_tmpfile()
{
  errno = 0;
  FILE* f = tmpfile();
  return {asof(f), f == nullptr ? errno : 0};
}

int os_freopen(OsFile* f, byte_view path, byte_view mode)
{
  std::string p(path.data(), path.size());
  std::string m(mode.data(), mode.size());
  errno = 0;
  return (freopen(p.c_str(), m.c_str(), asf(f)) == nullptr) ? errno : 0;
}

OsFile* os_stdin()
{
  return asof(stdin);
}

OsFile* os_stdout()
{
  return asof(stdout);
}

OsFile* os_stderr()
{
  return asof(stderr);
}

size_t os_fread(OsFile* f, byte_span out, size_t off, size_t n)
{
  assert(off + n <= out.size());
  return fread(out.data() + off, 1, n, asf(f));
}

size_t os_fwrite(OsFile* f, byte_view bytes)
{
  return fwrite(bytes.data(), 1, bytes.size(), asf(f));
}

int os_getbyte(OsFile* f)
{
  return getc_unlocked(asf(f));
}

void os_ungetbyte(OsFile* f, int c)
{
  ungetc(c, asf(f));
}

bool os_readline(OsFile* f, byte_span buff)
{
  return fgets(buff.data(), (int) buff.size(), asf(f)) != nullptr;
}

bool os_feof(OsFile* f)
{
  return feof(asf(f)) != 0;
}

bool os_ferror(OsFile* f)
{
  return ferror(asf(f)) != 0;
}

void os_clearerr(OsFile* f)
{
  clearerr(asf(f));
}

int os_fseek(OsFile* f, long long off, int whence)
{
  static const int modes[] = {SEEK_SET, SEEK_CUR, SEEK_END};
  errno = 0;
  return (fseeko(asf(f), (off_t) off, modes[whence]) == 0) ? 0 : errno;
}

long long os_ftell(OsFile* f)
{
  return (long long) ftello(asf(f));
}

int os_setvbuf(OsFile* f, int mode, size_t size)
{
  static const int modes[] = {_IONBF, _IOFBF, _IOLBF};
  errno = 0;
  return (setvbuf(asf(f), nullptr, modes[mode], size) == 0) ? 0 : errno;
}

int os_fflush(OsFile* f)
{
  errno = 0;
  return (fflush(asf(f)) == 0) ? 0 : errno;
}

void os_flushall()
{
  fflush(nullptr);
}

int os_fclose(OsFile* f)
{
  errno = 0;
  return (fclose(asf(f)) == 0) ? 0 : errno;
}

OsSystemResult os_pclose(OsFile* f)
{
  errno = 0;
  int stat = pclose(asf(f));
  return {stat, errno};
}

void os_flockfile(OsFile* f)
{
  flockfile(asf(f));
}

void os_funlockfile(OsFile* f)
{
  funlockfile(asf(f));
}

}
