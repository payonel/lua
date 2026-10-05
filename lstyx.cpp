/*
** Styx: persistence of Lua values (see docs/styx.md)
*/

#include <algorithm>
#include <cstring>
#include <functional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "lua.h"

#include "lua_internal.h"
#include "lobject.h"
#include "lstate.h"
#include "ldo.h"
#include "lfunc.h"
#include "lapi.h"
#include "lgc.h"
#include "ltable.h"
#include "lctype.h"
#include "ltm.h"
#include "lzio.h"

#include "internal_string.h"
#include "lauxlib.h"
#include "llimits.h"
#include "lualib.h"
#include "literals.h"

namespace lua::lib
{

/* value tags in the persisted stream */
constexpr char TAG_NIL = 0;
constexpr char TAG_FALSE = 1;
constexpr char TAG_TRUE = 2;
constexpr char TAG_INTEGER = 3;
constexpr char TAG_FLOAT = 4;
constexpr char TAG_STRING = 5;
constexpr char TAG_TABLE = 6;
constexpr char TAG_LCLOSURE = 7;
constexpr char TAG_THREAD = 8;
constexpr char TAG_REF = 9;      /* back reference to an object already written */
constexpr char TAG_PERM = 10;    /* object replaced by its permanent key */
constexpr char TAG_UPVAL_NEW = 11;
constexpr char TAG_UPVAL_REF = 12;
constexpr char TAG_UPVAL_OPEN = 13; /* open upvalue: owner thread, stack level */
constexpr char TAG_SPECIAL = 14;    /* a value rebuilt by a closure (__persist) */
constexpr char TAG_PROTO_NEW = 15;  /* a prototype, written the first time it is met */
constexpr char TAG_PROTO_REF = 16;  /* a prototype already written */
constexpr char TAG_STATE = 17;      /* a whole state: seed, registry, type metatables */
constexpr char TAG_MAINTHREAD = 18; /* the main thread of the state being restored */
constexpr char TAG_UPVAL_PERM = 19; /* a variable that is a permanent: its key */

/*
** The walks keep their own work stacks, so they have no depth limit. For a
** script's persist or unpersist the stacks are memory charged to the state
** (a byte buffer grown under the API lock, as an API function allocates);
** for a host persist they are host memory, so the state is not touched.
*/
template <class T>
class WorkStack
{
  static_assert(std::is_trivially_copyable_v<T>);

public:
  WorkStack(lua_State* L, bool charged) : L_(L), charged_(charged), bytes_(L) {}
  WorkStack(const WorkStack&) = delete;
  WorkStack& operator=(const WorkStack&) = delete;

  size_t size() const { return charged_ ? n_ : host_.size(); }
  bool empty() const { return size() == 0; }

  void push(const T& v)
  {
    if (!charged_)
    {
      host_.push_back(v);
      return;
    }
    if ((n_ + 1) * sizeof(T) > bytes_.capacity())
    {
      size_t cap = std::max<size_t>(16, 2 * (n_ + 1)) * sizeof(T);
      lua_lock(L_);
      bytes_.growTo(cap);
      lua_unlock(L_);
    }
    std::memcpy(bytes_.area().data() + n_ * sizeof(T), &v, sizeof(T));
    n_++;
  }

  T get(size_t i)
  {
    if (!charged_)
      return host_[i];
    T v;
    std::memcpy(&v, bytes_.area().data() + i * sizeof(T), sizeof(T));
    return v;
  }

  void set(size_t i, const T& v)
  {
    if (!charged_)
      host_[i] = v;
    else
      std::memcpy(bytes_.area().data() + i * sizeof(T), &v, sizeof(T));
  }

  T top() { return get(size() - 1); }
  void set_top(const T& v) { set(size() - 1, v); }

  void pop()
  {
    if (!charged_)
      host_.pop_back();
    else
      n_--;
  }

private:
  lua_State* L_;
  bool charged_;
  MBuffer bytes_;
  size_t n_ = 0;
  std::vector<T> host_;
};

/* a value as stored in the walks' records (TValue copies are named) */
struct Slot
{
  Value v{0, nullptr};
  lu_byte tt = LUA_VNIL;
  static Slot of(const TValue& o) { return Slot{o.value_, o.tt_}; }
  TValue get() const { return TValue{v, tt}; }
};

static bool is_name(byte_view s)
{
  if (s.size() == 0 || !lislalpha(s[0] & 0xFF))
    return false;
  for (size_t i = 1; i < s.size(); i++)
    if (!lislalnum(s[i] & 0xFF))
      return false;
  return true;
}

static std::string text(byte_view b)
{
  return std::string(b.data(), b.size());
}

/* 'fmt' with each "%s" or "%d" replaced by the next argument */
static std::string format(byte_view fmt, std::initializer_list<std::string> args)
{
  std::string r;
  auto a = args.begin();
  for (size_t i = 0; i < fmt.size(); i++)
  {
    if (fmt[i] == '%' && i + 1 < fmt.size() && (fmt[i + 1] == 's' || fmt[i + 1] == 'd') &&
        a != args.end())
    {
      r += *a++;
      i++;
    }
    else
      r.push_back(fmt[i]);
  }
  return r;
}

/* the raw value of a string field of a table (nil if absent) */
static TValue string_field(const Table* t, byte_view name)
{
  TValue r{};
  setnilvalue(&r);
  for (int i = 0; i < sizenode(t); i++)
  {
    const Node* n = gnode(t, static_cast<size_t>(i));
    if (!isempty(&n->val) && (n->key_tt & NOVARIANT_MASK) == LUA_TSTRING &&
        static_cast<TString*>(n->key_val.ref)->data.view() == name)
    {
      copyobj(&r, &n->val);
      break;
    }
  }
  return r;
}

/*
** {======================================================
** Persisting: one walk for a script's persist and for the host's. The walk
** reads the objects directly and keeps a stack of work; the path from the
** root to the value at hand is a stack too, turned into text only when an
** error is raised. A script's persist writes to a buffer and keeps its
** references in a table, all charged to the state, and calls '__persist'
** functions; a host persist writes to a writer, keeps everything in host
** memory and runs no Lua code (nothing is allocated in the state and no
** collection starts). Table entries, thread slots and upvalues are read
** when their turn comes, so Lua code run by a '__persist' function cannot
** leave pending work pointing at a collected value.
** =======================================================
*/

struct HostError
{
  std::string message;
};

/* how the walk reached a value, for error paths */
enum class Step : char
{
  None,      /* no step of its own (a part of the value at hand) */
  Root,      /* 'label' */
  Value,     /* table value under 'key' */
  Key,       /* table key 'key' */
  Metatable,
  Upvalue,   /* upvalue 'n' of a closure of 'proto' */
  Stack,     /* thread stack slot 'n' */
  Special,   /* closure returned by __persist */
  PermKey,   /* key of a permanent */
  Hook,      /* a thread's debug hook function */
};

struct PathRec
{
  Step step = Step::None;
  Slot key;
  const Proto* proto = nullptr;
  int n = 0;
  const byte_view* label = nullptr;
};

enum class Op : char
{
  Value,      /* write 'v' */
  PopPath,    /* the value whose step is on top of the path stack is done */
  Byte,       /* write 'i' as one byte */
  I64,        /* write 'i' */
  Raw,        /* write 'i' bytes at 'p' */
  Proto,      /* write prototype 'p' */
  Upvalue,    /* write upvalue 'i' of closure 'p' */
  TableNext,  /* write the entries of table 'p' from position 'i' */
  TableValue, /* write the value at position 'i' of table 'p' */
  ThreadSlot, /* write slots 'i' to 'n' of thread 'p' */
  ThreadTail, /* write the frames, to-be-closed list and hook of thread 'p' */
};

struct Task
{
  Op op = Op::Byte;
  PathRec step;
  Slot v;
  lua_Integer i = 0;
  lua_Integer n = 0;
  const void* p = nullptr;
};

class Walker
{
public:
  /* a script's persist: output to 'B', references in the table at 'refs' */
  Walker(lua_State* L, Table* perms, luaL_Buffer* B, int refs)
      : L_(L), host_(false), perms_(perms), B_(B), refs_(refs), tasks_(L, true), paths_(L, true)
  {
  }

  /* a host persist: output to 'w', nothing allocated in the state */
  Walker(lua_State* L, Table* perms, lua_Writer* w)
      : L_(L), host_(true), perms_(perms), w_(w), tasks_(L, false), paths_(L, false)
  {
  }

  void root(const TValue& v, const byte_view* label)
  {
    Task t;
    t.op = Op::Value;
    t.step.step = Step::Root;
    t.step.label = label;
    t.v = Slot::of(v);
    tasks_.push(t);
    run();
  }

  void byte(char c) { out(&c, 1); }
  void i64(lua_Integer v) { out(&v, sizeof(v)); }
  void raw(const void* p, size_t n) { out(p, n); }

  void finish()
  {
    if (host_)
      flush(true);
  }

private:
  lua_State* L_;
  bool host_;
  Table* perms_;
  luaL_Buffer* B_ = nullptr;
  int refs_ = 0;
  lua_Writer* w_ = nullptr;
  std::string buf_;
  std::unordered_map<const void*, lua_Integer> map_;
  lua_Integer refcount_ = 0;
  WorkStack<Task> tasks_;
  WorkStack<PathRec> paths_;

  static constexpr size_t FLUSH_SIZE = 64 * 1024;

  void out(const void* p, size_t n)
  {
    if (host_)
      buf_.append(static_cast<const char*>(p), n);
    else
      luaL_addlstring(B_, byte_view(static_cast<const char*>(p), n), 0, n);
  }

  void flush(bool end)
  {
    if (!buf_.empty())
    {
      if (w_->write(L_, byte_view(buf_.data(), buf_.size())) != 0)
        throw HostError{text(lt::persist_output_stopped_by_the_writer)};
      buf_.clear();
    }
    if (end && w_->write(L_, std::nullopt) != 0)
      throw HostError{text(lt::persist_output_stopped_by_the_writer)};
  }

  /* ---- work ---- */

  void push_value(const TValue& v, Step step, const TValue* key = nullptr,
                  const Proto* proto = nullptr, int n = 0)
  {
    Task t;
    t.op = Op::Value;
    t.step.step = step;
    if (key != nullptr)
      t.step.key = Slot::of(*key);
    t.step.proto = proto;
    t.step.n = n;
    t.v = Slot::of(v);
    tasks_.push(t);
  }

  void push_op(Op op, lua_Integer i = 0, const void* p = nullptr, lua_Integer n = 0)
  {
    Task t;
    t.op = op;
    t.i = i;
    t.p = p;
    t.n = n;
    tasks_.push(t);
  }

  void push_name(TString* ts)
  {
    TValue v{};
    if (ts != nullptr)
      setsvalue(L_, &v, ts);
    else
      setnilvalue(&v);
    push_value(v, Step::None);
  }

  /* enters the step of a value: it stays on the path until the value is done */
  void enter(const PathRec& step)
  {
    if (step.step == Step::None)
      return;
    paths_.push(step);
    push_op(Op::PopPath);
  }

  void run()
  {
    while (!tasks_.empty())
    {
      Task t = tasks_.top();
      tasks_.pop();
      switch (t.op)
      {
      case Op::Value:
      {
        enter(t.step);
        TValue v = t.v.get();
        value(v);
        break;
      }
      case Op::PopPath:
        paths_.pop();
        break;
      case Op::Byte:
        byte(static_cast<char>(t.i));
        break;
      case Op::I64:
        i64(t.i);
        break;
      case Op::Raw:
        raw(t.p, static_cast<size_t>(t.i));
        break;
      case Op::Proto:
        proto(static_cast<const Proto*>(t.p));
        break;
      case Op::Upvalue:
        enter(t.step);
        upvalue(static_cast<const LClosure*>(t.p), static_cast<int>(t.i));
        break;
      case Op::TableNext:
        table_next(static_cast<Table*>(const_cast<void*>(t.p)), t.i);
        break;
      case Op::TableValue:
      {
        enter(t.step);
        TValue v = table_value(static_cast<Table*>(const_cast<void*>(t.p)), t.i);
        value(v);
        break;
      }
      case Op::ThreadSlot:
        thread_slot(static_cast<lua_State*>(const_cast<void*>(t.p)), t.i, t.n);
        break;
      case Op::ThreadTail:
        thread_tail(static_cast<lua_State*>(const_cast<void*>(t.p)));
        break;
      }
      if (host_ && buf_.size() >= FLUSH_SIZE)
        flush(false);
    }
  }

  /* ---- errors ---- */

  template <class Add>
  void key_text(const TValue& k, Add&& add)
  {
    switch (ttype(&k))
    {
    case LUA_TSTRING:
    {
      byte_view s = tsvalue(&k)->data.view();
      add(format(is_name(s) ? lt::path_field_s : lt::path_string_key_s, {text(s)}));
      return;
    }
    case LUA_TNUMBER:
    {
      byte_inline<LUA_N2SBUFFSZ> b;
      size_t len = luaO_tostringbuff(&k, b);
      add(format(lt::path_index_s, {text(b.view().substr(0, len))}));
      return;
    }
    case LUA_TBOOLEAN:
      add(format(lt::path_index_s, {tagisfalse(k.tt_) ? "false" : "true"}));
      return;
    default:
      add(format(lt::path_object_key_s, {text(ttypename(ttype(&k)))}));
      return;
    }
  }

  template <class Add>
  void path_text(Add&& add)
  {
    for (size_t i = 0; i < paths_.size(); i++)
    {
      PathRec r = paths_.get(i);
      TValue k = r.key.get();
      switch (r.step)
      {
      case Step::None:
        break;
      case Step::Root:
        add(text(*r.label));
        break;
      case Step::Value:
        key_text(k, add);
        break;
      case Step::Key:
        add(format(lt::path_key_s, {text(ttypename(ttype(&k)))}));
        break;
      case Step::Metatable:
        add(text(lt::path_metatable));
        break;
      case Step::Upvalue:
      {
        const TString* name = r.proto->upvalues[static_cast<size_t>(r.n - 1)].name;
        if (name != nullptr && name->data.size() > 0)
          add(format(lt::path_upvalue_s, {text(name->data.view())}));
        else
          add(format(lt::path_upvalue_d, {std::to_string(r.n)}));
        break;
      }
      case Step::Stack:
        add(format(lt::path_stack_d, {std::to_string(r.n)}));
        break;
      case Step::Special:
        add(text(lt::path_special));
        break;
      case Step::PermKey:
        add(text(lt::path_permanent_key));
        break;
      case Step::Hook:
        add(text(lt::path_hook));
        break;
      }
    }
  }

  /* raises 'what' with the path of the value at hand */
  [[noreturn]] void fail(byte_view what)
  {
    if (host_)
    {
      std::string m = text(what) + text(lt::path_at);
      path_text([&](const std::string& s) { m += s; });
      m += text(lt::path_end);
      throw HostError{m};
    }
    luaL_Buffer E;
    luaL_buffinit(L_, &E);
    auto add = [&](const std::string& s) { luaL_addlstring(&E, byte_view(s.data(), s.size()), 0, s.size()); };
    luaL_addlstring(&E, what, 0, what.size());
    add(text(lt::path_at));
    path_text(add);
    add(text(lt::path_end));
    luaL_pushresult(&E);
    lua_error(L_);
    std::abort();
  }

  [[noreturn]] void fail(const std::string& what)
  {
    fail(byte_view(what.data(), what.size()));
  }

  /* ---- references ---- */

  bool ref_find(const TValue& key, const void* id, lua_Integer* found)
  {
    if (host_)
    {
      auto it = map_.find(id);
      if (it == map_.end())
        return false;
      *found = it->second;
      return true;
    }
    TValue rt = getslot(L_, L_->ci->func + refs_);
    TValue r{};
    lu_byte tag = hvalue(&rt)->luaH_get(&key, &r);
    if (tagisempty(tag) || !ttisinteger(&r))
      return false;
    *found = ivalue(&r);
    return true;
  }

  void ref_add(const TValue& key, const void* id)
  {
    lua_Integer n = ++refcount_;
    if (host_)
    {
      map_.emplace(id, n);
      return;
    }
    luaL_checkstacki(L_, 2, std::nullopt);
    setslot(L_, L_->top, &key);
    L_->top++;
    lua_pushinteger(L_, n);
    lua_rawset(L_, refs_);
  }

  /* an internal object (prototype, upvalue) as a reference key */
  static TValue pointer_key(const void* p)
  {
    TValue k{};
    setpvalue(&k, p);
    return k;
  }

  bool perm_key(const TValue& v, TValue* key)
  {
    if (perms_ == nullptr)
      return false;
    lu_byte tag = perms_->luaH_get(&v, key);
    return !tagisempty(tag) && !ttisnil(key);
  }

  /* reference or permanent: true if the object was fully written */
  bool begin_object(const TValue& v)
  {
    const void* id = v.value_.ref;
    lua_Integer found;
    if (ref_find(v, id, &found))
    {
      byte(TAG_REF);
      i64(found);
      return true;
    }
    ref_add(v, id);
    TValue key{};
    if (perm_key(v, &key))
    {
      byte(TAG_PERM);
      byte(static_cast<char>(ttype(&v)));
      push_value(key, Step::PermKey);
      return true;
    }
    return false;
  }

  /* ---- values ---- */

  void value(const TValue& v)
  {
    switch (ttype(&v))
    {
    case LUA_TNIL:
      byte(TAG_NIL);
      return;
    case LUA_TBOOLEAN:
      byte(tagisfalse(v.tt_) ? TAG_FALSE : TAG_TRUE);
      return;
    case LUA_TNUMBER:
      if (ttisinteger(&v))
      {
        byte(TAG_INTEGER);
        i64(ivalue(&v));
      }
      else
      {
        lua_Number n = fltvalue(&v);
        byte(TAG_FLOAT);
        raw(&n, sizeof(n));
      }
      return;
    case LUA_TLIGHTUSERDATA:
      if (!begin_object(v))
        fail(lt::persisting_a_userdata_value_is_not_supported_yet);
      return;
    case LUA_TSTRING:
    case LUA_TTABLE:
    case LUA_TFUNCTION:
    case LUA_TTHREAD:
    case LUA_TUSERDATA:
      break;
    default:
      fail(format(lt::cannot_persist_a_s_value, {text(ttypename(ttype(&v)))}));
    }
    if (begin_object(v))
      return;
    switch (ttype(&v))
    {
    case LUA_TSTRING:
    {
      byte_view s = tsvalue(&v)->data.view();
      byte(TAG_STRING);
      i64(static_cast<lua_Integer>(s.size()));
      raw(s.data(), s.size());
      return;
    }
    case LUA_TUSERDATA:
      fail(lt::persisting_a_userdata_value_is_not_supported_yet);
    case LUA_TTABLE:
      table(hvalue(&v));
      return;
    case LUA_TFUNCTION:
      if (!ttisLclosure(&v))
        fail(lt::cannot_persist_a_C_function);
      closure(clLvalue(&v));
      return;
    default:
      thread(thvalue(&v));
      return;
    }
  }

  /*
  ** The '__persist' field of a table's metatable (read raw): absent or true
  ** persists the table literally, false forbids it, and a function is called
  ** with the table and returns a closure that is persisted in its place
  ** (unpersist calls it to rebuild the table). Returns true if the table was
  ** written this way.
  */
  bool special(Table* h)
  {
    if (h->metatable == nullptr)
      return false;
    TValue f = string_field(h->metatable, lt::mm_persist);
    switch (ttype(&f))
    {
    case LUA_TNIL:
      return false;
    case LUA_TBOOLEAN:
      if (tagisfalse(f.tt_))
        fail(lt::cannot_persist_a_table_its___persist_is_false);
      return false;
    case LUA_TFUNCTION:
      break;
    default:
      fail(lt::mm_persist_must_be_a_boolean_or_a_function);
    }
    if (host_)
      fail(lt::a___persist_function_is_not_supported_yet_in_a_host_persist);
    luaL_checkstacki(L_, 3, std::nullopt);
    setslot(L_, L_->top, &f);
    L_->top++;
    sethvalue2s(h);
    int st = lua_pcall(L_, 1, 1, 0);
    if (st == LUA_ERRMEM)
    {
      lua_lock(L_);
      luaD_throw(L_, LUA_ERRMEM);
    }
    if (st != LUA_OK)
    {
      if (lua_type(L_, -1) == LUA_TSTRING) /* a message gets the path */
        fail(*lua_tolstring(L_, -1));
      lua_error(L_); /* any other value as it is */
    }
    if (!lua_isfunction(L_, -1))
      fail(lt::mm_persist_must_return_a_function);
    byte(TAG_SPECIAL);
    byte(static_cast<char>(LUA_TTABLE));
    TValue c = getslot(L_, L_->top - 1);
    push_value(c, Step::Special);
    lua_pop(L_, 1);
    return true;
  }

  void sethvalue2s(Table* h)
  {
    TValue t{};
    sethvalue(L_, &t, h);
    setslot(L_, L_->top, &t);
    L_->top++;
  }

  void table(Table* h)
  {
    if (special(h))
      return;
    byte(TAG_TABLE);
    i64(h->asize());
    i64(allocsizenode(h));
    i64(h->asize() > 0 ? h->lenhint : 0);
    /* the metatable, then the entries in the order of 'next' */
    push_op(Op::TableNext, 0, h);
    if (h->metatable != nullptr)
    {
      TValue mt{};
      sethvalue(L_, &mt, h->metatable);
      push_value(mt, Step::Metatable);
    }
    else
      push_op(Op::Byte, TAG_NIL);
  }

  /* the next entry from position 'pos' (array part, then hash part) */
  void table_next(Table* h, lua_Integer pos)
  {
    int asize = h->asize();
    for (;; pos++)
    {
      TValue k{};
      if (pos < asize)
      {
        if (tagisempty(getArrTag(h, static_cast<int>(pos))))
          continue;
        setivalue(&k, pos + 1);
      }
      else
      {
        lua_Integer j = pos - asize;
        if (j >= sizenode(h))
        {
          byte(TAG_NIL); /* end of entries */
          return;
        }
        Node* n = gnode(h, static_cast<size_t>(j));
        if (isempty(&n->val))
          continue;
        getnodekey(L_, &k, n);
      }
      push_op(Op::TableNext, pos + 1, h);
      Task tv;
      tv.op = Op::TableValue;
      tv.step.step = Step::Value;
      tv.step.key = Slot::of(k);
      tv.i = pos;
      tv.p = h;
      tasks_.push(tv);
      push_value(k, Step::Key, &k);
      return;
    }
  }

  /* the value at position 'pos', as it is now (nil if it is gone) */
  TValue table_value(Table* h, lua_Integer pos)
  {
    TValue v{};
    setnilvalue(&v);
    int asize = h->asize();
    if (pos < asize)
    {
      lu_byte tag = getArrTag(h, static_cast<int>(pos));
      if (!tagisempty(tag))
        farr2val(h, static_cast<int>(pos), tag, &v);
    }
    else if (pos - asize < sizenode(h))
    {
      Node* n = gnode(h, static_cast<size_t>(pos - asize));
      if (!isempty(&n->val))
        copyobj(&v, &n->val);
    }
    return v;
  }

  void closure(LClosure* cl)
  {
    byte(TAG_LCLOSURE);
    int nup = static_cast<int>(cl->upvals.size());
    for (int i = nup; i >= 1; i--)
    {
      Task t;
      t.op = Op::Upvalue;
      t.step.step = Step::Upvalue;
      t.step.proto = cl->p;
      t.step.n = i;
      t.i = i;
      t.p = cl;
      tasks_.push(t);
    }
    push_op(Op::I64, nup);
    push_op(Op::Proto, 0, cl->p);
  }

  bool is_running(lua_State* T)
  {
    return (!host_ && T == L_) || (T->status == LUA_OK && T->ci != &T->base_ci);
  }

  void upvalue(const LClosure* cl, int i)
  {
    UpVal* uv = cl->upvals[static_cast<size_t>(i - 1)];
    TValue id = pointer_key(uv);
    lua_Integer found;
    if (ref_find(id, uv, &found))
    {
      byte(TAG_UPVAL_REF);
      i64(found);
      return;
    }
    ref_add(id, uv);
    TValue key{};
    if (perm_key(id, &key)) /* the variable is a permanent */
    {
      byte(TAG_UPVAL_PERM);
      push_value(key, Step::PermKey);
      return;
    }
    if (upisopen(uv) && !is_running(uv->owner))
    {
      byte(TAG_UPVAL_OPEN);
      push_op(Op::I64, uv->stkid);
      TValue th{};
      setthvalue(L_, &th, uv->owner);
      push_value(th, Step::None);
      return;
    }
    byte(TAG_UPVAL_NEW);
    push_value(upvalgetval(uv), Step::None);
  }

  /*
  ** A prototype is written once, field by field; later uses refer to it by
  ** id. Its constants and names are values (strings get reference ids like
  ** any other) and its nested prototypes are prototypes too.
  */
  void proto(const Proto* f)
  {
    TValue id = pointer_key(f);
    lua_Integer found;
    if (ref_find(id, f, &found))
    {
      byte(TAG_PROTO_REF);
      i64(found);
      return;
    }
    ref_add(id, f);
    byte(TAG_PROTO_NEW);
    i64(f->linedefined);
    i64(f->lastlinedefined);
    i64(f->numparams);
    byte(static_cast<char>(f->flag & ~PF_FIXED));
    i64(f->maxstacksize);
    i64(static_cast<lua_Integer>(f->code.size()));
    raw(f->code.data(), f->code.size() * sizeof(Instruction));
    i64(static_cast<lua_Integer>(f->k.size()));
    /* the rest, pushed from last to first */
    for (size_t j = f->locvars.size(); j-- > 0;)
    {
      push_op(Op::I64, f->locvars[j].endpc);
      push_op(Op::I64, f->locvars[j].startpc);
      push_name(f->locvars[j].varname);
    }
    push_op(Op::I64, static_cast<lua_Integer>(f->locvars.size()));
    for (size_t j = f->abslineinfo.size(); j-- > 0;)
    {
      push_op(Op::I64, f->abslineinfo[j].line);
      push_op(Op::I64, f->abslineinfo[j].pc);
    }
    push_op(Op::I64, static_cast<lua_Integer>(f->abslineinfo.size()));
    push_op(Op::Raw, static_cast<lua_Integer>(f->lineinfo.size()), f->lineinfo.data());
    push_op(Op::I64, static_cast<lua_Integer>(f->lineinfo.size()));
    push_name(f->source);
    for (size_t j = f->p.size(); j-- > 0;)
      push_op(Op::Proto, 0, f->p[j]);
    push_op(Op::I64, static_cast<lua_Integer>(f->p.size()));
    for (size_t j = f->upvalues.size(); j-- > 0;)
    {
      push_name(f->upvalues[j].name);
      push_op(Op::Byte, f->upvalues[j].kind);
      push_op(Op::I64, f->upvalues[j].idx);
      push_op(Op::Byte, f->upvalues[j].instack);
    }
    push_op(Op::I64, static_cast<lua_Integer>(f->upvalues.size()));
    for (size_t j = f->k.size(); j-- > 0;)
      push_value(f->k[j], Step::None);
  }

  void thread(lua_State* T)
  {
    if (host_ && T == mainthread(L_->l_G))
    {
      byte(TAG_MAINTHREAD);
      return;
    }
    if (is_running(T))
      fail(lt::cannot_persist_a_running_thread);
    StkId maxtop = T->top;
    if (T->ci != &T->base_ci)
    {
      for (CallInfo* ci = T->base_ci.next;; ci = ci->next)
      {
        if (ci->k != nullptr)
          fail(format(lt::cannot_persist_a_thread_with_s, {text(lt::a_continuation_function)}));
        if (ci->callstatus & (CIST_HOOKED | CIST_HOOKYIELD | CIST_FIN | CIST_TBC | CIST_CLSRET))
          fail(format(lt::cannot_persist_a_thread_with_s, {text(lt::a_hook_or_finalizer_call)}));
        if (ci->top > maxtop)
          maxtop = ci->top;
        if (ci == T->ci)
          break;
      }
    }
    byte(TAG_THREAD);
    byte(static_cast<char>(T->status));
    i64(T->top);
    i64(maxtop);
    push_op(Op::ThreadTail, 0, T);
    push_op(Op::ThreadSlot, 0, T, T->top);
  }

  /* slots 'i' to 'n' (a slot no longer in use is written as nil) */
  void thread_slot(lua_State* T, lua_Integer i, lua_Integer n)
  {
    if (i >= n)
      return;
    push_op(Op::ThreadSlot, i + 1, T, n);
    TValue v{};
    if (i < T->top)
    {
      TValue s = getslot(T, static_cast<StkId>(i));
      copyobj(&v, &s);
    }
    else
      setnilvalue(&v);
    push_value(v, Step::Stack, nullptr, nullptr, static_cast<int>(i));
  }

  void thread_tail(lua_State* T)
  {
    lua_Integer nframes = 0;
    if (T->ci != &T->base_ci)
      for (CallInfo* ci = T->base_ci.next;; ci = ci->next)
      {
        nframes++;
        if (ci == T->ci)
          break;
      }
    i64(nframes);
    if (nframes > 0)
      for (CallInfo* ci = T->base_ci.next;; ci = ci->next)
      {
        i64(ci->func);
        i64(ci->top);
        i64(ci->savedpc);
        i64(ci->nextraargs);
        i64(ci->callstatus);
        i64(ci->old_errfunc);
        i64(ci->ctx);
        i64(ci->funcidx);
        i64(ci->nyield);
        i64(ci->nres);
        if (ci == T->ci)
          break;
      }
    i64(T->ntbc);
    for (int k = 0; k < T->ntbc; k++)
      i64(T->tbc[k]);
    hook(T);
  }

  /*
  ** A thread's debug hook, as debug.sethook sets it: the debug library's C
  ** hook with the Lua hook function kept in the registry. Written with its
  ** mask, base count, current count and last traced pc, then the function.
  */
  void hook(lua_State* T)
  {
    if (T->hook == nullptr)
    {
      byte(0);
      return;
    }
    if (T->hook != luaL_debughook())
      fail(lt::a_hook_not_set_by_the_debug_library_is_not_supported_yet);
    byte(1);
    i64(T->hookmask);
    i64(T->basehookcount);
    i64(T->hookcount);
    i64(T->oldpc);
    byte(static_cast<char>(T->allowhook));
    TValue f{};
    setnilvalue(&f);
    if (ttistable(&L_->l_G->l_registry))
    {
      TValue hooks = string_field(hvalue(&L_->l_G->l_registry), lt::_HOOKKEY);
      if (ttistable(&hooks))
      {
        TValue th{};
        setthvalue(L_, &th, T);
        TValue v{};
        lu_byte tag = hvalue(&hooks)->luaH_get(&th, &v);
        if (!tagisempty(tag))
          copyobj(&f, &v);
      }
    }
    push_value(f, Step::Hook);
  }
};

/* }====================================================== */

/*
** {======================================================
** Unpersisting: one reader for a script's unpersist and for the host's.
** A value is built from frames kept on a work stack (charged to the
** state): each frame is an object being built and the part it waits for.
** Every object is anchored in the reference table from the moment it is
** made, so frames hold plain pointers and the Lua stack only ever holds
** the value being handed to the frame below.
** =======================================================
*/

struct Reader
{
  lua_State* L;
  byte_view data;
  size_t pos = 0;

  [[noreturn]] void invalid() const
  {
    lua_pushinteger(L, static_cast<lua_Integer>(pos));
    luaL_errori(L, lt::invalid_persisted_data_at_byte_d);
    std::abort();
  }

  void need(size_t n)
  {
    if (data.size() - pos < n)
      invalid();
  }

  char get_byte()
  {
    need(1);
    return data[pos++];
  }

  void get_raw(void* p, size_t n)
  {
    need(n);
    std::memcpy(p, data.substr(pos, n).data(), n);
    pos += n;
  }

  lua_Integer get_i64()
  {
    lua_Integer v;
    get_raw(&v, sizeof(v));
    return v;
  }

  byte_view get_bytes(size_t n)
  {
    need(n);
    byte_view v = data.substr(pos, n);
    pos += n;
    return v;
  }

  int get_int()
  {
    lua_Integer n = get_i64();
    if (n < INT_MIN || n > INT_MAX)
      invalid();
    return static_cast<int>(n);
  }

  /* a count, at most the number of bytes left (each item takes 'item' bytes) */
  int get_count(size_t item = 1)
  {
    lua_Integer n = get_i64();
    if (n < 0 || n > INT_MAX || static_cast<size_t>(n) > (data.size() - pos) / item)
      invalid();
    return static_cast<int>(n);
  }
};

/*
** A thread being restored is not running, so nothing may raise an error on
** it: its stack and frames are grown in the modes that report failure, work
** that can only raise runs protected on it (as `lua_resume` runs a
** coroutine), and a failure is raised on the calling thread, as
** `coroutine.resume` does when it grows a coroutine's stack.
*/
[[noreturn]] static void memory_error(lua_State* L)
{
  lua_pushistringref(L, lt::not_enough_memory);
  lua_error(L);
  std::abort();
}

/* finds or creates the open upvalue at 'level' of a thread (run protected) */
struct FindUpval : ProtectedCall
{
  StkId level = 0;
  UpVal* uv = nullptr;
  void run(lua_State* L1) override
  {
    uv = UpVal::luaF_findupval(L1, level);
  }
};

/* registers a to-be-closed variable of a thread (run protected) */
struct NewTbc : ProtectedCall
{
  StkId level = 0;
  void run(lua_State* L1) override
  {
    luaF_newtbcupval(L1, level);
  }
};

/*
** A thread is persisted with its status: not started or dead (LUA_OK, no
** frames), suspended (LUA_YIELD) or dead after an error (an error status,
** keeping its frames for tracebacks and its error value).
*/
static bool valid_status(char status)
{
  switch (status)
  {
  case LUA_OK:
  case LUA_YIELD:
  case LUA_ERRRUN:
  case LUA_ERRSYNTAX:
  case LUA_ERRMEM:
  case LUA_ERRERR:
    return true;
  default:
    return false;
  }
}

/* the frame's function slot holds what its call status says it calls */
static bool valid_frame(lua_State* L1, const CallInfo* ci)
{
  TValue f = getslot(L1, ci->func);
  if (ci->callstatus & CIST_C)
    return ttisfunction(&f);
  if (!ttisLclosure(&f))
    return false;
  const Proto* p = clLvalue(&f)->p;
  return ci->savedpc >= 0 && ci->savedpc <= static_cast<int>(p->code.size());
}

/*
** A closure is made as OP_CLOSURE makes one: a new closure on the shared
** prototype, anchored on the stack before its upvalues are created. This
** allocates, so it runs under the API lock, as an API function would.
*/
static void push_new_closure(lua_State* L, Proto* p)
{
  lua_lock(L);
  LClosure* cl = LClosure::luaF_newLclosure(L, static_cast<int>(p->upvalues.size()));
  cl->p = p;
  setslotclL(L, L->top, cl);
  L->top++;
  luaC_objbarrier(L, cl, p);
  cl->luaF_initupvals(L);
  lua_unlock(L);
}

/* what is being built */
enum class Build : char
{
  Table,
  Perm,
  Special,
  Closure,
  Proto,
  Thread,
};

/* the part a frame waits for */
enum class Part : char
{
  Meta,      /* Table: the metatable */
  Key,       /* Table: a key, or nil for the end */
  Val,       /* Table: the value of 'key' */
  End,       /* Table: all entries read */
  Object,    /* Perm: the key; Special: the restore closure */
  Proto,     /* Closure: its prototype */
  Upvals,    /* Closure: upvalue 'i' (raw part) */
  UpNew,     /* Closure: the value of upvalue 'i' */
  UpOpen,    /* Closure: the thread owning upvalue 'i' */
  UpPerm,    /* Closure: the key of upvalue 'i' */
  Consts,    /* Proto: constant 'i' */
  UpDesc,    /* Proto: upvalue description 'i' (raw part) */
  UpName,    /* Proto: the name of upvalue 'i' */
  Nested,    /* Proto: nested prototype 'i' */
  Source,    /* Proto: its source */
  LocVar,    /* Proto: the name of local variable 'i' */
  Slots,     /* Thread: stack slot 'i' */
  HookFn,    /* Thread: its hook function */
};

struct Frame
{
  Build kind = Build::Table;
  Part part = Part::Meta;
  char type = 0;      /* Perm, Special: the expected type; Thread: status */
  int i = 0;          /* the part being read */
  int n = 0;          /* number of parts */
  lua_Integer id = 0; /* reference id */
  lua_Integer a = 0;  /* Table: asize; Thread: top;  hook: mask */
  lua_Integer b = 0;  /* Table: hsize; Thread: maxtop; hook: base count */
  lua_Integer c = 0;  /* Table: length hint; hook: current count */
  lua_Integer d = 0;  /* hook: last traced pc */
  lua_Integer e = 0;  /* hook: allowhook */
  void* p = nullptr;  /* Table*, LClosure* (a Proto's holder, or the closure), lua_State* */
  Slot key;           /* Table: the key waiting for its value */
};

class Restorer
{
public:
  /* stack: 1: permanents (key -> object); 3: refs (id -> object); 4: upvalues */
  Restorer(lua_State* L, byte_view data, bool host) : r_{L, data}, L_(L), host_(host), frames_(L, true) {}

  Reader r_;

  /* reads one value and pushes it */
  void value()
  {
    size_t base = frames_.size();
    Next next = Next::Read;
    for (;;)
    {
      switch (next)
      {
      case Next::Read:
        next = start();
        break;
      case Next::Advance:
        next = advance();
        break;
      case Next::Ready:
        if (frames_.size() == base)
          return;
        next = deliver();
        break;
      }
    }
  }

private:
  enum class Next : char
  {
    Read,    /* read a value */
    Advance, /* the frame on top reads its next part */
    Ready,   /* a value is on top of the stack, for the frame on top */
  };

  static constexpr int PERMS = 1;
  static constexpr int REFS = 3;
  static constexpr int UPS = 4;

  lua_State* L_;
  bool host_;
  lua_Integer refcount_ = 0;
  WorkStack<Frame> frames_;

  void register_top(lua_Integer id)
  {
    lua_pushvalue(L_, -1);
    lua_rawseti(L_, REFS, id);
  }

  void push_table(Table* h)
  {
    TValue t{};
    sethvalue(L_, &t, h);
    setslot(L_, L_->top, &t);
    L_->top++;
  }

  void push_closure(LClosure* cl)
  {
    setslotclL(L_, L_->top, cl);
    L_->top++;
  }

  void push_thread(lua_State* T)
  {
    setslotth(L_, L_->top, T);
    L_->top++;
  }

  /* the frame on top is done and its value is on the stack */
  Next done()
  {
    frames_.pop();
    return Next::Ready;
  }

  Next start()
  {
    luaL_checkstacki(L_, 8, std::nullopt);
    char tag = r_.get_byte();
    switch (tag)
    {
    case TAG_NIL:
      lua_pushnil(L_);
      return Next::Ready;
    case TAG_FALSE:
      lua_pushboolean(L_, 0);
      return Next::Ready;
    case TAG_TRUE:
      lua_pushboolean(L_, 1);
      return Next::Ready;
    case TAG_INTEGER:
      lua_pushinteger(L_, r_.get_i64());
      return Next::Ready;
    case TAG_FLOAT:
    {
      lua_Number n;
      r_.get_raw(&n, sizeof(n));
      lua_pushnumber(L_, n);
      return Next::Ready;
    }
    case TAG_REF:
    {
      lua_Integer id = r_.get_i64();
      lua_rawgeti(L_, REFS, id);
      if (lua_isnil(L_, -1))
        r_.invalid();
      return Next::Ready;
    }
    case TAG_PERM:
    case TAG_SPECIAL:
    {
      Frame f;
      f.kind = (tag == TAG_PERM) ? Build::Perm : Build::Special;
      f.part = Part::Object;
      f.type = r_.get_byte();
      f.id = ++refcount_;
      if (f.kind == Build::Special && f.type != LUA_TTABLE)
        r_.invalid();
      frames_.push(f);
      return Next::Read;
    }
    case TAG_STRING:
    {
      lua_Integer id = ++refcount_;
      lua_Integer n = r_.get_i64();
      if (n < 0)
        r_.invalid();
      byte_view s = r_.get_bytes(static_cast<size_t>(n));
      lua_pushlstring(L_, s, 0, s.size());
      register_top(id);
      return Next::Ready;
    }
    case TAG_TABLE:
      start_table(++refcount_);
      return Next::Read;
    case TAG_LCLOSURE:
    {
      Frame f;
      f.kind = Build::Closure;
      f.part = Part::Proto;
      f.id = ++refcount_;
      frames_.push(f);
      return Next::Advance;
    }
    case TAG_THREAD:
      start_thread(++refcount_);
      return Next::Advance;
    case TAG_MAINTHREAD:
    {
      if (!host_) /* a script never gets the main thread from a stream */
        r_.invalid();
      lua_Integer id = ++refcount_;
      push_thread(mainthread(L_->l_G));
      register_top(id);
      return Next::Ready;
    }
    default:
      r_.invalid();
    }
  }

  /* the value on top of the stack goes to the frame on top */
  Next deliver()
  {
    Frame f = frames_.top();
    switch (f.kind)
    {
    case Build::Table:
      return table_part(f);
    case Build::Perm:
      return perm_part(f);
    case Build::Special:
      return special_part(f);
    case Build::Closure:
      return closure_part(f);
    case Build::Proto:
      return proto_part(f);
    default:
      return thread_part(f);
    }
  }

  /* the frame on top reads its next raw parts */
  Next advance()
  {
    Frame f = frames_.top();
    switch (f.kind)
    {
    case Build::Closure:
      return closure_advance(f);
    case Build::Proto:
      return proto_advance(f);
    case Build::Thread:
      return thread_advance(f);
    default:
      return Next::Read;
    }
  }

  /* ---- tables ---- */

  void start_table(lua_Integer id)
  {
    lua_Integer asize = r_.get_i64();
    lua_Integer hsize = r_.get_i64();
    lua_Integer lenhint = r_.get_i64();
    if (asize < 0 || asize > INT_MAX || hsize < 0 || hsize > INT_MAX ||
        (hsize & (hsize - 1)) != 0 || lenhint < 0 || lenhint > asize)
      r_.invalid();
    /* same sizes as the original (the table limits are checked by
       lua_createtable): entries go where they were, with no rehash */
    lua_createtable(L_, static_cast<int>(asize), static_cast<int>(hsize));
    register_top(id);
    TValue t = getslot(L_, L_->top - 1);
    Frame f;
    f.kind = Build::Table;
    f.part = Part::Meta;
    f.a = asize;
    f.b = hsize;
    f.c = lenhint;
    f.p = hvalue(&t);
    lua_pop(L_, 1);
    frames_.push(f);
  }

  Next table_part(Frame f)
  {
    Table* h = static_cast<Table*>(f.p);
    switch (f.part)
    {
    case Part::Meta:
      if (lua_isnil(L_, -1))
        lua_pop(L_, 1);
      else
      {
        if (!lua_istable(L_, -1))
          r_.invalid();
        push_table(h);
        lua_insert(L_, -2);
        lua_setmetatable(L_, -2);
        lua_pop(L_, 1);
      }
      f.part = Part::Key;
      frames_.set_top(f);
      return Next::Read;
    case Part::Key:
      if (lua_isnil(L_, -1))
      {
        lua_pop(L_, 1);
        if (h->asize() != f.a || allocsizenode(h) != f.b)
          r_.invalid();
        h->lenhint = static_cast<int>(f.c);
        push_table(h);
        return done();
      }
      {
        TValue k = getslot(L_, L_->top - 1);
        f.key = Slot::of(k);
      }
      lua_pop(L_, 1);
      f.part = Part::Val;
      frames_.set_top(f);
      return Next::Read;
    default: /* Part::Val */
    {
      push_table(h);
      TValue k = f.key.get();
      setslot(L_, L_->top, &k);
      L_->top++;
      lua_rotate(L_, -3, 2); /* table, key, value */
      lua_rawset(L_, -3);
      lua_pop(L_, 1);
      f.part = Part::Key;
      frames_.set_top(f);
      return Next::Read;
    }
    }
  }

  /* ---- permanents and __persist ---- */

  Next perm_part(Frame f)
  {
    lua_pushvalue(L_, -1);
    lua_rawget(L_, PERMS);
    if (lua_isnil(L_, -1))
    {
      luaL_tolstring(L_, -2);
      luaL_errori(L_, lt::bad_permanent_value_for_key_s);
    }
    if (lua_type(L_, -1) != f.type)
    {
      lua_pushstring(L_, lua_typename(L_, f.type));
      lua_pushstring(L_, luaL_typename(L_, -2));
      luaL_errori(L_, lt::bad_permanent_value_s_expected_got_s);
    }
    lua_remove(L_, -2);
    register_top(f.id);
    return done();
  }

  /*
  ** A value rebuilt by the closure its '__persist' function returned. Its
  ** reference id was taken before the closure was read, matching persist.
  */
  Next special_part(Frame f)
  {
    if (!lua_isfunction(L_, -1))
      r_.invalid();
    lua_call(L_, 0, 1);
    if (lua_type(L_, -1) != f.type)
    {
      lua_pushstring(L_, luaL_typename(L_, -1));
      lua_pushstring(L_, lua_typename(L_, f.type));
      luaL_errori(L_, lt::mm_persist_restore_returned_s_s_expected);
    }
    register_top(f.id);
    return done();
  }

  /* ---- prototypes ---- */

  /*
  ** A prototype is built field by field, as the engine's loader builds one:
  ** it is anchored first (by a holder closure, registered under its id), and
  ** each part is made valid for the collector before anything that can run
  ** it (reading a value). Allocations run under the API lock.
  */
  Next proto_tag()
  {
    char tag = r_.get_byte();
    if (tag == TAG_PROTO_REF)
    {
      lua_rawgeti(L_, REFS, r_.get_i64());
      if (lua_type(L_, -1) != LUA_TFUNCTION || lua_iscfunction(L_, -1))
        r_.invalid();
      return Next::Ready;
    }
    if (tag != TAG_PROTO_NEW)
      r_.invalid();
    start_proto(++refcount_);
    return Next::Advance;
  }

  void start_proto(lua_Integer pid)
  {
    global_State* g = L_->l_G;
    lua_lock(L_);
    Proto* p = Proto::luaF_newproto(L_);
    LClosure* holder = LClosure::luaF_newLclosure(L_, 0);
    holder->p = p;
    setslotclL(L_, L_->top, holder);
    L_->top++;
    luaC_objbarrier(L_, holder, p);
    lua_unlock(L_);
    register_top(pid);
    lua_pop(L_, 1);

    p->linedefined = r_.get_int();
    p->lastlinedefined = r_.get_int();
    p->numparams = r_.get_int();
    p->flag = static_cast<lu_byte>(r_.get_byte() & ~PF_FIXED);
    p->maxstacksize = r_.get_int();
    if (p->numparams < 0 || p->maxstacksize < 0 || p->numparams > p->maxstacksize)
      r_.invalid();
    int n = r_.get_count(sizeof(Instruction));
    byte_view code = r_.get_bytes(static_cast<size_t>(n) * sizeof(Instruction));
    lua_lock(L_);
    p->code = luaM_alloc_Instruction_span(g, L_, {}, static_cast<size_t>(n), true);
    lua_unlock(L_);
    std::memcpy(p->code.data(), code.data(), code.size());
    n = r_.get_count();
    lua_lock(L_);
    p->k = luaM_alloc_TValue_span(g, L_, {}, static_cast<size_t>(n), true);
    for (TValue& k : p->k)
      setnilvalue(&k);
    lua_unlock(L_);
    Frame f;
    f.kind = Build::Proto;
    f.part = Part::Consts;
    f.n = n;
    f.p = holder;
    frames_.push(f);
  }

  /* a string or nil for a name */
  TString* name_part(GCObject* owner)
  {
    TString* ts = nullptr;
    if (lua_type(L_, -1) == LUA_TSTRING)
    {
      TValue v = getslot(L_, L_->top - 1);
      ts = tsvalue(&v);
      luaC_objbarrier(L_, owner, ts);
    }
    else if (!lua_isnil(L_, -1))
      r_.invalid();
    lua_pop(L_, 1);
    return ts;
  }

  Next proto_advance(Frame f)
  {
    global_State* g = L_->l_G;
    Proto* p = static_cast<LClosure*>(f.p)->p;
    for (;;)
    {
      switch (f.part)
      {
      case Part::Consts:
        if (f.i < f.n)
          return Next::Read;
        f.n = r_.get_count();
        f.i = 0;
        lua_lock(L_);
        p->upvalues = luaM_alloc_Upvaldesc_span(g, L_, {}, static_cast<size_t>(f.n), true);
        for (Upvaldesc& uv : p->upvalues)
          uv = Upvaldesc{nullptr, 0, 0, 0};
        lua_unlock(L_);
        f.part = Part::UpDesc;
        break;
      case Part::UpDesc:
        if (f.i < f.n)
        {
          Upvaldesc& uv = p->upvalues[static_cast<size_t>(f.i)];
          uv.instack = static_cast<lu_byte>(r_.get_byte());
          uv.idx = r_.get_int();
          uv.kind = static_cast<lu_byte>(r_.get_byte());
          f.part = Part::UpName;
          frames_.set_top(f);
          return Next::Read;
        }
        f.n = r_.get_count();
        f.i = 0;
        lua_lock(L_);
        p->p = luaM_alloc_Proto_ptr_span(g, L_, {}, static_cast<size_t>(f.n), true);
        for (Proto*& nested : p->p)
          nested = nullptr;
        lua_unlock(L_);
        f.part = Part::Nested;
        break;
      case Part::Nested:
        frames_.set_top(f);
        if (f.i < f.n)
          return proto_tag();
        f.part = Part::Source;
        frames_.set_top(f);
        return Next::Read;
      default:
        frames_.set_top(f);
        return Next::Read;
      }
    }
  }

  Next proto_part(Frame f)
  {
    global_State* g = L_->l_G;
    LClosure* holder = static_cast<LClosure*>(f.p);
    Proto* p = holder->p;
    switch (f.part)
    {
    case Part::Consts:
    {
      int t = lua_type(L_, -1);
      if (t != LUA_TNIL && t != LUA_TBOOLEAN && t != LUA_TNUMBER && t != LUA_TSTRING)
        r_.invalid();
      TValue v = getslot(L_, L_->top - 1);
      TValue& k = p->k[static_cast<size_t>(f.i)];
      copyobj(&k, &v);
      luaC_barrier(L_, p, &k);
      lua_pop(L_, 1);
      f.i++;
      frames_.set_top(f);
      return Next::Advance;
    }
    case Part::UpName:
      p->upvalues[static_cast<size_t>(f.i)].name = name_part(p);
      f.i++;
      f.part = Part::UpDesc;
      frames_.set_top(f);
      return Next::Advance;
    case Part::Nested:
    {
      TValue h = getslot(L_, L_->top - 1);
      Proto* nested = clLvalue(&h)->p;
      p->p[static_cast<size_t>(f.i)] = nested;
      luaC_objbarrier(L_, p, nested);
      lua_pop(L_, 1);
      f.i++;
      frames_.set_top(f);
      return Next::Advance;
    }
    case Part::Source:
    {
      p->source = name_part(p);
      int n = r_.get_count();
      byte_view lines = r_.get_bytes(static_cast<size_t>(n));
      lua_lock(L_);
      p->lineinfo = luaM_alloc_ls_byte_span(g, L_, {}, static_cast<size_t>(n), true);
      lua_unlock(L_);
      std::memcpy(p->lineinfo.data(), lines.data(), lines.size());
      n = r_.get_count(16);
      lua_lock(L_);
      p->abslineinfo = luaM_alloc_AbsLineInfo_span(g, L_, {}, static_cast<size_t>(n), true);
      lua_unlock(L_);
      for (AbsLineInfo& a : p->abslineinfo)
      {
        a.pc = r_.get_int();
        a.line = r_.get_int();
      }
      f.n = r_.get_count();
      f.i = 0;
      lua_lock(L_);
      p->locvars = luaM_alloc_LocVar_span(g, L_, {}, static_cast<size_t>(f.n), true);
      for (LocVar& lv : p->locvars)
        lv = LocVar{nullptr, 0, 0};
      lua_unlock(L_);
      f.part = Part::LocVar;
      break;
    }
    default: /* Part::LocVar */
    {
      LocVar& lv = p->locvars[static_cast<size_t>(f.i)];
      lv.varname = name_part(p);
      lv.startpc = r_.get_int();
      lv.endpc = r_.get_int();
      f.i++;
      break;
    }
    }
    if (f.i < f.n)
    {
      frames_.set_top(f);
      return Next::Read;
    }
    push_closure(holder); /* the prototype is complete */
    return done();
  }

  /* ---- closures ---- */

  Next closure_advance(Frame f)
  {
    if (f.part == Part::Proto)
      return proto_tag();
    LClosure* cl = static_cast<LClosure*>(f.p);
    while (f.i <= f.n)
    {
      char tag = r_.get_byte();
      if (tag == TAG_UPVAL_NEW || tag == TAG_UPVAL_OPEN || tag == TAG_UPVAL_PERM)
      {
        lua_Integer uid = ++refcount_;
        push_closure(cl);
        lua_rawseti(L_, UPS, uid);
        lua_pushinteger(L_, f.i);
        lua_rawseti(L_, UPS, -uid);
        f.part = (tag == TAG_UPVAL_NEW) ? Part::UpNew : (tag == TAG_UPVAL_OPEN) ? Part::UpOpen : Part::UpPerm;
        frames_.set_top(f);
        return Next::Read;
      }
      if (tag != TAG_UPVAL_REF)
        r_.invalid();
      lua_Integer uid = r_.get_i64();
      lua_rawgeti(L_, UPS, uid);
      lua_rawgeti(L_, UPS, -uid);
      if (!lua_isfunction(L_, -2) || !lua_isinteger(L_, -1))
        r_.invalid();
      push_closure(cl);
      lua_upvaluejoin(L_, -1, f.i, -3, static_cast<int>(lua_tointeger(L_, -2)));
      lua_pop(L_, 3);
      f.i++;
    }
    push_closure(cl);
    return done();
  }

  Next closure_part(Frame f)
  {
    switch (f.part)
    {
    case Part::Proto:
    {
      TValue h = getslot(L_, L_->top - 1);
      push_new_closure(L_, clLvalue(&h)->p);
      lua_remove(L_, -2); /* the prototype's holder */
      register_top(f.id);
      TValue c = getslot(L_, L_->top - 1);
      LClosure* cl = clLvalue(&c);
      lua_pop(L_, 1);
      lua_Integer nup = r_.get_i64();
      if (nup != static_cast<lua_Integer>(cl->upvals.size()))
        r_.invalid();
      f.p = cl;
      f.n = static_cast<int>(nup);
      f.i = 1;
      f.part = Part::Upvals;
      break;
    }
    case Part::UpNew:
    {
      push_closure(static_cast<LClosure*>(f.p));
      lua_insert(L_, -2);
      lua_setupvalue(L_, -2, f.i);
      lua_pop(L_, 1);
      break;
    }
    case Part::UpOpen:
    {
      lua_State* L1 = lua_tothread(L_, -1);
      lua_Integer level = r_.get_i64();
      if (L1 == nullptr || L1 == L_ || (L1->status == LUA_OK && L1->ci != &L1->base_ci) ||
          level < 0 || level >= L1->top)
        r_.invalid();
      FindUpval fu;
      fu.level = level;
      if (luaD_rawrunprotected(L1, &fu) != LUA_OK)
        memory_error(L_);
      LClosure* cl = static_cast<LClosure*>(f.p);
      cl->upvals[static_cast<size_t>(f.i - 1)] = fu.uv;
      luaC_objbarrier(L_, cl, fu.uv);
      lua_pop(L_, 1);
      break;
    }
    default: /* Part::UpPerm */
    {
      /*
      ** A variable that is a permanent: the permanents give, for its key, a
      ** Lua function whose first upvalue is the variable to share.
      */
      lua_pushvalue(L_, -1);
      lua_rawget(L_, PERMS);
      if (lua_isnil(L_, -1))
      {
        luaL_tolstring(L_, -2);
        luaL_errori(L_, lt::bad_permanent_value_for_key_s);
      }
      if (lua_type(L_, -1) != LUA_TFUNCTION || lua_iscfunction(L_, -1) ||
          lua_getupvaluei(L_, -1, 1) == nullptr)
      {
        lua_pushistringref(L_, lt::a_function_capturing_the_variable);
        lua_pushstring(L_, luaL_typename(L_, -2));
        luaL_errori(L_, lt::bad_permanent_value_s_expected_got_s);
      }
      lua_pop(L_, 1); /* the upvalue's value */
      push_closure(static_cast<LClosure*>(f.p));
      lua_upvaluejoin(L_, -1, f.i, -2, 1);
      lua_pop(L_, 3);
      break;
    }
    }
    if (f.part != Part::Upvals)
      f.i++;
    f.part = Part::Upvals;
    frames_.set_top(f);
    return Next::Advance;
  }

  /* ---- threads ---- */

  /*
  ** The thread is valid at every point a collection can see it: its stack is
  ** filled while it has only its base frame, then the frames are read into
  ** spare CallInfos past 'ci' (only an emergency collection, which keeps
  ** them, can run meanwhile) and become part of the thread in one step.
  */
  void start_thread(lua_Integer id)
  {
    lua_State* L1 = lua_newthread(L_);
    register_top(id);
    lua_pop(L_, 1);
    char status = r_.get_byte();
    lua_Integer top = r_.get_i64();
    lua_Integer maxtop = r_.get_i64();
    if (!valid_status(status) || top < 1 || maxtop < top ||
        maxtop >= LUAI_MAXSTACK - LUA_MINSTACK)
      r_.invalid();
    int need = static_cast<int>(maxtop) + LUA_MINSTACK;
    if (!lua_checkstack(L1, need - static_cast<int>(L1->top)))
      memory_error(L_);
    L1->top = top;
    Frame f;
    f.kind = Build::Thread;
    f.part = Part::Slots;
    f.type = status;
    f.a = top;
    f.b = maxtop;
    f.p = L1;
    frames_.push(f);
  }

  Next thread_advance(Frame f)
  {
    lua_State* L1 = static_cast<lua_State*>(f.p);
    if (f.i < f.a)
      return Next::Read;
    lua_Integer nframes = r_.get_i64();
    if (nframes < 0 || (nframes > 0 && f.type == LUA_OK))
      r_.invalid();
    if (nframes > 0)
      read_frames(L1, nframes, f.a, f.b);
    read_tbc(L1, nframes, f.a);
    char kind = r_.get_byte();
    if (kind == 0)
    {
      L1->status = static_cast<TStatus>(f.type);
      push_thread(L1);
      return done();
    }
    if (kind != 1)
      r_.invalid();
    f.a = r_.get_i64();
    f.b = r_.get_int();
    f.c = r_.get_int();
    f.d = r_.get_int();
    f.e = r_.get_byte();
    if ((f.a & ~(LUA_MASKCALL | LUA_MASKRET | LUA_MASKLINE | LUA_MASKCOUNT)) != 0 ||
        f.a == 0 || f.b < 0 || f.c < 0 || (f.e != 0 && f.e != 1))
      r_.invalid();
    f.part = Part::HookFn;
    frames_.set_top(f);
    return Next::Read;
  }

  Next thread_part(Frame f)
  {
    lua_State* L1 = static_cast<lua_State*>(f.p);
    if (f.part == Part::Slots)
    {
      copyslot(L1, static_cast<StkId>(f.i), L_, L_->top - 1);
      lua_pop(L_, 1);
      f.i++;
      frames_.set_top(f);
      return Next::Advance;
    }
    /*
    ** A debug hook: installed as debug.sethook installs it (the Lua function
    ** in the registry's hook table, the debug library's C hook on the
    ** thread), then the count and last traced pc are put back.
    */
    if (!lua_isfunction(L_, -1) && !lua_isnil(L_, -1))
      r_.invalid();
    if (!luaL_getsubtablei(L_, LUA_REGISTRYINDEX, lt::_HOOKKEY))
    { /* as debug.sethook creates it */
      lua_pushistringref(L_, lt::k_char);
      lua_setfieldi(L_, -2, lt::mm_mode);
      lua_pushvalue(L_, -1);
      lua_setmetatable(L_, -2);
    }
    push_thread(L1);
    lua_pushvalue(L_, -3);
    lua_rawset(L_, -3);
    lua_pop(L_, 2);
    lua_sethook(L1, luaL_debughook(), static_cast<int>(f.a), static_cast<int>(f.b));
    L1->hookcount = static_cast<int>(f.c);
    L1->oldpc = static_cast<int>(f.d);
    L1->allowhook = static_cast<lu_byte>(f.e);
    L1->status = static_cast<TStatus>(f.type);
    push_thread(L1);
    return done();
  }

  /* frames are read into spare CallInfos and attached in one step */
  void read_frames(lua_State* L1, lua_Integer nframes, lua_Integer top, lua_Integer maxtop)
  {
    int need = static_cast<int>(maxtop) + LUA_MINSTACK;
    /* a collection may have shrunk the stack while the values were read */
    if (!lua_checkstack(L1, need - static_cast<int>(L1->top)))
      memory_error(L_);
    CallInfo* last = L1->ci;
    for (lua_Integer k = 0; k < nframes; k++)
    {
      CallInfo* ci = last->next;
      if (ci == nullptr)
      {
        L1->ci = last;
        ci = luaE_extendCI(L1, 0);
        L1->ci = &L1->base_ci;
        if (ci == nullptr)
          memory_error(L_);
      }
      ci->func = r_.get_i64();
      ci->top = r_.get_i64();
      ci->savedpc = static_cast<int>(r_.get_i64());
      ci->nextraargs = static_cast<int>(r_.get_i64());
      ci->callstatus = static_cast<l_uint32>(r_.get_i64());
      ci->old_errfunc = r_.get_i64();
      ci->ctx = r_.get_i64();
      ci->funcidx = static_cast<int>(r_.get_i64());
      ci->nyield = static_cast<int>(r_.get_i64());
      ci->nres = static_cast<int>(r_.get_i64());
      ci->k = nullptr;
      ci->trap = 0;
      if (ci->func < 0 || ci->func >= top || ci->top < ci->func || ci->top > maxtop ||
          !valid_frame(L1, ci))
        r_.invalid();
      last = ci;
    }
    L1->ci = last;
  }

  /*
  ** To-be-closed variables are registered once the thread's stack and frames
  ** are complete, with the engine's own luaF_newtbcupval, run protected on
  ** the thread; an error there is raised on the caller.
  */
  void read_tbc(lua_State* L1, lua_Integer nframes, lua_Integer top)
  {
    lua_Integer ntbc = r_.get_i64();
    if (ntbc < 0 || ntbc > top || (ntbc > 0 && nframes == 0))
      r_.invalid();
    StkId prev = 0;
    for (lua_Integer k = 0; k < ntbc; k++)
    {
      lua_Integer level = r_.get_i64();
      if (level <= prev || level >= top)
        r_.invalid();
      prev = level;
      NewTbc nt;
      nt.level = level;
      TStatus st = luaD_rawrunprotected(L1, &nt);
      if (st == LUA_ERRMEM)
        memory_error(L_);
      if (st != LUA_OK)
      {
        lua_xmove(L1, L_, 1);
        lua_error(L_);
      }
    }
  }
};

/* }====================================================== */

/* checks the header and sets up the stack for a Restorer (3: refs; 4: upvalues) */
static void begin_restore(lua_State* L, Restorer& R)
{
  const byte_view header = lt::styx_header;
  if (R.r_.data.size() < header.size() || R.r_.data.substr(0, header.size()) != header)
    R.r_.invalid();
  R.r_.pos = header.size();
  lua_createtable(L, 0, 0);
  lua_createtable(L, 0, 0);
}

/* persist([perms,] value) */
static int styx_persist(lua_State* L)
{
  luaL_checkany(L, 1);
  if (lua_gettop(L) >= 2)
  {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 2);
  }
  else
  {
    lua_createtable(L, 0, 0);
    lua_insert(L, 1);
  }
  lua_createtable(L, 0, 0); /* 3: object -> reference id */
  TValue perms = getslot(L, L->ci->func + 1);
  luaL_Buffer B;
  luaL_buffinit(L, &B);
  luaL_addlstring(&B, lt::styx_header, 0, lt::styx_header.size());
  {
    Walker w(L, hvalue(&perms), &B, 3);
    TValue v = getslot(L, L->ci->func + 2);
    w.root(v, &lt::path_root);
  }
  luaL_pushresult(&B);
  return 1;
}

/* unpersist([perms,] string) */
static int styx_unpersist(lua_State* L)
{
  if (lua_gettop(L) >= 2)
  {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 2);
  }
  else
  {
    lua_createtable(L, 0, 0);
    lua_insert(L, 1);
  }
  Restorer R(L, luaL_checklstring(L, 2), false);
  begin_restore(L, R);
  R.value();
  if (R.r_.pos != R.r_.data.size())
    R.r_.invalid();
  return 1;
}

static int styx_settings(lua_State* L)
{
  return luaL_errori(L, lt::not_implemented);
}

static CFuncInfo cfi_persist(styx_persist);
static CFuncInfo cfi_unpersist(styx_unpersist);
static CFuncInfo cfi_settings(styx_settings);

static const luaL_RegCFI funcs[] = {
  {lt::persist, &cfi_persist},
  {lt::unpersist, &cfi_unpersist},
  {lt::settings, &cfi_settings},
};

/*
** {======================================================
** The host side. A host persist works on a state that is not running: it
** allocates nothing in the state, runs no Lua code and starts no
** collection, so the result is a consistent snapshot, also of a state at
** its memory cap. A host restore runs protected on the idle state; what it
** restores is charged through the state's allocator, and a state's registry
** and type metatables replace the state's own only after the whole stream
** was read, so a failed restore leaves the state as it was.
** =======================================================
*/

/* the value at a stack index of a state that is not running */
static bool value_at(lua_State* L, int idx, TValue* v)
{
  StkId slot = (idx > 0) ? L->ci->func + idx : L->top + idx;
  if (idx == 0 || slot <= L->ci->func || slot >= L->top)
    return false;
  TValue s = getslot(L, slot);
  copyobj(v, &s);
  return true;
}

static int host_persist(lua_State* L, int perms, lua_Writer* w, HostString* msg,
                        const std::function<void(Walker&)>& body)
{
  try
  {
    lua_State* main = mainthread(L->l_G);
    if (main->ci != &main->base_ci || L->ci != &L->base_ci)
      throw HostError{text(lt::the_state_is_running)};
    Table* pt = nullptr;
    if (perms != 0)
    {
      TValue pv{};
      if (!value_at(L, perms, &pv) || !ttistable(&pv))
        throw HostError{text(lt::the_permanents_must_be_a_table)};
      pt = hvalue(&pv);
    }
    Walker hp(L, pt, w);
    hp.raw(lt::styx_header.data(), lt::styx_header.size());
    body(hp);
    hp.finish();
    return LUA_OK;
  }
  catch (HostError& e)
  {
    *msg = HostString::from_bytes(byte_view(e.message.data(), e.message.size()));
    return LUA_ERRRUN;
  }
}

struct HostRestore
{
  byte_view data;
  bool state;
};

/* 1: permanents (key -> object) or nil; 2: the HostRestore */
static int host_restore_body(lua_State* L)
{
  HostRestore* hr = static_cast<HostRestore*>(lua_touserdata(L, 2));
  if (lua_isnil(L, 1))
  {
    lua_createtable(L, 0, 0);
    lua_replace(L, 1);
  }
  Restorer R(L, hr->data, true);
  begin_restore(L, R);
  if (!hr->state)
  {
    if (R.r_.pos < R.r_.data.size() && R.r_.data[R.r_.pos] == TAG_STATE)
      R.r_.invalid();
    R.value();
    if (R.r_.pos != R.r_.data.size())
      R.r_.invalid();
    return 1;
  }
  if (R.r_.get_byte() != TAG_STATE)
    R.r_.invalid();
  R.r_.get_i64(); /* the seed: the host created the state (luaL_persistedseed) */
  R.value();      /* the registry, at 5 */
  if (!lua_istable(L, -1))
    R.r_.invalid();
  for (int t = 0; t < LUA_NUMTYPES; t++) /* the type metatables, at 6... */
  {
    R.value();
    if (!lua_isnil(L, -1) && !lua_istable(L, -1))
      R.r_.invalid();
  }
  if (R.r_.pos != R.r_.data.size())
    R.r_.invalid();
  /* the whole stream is read: the state takes the restored roots */
  global_State* g = L->l_G;
  TValue reg = getslot(L, L->ci->func + 5);
  copyobj(&g->l_registry, &reg);
  for (int t = 0; t < LUA_NUMTYPES; t++)
  {
    TValue mt = getslot(L, L->ci->func + 6 + t);
    g->mt[t] = ttistable(&mt) ? hvalue(&mt) : nullptr;
  }
  return 0;
}

static CFuncInfo cfi_host_restore(host_restore_body);

static int host_restore(lua_State* L, int perms, byte_view data, bool state, HostString* msg)
{
  lua_State* main = mainthread(L->l_G);
  if (main->ci != &main->base_ci || L->ci != &L->base_ci)
  {
    *msg = HostString::from_bytes(lt::the_state_is_running);
    return LUA_ERRRUN;
  }
  int top = lua_gettop(L);
  if (perms < 0)
    perms = top + perms + 1;
  if (perms != 0 && (perms > top || !lua_istable(L, perms)))
  {
    *msg = HostString::from_bytes(lt::the_permanents_must_be_a_table);
    return LUA_ERRRUN;
  }
  if (!lua_checkstack(L, 3))
  {
    *msg = HostString::from_bytes(lt::not_enough_memory);
    return LUA_ERRMEM;
  }
  HostRestore hr{data, state};
  lua_pushcfuncinfo(L, &cfi_host_restore);
  if (perms != 0)
    lua_pushvalue(L, perms);
  else
    lua_pushnil(L);
  lua_pushlightuserdata(L, &hr);
  int st = lua_pcall(L, 2, state ? 0 : 1, 0);
  if (st != LUA_OK)
  {
    auto m = lua_tolstring(L, -1);
    *msg = m ? HostString::from_bytes(*m) : HostString::from_bytes(lt::error_object_is_not_a_string);
    lua_settop(L, top);
  }
  return st;
}

/* }====================================================== */

}

int luaL_persistedseed(byte_view data, unsigned int* seed)
{
  using namespace lua::lib;
  const byte_view header = lt::styx_header;
  size_t need = header.size() + 1 + sizeof(lua_Integer);
  if (data.size() < need || data.substr(0, header.size()) != header ||
      data[header.size()] != TAG_STATE)
    return 0;
  lua_Integer v;
  std::memcpy(&v, data.substr(header.size() + 1, sizeof(v)).data(), sizeof(v));
  *seed = static_cast<unsigned int>(v);
  return 1;
}

int luaL_unpersistvalue(lua_State* L, int perms, byte_view data, HostString* msg)
{
  return lua::lib::host_restore(L, perms, data, false, msg);
}

int luaL_unpersiststate(lua_State* L, int perms, byte_view data, HostString* msg)
{
  return lua::lib::host_restore(L, perms, data, true, msg);
}

int luaL_persistvalue(lua_State* L, int perms, int idx, lua_Writer* w, HostString* msg)
{
  using namespace lua::lib;
  TValue v{};
  if (!value_at(L, idx, &v))
  {
    *msg = HostString::from_bytes(lt::invalid_value_index);
    return LUA_ERRRUN;
  }
  return host_persist(L, perms, w, msg, [&](Walker& hp) { hp.root(v, &lt::path_root); });
}

int luaL_persiststate(lua_State* L, int perms, lua_Writer* w, HostString* msg)
{
  using namespace lua::lib;
  return host_persist(L, perms, w, msg, [&](Walker& hp)
  {
    global_State* g = L->l_G;
    hp.byte(TAG_STATE);
    hp.i64(g->seed);
    hp.root(g->l_registry, &lt::path_registry);
    for (int t = 0; t < LUA_NUMTYPES; t++)
    {
      TValue mt{};
      if (g->mt[t] != nullptr)
        sethvalue(L, &mt, g->mt[t]);
      else
        setnilvalue(&mt);
      hp.root(mt, &lt::path_type_metatable);
    }
  });
}

LUAMOD_API int luaopen_styx(lua_State* L)
{
  luaL_newlibcfi(L, lua::lib::funcs);
  return 1;
}
