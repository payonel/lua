/*
** Styx: persistence of Lua values (see docs/styx.md)
*/

#include <algorithm>
#include <bit>
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
constexpr char TAG_PROTO_NEW = 15;  /* a prototype, written the first time it is met */
constexpr char TAG_PROTO_REF = 16;  /* a prototype already written */
constexpr char TAG_STATE = 17;      /* a whole state: seed, registry, type metatables */
constexpr char TAG_MAINTHREAD = 18; /* the main thread of the state being restored */
constexpr char TAG_UPVAL_PERM = 19; /* a variable that is a permanent: its key */
constexpr char TAG_GROUP = 20;      /* a budget group: its limit, steps left, callback, value and thread */
constexpr char TAG_CFI = 21;        /* a light C function of the engine, by its name */
constexpr char TAG_USEROBJECT = 22; /* a user object of a registered kind: its state, metatable, user values */
constexpr unsigned short GROUP_GONE = 65535; /* an owner id no group has */
constexpr char TAG_CCLOSURE = 23;   /* a C closure of a registered kind: its state and upvalues */
constexpr char TAG_MAINTHREAD_FROZEN = 24; /* the main thread, frozen: its frames, into the restoring state's */

/*
** An object's hash (ADR-30) is written as its place in the sequence of the
** state's hashes (the k-th object made gets seed + k * HASHSTEP), relative
** to the previous object's: objects written one after another were mostly
** made one after another, so that is mostly one byte.
*/
constexpr unsigned int inverse(unsigned int a) /* modulo 2^32, 'a' odd */
{
  unsigned int x = a;
  for (int i = 0; i < 5; i++)
    x *= 2u - a * x;
  return x;
}
constexpr unsigned int HASHSTEP_INV = inverse(HASHSTEP);
static_assert(HASHSTEP * HASHSTEP_INV == 1u);

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

  /* room for 'n' more items, so that pushing them does not allocate */
  void reserve(size_t n)
  {
    if (!charged_)
      host_.reserve(host_.size() + n);
    else if ((n_ + n) * sizeof(T) > bytes_.capacity())
    {
      lua_lock(L_);
      bytes_.growTo((n_ + n) * sizeof(T));
      lua_unlock(L_);
    }
  }

  template <class Less>
  void sort(Less less)
  {
    T* p = charged_ ? reinterpret_cast<T*>(bytes_.area().data()) : host_.data();
    std::sort(p, p + size(), less);
  }

  /* the first position whose item is not less than 'v' (the items are sorted) */
  template <class Less>
  size_t lower_bound(const T& v, Less less)
  {
    size_t lo = 0, hi = size();
    while (lo < hi)
    {
      size_t mid = lo + (hi - lo) / 2;
      if (less(get(mid), v))
        lo = mid + 1;
      else
        hi = mid;
    }
    return lo;
  }

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
** references in a table, all charged to the state, so the collector may
** run, and with it '__gc' metamethods; a host persist writes to a writer,
** keeps everything in host memory and runs no Lua code (nothing is
** allocated in the state and no collection starts). Table entries, thread
** slots and upvalues are read when their turn comes, so Lua code run by a
** '__gc' cannot leave pending work pointing at a collected value.
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
  PermKey,   /* key of a permanent */
  Hook,      /* a thread's debug hook function */
  Group,     /* a thread's budget group, or a group's value */
  Chain,     /* the thread before or after it in a frozen chain */
  Meta,      /* a user object's metatable */
  UserValue, /* a user object's user value 'n' */
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
  Var,        /* write 'i' as a variable-length integer */
  Proto,      /* write prototype 'p' */
  Upvalue,    /* write upvalue 'i' of closure 'p' */
  TableNext,  /* write the entries of table 'p' from position 'i' ('n': array only) */
  NodeNext,   /* write node 'i' on of table 'p', which has 'n' nodes ('v': its id; 'nodes') */
  TableValue, /* write the value at position 'i' of table 'p' */
  ThreadSlot, /* write slots 'i' to 'n' of thread 'p' */
  ThreadTail, /* write the frames, to-be-closed list and hook of thread 'p' */
};

/* NodeNext: the dead keys among the nodes of the current word, and the
   table's dead-node record, plus one (0: none yet) */
struct NodeWalk
{
  lua_Unsigned bits;
  lua_Integer rec;
};

struct Task
{
  Op op = Op::Byte;
  union
  {
    PathRec step{}; /* how the value was reached (a node walk has none) */
    NodeWalk nodes;
  };
  Slot v;
  lua_Integer i = 0;
  lua_Integer n = 0;
  const void* p = nullptr;
};

/*
** A filter of addresses: 'maybe' is true for every address added, and for
** few others (16 bits per address). It never dereferences an address.
*/
class AddressFilter
{
public:
  AddressFilter(lua_State* L, bool charged, size_t n) : words_(L, charged)
  {
    while ((size_t(1) << lgbits_) < 16 * n && lgbits_ < 40)
      lgbits_++;
    size_t nwords = size_t(1) << (lgbits_ - 6);
    words_.reserve(nwords);
    for (size_t i = 0; i < nwords; i++)
      words_.push(0);
  }

  void add(const void* p)
  {
    size_t b = slot(p);
    words_.set(b / 64, words_.get(b / 64) | (lua_Unsigned(1) << (b % 64)));
  }

  bool maybe(const void* p)
  {
    size_t b = slot(p);
    return (words_.get(b / 64) >> (b % 64)) & 1;
  }

private:
  WorkStack<lua_Unsigned> words_;
  int lgbits_ = 6;

  size_t slot(const void* p) const
  {
    lua_Unsigned h = (static_cast<lua_Unsigned>(reinterpret_cast<uintptr_t>(p)) >> 3) * 0x9E3779B97F4A7C15u;
    return static_cast<size_t>(h >> (64 - lgbits_));
  }
};

class Walker
{
public:
  /* a script's persist: output to 'B', references in the table at 'refs' */
  Walker(lua_State* L, Table* perms, luaL_Buffer* B, int refs)
      : L_(L), host_(false), perms_(perms), B_(B), refs_(refs), tasks_(L, true), paths_(L, true),
        marked_(L, true), dead_large_(L, true), dead_(L, true), dead_bits_(L, true)
  {
  }

  /* a host persist: output to 'w', nothing allocated in the state */
  Walker(lua_State* L, Table* perms, lua_Writer* w)
      : L_(L), host_(true), perms_(perms), w_(w), tasks_(L, false), paths_(L, false),
        marked_(L, false), dead_large_(L, false), dead_(L, false), dead_bits_(L, false)
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

  /* an integer in as few bytes as it needs (zigzag, 7 bits per byte) */
  void var(lua_Integer v)
  {
    lua_Unsigned u = (static_cast<lua_Unsigned>(v) << 1) ^ static_cast<lua_Unsigned>(v >> 63);
    char b[10];
    size_t n = 0;
    do
    {
      char c = static_cast<char>(u & 0x7F);
      u >>= 7;
      if (u != 0)
        c = static_cast<char>(c | 0x80);
      b[n++] = c;
    } while (u != 0);
    out(b, n);
  }

  void finish()
  {
    if (host_)
      flush(true);
  }

  /*
  ** The trailer (ADR-10), after the value: what only the whole walk knows.
  ** The tables to mark for finalization, in the order their originals were
  ** marked (ADR-28), and the dead keys that are persisted objects (ADR-29);
  ** then the trailer's length, written backward, so that unpersist reads
  ** it first, from the end.
  */
  void trailer()
  {
    lua_Integer start = position();
    marks();
    dead_keys();
    lua_Unsigned len = static_cast<lua_Unsigned>(position() - start);
    char b[10];
    size_t n = 0;
    do
    {
      char c = static_cast<char>(len & 0x7F);
      len >>= 7;
      if (len != 0)
        c = static_cast<char>(c | 0x80);
      b[n++] = c;
    } while (len != 0);
    for (size_t i = n; i > 0; i--)
      byte(b[i - 1]);
  }

  /*
  ** A state's objects marked for finalization, garbage or not: they are part
  ** of the state until their finalizers run (ADR-28). Permanents are the
  ** restoring host's own objects.
  */
  /* a whole state is persisted (the kernel's functions are part of it) */
  void whole() { whole_ = true; }

  void marked_objects()
  {
    std::vector<GCObject*> list; /* host memory: a state persist is a host's */
    each_marked([&](GCObject* o, bool)
    {
      TValue v, key;
      setgcovalue(L_, &v, o);
      if (!perm_key(v, &key))
        list.push_back(o);
    });
    var(static_cast<lua_Integer>(list.size()));
    for (GCObject* o : list)
    {
      TValue v{};
      setgcovalue(L_, &v, o);
      root(v, &lt::path_finalizer_list);
    }
  }

  /* the thread the state is persisted from, which luaL_restorestate gives
     back restored: the main thread, or a thread the state holds (the
     host's other thread, for a frozen main thread) */
  void persisting_thread()
  {
    TValue v{};
    setthvalue(L_, &v, L_);
    lua_Integer found = 0;
    if (L_ != mainthread(L_->l_G) && !ref_find(v, gcvalue(&v), &found))
      throw HostError{text(lt::persist_from_the_main_thread_or_a_thread_the_state_holds)}; /* (no path: not a value's) */
    root(v, &lt::path_persisting_thread);
  }

  /* the collector's settings that a script sees: its mode, whether it is
     stopped, and its parameters (as stored, so they read back the same) */
  void collector()
  {
    global_State* g = L_->l_G;
    byte(g->gckind == KGC_INC ? 0 : 1);
    byte((g->gcstp & GCSTPUSR) ? 1 : 0);
    for (int p = 0; p < LUA_GCPN; p++)
      byte(static_cast<char>(g->gcparams[p]));
  }

  /* the hash the state's next object gets (ADR-30) */
  void next_hash()
  {
    raw(&L_->l_G->nexthash, sizeof(L_->l_G->nexthash));
  }

  bool check_ = false; /* (only checks what it could write, while the kernel runs) */
private:
  lua_State* L_;
  bool host_;
  Table* perms_;
  bool whole_ = false; /* a whole state, not a value */
  luaL_Buffer* B_ = nullptr;
  int refs_ = 0;
  lua_Writer* w_ = nullptr;
  std::string buf_;
  struct HostRef
  {
    lua_Integer id;
    lu_byte tt;
  };
  std::unordered_map<const void*, HostRef> map_;
  lua_Integer refcount_ = 0;
  lua_Integer flushed_ = 0; /* bytes handed to the writer */
  unsigned int lasthash_ = 0; /* the place of the last hash written */
  WorkStack<Task> tasks_;
  WorkStack<PathRec> paths_;

  /* a table written as such that is marked for finalization */
  struct Marked
  {
    const GCObject* o;
    lua_Integer id;
  };
  WorkStack<Marked> marked_;

  /* the nodes of a table written slot by slot that were dead keys, as bits:
     a table of up to 64 nodes has them in its record, a larger one in 'n'
     bits of 'dead_bits_' from word 'bits' */
  struct DeadNodesLarge
  {
    const Table* h;
    lua_Integer n;
    lua_Integer bits;
  };
  struct DeadNodes
  {
    const Table* h;
    lua_Unsigned bits;
  };
  WorkStack<DeadNodesLarge> dead_large_;
  WorkStack<DeadNodes> dead_;
  WorkStack<lua_Unsigned> dead_bits_;

  /* a persisted object, by address */
  struct ObjectRef
  {
    const void* p;
    lua_Integer id;
  };

  /* a dead key that is a persisted object */
  struct DeadFix
  {
    lua_Integer table;
    lua_Integer node;
    lua_Integer object;
  };

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
      flushed_ += static_cast<lua_Integer>(buf_.size());
      buf_.clear();
    }
    if (end && w_->write(L_, std::nullopt) != 0)
      throw HostError{text(lt::persist_output_stopped_by_the_writer)};
  }

  /* ---- the trailer ---- */

  /* bytes written so far */
  lua_Integer position() const
  {
    size_t n = host_ ? buf_.size() : luaL_bufflen(B_);
    return flushed_ + static_cast<lua_Integer>(n);
  }

  /* every object the collector would finalize, in the order it would call
     their finalizers: those waiting for it, then the marked, newest first */
  template <class F>
  void each_marked(F f)
  {
    global_State* g = L_->l_G;
    for (GCObject* o = g->tobefnz.next; o != nullptr; o = o->next)
      f(o, true);
    for (const BudgetGroup* bg : g->groupbyid) /* (and the ones due in a group) */
      if (bg != nullptr)
        for (GCObject* o = bg->fnz.next; o != nullptr; o = o->next)
          f(o, true);
    for (GCObject* o = g->finobj.next; o != nullptr; o = o->next)
      f(o, false);
  }

  /*
  ** Who a marked object belongs to, in the trailer: 0 for the kernel and the
  ** host, 1 for a group that is gone (the object is then an ordinary one
  ** when it is collected, as in the persisting state), and 2 plus the
  ** reference id of its group.
  */
  lua_Integer owner_code(const GCObject* o)
  {
    if (o->owner == 0)
      return 0;
    global_State* g = L_->l_G;
    BudgetGroup* bg = (o->owner < g->groupbyid.size()) ? g->groupbyid[o->owner] : nullptr;
    if (bg == nullptr)
      return 1;
    TValue v{};
    setgcovalue(L_, &v, bg);
    lua_Integer found;
    if (!ref_find(v, bg, &found))
      return 1; /* (a group nothing persisted refers to is gone, as far as the restored state can tell) */
    return 2 + found;
  }

  /* the persisted tables marked for finalization, oldest mark first */
  void marks()
  {
    size_t n = marked_.size();
    if (n == 1 && !tofinalize(marked_.get(0).o)) /* finalized meanwhile */
      n = 0;
    if (n <= 1)
    {
      var(static_cast<lua_Integer>(n));
      if (n == 1)
      {
        var(marked_.get(0).o->tt == LUA_VTABLE ? 0 : 1);
        var(marked_.get(0).id);
        var(owner_code(marked_.get(0).o));
        bool due = false;
        each_marked([&](GCObject* o, bool d) { due = due || (o == marked_.get(0).o && d); });
        var(due ? 1 : 0);
      }
      return;
    }
    /* their order is the collector's: nothing below allocates while its
       lists are read */
    auto by_object = [](const Marked& a, const Marked& b) { return std::less<const GCObject*>()(a.o, b.o); };
    marked_.sort(by_object);
    WorkStack<lua_Integer> order(L_, !host_);
    WorkStack<lua_Integer> kinds(L_, !host_);
    WorkStack<lua_Integer> owners(L_, !host_);
    WorkStack<lua_Integer> dues(L_, !host_);
    order.reserve(n);
    kinds.reserve(n);
    owners.reserve(n);
    dues.reserve(n);
    each_marked([&](GCObject* o, bool due)
    {
      size_t i = marked_.lower_bound(Marked{o, 0}, by_object);
      if (i < marked_.size() && marked_.get(i).o == o)
      {
        order.push(marked_.get(i).id);
        kinds.push(o->tt == LUA_VTABLE ? 0 : 1);
        owners.push(owner_code(o));
        dues.push(due ? 1 : 0);
      }
    });
    var(static_cast<lua_Integer>(order.size()));
    for (size_t i = kinds.size(); i > 0; i--)
      var(kinds.get(i - 1));
    for (size_t i = order.size(); i > 0; i--)
      var(order.get(i - 1));
    for (size_t i = owners.size(); i > 0; i--)
      var(owners.get(i - 1));
    for (size_t i = dues.size(); i > 0; i--)
      var(dues.get(i - 1));
  }

  /* every node written as a dead key that still is one: f(key, table, node) */
  template <class F>
  void each_dead_key(F f)
  {
    auto word = [&](const Table* h, lua_Integer w, lua_Unsigned bits)
    {
      for (int b = 0; bits != 0; b++, bits >>= 1)
        if (bits & 1)
        {
          lua_Integer j = w * 64 + b;
          if (j >= sizenode(h)) /* a table resized since it was written */
            return;
          const Node* nd = gnode(h, static_cast<size_t>(j));
          if (keyisdead(nd))
            f(nd->key_val.ref, h, j);
        }
    };
    for (size_t r = 0; r < dead_.size(); r++)
    {
      DeadNodes d = dead_.get(r);
      word(d.h, 0, d.bits);
    }
    for (size_t r = 0; r < dead_large_.size(); r++)
    {
      DeadNodesLarge d = dead_large_.get(r);
      for (lua_Integer w = 0; w < (d.n + 63) / 64; w++)
        word(d.h, w, dead_bits_.get(static_cast<size_t>(d.bits + w)));
    }
  }

  /* every persisted object (permanents included): f(address, id) */
  template <class F>
  void each_object(F f)
  {
    if (host_)
    {
      for (const auto& e : map_)
        if (e.second.tt & BIT_ISCOLLECTABLE)
          f(e.first, e.second.id);
      return;
    }
    TValue rt = getslot(L_, L_->ci->func + refs_);
    const Table* refs = hvalue(&rt);
    for (int i = 0; i < sizenode(refs); i++)
    {
      const Node* nd = gnode(refs, static_cast<size_t>(i));
      if (!isempty(&nd->val) && keyiscollectable(nd) && ttisinteger(&nd->val))
        f(nd->key_val.ref, ivalue(&nd->val));
    }
  }

  /*
  ** The dead keys whose objects are persisted: their identities (ADR-29).
  ** A node counts if it was written as a dead key and still is one (a
  ** '__gc' or a collection may have changed a table since it was
  ** written). Keys are matched by address, so nothing dereferences
  ** them, against candidate objects sorted by address: with fewer keys
  ** than objects, the objects that pass a filter of the keys' addresses;
  ** otherwise every object. A filter of the candidates picks the keys to
  ** look up.
  */
  void dead_keys()
  {
    size_t nkeys = 0; /* written as dead keys (some may be no longer) */
    for (size_t r = 0; r < dead_.size(); r++)
      nkeys += static_cast<size_t>(std::popcount(dead_.get(r).bits));
    for (size_t w = 0; w < dead_bits_.size(); w++)
      nkeys += static_cast<size_t>(std::popcount(dead_bits_.get(w)));
    if (nkeys == 0)
    {
      var(0);
      return;
    }
    bool few_keys = nkeys <= static_cast<size_t>(refcount_);
    AddressFilter keys(L_, !host_, few_keys ? nkeys : 0);
    if (few_keys)
      each_dead_key([&](const void* p, const Table*, lua_Integer) { keys.add(p); });
    auto candidate = [&](const void* p) { return !few_keys || keys.maybe(p); };
    size_t ncand = 0;
    each_object([&](const void* p, lua_Integer) { if (candidate(p)) ncand++; });
    if (ncand == 0)
    {
      var(0);
      return;
    }
    WorkStack<ObjectRef> cand(L_, !host_);
    cand.reserve(ncand); /* nothing below allocates while the objects are read */
    each_object([&](const void* p, lua_Integer id) { if (candidate(p)) cand.push(ObjectRef{p, id}); });
    auto by_address = [](const ObjectRef& a, const ObjectRef& b) { return std::less<const void*>()(a.p, b.p); };
    cand.sort(by_address);
    AddressFilter objects(L_, !host_, ncand);
    for (size_t i = 0; i < cand.size(); i++)
      objects.add(cand.get(i).p);
    WorkStack<DeadFix> fixes(L_, !host_);
    each_dead_key([&](const void* p, const Table* h, lua_Integer node)
    {
      if (!objects.maybe(p))
        return;
      size_t k = cand.lower_bound(ObjectRef{p, 0}, by_address);
      if (k < cand.size() && cand.get(k).p == p)
      {
        TValue t{};
        sethvalue(L_, &t, const_cast<Table*>(h));
        lua_Integer table = 0;
        ref_find(t, h, &table);
        fixes.push(DeadFix{table, node, cand.get(k).id});
      }
    });
    var(static_cast<lua_Integer>(fixes.size()));
    for (size_t i = 0; i < fixes.size(); i++)
    {
      DeadFix f = fixes.get(i);
      var(f.table);
      var(f.node);
      var(f.object);
    }
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
      case Op::Var:
        var(t.i);
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
        table_next(static_cast<Table*>(const_cast<void*>(t.p)), t.i, t.n != 0);
        break;
      case Op::NodeNext:
        node_next(t);
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
        const TString* name = (r.proto != nullptr) ? r.proto->upvalues[static_cast<size_t>(r.n - 1)].name : nullptr;
        if (name != nullptr && name->data.size() > 0)
          add(format(lt::path_upvalue_s, {text(name->data.view())}));
        else
          add(format(lt::path_upvalue_d, {std::to_string(r.n)}));
        break;
      }
      case Step::Stack:
        add(format(lt::path_stack_d, {std::to_string(r.n)}));
        break;
      case Step::PermKey:
        add(text(lt::path_permanent_key));
        break;
      case Step::Hook:
        add(text(lt::path_hook));
        break;
      case Step::Group:
        add(text(lt::path_budget_group));
        break;
      case Step::Chain:
        add(text(lt::path_frozen_chain));
        break;
      case Step::Meta:
        add(text(lt::path_metatable));
        break;
      case Step::UserValue:
        add(format(lt::path_user_value_d, {std::to_string(r.n)}));
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
      *found = it->second.id;
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
      map_.emplace(id, HostRef{n, key.tt_});
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
      var(found);
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
        var(ivalue(&v));
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
    case LUA_TBUDGET:
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
      var(static_cast<lua_Integer>(s.size()));
      raw(s.data(), s.size());
      return;
    }
    case LUA_TUSERDATA:
      userobject(uvalue(&v));
      return;
    case LUA_TTABLE:
      table(hvalue(&v));
      return;
    case LUA_TFUNCTION:
      if (ttisLclosure(&v))
        closure(clLvalue(&v));
      else
        cfunction(v);
      return;
    case LUA_TBUDGET:
      group(static_cast<BudgetGroup*>(gcvalue(&v)));
      return;
    default:
      thread(thvalue(&v));
      return;
    }
  }

  /*
  ** A user object of a registered kind: the name of its kind, its
  ** accounting size, its state as integers and bytes, then its metatable and
  ** user values. Any other userdata is for the permanents.
  */
  void userobject(Udata* u)
  {
    lua_Integer id = refcount_; /* the object's, taken just before */
    const char* kind = (u->obj != nullptr) ? u->obj->persistkind() : nullptr;
    if (kind == nullptr)
      fail(lt::persisting_a_userdata_value_is_not_supported_yet);
    if (tofinalize(u))
      marked_.push(Marked{u, id}); /* (its mark and owner, in the trailer) */
    lua_Integer ints[8];
    byte_view bytes;
    int n = u->obj->persistsave(ints, &bytes);
    byte(TAG_USEROBJECT);
    hash(u);
    name(kind);
    var(static_cast<lua_Integer>(u->objsize));
    var(static_cast<lua_Integer>(u->uv.size()));
    var(n);
    for (int i = 0; i < n; i++)
      var(ints[i]);
    var(static_cast<lua_Integer>(bytes.size()));
    raw(bytes.data(), bytes.size());
    bool values = u->obj->persistuservalues();
    for (size_t i = u->uv.size(); i-- > 0;)
    {
      TValue v{};
      if (values)
        v = getobj(&u->uv[i]);
      else
        setnilvalue(&v);
      Task t;
      t.op = Op::Value;
      t.step.step = Step::UserValue;
      t.step.n = static_cast<int>(i + 1);
      t.v = Slot::of(v);
      tasks_.push(t);
    }
    TValue mt{};
    if (u->metatable != nullptr)
      sethvalue(L_, &mt, u->metatable);
    else
      setnilvalue(&mt);
    push_value(mt, Step::Meta);
  }

  /* a name, as a length and its bytes */
  void name(const char* n)
  {
    size_t len = std::strlen(n);
    var(static_cast<lua_Integer>(len));
    raw(n, len);
  }

  /*
  ** A C function that is part of the engine is written by its name (the
  ** permanents of the persisting host come first, in 'begin_object').
  */
  void cfunction(const TValue& v)
  {
    if (ttislcf(&v))
    {
      const char* n = lua_cfiname(static_cast<const CFuncInfo*>(v.value_.ref));
      if (n != nullptr)
      {
        byte(TAG_CFI);
        name(n);
        return;
      }
    }
    if (ttisCclosure(&v))
    {
      const CClosure* cl = clCvalue(&v);
      const lua_CClosureKind* kind = lua_cclosurekind(cl->f);
      if (kind != nullptr && cl->upvalue.size() == static_cast<size_t>(kind->nup))
      {
        lua_Integer ints[8];
        int n = (kind->save != nullptr) ? kind->save(cl, ints) : 0;
        int nvals = (kind->save != nullptr) ? kind->nup - 1 : kind->nup;
        byte(TAG_CCLOSURE);
        hash(cl);
        name(kind->name);
        var(n);
        for (int i = 0; i < n; i++)
          var(ints[i]);
        for (int i = nvals; i-- > 0;)
        {
          TValue u = getobj(&cl->upvalue[static_cast<size_t>(i)]);
          Task t;
          t.op = Op::Value;
          t.step.step = Step::Upvalue;
          t.step.n = i + 1;
          t.step.proto = nullptr;
          t.v = Slot::of(u);
          tasks_.push(t);
        }
        return;
      }
    }
    fail(lt::cannot_persist_a_C_function);
  }

  /*
  ** A budget group: its limit and steps left, the name of its callback (the
  ** debug library's, or one the host named), whether a thaw went on, then
  ** its value and the thread it was set on. The dead objects that wait for
  ** their '__gc' are part of a state's marked objects (the trailer).
  */
  void group(BudgetGroup* bg)
  {
    const char* n = lua_budgetname(bg->f);
    if (n == nullptr)
      fail(lt::cannot_persist_a_budget_callback_that_has_no_name);
    TValue key{};
    if (!whole_ && !ttisnil(&bg->cb) && !perm_key(bg->cb, &key))
      fail(lt::cannot_persist_a_budget_callback_that_is_not_a_permanent); /* (the kernel's own function) */
    byte(TAG_GROUP);
    var(bg->budget);
    global_State* g = L_->l_G;
    var((g->budgetL != nullptr && g->budgetg == bg) ? bg->budget : bg->left); /* (in its callback, as after it) */
    name(n);
    byte(bg->nodrain ? 1 : 0);
    /* the dead objects waiting for its '__gc' (marked due in the trailer):
       read after its value and its root, so pushed before them */
    WorkStack<GCObject*> queue(L_, !host_);
    for (GCObject* o = bg->fnz.next; o != nullptr; o = o->next)
      queue.push(o);
    var(static_cast<lua_Integer>(queue.size()));
    for (size_t i = queue.size(); i > 0; i--)
    {
      TValue v{};
      setgcovalue(L_, &v, queue.get(i - 1));
      push_value(v, Step::Group);
    }
    TValue root{};
    if (bg->root != nullptr)
      setthvalue(L_, &root, bg->root);
    else
      setnilvalue(&root);
    push_value(root, Step::Group);
    push_value(bg->cb, Step::Group);
  }

  /*
  ** The '__persist' field of a table's metatable (read raw): absent or true
  ** persists the table as it is, false forbids it. Nothing else is allowed:
  ** a function would be user code that the persist runs (and its restore
  ** closure, user code that the unpersist runs), outside any budget.
  */
  void special(Table* h)
  {
    if (h->metatable == nullptr)
      return;
    TValue f = string_field(h->metatable, lt::mm_persist);
    if (ttisnil(&f) || (ttisboolean(&f) && !tagisfalse(f.tt_)))
      return;
    if (ttisboolean(&f))
      fail(lt::cannot_persist_a_table_its___persist_is_false);
    fail(lt::mm_persist_must_be_a_boolean);
  }

  void sethvalue2s(Table* h)
  {
    TValue t{};
    sethvalue(L_, &t, h);
    setslot(L_, L_->top, &t);
    L_->top++;
  }

  /* an object's hash, given when it was created: it hashes by it as a key */
  void hash(const GCObject* o)
  {
    unsigned int k = (o->hash - L_->l_G->seed) * HASHSTEP_INV;
    var(static_cast<int>(k - lasthash_));
    lasthash_ = k;
  }

  void table(Table* h)
  {
    lua_Integer id = refcount_; /* the table's, taken just before */
    special(h);
    if (tofinalize(h))
      marked_.push(Marked{h, id});
    /* a hash part is written slot by slot (its layout): keys hash by the
       seed and their content, or by the hash of the object, which is
       written with it, so with the same seed it is restored as it is and
       iterates in the same order (ADR-22, ADR-30) */
    bool layout = allocsizenode(h) > 0;
    byte(TAG_TABLE);
    hash(h);
    var(h->asize());
    var(allocsizenode(h));
    var(h->asize() > 0 ? h->lenhint : 0);
    byte(layout ? 1 : 0);
    /* the metatable, then the entries in the order of 'next' (in layout
       mode the array part's, then the free-slot position and the nodes) */
    if (layout)
    {
      Task t;
      t.op = Op::NodeNext;
      t.nodes = NodeWalk{0, 0};
      t.p = h;
      t.n = sizenode(h);
      t.v.v.prim = id;
      tasks_.push(t);
    }
    push_op(Op::TableNext, 0, h, layout ? 1 : 0);
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
  void table_next(Table* h, lua_Integer pos, bool array_only)
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
        if (array_only || j >= sizenode(h))
        {
          byte(TAG_NIL); /* end of entries */
          return;
        }
        Node* n = gnode(h, static_cast<size_t>(j));
        if (isempty(&n->val))
          continue;
        getnodekey(L_, &k, n);
      }
      push_op(Op::TableNext, pos + 1, h, array_only ? 1 : 0);
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

  /*
  ** The dead keys among the nodes of word 'w' of a task's table: a table
  ** gets its record, with a word for every 64 nodes, at its first dead key.
  */
  void dead_word(Task& t, lua_Integer w)
  {
    if (t.nodes.bits == 0)
      return;
    const Table* h = static_cast<const Table*>(t.p);
    if (t.n <= 64)
      dead_.push(DeadNodes{h, t.nodes.bits});
    else
    {
      if (t.nodes.rec == 0)
      {
        dead_large_.push(DeadNodesLarge{h, t.n, static_cast<lua_Integer>(dead_bits_.size())});
        t.nodes.rec = static_cast<lua_Integer>(dead_large_.size());
        for (lua_Integer k = 0; k < (t.n + 63) / 64; k++)
          dead_bits_.push(0);
      }
      size_t first = static_cast<size_t>(dead_large_.get(static_cast<size_t>(t.nodes.rec - 1)).bits);
      dead_bits_.set(first + static_cast<size_t>(w), t.nodes.bits);
    }
    t.nodes.bits = 0;
  }

  /*
  ** Node 'j' of a table written slot by slot: a state byte (0: no key, 1: a
  ** dead key, 2: a key, then its key and value), then its chain link. The
  ** nodes are read when their turn comes; a table whose hash part changed
  ** meanwhile (by a '__gc') is an error. Dead keys are noted
  ** for the trailer (ADR-29).
  */
  void node_next(Task t)
  {
    Table* h = static_cast<Table*>(const_cast<void*>(t.p));
    lua_Integer j = t.i, n = t.n;
    if (sizenode(h) != n)
      fail(lt::a_table_changed_while_being_persisted);
    if (j == 0)
      var(h->lastfree);
    if (j == n)
    {
      dead_word(t, (j - 1) / 64);
      return;
    }
    if (j % 64 == 0 && j > 0)
      dead_word(t, j / 64 - 1);
    const Node* nd = gnode(h, static_cast<size_t>(j));
    if (keyisdead(nd))
      t.nodes.bits |= lua_Unsigned(1) << (j % 64);
    t.i = j + 1;
    tasks_.push(t); /* the next node, after this one's key and value */
    if (keyisnil(nd) || keyisdead(nd))
    {
      byte(keyisnil(nd) ? 0 : 1);
      var(nd->next);
      return;
    }
    byte(2);
    TValue k{};
    getnodekey(L_, &k, nd);
    push_op(Op::Var, nd->next);
    Task tv;
    tv.op = Op::TableValue;
    tv.step.step = Step::Value;
    tv.step.key = Slot::of(k);
    tv.i = h->asize() + j;
    tv.p = h;
    tasks_.push(tv);
    push_value(k, Step::Key, &k);
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
    hash(cl);
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
    push_op(Op::Var, nup);
    push_op(Op::Proto, 0, cl->p);
  }

  bool is_running(lua_State* T)
  {
    return (!host_ && T == L_) || (T->status == LUA_OK && T->ci != &T->base_ci && T->frozen == 0);
  }

  void upvalue(const LClosure* cl, int i)
  {
    UpVal* uv = cl->upvals[static_cast<size_t>(i - 1)];
    TValue id = pointer_key(uv);
    lua_Integer found;
    if (ref_find(id, uv, &found))
    {
      byte(TAG_UPVAL_REF);
      var(found);
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
      push_op(Op::Var, uv->stkid);
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
      var(found);
      return;
    }
    ref_add(id, f);
    byte(TAG_PROTO_NEW);
    var(f->linedefined);
    var(f->lastlinedefined);
    var(f->numparams);
    byte(static_cast<char>(f->flag & ~PF_FIXED));
    var(f->maxstacksize);
    var(static_cast<lua_Integer>(f->code.size()));
    raw(f->code.data(), f->code.size() * sizeof(Instruction));
    var(static_cast<lua_Integer>(f->k.size()));
    /* the rest, pushed from last to first */
    for (size_t j = f->locvars.size(); j-- > 0;)
    {
      push_op(Op::Var, f->locvars[j].endpc);
      push_op(Op::Var, f->locvars[j].startpc);
      push_name(f->locvars[j].varname);
    }
    push_op(Op::Var, static_cast<lua_Integer>(f->locvars.size()));
    for (size_t j = f->abslineinfo.size(); j-- > 0;)
    {
      push_op(Op::Var, f->abslineinfo[j].line);
      push_op(Op::Var, f->abslineinfo[j].pc);
    }
    push_op(Op::Var, static_cast<lua_Integer>(f->abslineinfo.size()));
    push_op(Op::Raw, static_cast<lua_Integer>(f->lineinfo.size()), f->lineinfo.data());
    push_op(Op::Var, static_cast<lua_Integer>(f->lineinfo.size()));
    push_name(f->source);
    for (size_t j = f->p.size(); j-- > 0;)
      push_op(Op::Proto, 0, f->p[j]);
    push_op(Op::Var, static_cast<lua_Integer>(f->p.size()));
    for (size_t j = f->upvalues.size(); j-- > 0;)
    {
      push_name(f->upvalues[j].name);
      push_op(Op::Byte, f->upvalues[j].kind);
      push_op(Op::Var, f->upvalues[j].idx);
      push_op(Op::Byte, f->upvalues[j].instack);
    }
    push_op(Op::Var, static_cast<lua_Integer>(f->upvalues.size()));
    for (size_t j = f->k.size(); j-- > 0;)
      push_value(f->k[j], Step::None);
  }

  void thread(lua_State* T)
  {
    bool main = T == mainthread(L_->l_G);
    if (main && host_ && T->frozen == 0)
    {
      byte(TAG_MAINTHREAD); /* (the restoring state's own, idle) */
      return;
    }
    if (check_ && is_running(T))
    { /* (a check while the kernel runs: the host persists it at rest) */
      byte(TAG_MAINTHREAD);
      return;
    }
    if (is_running(T))
      fail(lt::cannot_persist_a_running_thread);
    if (main && !host_) /* (the host persists it: idle, by reference; frozen, whole) */
      fail(lt::cannot_persist_the_main_thread_from_a_script);
    StkId top = T->frozen ? T->frozentop : T->top; /* (a frozen thread's own: see 'luaE_setfrozen') */
    StkId maxtop = top;
    if (T->ci != &T->base_ci)
    {
      for (CallInfo* ci = T->base_ci.next;; ci = ci->next)
      {
        if (ci->k != nullptr && lua_kname(ci->k) == nullptr)
          fail(format(lt::cannot_persist_a_thread_with_s, {text(lt::a_continuation_that_has_no_name)}));
        if (ci->top > maxtop)
          maxtop = ci->top;
        if (ci == T->ci)
          break;
      }
    }
    if (main)
      byte(TAG_MAINTHREAD_FROZEN); /* (a program runs on it: its frames go with it) */
    else
    {
      byte(TAG_THREAD);
      hash(T);
    }
    byte(static_cast<char>(T->status));
    var(top);
    var(maxtop);
    push_op(Op::ThreadTail, 0, T);
    push_op(Op::ThreadSlot, 0, T, top);
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
        /* what a freeze keeps in a frame: the continuation (by name), the
           calls it runs with, how an API call goes on, a hook function in
           progress */
        const char* kn = (ci->k != nullptr) ? lua_kname(ci->k) : nullptr;
        byte(kn != nullptr ? 1 : 0);
        if (kn != nullptr)
          name(kn);
        var(ci->nccalls);
        /* (an API call's and a hook's state mean something only while they
           are going on; the rest is stale: not written) */
        bool api = (ci->callstatus & CIST_APIK) != 0;
        bool hooked = (ci->callstatus & CIST_HOOKED) != 0;
        byte(static_cast<char>(api ? ci->apiop : 0));
        var(api ? ci->apifirst : 0);
        byte(static_cast<char>(hooked ? ci->hookev : 0));
        byte(static_cast<char>(hooked ? ci->hookdone : 0));
        var(hooked ? ci->hooktop : 0);
        var(hooked ? ci->hookcitop : 0);
        var(hooked ? ci->hookdelta : 0);
        var(hooked ? ci->hookres : 0);
        if (ci == T->ci)
          break;
      }
    i64(T->ntbc);
    for (int k = 0; k < T->ntbc; k++)
      i64(T->tbc[k]);
    /* the extras (after the hook, whose function is a value): the state of a
       frozen thread, then its budget groups and its places in a chain */
    push_thread_values(T);
    thread_extras(T);
    hook(T);
  }

  /* the values after a thread's extras, pushed to be written in this order:
     its own group, its group in effect, the thread that resumed it, the
     thread it resumed (frozen) */
  void push_thread_values(lua_State* T)
  {
    auto thread_or_nil = [&](lua_State* th)
    {
      TValue v{};
      if (th != nullptr)
        setthvalue(L_, &v, th);
      else
        setnilvalue(&v);
      push_value(v, Step::Chain);
    };
    auto group_or_nil = [&](BudgetGroup* bg)
    {
      TValue v{};
      if (bg != nullptr)
        setgcovalue(L_, &v, bg);
      else
        setnilvalue(&v);
      push_value(v, Step::Group);
    };
    thread_or_nil(T->frozenchild);
    thread_or_nil(T->frozenparent);
    group_or_nil(T->beff);
    group_or_nil(T->bown);
  }

  /* the raw state of a frozen thread (written before the debug hook) */
  void thread_extras(lua_State* T)
  {
    byte(static_cast<char>(T->allowhook)); /* (off while a drained '__gc' or a hook function runs) */
    byte(static_cast<char>(T->frozen));
    if (T->frozen == 0)
      return;
    var(T->frozenfunc);
    var(T->frozenerrfunc);
    var(T->frozennres);
    var(T->frozencc);
    var(T->ccbase);
    var(T->nCcalls);
    var(T->errfunc);
    byte(static_cast<char>(T->closestatus));
    byte(T->closeself ? 1 : 0);
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
    i64(T->hookmask & ~LUA_MASKBUDGET); /* (the budget is its group's) */
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

  /* a variable-length integer (see Walker::var) */
  lua_Integer get_var()
  {
    lua_Unsigned u = 0;
    for (int shift = 0;; shift += 7)
    {
      if (shift > 63)
        invalid();
      lua_Unsigned c = static_cast<unsigned char>(get_byte());
      u |= (c & 0x7F) << shift;
      if ((c & 0x80) == 0)
        break;
    }
    return static_cast<lua_Integer>((u >> 1) ^ (0 - (u & 1)));
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
    lua_Integer n = get_var();
    if (n < INT_MIN || n > INT_MAX)
      invalid();
    return static_cast<int>(n);
  }

  /* a count, at most the number of bytes left (each item takes 'item' bytes) */
  int get_count(size_t item = 1)
  {
    lua_Integer n = get_var();
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
  Closure,
  Proto,
  Thread,
  Group,
  Userobject,
  CClosure,
};

/* the part a frame waits for */
enum class Part : char
{
  Meta,      /* Table: the metatable */
  Key,       /* Table: a key, or nil for the end */
  Val,       /* Table: the value of 'key' */
  Nodes,     /* Table: node 'i' (raw part) */
  NodeKey,   /* Table: the key of node 'i' */
  NodeVal,   /* Table: the value of node 'i' */
  End,       /* Table: all entries read */
  Object,    /* Perm: the key */
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
  Bown,      /* Thread: its own budget group */
  Beff,      /* Thread: its group in effect */
  Parent,    /* Thread: the thread that resumed it (frozen) */
  Child,     /* Thread: the thread it resumed (frozen) */
  GroupCb,   /* Group: its value */
  GroupRoot, /* Group: the thread it was set on */
  GroupQueue, /* Group: dead object 'i' waiting for its '__gc' */
  UMeta,     /* Userobject: its metatable */
  UValue,    /* Userobject: user value 'i' */
  CUp,       /* CClosure: upvalue 'i' */
};

struct Frame
{
  Build kind = Build::Table;
  Part part = Part::Meta;
  char type = 0;      /* Perm: the expected type; Thread: status; Table: layout */
  int i = 0;          /* the part being read */
  int n = 0;          /* number of parts */
  lua_Integer id = 0; /* reference id */
  lua_Integer a = 0;  /* Table: asize; Thread: top;  hook: mask */
  lua_Integer b = 0;  /* Table: hsize; Thread: maxtop; hook: base count */
  lua_Integer c = 0;  /* Table: length hint; hook: current count */
  lua_Integer d = 0;  /* hook: last traced pc; Table: lastfree */
  lua_Integer e = 0;  /* hook: allowhook */
  void* p = nullptr;  /* Table*, LClosure* (a Proto's holder, or the closure), lua_State* */
  Slot key;           /* Table: the key waiting for its value */
};

class Restorer
{
public:
  /* stack: 1: permanents (key -> object); 3: refs (id -> object); 4: upvalues */
  Restorer(lua_State* L, byte_view data, bool host)
      : r_{L, data}, L_(L), host_(host), frames_(L, true), marked_(L, true), made_(L, true), umarked_(L, true), owners_(L, true), dues_(L, true),
        fixes_(L, true), rebuilt_(L, true)
  {
  }

  Restorer(const Restorer&) = delete;
  Restorer& operator=(const Restorer&) = delete;

  /* a failed restore leaves no finalizer to run on what it built (ADR-28),
     and the main thread, if it went into it, idle and empty as it was */
  ~Restorer()
  {
    if (!done_ && mainrestored_)
    {
      lua_State* M = mainthread(L_->l_G);
      luaF_closeupval(M, 0);
      M->ci = &M->base_ci;
      M->top = 1;
      M->tbclist = 0;
      M->ntbc = 0;
      M->frozen = 0;
      M->frozenchild = nullptr;
      M->frozenparent = nullptr;
      M->closeself = false;
      M->hook = mainsaved_.hook;
      M->hookmask = mainsaved_.hookmask;
      M->basehookcount = mainsaved_.basehookcount;
      M->hookcount = mainsaved_.hookcount;
      M->oldpc = mainsaved_.oldpc;
      M->allowhook = mainsaved_.allowhook;
      M->errfunc = mainsaved_.errfunc;
      M->nCcalls = mainsaved_.nCcalls;
      M->ccbase = mainsaved_.ccbase;
      M->status = mainsaved_.status;
      M->bown = mainsaved_.bown;
      luaE_setbeff(M, mainsaved_.beff);
    }
    if (!done_)
      for (size_t i = 0; i < made_.size(); i++)
      {
        GCObject* o = made_.get(i);
        if (o == nullptr)
          continue;
        else if (o->tt == LUA_VTABLE)
          static_cast<Table*>(o)->metatable = nullptr;
        else
          static_cast<Udata*>(o)->metatable = nullptr;
      }
  }

  Reader r_;
  lua_Integer seed_ = 0; /* the hash seed of the persisting state */

  /*
  ** The trailer (ADR-10), read first, from the end: the value ends where it
  ** starts. The tables to mark for finalization are made now, in the order
  ** their originals were marked, and marked as a script marks a table (by
  ** a metatable with '__gc'); the value's table records fill them (ADR-28).
  */
  void read_trailer()
  {
    byte_view d = r_.data;
    size_t end = d.size();
    lua_Unsigned len = 0;
    for (int shift = 0;; shift += 7)
    {
      if (shift > 63 || end <= r_.pos)
        Reader{L_, d, end}.invalid();
      lua_Unsigned c = static_cast<unsigned char>(d[--end]);
      len |= (c & 0x7F) << shift;
      if ((c & 0x80) == 0)
        break;
    }
    if (len > end - r_.pos)
      Reader{L_, d, end}.invalid();
    Reader t{L_, d.substr(0, end), end - static_cast<size_t>(len)};
    int n = t.get_count();
    if (n > 0)
    {
      luaL_checkstacki(L_, 4, std::nullopt);
      lua_createtable(L_, 0, 1); /* a metatable that marks */
      TValue gc{};
      setsvalue(L_, &gc, L_->l_G->tmname[TM_GC]);
      setslot(L_, L_->top, &gc);
      L_->top++;
      lua_pushboolean(L_, 1);
      lua_rawset(L_, -3);
      WorkStack<lua_Integer> kinds(L_, true);
      kinds.reserve(static_cast<size_t>(n));
      for (int i = 0; i < n; i++)
      {
        lua_Integer kind = t.get_var();
        if (kind != 0 && kind != 1)
          t.invalid();
        kinds.push(kind);
      }
      for (int i = 0; i < n; i++)
      {
        lua_Integer id = t.get_var();
        if (id < 1 || lua_rawgeti(L_, REFS, id) != LUA_TNIL)
          t.invalid();
        lua_pop(L_, 1);
        if (kinds.get(static_cast<size_t>(i)) == 1)
        { /* a user object: marked when it is read (its kind makes it) */
          made_.push(nullptr);
          umarked_.push(UMarked{id, static_cast<size_t>(i)});
          continue;
        }
        lua_createtable(L_, 0, 0);
        TValue h = getslot(L_, L_->top - 1);
        made_.push(gcvalue(&h));
        marked_.push(id);
        lua_pushvalue(L_, -2);
        lua_setmetatable(L_, -2);
        lua_rawseti(L_, REFS, id);
      }
      lua_pop(L_, 1);
      marked_.sort([](lua_Integer a, lua_Integer b) { return a < b; });
      umarked_.sort([](const UMarked& a, const UMarked& b) { return a.id < b.id; });
      /* who they belong to, in the same order: the groups come with the value */
      for (int i = 0; i < n; i++)
      {
        lua_Integer code = t.get_var();
        if (code < 0)
          t.invalid();
        owners_.push(code);
      }
      for (int i = 0; i < n; i++)
      {
        lua_Integer due = t.get_var();
        if (due != 0 && due != 1)
          t.invalid();
        dues_.push(due);
      }
    }
    int m = t.get_count(3);
    for (int i = 0; i < m; i++)
    {
      DeadFix f{t.get_var(), t.get_var(), t.get_var()};
      if (f.table < 1 || f.node < 0 || f.node > INT_MAX || f.key < 1)
        t.invalid();
      fixes_.push(f);
    }
    if (t.pos != t.data.size())
      t.invalid();
    r_.data = d.substr(0, end - static_cast<size_t>(len));
  }

  /*
  ** The value is read: every table made for the trailer must have been met,
  ** and dead keys get their identities (ADR-29). The restore succeeded.
  */
  void finish()
  {
    if (next_marked_ != marked_.size())
      r_.invalid();
    for (size_t i = 0; i < made_.size(); i++)
      if (made_.get(i) == nullptr)
        r_.invalid(); /* (a marked user object the value did not have) */
    /* in the order they were marked (user objects are marked as they are
       read): those due go to their queues from it */
    if (umarked_.size() > 0)
    {
      std::vector<GCObject*> order; /* (host memory, a short time) */
      for (size_t i = 0; i < made_.size(); i++)
        order.push_back(made_.get(i));
      luaC_ordermarked(L_, order.data(), order.size());
    }
    /* the owners of the marked tables: their groups are made now */
    for (size_t i = 0; i < made_.size(); i++)
    {
      lua_Integer code = owners_.get(i);
      unsigned short owner = 0;
      if (code == 1)
        owner = GROUP_GONE;
      else if (code >= 2)
      {
        lua_rawgeti(L_, REFS, code - 2);
        TValue v = getslot(L_, L_->top - 1);
        if (!iscollectable(&v) || gcvalue(&v)->tt != LUA_VBUDGET)
          r_.invalid();
        owner = static_cast<BudgetGroup*>(gcvalue(&v))->id;
        lua_pop(L_, 1);
      }
      made_.get(i)->owner = owner;
    }
    /* those whose '__gc' was due are due again, in the order they had in their queues */
    {
      std::vector<GCObject*> due; /* (host memory, a short time) */
      for (size_t i = made_.size(); i > 0; i--)
        if (dues_.get(i - 1) != 0)
          due.push_back(made_.get(i - 1));
      if (!due.empty())
        luaC_makedue(L_, due.data(), due.size());
    }
    auto by_address = [](Table* a, Table* b) { return std::less<Table*>()(a, b); };
    rebuilt_.sort(by_address);
    for (size_t i = 0; i < fixes_.size(); i++)
    {
      DeadFix f = fixes_.get(i);
      lua_rawgeti(L_, REFS, f.table);
      lua_rawgeti(L_, REFS, f.key);
      TValue t = getslot(L_, L_->top - 2);
      TValue k = getslot(L_, L_->top - 1);
      if (!ttistable(&t) || !iscollectable(&k))
        r_.invalid();
      Table* h = hvalue(&t);
      size_t r = rebuilt_.lower_bound(h, by_address);
      bool was_rebuilt = r < rebuilt_.size() && rebuilt_.get(r) == h;
      /* a table rebuilt, here or for another seed, has no dead keys */
      if (same_seed() && !was_rebuilt)
      {
        if (isdummy(h) || f.node >= sizenode(h) || !keyisdead(gnode(h, static_cast<size_t>(f.node))))
          r_.invalid();
        int j = static_cast<int>(f.node);
        h->luaH_setnode(L_, j, &k, true, nullptr, gnode(h, static_cast<size_t>(j))->next);
      }
      lua_pop(L_, 2);
    }
    done_ = true;
  }

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
  WorkStack<lua_Integer> marked_; /* ids of the tables made for the trailer, sorted */
  WorkStack<GCObject*> made_;     /* those objects (user objects as they are read) */
  struct UMarked
  {
    lua_Integer id; /* a marked user object's reference id */
    size_t made;    /* its place in 'made_' */
  };
  WorkStack<UMarked> umarked_;    /* the user objects of the trailer, sorted by id */
  WorkStack<lua_Integer> owners_; /* their owners (see Walker::owner_code), in the order they were made */
  WorkStack<lua_Integer> dues_;   /* whether their '__gc' was due (waiting in a queue) */
  size_t next_marked_ = 0;        /* the next of them the value meets */

  struct DeadFix
  {
    lua_Integer table;
    lua_Integer node;
    lua_Integer key;
  };
  WorkStack<DeadFix> fixes_;
  bool done_ = false;
  bool mainrestored_ = false; /* a frozen main thread is being restored into this state's */
  struct MainSaved
  { /* what the main thread had (idle and empty) before the restore went into it */
    lua_Hook hook;
    l_signalT hookmask;
    int basehookcount, hookcount, oldpc;
    lu_byte allowhook;
    StkId errfunc;
    l_uint32 nCcalls, ccbase;
    TStatus status;
    BudgetGroup* bown;
    BudgetGroup* beff;
  } mainsaved_{};

  WorkStack<Table*> rebuilt_; /* layout tables whose keys did not all fit */

  unsigned int lasthash_ = 0; /* the place of the last hash read */

  /* an object's hash (Walker::hash), in the persisting state's sequence */
  unsigned int get_hash()
  {
    lua_Integer d = r_.get_var();
    if (d < INT_MIN || d > INT_MAX)
      r_.invalid();
    lasthash_ += static_cast<unsigned int>(static_cast<int>(d));
    return static_cast<unsigned int>(seed_) + lasthash_ * HASHSTEP;
  }

  /* the next reference id, of an object other than a table */
  lua_Integer new_id()
  {
    lua_Integer id = ++refcount_;
    if (next_marked_ < marked_.size() && marked_.get(next_marked_) == id)
      r_.invalid(); /* marked for finalization, so it must be a table */
    return id;
  }

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
      lua_pushinteger(L_, r_.get_var());
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
      lua_Integer id = r_.get_var();
      lua_rawgeti(L_, REFS, id);
      if (lua_isnil(L_, -1))
        r_.invalid();
      return Next::Ready;
    }
    case TAG_PERM:
    {
      Frame f;
      f.kind = Build::Perm;
      f.part = Part::Object;
      f.type = r_.get_byte();
      f.id = new_id();
      if (f.type < 0 || f.type >= LUA_NUMTYPES)
        r_.invalid();
      frames_.push(f);
      return Next::Read;
    }
    case TAG_STRING:
    {
      lua_Integer id = new_id();
      lua_Integer n = r_.get_var();
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
      f.id = new_id();
      f.a = get_hash();
      frames_.push(f);
      return Next::Advance;
    }
    case TAG_THREAD:
      start_thread(new_id());
      return Next::Advance;
    case TAG_MAINTHREAD_FROZEN:
      if (!host_) /* (as TAG_MAINTHREAD) */
        r_.invalid();
      start_thread(new_id(), true);
      return Next::Advance;
    case TAG_GROUP:
      start_group(new_id());
      return Next::Read;
    case TAG_USEROBJECT:
      start_userobject(new_id());
      return Next::Read;
    case TAG_CCLOSURE:
      return start_cclosure(new_id());
    case TAG_CFI:
    {
      lua_Integer id = new_id();
      CFuncInfo* cfi = lua_cfibyname(read_name());
      if (cfi == nullptr)
        r_.invalid();
      setslotf(L_, L_->top, cfi);
      L_->top++;
      register_top(id);
      return Next::Ready;
    }
    case TAG_MAINTHREAD:
    {
      if (!host_) /* a script never gets the main thread from a stream */
        r_.invalid();
      lua_Integer id = new_id();
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
    case Build::Closure:
      return closure_part(f);
    case Build::Proto:
      return proto_part(f);
    case Build::Group:
      return group_part(f);
    case Build::Userobject:
      return userobject_part(f);
    case Build::CClosure:
      return cclosure_part(f);
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
    case Build::Table:
      return table_advance(f);
    default:
      return Next::Read;
    }
  }

  /* ---- tables ---- */

  void start_table(lua_Integer id)
  {
    unsigned int hash = get_hash();
    lua_Integer asize = r_.get_var();
    lua_Integer hsize = r_.get_var();
    lua_Integer lenhint = r_.get_var();
    char layout = r_.get_byte();
    if (asize < 0 || asize > INT_MAX || hsize < 0 || hsize > INT_MAX ||
        (hsize & (hsize - 1)) != 0 || lenhint < 0 || lenhint > asize ||
        (layout != 0 && layout != 1) || (layout == 1 && hsize == 0))
      r_.invalid();
    /* same sizes as the original (the table limits are checked as by
       lua_createtable): entries go where they were, with no rehash */
    if (next_marked_ < marked_.size() && marked_.get(next_marked_) == id)
    { /* made for the trailer */
      next_marked_++;
      lua_rawgeti(L_, REFS, id);
      TValue m = getslot(L_, L_->top - 1);
      lua_lock(L_);
      hvalue(&m)->luaH_resize(L_, static_cast<int>(asize), static_cast<int>(hsize));
      lua_unlock(L_);
    }
    else
    {
      lua_createtable(L_, static_cast<int>(asize), static_cast<int>(hsize));
      register_top(id);
    }
    TValue t = getslot(L_, L_->top - 1);
    hvalue(&t)->hash = hash; /* before it can be a key anywhere */
    Frame f;
    f.kind = Build::Table;
    f.part = Part::Meta;
    f.a = asize;
    f.b = hsize;
    f.c = lenhint;
    f.type = layout;
    f.p = hvalue(&t);
    lua_pop(L_, 1);
    frames_.push(f);
  }

  /* with the persisting state's seed, keys hash as they did: nodes go in
     their slots; otherwise their entries are inserted */
  bool same_seed() const
  {
    return seed_ == static_cast<lua_Integer>(L_->l_G->seed);
  }

  /*
  ** Every key that hashes by an object's hash sits where a lookup finds it:
  ** in the chain from its main position. A key the restoring side supplied
  ** (a permanent, a light userdata or a host's C function, which hash by
  ** address) can hash otherwise (ADR-30); an engine C function hashes by
  ** its name.
  */
  bool keys_in_place(const Table* h) const
  {
    int size = sizenode(h);
    for (int i = 0; i < size; i++)
    {
      const Node* n = gnode(h, static_cast<size_t>(i));
      if (keyisnil(n) || keyisdead(n))
        continue;
      lu_byte kt = novariant(n->key_tt);
      if (kt == LUA_TSTRING || kt == LUA_TNUMBER || kt == LUA_TBOOLEAN)
        continue;
      TValue k{};
      getnodekey(L_, &k, n);
      int j = h->luaH_mainposition(&k);
      for (int steps = 0; j != i; steps++)
      {
        int next = gnode(h, static_cast<size_t>(j))->next;
        if (next == 0 || steps == size)
          return false;
        j += next;
      }
    }
    return true;
  }

  /* the table is complete */
  Next end_table(Frame f)
  {
    Table* h = static_cast<Table*>(f.p);
    if (h->asize() != f.a || allocsizenode(h) != f.b)
      r_.invalid();
    if (f.type == 1 && same_seed())
    {
      if (keys_in_place(h))
        h->lastfree = static_cast<int>(f.d);
      else
      { /* rebuilt with the same sizes: its contents, in another order */
        rebuilt_.push(h);
        lua_lock(L_);
        h->luaH_resize(L_, static_cast<int>(f.a), static_cast<int>(f.b));
        lua_unlock(L_);
      }
    }
    h->lenhint = static_cast<int>(f.c);
    push_table(h);
    return done();
  }

  Next table_advance(Frame f)
  {
    Table* h = static_cast<Table*>(f.p);
    while (f.i < f.b)
    {
      char state = r_.get_byte();
      if (state == 2)
      {
        f.part = Part::NodeKey;
        frames_.set_top(f);
        return Next::Read;
      }
      if (state != 0 && state != 1)
        r_.invalid();
      int link = node_link(f);
      if (same_seed())
      {
        if (allocsizenode(h) != f.b) /* (a corrupted stream made the table something else) */
          r_.invalid();
        h->luaH_setnode(L_, f.i, nullptr, state == 1, nullptr, link);
      }
      f.i++;
    }
    return end_table(f);
  }

  int node_link(const Frame& f)
  {
    lua_Integer link = r_.get_var();
    if (f.i + link < 0 || f.i + link >= f.b)
      r_.invalid();
    return static_cast<int>(link);
  }

  Next table_part(Frame f)
  {
    Table* h = static_cast<Table*>(f.p);
    switch (f.part)
    {
    case Part::Meta:
    {
      /* set as lua_setmetatable sets it, but without marking the table for
         finalization: the trailer marks exactly the marked (ADR-28) */
      Table* mt = nullptr;
      if (!lua_isnil(L_, -1))
      {
        if (!lua_istable(L_, -1))
          r_.invalid();
        TValue m = getslot(L_, L_->top - 1);
        mt = hvalue(&m);
      }
      h->metatable = mt;
      if (mt != nullptr)
        luaC_objbarrier(L_, h, mt);
      lua_pop(L_, 1);
      f.part = Part::Key;
      frames_.set_top(f);
      return Next::Read;
    }
    case Part::Key:
      if (lua_isnil(L_, -1))
      {
        lua_pop(L_, 1);
        if (f.type == 0)
          return end_table(f);
        f.d = r_.get_var(); /* lastfree */
        if (f.d < 0 || f.d > f.b)
          r_.invalid();
        f.i = 0;
        f.part = Part::Nodes;
        frames_.set_top(f);
        return Next::Advance;
      }
      {
        TValue k = getslot(L_, L_->top - 1);
        f.key = Slot::of(k);
      }
      lua_pop(L_, 1);
      f.part = Part::Val;
      frames_.set_top(f);
      return Next::Read;
    case Part::NodeKey:
    {
      TValue k = getslot(L_, L_->top - 1);
      if (ttisnil(&k) || (ttisfloat(&k) && fltvalue(&k) != fltvalue(&k)))
        r_.invalid();
      f.key = Slot::of(k);
      lua_pop(L_, 1);
      f.part = Part::NodeVal;
      frames_.set_top(f);
      return Next::Read;
    }
    case Part::NodeVal:
    {
      TValue k = f.key.get();
      int link = node_link(f);
      if (same_seed())
      {
        if (allocsizenode(h) != f.b)
          r_.invalid();
        TValue v = getslot(L_, L_->top - 1);
        h->luaH_setnode(L_, f.i, &k, false, &v, link);
        lua_pop(L_, 1);
      }
      else if (lua_isnil(L_, -1))
        lua_pop(L_, 1);
      else
      {
        push_table(h);
        setslot(L_, L_->top, &k);
        L_->top++;
        lua_rotate(L_, -3, 2); /* table, key, value */
        lua_rawset(L_, -3);
        lua_pop(L_, 1);
      }
      f.i++;
      f.part = Part::Nodes;
      frames_.set_top(f);
      return Next::Advance;
    }
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

  /* ---- permanents ---- */

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
      lua_rawgeti(L_, REFS, r_.get_var());
      if (lua_type(L_, -1) != LUA_TFUNCTION || lua_iscfunction(L_, -1))
        r_.invalid();
      return Next::Ready;
    }
    if (tag != TAG_PROTO_NEW)
      r_.invalid();
    start_proto(new_id());
    return Next::Advance;
  }

  void start_proto(lua_Integer pid)
  {
    global_State* g = L_->l_G;
    lua_lock(L_);
    /* (the holder first, anchored on the stack: making the prototype can
       run an emergency collection, which would take it) */
    LClosure* holder = LClosure::luaF_newLclosure(L_, 0);
    setslotclL(L_, L_->top, holder);
    L_->top++;
    Proto* p = Proto::luaF_newproto(L_);
    holder->p = p;
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
      n = r_.get_count(2);
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
        lua_Integer uid = new_id();
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
      lua_Integer uid = r_.get_var();
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
      cl->hash = static_cast<unsigned int>(f.a);
      lua_pop(L_, 1);
      lua_Integer nup = r_.get_var();
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
      lua_Integer level = r_.get_var();
      if (L1 == nullptr || L1 == L_ || (L1->status == LUA_OK && L1->ci != &L1->base_ci && L1->frozen == 0) ||
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
  void start_thread(lua_Integer id, bool main = false)
  {
    lua_State* L1;
    if (main)
    { /* a frozen main thread: into this state's, which must be idle and
         empty, and not the thread restoring (whose stack is in use) */
      L1 = mainthread(L_->l_G);
      if (L1 == L_ || L1->ci != &L1->base_ci || L1->top != 1 || mainrestored_)
        luaL_errori(L_, lt::a_frozen_main_thread_is_restored_into_an_idle_main_thread);
      push_thread(L1);
      register_top(id);
      lua_pop(L_, 1);
      mainrestored_ = true;
      mainsaved_ = MainSaved{L1->hook, L1->hookmask, L1->basehookcount, L1->hookcount, L1->oldpc,
                             L1->allowhook, L1->errfunc, L1->nCcalls, L1->ccbase, L1->status,
                             L1->bown, L1->beff};
    }
    else
    {
      unsigned int hash = get_hash();
      L1 = lua_newthread(L_);
      L1->hash = hash;
      register_top(id);
      lua_pop(L_, 1);
    }
    char status = r_.get_byte();
    lua_Integer top = r_.get_var();
    lua_Integer maxtop = r_.get_var();
    if (!valid_status(status) || top < 1 || maxtop < top ||
        maxtop >= LUAI_MAXSTACK - LUA_MINSTACK)
      r_.invalid();
    int need = static_cast<int>(maxtop) + LUA_MINSTACK;
    if (!lua_checkstack(L1, need - static_cast<int>(L1->top)))
      memory_error(L_);
    for (StkId i = L1->top; i < top; i++)
      setslotnil(L1, i); /* (what was above the main thread's top is stale) */
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
    if (nframes < 0)
      r_.invalid();
    if (nframes > 0)
      read_frames(L1, nframes, f.a, f.b);
    read_tbc(L1, nframes, f.a);
    read_thread_extras(L1, f.a);
    if (nframes > 0 && f.type == LUA_OK && L1->frozen == 0)
      r_.invalid(); /* (frames of a thread not started or dead: only a frozen one has them) */
    char kind = r_.get_byte();
    if (kind == 0)
    {
      L1->status = static_cast<TStatus>(f.type);
      return thread_values(f);
    }
    if (kind != 1)
      r_.invalid();
    f.a = r_.get_i64();
    f.b = r_.get_i64();
    f.c = r_.get_i64();
    f.d = r_.get_i64();
    f.e = r_.get_byte();
    if (f.b > INT_MAX || f.c > INT_MAX || f.d < INT_MIN || f.d > INT_MAX)
      r_.invalid();
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
    if (f.part >= Part::Bown)
      return thread_chain_part(f);
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
    return thread_values(f);
  }

  /* what a frozen thread keeps (see Walker::thread_extras) */
  void read_thread_extras(lua_State* L1, lua_Integer top)
  {
    char ah = r_.get_byte();
    if (ah != 0 && ah != 1)
      r_.invalid();
    L1->allowhook = static_cast<lu_byte>(ah);
    char fz = r_.get_byte();
    if (fz < 0 || fz > FZ_HOSTCLOSE)
      r_.invalid();
    if (fz == 0)
      return;
    luaE_setfrozen(L1, static_cast<lu_byte>(fz));
    L1->frozenfunc = r_.get_var();
    L1->frozenerrfunc = r_.get_var();
    L1->frozennres = r_.get_int();
    L1->frozencc = static_cast<l_uint32>(r_.get_var());
    L1->ccbase = static_cast<l_uint32>(r_.get_var());
    L1->nCcalls = static_cast<l_uint32>(r_.get_var());
    L1->errfunc = r_.get_var();
    char cs = r_.get_byte();
    char self = r_.get_byte();
    if (L1->frozenfunc < 0 || L1->frozenfunc > top || L1->frozenerrfunc < 0 || L1->frozenerrfunc > top ||
        L1->frozennres < -1 || L1->errfunc < 0 || L1->errfunc > top || cs < 0 || (self != 0 && self != 1))
      r_.invalid();
    L1->closestatus = static_cast<lu_byte>(cs);
    L1->closeself = (self == 1);
  }

  /* the values after the raw parts of a thread: its groups and chain */
  Next thread_values(Frame f)
  {
    f.part = Part::Bown;
    frames_.set_top(f);
    return Next::Read;
  }

  /* a budget group (or nil) on top of the stack, popped */
  BudgetGroup* pop_group()
  {
    TValue v = getslot(L_, L_->top - 1);
    BudgetGroup* bg = nullptr;
    if (!ttisnil(&v))
    {
      if (!iscollectable(&v) || gcvalue(&v)->tt != LUA_VBUDGET)
        r_.invalid();
      bg = static_cast<BudgetGroup*>(gcvalue(&v));
    }
    lua_pop(L_, 1);
    return bg;
  }

  /* a thread (or nil) on top of the stack, popped */
  lua_State* pop_thread()
  {
    lua_State* th = nullptr;
    if (!lua_isnil(L_, -1))
    {
      th = lua_tothread(L_, -1);
      if (th == nullptr)
        r_.invalid();
    }
    lua_pop(L_, 1);
    return th;
  }

  Next thread_chain_part(Frame f)
  {
    lua_State* L1 = static_cast<lua_State*>(f.p);
    switch (f.part)
    {
    case Part::Bown:
      L1->bown = pop_group();
      f.part = Part::Beff;
      break;
    case Part::Beff:
    {
      BudgetGroup* bg = pop_group();
      if (L1->frozen != 0 && bg == nullptr)
        r_.invalid(); /* (a thread frozen by a budget had one in effect) */
      luaE_setbeff(L1, bg);
      f.part = Part::Parent;
      break;
    }
    case Part::Parent:
    {
      /* (the chain is linked from either end, whichever is read first) */
      lua_State* p = pop_thread();
      L1->frozenparent = p;
      if (p != nullptr)
      {
        if (p == L1 || (p->frozenchild != nullptr && p->frozenchild != L1))
          r_.invalid();
        p->frozenchild = L1;
      }
      f.part = Part::Child;
      break;
    }
    default: /* Part::Child */
    {
      lua_State* c = pop_thread();
      if (c != nullptr)
      {
        if (c == L1 || (c->frozenparent != nullptr && c->frozenparent != L1) ||
            (L1->frozenchild != nullptr && L1->frozenchild != c))
          r_.invalid();
        c->frozenparent = L1;
        L1->frozenchild = c;
      }
      else if (L1->frozenchild != nullptr)
        r_.invalid(); /* (a thread says it was resumed by this one, which says it was not) */
      if ((L1->frozenparent != nullptr || L1->frozenchild != nullptr) && L1->frozen == 0)
        r_.invalid();
      if (L1->frozen != 0 && L1->frozenparent == nullptr && L1->bown == nullptr)
        r_.invalid(); /* (the freeze target has its own group: a thaw puts it in effect;
                         the others get their resumer's, which may be all they have) */
      push_thread(L1);
      return done();
    }
    }
    frames_.set_top(f);
    return Next::Read;
  }

  /* ---- user objects ---- */

  void start_userobject(lua_Integer id)
  {
    unsigned int hash = get_hash();
    lua_ObjectFactory factory = lua_objectkind(read_name());
    lua_Integer objsize = r_.get_var();
    lua_Integer nuv = r_.get_var();
    int nints = r_.get_count(1);
    if (factory == nullptr || objsize < 0 || nuv < 0 || nuv > 255 || nints > 8)
      r_.invalid();
    lua_Integer ints[8];
    for (int i = 0; i < nints; i++)
      ints[i] = r_.get_var();
    int nbytes = r_.get_count(1);
    byte_view bytes = r_.get_bytes(static_cast<size_t>(nbytes));
    std::unique_ptr<lua_UserObject> obj = factory(L_, ints, nints, bytes);
    if (obj == nullptr)
      r_.invalid();
    lua_newuserobject(L_, std::move(obj), static_cast<size_t>(objsize), static_cast<int>(nuv));
    TValue t = getslot(L_, L_->top - 1);
    uvalue(&t)->hash = hash;
    register_top(id);
    lua_pop(L_, 1);
    Frame f;
    f.kind = Build::Userobject;
    f.part = Part::UMeta;
    f.id = id;
    f.n = static_cast<int>(nuv);
    f.i = 1;
    f.p = uvalue(&t);
    frames_.push(f);
  }

  Next userobject_part(Frame f)
  {
    Udata* u = static_cast<Udata*>(f.p);
    TValue v = getslot(L_, L_->top - 1);
    if (f.part == Part::UMeta)
    {
      if (ttisnil(&v))
        u->metatable = nullptr;
      else
      {
        if (!ttistable(&v))
          r_.invalid();
        u->metatable = hvalue(&v);
        luaC_objbarrier(L_, u, u->metatable);
      }
      lua_pop(L_, 1);
      size_t at = umarked_.lower_bound(UMarked{f.id, 0}, [](const UMarked& a, const UMarked& b) { return a.id < b.id; });
      if (at < umarked_.size() && umarked_.get(at).id == f.id)
      { /* marked for finalization in the persisting state (whatever its
           metatable says now): marked as a script marks it, by a metatable
           with '__gc'; its owner is set at the end ('finish') */
        Table* mt = u->metatable;
        luaL_checkstacki(L_, 3, std::nullopt);
        lua_createtable(L_, 0, 1);
        TValue gc{};
        setsvalue(L_, &gc, L_->l_G->tmname[TM_GC]);
        setslot(L_, L_->top, &gc);
        L_->top++;
        lua_pushboolean(L_, 1);
        lua_rawset(L_, -3);
        TValue mv = getslot(L_, L_->top - 1);
        u->metatable = hvalue(&mv);
        luaC_checkfinalizer(L_, u, u->metatable);
        u->metatable = mt;
        lua_pop(L_, 1);
        made_.set(umarked_.get(at).made, u);
      }
    }
    else
    {
      copyobj(&u->uv[static_cast<size_t>(f.i - 1)], &v);
      luaC_barrier(L_, u, &u->uv[static_cast<size_t>(f.i - 1)]);
      lua_pop(L_, 1);
      f.i++;
    }
    if (f.part == Part::UMeta)
      f.part = Part::UValue;
    if (f.i <= f.n)
    {
      frames_.set_top(f);
      return Next::Read;
    }
    TValue d{};
    setuvalue(L_, &d, u);
    setslot(L_, L_->top, &d);
    L_->top++;
    return done();
  }

  /* ---- C closures of a registered kind ---- */

  Next start_cclosure(lua_Integer id)
  {
    unsigned int hash = get_hash();
    const lua_CClosureKind* kind = lua_cclosurekindbyname(read_name());
    int nints = r_.get_count(1);
    if (kind == nullptr || nints > 5 || (nints > 0 && kind->save == nullptr))
      r_.invalid();
    Frame f;
    lua_Integer* slots[5] = {&f.a, &f.b, &f.c, &f.d, &f.e};
    for (int i = 0; i < nints; i++)
      *slots[i] = r_.get_var();
    lua_lock(L_);
    CClosure* cl = CClosure::luaF_newCclosure(L_, kind->nup);
    for (TValue& u : cl->upvalue)
      setnilvalue(&u);
    cl->f = kind->f;
    cl->hash = hash;
    TValue cv{};
    setclCvalue(L_, &cv, cl);
    setslot(L_, L_->top, &cv);
    L_->top++;
    lua_unlock(L_);
    register_top(id);
    lua_pop(L_, 1);
    f.kind = Build::CClosure;
    f.part = Part::CUp;
    f.id = id;
    f.p = const_cast<lua_CClosureKind*>(kind);
    f.i = 0;
    f.n = (kind->save != nullptr) ? kind->nup - 1 : kind->nup;
    f.type = static_cast<char>(nints);
    frames_.push(f);
    return f.n > 0 ? Next::Read : finish_cclosure();
  }

  /* all its upvalues are in: the closure it has under the frame's id is the value */
  Next finish_cclosure()
  {
    Frame f = frames_.top();
    const lua_CClosureKind* kind = static_cast<const lua_CClosureKind*>(f.p);
    lua_rawgeti(L_, REFS, f.id);
    TValue cv = getslot(L_, L_->top - 1);
    CClosure* cl = clCvalue(&cv);
    if (kind->make != nullptr)
    {
      lua_Integer ints[5] = {f.a, f.b, f.c, f.d, f.e};
      if (!kind->make(L_, cl, ints, f.type))
        r_.invalid();
    }
    return done();
  }

  Next cclosure_part(Frame f)
  {
    lua_rawgeti(L_, REFS, f.id);
    TValue cv = getslot(L_, L_->top - 1);
    CClosure* cl = clCvalue(&cv);
    lua_pop(L_, 1);
    TValue v = getslot(L_, L_->top - 1);
    copyobj(&cl->upvalue[static_cast<size_t>(f.i)], &v);
    luaC_barrier(L_, cl, &cl->upvalue[static_cast<size_t>(f.i)]);
    lua_pop(L_, 1);
    f.i++;
    frames_.set_top(f);
    if (f.i < f.n)
      return Next::Read;
    return finish_cclosure();
  }

  /* ---- budget groups ---- */

  byte_view read_name()
  {
    lua_Integer n = r_.get_var();
    if (n < 0 || n > 256)
      r_.invalid();
    return r_.get_bytes(static_cast<size_t>(n));
  }

  void start_group(lua_Integer id)
  {
    lua_Integer budget = r_.get_var();
    lua_Integer left = r_.get_var();
    lua_Budget fn = lua_budgetbyname(read_name());
    char nodrain = r_.get_byte();
    int nqueue = r_.get_count();
    if (budget < 1 || left > budget || fn == nullptr || (nodrain != 0 && nodrain != 1))
      r_.invalid();
    lua_lock(L_);
    BudgetGroup* bg = luaM_malloc_BudgetGroup(L_);
    luaC_link(L_, bg, LUA_VBUDGET);
    bg->budget = static_cast<l_mem>(budget);
    bg->left = static_cast<l_mem>(left);
    bg->f = fn;
    bg->nodrain = (nodrain == 1);
    TValue gv{};
    setgcovalue(L_, &gv, bg);
    setslot(L_, L_->top, &gv);
    L_->top++;
    bool ok = luaC_newowner(L_, bg);
    lua_unlock(L_);
    if (!ok)
      luaL_errori(L_, lt::too_many_budget_groups);
    register_top(id);
    lua_pop(L_, 1);
    luaL_checkstacki(L_, 3, std::nullopt);
    lua_preparebudget(L_, fn); /* (what its callback needs of this state) */
    Frame f;
    f.kind = Build::Group;
    f.part = Part::GroupCb;
    f.id = id;
    f.n = nqueue;
    f.p = bg;
    frames_.push(f);
  }

  Next group_part(Frame f)
  {
    BudgetGroup* bg = static_cast<BudgetGroup*>(f.p);
    if (f.part == Part::GroupCb)
    {
      TValue cbv = getslot(L_, L_->top - 1);
      copyobj(&bg->cb, &cbv);
      luaC_barrier(L_, bg, &bg->cb);
      lua_pop(L_, 1);
      f.part = Part::GroupRoot;
      frames_.set_top(f);
      return Next::Read;
    }
    if (f.part == Part::GroupRoot)
    {
      bg->root = pop_thread();
      f.part = Part::GroupQueue;
      f.i = 0;
    }
    else
    { /* a dead object waiting for its '__gc' (the trailer makes it due) */
      TValue v = getslot(L_, L_->top - 1);
      if (!ttistable(&v) && !ttisfulluserdata(&v))
        r_.invalid();
      lua_pop(L_, 1);
      f.i++;
    }
    if (f.i < f.n)
    {
      frames_.set_top(f);
      return Next::Read;
    }
    TValue gv{};
    setgcovalue(L_, &gv, bg);
    setslot(L_, L_->top, &gv);
    L_->top++;
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
      char hask = r_.get_byte();
      if (hask != 0 && hask != 1)
        r_.invalid();
      if (hask == 1)
      {
        ci->k = lua_kbyname(read_name());
        if (ci->k == nullptr)
          r_.invalid();
      }
      ci->nccalls = static_cast<l_uint32>(r_.get_var());
      ci->apiop = static_cast<lu_byte>(r_.get_byte());
      ci->apifirst = r_.get_var();
      ci->hookev = static_cast<lu_byte>(r_.get_byte());
      ci->hookdone = static_cast<lu_byte>(r_.get_byte());
      ci->hooktop = r_.get_var();
      ci->hookcitop = r_.get_var();
      ci->hookdelta = r_.get_int();
      ci->hookres = r_.get_var();
      if (ci->func < 0 || ci->func >= top || ci->top < ci->func || ci->top > maxtop ||
          !valid_frame(L1, ci) || ci->apiop > APIK_CLOSESLOT || ci->hookev > HE_RETTBC ||
          ci->apifirst < 0 || ci->apifirst > maxtop || ci->hooktop < 0 || ci->hooktop > maxtop ||
          ci->hookcitop < 0 || ci->hookcitop > maxtop || ci->hookres < 0 || ci->hookres > maxtop ||
          ((ci->callstatus & CIST_APIK) == 0 && (ci->apiop != 0 || ci->apifirst != 0)) ||
          ((ci->callstatus & CIST_HOOKED) == 0 && (ci->hookev != 0 || ci->hookdone != 0 || ci->hooktop != 0 ||
                                                   ci->hookcitop != 0 || ci->hookdelta != 0 || ci->hookres != 0)))
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

/*
** Checks the header, reads the persisting state's hash seed and sets up the
** stack for a Restorer (3: refs; 4: upvalues)
*/
static void begin_restore(lua_State* L, Restorer& R)
{
  const byte_view header = lt::styx_header;
  if (R.r_.data.size() < header.size() || R.r_.data.substr(0, header.size()) != header)
    R.r_.invalid();
  R.r_.pos = header.size();
  R.seed_ = R.r_.get_i64();
  lua_createtable(L, 0, 0);
  lua_createtable(L, 0, 0);
  R.read_trailer();
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
    w.i64(L->l_G->seed);
    TValue v = getslot(L, L->ci->func + 2);
    w.root(v, &lt::path_root);
    w.trailer();
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
  R.finish();
  return 1;
}

static CFuncInfo cfi_persist(styx_persist, "styx.persist");
static CFuncInfo cfi_unpersist(styx_unpersist, "styx.unpersist");

static const luaL_RegCFI funcs[] = {
  {lt::persist, &cfi_persist},
  {lt::unpersist, &cfi_unpersist},
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

/* the string hash seed of the state that wrote a stream (false: not one) */
static bool stream_seed(byte_view data, unsigned int* seed)
{
  const byte_view header = lt::styx_header;
  size_t need = header.size() + sizeof(lua_Integer);
  if (data.size() < need || data.substr(0, header.size()) != header)
    return false;
  lua_Integer v;
  std::memcpy(&v, data.substr(header.size(), sizeof(v)).data(), sizeof(v));
  *seed = static_cast<unsigned int>(v);
  return true;
}

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
    if (L == main && main->frozen != 0)
      throw HostError{text(lt::the_main_thread_is_frozen_use_another_thread)};
    if ((main->ci != &main->base_ci && main->frozen == 0) || L->ci != &L->base_ci)
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
    hp.i64(L->l_G->seed);
    body(hp);
    hp.trailer();
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
    R.finish();
    return 1;
  }
  if (R.r_.get_byte() != TAG_STATE)
    R.r_.invalid();
  if (static_cast<unsigned int>(R.seed_) != L->l_G->seed) /* (luaL_restorestate makes it with the seed) */
    luaL_errori(L, lt::the_state_does_not_hash_as_the_persisted_one);
  R.value(); /* the registry, at 5 */
  if (!lua_istable(L, -1))
    R.r_.invalid();
  for (int t = 0; t < LUA_NUMTYPES; t++) /* the type metatables, at 6... */
  {
    R.value();
    if (!lua_isnil(L, -1) && !lua_istable(L, -1))
      R.r_.invalid();
  }
  int nmarked = R.r_.get_count(); /* the objects marked for finalization */
  for (int i = 0; i < nmarked; i++)
  {
    R.value();
    if (!lua_istable(L, -1) && lua_type(L, -1) != LUA_TUSERDATA)
      R.r_.invalid();
    lua_pop(L, 1);
  }
  R.value(); /* the thread it was persisted from, at 6 + LUA_NUMTYPES */
  if (!lua_isthread(L, -1))
    R.r_.invalid();
  char kind = R.r_.get_byte(); /* the collector's settings */
  char stopped = R.r_.get_byte();
  if ((kind != 0 && kind != 1) || (stopped != 0 && stopped != 1))
    R.r_.invalid();
  unsigned char params[LUA_GCPN];
  R.r_.get_raw(params, sizeof(params));
  unsigned int nexthash; /* the hash of the next object (ADR-30) */
  R.r_.get_raw(&nexthash, sizeof(nexthash));
  if (R.r_.pos != R.r_.data.size())
    R.r_.invalid();
  R.finish();
  /* the whole stream is read: the state takes the restored roots, then
     the collector's settings */
  global_State* g = L->l_G;
  TValue reg = getslot(L, L->ci->func + 5);
  copyobj(&g->l_registry, &reg);
  for (int t = 0; t < LUA_NUMTYPES; t++)
  {
    TValue mt = getslot(L, L->ci->func + 6 + t);
    g->mt[t] = ttistable(&mt) ? hvalue(&mt) : nullptr;
  }
  for (int p = 0; p < LUA_GCPN; p++)
    g->gcparams[p] = params[p];
  g->nexthash = nexthash;
  lua_gc(L, kind ? LUA_GCGEN : LUA_GCINC, 0, 0, 0);
  lua_gc(L, stopped ? LUA_GCSTOP : LUA_GCRESTART, 0, 0, 0);
  lua_pushvalue(L, 6 + LUA_NUMTYPES);
  return 1;
}

static CFuncInfo cfi_host_restore(host_restore_body, "styx.host_restore");

static int host_restore(lua_State* L, int perms, byte_view data, bool state, HostString* msg)
{
  lua_State* main = mainthread(L->l_G);
  if (L == main && main->frozen != 0)
  {
    *msg = HostString::from_bytes(lt::the_main_thread_is_frozen_use_another_thread);
    return LUA_ERRRUN;
  }
  if ((main->ci != &main->base_ci && main->frozen == 0) || L->ci != &L->base_ci)
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
  int st = lua_pcall(L, 2, 1, 0); /* (the value, or the thread a state was persisted from) */
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

/* a writer that keeps nothing: a walk that only checks */
namespace lua::lib
{
struct NullWriter : lua_Writer
{
  int write(lua_State*, std::optional<byte_view>) override { return 0; }
};
}

/*
** Can a persist of the state write all that the value at 'idx' holds
** ('perms': the stack index of the permanents, or 0)? The walk of a host
** persist, writing nothing: no Lua code runs and nothing is allocated in
** the state, so a running kernel can ask it of a user program. If not,
** 'msg' says why, with the path of what cannot be written.
*/
int lua_styxcheck(lua_State* L, int idx, int perms, HostString* msg)
{
  using namespace lua::lib;
  try
  {
    TValue v{};
    if (!value_at(L, idx, &v))
      throw HostError{text(lt::invalid_value_index)};
    Table* pt = nullptr;
    if (perms != 0)
    {
      TValue pv{};
      if (!value_at(L, perms, &pv) || !ttistable(&pv))
        throw HostError{text(lt::the_permanents_must_be_a_table)};
      pt = hvalue(&pv);
    }
    NullWriter w;
    Walker hp(L, pt, &w);
    hp.check_ = true;
    hp.root(v, &lt::path_root);
    hp.finish();
    return LUA_OK;
  }
  catch (HostError& e)
  {
    *msg = HostString::from_bytes(byte_view(e.message.data(), e.message.size()));
    return LUA_ERRRUN;
  }
}

int lua_styxunpersistvalue(lua_State* L, int perms, byte_view data, HostString* msg)
{
  return lua::lib::host_restore(L, perms, data, false, msg);
}

int lua_styxpersistvalue(lua_State* L, int perms, int idx, lua_Writer* w, HostString* msg)
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
    hp.whole();
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
    hp.marked_objects();
    hp.persisting_thread();
    hp.collector();
    hp.next_hash();
  });
}

namespace lua::lib
{
struct RestoreSetup
{
  luaL_RestoreSetup setup;
  void* ud;
};

/*
** 1: the RestoreSetup -> the inverse permanents (or nil), and the thread
** the restore runs on (anchored in the registry the restore replaces)
*/
static int restore_setup(lua_State* L)
{
  RestoreSetup* rs = static_cast<RestoreSetup*>(lua_touserdata(L, 1));
  lua_pop(L, 1);
  if (rs->setup != nullptr)
    rs->setup(L, rs->ud);
  else
    lua_pushnil(L);
  if (lua_gettop(L) != 1 || (!lua_isnil(L, 1) && !lua_istable(L, 1)))
    luaL_errori(L, lt::the_setup_must_leave_the_permanents_table_or_nil);
  lua_newthread(L);
  lua_pushvalue(L, -1);
  luaL_ref(L, LUA_REGISTRYINDEX);
  return 2;
}

static CFuncInfo cfi_restore_setup(restore_setup, "styx.restore_setup");
}

lua_State* luaL_restorestate(lua_Alloc* f, byte_view data, luaL_RestoreSetup setup, void* ud, HostString* msg)
{
  using namespace lua::lib;
  unsigned int seed;
  if (!stream_seed(data, &seed))
  {
    *msg = HostString::from_bytes(lt::not_a_persisted_state);
    return nullptr;
  }
  lua_State* L = lua_newstate(f, seed); /* (hashes as the persisting state) */
  if (L == nullptr)
  {
    *msg = HostString::from_bytes(lt::not_enough_memory);
    return nullptr;
  }
  RestoreSetup rs{setup, ud};
  lua_pushcfuncinfo(L, &cfi_restore_setup);
  lua_pushlightuserdata(L, &rs);
  int st = lua_pcall(L, 1, 2, 0);
  if (st != LUA_OK)
  {
    auto m = lua_tolstring(L, -1);
    *msg = m ? HostString::from_bytes(*m) : HostString::from_bytes(lt::error_object_is_not_a_string);
  }
  else
  { /* (restored from another thread, so that a frozen main thread is
       restored into the main thread, idle and with an empty stack) */
    lua_State* T = lua_tothread(L, 2);
    lua_pop(L, 1);
    bool perms = lua_istable(L, 1);
    lua_xmove(L, T, 1);
    st = host_restore(T, perms ? 1 : 0, data, true, msg);
    if (st == LUA_OK)
    { /* (the main thread's stack is empty, or a frozen main thread's, which only a thaw changes) */
      lua_State* from = lua_tothread(T, -1);
      lua_pop(T, 1);
      /* a whole cycle, unless the program stopped the collector: what was
         garbage when the state was persisted goes now (dead entries of
         weak tables), or is due (objects waiting for their '__gc': called
         at the collector's next step, in the program's time, none in the
         restore), as a cycle of the original would have taken it. A new
         state's first cycle starts only once its heap has grown by about
         its size: a host that restores more often than that would
         otherwise starve a program that waits on its collector. (After
         the restore's call, so that its table of the objects restored
         holds nothing.) */
      if (lua_gc(T, LUA_GCISRUNNING, 0, 0, 0))
      {
        lua_lock(T);
        luaC_fullgcdue(T);
        lua_unlock(T);
      }
      return from;
    }
  }
  lua_close(L);
  return nullptr;
}

int lua_styxtestmodule(lua_State* L)
{
  luaL_newlibcfi(L, lua::lib::funcs);
  return 1;
}
