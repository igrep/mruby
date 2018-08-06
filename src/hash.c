/*
** hash.c - Hash class
**
** See Copyright Notice in mruby.h
*/

#include <mruby.h>
#include <mruby/array.h>
#include <mruby/class.h>
#include <mruby/hash.h>
#include <mruby/string.h>
#include <mruby/variable.h>

#ifndef MRB_WITHOUT_FLOAT
/* a function to get hash value of a float number */
mrb_int mrb_float_id(mrb_float f);
#endif

static inline khint_t
mrb_hash_ht_hash_func(mrb_state *mrb, mrb_value key)
{
  enum mrb_vtype t = mrb_type(key);
  mrb_value hv;
  khint_t h;

  switch (t) {
  case MRB_TT_STRING:
    h = mrb_str_hash(mrb, key);
    break;

  case MRB_TT_TRUE:
  case MRB_TT_FALSE:
  case MRB_TT_SYMBOL:
  case MRB_TT_FIXNUM:
#ifndef MRB_WITHOUT_FLOAT
  case MRB_TT_FLOAT:
#endif
    h = (khint_t)mrb_obj_id(key);
    break;

  default:
    hv = mrb_funcall(mrb, key, "hash", 0);
    h = (khint_t)t ^ (khint_t)mrb_fixnum(hv);
    break;
  }
  return kh_int_hash_func(mrb, h);
}

static inline mrb_bool
mrb_hash_ht_hash_equal(mrb_state *mrb, mrb_value a, mrb_value b)
{
  enum mrb_vtype t = mrb_type(a);

  switch (t) {
  case MRB_TT_STRING:
    return mrb_str_equal(mrb, a, b);

  case MRB_TT_SYMBOL:
    if (mrb_type(b) != MRB_TT_SYMBOL) return FALSE;
    return mrb_symbol(a) == mrb_symbol(b);

  case MRB_TT_FIXNUM:
    switch (mrb_type(b)) {
    case MRB_TT_FIXNUM:
      return mrb_fixnum(a) == mrb_fixnum(b);
#ifndef MRB_WITHOUT_FLOAT
    case MRB_TT_FLOAT:
      return (mrb_float)mrb_fixnum(a) == mrb_float(b);
#endif
    default:
      return FALSE;
    }

#ifndef MRB_WITHOUT_FLOAT
  case MRB_TT_FLOAT:
    switch (mrb_type(b)) {
    case MRB_TT_FIXNUM:
      return mrb_float(a) == (mrb_float)mrb_fixnum(b);
    case MRB_TT_FLOAT:
      return mrb_float(a) == mrb_float(b);
    default:
      return FALSE;
    }
#endif

  default:
    return mrb_eql(mrb, a, b);
  }
}

/* return non zero to break the loop */
typedef int (sg_foreach_func)(mrb_state *mrb,mrb_value key,mrb_value val, void *data);

#ifndef MRB_SG_SEGMENT_SIZE
#define MRB_SG_SEGMENT_SIZE 5
#endif

typedef struct segment {
  mrb_value key[MRB_SG_SEGMENT_SIZE];
  mrb_value val[MRB_SG_SEGMENT_SIZE];
  struct segment *next;
} segment;

/* Instance variable table structure */
typedef struct seglist {
  segment *rootseg;
  size_t size;
  size_t last_len;
} seglist;

/* Creates the instance variable table. */
static seglist*
sg_new(mrb_state *mrb)
{
  seglist *t;

  t = (seglist*)mrb_malloc(mrb, sizeof(seglist));
  t->size = 0;
  t->rootseg =  NULL;
  t->last_len = 0;

  return t;
}

/* Set the value for the symbol in the instance variable table. */
static void
sg_put(mrb_state *mrb, seglist *t, mrb_value key, mrb_value val)
{
  segment *seg;
  segment *prev = NULL;
  size_t i;

  if (t == NULL) return;
  seg = t->rootseg;
  while (seg) {
    for (i=0; i<MRB_SG_SEGMENT_SIZE; i++) {
      mrb_value k = seg->key[i];
      /* Found room in last segment after last_len */
      if (!seg->next && i >= t->last_len) {
        seg->key[i] = key;
        seg->val[i] = val;
        t->last_len = i+1;
        t->size++;
        return;
      }
      if (mrb_hash_ht_hash_equal(mrb, k, key)) {
        seg->val[i] = val;
        return;
      }
    }
    prev = seg;
    seg = seg->next;
  }

  /* Not found */
  t->size++;

  seg = (segment*)mrb_malloc(mrb, sizeof(segment));
  if (!seg) return;
  seg->next = NULL;
  seg->key[0] = key;
  seg->val[0] = val;
  t->last_len = 1;
  if (prev) {
    prev->next = seg;
  }
  else {
    t->rootseg = seg;
  }
}

/* Get a value for a symbol from the instance variable table. */
static mrb_bool
sg_get(mrb_state *mrb, seglist *t, mrb_value key, mrb_value *vp)
{
  segment *seg;
  size_t i;

  if (t == NULL) return FALSE;
  seg = t->rootseg;
  while (seg) {
    for (i=0; i<MRB_SG_SEGMENT_SIZE; i++) {
      mrb_value k = seg->key[i];

      if (!seg->next && i >= t->last_len) {
        return FALSE;
      }
      if (mrb_hash_ht_hash_equal(mrb, k, key)) {
        if (vp) *vp = seg->val[i];
        return TRUE;
      }
    }
    seg = seg->next;
  }
  return FALSE;
}

/* Deletes the value for the symbol from the instance variable table. */
static mrb_bool
sg_del(mrb_state *mrb, seglist *t, mrb_value key, mrb_value *vp)
{
  segment *seg;
  size_t i;

  if (t == NULL) return FALSE;
  seg = t->rootseg;
  while (seg) {
    for (i=0; i<MRB_SG_SEGMENT_SIZE; i++) {
      mrb_value k = seg->key[i];

      if (!seg->next && i >= t->last_len) {
        /* not found */
        return FALSE;
      }
      if (mrb_hash_ht_hash_equal(mrb, k, key)) {
        segment *sg0;
        size_t i0;

        t->size--;
        if (vp) *vp = seg->val[i];
        sg0 = seg;
        i0 = i;
        while (seg) {
          for (i++; i<MRB_SG_SEGMENT_SIZE; i++) {
            if (!seg->next && i >= t->last_len) {
              t->last_len--;
              if (t->last_len == 0 && sg0 != seg) {
                sg0->next = NULL;
                t->last_len = MRB_SG_SEGMENT_SIZE;
                mrb_free(mrb, seg);
              }
              return TRUE;
            }
            sg0->key[i0] = seg->key[i];
            sg0->val[i0] = seg->val[i];
            i0 = i;
          }
          sg0 = seg;
          seg = seg->next;
        }
        t->last_len--;
        return TRUE;
      }
    }
    seg = seg->next;
  }
  return FALSE;
}

/* Iterates over the instance variable table. */
static void
sg_foreach(mrb_state *mrb, seglist *t, sg_foreach_func *func, void *p)
{
  segment *seg;
  size_t i;

  if (t == NULL) return;
  seg = t->rootseg;
  while (seg) {
    for (i=0; i<MRB_SG_SEGMENT_SIZE; i++) {
      /* no value in last segment after last_len */
      if (!seg->next && i >= t->last_len) {
        return;
      }
      if ((*func)(mrb, seg->key[i], seg->val[i], p) != 0)
        return;
    }
    seg = seg->next;
  }
}

/* Get the size of the instance variable table. */
static size_t
sg_size(mrb_state *mrb, seglist *t)
{
  segment *seg;
  size_t size = 0;

  if (t == NULL) return 0;
  if (t->size > 0) return t->size;
  seg = t->rootseg;
  while (seg) {
    if (seg->next == NULL) {
      size += t->last_len;
      return size;
    }
    seg = seg->next;
    size += MRB_SG_SEGMENT_SIZE;
  }
  /* empty seglist */
  return 0;
}

/* Copy the instance variable table. */
static seglist*
sg_copy(mrb_state *mrb, seglist *t)
{
  segment *seg;
  seglist *t2;

  size_t i;

  seg = t->rootseg;
  t2 = sg_new(mrb);

  while (seg != NULL) {
    for (i=0; i<MRB_SG_SEGMENT_SIZE; i++) {
      mrb_value key = seg->key[i];
      mrb_value val = seg->val[i];

      if ((seg->next == NULL) && (i >= t->last_len)) {
        return t2;
      }
      sg_put(mrb, t2, key, val);
    }
    seg = seg->next;
  }
  return t2;
}

/* Free memory of the instance variable table. */
static void
sg_free(mrb_state *mrb, seglist *t)
{
  segment *seg;

  if (!t) return;
  seg = t->rootseg;
  while (seg) {
    segment *p = seg;
    seg = seg->next;
    mrb_free(mrb, p);
  }
  mrb_free(mrb, t);
}

static void mrb_hash_modify(mrb_state *mrb, mrb_value hash);

static inline mrb_value
mrb_hash_ht_key(mrb_state *mrb, mrb_value key)
{
  if (mrb_string_p(key) && !MRB_FROZEN_P(mrb_str_ptr(key))) {
    key = mrb_str_dup(mrb, key);
    MRB_SET_FROZEN_FLAG(mrb_str_ptr(key));
  }
  return key;
}

#define KEY(key) mrb_hash_ht_key(mrb, key)

static int
hash_mark_i(mrb_state *mrb, mrb_value key, mrb_value val, void *p)
{
  mrb_gc_mark_value(mrb, key);
  mrb_gc_mark_value(mrb, val);
  return 0;
}

void
mrb_gc_mark_hash(mrb_state *mrb, struct RHash *hash)
{
  sg_foreach(mrb, hash->ht, hash_mark_i, NULL);
}

size_t
mrb_gc_mark_hash_size(mrb_state *mrb, struct RHash *hash)
{
  if (!hash->ht) return 0;
  return sg_size(mrb, hash->ht)*2;
}

void
mrb_gc_free_hash(mrb_state *mrb, struct RHash *hash)
{
  sg_free(mrb, hash->ht);
}

MRB_API mrb_value
mrb_hash_new(mrb_state *mrb)
{
  struct RHash *h;

  h = (struct RHash*)mrb_obj_alloc(mrb, MRB_TT_HASH, mrb->hash_class);
  h->ht = 0;
  h->iv = 0;
  return mrb_obj_value(h);
}

MRB_API mrb_value
mrb_hash_new_capa(mrb_state *mrb, mrb_int capa)
{
  struct RHash *h;

  h = (struct RHash*)mrb_obj_alloc(mrb, MRB_TT_HASH, mrb->hash_class);
  /* preallocate segment list */
  h->ht = sg_new(mrb);
  /* capacity ignored */
  h->iv = 0;
  return mrb_obj_value(h);
}

static mrb_value mrb_hash_default(mrb_state *mrb, mrb_value hash);
static mrb_value hash_default(mrb_state *mrb, mrb_value hash, mrb_value key);

static mrb_value
mrb_hash_init_copy(mrb_state *mrb, mrb_value self)
{
  mrb_value orig;
  struct RHash* copy;
  seglist *orig_h;
  mrb_value ifnone, vret;

  mrb_get_args(mrb, "o", &orig);
  if (mrb_obj_equal(mrb, self, orig)) return self;
  if ((mrb_type(self) != mrb_type(orig)) || (mrb_obj_class(mrb, self) != mrb_obj_class(mrb, orig))) {
      mrb_raise(mrb, E_TYPE_ERROR, "initialize_copy should take same class object");
  }

  orig_h = RHASH_TBL(self);
  copy = (struct RHash*)mrb_obj_alloc(mrb, MRB_TT_HASH, mrb->hash_class);
  copy->ht = sg_copy(mrb, orig_h);

  if (MRB_RHASH_DEFAULT_P(self)) {
    copy->flags |= MRB_HASH_DEFAULT;
  }
  if (MRB_RHASH_PROCDEFAULT_P(self)) {
    copy->flags |= MRB_HASH_PROC_DEFAULT;
  }
  vret = mrb_obj_value(copy);
  ifnone = RHASH_IFNONE(self);
  if (!mrb_nil_p(ifnone)) {
      mrb_iv_set(mrb, vret, mrb_intern_lit(mrb, "ifnone"), ifnone);
  }
  return vret;
}

static int
check_kdict_i(mrb_state *mrb, mrb_value key, mrb_value val, void *data)
{
  if (!mrb_symbol_p(key)) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "keyword argument hash with non symbol keys");
  }
  return 0;
}

void
mrb_hash_check_kdict(mrb_state *mrb, mrb_value self)
{
  seglist *sg;

  sg = RHASH_TBL(self);
  if (!sg || sg_size(mrb, sg) == 0) return;
  sg_foreach(mrb, sg, check_kdict_i, NULL);
}

MRB_API mrb_value
mrb_hash_dup(mrb_state *mrb, mrb_value self)
{
  struct RHash* copy;
  seglist *orig_h;

  orig_h = RHASH_TBL(self);
  copy = (struct RHash*)mrb_obj_alloc(mrb, MRB_TT_HASH, mrb->hash_class);
  copy->ht = orig_h ? sg_copy(mrb, orig_h) : NULL;
  return mrb_obj_value(copy);
}

MRB_API mrb_bool
mrb_hash_has_key_p(mrb_state *mrb, mrb_value hash, mrb_value key)
{
  if (sg_get(mrb, RHASH_TBL(hash), key, NULL)) {
    return TRUE;
  }
  return FALSE;
}

MRB_API mrb_value
mrb_hash_get(mrb_state *mrb, mrb_value hash, mrb_value key)
{
  mrb_value val;
  mrb_sym mid;

  if (sg_get(mrb, RHASH_TBL(hash), key, &val)) {
    return val;
  }

  mid = mrb_intern_lit(mrb, "default");
  if (mrb_func_basic_p(mrb, hash, mid, mrb_hash_default)) {
    return hash_default(mrb, hash, key);
  }
  /* xxx mrb_funcall_tailcall(mrb, hash, "default", 1, key); */
  return mrb_funcall_argv(mrb, hash, mid, 1, &key);
}

MRB_API mrb_value
mrb_hash_fetch(mrb_state *mrb, mrb_value hash, mrb_value key, mrb_value def)
{
  mrb_value val;

  if (sg_get(mrb, RHASH_TBL(hash), key, &val)) {
    return val;
  }
  /* not found */
  return def;
}

MRB_API void
mrb_hash_set(mrb_state *mrb, mrb_value hash, mrb_value key, mrb_value val)
{
  mrb_hash_modify(mrb, hash);

  sg_put(mrb, RHASH_TBL(hash), key, val);
  mrb_field_write_barrier_value(mrb, (struct RBasic*)RHASH(hash), key);
  mrb_field_write_barrier_value(mrb, (struct RBasic*)RHASH(hash), val);
  return;
}

MRB_API mrb_value
mrb_ensure_hash_type(mrb_state *mrb, mrb_value hash)
{
  return mrb_convert_type(mrb, hash, MRB_TT_HASH, "Hash", "to_hash");
}

MRB_API mrb_value
mrb_check_hash_type(mrb_state *mrb, mrb_value hash)
{
  return mrb_check_convert_type(mrb, hash, MRB_TT_HASH, "Hash", "to_hash");
}

static void
mrb_hash_modify(mrb_state *mrb, mrb_value hash)
{
  if (MRB_FROZEN_P(mrb_hash_ptr(hash))) {
    mrb_raise(mrb, E_FROZEN_ERROR, "can't modify frozen hash");
  }

  if (!RHASH_TBL(hash)) {
    RHASH_TBL(hash) = sg_new(mrb);
  }
}

/* 15.2.13.4.16 */
/*
 *  call-seq:
 *     Hash.new                          -> new_hash
 *     Hash.new(obj)                     -> new_hash
 *     Hash.new {|hash, key| block }     -> new_hash
 *
 *  Returns a new, empty hash. If this hash is subsequently accessed by
 *  a key that doesn't correspond to a hash entry, the value returned
 *  depends on the style of <code>new</code> used to create the hash. In
 *  the first form, the access returns <code>nil</code>. If
 *  <i>obj</i> is specified, this single object will be used for
 *  all <em>default values</em>. If a block is specified, it will be
 *  called with the hash object and the key, and should return the
 *  default value. It is the block's responsibility to store the value
 *  in the hash if required.
 *
 *      h = Hash.new("Go Fish")
 *      h["a"] = 100
 *      h["b"] = 200
 *      h["a"]           #=> 100
 *      h["c"]           #=> "Go Fish"
 *      # The following alters the single default object
 *      h["c"].upcase!   #=> "GO FISH"
 *      h["d"]           #=> "GO FISH"
 *      h.keys           #=> ["a", "b"]
 *
 *      # While this creates a new default object each time
 *      h = Hash.new { |hash, key| hash[key] = "Go Fish: #{key}" }
 *      h["c"]           #=> "Go Fish: c"
 *      h["c"].upcase!   #=> "GO FISH: C"
 *      h["d"]           #=> "Go Fish: d"
 *      h.keys           #=> ["c", "d"]
 *
 */

static mrb_value
mrb_hash_init(mrb_state *mrb, mrb_value hash)
{
  mrb_value block, ifnone;
  mrb_bool ifnone_p;

  ifnone = mrb_nil_value();
  mrb_get_args(mrb, "&|o?", &block, &ifnone, &ifnone_p);
  mrb_hash_modify(mrb, hash);
  if (!mrb_nil_p(block)) {
    if (ifnone_p) {
      mrb_raise(mrb, E_ARGUMENT_ERROR, "wrong number of arguments");
    }
    RHASH(hash)->flags |= MRB_HASH_PROC_DEFAULT;
    ifnone = block;
  }
  if (!mrb_nil_p(ifnone)) {
    RHASH(hash)->flags |= MRB_HASH_DEFAULT;
    mrb_iv_set(mrb, hash, mrb_intern_lit(mrb, "ifnone"), ifnone);
  }
  return hash;
}

/* 15.2.13.4.2  */
/*
 *  call-seq:
 *     hsh[key]    ->  value
 *
 *  Element Reference---Retrieves the <i>value</i> object corresponding
 *  to the <i>key</i> object. If not found, returns the default value (see
 *  <code>Hash::new</code> for details).
 *
 *     h = { "a" => 100, "b" => 200 }
 *     h["a"]   #=> 100
 *     h["c"]   #=> nil
 *
 */
static mrb_value
mrb_hash_aget(mrb_state *mrb, mrb_value self)
{
  mrb_value key;

  mrb_get_args(mrb, "o", &key);
  return mrb_hash_get(mrb, self, key);
}

static mrb_value
hash_default(mrb_state *mrb, mrb_value hash, mrb_value key)
{
  if (MRB_RHASH_DEFAULT_P(hash)) {
    if (MRB_RHASH_PROCDEFAULT_P(hash)) {
      return mrb_funcall(mrb, RHASH_PROCDEFAULT(hash), "call", 2, hash, key);
    }
    else {
      return RHASH_IFNONE(hash);
    }
  }
  return mrb_nil_value();
}

/* 15.2.13.4.5  */
/*
 *  call-seq:
 *     hsh.default(key=nil)   -> obj
 *
 *  Returns the default value, the value that would be returned by
 *  <i>hsh</i>[<i>key</i>] if <i>key</i> did not exist in <i>hsh</i>.
 *  See also <code>Hash::new</code> and <code>Hash#default=</code>.
 *
 *     h = Hash.new                            #=> {}
 *     h.default                               #=> nil
 *     h.default(2)                            #=> nil
 *
 *     h = Hash.new("cat")                     #=> {}
 *     h.default                               #=> "cat"
 *     h.default(2)                            #=> "cat"
 *
 *     h = Hash.new {|h,k| h[k] = k.to_i*10}   #=> {}
 *     h.default                               #=> nil
 *     h.default(2)                            #=> 20
 */

static mrb_value
mrb_hash_default(mrb_state *mrb, mrb_value hash)
{
  mrb_value key;
  mrb_bool given;

  mrb_get_args(mrb, "|o?", &key, &given);
  if (MRB_RHASH_DEFAULT_P(hash)) {
    if (MRB_RHASH_PROCDEFAULT_P(hash)) {
      if (!given) return mrb_nil_value();
      return mrb_funcall(mrb, RHASH_PROCDEFAULT(hash), "call", 2, hash, key);
    }
    else {
      return RHASH_IFNONE(hash);
    }
  }
  return mrb_nil_value();
}

/* 15.2.13.4.6  */
/*
 *  call-seq:
 *     hsh.default = obj     -> obj
 *
 *  Sets the default value, the value returned for a key that does not
 *  exist in the hash. It is not possible to set the default to a
 *  <code>Proc</code> that will be executed on each key lookup.
 *
 *     h = { "a" => 100, "b" => 200 }
 *     h.default = "Go fish"
 *     h["a"]     #=> 100
 *     h["z"]     #=> "Go fish"
 *     # This doesn't do what you might hope...
 *     h.default = proc do |hash, key|
 *       hash[key] = key + key
 *     end
 *     h[2]       #=> #<Proc:0x401b3948@-:6>
 *     h["cat"]   #=> #<Proc:0x401b3948@-:6>
 */

static mrb_value
mrb_hash_set_default(mrb_state *mrb, mrb_value hash)
{
  mrb_value ifnone;

  mrb_get_args(mrb, "o", &ifnone);
  mrb_hash_modify(mrb, hash);
  mrb_iv_set(mrb, hash, mrb_intern_lit(mrb, "ifnone"), ifnone);
  RHASH(hash)->flags &= ~MRB_HASH_PROC_DEFAULT;
  if (!mrb_nil_p(ifnone)) {
    RHASH(hash)->flags |= MRB_HASH_DEFAULT;
  }
  else {
    RHASH(hash)->flags &= ~MRB_HASH_DEFAULT;
  }
  return ifnone;
}

/* 15.2.13.4.7  */
/*
 *  call-seq:
 *     hsh.default_proc -> anObject
 *
 *  If <code>Hash::new</code> was invoked with a block, return that
 *  block, otherwise return <code>nil</code>.
 *
 *     h = Hash.new {|h,k| h[k] = k*k }   #=> {}
 *     p = h.default_proc                 #=> #<Proc:0x401b3d08@-:1>
 *     a = []                             #=> []
 *     p.call(a, 2)
 *     a                                  #=> [nil, nil, 4]
 */


static mrb_value
mrb_hash_default_proc(mrb_state *mrb, mrb_value hash)
{
  if (MRB_RHASH_PROCDEFAULT_P(hash)) {
    return RHASH_PROCDEFAULT(hash);
  }
  return mrb_nil_value();
}

/*
 *  call-seq:
 *     hsh.default_proc = proc_obj     -> proc_obj
 *
 *  Sets the default proc to be executed on each key lookup.
 *
 *     h.default_proc = proc do |hash, key|
 *       hash[key] = key + key
 *     end
 *     h[2]       #=> 4
 *     h["cat"]   #=> "catcat"
 */

static mrb_value
mrb_hash_set_default_proc(mrb_state *mrb, mrb_value hash)
{
  mrb_value ifnone;

  mrb_get_args(mrb, "o", &ifnone);
  mrb_hash_modify(mrb, hash);
  mrb_iv_set(mrb, hash, mrb_intern_lit(mrb, "ifnone"), ifnone);
  if (!mrb_nil_p(ifnone)) {
    RHASH(hash)->flags |= MRB_HASH_PROC_DEFAULT;
    RHASH(hash)->flags |= MRB_HASH_DEFAULT;
  }
  else {
    RHASH(hash)->flags &= ~MRB_HASH_DEFAULT;
    RHASH(hash)->flags &= ~MRB_HASH_PROC_DEFAULT;
  }

  return ifnone;
}

MRB_API mrb_value
mrb_hash_delete_key(mrb_state *mrb, mrb_value hash, mrb_value key)
{
  seglist *sg = RHASH_TBL(hash);
  mrb_value del_val;

  if (sg_del(mrb, sg, key, &del_val)) {
    return del_val;
  }

  /* not found */
  return mrb_nil_value();
}

/* 15.2.13.4.8  */
/*
 *  call-seq:
 *     hsh.delete(key)                   -> value
 *     hsh.delete(key) {| key | block }  -> value
 *
 *  Deletes and returns a key-value pair from <i>hsh</i> whose key is
 *  equal to <i>key</i>. If the key is not found, returns the
 *  <em>default value</em>. If the optional code block is given and the
 *  key is not found, pass in the key and return the result of
 *  <i>block</i>.
 *
 *      h = { "a" => 100, "b" => 200 }
 *      h.delete("a")                              #=> 100
 *      h.delete("z")                              #=> nil
 *      h.delete("z") { |el| "#{el} not found" }   #=> "z not found"
 *
 */
static mrb_value
mrb_hash_delete(mrb_state *mrb, mrb_value self)
{
  mrb_value key;

  mrb_get_args(mrb, "o", &key);
  mrb_hash_modify(mrb, self);
  return mrb_hash_delete_key(mrb, self, key);
}

/* 15.2.13.4.24 */
/*
 *  call-seq:
 *     hsh.shift -> anArray or obj
 *
 *  Removes a key-value pair from <i>hsh</i> and returns it as the
 *  two-item array <code>[</code> <i>key, value</i> <code>]</code>, or
 *  the hash's default value if the hash is empty.
 *
 *      h = { 1 => "a", 2 => "b", 3 => "c" }
 *      h.shift   #=> [1, "a"]
 *      h         #=> {2=>"b", 3=>"c"}
 */

static mrb_value
mrb_hash_shift(mrb_state *mrb, mrb_value hash)
{
  seglist *sg = RHASH_TBL(hash);

  mrb_hash_modify(mrb, hash);
  if (sg && sg_size(mrb, sg) > 0) {
    mrb_value del_key = sg->rootseg->key[0];
    mrb_value del_val = sg->rootseg->val[0];
    sg_del(mrb, sg, del_key, NULL);
    return mrb_assoc_new(mrb, del_key, del_val);
  }

  if (MRB_RHASH_DEFAULT_P(hash)) {
    if (MRB_RHASH_PROCDEFAULT_P(hash)) {
      return mrb_funcall(mrb, RHASH_PROCDEFAULT(hash), "call", 2, hash, mrb_nil_value());
    }
    else {
      return RHASH_IFNONE(hash);
    }
  }
  return mrb_nil_value();
}

/* 15.2.13.4.4  */
/*
 *  call-seq:
 *     hsh.clear -> hsh
 *
 *  Removes all key-value pairs from `hsh`.
 *
 *      h = { "a" => 100, "b" => 200 }   #=> {"a"=>100, "b"=>200}
 *      h.clear                          #=> {}
 *
 */

MRB_API mrb_value
mrb_hash_clear(mrb_state *mrb, mrb_value hash)
{
  seglist *sg = RHASH_TBL(hash);

  mrb_hash_modify(mrb, hash);
  if (sg) {
    sg_free(mrb, sg);
    RHASH_TBL(hash) = NULL;
  }
  return hash;
}

/* 15.2.13.4.3  */
/* 15.2.13.4.26 */
/*
 *  call-seq:
 *     hsh[key] = value        -> value
 *     hsh.store(key, value)   -> value
 *
 *  Element Assignment---Associates the value given by
 *  <i>value</i> with the key given by <i>key</i>.
 *  <i>key</i> should not have its value changed while it is in
 *  use as a key (a <code>String</code> passed as a key will be
 *  duplicated and frozen).
 *
 *      h = { "a" => 100, "b" => 200 }
 *      h["a"] = 9
 *      h["c"] = 4
 *      h   #=> {"a"=>9, "b"=>200, "c"=>4}
 *
 */
static mrb_value
mrb_hash_aset(mrb_state *mrb, mrb_value self)
{
  mrb_value key, val;

  mrb_get_args(mrb, "oo", &key, &val);
  mrb_hash_set(mrb, self, key, val);
  return val;
}

/* 15.2.13.4.20 */
/* 15.2.13.4.25 */
/*
 *  call-seq:
 *     hsh.length    ->  fixnum
 *     hsh.size      ->  fixnum
 *
 *  Returns the number of key-value pairs in the hash.
 *
 *     h = { "d" => 100, "a" => 200, "v" => 300, "e" => 400 }
 *     h.length        #=> 4
 *     h.delete("a")   #=> 200
 *     h.length        #=> 3
 */
static mrb_value
mrb_hash_size_m(mrb_state *mrb, mrb_value self)
{
  seglist *sg = RHASH_TBL(self);

  if (!sg) return mrb_fixnum_value(0);
  return mrb_fixnum_value(sg_size(mrb, sg));
}

MRB_API mrb_bool
mrb_hash_empty_p(mrb_state *mrb, mrb_value self)
{
  seglist *sg = RHASH_TBL(self);

  if (!sg) return TRUE;
  return sg_size(mrb, sg) == 0;
}

/* 15.2.13.4.12 */
/*
 *  call-seq:
 *     hsh.empty?    -> true or false
 *
 *  Returns <code>true</code> if <i>hsh</i> contains no key-value pairs.
 *
 *     {}.empty?   #=> true
 *
 */
static mrb_value
mrb_hash_empty_m(mrb_state *mrb, mrb_value self)
{
  return mrb_bool_value(mrb_hash_empty_p(mrb, self));
}

/* 15.2.13.4.29 (x)*/
/*
 * call-seq:
 *    hsh.to_hash   => hsh
 *
 * Returns +self+.
 */

static mrb_value
mrb_hash_to_hash(mrb_state *mrb, mrb_value hash)
{
  return hash;
}

static int
hash_keys_i(mrb_state *mrb, mrb_value key, mrb_value val, void *p)
{
  mrb_ary_push(mrb, *(mrb_value*)p, key);
  return 0;
}

/* 15.2.13.4.19 */
/*
 *  call-seq:
 *     hsh.keys    -> array
 *
 *  Returns a new array populated with the keys from this hash. See also
 *  <code>Hash#values</code>.
 *
 *     h = { "a" => 100, "b" => 200, "c" => 300, "d" => 400 }
 *     h.keys   #=> ["a", "b", "c", "d"]
 *
 */

MRB_API mrb_value
mrb_hash_keys(mrb_state *mrb, mrb_value hash)
{
  seglist *sg = RHASH_TBL(hash);
  size_t size;
  mrb_value ary;

  if (!sg || (size = sg_size(mrb, sg)) == 0)
    return mrb_ary_new(mrb);
  ary = mrb_ary_new_capa(mrb, size);
  sg_foreach(mrb, sg, hash_keys_i, (void*)&ary);
  return ary;
}

static int
hash_vals_i(mrb_state *mrb, mrb_value key, mrb_value val, void *p)
{
  mrb_ary_push(mrb, *(mrb_value*)p, val);
  return 0;
}

/* 15.2.13.4.28 */
/*
 *  call-seq:
 *     hsh.values    -> array
 *
 *  Returns a new array populated with the values from <i>hsh</i>. See
 *  also <code>Hash#keys</code>.
 *
 *     h = { "a" => 100, "b" => 200, "c" => 300 }
 *     h.values   #=> [100, 200, 300]
 *
 */

MRB_API mrb_value
mrb_hash_values(mrb_state *mrb, mrb_value hash)
{
  seglist *sg = RHASH_TBL(hash);
  size_t size;
  mrb_value ary;

  if (!sg || (size = sg_size(mrb, sg)) == 0)
    return mrb_ary_new(mrb);
  ary = mrb_ary_new_capa(mrb, size);
  sg_foreach(mrb, sg, hash_vals_i, (void*)&ary);
  return ary;
}

/* 15.2.13.4.13 */
/* 15.2.13.4.15 */
/* 15.2.13.4.18 */
/* 15.2.13.4.21 */
/*
 *  call-seq:
 *     hsh.has_key?(key)    -> true or false
 *     hsh.include?(key)    -> true or false
 *     hsh.key?(key)        -> true or false
 *     hsh.member?(key)     -> true or false
 *
 *  Returns <code>true</code> if the given key is present in <i>hsh</i>.
 *
 *     h = { "a" => 100, "b" => 200 }
 *     h.has_key?("a")   #=> true
 *     h.has_key?("z")   #=> false
 *
 */

MRB_API mrb_bool
mrb_hash_key_p(mrb_state *mrb, mrb_value hash, mrb_value key)
{
  seglist *sg;

  sg = RHASH_TBL(hash);
  if (sg_get(mrb, sg, key, NULL)) {
    return TRUE;
  }
  return FALSE;
}

static mrb_value
mrb_hash_has_key(mrb_state *mrb, mrb_value hash)
{
  mrb_value key;
  mrb_bool key_p;

  mrb_get_args(mrb, "o", &key);
  key_p = mrb_hash_key_p(mrb, hash, key);
  return mrb_bool_value(key_p);
}

struct has_v_arg {
  mrb_bool found;
  mrb_value val;
};

static int
hash_has_value_i(mrb_state *mrb, mrb_value key, mrb_value val, void *p)
{
  struct has_v_arg *arg = (struct has_v_arg*)p;
  
  if (mrb_equal(mrb, arg->val, val)) {
    arg->found = TRUE;
    return 1;
  }
  return 0;
}

/* 15.2.13.4.14 */
/* 15.2.13.4.27 */
/*
 *  call-seq:
 *     hsh.has_value?(value)    -> true or false
 *     hsh.value?(value)        -> true or false
 *
 *  Returns <code>true</code> if the given value is present for some key
 *  in <i>hsh</i>.
 *
 *     h = { "a" => 100, "b" => 200 }
 *     h.has_value?(100)   #=> true
 *     h.has_value?(999)   #=> false
 */

static mrb_value
mrb_hash_has_value(mrb_state *mrb, mrb_value hash)
{
  mrb_value val;
  struct has_v_arg arg;
  
  mrb_get_args(mrb, "o", &val);
  arg.found = FALSE;
  arg.val = val;
  sg_foreach(mrb, RHASH_TBL(hash), hash_has_value_i, &arg);
  return mrb_bool_value(arg.found);
}

static int
merge_i(mrb_state *mrb, mrb_value key, mrb_value val, void *data)
{
  seglist *h1 = (seglist*)data;

  sg_put(mrb, h1, key, val);
  return 0;
}

MRB_API void
mrb_hash_merge(mrb_state *mrb, mrb_value hash1, mrb_value hash2)
{
  seglist *h1, *h2;

  mrb_hash_modify(mrb, hash1);
  hash2 = mrb_ensure_hash_type(mrb, hash2);
  h1 = RHASH_TBL(hash1);
  h2 = RHASH_TBL(hash2);

  if (!h2) return;
  if (!h1) {
    RHASH_TBL(hash1) = sg_copy(mrb, h2);
    return;
  }
  sg_foreach(mrb, h2, merge_i, h1);
  mrb_write_barrier(mrb, (struct RBasic*)RHASH(hash1));
  return;
}

void
mrb_init_hash(mrb_state *mrb)
{
  struct RClass *h;

  mrb->hash_class = h = mrb_define_class(mrb, "Hash", mrb->object_class);              /* 15.2.13 */
  MRB_SET_INSTANCE_TT(h, MRB_TT_HASH);

  mrb_define_method(mrb, h, "initialize_copy", mrb_hash_init_copy,   MRB_ARGS_REQ(1));
  mrb_define_method(mrb, h, "[]",              mrb_hash_aget,        MRB_ARGS_REQ(1)); /* 15.2.13.4.2  */
  mrb_define_method(mrb, h, "[]=",             mrb_hash_aset,        MRB_ARGS_REQ(2)); /* 15.2.13.4.3  */
  mrb_define_method(mrb, h, "clear",           mrb_hash_clear,       MRB_ARGS_NONE()); /* 15.2.13.4.4  */
  mrb_define_method(mrb, h, "default",         mrb_hash_default,     MRB_ARGS_ANY());  /* 15.2.13.4.5  */
  mrb_define_method(mrb, h, "default=",        mrb_hash_set_default, MRB_ARGS_REQ(1)); /* 15.2.13.4.6  */
  mrb_define_method(mrb, h, "default_proc",    mrb_hash_default_proc,MRB_ARGS_NONE()); /* 15.2.13.4.7  */
  mrb_define_method(mrb, h, "default_proc=",   mrb_hash_set_default_proc,MRB_ARGS_REQ(1)); /* 15.2.13.4.7  */
  mrb_define_method(mrb, h, "__delete",        mrb_hash_delete,      MRB_ARGS_REQ(1)); /* core of 15.2.13.4.8  */
  mrb_define_method(mrb, h, "empty?",          mrb_hash_empty_m,     MRB_ARGS_NONE()); /* 15.2.13.4.12 */
  mrb_define_method(mrb, h, "has_key?",        mrb_hash_has_key,     MRB_ARGS_REQ(1)); /* 15.2.13.4.13 */
  mrb_define_method(mrb, h, "has_value?",      mrb_hash_has_value,   MRB_ARGS_REQ(1)); /* 15.2.13.4.14 */
  mrb_define_method(mrb, h, "include?",        mrb_hash_has_key,     MRB_ARGS_REQ(1)); /* 15.2.13.4.15 */
  mrb_define_method(mrb, h, "initialize",      mrb_hash_init,        MRB_ARGS_OPT(1)); /* 15.2.13.4.16 */
  mrb_define_method(mrb, h, "key?",            mrb_hash_has_key,     MRB_ARGS_REQ(1)); /* 15.2.13.4.18 */
  mrb_define_method(mrb, h, "keys",            mrb_hash_keys,        MRB_ARGS_NONE()); /* 15.2.13.4.19 */
  mrb_define_method(mrb, h, "length",          mrb_hash_size_m,      MRB_ARGS_NONE()); /* 15.2.13.4.20 */
  mrb_define_method(mrb, h, "member?",         mrb_hash_has_key,     MRB_ARGS_REQ(1)); /* 15.2.13.4.21 */
  mrb_define_method(mrb, h, "shift",           mrb_hash_shift,       MRB_ARGS_NONE()); /* 15.2.13.4.24 */
  mrb_define_method(mrb, h, "size",            mrb_hash_size_m,      MRB_ARGS_NONE()); /* 15.2.13.4.25 */
  mrb_define_method(mrb, h, "store",           mrb_hash_aset,        MRB_ARGS_REQ(2)); /* 15.2.13.4.26 */
  mrb_define_method(mrb, h, "value?",          mrb_hash_has_value,   MRB_ARGS_REQ(1)); /* 15.2.13.4.27 */
  mrb_define_method(mrb, h, "values",          mrb_hash_values,      MRB_ARGS_NONE()); /* 15.2.13.4.28 */

  mrb_define_method(mrb, h, "to_hash",         mrb_hash_to_hash,     MRB_ARGS_NONE()); /* 15.2.13.4.29 (x)*/
}
