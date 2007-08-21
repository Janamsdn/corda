#include "heap.h"
#include "system.h"
#include "common.h"

using namespace vm;

namespace {

// an object must survive TenureThreshold + 2 garbage collections
// before being copied to gen2 (muat be at least 1):
const unsigned TenureThreshold = 3;

const unsigned Top = ~static_cast<unsigned>(0);

const unsigned InitialGen2CapacityInBytes = 4 * 1024 * 1024;

const bool Verbose = true;
const bool Verbose2 = false;
const bool Debug = false;

class Context;

void NO_RETURN abort(Context*);
#ifndef NDEBUG
void assert(Context*, bool);
#endif

System* system(Context*);

inline object
get(object o, unsigned offsetInWords)
{
  return mask(cast<object>(o, offsetInWords * BytesPerWord));
}

inline object*
getp(object o, unsigned offsetInWords)
{
  return &cast<object>(o, offsetInWords * BytesPerWord);
}

inline void
set(object* o, object value)
{
  *o = reinterpret_cast<object>
    (reinterpret_cast<uintptr_t>(value)
     | reinterpret_cast<uintptr_t>(*o) & (~PointerMask));
}

inline void
set(object o, unsigned offsetInWords, object value)
{
  set(getp(o, offsetInWords), value);
}

class Segment {
 public:
  class Map {
   public:
    class Iterator {
     public:
      Map* map;
      unsigned index;
      unsigned limit;
      
      Iterator(Map* map, unsigned start, unsigned end):
        map(map)
      {
        assert(map->segment->context, map->bitsPerRecord == 1);
        assert(map->segment->context, map->segment);
        assert(map->segment->context, start <= map->segment->position());

        if (end > map->segment->position()) end = map->segment->position();

        index = map->indexOf(start);
        limit = map->indexOf(end);

        if ((end - start) % map->scale) ++ limit;
      }

      bool hasMore() {
        unsigned word = wordOf(index);
        unsigned bit = bitOf(index);
        unsigned wordLimit = wordOf(limit);
        unsigned bitLimit = bitOf(limit);

        for (; word <= wordLimit and (word < wordLimit or bit < bitLimit);
             ++word)
        {
          uintptr_t* p = map->data() + word;
          if (*p) {
            for (; bit < BitsPerWord and (word < wordLimit or bit < bitLimit);
                 ++bit)
            {
              if (map->data()[word] & (static_cast<uintptr_t>(1) << bit)) {
                index = ::indexOf(word, bit);
//                 printf("hit at index %d\n", index);
                return true;
              } else {
//                 printf("miss at index %d\n", indexOf(word, bit));
              }
            }
          }
          bit = 0;
        }

        index = limit;

        return false;
      }
      
      unsigned next() {
        assert(map->segment->context, hasMore());
        assert(map->segment->context, map->segment);

        return (index++) * map->scale;
      }
    };

    Segment* segment;
    Map* child;
    unsigned bitsPerRecord;
    unsigned scale;
    bool clearNewData;

    Map(Segment* segment, unsigned bitsPerRecord, unsigned scale,
        Map* child, bool clearNewData):
      segment(segment),
      child(child),
      bitsPerRecord(bitsPerRecord),
      scale(scale),
      clearNewData(clearNewData)
    { }

    void init() {
      assert(segment->context, bitsPerRecord);
      assert(segment->context, scale);
      assert(segment->context, powerOfTwo(scale));

      if (clearNewData) {
        memset(data(), 0, size() * BytesPerWord);
      }

      if (child) {
        child->init();
      }
    }

    void replaceWith(Map* m) {
      assert(segment->context, bitsPerRecord == m->bitsPerRecord);
      assert(segment->context, scale == m->scale);

      m->segment = 0;
      
      if (child) child->replaceWith(m->child);
    }

    unsigned offset(unsigned capacity) {
      unsigned n = 0;
      if (child) n += child->footprint(capacity);
      return n;
    }

    unsigned offset() {
      return offset(segment->capacity());
    }

    uintptr_t* data() {
      return segment->data + segment->capacity() + offset();
    }

    unsigned size(unsigned capacity) {
      unsigned result
        = ceiling(ceiling(capacity, scale) * bitsPerRecord, BitsPerWord);
      assert(segment->context, result);
      return result;
    }

    unsigned size() {
      return size(max(segment->capacity(), 1));
    }

    unsigned indexOf(unsigned segmentIndex) {
      return (segmentIndex / scale) * bitsPerRecord;
    }

    unsigned indexOf(void* p) {
      assert(segment->context, segment->almostContains(p));
      assert(segment->context, segment->capacity());
      return indexOf(segment->indexOf(p));
    }

    void update(uintptr_t* newData, unsigned capacity) {
      assert(segment->context, capacity >= segment->capacity());

      uintptr_t* p = newData + offset(capacity);
      if (segment->position()) {
        memcpy(p, data(), size(segment->position()) * BytesPerWord);
      }

      if (child) {
        child->update(newData, capacity);
      }
    }

    void clearBit(unsigned i) {
      assert(segment->context, wordOf(i) < size());

      data()[wordOf(i)] &= ~(static_cast<uintptr_t>(1) << bitOf(i));
    }

    void setBit(unsigned i) {
      assert(segment->context, wordOf(i) < size());

      data()[wordOf(i)] |= static_cast<uintptr_t>(1) << bitOf(i);
    }

    void clearOnlyIndex(unsigned index) {
      for (unsigned i = index, limit = index + bitsPerRecord; i < limit; ++i) {
        clearBit(i);
      }
    }

    void clearOnly(unsigned segmentIndex) {
      clearOnlyIndex(indexOf(segmentIndex));
    }

    void clearOnly(void* p) {
      clearOnlyIndex(indexOf(p));
    }

    void clear(void* p) {
      clearOnly(p);
      if (child) child->clear(p);
    }

    void setOnlyIndex(unsigned index, unsigned v = 1) {
      unsigned i = index + bitsPerRecord - 1;
      while (true) {
        if (v & 1) setBit(i); else clearBit(i);
        v >>= 1;
        if (i == index) break;
        --i;
      }
    }

    void setOnly(unsigned segmentIndex, unsigned v = 1) {
      setOnlyIndex(indexOf(segmentIndex), v);
    }

    void setOnly(void* p, unsigned v = 1) {
      setOnlyIndex(indexOf(p), v);
    }

    void set(void* p, unsigned v = 1) {
      setOnly(p, v);
      assert(segment->context, get(p) == v);
      if (child) child->set(p, v);
    }

    unsigned get(void* p) {
      unsigned index = indexOf(p);
      unsigned v = 0;
      for (unsigned i = index, limit = index + bitsPerRecord; i < limit; ++i) {
        unsigned wi = bitOf(i);
        v <<= 1;
        v |= ((data()[wordOf(i)]) & (static_cast<uintptr_t>(1) << wi)) >> wi;
      }
      return v;
    }

    unsigned footprint(unsigned capacity) {
      unsigned n = size(capacity);
      if (child) n += child->footprint(capacity);
      return n;
    }
  };

  Context* context;
  uintptr_t* data;
  unsigned position_;
  unsigned capacity_;
  Map* map;

  Segment(Context* context, Map* map, unsigned desired, unsigned minimum):
    context(context),
    data(0),
    position_(0),
    capacity_(0),
    map(map)
  {
    if (desired) {
      assert(context, desired >= minimum);

      capacity_ = desired;
      while (data == 0) {
        data = static_cast<uintptr_t*>
          (system(context)->tryAllocate
           ((capacity_ + map->footprint(capacity_)) * BytesPerWord));

        if (data == 0) {
          if (capacity_ > minimum) {
            capacity_ = avg(minimum, capacity_);
            if (capacity_ == 0) {
              break;
            }
          } else {
            abort(context);
          }
        }
      }

      if (map) {
        map->init();
      }
    }
  }

  unsigned capacity() {
    return capacity_;
  }

  unsigned position() {
    return position_;
  }

  unsigned remaining() {
    return capacity() - position();
  }

  void replaceWith(Segment* s) {
    system(context)->free(data);
    data = s->data;
    s->data = 0;

    position_ = s->position_;
    s->position_ = 0;

    capacity_ = s->capacity_;
    s->capacity_ = 0;

    if (s->map) {
      if (map) {
        map->replaceWith(s->map);
        s->map = 0;
      } else {
        abort(context);
      }
    } else {
      map = 0;
    }    
  }

  bool contains(void* p) {
    return position() and p >= data and p < data + position();
  }

  bool almostContains(void* p) {
    return contains(p) or p == data + position();
  }

  void* get(unsigned offset) {
    assert(context, offset <= position());
    return data + offset;
  }

  unsigned indexOf(void* p) {
    assert(context, almostContains(p));
    return static_cast<uintptr_t*>(p) - data;
  }

  void* allocate(unsigned size) {
    assert(context, size);
    assert(context, position() + size <= capacity());

    void* p = data + position();
    position_ += size;
    return p;
  }

  void dispose() {
    system(context)->free(data);
    data = 0;
    map = 0;
  }
};

class Context {
 public:
  Context(System* system):
    system(system),
    client(0),

    ageMap(&gen1, log(TenureThreshold), 1, 0, false),
    gen1(this, &ageMap, 0, 0),

    nextAgeMap(&nextGen1, log(TenureThreshold), 1, 0, false),
    nextGen1(this, &nextAgeMap, 0, 0),

    pointerMap(&gen2, 1, 1, 0, true),
    pageMap(&gen2, 1, LikelyPageSizeInBytes / BytesPerWord, &pointerMap, true),
    heapMap(&gen2, 1, pageMap.scale * 1024, &pageMap, true),
    gen2(this, &heapMap, 0, 0),

    nextPointerMap(&nextGen2, 1, 1, 0, true),
    nextPageMap(&nextGen2, 1, LikelyPageSizeInBytes / BytesPerWord,
                &nextPointerMap, true),
    nextHeapMap(&nextGen2, 1, nextPageMap.scale * 1024, &nextPageMap, true),
    nextGen2(this, &nextHeapMap, 0, 0),

    gen2Base(0),
    tenureFootprint(0),
    gen1padding(0),
    gen2padding(0),
    mode(Heap::MinorCollection),

    lastCollectionTime(system->now()),
    totalCollectionTime(0),
    totalTime(0)
  { }

  void dispose() {
    gen1.dispose();
    nextGen1.dispose();
    gen2.dispose();
    nextGen2.dispose();
  }

  System* system;
  Heap::Client* client;
  
  Segment::Map ageMap;
  Segment gen1;

  Segment::Map nextAgeMap;
  Segment nextGen1;

  Segment::Map pointerMap;
  Segment::Map pageMap;
  Segment::Map heapMap;
  Segment gen2;

  Segment::Map nextPointerMap;
  Segment::Map nextPageMap;
  Segment::Map nextHeapMap;
  Segment nextGen2;

  unsigned gen2Base;
  
  unsigned tenureFootprint;
  unsigned gen1padding;
  unsigned gen2padding;

  Heap::CollectionType mode;

  int64_t lastCollectionTime;
  int64_t totalCollectionTime;
  int64_t totalTime;
};

inline System*
system(Context* c)
{
  return c->system;
}

const char*
segment(Context* c, void* p)
{
  if (c->gen1.contains(p)) {
    return "gen1";
  } else if (c->nextGen1.contains(p)) {
    return "nextGen1";
  } else if (c->gen2.contains(p)) {
    return "gen2";
  } else if (c->nextGen2.contains(p)) {
    return "nextGen2";
  } else {
    return "none";
  }
}

inline void NO_RETURN
abort(Context* c)
{
  abort(c->system);
}

#ifndef NDEBUG
inline void
assert(Context* c, bool v)
{
  assert(c->system, v);
}
#endif

inline void
initNextGen1(Context* c, unsigned footprint)
{
  new (&(c->nextAgeMap)) Segment::Map
    (&(c->nextGen1), log(TenureThreshold), 1, 0, false);

  unsigned minimum
    = (c->gen1.position() - c->tenureFootprint) + footprint + c->gen1padding;
  unsigned desired = minimum;

  new (&(c->nextGen1)) Segment(c, &(c->nextAgeMap), desired, minimum);

  if (Verbose2) {
    fprintf(stderr, "init nextGen1 to %d bytes\n",
            c->nextGen1.capacity() * BytesPerWord);
  }
}

inline void
initNextGen2(Context* c)
{
  new (&(c->nextPointerMap)) Segment::Map
    (&(c->nextGen2), 1, 1, 0, true);

  new (&(c->nextPageMap)) Segment::Map
    (&(c->nextGen2), 1, LikelyPageSizeInBytes / BytesPerWord,
     &(c->nextPointerMap), true);

  new (&(c->nextHeapMap)) Segment::Map
    (&(c->nextGen2), 1, c->pageMap.scale * 1024, &(c->nextPageMap), true);

  unsigned minimum = c->gen2.position() + c->tenureFootprint + c->gen2padding;
  unsigned desired = max
    (minimum * 2, InitialGen2CapacityInBytes / BytesPerWord);

  new (&(c->nextGen2)) Segment(c, &(c->nextHeapMap), desired, minimum);

  if (Verbose2) {
    fprintf(stderr, "init nextGen2 to %d bytes\n",
            c->nextGen2.capacity() * BytesPerWord);
  }
}

inline bool
fresh(Context* c, object o)
{
  return c->nextGen1.contains(o)
    or c->nextGen2.contains(o)
    or (c->gen2.contains(o) and c->gen2.indexOf(o) >= c->gen2Base);
}

inline bool
wasCollected(Context* c, object o)
{
  return o and (not fresh(c, o)) and fresh(c, get(o, 0));
}

inline object
follow(Context* c UNUSED, object o)
{
  assert(c, wasCollected(c, o));
  return cast<object>(o, 0);
}

inline object&
parent(Context* c UNUSED, object o)
{
  assert(c, wasCollected(c, o));
  return cast<object>(o, BytesPerWord);
}

inline uintptr_t*
bitset(Context* c UNUSED, object o)
{
  assert(c, wasCollected(c, o));
  return &cast<uintptr_t>(o, BytesPerWord * 2);
}

inline object
copyTo(Context* c, Segment* s, object o, unsigned size)
{
  assert(c, s->remaining() >= size);
  object dst = s->allocate(size);
  c->client->copy(o, dst);
  return dst;
}

object
copy2(Context* c, object o)
{
  unsigned size = c->client->copiedSizeInWords(o);

  if (c->gen2.contains(o)) {
    assert(c, c->mode == Heap::MajorCollection);

    return copyTo(c, &(c->nextGen2), o, size);
  } else if (c->gen1.contains(o)) {
    unsigned age = c->ageMap.get(o);
    if (age == TenureThreshold) {
      if (c->mode == Heap::MinorCollection) {
        assert(c, c->gen2.remaining() >= size);

        if (c->gen2Base == Top) {
          c->gen2Base = c->gen2.position();
        }

        return copyTo(c, &(c->gen2), o, size);
      } else {
        return copyTo(c, &(c->nextGen2), o, size);
      }
    } else {
      o = copyTo(c, &(c->nextGen1), o, size);

      c->nextAgeMap.setOnly(o, age + 1);
      if (age + 1 == TenureThreshold) {
        c->tenureFootprint += size;
      }

      return o;
    }
  } else {
    assert(c, not c->nextGen1.contains(o));
    assert(c, not c->nextGen2.contains(o));

    o = copyTo(c, &(c->nextGen1), o, size);

    c->nextAgeMap.clear(o);

    return o;
  }
}

object
copy(Context* c, object o)
{
  object r = copy2(c, o);

  if (Debug) {
    fprintf(stderr, "copy %p (%s) to %p (%s)\n",
            o, segment(c, o), r, segment(c, r));
  }

  // leave a pointer to the copy in the original
  cast<object>(o, 0) = r;

  return r;
}

object
update3(Context* c, object o, bool* needsVisit)
{
  if (wasCollected(c, o)) {
    *needsVisit = false;
    return follow(c, o);
  } else {
    *needsVisit = true;
    return copy(c, o);
  }
}

object
update2(Context* c, object o, bool* needsVisit)
{
  if (c->mode == Heap::MinorCollection and c->gen2.contains(o)) {
    *needsVisit = false;
    return o;
  }

  return update3(c, o, needsVisit);
}

object
update(Context* c, object* p, bool* needsVisit)
{
  if (mask(*p) == 0) {
    *needsVisit = false;
    return 0;
  }

  object r = update2(c, mask(*p), needsVisit);

  // update heap map.
  if (r) {
    if (c->mode == Heap::MinorCollection) {
      if (c->gen2.contains(p) and not c->gen2.contains(r)) {
        if (Debug) {        
          fprintf(stderr, "mark %p (%s) at %p (%s)\n",
                  r, segment(c, r), p, segment(c, p));
        }

        c->heapMap.set(p);
      }
    } else {
      if (c->nextGen2.contains(p) and not c->nextGen2.contains(r)) {
        if (Debug) {        
          fprintf(stderr, "mark %p (%s) at %p (%s)\n",
                  r, segment(c, r), p, segment(c, p));
        }

        c->nextHeapMap.set(p);
      }      
    }
  }

  return r;
}

const uintptr_t BitsetExtensionBit
= (static_cast<uintptr_t>(1) << (BitsPerWord - 1));

void
bitsetInit(uintptr_t* p)
{
  memset(p, 0, BytesPerWord);
}

void
bitsetClear(uintptr_t* p, unsigned start, unsigned end)
{
  if (end < BitsPerWord - 1) {
    // do nothing
  } else if (start < BitsPerWord - 1) {
    memset(p + 1, 0, (wordOf(end + (BitsPerWord * 2) + 1)) * BytesPerWord);
  } else {
    unsigned startWord = wordOf(start + (BitsPerWord * 2) + 1);
    unsigned endWord = wordOf(end + (BitsPerWord * 2) + 1);
    if (endWord > startWord) {
      memset(p + startWord + 1, 0, (endWord - startWord) * BytesPerWord);
    }
  }
}

void
bitsetSet(uintptr_t* p, unsigned i, bool v)
{
  if (i >= BitsPerWord - 1) {
    i += (BitsPerWord * 2) + 1;
    if (v) {
      p[0] |= BitsetExtensionBit;
      if (p[2] <= wordOf(i) - 3) p[2] = wordOf(i) - 2;
    }
  }

  if (v) {
    p[wordOf(i)] |= static_cast<uintptr_t>(1) << bitOf(i);
  } else {
    p[wordOf(i)] &= ~(static_cast<uintptr_t>(1) << bitOf(i));
  }
}

bool
bitsetHasMore(uintptr_t* p)
{
  switch (*p) {
  case 0: return false;

  case BitsetExtensionBit: {
    uintptr_t length = p[2];
    uintptr_t word = wordOf(p[1]);
    for (; word < length; ++word) {
      if (p[word + 3]) {
        p[1] = indexOf(word, 0);
        return true;
      }
    }
    p[1] = indexOf(word, 0);
    return false;
  }

  default: return true;
  }
}

unsigned
bitsetNext(Context* c, uintptr_t* p)
{
  bool more UNUSED = bitsetHasMore(p);
  assert(c, more);

  switch (*p) {
  case 0: abort(c);

  case BitsetExtensionBit: {
    uintptr_t i = p[1];
    uintptr_t word = wordOf(i);
    assert(c, word < p[2]);
    for (uintptr_t bit = bitOf(i); bit < BitsPerWord; ++bit) {
      if (p[word + 3] & (static_cast<uintptr_t>(1) << bit)) {
        p[1] = indexOf(word, bit) + 1;
        bitsetSet(p, p[1] + BitsPerWord - 2, false);
        return p[1] + BitsPerWord - 2;
      }
    }
    abort(c);
  }

  default: {
    for (unsigned i = 0; i < BitsPerWord - 1; ++i) {
      if (*p & (static_cast<uintptr_t>(1) << i)) {
        bitsetSet(p, i, false);
        return i;
      }
    }
    abort(c);
  }
  }
}

void
collect(Context* c, object* p)
{
  object original = mask(*p);
  object parent = 0;
  
  if (Debug) {
    fprintf(stderr, "update %p (%s) at %p (%s)\n",
            mask(*p), segment(c, *p), p, segment(c, p));
  }

  bool needsVisit;
  set(p, update(c, mask(p), &needsVisit));

  if (Debug) {
    fprintf(stderr, "  result: %p (%s) (visit? %d)\n",
            mask(*p), segment(c, *p), needsVisit);
  }

  if (not needsVisit) return;

 visit: {
    object copy = follow(c, original);

    class Walker : public Heap::Walker {
     public:
      Walker(Context* c, object copy, uintptr_t* bitset):
        c(c),
        copy(copy),
        bitset(bitset),
        first(0),
        second(0),
        last(0),
        visits(0),
        total(0)
      { }

      virtual bool visit(unsigned offset) {
        if (Debug) {
          fprintf(stderr, "  update %p (%s) at %p - offset %d from %p (%s)\n",
                  get(copy, offset),
                  segment(c, get(copy, offset)),
                  getp(copy, offset),
                  offset,
                  copy,
                  segment(c, copy));
        }

        bool needsVisit;
        object childCopy = update(c, getp(copy, offset), &needsVisit);
        
        if (Debug) {
          fprintf(stderr, "    result: %p (%s) (visit? %d)\n",
                  childCopy, segment(c, childCopy), needsVisit);
        }

        ++ total;

        if (total == 3) {
          bitsetInit(bitset);
        }

        if (needsVisit) {
          ++ visits;

          if (visits == 1) {
            first = offset;
          } else if (visits == 2) {
            second = offset;
          }
        } else {
          set(copy, offset, childCopy);
        }

        if (visits > 1 and total > 2 and (second or needsVisit)) {
          bitsetClear(bitset, last, offset);
          last = offset;

          if (second) {
            bitsetSet(bitset, second, true);
            second = 0;
          }
          
          if (needsVisit) {
            bitsetSet(bitset, offset, true);
          }
        }

        return true;
      }

      Context* c;
      object copy;
      uintptr_t* bitset;
      unsigned first;
      unsigned second;
      unsigned last;
      unsigned visits;
      unsigned total;
    } walker(c, copy, bitset(c, original));

    if (Debug) {
      fprintf(stderr, "walk %p (%s)\n", copy, segment(c, copy));
    }

    c->client->walk(copy, &walker);

    if (walker.visits) {
      // descend
      if (walker.visits > 1) {
        ::parent(c, original) = parent;
        parent = original;
      }

      original = get(copy, walker.first);
      set(copy, walker.first, follow(c, original));
      goto visit;
    } else {
      // ascend
      original = parent;
    }
  }

  if (original) {
    object copy = follow(c, original);

    class Walker : public Heap::Walker {
     public:
      Walker(Context* c, uintptr_t* bitset):
        c(c),
        bitset(bitset),
        next(0),
        total(0)
      { }

      virtual bool visit(unsigned offset) {
        switch (++ total) {
        case 1:
          return true;

        case 2:
          next = offset;
          return true;
          
        case 3:
          next = bitsetNext(c, bitset);
          return false;

        default:
          abort(c);
        }
      }

      Context* c;
      uintptr_t* bitset;
      unsigned next;
      unsigned total;
    } walker(c, bitset(c, original));

    if (Debug) {
      fprintf(stderr, "scan %p\n", copy);
    }

    c->client->walk(copy, &walker);

    assert(c, walker.total > 1);

    if (walker.total == 3 and bitsetHasMore(bitset(c, original))) {
      parent = original;
    } else {
      parent = ::parent(c, original);
    }

    if (Debug) {
      fprintf(stderr, "  next is %p (%s) at %p - offset %d from %p (%s)\n",
              get(copy, walker.next),
              segment(c, get(copy, walker.next)),
              getp(copy, walker.next),
              walker.next,
              copy,
              segment(c, copy));
    }

    original = get(copy, walker.next);
    set(copy, walker.next, follow(c, original));
    goto visit;
  } else {
    return;
  }
}

void
collect(Context* c, Segment::Map* map, unsigned start, unsigned end,
        bool* dirty, bool expectDirty UNUSED)
{
  bool wasDirty = false;
  for (Segment::Map::Iterator it(map, start, end); it.hasMore();) {
    wasDirty = true;
    if (map->child) {
      assert(c, map->scale > 1);
      unsigned s = it.next();
      unsigned e = s + map->scale;

      map->clearOnly(s);
      bool childDirty = false;
      collect(c, map->child, s, e, &childDirty, true);
      if (childDirty) {
        map->setOnly(s);
        *dirty = true;
      }
    } else {
      assert(c, map->scale == 1);
      object* p = reinterpret_cast<object*>(map->segment->get(it.next()));

      map->clearOnly(p);
      if (c->nextGen1.contains(*p)) {
        map->setOnly(p);
        *dirty = true;
      } else {
        collect(c, p);

        if (not c->gen2.contains(*p)) {
          map->setOnly(p);
          *dirty = true;
        }
      }
    }
  }

  assert(c, wasDirty or not expectDirty);
}

void
collect2(Context* c)
{
  c->gen2Base = Top;
  c->tenureFootprint = 0;
  c->gen1padding = 0;
  c->gen2padding = 0;

  if (c->mode == Heap::MinorCollection and c->gen2.position()) {
    unsigned start = 0;
    unsigned end = start + c->gen2.position();
    bool dirty;
    collect(c, &(c->heapMap), start, end, &dirty, false);
  }

  class Visitor : public Heap::Visitor {
   public:
    Visitor(Context* c): c(c) { }

    virtual void visit(void** p) {
      collect(c, p);
    }

    Context* c;
  } v(c);

  c->client->visitRoots(&v);
}

void
collect(Context* c, unsigned footprint)
{
  if (c->tenureFootprint > c->gen2.remaining()) {
    c->mode = Heap::MajorCollection;
  }

  int64_t then;
  if (Verbose) {
    if (c->mode == Heap::MajorCollection) {
      fprintf(stderr, "major collection\n");
    } else {
      fprintf(stderr, "minor collection\n");
    }

    then = c->system->now();
  }

  initNextGen1(c, footprint);
  if (c->mode == Heap::MajorCollection) {
    initNextGen2(c);
  }

  collect2(c);

  c->gen1.replaceWith(&(c->nextGen1));
  if (c->mode == Heap::MajorCollection) {
    c->gen2.replaceWith(&(c->nextGen2));
  }

  if (Verbose) {
    int64_t now = c->system->now();
    int64_t collection = now - then;
    int64_t run = then - c->lastCollectionTime;
    c->totalCollectionTime += collection;
    c->totalTime += collection + run;
    c->lastCollectionTime = now;

    fprintf(stderr,
            " - collect: %4"LLD"ms; "
            "total: %4"LLD"ms; "
            "run: %4"LLD"ms; "
            "total: %4"LLD"ms\n",
            collection,
            c->totalCollectionTime,
            run,
            c->totalTime - c->totalCollectionTime);
  }
}

class MyHeap: public Heap {
 public:
  MyHeap(System* system): c(system) { }

  virtual void collect(CollectionType type, Client* client, unsigned footprint)
  {
    c.mode = type;
    c.client = client;

    ::collect(&c, footprint);
  }

  virtual bool needsMark(void** p) {
    return *p and c.gen2.contains(p) and not c.gen2.contains(*p);
  }

  virtual void mark(void** p) {
    if (Debug) {        
      fprintf(stderr, "mark %p (%s) at %p (%s)\n",
              *p, segment(&c, *p), p, segment(&c, p));
    }

    c.heapMap.set(p);
  }

  virtual void pad(void* p, unsigned extra) {
    if (c.gen1.contains(p)) {
      if (c.ageMap.get(p) == TenureThreshold) {
        c.gen2padding += extra;
      } else {
        c.gen1padding += extra;
      }
    } else if (c.gen2.contains(p)) {
      c.gen2padding += extra;
    } else {
      c.gen1padding += extra;
    }
  }

  virtual void* follow(void* p) {
    if (wasCollected(&c, p)) {
      if (Debug) {
        fprintf(stderr, "follow %p (%s) to %p (%s)\n",
                p, segment(&c, p),
                ::follow(&c, p), segment(&c, ::follow(&c, p)));
      }

      return ::follow(&c, p);
    } else {
      return p;
    }
  }

  virtual Status status(void* p) {
    p = mask(p);

    if (p == 0) {
      return Null;
    } else if (c.nextGen1.contains(p)) {
      return Reachable;
    } else if (c.nextGen2.contains(p)
               or (c.gen2.contains(p)
                   and (c.mode == Heap::MinorCollection
                        or c.gen2.indexOf(p) >= c.gen2Base)))
    {
      return Tenured;
    } else if (wasCollected(&c, p)) {
      return Reachable;
    } else {
      return Unreachable;
    }
  }

  virtual CollectionType collectionType() {
    return c.mode;
  }

  virtual void dispose() {
    c.dispose();
    c.system->free(this);
  }

  Context c;
};

} // namespace

namespace vm {

Heap*
makeHeap(System* system)
{  
  return new (system->allocate(sizeof(MyHeap))) MyHeap(system);
}

} // namespace vm
