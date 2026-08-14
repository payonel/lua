#pragma once

#include <optional>
#include <span>

#include "internal_string.h"

void os_print_stdout(byte_view utf8);
void os_print_stderr(byte_view utf8);
void os_flush_stdout();
void os_flush_stderr();

std::optional<byte_view> os_getenv(byte_view name);
byte_view os_strerror(int err);

int os_strcoll(byte_view a, byte_view b);

struct OsResult
{
  bool ok;
  int err;
};
OsResult os_remove(byte_view path);
OsResult os_rename(byte_view from, byte_view to);
std::optional<InternalString> os_tmpname();

bool os_shell_available();
struct OsSystemResult
{
  int stat;
  int err;
};
OsSystemResult os_system(byte_view cmd);
struct OsExecStatus
{
  bool exited;
  int code;
};
OsExecStatus os_decode_exec(int stat);

struct OsLib;
OsLib* os_dlopen(byte_view path, bool global);
void* os_dlsym(OsLib* lib, byte_view sym);
void os_dlclose(OsLib* lib);
InternalString os_dlerror();

inline constexpr int OS_EOF = -1;

struct OsFile;
struct OsOpenResult
{
  OsFile* f;
  int err;
};
OsOpenResult os_fopen(byte_view path, byte_view mode);
OsOpenResult os_popen(byte_view cmd, byte_view mode);
OsOpenResult os_tmpfile();
int os_freopen(OsFile* f, byte_view path, byte_view mode);
OsFile* os_stdin();
OsFile* os_stdout();
OsFile* os_stderr();
size_t os_fread(OsFile* f, byte_span out);
size_t os_fwrite(OsFile* f, byte_view bytes);
int os_getbyte(OsFile* f);
void os_ungetbyte(OsFile* f, int c);
bool os_readline(OsFile* f, byte_span buff);
bool os_feof(OsFile* f);
bool os_ferror(OsFile* f);
void os_clearerr(OsFile* f);
int os_fseek(OsFile* f, long long off, int whence);
long long os_ftell(OsFile* f);
int os_setvbuf(OsFile* f, int mode, size_t size);
int os_fflush(OsFile* f);
void os_flushall();
int os_fclose(OsFile* f);
OsSystemResult os_pclose(OsFile* f);
void os_flockfile(OsFile* f);
void os_funlockfile(OsFile* f);
