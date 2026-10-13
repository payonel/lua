package com.payonel.lungolua.lib;

import static com.payonel.lungolua.api.LuaApi.*;
import static com.payonel.lungolua.api.LuaAuxLib.*;
import static com.payonel.lungolua.bytes.Bytes.copy_bytes;
import static com.payonel.lungolua.bytes.Lctype.*;
import static com.payonel.lungolua.gc.Lgc.*;
import static com.payonel.lungolua.lib.Ldblib.luaL_debughook;
import static com.payonel.lungolua.object.Lfunc.*;
import static com.payonel.lungolua.object.Llimits.*;
import static com.payonel.lungolua.object.Lnames.*;
import static com.payonel.lungolua.object.Lobject.*;
import static com.payonel.lungolua.object.Ltable.*;
import static com.payonel.lungolua.runtime.Ldo.*;
import static com.payonel.lungolua.runtime.Lhack.*;
import static com.payonel.lungolua.runtime.Lmem.*;
import static com.payonel.lungolua.runtime.Lstate.*;
import static com.payonel.lungolua.runtime.Ltm.*;
import static com.payonel.lungolua.vm.Ldebug.lua_sethook;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.IdentityHashMap;
import java.util.Map;
import java.util.OptionalInt;
import java.util.OptionalLong;
import java.util.function.Consumer;
import java.util.function.Predicate;

import com.payonel.lungolua.api.HostString;
import com.payonel.lungolua.api.luaL_Buffer;
import com.payonel.lungolua.api.luaL_RegCFI;
import com.payonel.lungolua.api.lua_Alloc;
import com.payonel.lungolua.api.lua_Budget;
import com.payonel.lungolua.api.lua_CClosureKind;
import com.payonel.lungolua.api.lua_CFunction;
import com.payonel.lungolua.api.lua_Hook;
import com.payonel.lungolua.api.lua_ObjectFactory;
import com.payonel.lungolua.api.lua_UserObject;
import com.payonel.lungolua.api.lua_Writer;
import com.payonel.lungolua.bytes.ByteOwner;
import com.payonel.lungolua.bytes.ByteView;
import com.payonel.lungolua.bytes.Lt;
import com.payonel.lungolua.object.AbsLineInfo;
import com.payonel.lungolua.object.BudgetGroup;
import com.payonel.lungolua.object.CClosure;
import com.payonel.lungolua.object.CFuncInfo;
import com.payonel.lungolua.object.GCObject;
import com.payonel.lungolua.object.LClosure;
import com.payonel.lungolua.object.LocVar;
import com.payonel.lungolua.object.Node;
import com.payonel.lungolua.object.Proto;
import com.payonel.lungolua.object.TString;
import com.payonel.lungolua.object.TValue;
import com.payonel.lungolua.object.Table;
import com.payonel.lungolua.object.Udata;
import com.payonel.lungolua.object.UpVal;
import com.payonel.lungolua.object.Upvaldesc;
import com.payonel.lungolua.runtime.CallInfo;
import com.payonel.lungolua.runtime.ProtectedCall;
import com.payonel.lungolua.runtime.global_State;
import com.payonel.lungolua.runtime.lua_State;

/*
** Styx: persistence of Lua values (see docs/styx.md)
*/
public final class Lstyx
{
  private Lstyx()
  {
  }

  /* value tags in the persisted stream */
  private static final byte TAG_NIL = 0;
  private static final byte TAG_FALSE = 1;
  private static final byte TAG_TRUE = 2;
  private static final byte TAG_INTEGER = 3;
  private static final byte TAG_FLOAT = 4;
  private static final byte TAG_STRING = 5;
  private static final byte TAG_TABLE = 6;
  private static final byte TAG_LCLOSURE = 7;
  private static final byte TAG_THREAD = 8;
  private static final byte TAG_REF = 9;      /* back reference to an object already written */
  private static final byte TAG_PERM = 10;    /* object replaced by its permanent key */
  private static final byte TAG_UPVAL_NEW = 11;
  private static final byte TAG_UPVAL_REF = 12;
  private static final byte TAG_UPVAL_OPEN = 13; /* open upvalue: owner thread, stack level */
  private static final byte TAG_PROTO_NEW = 15;  /* a prototype, written the first time it is met */
  private static final byte TAG_PROTO_REF = 16;  /* a prototype already written */
  private static final byte TAG_STATE = 17;      /* a whole state: seed, registry, type metatables */
  private static final byte TAG_MAINTHREAD = 18; /* the main thread of the state being restored */
  private static final byte TAG_UPVAL_PERM = 19; /* a variable that is a permanent: its key */
  private static final byte TAG_GROUP = 20;      /* a budget group: its limit, steps left, callback, value and thread */
  private static final byte TAG_CFI = 21;        /* a light C function of the engine, by its name */
  private static final byte TAG_USEROBJECT = 22; /* a user object of a registered kind: its state, metatable, user values */
  private static final int GROUP_GONE = 65535; /* an owner id no group has */
  private static final byte TAG_CCLOSURE = 23;   /* a C closure of a registered kind: its state and upvalues */
  private static final byte TAG_MAINTHREAD_FROZEN = 24; /* the main thread, frozen: its frames, into the restoring state's */

  /*
  ** An object's hash (ADR-30) is written as its place in the sequence of the
  ** state's hashes (the k-th object made gets seed + k * HASHSTEP), relative
  ** to the previous object's: objects written one after another were mostly
  ** made one after another, so that is mostly one byte.
  */
  private static int inverse(int a) /* modulo 2^32, 'a' odd */
  {
    int x = a;
    for (int i = 0; i < 5; i++)
      x *= 2 - a * x;
    return x;
  }

  private static final int HASHSTEP_INV = inverse(HASHSTEP);

  static
  {
    assert HASHSTEP * HASHSTEP_INV == 1;
  }

  /*
  ** The walks keep their own work stacks, so they have no depth limit. For a
  ** script's persist or unpersist, and a restore, the stacks are counted in
  ** the state, at the cost given for an item (under the API lock, as an API
  ** function allocates); for a host persist they are host memory, so the
  ** state is not touched. What a stack counted is given back by 'release'
  ** (at its end in C++; by its owner in Java).
  */
  /* the stream's width of an instruction (prototype code is written as its bytes) */
  private static final int W_INSTRUCTION = 4;

  static
  {
    assert Integer.BYTES == W_INSTRUCTION;
  }

  private static final int WS_WORD = 8;    /* an integer or an object */
  private static final int WS_PAIR = 16;   /* two of them */
  private static final int WS_TRIPLE = 24; /* three */
  private static final int WS_PATH = 56;   /* a step of a path */
  private static final int WS_TASK = 128;  /* a task of the walk */
  private static final int WS_FRAME = 104; /* a frame of the restore */

  private interface Struct<T>
  {
    T copy();
  }

  @FunctionalInterface
  private interface Less<T>
  {
    boolean less(T a, T b);
  }

  private static final class WorkStack<T>
  {
    @SafeVarargs
    @SuppressWarnings("varargs")
    WorkStack(lua_State L, boolean charged, int cost, T... none)
    {
      L_ = L;
      charged_ = charged;
      cost_ = cost;
      items_ = none;
    }

    int size()
    {
      return size_;
    }

    boolean empty()
    {
      return size_ == 0;
    }

    void push(T v)
    {
      if (size_ == items_.length)
        grow(Math.max(16, 2 * (size_ + 1)));
      items_[size_++] = copied(v);
    }

    T get(int i)
    {
      return copied(items_[i]);
    }

    T[] data() /* (the items, for a call that takes an array) */
    {
      return items_;
    }

    void set(int i, T v)
    {
      items_[i] = copied(v);
    }

    T top()
    {
      return copied(items_[size_ - 1]);
    }

    void set_top(T v)
    {
      items_[size_ - 1] = copied(v);
    }

    void pop()
    {
      items_[--size_] = null;
    }

    /* room for 'n' more items, so that pushing them does not allocate */
    void reserve(int n)
    {
      if (size_ + n > items_.length)
        grow(size_ + n);
    }

    void sort(Less<T> less)
    {
      Arrays.sort(items_, 0, size_, (a, b) -> less.less(a, b) ? -1 : (less.less(b, a) ? 1 : 0));
    }

    /* the first position whose item is not less than 'v' (the items are sorted) */
    int lower_bound(T v, Less<T> less)
    {
      int lo = 0, hi = size();
      while (lo < hi)
      {
        int mid = lo + (hi - lo) / 2;
        if (less.less(get(mid), v))
          lo = mid + 1;
        else
          hi = mid;
      }
      return lo;
    }

    void release()
    {
      if (counted_ != 0)
      { /* (no lock: it may run as an error passes, with the lock held) */
        luaM_resizeblock(L_, counted_, 0);
        counted_ = 0;
      }
      items_ = Arrays.copyOf(items_, 0);
      size_ = 0;
    }

    private void grow(int cap)
    {
      if (charged_)
      {
        lua_lock(L_);
        luaM_resizeblock(L_, counted_, (long) cap * cost_);
        lua_unlock(L_);
        counted_ = (long) cap * cost_;
      }
      items_ = Arrays.copyOf(items_, cap);
    }

    @SuppressWarnings("unchecked")
    private static <T> T copied(T v)
    {
      return (v instanceof Struct<?> s) ? (T) s.copy() : v;
    }

    private final lua_State L_;
    private final boolean charged_;
    private final int cost_;
    private long counted_ = 0;
    private T[] items_;
    private int size_ = 0;
  }

  /* a value as stored in the walks' records (TValue copies are named) */
  private static final class Slot implements Struct<Slot>
  {
    long prim = 0;
    Object ref = null;
    byte tt = LUA_VNIL;

    static Slot of(TValue o)
    {
      Slot s = new Slot();
      s.prim = o.prim;
      s.ref = o.ref;
      s.tt = o.tt_;
      return s;
    }

    TValue get()
    {
      return new TValue(prim, ref, tt);
    }

    @Override
    public Slot copy()
    {
      Slot s = new Slot();
      s.prim = prim;
      s.ref = ref;
      s.tt = tt;
      return s;
    }
  }

  private static boolean is_name(ByteView s)
  {
    if (s.size() == 0 || !lislalpha(s.at(0) & 0xFF))
      return false;
    for (int i = 1; i < s.size(); i++)
      if (!lislalnum(s.at(i) & 0xFF))
        return false;
    return true;
  }

  private static String text(ByteView b)
  {
    return new String(b.array(), b.offset(), b.size(), StandardCharsets.ISO_8859_1);
  }

  /* 'fmt' with each "%s" or "%d" replaced by the next argument */
  private static String format(ByteView fmt, String... args)
  {
    StringBuilder r = new StringBuilder();
    int a = 0;
    for (int i = 0; i < fmt.size(); i++)
    {
      if (fmt.at(i) == '%' && i + 1 < fmt.size() && (fmt.at(i + 1) == 's' || fmt.at(i + 1) == 'd') &&
          a != args.length)
      {
        r.append(args[a++]);
        i++;
      }
      else
        r.append((char) (fmt.at(i) & 0xFF));
    }
    return r.toString();
  }

  /* the raw value of a string field of a table (nil if absent) */
  private static TValue string_field(Table t, ByteView name)
  {
    TValue r = new TValue();
    setnilvalue(r);
    for (int i = 0; i < sizenode(t); i++)
    {
      Node n = gnode(t, i);
      if (!isempty(n) && (n.key_tt & NOVARIANT_MASK) == LUA_TSTRING &&
          ((TString) n.key_ref).data.view().equals(name))
      {
        copyobj(r, n);
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

  private static final class HostError extends RuntimeException
  {
    final String message;

    HostError(String message)
    {
      super(null, null, false, false);
      this.message = message;
    }
  }

  /* how the walk reached a value, for error paths */
  private enum Step
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
  }

  private static final class PathRec implements Struct<PathRec>
  {
    Step step = Step.None;
    Slot key = new Slot();
    Proto proto = null;
    int n = 0;
    ByteView label = ByteView.EMPTY;

    @Override
    public PathRec copy()
    {
      PathRec r = new PathRec();
      r.step = step;
      r.key = key.copy();
      r.proto = proto;
      r.n = n;
      r.label = label;
      return r;
    }
  }

  private enum Op
  {
    Value,      /* write 'v' */
    PopPath,    /* the value whose step is on top of the path stack is done */
    Byte,       /* write 'i' as one byte */
    I64,        /* write 'i' */
    LineInfo,   /* write the line information of prototype 'p' */
    Var,        /* write 'i' as a variable-length integer */
    Proto,      /* write prototype 'p' */
    Upvalue,    /* write upvalue 'i' of closure 'p' */
    TableNext,  /* write the entries of table 'p' from position 'i' ('n': array only) */
    NodeNext,   /* write node 'i' on of table 'p', which has 'n' nodes ('v': its id; 'nodes') */
    TableValue, /* write the value at position 'i' of table 'p' */
    ThreadSlot, /* write slots 'i' to 'n' of thread 'p' */
    ThreadTail, /* write the frames, to-be-closed list and hook of thread 'p' */
  }

  /* NodeNext: the dead keys among the nodes of the current word, and the
     table's dead-node record, plus one (0: none yet) */
  private static final class NodeWalk implements Struct<NodeWalk>
  {
    long bits;
    long rec;

    NodeWalk()
    {
    }

    NodeWalk(long bits, long rec)
    {
      this.bits = bits;
      this.rec = rec;
    }

    @Override
    public NodeWalk copy()
    {
      return new NodeWalk(bits, rec);
    }
  }

  private static final class Task implements Struct<Task>
  {
    Op op = Op.Byte;
    PathRec step = new PathRec();   /* how the value was reached */
    NodeWalk nodes = new NodeWalk(); /* (a node walk's) */
    Slot v = new Slot();
    long i = 0;
    long n = 0;
    GCObject p = null; /* a Proto, LClosure, Table or lua_State */

    @Override
    public Task copy()
    {
      Task t = new Task();
      t.op = op;
      t.step = step.copy();
      t.nodes = nodes.copy();
      t.v = v.copy();
      t.i = i;
      t.n = n;
      t.p = p;
      return t;
    }
  }

  /*
  ** A filter of addresses: 'maybe' is true for every address added, and for
  ** few others (16 bits per address). It never dereferences an address.
  */
  private static final class AddressFilter
  {
    AddressFilter(lua_State L, boolean charged, int n)
    {
      words_ = new WorkStack<>(L, charged, WS_WORD);
      while ((1L << lgbits_) < 16L * n && lgbits_ < 40)
        lgbits_++;
      int nwords = (int) (1L << (lgbits_ - 6));
      words_.reserve(nwords);
      for (int i = 0; i < nwords; i++)
        words_.push(0L);
    }

    void release()
    {
      words_.release();
    }

    void add(Object p)
    {
      long b = slot(p);
      words_.set((int) (b / 64), words_.get((int) (b / 64)) | (1L << (b % 64)));
    }

    boolean maybe(Object p)
    {
      long b = slot(p);
      return ((words_.get((int) (b / 64)) >>> (b % 64)) & 1) != 0;
    }

    private final WorkStack<Long> words_;
    private int lgbits_ = 6;

    private long slot(Object p)
    {
      long h = (Integer.toUnsignedLong(point2uint(p)) >>> 3) * 0x9E3779B97F4A7C15L;
      return h >>> (64 - lgbits_);
    }
  }

  private static final class ByteString
  {
    private byte[] a = new byte[0];
    private int n = 0;

    void push_back(byte c)
    {
      reserve(n + 1);
      a[n++] = c;
    }

    void append(ByteView b)
    {
      reserve(n + b.size());
      System.arraycopy(b.array(), b.offset(), a, n, b.size());
      n += b.size();
    }

    int size()
    {
      return n;
    }

    boolean empty()
    {
      return n == 0;
    }

    void clear()
    {
      n = 0;
    }

    ByteView view()
    {
      return new ByteView(a, 0, n);
    }

    private void reserve(int m)
    {
      if (m > a.length)
        a = Arrays.copyOf(a, Math.max(m, 2 * a.length));
    }
  }

  private static final class Walker
  {
    /* a script's persist: output to 'B', references in the table at 'refs' */
    Walker(lua_State L, Table perms, luaL_Buffer B, int refs)
    {
      L_ = L;
      host_ = false;
      perms_ = perms;
      B_ = B;
      refs_ = refs;
      tasks_ = new WorkStack<>(L, true, WS_TASK);
      paths_ = new WorkStack<>(L, true, WS_PATH);
      marked_ = new WorkStack<>(L, true, WS_PAIR);
      dead_large_ = new WorkStack<>(L, true, WS_TRIPLE);
      dead_ = new WorkStack<>(L, true, WS_PAIR);
      dead_bits_ = new WorkStack<>(L, true, WS_WORD);
    }

    /* a host persist: output to 'w', nothing allocated in the state */
    Walker(lua_State L, Table perms, lua_Writer w)
    {
      L_ = L;
      host_ = true;
      perms_ = perms;
      w_ = w;
      tasks_ = new WorkStack<>(L, false, WS_TASK);
      paths_ = new WorkStack<>(L, false, WS_PATH);
      marked_ = new WorkStack<>(L, false, WS_PAIR);
      dead_large_ = new WorkStack<>(L, false, WS_TRIPLE);
      dead_ = new WorkStack<>(L, false, WS_PAIR);
      dead_bits_ = new WorkStack<>(L, false, WS_WORD);
    }

    void release()
    {
      dead_bits_.release();
      dead_.release();
      dead_large_.release();
      marked_.release();
      paths_.release();
      tasks_.release();
    }

    void root(TValue v, ByteView label)
    {
      Task t = new Task();
      t.op = Op.Value;
      t.step.step = Step.Root;
      t.step.label = label;
      t.v = Slot.of(v);
      tasks_.push(t);
      run();
    }

    /* the stream's fixed widths (little-endian, ADR-10) */
    static final int W_INT = 8;  /* an integer or a float */
    static final int W_HASH = 4; /* a hash */

    void byte_kw(int c)
    {
      if (host_)
        buf_.push_back((byte) c);
      else
        luaL_addchar(B_, (byte) c);
    }

    void raw(ByteView b)
    {
      out(b);
    }

    void i64(long v)
    {
      ByteOwner b = new ByteOwner(W_INT);
      b.write_int64(v, 0);
      out(b.view());
    }

    void f64(double n)
    {
      ByteOwner b = new ByteOwner(W_INT);
      b.write_double(n, 0);
      out(b.view());
    }

    void u32(int v)
    {
      ByteOwner b = new ByteOwner(W_HASH);
      b.write_int32(v, 0);
      out(b.view());
    }

    /* an integer in as few bytes as it needs (zigzag, 7 bits per byte) */
    void var(long v)
    {
      long u = (v << 1) ^ (v >> 63);
      do
      {
        byte c = (byte) (u & 0x7F);
        u >>>= 7;
        if (u != 0)
          c = (byte) (c | 0x80);
        byte_kw(c);
      } while (u != 0);
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
      long start = position();
      marks();
      dead_keys();
      long len = position() - start;
      byte[] b = new byte[10];
      int n = 0;
      do
      {
        byte c = (byte) (len & 0x7F);
        len >>>= 7;
        if (len != 0)
          c = (byte) (c | 0x80);
        b[n++] = c;
      } while (len != 0);
      for (int i = n; i > 0; i--)
        byte_kw(b[i - 1]);
    }

    /*
    ** A state's objects marked for finalization, garbage or not: they are part
    ** of the state until their finalizers run (ADR-28). Permanents are the
    ** restoring host's own objects.
    */
    /* a whole state is persisted (the kernel's functions are part of it) */
    void whole()
    {
      whole_ = true;
    }

    void marked_objects()
    {
      ArrayList<GCObject> list = new ArrayList<>(); /* host memory: a state persist is a host's */
      each_marked((o, due) ->
      {
        TValue v = new TValue(), key = new TValue();
        setgcovalue(L_, v, o);
        if (!perm_key(v, key))
          list.add(o);
      });
      var(list.size());
      for (GCObject o : list)
      {
        TValue v = new TValue();
        setgcovalue(L_, v, o);
        root(v, Lt.path_finalizer_list);
      }
    }

    /* the thread the state is persisted from, which luaL_restorestate gives
       back restored: the main thread, or a thread the state holds (the
       host's other thread, for a frozen main thread) */
    void persisting_thread()
    {
      TValue v = new TValue();
      setthvalue(L_, v, L_);
      if (L_ != mainthread(L_.l_G) && !ref_find(v, gcvalue(v)).isPresent())
        throw new HostError(text(Lt.persist_from_the_main_thread_or_a_thread_the_state_holds)); /* (no path: not a value's) */
      root(v, Lt.path_persisting_thread);
    }

    /* the collector's settings that a script sees: its mode, whether it is
       stopped, and its parameters (as stored, so they read back the same) */
    void collector()
    {
      global_State g = L_.l_G;
      byte_kw(g.gckind == KGC_INC ? 0 : 1);
      byte_kw((g.gcstp & GCSTPUSR) != 0 ? 1 : 0);
      for (int p = 0; p < LUA_GCPN; p++)
        byte_kw((byte) g.gcparams[p]);
    }

    /* the hash the state's next object gets (ADR-30) */
    void next_hash()
    {
      u32(L_.l_G.nexthash);
    }

    boolean check_ = false; /* (only checks what it could write, while the kernel runs) */
    boolean found_ = false; /* (what a walk of the collector's lists found) */
    int count_ = 0;         /* (and counted) */
    private final lua_State L_;
    private final boolean host_;
    private final Table perms_;
    private boolean whole_ = false; /* a whole state, not a value */
    private luaL_Buffer B_ = null;
    private int refs_ = 0;
    private lua_Writer w_ = null;
    private final ByteString buf_ = new ByteString();

    private static final class HostRef
    {
      long id;
      byte tt;

      HostRef(long id, byte tt)
      {
        this.id = id;
        this.tt = tt;
      }
    }

    private final IdentityHashMap<Object, HostRef> map_ = new IdentityHashMap<>();
    private long refcount_ = 0;
    private long flushed_ = 0; /* bytes handed to the writer */
    private int lasthash_ = 0; /* the place of the last hash written */
    private final WorkStack<Task> tasks_;
    private final WorkStack<PathRec> paths_;

    /* a table written as such that is marked for finalization */
    private static final class Marked implements Struct<Marked>
    {
      GCObject o;
      long id;

      Marked(GCObject o, long id)
      {
        this.o = o;
        this.id = id;
      }

      @Override
      public Marked copy()
      {
        return new Marked(o, id);
      }
    }

    private final WorkStack<Marked> marked_;

    /* the nodes of a table written slot by slot that were dead keys, as bits:
       a table of up to 64 nodes has them in its record, a larger one in 'n'
       bits of 'dead_bits_' from word 'bits' */
    private static final class DeadNodesLarge implements Struct<DeadNodesLarge>
    {
      Table h;
      long n;
      long bits;

      DeadNodesLarge(Table h, long n, long bits)
      {
        this.h = h;
        this.n = n;
        this.bits = bits;
      }

      @Override
      public DeadNodesLarge copy()
      {
        return new DeadNodesLarge(h, n, bits);
      }
    }

    private static final class DeadNodes implements Struct<DeadNodes>
    {
      Table h;
      long bits;

      DeadNodes(Table h, long bits)
      {
        this.h = h;
        this.bits = bits;
      }

      @Override
      public DeadNodes copy()
      {
        return new DeadNodes(h, bits);
      }
    }

    private final WorkStack<DeadNodesLarge> dead_large_;
    private final WorkStack<DeadNodes> dead_;
    private final WorkStack<Long> dead_bits_;

    /* a persisted object, by address */
    private static final class ObjectRef implements Struct<ObjectRef>
    {
      Object p;
      long id;

      ObjectRef(Object p, long id)
      {
        this.p = p;
        this.id = id;
      }

      @Override
      public ObjectRef copy()
      {
        return new ObjectRef(p, id);
      }
    }

    /* a dead key that is a persisted object */
    private static final class DeadFix implements Struct<DeadFix>
    {
      long table;
      long node;
      long object;

      DeadFix(long table, long node, long object)
      {
        this.table = table;
        this.node = node;
        this.object = object;
      }

      @Override
      public DeadFix copy()
      {
        return new DeadFix(table, node, object);
      }
    }

    @FunctionalInterface
    private interface MarkedFn
    {
      void call(GCObject o, boolean due);
    }

    @FunctionalInterface
    private interface DeadKeyFn
    {
      void call(Object p, Table h, long node);
    }

    @FunctionalInterface
    private interface WordFn
    {
      void call(Table h, long w, long bits);
    }

    @FunctionalInterface
    private interface ObjectFn
    {
      void call(Object p, long id);
    }

    @FunctionalInterface
    private interface AddFn
    {
      void add(String s);
    }

    private static final int FLUSH_SIZE = 64 * 1024;

    private void out(ByteView b)
    {
      if (host_)
        buf_.append(b);
      else
        luaL_addlstring(B_, b, 0, b.size());
    }

    private void flush(boolean end)
    {
      if (!buf_.empty())
      {
        if (w_.write(L_, buf_.view()) != 0)
          throw new HostError(text(Lt.persist_output_stopped_by_the_writer));
        flushed_ += buf_.size();
        buf_.clear();
      }
      if (end && w_.write(L_, null) != 0)
        throw new HostError(text(Lt.persist_output_stopped_by_the_writer));
    }

    /* ---- the trailer ---- */

    /* bytes written so far */
    private long position()
    {
      int n = host_ ? buf_.size() : luaL_bufflen(B_);
      return flushed_ + n;
    }

    /* every object the collector would finalize, in the order it would call
       their finalizers: those waiting for it, then the marked, newest first */
    private void each_marked(MarkedFn f)
    {
      global_State g = L_.l_G;
      for (GCObject o = g.tobefnz.next; o != null; o = o.next)
        f.call(o, true);
      for (BudgetGroup bg : g.groupbyid) /* (and the ones due in a group) */
        if (bg != null)
          for (GCObject o = bg.fnz.next; o != null; o = o.next)
            f.call(o, true);
      for (GCObject o = g.finobj.next; o != null; o = o.next)
        f.call(o, false);
    }

    /*
    ** Who a marked object belongs to, in the trailer: 0 for the kernel and the
    ** host, 1 for a group that is gone (the object is then an ordinary one
    ** when it is collected, as in the persisting state), and 2 plus the
    ** reference id of its group.
    */
    private long owner_code(GCObject o)
    {
      if (o.owner == 0)
        return 0;
      global_State g = L_.l_G;
      BudgetGroup bg = (o.owner < g.groupbyid.length) ? g.groupbyid[o.owner] : null;
      if (bg == null)
        return 1;
      TValue v = new TValue();
      setgcovalue(L_, v, bg);
      OptionalLong found = ref_find(v, bg);
      if (!found.isPresent())
        return 1; /* (a group nothing persisted refers to is gone, as far as the restored state can tell) */
      return 2 + found.getAsLong();
    }

    /* the persisted tables marked for finalization, oldest mark first */
    private void marks()
    {
      int n = marked_.size();
      if (n == 1 && !tofinalize(marked_.get(0).o)) /* finalized meanwhile */
        n = 0;
      if (n <= 1)
      {
        var(n);
        if (n == 1)
        {
          var(marked_.get(0).o.tt == LUA_VTABLE ? 0 : 1);
          var(marked_.get(0).id);
          var(owner_code(marked_.get(0).o));
          found_ = false; /* (a field, set by the walk: in Java, a lambda sets no local) */
          each_marked((o, d) -> { if (o == marked_.get(0).o && d) found_ = true; });
          var(found_ ? 1 : 0);
        }
        return;
      }
      /* their order is the collector's: nothing below allocates while its
         lists are read */
      Less<Marked> by_id = (a, b) -> a.id < b.id;
      marked_.sort(by_id);
      WorkStack<Long> order = new WorkStack<>(L_, !host_, WS_WORD);
      WorkStack<Long> kinds = new WorkStack<>(L_, !host_, WS_WORD);
      WorkStack<Long> owners = new WorkStack<>(L_, !host_, WS_WORD);
      WorkStack<Long> dues = new WorkStack<>(L_, !host_, WS_WORD);
      try
      {
        order.reserve(n);
        kinds.reserve(n);
        owners.reserve(n);
        dues.reserve(n);
        each_marked((o, due) ->
        {
          TValue v = new TValue();
          setgcovalue(L_, v, o);
          OptionalLong id = ref_find(v, o);
          if (!id.isPresent())
            return;
          int i = marked_.lower_bound(new Marked(null, id.getAsLong()), by_id);
          if (i < marked_.size() && marked_.get(i).id == id.getAsLong())
          {
            order.push(marked_.get(i).id);
            kinds.push(o.tt == LUA_VTABLE ? 0L : 1L);
            owners.push(owner_code(o));
            dues.push(due ? 1L : 0L);
          }
        });
        var(order.size());
        for (int i = kinds.size(); i > 0; i--)
          var(kinds.get(i - 1));
        for (int i = order.size(); i > 0; i--)
          var(order.get(i - 1));
        for (int i = owners.size(); i > 0; i--)
          var(owners.get(i - 1));
        for (int i = dues.size(); i > 0; i--)
          var(dues.get(i - 1));
      }
      finally
      {
        dues.release();
        owners.release();
        kinds.release();
        order.release();
      }
    }

    /* every node written as a dead key that still is one: f(key, table, node) */
    private void each_dead_key(DeadKeyFn f)
    {
      WordFn word = (h, w, bits) ->
      {
        for (int b = 0; bits != 0; b++, bits >>>= 1)
          if ((bits & 1) != 0)
          {
            long j = w * 64 + b;
            if (j >= sizenode(h)) /* a table resized since it was written */
              return;
            Node nd = gnode(h, (int) j);
            if (keyisdead(nd))
              f.call(nd.key_ref, h, j);
          }
      };
      for (int r = 0; r < dead_.size(); r++)
      {
        DeadNodes d = dead_.get(r);
        word.call(d.h, 0, d.bits);
      }
      for (int r = 0; r < dead_large_.size(); r++)
      {
        DeadNodesLarge d = dead_large_.get(r);
        for (long w = 0; w < (d.n + 63) / 64; w++)
          word.call(d.h, w, dead_bits_.get((int) (d.bits + w)));
      }
    }

    /* every persisted object (permanents included): f(address, id) */
    private void each_object(ObjectFn f)
    {
      if (host_)
      {
        for (Map.Entry<Object, HostRef> e : map_.entrySet())
          if ((e.getValue().tt & BIT_ISCOLLECTABLE) != 0)
            f.call(e.getKey(), e.getValue().id);
        return;
      }
      TValue rt = getslot(L_, L_.ci.func + refs_);
      Table refs = hvalue(rt);
      for (int i = 0; i < sizenode(refs); i++)
      {
        Node nd = gnode(refs, i);
        if (!isempty(nd) && keyiscollectable(nd) && ttisinteger(nd))
          f.call(nd.key_ref, ivalue(nd));
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
    private void dead_keys()
    {
      int nkeys = 0; /* written as dead keys (some may be no longer) */
      for (int r = 0; r < dead_.size(); r++)
        nkeys += Long.bitCount(dead_.get(r).bits);
      for (int w = 0; w < dead_bits_.size(); w++)
        nkeys += Long.bitCount(dead_bits_.get(w));
      if (nkeys == 0)
      {
        var(0);
        return;
      }
      boolean few_keys = nkeys <= refcount_;
      AddressFilter keys = new AddressFilter(L_, !host_, few_keys ? nkeys : 0);
      try
      {
        if (few_keys)
          each_dead_key((p, h, node) -> keys.add(p));
        Predicate<Object> candidate = p -> !few_keys || keys.maybe(p);
        count_ = 0; /* (a field, counted by the walk) */
        each_object((p, id) -> { if (candidate.test(p)) count_++; });
        int ncand = count_;
        if (ncand == 0)
        {
          var(0);
          return;
        }
        WorkStack<ObjectRef> cand = new WorkStack<>(L_, !host_, WS_PAIR);
        try
        {
          cand.reserve(ncand); /* nothing below allocates while the objects are read */
          each_object((p, id) -> { if (candidate.test(p)) cand.push(new ObjectRef(p, id)); });
          Less<ObjectRef> by_address = (a, b) -> Integer.compareUnsigned(point2uint(a.p), point2uint(b.p)) < 0;
          cand.sort(by_address);
          AddressFilter objects = new AddressFilter(L_, !host_, ncand);
          try
          {
            for (int i = 0; i < cand.size(); i++)
              objects.add(cand.get(i).p);
            WorkStack<DeadFix> fixes = new WorkStack<>(L_, !host_, WS_TRIPLE);
            try
            {
              each_dead_key((p, h, node) ->
              {
                if (!objects.maybe(p))
                  return;
                int k = cand.lower_bound(new ObjectRef(p, 0), by_address);
                while (k < cand.size() && cand.get(k).p != p && !by_address.less(new ObjectRef(p, 0), cand.get(k)))
                  k++;
                if (k < cand.size() && cand.get(k).p == p)
                {
                  TValue t = new TValue();
                  sethvalue(L_, t, h);
                  fixes.push(new DeadFix(ref_find(t, h).orElse(0), node, cand.get(k).id));
                }
              });
              var(fixes.size());
              for (int i = 0; i < fixes.size(); i++)
              {
                DeadFix f = fixes.get(i);
                var(f.table);
                var(f.node);
                var(f.object);
              }
            }
            finally
            {
              fixes.release();
            }
          }
          finally
          {
            objects.release();
          }
        }
        finally
        {
          cand.release();
        }
      }
      finally
      {
        keys.release();
      }
    }

    /* ---- work ---- */

    private void push_value(TValue v, Step step, TValue key, Proto proto, int n)
    {
      Task t = new Task();
      t.op = Op.Value;
      t.step.step = step;
      if (key != null)
        t.step.key = Slot.of(key);
      t.step.proto = proto;
      t.step.n = n;
      t.v = Slot.of(v);
      tasks_.push(t);
    }

    private void push_value(TValue v, Step step, TValue key)
    {
      push_value(v, step, key, null, 0);
    }

    private void push_value(TValue v, Step step)
    {
      push_value(v, step, null, null, 0);
    }

    private void push_op(Op op, long i, GCObject p, long n)
    {
      Task t = new Task();
      t.op = op;
      t.i = i;
      t.p = p;
      t.n = n;
      tasks_.push(t);
    }

    private void push_op(Op op, long i, GCObject p)
    {
      push_op(op, i, p, 0);
    }

    private void push_op(Op op, long i)
    {
      push_op(op, i, null, 0);
    }

    private void push_op(Op op)
    {
      push_op(op, 0, null, 0);
    }

    private void push_name(TString ts)
    {
      TValue v = new TValue();
      if (ts != null)
        setsvalue(L_, v, ts);
      else
        setnilvalue(v);
      push_value(v, Step.None);
    }

    /* enters the step of a value: it stays on the path until the value is done */
    private void enter(PathRec step)
    {
      if (step.step == Step.None)
        return;
      paths_.push(step);
      push_op(Op.PopPath);
    }

    private void run()
    {
      while (!tasks_.empty())
      {
        Task t = tasks_.top();
        tasks_.pop();
        switch (t.op)
        {
        case Value:
        {
          enter(t.step);
          TValue v = t.v.get();
          value(v);
          break;
        }
        case PopPath:
          paths_.pop();
          break;
        case Byte:
          byte_kw((byte) t.i);
          break;
        case I64:
          i64(t.i);
          break;
        case Var:
          var(t.i);
          break;
        case LineInfo:
          out(new ByteView(((Proto) t.p).lineinfo));
          break;
        case Proto:
          proto((Proto) t.p);
          break;
        case Upvalue:
          enter(t.step);
          upvalue((LClosure) t.p, (int) t.i);
          break;
        case TableNext:
          table_next((Table) t.p, t.i, t.n != 0);
          break;
        case NodeNext:
          node_next(t);
          break;
        case TableValue:
        {
          enter(t.step);
          TValue v = table_value((Table) t.p, t.i);
          value(v);
          break;
        }
        case ThreadSlot:
          thread_slot((lua_State) t.p, t.i, t.n);
          break;
        case ThreadTail:
          thread_tail((lua_State) t.p);
          break;
        }
        if (host_ && buf_.size() >= FLUSH_SIZE)
          flush(false);
      }
    }

    /* ---- errors ---- */

    private void key_text(TValue k, AddFn add)
    {
      switch (ttype(k))
      {
      case LUA_TSTRING:
      {
        ByteView s = tsvalue(k).data.view();
        add.add(format(is_name(s) ? Lt.path_field_s : Lt.path_string_key_s, text(s)));
        return;
      }
      case LUA_TNUMBER:
      {
        ByteOwner b = new ByteOwner(LUA_N2SBUFFSZ);
        int len = luaO_tostringbuff(k, b.span());
        add.add(format(Lt.path_index_s, text(b.view().substr(0, len))));
        return;
      }
      case LUA_TBOOLEAN:
        add.add(format(Lt.path_index_s, tagisfalse(k.tt_) ? "false" : "true"));
        return;
      default:
        add.add(format(Lt.path_object_key_s, text(ttypename(ttype(k)))));
        return;
      }
    }

    private void path_text(AddFn add)
    {
      for (int i = 0; i < paths_.size(); i++)
      {
        PathRec r = paths_.get(i);
        TValue k = r.key.get();
        switch (r.step)
        {
        case None:
          break;
        case Root:
          add.add(text(r.label));
          break;
        case Value:
          key_text(k, add);
          break;
        case Key:
          add.add(format(Lt.path_key_s, text(ttypename(ttype(k)))));
          break;
        case Metatable:
          add.add(text(Lt.path_metatable));
          break;
        case Upvalue:
        {
          TString name = (r.proto != null) ? r.proto.upvalues[r.n - 1].name : null;
          if (name != null && name.data.size() > 0)
            add.add(format(Lt.path_upvalue_s, text(name.data.view())));
          else
            add.add(format(Lt.path_upvalue_d, Integer.toString(r.n)));
          break;
        }
        case Stack:
          add.add(format(Lt.path_stack_d, Integer.toString(r.n)));
          break;
        case PermKey:
          add.add(text(Lt.path_permanent_key));
          break;
        case Hook:
          add.add(text(Lt.path_hook));
          break;
        case Group:
          add.add(text(Lt.path_budget_group));
          break;
        case Chain:
          add.add(text(Lt.path_frozen_chain));
          break;
        case Meta:
          add.add(text(Lt.path_metatable));
          break;
        case UserValue:
          add.add(format(Lt.path_user_value_d, Integer.toString(r.n)));
          break;
        }
      }
    }

    /* raises 'what' with the path of the value at hand */
    private RuntimeException fail(ByteView what)
    {
      if (host_)
      {
        StringBuilder m = new StringBuilder(text(what) + text(Lt.path_at));
        path_text(s -> m.append(s));
        m.append(text(Lt.path_end));
        throw new HostError(m.toString());
      }
      luaL_Buffer E = new luaL_Buffer();
      try
      {
        luaL_buffinit(L_, E);
        AddFn add = s ->
        {
          byte[] b = s.getBytes(StandardCharsets.ISO_8859_1);
          luaL_addlstring(E, new ByteView(b), 0, b.length);
        };
        luaL_addlstring(E, what, 0, what.size());
        add.add(text(Lt.path_at));
        path_text(add);
        add.add(text(Lt.path_end));
        luaL_pushresult(E);
        lua_error(L_);
      }
      finally
      {
        E.release();
      }
      throw new AssertionError();
    }

    private RuntimeException fail(String what)
    {
      return fail(new ByteView(what.getBytes(StandardCharsets.ISO_8859_1)));
    }

    /* ---- references ---- */

    private OptionalLong ref_find(TValue key, Object id)
    {
      if (host_)
      {
        HostRef it = map_.get(id);
        if (it == null)
          return OptionalLong.empty();
        return OptionalLong.of(it.id);
      }
      TValue rt = getslot(L_, L_.ci.func + refs_);
      TValue r = new TValue();
      byte tag = hvalue(rt).luaH_get(key, r);
      if (tagisempty(tag) || !ttisinteger(r))
        return OptionalLong.empty();
      return OptionalLong.of(ivalue(r));
    }

    private void ref_add(TValue key, Object id)
    {
      long n = ++refcount_;
      if (host_)
      {
        map_.putIfAbsent(id, new HostRef(n, key.tt_));
        return;
      }
      luaL_checkstacki(L_, 2, null);
      setslot(L_, L_.top, key);
      L_.top++;
      lua_pushinteger(L_, n);
      lua_rawset(L_, refs_);
    }

    /* an internal object (prototype, upvalue) as a reference key */
    private static TValue pointer_key(Object p)
    {
      TValue k = new TValue();
      setpvalue(k, p);
      return k;
    }

    private boolean perm_key(TValue v, TValue key)
    {
      if (perms_ == null)
        return false;
      byte tag = perms_.luaH_get(v, key);
      return !tagisempty(tag) && !ttisnil(key);
    }

    /* reference or permanent: true if the object was fully written */
    private boolean begin_object(TValue v)
    {
      Object id = v.ref;
      OptionalLong found = ref_find(v, id);
      if (found.isPresent())
      {
        byte_kw(TAG_REF);
        var(found.getAsLong());
        return true;
      }
      ref_add(v, id);
      TValue key = new TValue();
      if (perm_key(v, key))
      {
        byte_kw(TAG_PERM);
        byte_kw(ttype(v));
        push_value(key, Step.PermKey);
        return true;
      }
      return false;
    }

    /*
    ** A light userdata is written only as a permanent, never as a reference:
    ** its address can be that of a prototype or a variable written before
    ** (debug.upvalueid gives one), which are no values
    */
    private void light_userdata(TValue v)
    {
      TValue key = new TValue();
      if (!perm_key(v, key))
        fail(Lt.persisting_a_userdata_value_is_not_supported_yet);
      ++refcount_; /* (the restore gives each permanent an id) */
      byte_kw(TAG_PERM);
      byte_kw(ttype(v));
      push_value(key, Step.PermKey);
    }

    /* ---- values ---- */

    private void value(TValue v)
    {
      switch (ttype(v))
      {
      case LUA_TNIL:
        byte_kw(TAG_NIL);
        return;
      case LUA_TBOOLEAN:
        byte_kw(tagisfalse(v.tt_) ? TAG_FALSE : TAG_TRUE);
        return;
      case LUA_TNUMBER:
        if (ttisinteger(v))
        {
          byte_kw(TAG_INTEGER);
          var(ivalue(v));
        }
        else
        {
          double n = fltvalue(v);
          byte_kw(TAG_FLOAT);
          f64(n);
        }
        return;
      case LUA_TLIGHTUSERDATA:
        light_userdata(v);
        return;
      case LUA_TSTRING:
      case LUA_TTABLE:
      case LUA_TFUNCTION:
      case LUA_TTHREAD:
      case LUA_TUSERDATA:
      case LUA_TBUDGET:
        break;
      default:
        fail(format(Lt.cannot_persist_a_s_value, text(ttypename(ttype(v)))));
      }
      if (begin_object(v))
        return;
      switch (ttype(v))
      {
      case LUA_TSTRING:
      {
        ByteView s = tsvalue(v).data.view();
        byte_kw(TAG_STRING);
        var(s.size());
        out(s);
        return;
      }
      case LUA_TUSERDATA:
        userobject(uvalue(v));
        return;
      case LUA_TTABLE:
        table(hvalue(v));
        return;
      case LUA_TFUNCTION:
        if (ttisLclosure(v))
          closure(clLvalue(v));
        else
          cfunction(v);
        return;
      case LUA_TBUDGET:
        group((BudgetGroup) gcvalue(v));
        return;
      default:
        thread(thvalue(v));
        return;
      }
    }

    /*
    ** A user object of a registered kind: the name of its kind, its
    ** accounting size, its state as integers and bytes, then its metatable and
    ** user values. Any other userdata is for the permanents.
    */
    private void userobject(Udata u)
    {
      long id = refcount_; /* the object's, taken just before */
      String kind = (u.obj != null) ? u.obj.persistkind() : null;
      if (kind == null)
        fail(Lt.persisting_a_userdata_value_is_not_supported_yet);
      if (tofinalize(u))
        marked_.push(new Marked(u, id)); /* (its mark and owner, in the trailer) */
      long[] ints = new long[8];
      int n = u.obj.persistints(ints);
      ByteView bytes = u.obj.persistbytes();
      byte_kw(TAG_USEROBJECT);
      hash(u);
      name(kind);
      var(u.objsize);
      var(u.uv.length);
      var(n);
      for (int i = 0; i < n; i++)
        var(ints[i]);
      var(bytes.size());
      out(bytes);
      boolean values = u.obj.persistuservalues();
      for (int i = u.uv.length; i-- > 0;)
      {
        TValue v = new TValue();
        if (values)
          v = getobj(u.uv[i]);
        else
          setnilvalue(v);
        Task t = new Task();
        t.op = Op.Value;
        t.step.step = Step.UserValue;
        t.step.n = i + 1;
        t.v = Slot.of(v);
        tasks_.push(t);
      }
      TValue mt = new TValue();
      if (u.metatable != null)
        sethvalue(L_, mt, u.metatable);
      else
        setnilvalue(mt);
      push_value(mt, Step.Meta);
    }

    /* a name, as a length and its bytes */
    private void name(String n)
    {
      byte[] b = n.getBytes(StandardCharsets.ISO_8859_1);
      int len = b.length;
      var(len);
      out(new ByteView(b, 0, len));
    }

    /*
    ** A C function that is part of the engine is written by its name (the
    ** permanents of the persisting host come first, in 'begin_object').
    */
    private void cfunction(TValue v)
    {
      if (ttislcf(v))
      {
        String n = lua_cfiname((CFuncInfo) v.ref);
        if (n != null)
        {
          byte_kw(TAG_CFI);
          name(n);
          return;
        }
      }
      if (ttisCclosure(v))
      {
        CClosure cl = clCvalue(v);
        lua_CClosureKind kind = lua_cclosurekind(cl.f);
        if (kind != null && cl.upvalue.length == kind.nup)
        {
          long[] ints = new long[8];
          int n = (kind.save != null) ? kind.save.call(cl, ints) : 0;
          if (n < 0)
            fail(format(Lt.cannot_persist_a_s_closure_as_it_is, kind.name));
          int nvals = (kind.save != null) ? kind.nup - 1 : kind.nup;
          byte_kw(TAG_CCLOSURE);
          hash(cl);
          name(kind.name);
          var(n);
          for (int i = 0; i < n; i++)
            var(ints[i]);
          for (int i = nvals; i-- > 0;)
          {
            TValue u = getobj(cl.upvalue[i]);
            Task t = new Task();
            t.op = Op.Value;
            t.step.step = Step.Upvalue;
            t.step.n = i + 1;
            t.step.proto = null;
            t.v = Slot.of(u);
            tasks_.push(t);
          }
          return;
        }
      }
      fail(Lt.cannot_persist_a_C_function);
    }

    /*
    ** A budget group: its limit and steps left, the name of its callback (the
    ** debug library's, or one the host named), whether a thaw went on, then
    ** its value and the thread it was set on. The dead objects that wait for
    ** their '__gc' are part of a state's marked objects (the trailer).
    */
    private void group(BudgetGroup bg)
    {
      String n = lua_budgetname(bg.f);
      if (n == null)
        fail(Lt.cannot_persist_a_budget_callback_that_has_no_name);
      TValue key = new TValue();
      if (!whole_ && !ttisnil(bg.cb) && !perm_key(bg.cb, key))
        fail(Lt.cannot_persist_a_budget_callback_that_is_not_a_permanent); /* (the kernel's own function) */
      byte_kw(TAG_GROUP);
      var(bg.budget);
      global_State g = L_.l_G;
      var((g.budgetL != null && g.budgetg == bg) ? bg.budget : bg.left); /* (in its callback, as after it) */
      name(n);
      byte_kw(bg.nodrain ? 1 : 0);
      /* the dead objects waiting for its '__gc' (marked due in the trailer):
         read after its value and its root, so pushed before them */
      WorkStack<GCObject> queue = new WorkStack<>(L_, !host_, WS_WORD);
      try
      {
        for (GCObject o = bg.fnz.next; o != null; o = o.next)
          queue.push(o);
        var(queue.size());
        for (int i = queue.size(); i > 0; i--)
        {
          TValue v = new TValue();
          setgcovalue(L_, v, queue.get(i - 1));
          push_value(v, Step.Group);
        }
        TValue root = new TValue();
        if (bg.root != null)
          setthvalue(L_, root, bg.root);
        else
          setnilvalue(root);
        push_value(root, Step.Group);
        push_value(bg.cb, Step.Group);
      }
      finally
      {
        queue.release();
      }
    }

    /*
    ** The '__persist' field of a table's metatable (read raw): absent or true
    ** persists the table as it is, false forbids it. Nothing else is allowed:
    ** a function would be user code that the persist runs (and its restore
    ** closure, user code that the unpersist runs), outside any budget.
    */
    private void special(Table h)
    {
      if (h.metatable == null)
        return;
      TValue f = string_field(h.metatable, Lt.mm_persist);
      if (ttisnil(f) || (ttisboolean(f) && !tagisfalse(f.tt_)))
        return;
      if (ttisboolean(f))
        fail(Lt.cannot_persist_a_table_its___persist_is_false);
      fail(Lt.mm_persist_must_be_a_boolean);
    }

    private void sethvalue2s(Table h)
    {
      TValue t = new TValue();
      sethvalue(L_, t, h);
      setslot(L_, L_.top, t);
      L_.top++;
    }

    /* an object's hash, given when it was created: it hashes by it as a key */
    private void hash(GCObject o)
    {
      int k = (o.hash - L_.l_G.seed) * HASHSTEP_INV;
      var(k - lasthash_);
      lasthash_ = k;
    }

    private void table(Table h)
    {
      long id = refcount_; /* the table's, taken just before */
      special(h);
      if (tofinalize(h))
        marked_.push(new Marked(h, id));
      /* a hash part is written slot by slot (its layout): keys hash by the
         seed and their content, or by the hash of the object, which is
         written with it, so with the same seed it is restored as it is and
         iterates in the same order (ADR-22, ADR-30) */
      boolean layout = allocsizenode(h) > 0;
      byte_kw(TAG_TABLE);
      hash(h);
      var(h.asize());
      var(allocsizenode(h));
      var(h.asize() > 0 ? h.lenhint : 0);
      byte_kw(layout ? 1 : 0);
      /* the metatable, then the entries in the order of 'next' (in layout
         mode the array part's, then the free-slot position and the nodes) */
      if (layout)
      {
        Task t = new Task();
        t.op = Op.NodeNext;
        t.nodes = new NodeWalk(0, 0);
        t.p = h;
        t.n = sizenode(h);
        t.v.prim = id;
        tasks_.push(t);
      }
      push_op(Op.TableNext, 0, h, layout ? 1 : 0);
      if (h.metatable != null)
      {
        TValue mt = new TValue();
        sethvalue(L_, mt, h.metatable);
        push_value(mt, Step.Metatable);
      }
      else
        push_op(Op.Byte, TAG_NIL);
    }

    /* the next entry from position 'pos' (array part, then hash part) */
    private void table_next(Table h, long pos, boolean array_only)
    {
      int asize = h.asize();
      for (;; pos++)
      {
        TValue k = new TValue();
        if (pos < asize)
        {
          if (tagisempty(getArrTag(h, (int) pos)))
            continue;
          setivalue(k, pos + 1);
        }
        else
        {
          long j = pos - asize;
          if (array_only || j >= sizenode(h))
          {
            byte_kw(TAG_NIL); /* end of entries */
            return;
          }
          Node n = gnode(h, (int) j);
          if (isempty(n))
            continue;
          getnodekey(L_, k, n);
        }
        push_op(Op.TableNext, pos + 1, h, array_only ? 1 : 0);
        Task tv = new Task();
        tv.op = Op.TableValue;
        tv.step.step = Step.Value;
        tv.step.key = Slot.of(k);
        tv.i = pos;
        tv.p = h;
        tasks_.push(tv);
        push_value(k, Step.Key, k);
        return;
      }
    }

    /*
    ** The dead keys among the nodes of word 'w' of a task's table: a table
    ** gets its record, with a word for every 64 nodes, at its first dead key.
    */
    private void dead_word(Task t, long w)
    {
      if (t.nodes.bits == 0)
        return;
      Table h = (Table) t.p;
      if (t.n <= 64)
        dead_.push(new DeadNodes(h, t.nodes.bits));
      else
      {
        if (t.nodes.rec == 0)
        {
          dead_large_.push(new DeadNodesLarge(h, t.n, dead_bits_.size()));
          t.nodes.rec = dead_large_.size();
          for (long k = 0; k < (t.n + 63) / 64; k++)
            dead_bits_.push(0L);
        }
        int first = (int) dead_large_.get((int) (t.nodes.rec - 1)).bits;
        dead_bits_.set(first + (int) w, t.nodes.bits);
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
    private void node_next(Task t)
    {
      Table h = (Table) t.p;
      long j = t.i, n = t.n;
      if (sizenode(h) != n)
        fail(Lt.a_table_changed_while_being_persisted);
      if (j == 0)
        var(h.lastfree);
      if (j == n)
      {
        dead_word(t, (j - 1) / 64);
        return;
      }
      if (j % 64 == 0 && j > 0)
        dead_word(t, j / 64 - 1);
      Node nd = gnode(h, (int) j);
      if (keyisdead(nd))
        t.nodes.bits |= 1L << (j % 64);
      t.i = j + 1;
      tasks_.push(t); /* the next node, after this one's key and value */
      if (keyisnil(nd) || keyisdead(nd))
      {
        byte_kw(keyisnil(nd) ? 0 : 1);
        var(nd.next);
        return;
      }
      byte_kw(2);
      TValue k = new TValue();
      getnodekey(L_, k, nd);
      push_op(Op.Var, nd.next);
      Task tv = new Task();
      tv.op = Op.TableValue;
      tv.step.step = Step.Value;
      tv.step.key = Slot.of(k);
      tv.i = h.asize() + j;
      tv.p = h;
      tasks_.push(tv);
      push_value(k, Step.Key, k);
    }

    /* the value at position 'pos', as it is now (nil if it is gone) */
    private TValue table_value(Table h, long pos)
    {
      TValue v = new TValue();
      setnilvalue(v);
      int asize = h.asize();
      if (pos < asize)
      {
        byte tag = getArrTag(h, (int) pos);
        if (!tagisempty(tag))
          farr2val(h, (int) pos, tag, v);
      }
      else if (pos - asize < sizenode(h))
      {
        Node n = gnode(h, (int) (pos - asize));
        if (!isempty(n))
          copyobj(v, n);
      }
      return v;
    }

    private void closure(LClosure cl)
    {
      byte_kw(TAG_LCLOSURE);
      hash(cl);
      int nup = cl.upvals.length;
      for (int i = nup; i >= 1; i--)
      {
        Task t = new Task();
        t.op = Op.Upvalue;
        t.step.step = Step.Upvalue;
        t.step.proto = cl.p;
        t.step.n = i;
        t.i = i;
        t.p = cl;
        tasks_.push(t);
      }
      push_op(Op.Var, nup);
      push_op(Op.Proto, 0, cl.p);
    }

    private boolean is_running(lua_State T)
    {
      return (!host_ && T == L_) || (T.status == LUA_OK && T.ci != T.base_ci && T.frozen == 0);
    }

    private void upvalue(LClosure cl, int i)
    {
      UpVal uv = cl.upvals[i - 1];
      TValue id = pointer_key(uv);
      OptionalLong found = ref_find(id, uv);
      if (found.isPresent())
      {
        byte_kw(TAG_UPVAL_REF);
        var(found.getAsLong());
        return;
      }
      ref_add(id, uv);
      TValue key = new TValue();
      if (perm_key(id, key)) /* the variable is a permanent */
      {
        byte_kw(TAG_UPVAL_PERM);
        push_value(key, Step.PermKey);
        return;
      }
      if (upisopen(uv) && !is_running(uv.owner))
      {
        byte_kw(TAG_UPVAL_OPEN);
        push_op(Op.Var, uv.stkid);
        TValue th = new TValue();
        setthvalue(L_, th, uv.owner);
        push_value(th, Step.None);
        return;
      }
      byte_kw(TAG_UPVAL_NEW);
      push_value(upvalgetval(uv), Step.None);
    }

    /*
    ** A prototype is written once, field by field; later uses refer to it by
    ** id. Its constants and names are values (strings get reference ids like
    ** any other) and its nested prototypes are prototypes too.
    */
    private void proto(Proto f)
    {
      TValue id = pointer_key(f);
      OptionalLong found = ref_find(id, f);
      if (found.isPresent())
      {
        byte_kw(TAG_PROTO_REF);
        var(found.getAsLong());
        return;
      }
      ref_add(id, f);
      byte_kw(TAG_PROTO_NEW);
      var(f.linedefined);
      var(f.lastlinedefined);
      var(f.numparams);
      byte_kw((byte) (f.flag & ~PF_FIXED));
      var(f.maxstacksize);
      var(f.code.length);
      ByteOwner code = new ByteOwner(f.code.length * W_INSTRUCTION);
      for (int j = 0; j < f.code.length; j++)
        code.write_int32(f.code[j], j * W_INSTRUCTION);
      out(code.view());
      var(f.k.length);
      /* the rest, pushed from last to first */
      for (int j = f.locvars.length; j-- > 0;)
      {
        push_op(Op.Var, f.locvars[j].endpc);
        push_op(Op.Var, f.locvars[j].startpc);
        push_name(f.locvars[j].varname);
      }
      push_op(Op.Var, f.locvars.length);
      for (int j = f.abslineinfo.length; j-- > 0;)
      {
        push_op(Op.Var, f.abslineinfo[j].line);
        push_op(Op.Var, f.abslineinfo[j].pc);
      }
      push_op(Op.Var, f.abslineinfo.length);
      push_op(Op.LineInfo, 0, f);
      push_op(Op.Var, f.lineinfo.length);
      push_name(f.source);
      for (int j = f.p.length; j-- > 0;)
        push_op(Op.Proto, 0, f.p[j]);
      push_op(Op.Var, f.p.length);
      for (int j = f.upvalues.length; j-- > 0;)
      {
        push_name(f.upvalues[j].name);
        push_op(Op.Byte, f.upvalues[j].kind);
        push_op(Op.Var, f.upvalues[j].idx);
        push_op(Op.Byte, f.upvalues[j].instack ? 1 : 0);
      }
      push_op(Op.Var, f.upvalues.length);
      for (int j = f.k.length; j-- > 0;)
        push_value(f.k[j], Step.None);
    }

    private void thread(lua_State T)
    {
      boolean main = T == mainthread(L_.l_G);
      if (main && host_ && T.frozen == 0)
      {
        byte_kw(TAG_MAINTHREAD); /* (the restoring state's own, idle) */
        return;
      }
      if (check_ && is_running(T))
      { /* (a check while the kernel runs: the host persists it at rest) */
        byte_kw(TAG_MAINTHREAD);
        return;
      }
      if (is_running(T))
        fail(Lt.cannot_persist_a_running_thread);
      if (main && !host_) /* (the host persists it: idle, by reference; frozen, whole) */
        fail(Lt.cannot_persist_the_main_thread_from_a_script);
      int top = T.frozen != 0 ? T.frozentop : T.top; /* (a frozen thread's own: see 'luaE_setfrozen') */
      int maxtop = top;
      if (T.ci != T.base_ci)
      {
        for (CallInfo ci = T.base_ci.next;; ci = ci.next)
        {
          if (ci.k != null && lua_kname(ci.k) == null)
            fail(format(Lt.cannot_persist_a_thread_with_s, text(Lt.a_continuation_that_has_no_name)));
          if (ci.top > maxtop)
            maxtop = ci.top;
          if (ci == T.ci)
            break;
        }
      }
      if (main)
        byte_kw(TAG_MAINTHREAD_FROZEN); /* (a program runs on it: its frames go with it) */
      else
      {
        byte_kw(TAG_THREAD);
        hash(T);
      }
      byte_kw(T.status);
      var(top);
      var(maxtop);
      push_op(Op.ThreadTail, 0, T);
      push_op(Op.ThreadSlot, 0, T, top);
    }

    /* slots 'i' to 'n' (a slot no longer in use is written as nil) */
    private void thread_slot(lua_State T, long i, long n)
    {
      if (i >= n)
        return;
      push_op(Op.ThreadSlot, i + 1, T, n);
      TValue v = new TValue();
      if (i < T.top)
      {
        TValue s = getslot(T, (int) i);
        copyobj(v, s);
      }
      else
        setnilvalue(v);
      push_value(v, Step.Stack, null, null, (int) i);
    }

    private void thread_tail(lua_State T)
    {
      long nframes = 0;
      if (T.ci != T.base_ci)
        for (CallInfo ci = T.base_ci.next;; ci = ci.next)
        {
          nframes++;
          if (ci == T.ci)
            break;
        }
      i64(nframes);
      if (nframes > 0)
        for (CallInfo ci = T.base_ci.next;; ci = ci.next)
        {
          i64(ci.func);
          i64(ci.top);
          i64(ci.savedpc);
          i64(ci.nextraargs);
          i64(Integer.toUnsignedLong(ci.callstatus));
          i64(ci.old_errfunc);
          i64(ci.ctx);
          i64(ci.funcidx);
          i64(ci.nyield);
          i64(ci.nres);
          /* what a freeze keeps in a frame: the continuation (by name), the
             calls it runs with, how an API call goes on, a hook function in
             progress */
          String kn = (ci.k != null) ? lua_kname(ci.k) : null;
          byte_kw(kn != null ? 1 : 0);
          if (kn != null)
            name(kn);
          var(Integer.toUnsignedLong(ci.nccalls));
          /* (an API call's and a hook's state mean something only while they
             are going on; the rest is stale: not written) */
          boolean api = (ci.callstatus & CIST_APIK) != 0;
          boolean hooked = (ci.callstatus & CIST_HOOKED) != 0;
          byte_kw(api ? ci.apiop : 0);
          var(api ? ci.apifirst : 0);
          byte_kw(hooked ? ci.hookev : 0);
          byte_kw(hooked ? ci.hookdone : 0);
          var(hooked ? ci.hooktop : 0);
          var(hooked ? ci.hookcitop : 0);
          var(hooked ? ci.hookdelta : 0);
          var(hooked ? ci.hookres : 0);
          if (ci == T.ci)
            break;
        }
      i64(T.ntbc);
      for (int k = 0; k < T.ntbc; k++)
        i64(T.tbc[k]);
      /* the extras (after the hook, whose function is a value): the state of a
         frozen thread, then its budget groups and its places in a chain */
      push_thread_values(T);
      thread_extras(T);
      hook(T);
    }

    /* the values after a thread's extras, pushed to be written in this order:
       its own group, its group in effect, the thread that resumed it, the
       thread it resumed (frozen) */
    private void push_thread_values(lua_State T)
    {
      Consumer<lua_State> thread_or_nil = th ->
      {
        TValue v = new TValue();
        if (th != null)
          setthvalue(L_, v, th);
        else
          setnilvalue(v);
        push_value(v, Step.Chain);
      };
      Consumer<BudgetGroup> group_or_nil = bg ->
      {
        TValue v = new TValue();
        if (bg != null)
          setgcovalue(L_, v, bg);
        else
          setnilvalue(v);
        push_value(v, Step.Group);
      };
      thread_or_nil.accept(T.frozenchild);
      thread_or_nil.accept(T.frozenparent);
      group_or_nil.accept(T.beff);
      group_or_nil.accept(T.bown);
    }

    /* the raw state of a frozen thread (written before the debug hook) */
    private void thread_extras(lua_State T)
    {
      byte_kw(T.allowhook ? 1 : 0); /* (off while a drained '__gc' or a hook function runs) */
      var(T.transferinfo.ftransfer); /* (what a call or return hook that runs reads: 'debug.getinfo' "r") */
      var(T.transferinfo.ntransfer);
      byte_kw(T.frozen);
      if (T.frozen == 0)
        return;
      var(T.frozenfunc);
      var(T.frozenerrfunc);
      var(T.frozennres);
      var(Integer.toUnsignedLong(T.frozencc));
      var(Integer.toUnsignedLong(T.ccbase));
      var(Integer.toUnsignedLong(T.nCcalls));
      var(T.errfunc);
      byte_kw(T.closestatus);
      byte_kw(T.closeself ? 1 : 0);
    }

    /*
    ** A thread's debug hook, as debug.sethook sets it: the debug library's C
    ** hook with the Lua hook function kept in the registry. Written with its
    ** mask, base count, current count and last traced pc, then the function.
    */
    private void hook(lua_State T)
    {
      if (T.hook == null)
      {
        byte_kw(0);
        return;
      }
      if (T.hook != luaL_debughook())
        fail(Lt.a_hook_not_set_by_the_debug_library_is_not_supported_yet);
      byte_kw(1);
      i64(T.hookmask & ~LUA_MASKBUDGET); /* (the budget is its group's) */
      i64(T.basehookcount);
      i64(T.hookcount);
      i64(T.oldpc);
      byte_kw(T.allowhook ? 1 : 0);
      TValue f = new TValue();
      setnilvalue(f);
      if (ttistable(L_.l_G.l_registry))
      {
        TValue hooks = string_field(hvalue(L_.l_G.l_registry), Lt._HOOKKEY);
        if (ttistable(hooks))
        {
          TValue th = new TValue();
          setthvalue(L_, th, T);
          TValue v = new TValue();
          byte tag = hvalue(hooks).luaH_get(th, v);
          if (!tagisempty(tag))
            copyobj(f, v);
        }
      }
      push_value(f, Step.Hook);
    }
  }

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

  private static final class Reader
  {
    lua_State L;
    ByteView data;
    int pos = 0;

    Reader(lua_State L, ByteView data)
    {
      this.L = L;
      this.data = data;
    }

    Reader(lua_State L, ByteView data, int pos)
    {
      this.L = L;
      this.data = data;
      this.pos = pos;
    }

    RuntimeException invalid()
    {
      lua_pushinteger(L, pos);
      luaL_errori(L, Lt.invalid_persisted_data_at_byte_d);
      throw new AssertionError();
    }

    void need(long n)
    {
      if (data.size() - pos < n)
        invalid();
    }

    byte get_byte()
    {
      need(1);
      return data.at(pos++);
    }

    long get_i64()
    {
      need(8);
      long v = data.toInt64(pos);
      pos += 8;
      return v;
    }

    double get_f64()
    {
      need(8);
      double n = data.toDouble(pos);
      pos += 8;
      return n;
    }

    int get_u32()
    {
      need(4);
      int v = data.toInt32(pos);
      pos += 4;
      return v;
    }

    /* a variable-length integer (see Walker::var) */
    long get_var()
    {
      long u = 0;
      for (int shift = 0;; shift += 7)
      {
        if (shift > 63)
          invalid();
        long c = get_byte() & 0xFF;
        u |= (c & 0x7F) << shift;
        if ((c & 0x80) == 0)
          break;
      }
      return (u >>> 1) ^ (0 - (u & 1));
    }

    ByteView get_bytes(long n)
    {
      need(n);
      ByteView v = data.substr(pos, (int) n);
      pos += (int) n;
      return v;
    }

    int get_int()
    {
      long n = get_var();
      if (n < Integer.MIN_VALUE || n > Integer.MAX_VALUE)
        invalid();
      return (int) n;
    }

    /* a count, at most the number of bytes left (each item takes 'item' bytes) */
    int get_count(long item)
    {
      long n = get_var();
      if (n < 0 || n > Integer.MAX_VALUE || n > (data.size() - pos) / item)
        invalid();
      return (int) n;
    }

    int get_count()
    {
      return get_count(1);
    }
  }

  /*
  ** A thread being restored is not running, so nothing may raise an error on
  ** it: its stack and frames are grown in the modes that report failure, work
  ** that can only raise runs protected on it (as `lua_resume` runs a
  ** coroutine), and a failure is raised on the calling thread, as
  ** `coroutine.resume` does when it grows a coroutine's stack.
  */
  private static RuntimeException memory_error(lua_State L)
  {
    lua_pushistringref(L, Lt.not_enough_memory);
    lua_error(L);
    throw new AssertionError();
  }

  /* finds or creates the open upvalue at 'level' of a thread (run protected) */
  private static final class FindUpval implements ProtectedCall
  {
    int level = 0;
    UpVal uv = null;

    @Override
    public void run(lua_State L1)
    {
      uv = UpVal.luaF_findupval(L1, level);
    }
  }

  /* registers a to-be-closed variable of a thread (run protected) */
  private static final class NewTbc implements ProtectedCall
  {
    int level = 0;

    @Override
    public void run(lua_State L1)
    {
      luaF_newtbcupval(L1, level);
    }
  }

  /*
  ** A thread is persisted with its status: not started or dead (LUA_OK, no
  ** frames), suspended (LUA_YIELD) or dead after an error (an error status,
  ** keeping its frames for tracebacks and its error value).
  */
  private static boolean valid_status(byte status)
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
  private static boolean valid_frame(lua_State L1, CallInfo ci)
  {
    TValue f = getslot(L1, ci.func);
    if ((ci.callstatus & CIST_C) != 0)
      return ttisfunction(f);
    if (!ttisLclosure(f))
      return false;
    Proto p = clLvalue(f).p;
    return ci.savedpc >= 0 && ci.savedpc <= p.code.length;
  }

  /*
  ** A closure is made as OP_CLOSURE makes one: a new closure on the shared
  ** prototype, anchored on the stack before its upvalues are created. This
  ** allocates, so it runs under the API lock, as an API function would.
  */
  private static void push_new_closure(lua_State L, Proto p)
  {
    lua_lock(L);
    LClosure cl = LClosure.luaF_newLclosure(L, p.upvalues.length);
    cl.p = p;
    setslotclL(L, L.top, cl);
    L.top++;
    luaC_objbarrier(L, cl, p);
    cl.luaF_initupvals(L);
    lua_unlock(L);
  }

  /* what is being built */
  private enum Build
  {
    Table,
    Perm,
    Closure,
    Proto,
    Thread,
    Group,
    Userobject,
    CClosure,
  }

  /* the part a frame waits for */
  private enum Part
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
  }

  private static final class Frame implements Struct<Frame>
  {
    Build kind = Build.Table;
    Part part = Part.Meta;
    byte type = 0;      /* Perm: the expected type; Thread: status; Table: layout */
    int i = 0;          /* the part being read */
    int n = 0;          /* number of parts */
    long id = 0; /* reference id */
    long a = 0;  /* Table: asize; Thread: top;  hook: mask */
    long b = 0;  /* Table: hsize; Thread: maxtop; hook: base count */
    long c = 0;  /* Table: length hint; hook: current count; Closure: the variable being made */
    long d = 0;  /* hook: last traced pc; Table: lastfree */
    long e = 0;  /* hook: allowhook */
    GCObject p = null; /* Table, LClosure (a Proto's holder, or the closure), lua_State, Udata, BudgetGroup */
    lua_CClosureKind ckind = null; /* CClosure: its kind */
    Slot key = new Slot();           /* Table: the key waiting for its value */

    @Override
    public Frame copy()
    {
      Frame f = new Frame();
      f.kind = kind;
      f.part = part;
      f.type = type;
      f.i = i;
      f.n = n;
      f.id = id;
      f.a = a;
      f.b = b;
      f.c = c;
      f.d = d;
      f.e = e;
      f.p = p;
      f.ckind = ckind;
      f.key = key.copy();
      return f;
    }
  }

  private static final class Restorer
  {
    /* stack: 1: permanents (key -> object); 3: refs (id -> object); 4: upvalues */
    Restorer(lua_State L, ByteView data, boolean host)
    {
      r_ = new Reader(L, data);
      L_ = L;
      host_ = host;
      frames_ = new WorkStack<>(L, true, WS_FRAME);
      marked_ = new WorkStack<>(L, true, WS_WORD);
      made_ = new WorkStack<>(L, true, WS_WORD);
      umarked_ = new WorkStack<>(L, true, WS_PAIR);
      owners_ = new WorkStack<>(L, true, WS_WORD);
      dues_ = new WorkStack<>(L, true, WS_WORD);
      fixes_ = new WorkStack<>(L, true, WS_TRIPLE);
      rebuilt_ = new WorkStack<>(L, true, WS_WORD);
      making_ = new WorkStack<>(L, true, WS_WORD);
      joins_ = new WorkStack<>(L, true, WS_TRIPLE);
    }

    void release()
    {
      joins_.release();
      making_.release();
      rebuilt_.release();
      fixes_.release();
      dues_.release();
      owners_.release();
      umarked_.release();
      made_.release();
      marked_.release();
      frames_.release();
    }

    /* a failed restore leaves no finalizer to run on what it built (ADR-28),
       and the main thread, if it went into it, idle and empty as it was
       (its owners call it as an error leaves the restore) */
    void abandon()
    {
      if (!done_ && mainrestored_)
      {
        lua_State M = mainthread(L_.l_G);
        luaF_closeupval(M, 0);
        M.ci = M.base_ci;
        M.top = 1;
        M.tbclist = 0;
        M.ntbc = 0;
        M.frozen = 0;
        M.frozenchild = null;
        M.frozenparent = null;
        M.closeself = false;
        M.hook = mainsaved_.hook;
        M.hookmask = mainsaved_.hookmask;
        M.basehookcount = mainsaved_.basehookcount;
        M.hookcount = mainsaved_.hookcount;
        M.oldpc = mainsaved_.oldpc;
        M.allowhook = mainsaved_.allowhook;
        M.errfunc = mainsaved_.errfunc;
        M.nCcalls = mainsaved_.nCcalls;
        M.ccbase = mainsaved_.ccbase;
        M.status = mainsaved_.status;
        M.bown = mainsaved_.bown;
        luaE_setbeff(M, mainsaved_.beff);
      }
      if (!done_)
        for (int i = 0; i < made_.size(); i++)
        {
          GCObject o = made_.get(i);
          if (o == null)
            continue;
          else if (o.tt == LUA_VTABLE)
            ((Table) o).metatable = null;
          else
            ((Udata) o).metatable = null;
        }
    }

    Reader r_;
    long seed_ = 0; /* the hash seed of the persisting state */

    /*
    ** The trailer (ADR-10), read first, from the end: the value ends where it
    ** starts. The tables to mark for finalization are made now, in the order
    ** their originals were marked, and marked as a script marks a table (by
    ** a metatable with '__gc'); the value's table records fill them (ADR-28).
    */
    void read_trailer()
    {
      ByteView d = r_.data;
      int end = d.size();
      long len = 0;
      for (int shift = 0;; shift += 7)
      {
        if (shift > 63 || end <= r_.pos)
          new Reader(L_, d, end).invalid();
        long c = d.at(--end) & 0xFF;
        len |= (c & 0x7F) << shift;
        if ((c & 0x80) == 0)
          break;
      }
      if (Long.compareUnsigned(len, end - r_.pos) > 0)
        new Reader(L_, d, end).invalid();
      Reader t = new Reader(L_, d.substr(0, end), end - (int) len);
      int n = t.get_count();
      if (n > 0)
      {
        luaL_checkstacki(L_, 4, null);
        lua_createtable(L_, 0, 1); /* a metatable that marks */
        TValue gc = new TValue();
        setsvalue(L_, gc, L_.l_G.tmname[TM_GC]);
        setslot(L_, L_.top, gc);
        L_.top++;
        lua_pushboolean(L_, true);
        lua_rawset(L_, -3);
        WorkStack<Long> kinds = new WorkStack<>(L_, true, WS_WORD);
        try
        {
          kinds.reserve(n);
          for (int i = 0; i < n; i++)
          {
            long kind = t.get_var();
            if (kind != 0 && kind != 1)
              t.invalid();
            kinds.push(kind);
          }
          for (int i = 0; i < n; i++)
          {
            long id = t.get_var();
            if (id < 1 || lua_rawgeti(L_, REFS, id) != LUA_TNIL)
              t.invalid();
            lua_pop(L_, 1);
            if (kinds.get(i) == 1)
            { /* a user object: marked when it is read (its kind makes it) */
              made_.push(null);
              umarked_.push(new UMarked(id, i));
              continue;
            }
            lua_createtable(L_, 0, 0);
            TValue h = getslot(L_, L_.top - 1);
            made_.push(gcvalue(h));
            marked_.push(id);
            lua_pushvalue(L_, -2);
            lua_setmetatable(L_, -2);
            lua_rawseti(L_, REFS, id);
          }
          lua_pop(L_, 1);
          marked_.sort((a, b) -> a < b);
          umarked_.sort((a, b) -> a.id < b.id);
          /* who they belong to, in the same order: the groups come with the value */
          for (int i = 0; i < n; i++)
          {
            long code = t.get_var();
            if (code < 0)
              t.invalid();
            owners_.push(code);
          }
          for (int i = 0; i < n; i++)
          {
            long due = t.get_var();
            if (due != 0 && due != 1)
              t.invalid();
            dues_.push(due);
          }
        }
        finally
        {
          kinds.release();
        }
      }
      int m = t.get_count(3);
      for (int i = 0; i < m; i++)
      {
        DeadFix f = new DeadFix(t.get_var(), t.get_var(), t.get_var());
        if (f.table < 1 || f.node < 0 || f.node > Integer.MAX_VALUE || f.key < 1)
          t.invalid();
        fixes_.push(f);
      }
      if (t.pos != t.data.size())
        t.invalid();
      r_.data = d.substr(0, end - (int) len);
    }

    /*
    ** The value is read: every table made for the trailer must have been met,
    ** and dead keys get their identities (ADR-29). The restore succeeded.
    */
    void finish()
    {
      if (next_marked_ != marked_.size())
        r_.invalid();
      for (int i = 0; i < made_.size(); i++)
        if (made_.get(i) == null)
          r_.invalid(); /* (a marked user object the value did not have) */
      /* in the order they were marked (user objects are marked as they are
         read): those due go to their queues from it */
      if (umarked_.size() > 0)
        luaC_ordermarked(L_, made_.data(), made_.size());
      /* the owners of the marked tables: their groups are made now */
      for (int i = 0; i < made_.size(); i++)
      {
        long code = owners_.get(i);
        int owner = 0;
        if (code == 1)
          owner = GROUP_GONE;
        else if (code >= 2)
        {
          lua_rawgeti(L_, REFS, code - 2);
          TValue v = getslot(L_, L_.top - 1);
          if (!iscollectable(v) || gcvalue(v).tt != LUA_VBUDGET)
            r_.invalid();
          owner = ((BudgetGroup) gcvalue(v)).id;
          lua_pop(L_, 1);
        }
        made_.get(i).owner = owner;
      }
      /* those whose '__gc' was due are due again, in the order they had in their queues */
      {
        WorkStack<GCObject> due = new WorkStack<>(L_, true, WS_WORD);
        try
        {
          for (int i = made_.size(); i > 0; i--)
            if (dues_.get(i - 1) != 0)
              due.push(made_.get(i - 1));
          if (!due.empty())
            luaC_makedue(L_, due.data(), due.size());
        }
        finally
        {
          due.release();
        }
      }
      Less<Long> by_id = (a, b) -> a < b;
      rebuilt_.sort(by_id);
      for (int i = 0; i < fixes_.size(); i++)
      {
        DeadFix f = fixes_.get(i);
        lua_rawgeti(L_, REFS, f.table);
        lua_rawgeti(L_, REFS, f.key);
        TValue t = getslot(L_, L_.top - 2);
        TValue k = getslot(L_, L_.top - 1);
        if (!ttistable(t) || !iscollectable(k))
          r_.invalid();
        Table h = hvalue(t);
        int r = rebuilt_.lower_bound(f.table, by_id);
        boolean was_rebuilt = r < rebuilt_.size() && rebuilt_.get(r) == f.table;
        /* a table rebuilt, here or for another seed, has no dead keys */
        if (same_seed() && !was_rebuilt)
        {
          if (isdummy(h) || f.node >= sizenode(h) || !keyisdead(gnode(h, (int) f.node)))
            r_.invalid();
          int j = (int) f.node;
          h.luaH_setnode(L_, j, k, true, null, gnode(h, j).next);
        }
        lua_pop(L_, 2);
      }
      done_ = true;
    }

    /* reads one value and pushes it */
    void value()
    {
      int base = frames_.size();
      Next next = Next.Read;
      for (;;)
      {
        switch (next)
        {
        case Read:
          next = start();
          break;
        case Advance:
          next = advance();
          break;
        case Ready:
          if (frames_.size() == base)
            return;
          next = deliver();
          break;
        }
      }
    }

    private enum Next
    {
      Read,    /* read a value */
      Advance, /* the frame on top reads its next part */
      Ready,   /* a value is on top of the stack, for the frame on top */
    }

    private static final int PERMS = 1;
    private static final int REFS = 3;
    private static final int UPS = 4;

    private final lua_State L_;
    private final boolean host_;
    private long refcount_ = 0;
    private final WorkStack<Frame> frames_;
    private final WorkStack<Long> marked_; /* ids of the tables made for the trailer, sorted */
    private final WorkStack<GCObject> made_;     /* those objects (user objects as they are read) */

    private static final class UMarked implements Struct<UMarked>
    {
      long id; /* a marked user object's reference id */
      int made;    /* its place in 'made_' */

      UMarked(long id, int made)
      {
        this.id = id;
        this.made = made;
      }

      @Override
      public UMarked copy()
      {
        return new UMarked(id, made);
      }
    }

    private final WorkStack<UMarked> umarked_;    /* the user objects of the trailer, sorted by id */
    private final WorkStack<Long> owners_; /* their owners (see Walker::owner_code), in the order they were made */
    private final WorkStack<Long> dues_;   /* whether their '__gc' was due (waiting in a queue) */
    private int next_marked_ = 0;        /* the next of them the value meets */

    private static final class DeadFix implements Struct<DeadFix>
    {
      long table;
      long node;
      long key;

      DeadFix(long table, long node, long key)
      {
        this.table = table;
        this.node = node;
        this.key = key;
      }

      @Override
      public DeadFix copy()
      {
        return new DeadFix(table, node, key);
      }
    }

    private final WorkStack<DeadFix> fixes_;
    private boolean done_ = false;
    private boolean mainrestored_ = false; /* a frozen main thread is being restored into this state's */

    private static final class MainSaved
    { /* what the main thread had (idle and empty) before the restore went into it */
      lua_Hook hook;
      int hookmask;
      int basehookcount, hookcount, oldpc;
      boolean allowhook;
      int errfunc;
      int nCcalls, ccbase;
      byte status;
      BudgetGroup bown;
      BudgetGroup beff;

      MainSaved()
      {
      }

      MainSaved(lua_Hook hook, int hookmask, int basehookcount, int hookcount, int oldpc, boolean allowhook,
                int errfunc, int nCcalls, int ccbase, byte status, BudgetGroup bown, BudgetGroup beff)
      {
        this.hook = hook;
        this.hookmask = hookmask;
        this.basehookcount = basehookcount;
        this.hookcount = hookcount;
        this.oldpc = oldpc;
        this.allowhook = allowhook;
        this.errfunc = errfunc;
        this.nCcalls = nCcalls;
        this.ccbase = ccbase;
        this.status = status;
        this.bown = bown;
        this.beff = beff;
      }
    }

    private MainSaved mainsaved_ = new MainSaved();

    private final WorkStack<Long> rebuilt_; /* ids of layout tables whose keys did not all fit */

    /*
    ** A variable that is open (its owner thread is read) or a permanent (its
    ** key is read) is made after its closure's other parts, so a closure met
    ** meanwhile (in that thread's stack) that shares it waits: it is joined
    ** to the variable once the variable is made.
    */
    private static final class Join implements Struct<Join>
    {
      long uid;     /* the variable */
      long closure; /* the reference id of the closure that shares it */
      int i;               /* and its upvalue */

      Join(long uid, long closure, int i)
      {
        this.uid = uid;
        this.closure = closure;
        this.i = i;
      }

      @Override
      public Join copy()
      {
        return new Join(uid, closure, i);
      }
    }

    private final WorkStack<Long> making_; /* the variables being made */
    private final WorkStack<Join> joins_;

    private int lasthash_ = 0; /* the place of the last hash read */

    /* an object's hash (Walker::hash), in the persisting state's sequence */
    private int get_hash()
    {
      long d = r_.get_var();
      if (d < Integer.MIN_VALUE || d > Integer.MAX_VALUE)
        r_.invalid();
      lasthash_ += (int) d;
      return (int) seed_ + lasthash_ * HASHSTEP;
    }

    /* the next reference id, of an object other than a table */
    private long new_id()
    {
      long id = ++refcount_;
      if (next_marked_ < marked_.size() && marked_.get(next_marked_) == id)
        r_.invalid(); /* marked for finalization, so it must be a table */
      return id;
    }

    private void register_top(long id)
    {
      lua_pushvalue(L_, -1);
      lua_rawseti(L_, REFS, id);
    }

    private void push_table(Table h)
    {
      TValue t = new TValue();
      sethvalue(L_, t, h);
      setslot(L_, L_.top, t);
      L_.top++;
    }

    private void push_closure(LClosure cl)
    {
      setslotclL(L_, L_.top, cl);
      L_.top++;
    }

    private void push_thread(lua_State T)
    {
      setslotth(L_, L_.top, T);
      L_.top++;
    }

    /* the frame on top is done and its value is on the stack */
    private Next done()
    {
      frames_.pop();
      return Next.Ready;
    }

    private Next start()
    {
      luaL_checkstacki(L_, 8, null);
      byte tag = r_.get_byte();
      switch (tag)
      {
      case TAG_NIL:
        lua_pushnil(L_);
        return Next.Ready;
      case TAG_FALSE:
        lua_pushboolean(L_, false);
        return Next.Ready;
      case TAG_TRUE:
        lua_pushboolean(L_, true);
        return Next.Ready;
      case TAG_INTEGER:
        lua_pushinteger(L_, r_.get_var());
        return Next.Ready;
      case TAG_FLOAT:
      {
        lua_pushnumber(L_, r_.get_f64());
        return Next.Ready;
      }
      case TAG_REF:
      {
        long id = r_.get_var();
        lua_rawgeti(L_, REFS, id);
        if (lua_isnil(L_, -1))
          r_.invalid();
        return Next.Ready;
      }
      case TAG_PERM:
      {
        Frame f = new Frame();
        f.kind = Build.Perm;
        f.part = Part.Object;
        f.type = r_.get_byte();
        f.id = new_id();
        if (f.type < 0 || f.type >= LUA_NUMTYPES)
          r_.invalid();
        frames_.push(f);
        return Next.Read;
      }
      case TAG_STRING:
      {
        long id = new_id();
        long n = r_.get_var();
        if (n < 0)
          r_.invalid();
        ByteView s = r_.get_bytes(n);
        lua_pushlstring(L_, s, 0, s.size());
        register_top(id);
        return Next.Ready;
      }
      case TAG_TABLE:
        start_table(++refcount_);
        return Next.Read;
      case TAG_LCLOSURE:
      {
        Frame f = new Frame();
        f.kind = Build.Closure;
        f.part = Part.Proto;
        f.id = new_id();
        f.a = Integer.toUnsignedLong(get_hash());
        frames_.push(f);
        return Next.Advance;
      }
      case TAG_THREAD:
        start_thread(new_id());
        return Next.Advance;
      case TAG_MAINTHREAD_FROZEN:
        if (!host_) /* (as TAG_MAINTHREAD) */
          r_.invalid();
        start_thread(new_id(), true);
        return Next.Advance;
      case TAG_GROUP:
        start_group(new_id());
        return Next.Read;
      case TAG_USEROBJECT:
        start_userobject(new_id());
        return Next.Read;
      case TAG_CCLOSURE:
        return start_cclosure(new_id());
      case TAG_CFI:
      {
        long id = new_id();
        CFuncInfo cfi = lua_cfibyname(read_name());
        if (cfi == null)
          r_.invalid();
        setslotf(L_, L_.top, cfi);
        L_.top++;
        register_top(id);
        return Next.Ready;
      }
      case TAG_MAINTHREAD:
      {
        if (!host_) /* a script never gets the main thread from a stream */
          r_.invalid();
        long id = new_id();
        push_thread(mainthread(L_.l_G));
        register_top(id);
        return Next.Ready;
      }
      default:
        throw r_.invalid();
      }
    }

    /* the value on top of the stack goes to the frame on top */
    private Next deliver()
    {
      Frame f = frames_.top();
      switch (f.kind)
      {
      case Table:
        return table_part(f);
      case Perm:
        return perm_part(f);
      case Closure:
        return closure_part(f);
      case Proto:
        return proto_part(f);
      case Group:
        return group_part(f);
      case Userobject:
        return userobject_part(f);
      case CClosure:
        return cclosure_part(f);
      default:
        return thread_part(f);
      }
    }

    /* the frame on top reads its next raw parts */
    private Next advance()
    {
      Frame f = frames_.top();
      switch (f.kind)
      {
      case Closure:
        return closure_advance(f);
      case Proto:
        return proto_advance(f);
      case Thread:
        return thread_advance(f);
      case Table:
        return table_advance(f);
      default:
        return Next.Read;
      }
    }

    /* ---- tables ---- */

    private void start_table(long id)
    {
      int hash = get_hash();
      long asize = r_.get_var();
      long hsize = r_.get_var();
      long lenhint = r_.get_var();
      byte layout = r_.get_byte();
      if (asize < 0 || asize > Integer.MAX_VALUE || hsize < 0 || hsize > Integer.MAX_VALUE ||
          (hsize & (hsize - 1)) != 0 || lenhint < 0 || lenhint > asize ||
          (layout != 0 && layout != 1) || (layout == 1 && hsize == 0))
        r_.invalid();
      /* same sizes as the original (the table limits are checked as by
         lua_createtable): entries go where they were, with no rehash */
      if (next_marked_ < marked_.size() && marked_.get(next_marked_) == id)
      { /* made for the trailer */
        next_marked_++;
        lua_rawgeti(L_, REFS, id);
        TValue m = getslot(L_, L_.top - 1);
        lua_lock(L_);
        hvalue(m).luaH_resize(L_, (int) asize, (int) hsize);
        lua_unlock(L_);
      }
      else
      {
        lua_createtable(L_, (int) asize, (int) hsize);
        register_top(id);
      }
      TValue t = getslot(L_, L_.top - 1);
      hvalue(t).hash = hash; /* before it can be a key anywhere */
      Frame f = new Frame();
      f.kind = Build.Table;
      f.part = Part.Meta;
      f.a = asize;
      f.b = hsize;
      f.c = lenhint;
      f.type = layout;
      f.id = id;
      f.p = hvalue(t);
      lua_pop(L_, 1);
      frames_.push(f);
    }

    /* with the persisting state's seed, keys hash as they did: nodes go in
       their slots; otherwise their entries are inserted */
    private boolean same_seed()
    {
      return seed_ == Integer.toUnsignedLong(L_.l_G.seed);
    }

    /*
    ** Every key that hashes by an object's hash sits where a lookup finds it:
    ** in the chain from its main position. A key the restoring side supplied
    ** (a permanent, a light userdata or a host's C function, which hash by
    ** address) can hash otherwise (ADR-30); an engine C function hashes by
    ** its name.
    */
    private boolean keys_in_place(Table h)
    {
      int size = sizenode(h);
      for (int i = 0; i < size; i++)
      {
        Node n = gnode(h, i);
        if (keyisnil(n) || keyisdead(n))
          continue;
        byte kt = novariant(n.key_tt);
        if (kt == LUA_TSTRING || kt == LUA_TNUMBER || kt == LUA_TBOOLEAN)
          continue;
        TValue k = new TValue();
        getnodekey(L_, k, n);
        int j = h.luaH_mainposition(k);
        for (int steps = 0; j != i; steps++)
        {
          int next = gnode(h, j).next;
          if (next == 0 || steps == size)
            return false;
          j += next;
        }
      }
      return true;
    }

    /* the table is complete */
    private Next end_table(Frame f)
    {
      Table h = (Table) f.p;
      if (h.asize() != f.a || allocsizenode(h) != f.b)
        r_.invalid();
      if (f.type == 1 && same_seed())
      {
        if (keys_in_place(h))
          h.lastfree = (int) f.d;
        else
        { /* rebuilt with the same sizes: its contents, in another order */
          rebuilt_.push(f.id);
          lua_lock(L_);
          h.luaH_resize(L_, (int) f.a, (int) f.b);
          lua_unlock(L_);
        }
      }
      h.lenhint = (int) f.c;
      push_table(h);
      return done();
    }

    private Next table_advance(Frame f)
    {
      Table h = (Table) f.p;
      while (f.i < f.b)
      {
        byte state = r_.get_byte();
        if (state == 2)
        {
          f.part = Part.NodeKey;
          frames_.set_top(f);
          return Next.Read;
        }
        if (state != 0 && state != 1)
          r_.invalid();
        int link = node_link(f);
        if (same_seed())
        {
          if (allocsizenode(h) != f.b) /* (a corrupted stream made the table something else) */
            r_.invalid();
          h.luaH_setnode(L_, f.i, null, state == 1, null, link);
        }
        f.i++;
      }
      return end_table(f);
    }

    private int node_link(Frame f)
    {
      long link = r_.get_var();
      if (f.i + link < 0 || f.i + link >= f.b)
        r_.invalid();
      return (int) link;
    }

    private Next table_part(Frame f)
    {
      Table h = (Table) f.p;
      switch (f.part)
      {
      case Meta:
      {
        /* set as lua_setmetatable sets it, but without marking the table for
           finalization: the trailer marks exactly the marked (ADR-28) */
        Table mt = null;
        if (!lua_isnil(L_, -1))
        {
          if (!lua_istable(L_, -1))
            r_.invalid();
          TValue m = getslot(L_, L_.top - 1);
          mt = hvalue(m);
        }
        h.metatable = mt;
        if (mt != null)
          luaC_objbarrier(L_, h, mt);
        lua_pop(L_, 1);
        f.part = Part.Key;
        frames_.set_top(f);
        return Next.Read;
      }
      case Key:
        if (lua_isnil(L_, -1))
        {
          lua_pop(L_, 1);
          if (f.type == 0)
            return end_table(f);
          f.d = r_.get_var(); /* lastfree */
          if (f.d < 0 || f.d > f.b)
            r_.invalid();
          f.i = 0;
          f.part = Part.Nodes;
          frames_.set_top(f);
          return Next.Advance;
        }
        {
          TValue k = getslot(L_, L_.top - 1);
          f.key = Slot.of(k);
        }
        lua_pop(L_, 1);
        f.part = Part.Val;
        frames_.set_top(f);
        return Next.Read;
      case NodeKey:
      {
        TValue k = getslot(L_, L_.top - 1);
        if (ttisnil(k) || (ttisfloat(k) && fltvalue(k) != fltvalue(k)))
          r_.invalid();
        f.key = Slot.of(k);
        lua_pop(L_, 1);
        f.part = Part.NodeVal;
        frames_.set_top(f);
        return Next.Read;
      }
      case NodeVal:
      {
        TValue k = f.key.get();
        int link = node_link(f);
        if (same_seed())
        {
          if (allocsizenode(h) != f.b)
            r_.invalid();
          TValue v = getslot(L_, L_.top - 1);
          h.luaH_setnode(L_, f.i, k, false, v, link);
          lua_pop(L_, 1);
        }
        else if (lua_isnil(L_, -1))
          lua_pop(L_, 1);
        else
        {
          push_table(h);
          setslot(L_, L_.top, k);
          L_.top++;
          lua_rotate(L_, -3, 2); /* table, key, value */
          lua_rawset(L_, -3);
          lua_pop(L_, 1);
        }
        f.i++;
        f.part = Part.Nodes;
        frames_.set_top(f);
        return Next.Advance;
      }
      default: /* Part::Val */
      {
        push_table(h);
        TValue k = f.key.get();
        setslot(L_, L_.top, k);
        L_.top++;
        lua_rotate(L_, -3, 2); /* table, key, value */
        lua_rawset(L_, -3);
        lua_pop(L_, 1);
        f.part = Part.Key;
        frames_.set_top(f);
        return Next.Read;
      }
      }
    }

    /* ---- permanents ---- */

    private Next perm_part(Frame f)
    {
      lua_pushvalue(L_, -1);
      lua_rawget(L_, PERMS);
      if (lua_isnil(L_, -1))
      {
        luaL_tolstring(L_, -2);
        luaL_errori(L_, Lt.bad_permanent_value_for_key_s);
      }
      if (lua_type(L_, -1) != f.type)
      {
        lua_pushstring(L_, lua_typename(L_, f.type));
        lua_pushstring(L_, luaL_typename(L_, -2));
        luaL_errori(L_, Lt.bad_permanent_value_s_expected_got_s);
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
    private Next proto_tag()
    {
      byte tag = r_.get_byte();
      if (tag == TAG_PROTO_REF)
      {
        lua_rawgeti(L_, REFS, r_.get_var());
        if (lua_type(L_, -1) != LUA_TFUNCTION || lua_iscfunction(L_, -1))
          r_.invalid();
        return Next.Ready;
      }
      if (tag != TAG_PROTO_NEW)
        r_.invalid();
      start_proto(new_id());
      return Next.Advance;
    }

    private void start_proto(long pid)
    {
      global_State g = L_.l_G;
      lua_lock(L_);
      /* (the holder first, anchored on the stack: making the prototype can
         run an emergency collection, which would take it) */
      LClosure holder = LClosure.luaF_newLclosure(L_, 0);
      setslotclL(L_, L_.top, holder);
      L_.top++;
      Proto p = Proto.luaF_newproto(L_);
      holder.p = p;
      luaC_objbarrier(L_, holder, p);
      lua_unlock(L_);
      register_top(pid);
      lua_pop(L_, 1);

      p.linedefined = r_.get_int();
      p.lastlinedefined = r_.get_int();
      p.numparams = r_.get_int();
      p.flag = (byte) (r_.get_byte() & ~PF_FIXED);
      p.maxstacksize = r_.get_int();
      if (p.numparams < 0 || p.maxstacksize < 0 || p.numparams > p.maxstacksize)
        r_.invalid();
      int n = r_.get_count(W_INSTRUCTION);
      ByteView code = r_.get_bytes((long) n * W_INSTRUCTION);
      lua_lock(L_);
      p.code = luaM_alloc_Instruction_span(g, L_, new int[0], n, true);
      lua_unlock(L_);
      for (int j = 0; j < n; j++)
        p.code[j] = code.toInt32(j * W_INSTRUCTION);
      n = r_.get_count();
      lua_lock(L_);
      p.k = luaM_alloc_TValue_span(g, L_, new TValue[0], n, true);
      for (TValue k : p.k)
        setnilvalue(k);
      lua_unlock(L_);
      Frame f = new Frame();
      f.kind = Build.Proto;
      f.part = Part.Consts;
      f.n = n;
      f.p = holder;
      frames_.push(f);
    }

    /* a string or nil for a name */
    private TString name_part(GCObject owner)
    {
      TString ts = null;
      if (lua_type(L_, -1) == LUA_TSTRING)
      {
        TValue v = getslot(L_, L_.top - 1);
        ts = tsvalue(v);
        luaC_objbarrier(L_, owner, ts);
      }
      else if (!lua_isnil(L_, -1))
        r_.invalid();
      lua_pop(L_, 1);
      return ts;
    }

    private Next proto_advance(Frame f)
    {
      global_State g = L_.l_G;
      Proto p = ((LClosure) f.p).p;
      for (;;)
      {
        switch (f.part)
        {
        case Consts:
          if (f.i < f.n)
            return Next.Read;
          f.n = r_.get_count();
          f.i = 0;
          lua_lock(L_);
          p.upvalues = luaM_alloc_Upvaldesc_span(g, L_, new Upvaldesc[0], f.n, true);
          for (Upvaldesc uv : p.upvalues)
          {
            uv.name = null;
            uv.instack = false;
            uv.idx = 0;
            uv.kind = 0;
          }
          lua_unlock(L_);
          f.part = Part.UpDesc;
          break;
        case UpDesc:
          if (f.i < f.n)
          {
            Upvaldesc uv = p.upvalues[f.i];
            uv.instack = r_.get_byte() != 0;
            uv.idx = r_.get_int();
            uv.kind = r_.get_byte();
            f.part = Part.UpName;
            frames_.set_top(f);
            return Next.Read;
          }
          f.n = r_.get_count();
          f.i = 0;
          lua_lock(L_);
          p.p = luaM_alloc_Proto_ptr_span(g, L_, new Proto[0], f.n, true);
          for (int j = 0; j < p.p.length; j++)
            p.p[j] = null;
          lua_unlock(L_);
          f.part = Part.Nested;
          break;
        case Nested:
          frames_.set_top(f);
          if (f.i < f.n)
            return proto_tag();
          f.part = Part.Source;
          frames_.set_top(f);
          return Next.Read;
        default:
          frames_.set_top(f);
          return Next.Read;
        }
      }
    }

    private Next proto_part(Frame f)
    {
      global_State g = L_.l_G;
      LClosure holder = (LClosure) f.p;
      Proto p = holder.p;
      switch (f.part)
      {
      case Consts:
      {
        int t = lua_type(L_, -1);
        if (t != LUA_TNIL && t != LUA_TBOOLEAN && t != LUA_TNUMBER && t != LUA_TSTRING)
          r_.invalid();
        TValue v = getslot(L_, L_.top - 1);
        TValue k = p.k[f.i];
        copyobj(k, v);
        luaC_barrier(L_, p, k);
        lua_pop(L_, 1);
        f.i++;
        frames_.set_top(f);
        return Next.Advance;
      }
      case UpName:
        p.upvalues[f.i].name = name_part(p);
        f.i++;
        f.part = Part.UpDesc;
        frames_.set_top(f);
        return Next.Advance;
      case Nested:
      {
        TValue h = getslot(L_, L_.top - 1);
        Proto nested = clLvalue(h).p;
        p.p[f.i] = nested;
        luaC_objbarrier(L_, p, nested);
        lua_pop(L_, 1);
        f.i++;
        frames_.set_top(f);
        return Next.Advance;
      }
      case Source:
      {
        p.source = name_part(p);
        int n = r_.get_count();
        ByteView lines = r_.get_bytes(n);
        lua_lock(L_);
        p.lineinfo = luaM_alloc_ls_byte_span(g, L_, new byte[0], n, true);
        lua_unlock(L_);
        copy_bytes(ByteOwner.adopt(p.lineinfo).span(), lines);
        n = r_.get_count(2);
        lua_lock(L_);
        p.abslineinfo = luaM_alloc_AbsLineInfo_span(g, L_, new AbsLineInfo[0], n, true);
        lua_unlock(L_);
        for (AbsLineInfo a : p.abslineinfo)
        {
          a.pc = r_.get_int();
          a.line = r_.get_int();
        }
        f.n = r_.get_count();
        f.i = 0;
        lua_lock(L_);
        p.locvars = luaM_alloc_LocVar_span(g, L_, new LocVar[0], f.n, true);
        for (LocVar lv : p.locvars)
        {
          lv.varname = null;
          lv.startpc = 0;
          lv.endpc = 0;
        }
        lua_unlock(L_);
        f.part = Part.LocVar;
        break;
      }
      default: /* Part::LocVar */
      {
        LocVar lv = p.locvars[f.i];
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
        return Next.Read;
      }
      push_closure(holder); /* the prototype is complete */
      return done();
    }

    /* ---- closures ---- */

    private boolean being_made(long uid)
    {
      for (int k = 0; k < making_.size(); k++)
        if (making_.get(k) == uid)
          return true;
      return false;
    }

    /* variable 'uid' is made, as upvalue 'i' of 'cl': the closures that wait join it */
    private void made_variable(LClosure cl, int i, long uid)
    {
      assert !making_.empty() && making_.top() == uid;
      making_.pop();
      int k = joins_.size();
      while (k-- > 0)
      {
        Join j = joins_.get(k);
        if (j.uid != uid)
          continue;
        joins_.set(k, joins_.top()); /* (the last takes its place) */
        joins_.pop();
        lua_rawgeti(L_, REFS, j.closure);
        push_closure(cl);
        lua_upvaluejoin(L_, -2, j.i, -1, i);
        lua_pop(L_, 2);
      }
    }

    private Next closure_advance(Frame f)
    {
      if (f.part == Part.Proto)
        return proto_tag();
      LClosure cl = (LClosure) f.p;
      while (f.i <= f.n)
      {
        byte tag = r_.get_byte();
        if (tag == TAG_UPVAL_NEW || tag == TAG_UPVAL_OPEN || tag == TAG_UPVAL_PERM)
        {
          long uid = new_id();
          push_closure(cl);
          lua_rawseti(L_, UPS, uid);
          lua_pushinteger(L_, f.i);
          lua_rawseti(L_, UPS, -uid);
          if (tag != TAG_UPVAL_NEW) /* (a new one is made in place: it is shared as it is) */
            making_.push(uid);
          f.part = (tag == TAG_UPVAL_NEW) ? Part.UpNew : (tag == TAG_UPVAL_OPEN) ? Part.UpOpen : Part.UpPerm;
          f.c = uid;
          frames_.set_top(f);
          return Next.Read;
        }
        if (tag != TAG_UPVAL_REF)
          r_.invalid();
        long uid = r_.get_var();
        if (being_made(uid))
        {
          joins_.push(new Join(uid, f.id, f.i));
          f.i++;
          continue;
        }
        lua_rawgeti(L_, UPS, uid);
        lua_rawgeti(L_, UPS, -uid);
        if (!lua_isfunction(L_, -2) || !lua_isinteger(L_, -1))
          r_.invalid();
        push_closure(cl);
        lua_upvaluejoin(L_, -1, f.i, -3, (int) lua_tointeger(L_, -2));
        lua_pop(L_, 3);
        f.i++;
      }
      push_closure(cl);
      return done();
    }

    private Next closure_part(Frame f)
    {
      switch (f.part)
      {
      case Proto:
      {
        TValue h = getslot(L_, L_.top - 1);
        push_new_closure(L_, clLvalue(h).p);
        lua_remove(L_, -2); /* the prototype's holder */
        register_top(f.id);
        TValue c = getslot(L_, L_.top - 1);
        LClosure cl = clLvalue(c);
        cl.hash = (int) f.a;
        lua_pop(L_, 1);
        long nup = r_.get_var();
        if (nup != cl.upvals.length)
          r_.invalid();
        f.p = cl;
        f.n = (int) nup;
        f.i = 1;
        f.part = Part.Upvals;
        break;
      }
      case UpNew:
      {
        push_closure((LClosure) f.p);
        lua_insert(L_, -2);
        lua_setupvalue(L_, -2, f.i);
        lua_pop(L_, 1);
        break;
      }
      case UpOpen:
      {
        lua_State L1 = lua_tothread(L_, -1);
        long level = r_.get_var();
        if (L1 == null || L1 == L_ || (L1.status == LUA_OK && L1.ci != L1.base_ci && L1.frozen == 0) ||
            level < 0 || level >= L1.top)
          r_.invalid();
        FindUpval fu = new FindUpval();
        fu.level = (int) level;
        if (luaD_rawrunprotected(L1, fu) != LUA_OK)
          memory_error(L_);
        LClosure cl = (LClosure) f.p;
        cl.upvals[f.i - 1] = fu.uv;
        luaC_objbarrier(L_, cl, fu.uv);
        lua_pop(L_, 1);
        made_variable(cl, f.i, f.c);
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
          luaL_errori(L_, Lt.bad_permanent_value_for_key_s);
        }
        if (lua_type(L_, -1) != LUA_TFUNCTION || lua_iscfunction(L_, -1) ||
            lua_getupvaluei(L_, -1, 1) == null)
        {
          lua_pushistringref(L_, Lt.a_function_capturing_the_variable);
          lua_pushstring(L_, luaL_typename(L_, -2));
          luaL_errori(L_, Lt.bad_permanent_value_s_expected_got_s);
        }
        lua_pop(L_, 1); /* the upvalue's value */
        push_closure((LClosure) f.p);
        lua_upvaluejoin(L_, -1, f.i, -2, 1);
        lua_pop(L_, 3);
        made_variable((LClosure) f.p, f.i, f.c);
        break;
      }
      }
      if (f.part != Part.Upvals)
        f.i++;
      f.part = Part.Upvals;
      frames_.set_top(f);
      return Next.Advance;
    }

    /* ---- threads ---- */

    /*
    ** The thread is valid at every point a collection can see it: its stack is
    ** filled while it has only its base frame, then the frames are read into
    ** spare CallInfos past 'ci' (only an emergency collection, which keeps
    ** them, can run meanwhile) and become part of the thread in one step.
    */
    private void start_thread(long id, boolean main)
    {
      lua_State L1;
      if (main)
      { /* a frozen main thread: into this state's, which must be idle and
           empty, and not the thread restoring (whose stack is in use) */
        L1 = mainthread(L_.l_G);
        if (L1 == L_ || L1.ci != L1.base_ci || L1.top != 1 || mainrestored_)
          luaL_errori(L_, Lt.a_frozen_main_thread_is_restored_into_an_idle_main_thread);
        push_thread(L1);
        register_top(id);
        lua_pop(L_, 1);
        mainrestored_ = true;
        mainsaved_ = new MainSaved(L1.hook, L1.hookmask, L1.basehookcount, L1.hookcount, L1.oldpc,
                                   L1.allowhook, L1.errfunc, L1.nCcalls, L1.ccbase, L1.status,
                                   L1.bown, L1.beff);
      }
      else
      {
        int hash = get_hash();
        L1 = lua_newthread(L_);
        L1.hash = hash;
        register_top(id);
        lua_pop(L_, 1);
      }
      byte status = r_.get_byte();
      long top = r_.get_var();
      long maxtop = r_.get_var();
      if (!valid_status(status) || top < 1 || maxtop < top || maxtop > ERRORSTACKSIZE)
        r_.invalid();
      thread_room(L1, maxtop);
      for (int i = L1.top; i < top; i++)
        setslotnil(L1, i); /* (what was above the main thread's top is stale) */
      L1.top = (int) top;
      Frame f = new Frame();
      f.kind = Build.Thread;
      f.part = Part.Slots;
      f.type = status;
      f.a = top;
      f.b = maxtop;
      f.p = L1;
      frames_.push(f);
    }

    private void start_thread(long id)
    {
      start_thread(id, false);
    }

    private Next thread_advance(Frame f)
    {
      lua_State L1 = (lua_State) f.p;
      if (f.i < f.a)
        return Next.Read;
      long nframes = r_.get_i64();
      if (nframes < 0)
        r_.invalid();
      if (nframes > 0)
        read_frames(L1, nframes, f.a, f.b);
      read_tbc(L1, nframes, f.a);
      read_thread_extras(L1, f.a);
      if (nframes > 0 && f.type == LUA_OK && L1.frozen == 0)
        r_.invalid(); /* (frames of a thread not started or dead: only a frozen one has them) */
      byte kind = r_.get_byte();
      if (kind == 0)
      {
        L1.status = f.type;
        return thread_values(f);
      }
      if (kind != 1)
        r_.invalid();
      f.a = r_.get_i64();
      f.b = r_.get_i64();
      f.c = r_.get_i64();
      f.d = r_.get_i64();
      f.e = r_.get_byte();
      if (f.b > Integer.MAX_VALUE || f.c > Integer.MAX_VALUE || f.d < Integer.MIN_VALUE || f.d > Integer.MAX_VALUE)
        r_.invalid();
      if ((f.a & ~(LUA_MASKCALL | LUA_MASKRET | LUA_MASKLINE | LUA_MASKCOUNT)) != 0 ||
          f.a == 0 || f.b < 0 || f.c < 0 || (f.e != 0 && f.e != 1))
        r_.invalid();
      f.part = Part.HookFn;
      frames_.set_top(f);
      return Next.Read;
    }

    private Next thread_part(Frame f)
    {
      lua_State L1 = (lua_State) f.p;
      if (f.part.ordinal() >= Part.Bown.ordinal())
        return thread_chain_part(f);
      if (f.part == Part.Slots)
      {
        copyslot(L1, f.i, L_, L_.top - 1);
        lua_pop(L_, 1);
        f.i++;
        frames_.set_top(f);
        return Next.Advance;
      }
      /*
      ** A debug hook: installed as debug.sethook installs it (the Lua function
      ** in the registry's hook table, the debug library's C hook on the
      ** thread), then the count and last traced pc are put back.
      */
      if (!lua_isfunction(L_, -1) && !lua_isnil(L_, -1))
        r_.invalid();
      if (!luaL_getsubtablei(L_, LUA_REGISTRYINDEX, Lt._HOOKKEY))
      { /* as debug.sethook creates it */
        lua_pushistringref(L_, Lt.k_char);
        lua_setfieldi(L_, -2, Lt.mm_mode);
        lua_pushvalue(L_, -1);
        lua_setmetatable(L_, -2);
      }
      push_thread(L1);
      lua_pushvalue(L_, -3);
      lua_rawset(L_, -3);
      lua_pop(L_, 2);
      lua_sethook(L1, luaL_debughook(), (int) f.a, (int) f.b);
      L1.hookcount = (int) f.c;
      L1.oldpc = (int) f.d;
      L1.allowhook = f.e != 0;
      L1.status = f.type;
      return thread_values(f);
    }

    /* what a frozen thread keeps (see Walker::thread_extras) */
    private void read_thread_extras(lua_State L1, long top)
    {
      byte ah = r_.get_byte();
      if (ah != 0 && ah != 1)
        r_.invalid();
      L1.allowhook = ah != 0;
      long ft = r_.get_var();
      long nt = r_.get_var();
      if (ft < 0 || ft > MAXSTACK || nt < 0 || nt > MAXSTACK)
        r_.invalid();
      L1.transferinfo.ftransfer = (int) ft;
      L1.transferinfo.ntransfer = (int) nt;
      byte fz = r_.get_byte();
      if (fz < 0 || fz > FZ_HOSTCLOSE)
        r_.invalid();
      if (fz == 0)
        return;
      luaE_setfrozen(L1, fz);
      long frozenfunc = r_.get_var();
      L1.frozenfunc = (int) frozenfunc;
      long frozenerrfunc = r_.get_var();
      L1.frozenerrfunc = (int) frozenerrfunc;
      L1.frozennres = r_.get_int();
      L1.frozencc = (int) r_.get_var();
      L1.ccbase = (int) r_.get_var();
      L1.nCcalls = (int) r_.get_var();
      long errfunc = r_.get_var();
      L1.errfunc = (int) errfunc;
      byte cs = r_.get_byte();
      byte self = r_.get_byte();
      if (frozenfunc < 0 || frozenfunc > top || frozenerrfunc < 0 || frozenerrfunc > top ||
          L1.frozennres < -1 || errfunc < 0 || errfunc > top || cs < 0 || (self != 0 && self != 1))
        r_.invalid();
      L1.closestatus = cs;
      L1.closeself = (self == 1);
    }

    /* the values after the raw parts of a thread: its groups and chain */
    private Next thread_values(Frame f)
    {
      f.part = Part.Bown;
      frames_.set_top(f);
      return Next.Read;
    }

    /* a budget group (or nil) on top of the stack, popped */
    private BudgetGroup pop_group()
    {
      TValue v = getslot(L_, L_.top - 1);
      BudgetGroup bg = null;
      if (!ttisnil(v))
      {
        if (!iscollectable(v) || gcvalue(v).tt != LUA_VBUDGET)
          r_.invalid();
        bg = (BudgetGroup) gcvalue(v);
      }
      lua_pop(L_, 1);
      return bg;
    }

    /* a thread (or nil) on top of the stack, popped */
    private lua_State pop_thread()
    {
      lua_State th = null;
      if (!lua_isnil(L_, -1))
      {
        th = lua_tothread(L_, -1);
        if (th == null)
          r_.invalid();
      }
      lua_pop(L_, 1);
      return th;
    }

    private Next thread_chain_part(Frame f)
    {
      lua_State L1 = (lua_State) f.p;
      switch (f.part)
      {
      case Bown:
        L1.bown = pop_group();
        f.part = Part.Beff;
        break;
      case Beff:
      {
        BudgetGroup bg = pop_group();
        if (L1.frozen != 0 && bg == null)
          r_.invalid(); /* (a thread frozen by a budget had one in effect) */
        luaE_setbeff(L1, bg);
        f.part = Part.Parent;
        break;
      }
      case Parent:
      {
        /* (the chain is linked from either end, whichever is read first) */
        lua_State p = pop_thread();
        L1.frozenparent = p;
        if (p != null)
        {
          if (p == L1 || (p.frozenchild != null && p.frozenchild != L1))
            r_.invalid();
          p.frozenchild = L1;
        }
        f.part = Part.Child;
        break;
      }
      default: /* Part::Child */
      {
        lua_State c = pop_thread();
        if (c != null)
        {
          if (c == L1 || (c.frozenparent != null && c.frozenparent != L1) ||
              (L1.frozenchild != null && L1.frozenchild != c))
            r_.invalid();
          c.frozenparent = L1;
          L1.frozenchild = c;
        }
        else if (L1.frozenchild != null)
          r_.invalid(); /* (a thread says it was resumed by this one, which says it was not) */
        if ((L1.frozenparent != null || L1.frozenchild != null) && L1.frozen == 0)
          r_.invalid();
        if (L1.frozen != 0 && L1.frozenparent == null && L1.bown == null)
          r_.invalid(); /* (the freeze target has its own group: a thaw puts it in effect;
                           the others get their resumer's, which may be all they have) */
        push_thread(L1);
        return done();
      }
      }
      frames_.set_top(f);
      return Next.Read;
    }

    /* ---- user objects ---- */

    private void start_userobject(long id)
    {
      int hash = get_hash();
      lua_ObjectFactory factory = lua_objectkind(read_name());
      long objsize = r_.get_var();
      long nuv = r_.get_var();
      int nints = r_.get_count(1);
      if (factory == null || objsize < 0 || nuv < 0 || nuv > 255 || nints > 8)
        r_.invalid();
      long[] ints = new long[8];
      for (int i = 0; i < nints; i++)
        ints[i] = r_.get_var();
      int nbytes = r_.get_count(1);
      ByteView bytes = r_.get_bytes(nbytes);
      lua_UserObject obj = factory.call(L_, ints, nints, bytes);
      if (obj == null)
        r_.invalid();
      lua_newuserobject(L_, obj, objsize, (int) nuv);
      TValue t = getslot(L_, L_.top - 1);
      uvalue(t).hash = hash;
      register_top(id);
      lua_pop(L_, 1);
      Frame f = new Frame();
      f.kind = Build.Userobject;
      f.part = Part.UMeta;
      f.id = id;
      f.n = (int) nuv;
      f.i = 1;
      f.p = uvalue(t);
      frames_.push(f);
    }

    private Next userobject_part(Frame f)
    {
      Udata u = (Udata) f.p;
      TValue v = getslot(L_, L_.top - 1);
      if (f.part == Part.UMeta)
      {
        if (ttisnil(v))
          u.metatable = null;
        else
        {
          if (!ttistable(v))
            r_.invalid();
          u.metatable = hvalue(v);
          luaC_objbarrier(L_, u, u.metatable);
        }
        lua_pop(L_, 1);
        int at = umarked_.lower_bound(new UMarked(f.id, 0), (a, b) -> a.id < b.id);
        if (at < umarked_.size() && umarked_.get(at).id == f.id)
        { /* marked for finalization in the persisting state (whatever its
             metatable says now): marked as a script marks it, by a metatable
             with '__gc'; its owner is set at the end ('finish') */
          Table mt = u.metatable;
          luaL_checkstacki(L_, 3, null);
          lua_createtable(L_, 0, 1);
          TValue gc = new TValue();
          setsvalue(L_, gc, L_.l_G.tmname[TM_GC]);
          setslot(L_, L_.top, gc);
          L_.top++;
          lua_pushboolean(L_, true);
          lua_rawset(L_, -3);
          TValue mv = getslot(L_, L_.top - 1);
          u.metatable = hvalue(mv);
          luaC_checkfinalizer(L_, u, u.metatable);
          u.metatable = mt;
          lua_pop(L_, 1);
          made_.set(umarked_.get(at).made, u);
        }
      }
      else
      {
        copyobj(u.uv[f.i - 1], v);
        luaC_barrier(L_, u, u.uv[f.i - 1]);
        lua_pop(L_, 1);
        f.i++;
      }
      if (f.part == Part.UMeta)
        f.part = Part.UValue;
      if (f.i <= f.n)
      {
        frames_.set_top(f);
        return Next.Read;
      }
      TValue d = new TValue();
      setuvalue(L_, d, u);
      setslot(L_, L_.top, d);
      L_.top++;
      return done();
    }

    /* ---- C closures of a registered kind ---- */

    private Next start_cclosure(long id)
    {
      int hash = get_hash();
      lua_CClosureKind kind = lua_cclosurekindbyname(read_name());
      int nints = r_.get_count(1);
      if (kind == null || nints > 5 || (nints > 0 && kind.save == null))
        r_.invalid();
      Frame f = new Frame();
      long[] ints = {0, 0, 0, 0, 0};
      for (int i = 0; i < nints; i++)
        ints[i] = r_.get_var();
      f.a = ints[0];
      f.b = ints[1];
      f.c = ints[2];
      f.d = ints[3];
      f.e = ints[4];
      lua_lock(L_);
      CClosure cl = CClosure.luaF_newCclosure(L_, kind.nup);
      for (TValue u : cl.upvalue)
        setnilvalue(u);
      cl.f = kind.f;
      cl.hash = hash;
      TValue cv = new TValue();
      setclCvalue(L_, cv, cl);
      setslot(L_, L_.top, cv);
      L_.top++;
      lua_unlock(L_);
      register_top(id);
      lua_pop(L_, 1);
      f.kind = Build.CClosure;
      f.part = Part.CUp;
      f.id = id;
      f.ckind = kind;
      f.i = 0;
      f.n = (kind.save != null) ? kind.nup - 1 : kind.nup;
      f.type = (byte) nints;
      frames_.push(f);
      return f.n > 0 ? Next.Read : finish_cclosure();
    }

    /* all its upvalues are in: the closure it has under the frame's id is the value */
    private Next finish_cclosure()
    {
      Frame f = frames_.top();
      lua_CClosureKind kind = f.ckind;
      lua_rawgeti(L_, REFS, f.id);
      TValue cv = getslot(L_, L_.top - 1);
      CClosure cl = clCvalue(cv);
      if (kind.make != null)
      {
        long[] ints = {f.a, f.b, f.c, f.d, f.e};
        if (!kind.make.call(L_, cl, ints, f.type))
          r_.invalid();
      }
      return done();
    }

    private Next cclosure_part(Frame f)
    {
      lua_rawgeti(L_, REFS, f.id);
      TValue cv = getslot(L_, L_.top - 1);
      CClosure cl = clCvalue(cv);
      lua_pop(L_, 1);
      TValue v = getslot(L_, L_.top - 1);
      copyobj(cl.upvalue[f.i], v);
      luaC_barrier(L_, cl, cl.upvalue[f.i]);
      lua_pop(L_, 1);
      f.i++;
      frames_.set_top(f);
      if (f.i < f.n)
        return Next.Read;
      return finish_cclosure();
    }

    /* ---- budget groups ---- */

    private ByteView read_name()
    {
      long n = r_.get_var();
      if (n < 0 || n > 256)
        r_.invalid();
      return r_.get_bytes(n);
    }

    private void start_group(long id)
    {
      long budget = r_.get_var();
      long left = r_.get_var();
      lua_Budget fn = lua_budgetbyname(read_name());
      byte nodrain = r_.get_byte();
      int nqueue = r_.get_count();
      if (budget < 1 || left > budget || fn == null || (nodrain != 0 && nodrain != 1))
        r_.invalid();
      lua_lock(L_);
      BudgetGroup bg = luaM_malloc_BudgetGroup(L_);
      luaC_link(L_, bg, LUA_VBUDGET);
      bg.budget = budget;
      bg.left = left;
      bg.f = fn;
      bg.nodrain = (nodrain == 1);
      TValue gv = new TValue();
      setgcovalue(L_, gv, bg);
      setslot(L_, L_.top, gv);
      L_.top++;
      boolean ok = luaC_newowner(L_, bg);
      lua_unlock(L_);
      if (!ok)
        luaL_errori(L_, Lt.too_many_budget_groups);
      register_top(id);
      lua_pop(L_, 1);
      luaL_checkstacki(L_, 3, null);
      lua_preparebudget(L_, fn); /* (what its callback needs of this state) */
      Frame f = new Frame();
      f.kind = Build.Group;
      f.part = Part.GroupCb;
      f.id = id;
      f.n = nqueue;
      f.p = bg;
      frames_.push(f);
    }

    private Next group_part(Frame f)
    {
      BudgetGroup bg = (BudgetGroup) f.p;
      if (f.part == Part.GroupCb)
      {
        TValue cbv = getslot(L_, L_.top - 1);
        copyobj(bg.cb, cbv);
        luaC_barrier(L_, bg, bg.cb);
        lua_pop(L_, 1);
        f.part = Part.GroupRoot;
        frames_.set_top(f);
        return Next.Read;
      }
      if (f.part == Part.GroupRoot)
      {
        bg.root = pop_thread();
        f.part = Part.GroupQueue;
        f.i = 0;
      }
      else
      { /* a dead object waiting for its '__gc' (the trailer makes it due) */
        TValue v = getslot(L_, L_.top - 1);
        if (!ttistable(v) && !ttisfulluserdata(v))
          r_.invalid();
        lua_pop(L_, 1);
        f.i++;
      }
      if (f.i < f.n)
      {
        frames_.set_top(f);
        return Next.Read;
      }
      TValue gv = new TValue();
      setgcovalue(L_, gv, bg);
      setslot(L_, L_.top, gv);
      L_.top++;
      return done();
    }

    /* frames are read into spare CallInfos and attached in one step */
    /*
    ** Room for a restored thread's stack up to 'maxtop', and LUA_MINSTACK
    ** more where the stack's limit allows: a thread can use its stack up to
    ** the limit (a C function that checked it for that many values), and a
    ** thread handling a stack overflow has the room for errors past it (a
    ** stack of ERRORSTACKSIZE, as the original's while it uses it).
    */
    private void thread_room(lua_State L1, long maxtop)
    {
      boolean ok;
      if (maxtop > MAXSTACK)
      {
        lua_lock(L1);
        ok = stacksize(L1) == ERRORSTACKSIZE || luaD_reallocstack(L1, ERRORSTACKSIZE, false);
        lua_unlock(L1);
      }
      else
      {
        long need = Math.min(maxtop + LUA_MINSTACK, MAXSTACK);
        ok = lua_checkstack(L1, (int) need - L1.top);
      }
      if (!ok)
        memory_error(L_);
    }

    private void read_frames(lua_State L1, long nframes, long top, long maxtop)
    {
      /* a collection may have shrunk the stack while the values were read */
      thread_room(L1, maxtop);
      CallInfo last = L1.ci;
      for (long k = 0; k < nframes; k++)
      {
        CallInfo ci = last.next;
        if (ci == null)
        {
          L1.ci = last;
          ci = luaE_extendCI(L1, 0);
          L1.ci = L1.base_ci;
          if (ci == null)
            memory_error(L_);
        }
        long func = r_.get_i64();
        ci.func = (int) func;
        long citop = r_.get_i64();
        ci.top = (int) citop;
        ci.savedpc = (int) r_.get_i64();
        ci.nextraargs = (int) r_.get_i64();
        ci.callstatus = (int) r_.get_i64();
        ci.old_errfunc = (int) r_.get_i64();
        ci.ctx = r_.get_i64();
        ci.funcidx = (int) r_.get_i64();
        ci.nyield = (int) r_.get_i64();
        ci.nres = (int) r_.get_i64();
        ci.k = null;
        ci.trap = 0;
        byte hask = r_.get_byte();
        if (hask != 0 && hask != 1)
          r_.invalid();
        if (hask == 1)
        {
          ci.k = lua_kbyname(read_name());
          if (ci.k == null)
            r_.invalid();
        }
        ci.nccalls = (int) r_.get_var();
        ci.apiop = r_.get_byte();
        long apifirst = r_.get_var();
        ci.apifirst = (int) apifirst;
        ci.hookev = r_.get_byte();
        ci.hookdone = r_.get_byte();
        long hooktop = r_.get_var();
        ci.hooktop = (int) hooktop;
        long hookcitop = r_.get_var();
        ci.hookcitop = (int) hookcitop;
        ci.hookdelta = r_.get_int();
        long hookres = r_.get_var();
        ci.hookres = (int) hookres;
        if (func < 0 || func >= top || citop < func || citop > maxtop ||
            !valid_frame(L1, ci) || (ci.apiop & 0xFF) > APIK_CLOSESLOT || (ci.hookev & 0xFF) > HE_RETTBC ||
            apifirst < 0 || apifirst > maxtop || hooktop < 0 || hooktop > maxtop ||
            hookcitop < 0 || hookcitop > maxtop || hookres < 0 || hookres > maxtop ||
            ((ci.callstatus & CIST_APIK) == 0 && (ci.apiop != 0 || apifirst != 0)) ||
            ((ci.callstatus & CIST_HOOKED) == 0 && (ci.hookev != 0 || ci.hookdone != 0 || hooktop != 0 ||
                                                     hookcitop != 0 || ci.hookdelta != 0 || hookres != 0)))
          r_.invalid();
        last = ci;
      }
      L1.ci = last;
    }

    /*
    ** To-be-closed variables are registered once the thread's stack and frames
    ** are complete, with the engine's own luaF_newtbcupval, run protected on
    ** the thread; an error there is raised on the caller.
    */
    private void read_tbc(lua_State L1, long nframes, long top)
    {
      long ntbc = r_.get_i64();
      if (ntbc < 0 || ntbc > top || (ntbc > 0 && nframes == 0))
        r_.invalid();
      int prev = 0;
      for (long k = 0; k < ntbc; k++)
      {
        long level = r_.get_i64();
        if (level <= prev || level >= top)
          r_.invalid();
        prev = (int) level;
        NewTbc nt = new NewTbc();
        nt.level = (int) level;
        byte st = luaD_rawrunprotected(L1, nt);
        if (st == LUA_ERRMEM)
          memory_error(L_);
        if (st != LUA_OK)
        {
          lua_xmove(L1, L_, 1);
          lua_error(L_);
        }
      }
    }
  }

  /* }====================================================== */

  /*
  ** Checks the header, reads the persisting state's hash seed and sets up the
  ** stack for a Restorer (3: refs; 4: upvalues)
  */
  private static void begin_restore(lua_State L, Restorer R)
  {
    final ByteView header = Lt.styx_header;
    if (R.r_.data.size() < header.size() || !R.r_.data.substr(0, header.size()).equals(header))
      R.r_.invalid();
    R.r_.pos = header.size();
    R.seed_ = R.r_.get_i64();
    lua_createtable(L, 0, 0);
    lua_createtable(L, 0, 0);
    R.read_trailer();
  }

  /* persist([perms,] value) */
  private static int styx_persist(lua_State L)
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
    TValue perms = getslot(L, L.ci.func + 1);
    luaL_Buffer B = new luaL_Buffer();
    try
    {
      luaL_buffinit(L, B);
      luaL_addlstring(B, Lt.styx_header, 0, Lt.styx_header.size());
      {
        Walker w = new Walker(L, hvalue(perms), B, 3);
        try
        {
          w.i64(Integer.toUnsignedLong(L.l_G.seed));
          TValue v = getslot(L, L.ci.func + 2);
          w.root(v, Lt.path_root);
          w.trailer();
        }
        finally
        {
          w.release();
        }
      }
      luaL_pushresult(B);
    }
    finally
    {
      B.release();
    }
    return 1;
  }

  private static final lua_CFunction styx_persist = Lstyx::styx_persist;

  /* unpersist([perms,] string) */
  private static int styx_unpersist(lua_State L)
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
    Restorer R = new Restorer(L, luaL_checklstring(L, 2), false);
    try
    {
      try
      {
        begin_restore(L, R);
        R.value();
        if (R.r_.pos != R.r_.data.size())
          R.r_.invalid();
        R.finish();
      }
      catch (Throwable e)
      {
        R.abandon();
        throw e;
      }
    }
    finally
    {
      R.release();
    }
    return 1;
  }

  private static final lua_CFunction styx_unpersist = Lstyx::styx_unpersist;

  private static final CFuncInfo cfi_persist = new CFuncInfo(styx_persist, "styx.persist");
  private static final CFuncInfo cfi_unpersist = new CFuncInfo(styx_unpersist, "styx.unpersist");

  private static final luaL_RegCFI[] funcs = {
    new luaL_RegCFI(Lt.persist, cfi_persist),
    new luaL_RegCFI(Lt.unpersist, cfi_unpersist),
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

  /* the string hash seed of the state that wrote a stream (none: not one) */
  private static OptionalInt stream_seed(ByteView data)
  {
    final ByteView header = Lt.styx_header;
    if (data.size() < header.size() + 8 || !data.substr(0, header.size()).equals(header))
      return OptionalInt.empty();
    return OptionalInt.of((int) data.toInt64(header.size()));
  }

  /* the value at a stack index of a state that is not running */
  private static boolean value_at(lua_State L, int idx, TValue v)
  {
    int slot = (idx > 0) ? L.ci.func + idx : L.top + idx;
    if (idx == 0 || slot <= L.ci.func || slot >= L.top)
      return false;
    TValue s = getslot(L, slot);
    copyobj(v, s);
    return true;
  }

  private static luaL_StyxResult host_persist(lua_State L, int perms, lua_Writer w,
                                              Consumer<Walker> body)
  {
    try
    {
      lua_State main = mainthread(L.l_G);
      if (L == main && main.frozen != 0)
        throw new HostError(text(Lt.the_main_thread_is_frozen_use_another_thread));
      if ((main.ci != main.base_ci && main.frozen == 0) || L.ci != L.base_ci)
        throw new HostError(text(Lt.the_state_is_running));
      Table pt = null;
      if (perms != 0)
      {
        TValue pv = new TValue();
        if (!value_at(L, perms, pv) || !ttistable(pv))
          throw new HostError(text(Lt.the_permanents_must_be_a_table));
        pt = hvalue(pv);
      }
      Walker hp = new Walker(L, pt, w);
      try
      {
        hp.raw(Lt.styx_header);
        hp.i64(Integer.toUnsignedLong(L.l_G.seed));
        body.accept(hp);
        hp.trailer();
        hp.finish();
      }
      finally
      {
        hp.release();
      }
      return new luaL_StyxResult(LUA_OK, new HostString());
    }
    catch (HostError e)
    {
      return new luaL_StyxResult(LUA_ERRRUN, HostString.from_bytes(new ByteView(e.message.getBytes(StandardCharsets.ISO_8859_1))));
    }
  }

  private static final class HostRestore
  {
    ByteView data;
    boolean state;

    HostRestore(ByteView data, boolean state)
    {
      this.data = data;
      this.state = state;
    }
  }

  /* 1: permanents (key -> object) or nil; 2: the HostRestore */
  private static int host_restore_body(lua_State L)
  {
    HostRestore hr = (HostRestore) lua_touserdata(L, 2);
    if (lua_isnil(L, 1))
    {
      lua_createtable(L, 0, 0);
      lua_replace(L, 1);
    }
    Restorer R = new Restorer(L, hr.data, true);
    try
    {
      try
      {
        return host_restore_run(L, R, hr);
      }
      catch (Throwable e)
      {
        R.abandon();
        throw e;
      }
    }
    finally
    {
      R.release();
    }
  }

  private static final lua_CFunction host_restore_body = Lstyx::host_restore_body;

  private static int host_restore_run(lua_State L, Restorer R, HostRestore hr)
  {
    begin_restore(L, R);
    if (!hr.state)
    {
      if (R.r_.pos < R.r_.data.size() && R.r_.data.at(R.r_.pos) == TAG_STATE)
        R.r_.invalid();
      R.value();
      if (R.r_.pos != R.r_.data.size())
        R.r_.invalid();
      R.finish();
      return 1;
    }
    if (R.r_.get_byte() != TAG_STATE)
      R.r_.invalid();
    if ((int) R.seed_ != L.l_G.seed) /* (luaL_restorestate makes it with the seed) */
      luaL_errori(L, Lt.the_state_does_not_hash_as_the_persisted_one);
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
    byte kind = R.r_.get_byte(); /* the collector's settings */
    byte stopped = R.r_.get_byte();
    if ((kind != 0 && kind != 1) || (stopped != 0 && stopped != 1))
      R.r_.invalid();
    int[] params = new int[LUA_GCPN];
    for (int c = 0; c < params.length; c++)
      params[c] = R.r_.get_byte() & 0xFF;
    int nexthash = R.r_.get_u32(); /* the hash of the next object (ADR-30) */
    if (R.r_.pos != R.r_.data.size())
      R.r_.invalid();
    R.finish();
    /* the whole stream is read: the state takes the restored roots, then
       the collector's settings */
    global_State g = L.l_G;
    TValue reg = getslot(L, L.ci.func + 5);
    copyobj(g.l_registry, reg);
    for (int t = 0; t < LUA_NUMTYPES; t++)
    {
      TValue mt = getslot(L, L.ci.func + 6 + t);
      g.mt[t] = ttistable(mt) ? hvalue(mt) : null;
    }
    for (int p = 0; p < LUA_GCPN; p++)
      g.gcparams[p] = params[p];
    g.nexthash = nexthash;
    lua_gc(L, kind != 0 ? LUA_GCGEN : LUA_GCINC, 0, 0, 0);
    lua_gc(L, stopped != 0 ? LUA_GCSTOP : LUA_GCRESTART, 0, 0, 0);
    lua_pushvalue(L, 6 + LUA_NUMTYPES);
    return 1;
  }

  private static final CFuncInfo cfi_host_restore = new CFuncInfo(host_restore_body, "styx.host_restore");

  private static luaL_StyxResult host_restore(lua_State L, int perms, ByteView data, boolean state)
  {
    lua_State main = mainthread(L.l_G);
    if (L == main && main.frozen != 0)
      return new luaL_StyxResult(LUA_ERRRUN, HostString.from_bytes(Lt.the_main_thread_is_frozen_use_another_thread));
    if ((main.ci != main.base_ci && main.frozen == 0) || L.ci != L.base_ci)
      return new luaL_StyxResult(LUA_ERRRUN, HostString.from_bytes(Lt.the_state_is_running));
    int top = lua_gettop(L);
    if (perms < 0)
      perms = top + perms + 1;
    if (perms != 0 && (perms > top || !lua_istable(L, perms)))
      return new luaL_StyxResult(LUA_ERRRUN, HostString.from_bytes(Lt.the_permanents_must_be_a_table));
    if (!lua_checkstack(L, 3))
      return new luaL_StyxResult(LUA_ERRMEM, HostString.from_bytes(Lt.not_enough_memory));
    HostRestore hr = new HostRestore(data, state);
    lua_pushcfuncinfo(L, cfi_host_restore);
    if (perms != 0)
      lua_pushvalue(L, perms);
    else
      lua_pushnil(L);
    lua_pushlightuserdata(L, hr);
    int st = lua_pcall(L, 2, 1, 0); /* (the value, or the thread a state was persisted from) */
    if (st == LUA_OK)
      return new luaL_StyxResult(LUA_OK, new HostString());
    ByteView m = lua_tolstring(L, -1);
    HostString msg = m != null ? HostString.from_bytes(m) : HostString.from_bytes(Lt.error_object_is_not_a_string);
    lua_settop(L, top);
    return new luaL_StyxResult(st, msg);
  }

  /* }====================================================== */

  /* a writer that keeps nothing: a walk that only checks */
  private static final class NullWriter implements lua_Writer
  {
    @Override
    public int write(lua_State L, ByteView b)
    {
      return 0;
    }
  }

  /*
  ** Can a persist of the state write all that the value at 'idx' holds
  ** ('perms': the stack index of the permanents, or 0)? The walk of a host
  ** persist, writing nothing: no Lua code runs and nothing is allocated in
  ** the state, so a running kernel can ask it of a user program. If not,
  ** the message says why, with the path of what cannot be written.
  */
  public static luaL_StyxResult lua_styxcheck(lua_State L, int idx, int perms)
  {
    try
    {
      TValue v = new TValue();
      if (!value_at(L, idx, v))
        throw new HostError(text(Lt.invalid_value_index));
      Table pt = null;
      if (perms != 0)
      {
        TValue pv = new TValue();
        if (!value_at(L, perms, pv) || !ttistable(pv))
          throw new HostError(text(Lt.the_permanents_must_be_a_table));
        pt = hvalue(pv);
      }
      NullWriter w = new NullWriter();
      Walker hp = new Walker(L, pt, w);
      try
      {
        hp.check_ = true;
        hp.root(v, Lt.path_root);
        hp.finish();
      }
      finally
      {
        hp.release();
      }
      return new luaL_StyxResult(LUA_OK, new HostString());
    }
    catch (HostError e)
    {
      return new luaL_StyxResult(LUA_ERRRUN, HostString.from_bytes(new ByteView(e.message.getBytes(StandardCharsets.ISO_8859_1))));
    }
  }

  public static luaL_StyxResult lua_styxunpersistvalue(lua_State L, int perms, ByteView data)
  {
    return host_restore(L, perms, data, false);
  }

  public static luaL_StyxResult lua_styxpersistvalue(lua_State L, int perms, int idx, lua_Writer w)
  {
    TValue v = new TValue();
    if (!value_at(L, idx, v))
      return new luaL_StyxResult(LUA_ERRRUN, HostString.from_bytes(Lt.invalid_value_index));
    return host_persist(L, perms, w, hp -> hp.root(v, Lt.path_root));
  }

  public static luaL_StyxResult luaL_persiststate(lua_State L, int perms, lua_Writer w)
  {
    return host_persist(L, perms, w, hp ->
    {
      global_State g = L.l_G;
      hp.byte_kw(TAG_STATE);
      hp.whole();
      hp.root(g.l_registry, Lt.path_registry);
      for (int t = 0; t < LUA_NUMTYPES; t++)
      {
        TValue mt = new TValue();
        if (g.mt[t] != null)
          sethvalue(L, mt, g.mt[t]);
        else
          setnilvalue(mt);
        hp.root(mt, Lt.path_type_metatable);
      }
      hp.marked_objects();
      hp.persisting_thread();
      hp.collector();
      hp.next_hash();
    });
  }

  private static final class RestoreSetup
  {
    luaL_RestoreSetup setup;

    RestoreSetup(luaL_RestoreSetup setup)
    {
      this.setup = setup;
    }
  }

  /*
  ** 1: the RestoreSetup -> the inverse permanents (or nil), and the thread
  ** the restore runs on (anchored in the registry the restore replaces)
  */
  private static int restore_setup(lua_State L)
  {
    RestoreSetup rs = (RestoreSetup) lua_touserdata(L, 1);
    lua_pop(L, 1);
    if (rs.setup != null)
      rs.setup.setup(L);
    else
      lua_pushnil(L);
    if (lua_gettop(L) != 1 || (!lua_isnil(L, 1) && !lua_istable(L, 1)))
      luaL_errori(L, Lt.the_setup_must_leave_the_permanents_table_or_nil);
    lua_newthread(L);
    lua_pushvalue(L, -1);
    luaL_ref(L, LUA_REGISTRYINDEX);
    return 2;
  }

  private static final lua_CFunction restore_setup = Lstyx::restore_setup;

  private static final CFuncInfo cfi_restore_setup = new CFuncInfo(restore_setup, "styx.restore_setup");

  public static luaL_Restored luaL_restorestate(lua_Alloc f, ByteView data, luaL_RestoreSetup setup)
  {
    OptionalInt seed = stream_seed(data);
    if (!seed.isPresent())
      return new luaL_Restored(null, HostString.from_bytes(Lt.not_a_persisted_state));
    lua_State L = lua_newstate(f, seed.getAsInt()); /* (hashes as the persisting state) */
    if (L == null)
      return new luaL_Restored(null, HostString.from_bytes(Lt.not_enough_memory));
    RestoreSetup rs = new RestoreSetup(setup);
    HostString msg = new HostString();
    lua_pushcfuncinfo(L, cfi_restore_setup);
    lua_pushlightuserdata(L, rs);
    int st = lua_pcall(L, 1, 2, 0);
    if (st != LUA_OK)
    {
      ByteView m = lua_tolstring(L, -1);
      msg = m != null ? HostString.from_bytes(m) : HostString.from_bytes(Lt.error_object_is_not_a_string);
    }
    else
    { /* (restored from another thread, so that a frozen main thread is
         restored into the main thread, idle and with an empty stack) */
      lua_State T = lua_tothread(L, 2);
      lua_pop(L, 1);
      boolean perms = lua_istable(L, 1);
      lua_xmove(L, T, 1);
      luaL_StyxResult res = host_restore(T, perms ? 1 : 0, data, true);
      msg = res.msg;
      if (res.status == LUA_OK)
      { /* (the main thread's stack is empty, or a frozen main thread's, which only a thaw changes) */
        lua_State from = lua_tothread(T, -1);
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
           holds nothing; and on the main thread: a cycle keeps the thread
           it runs on, and the thread the restore ran on, with the
           permanents it held, goes with it.) */
        if (lua_gc(L, LUA_GCISRUNNING, 0, 0, 0) != 0)
        {
          lua_lock(L);
          luaC_fullgcdue(L);
          lua_unlock(L);
        }
        return new luaL_Restored(from, new HostString());
      }
    }
    lua_close(L);
    return new luaL_Restored(null, msg);
  }

  public static int lua_styxtestmodule(lua_State L)
  {
    luaL_newlibcfi(L, funcs);
    return 1;
  }

  public static final lua_CFunction lua_styxtestmodule = Lstyx::lua_styxtestmodule;
}
