#include "os_api.h"

#include <cerrno>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>

void os_print_stdout(std::string_view utf8)
{
  fwrite(utf8.data(), sizeof(char), utf8.size(), stdout);
}

void os_print_stderr(std::string_view utf8)
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

std::optional<InternalString> os_getenv(std::string_view name)
{
  std::string n(name);
  const char* v = getenv(n.c_str());
  if (v == nullptr)
    return std::nullopt;
  return InternalString(std::string_view{v});
}

InternalString os_strerror(int err)
{
  return InternalString(std::string_view{strerror(err)});
}

int os_strcoll(std::string_view a, std::string_view b)
{
  std::string sa(a);
  std::string sb(b);
  return strcoll(sa.c_str(), sb.c_str());
}

std::optional<InternalString> os_setlocale(int category,
                                           std::optional<std::string_view> locale)
{
  const char* res;
  if (locale.has_value())
  {
    std::string l(*locale);
    res = setlocale(category, l.c_str());
  }
  else
    res = setlocale(category, nullptr);
  if (res == nullptr)
    return std::nullopt;
  return InternalString(std::string_view{res});
}

OsResult os_remove(std::string_view path)
{
  std::string p(path);
  errno = 0;
  bool ok = (remove(p.c_str()) == 0);
  return {ok, errno};
}

OsResult os_rename(std::string_view from, std::string_view to)
{
  std::string f(from);
  std::string t(to);
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
  return InternalString(std::string_view{buff});
}

bool os_shell_available()
{
  return system(nullptr) != 0;
}

OsSystemResult os_system(std::string_view cmd)
{
  std::string c(cmd);
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

OsLib* os_dlopen(std::string_view path, bool global)
{
  std::string p(path);
  return reinterpret_cast<OsLib*>(dlopen(p.c_str(), RTLD_NOW | (global ? RTLD_GLOBAL : RTLD_LOCAL)));
}

void* os_dlsym(OsLib* lib, std::string_view sym)
{
  std::string s(sym);
  return dlsym(reinterpret_cast<void*>(lib), s.c_str());
}

void os_dlclose(OsLib* lib)
{
  dlclose(reinterpret_cast<void*>(lib));
}

InternalString os_dlerror()
{
  const char* e = dlerror();
  return InternalString(std::string_view{e != nullptr ? e : "unknown dynamic-link error"});
}

static FILE* asf(OsFile* f)
{
  return reinterpret_cast<FILE*>(f);
}

static OsFile* asof(FILE* f)
{
  return reinterpret_cast<OsFile*>(f);
}

OsOpenResult os_fopen(std::string_view path, std::string_view mode)
{
  std::string p(path);
  std::string m(mode);
  errno = 0;
  FILE* f = fopen(p.c_str(), m.c_str());
  return {asof(f), f == nullptr ? errno : 0};
}

OsOpenResult os_popen(std::string_view cmd, std::string_view mode)
{
  std::string c(cmd);
  std::string m(mode);
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

int os_freopen(OsFile* f, std::string_view path, std::string_view mode)
{
  std::string p(path);
  std::string m(mode);
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

size_t os_fread(OsFile* f, std::span<char> out)
{
  return fread(out.data(), 1, out.size(), asf(f));
}

size_t os_fwrite(OsFile* f, std::string_view bytes)
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

bool os_readline(OsFile* f, std::span<char> buff)
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
