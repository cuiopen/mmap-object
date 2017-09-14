#if defined(__linux__)
  #include <features.h>
  #if defined(__GLIBC_PREREQ)
    #if __GLIBC_PREREQ(2, 13)
      __asm__(".symver clock_gettime,clock_gettime@GLIBC_2.2.5");
      __asm__(".symver memcpy,memcpy@GLIBC_2.2.5");
    #endif
  #endif
#endif
#include <stdbool.h>
#include <assert.h>
#include <boost/interprocess/managed_mapped_file.hpp>
#include <boost/interprocess/managed_shared_memory.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/sync/interprocess_upgradable_mutex.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>
#include <boost/interprocess/sync/sharable_lock.hpp>
#include <boost/interprocess/sync/upgradable_lock.hpp>
#include <boost/interprocess/containers/string.hpp>
#include <boost/scope_exit.hpp>
#include <boost/thread/thread_time.hpp>
#include <boost/unordered_map.hpp>
#include <boost/version.hpp>
#include <nan.h>

#define LOCKINFO(lock) // cout << ::getpid() << " LOCK " << lock << endl

#if BOOST_VERSION < 105500
#pragma message("Found boost version " BOOST_PP_STRINGIZE(BOOST_LIB_VERSION))
#error mmap-object needs at least version 1_55 to maintain compatibility.
#endif

#define MINIMUM_FILE_SIZE 500 // Minimum necessary to handle an mmap'd unordered_map on all platforms.
#define DEFAULT_FILE_SIZE 5ul<<20 // 5 megs
#define DEFAULT_MAX_SIZE 5000ul<<20 // 5000 megs

// For Win32 compatibility
#ifndef S_ISDIR
#define S_ISDIR(mode)  (((mode) & S_IFMT) == S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(mode)  (((mode) & S_IFMT) == S_IFREG)
#endif

namespace bip=boost::interprocess;
using namespace std;

typedef bip::managed_shared_memory::segment_manager segment_manager_t;

template <typename StorageType> using SharedAllocator =
  bip::allocator<StorageType, segment_manager_t>;

typedef SharedAllocator<char> char_allocator;

typedef bip::basic_string<char, char_traits<char>, char_allocator> shared_string;
typedef bip::basic_string<char, char_traits<char>> char_string;

#define UNINITIALIZED 0
#define STRING_TYPE 1
#define NUMBER_TYPE 2
class WrongPropertyType: public exception {};
class FileTooLarge: public exception {};

class Cell {
private:
  char cell_type;
  union values {
    shared_string string_value;
    double number_value;
    values(const char *value, char_allocator allocator): string_value(value, allocator) {}
    values(const double value): number_value(value) {}
    values() {}
    ~values() {}
  } cell_value;
  Cell& operator =(const Cell&) = default;
  Cell(Cell&&) = default;
  Cell& operator=(Cell&&) & = default;
public:
  Cell(const char *value, char_allocator allocator) : cell_type(STRING_TYPE), cell_value(value, allocator) {}
  Cell(const double value) : cell_type(NUMBER_TYPE), cell_value(value) {}
  Cell(const Cell &cell);
  char type() { return cell_type; }
  const char *c_str();
  operator string();
  operator double();
};

typedef shared_string KeyType;
typedef Cell ValueType;

typedef SharedAllocator<pair<KeyType, ValueType>> map_allocator;

struct s_equal_to {
  bool operator()( const char_string& lhs, const shared_string& rhs ) const {
    return string(lhs.c_str()) == string(rhs.c_str());
  }
  bool operator()( const shared_string& lhs, const shared_string& rhs ) const {
    return string(lhs.c_str()) == string(rhs.c_str());
  }
};

class hasher {
public:
  size_t operator() (shared_string const& key) const {
    return boost::hash<shared_string>()(key);
  }
  size_t operator() (char_string const& key) const {
    return boost::hash<char_string>()(key);
  }
};

typedef boost::unordered_map<
  KeyType,
  ValueType,
  hasher,
  s_equal_to,
  map_allocator> PropertyHash;

#define MUTEX_NAME "MMAP_OBJECT_SHARED_MUTEX"
typedef bip::interprocess_upgradable_mutex upgradable_mutex_type;

class SharedMap : public Nan::ObjectWrap {
public:
  SharedMap() : closed(false), inWriteLock(false) {}
  static NAN_MODULE_INIT(Init);

private:
  string file_name;
  size_t file_size;
  size_t max_file_size;
  bip::managed_mapped_file *map_seg;
  PropertyHash *property_map;
  bool readonly;
  bool closed;
  
  upgradable_mutex_type *mutex;
  bool inWriteLock;
  bip::mapped_region mutex_region;

  void grow(size_t);
  void grow_private(size_t);
  void setFilename(string);
  void reify_mutex();
  static NAN_METHOD(Create);
  static NAN_METHOD(Open);
  static NAN_METHOD(Close);
  static NAN_METHOD(isClosed);
  static NAN_METHOD(isOpen);
  static NAN_METHOD(isData);
  static NAN_METHOD(writeLock);
  static NAN_METHOD(writeUnlock);
  static NAN_METHOD(get_free_memory);
  static NAN_METHOD(get_size);
  static NAN_METHOD(bucket_count);
  static NAN_METHOD(max_bucket_count);
  static NAN_METHOD(load_factor);
  static NAN_METHOD(max_load_factor);
  static NAN_METHOD(inspect);
  static NAN_METHOD(remove_shared_mutex);
  static NAN_PROPERTY_SETTER(PropSetter);
  static NAN_PROPERTY_GETTER(PropGetter);
  static NAN_PROPERTY_QUERY(PropQuery);
  static NAN_PROPERTY_ENUMERATOR(PropEnumerator);
  static NAN_PROPERTY_DELETER(PropDeleter);
  static NAN_INDEX_GETTER(IndexGetter);
  static NAN_INDEX_SETTER(IndexSetter);
  static NAN_INDEX_QUERY(IndexQuery);
  static NAN_INDEX_DELETER(IndexDeleter);
  static NAN_INDEX_ENUMERATOR(IndexEnumerator);

  static v8::Local<v8::Function> init_methods(v8::Local<v8::FunctionTemplate> f_tpl);
  static inline Nan::Persistent<v8::Function> & constructor() {
    static Nan::Persistent<v8::Function> my_constructor;
    return my_constructor;
  }
};


bool isMethod(string name) {
  string methods[] = {
    "isClosed",
    "isOpen",
    "close",
    "valueOf",
    "toString",
    "close",
    "get_free_memory",
    "get_size",
    "bucket_count",
    "max_bucket_count",
    "load_factor",
    "max_load_factor",
    "isData",
    "remove_shared_mutex"
  };
  set<string> method_set(methods, methods + sizeof(methods) / sizeof(methods[0]));

  return method_set.find(name) != method_set.end();
}

const char *Cell::c_str() {
  if (type() != STRING_TYPE)
    throw WrongPropertyType();
 return cell_value.string_value.c_str();
}

Cell::operator string() {
  if (type() != STRING_TYPE)
    throw WrongPropertyType();
  return cell_value.string_value.c_str();
}

Cell::operator double() {
  if (type() != NUMBER_TYPE)
    throw WrongPropertyType();
  return cell_value.number_value;
}

Cell::Cell(const Cell &cell) {
  cell_type = cell.cell_type;
  if (cell.cell_type == STRING_TYPE) {
    new (&cell_value.string_value)(shared_string)(cell.cell_value.string_value, cell.cell_value.string_value.get_allocator());
  } else { // is a number type
    cell_value.number_value = cell.cell_value.number_value;
  }
}

NAN_PROPERTY_SETTER(SharedMap::PropSetter) {
  auto self = Nan::ObjectWrap::Unwrap<SharedMap>(info.This());
  if (self->readonly) {
    Nan::ThrowError("Read-only object.");
    return;
  }

  if (self->closed) {
    Nan::ThrowError("Cannot write to closed object.");
    return;
  }

  if (property->IsSymbol()) {
    Nan::ThrowError("Symbol properties are not supported.");
    return;
  }

  size_t data_length = sizeof(Cell);
  bip::scoped_lock<upgradable_mutex_type> lock;
  
  if (!self->inWriteLock) {
    LOCKINFO("1");
    bip::scoped_lock<upgradable_mutex_type> lock_(*self->mutex);
    lock.swap(lock_);
    LOCKINFO("1 SUCCESS");
  }

  try {
    Cell *c;
    while(true) {
      try {
        if (value->IsString()) {
          v8::String::Utf8Value data(value);
          data_length += data.length();
          char_allocator allocer(self->map_seg->get_segment_manager());
          c = new Cell(string(*data).c_str(), allocer);
        } else if (value->IsNumber()) {
          data_length += sizeof(double);
          c = new Cell(Nan::To<double>(value).FromJust());
        } else {
          Nan::ThrowError("Value must be a string or number.");
          return;
        }

        v8::String::Utf8Value prop(property);
        data_length += prop.length();
        shared_string *string_key;
        char_allocator allocer(self->map_seg->get_segment_manager());
        string_key = new shared_string(string(*prop).c_str(), allocer);
        auto pair = self->property_map->insert({ *string_key, *c });
        if (!pair.second) {
          self->property_map->erase(*string_key);
          self->property_map->insert({ *string_key, *c });
        }
        break;
      } catch(length_error) {
        self->grow_private(data_length * 2);
      } catch(bip::bad_alloc) {
        self->grow_private(data_length * 2);
      }
    }
  } catch(FileTooLarge) {
    Nan::ThrowError("File grew too large.");
  }
  info.GetReturnValue().Set(value);
}

#define STRINGINDEX                                             \
  std::ostringstream ss;                                        \
  ss << index;                                                  \
  auto prop = Nan::New<v8::String>(ss.str()).ToLocalChecked()

NAN_INDEX_GETTER(SharedMap::IndexGetter) {
  STRINGINDEX;
  SharedMap::PropGetter(prop, info);
}

NAN_INDEX_SETTER(SharedMap::IndexSetter) {
  STRINGINDEX;
  SharedMap::PropSetter(prop, value, info);
}

NAN_INDEX_QUERY(SharedMap::IndexQuery) {
  STRINGINDEX;
  SharedMap::PropQuery(prop, info);
}

NAN_INDEX_DELETER(SharedMap::IndexDeleter) {
  STRINGINDEX;
  SharedMap::PropDeleter(prop, info);
}

NAN_INDEX_ENUMERATOR(SharedMap::IndexEnumerator) {
  info.GetReturnValue().Set(Nan::New<v8::Array>(v8::None));
}

NAN_METHOD(SharedMap::inspect) {
  info.GetReturnValue().Set(v8::None);
}

NAN_PROPERTY_GETTER(SharedMap::PropGetter) {
  v8::String::Utf8Value data(info.Data());
  v8::String::Utf8Value src(property);
  if (property->IsSymbol() || string(*data) == "prototype") {
    return;
  }

  if (string(*src) == "inspect") {
    v8::Local<v8::FunctionTemplate> tmpl = Nan::New<v8::FunctionTemplate>(inspect);
    v8::Local<v8::Function> fn = Nan::GetFunction(tmpl).ToLocalChecked();
    fn->SetName(Nan::New("inspect").ToLocalChecked());
    info.GetReturnValue().Set(fn);
    return;
  }

  if (!property->IsNull() && isMethod(string(*src))) {
    return;
  }

  auto self = Nan::ObjectWrap::Unwrap<SharedMap>(info.This());
  if (self->closed) {
    Nan::ThrowError("Cannot read from closed object.");
    return;
  }
  LOCKINFO("SHARE 1");
  bip::sharable_lock<upgradable_mutex_type> lock(*self->mutex);
  LOCKINFO("SHARE 1 SUCCESS");

  // If the map doesn't have it, let v8 continue the search.
  auto pair = self->property_map->find<char_string, hasher, s_equal_to>
    (*src, hasher(), s_equal_to());

  if (pair == self->property_map->end())
    return;
  Cell *c = &pair->second;
  if (c->type() == STRING_TYPE) {
    info.GetReturnValue().Set(Nan::New<v8::String>(c->c_str()).ToLocalChecked());
  } else if (c->type() == NUMBER_TYPE) {
    info.GetReturnValue().Set((double)*c);
  }
}

NAN_PROPERTY_QUERY(SharedMap::PropQuery) {
  v8::String::Utf8Value src(property);

  if (isMethod(string(*src))) {
    info.GetReturnValue().Set(Nan::New<v8::Integer>(v8::ReadOnly | v8::DontEnum | v8::DontDelete));
    return;
  }
  auto self = Nan::ObjectWrap::Unwrap<SharedMap>(info.This());

  if (self->readonly) {
    info.GetReturnValue().Set(Nan::New<v8::Integer>(v8::ReadOnly | v8::DontDelete));
    return;
  }
    
  info.GetReturnValue().Set(Nan::New<v8::Integer>(v8::None));
}

NAN_PROPERTY_DELETER(SharedMap::PropDeleter) {
  if (property->IsSymbol()) {
    Nan::ThrowError("Symbol properties are not supported for delete.");
    return;
  }
  
  v8::String::Utf8Value src(property);

  if (isMethod(string(*src))) {
    info.GetReturnValue().Set(Nan::New<v8::Boolean>(v8::None));
    return;
  }
  
  auto self = Nan::ObjectWrap::Unwrap<SharedMap>(info.This());

  if (self->readonly) {
    Nan::ThrowError("Cannot delete from read-only object.");
    return;
  }

  if (self->closed) {
    Nan::ThrowError("Cannot delete from closed object.");
    return;
  }
  LOCKINFO("2");
  bip::scoped_lock<upgradable_mutex_type> lock(*self->mutex);
  LOCKINFO("2 SUCCESS");
  v8::String::Utf8Value prop(property);
  shared_string *string_key;
  char_allocator allocer(self->map_seg->get_segment_manager());
  string_key = new shared_string(string(*prop).c_str(), allocer);
  self->property_map->erase(*string_key);
}

NAN_PROPERTY_ENUMERATOR(SharedMap::PropEnumerator) {
  v8::Local<v8::Array> arr = Nan::New<v8::Array>();
  auto self = Nan::ObjectWrap::Unwrap<SharedMap>(info.This());

  if (self->closed) {
    info.GetReturnValue().Set(Nan::New<v8::Array>(v8::None));
    return;
  }

  int i = 0;
  bip::sharable_lock<upgradable_mutex_type> lock(*self->mutex);
  for (auto it = self->property_map->begin(); it != self->property_map->end(); ++it) {
	  arr->Set(i++, Nan::New<v8::String>(it->first.c_str()).ToLocalChecked());
  }
  info.GetReturnValue().Set(arr);
}

#define INFO_METHOD(name, type, object) NAN_METHOD(SharedMap::name) { \
  auto self = Nan::ObjectWrap::Unwrap<SharedMap>(info.This()); \
  bip::sharable_lock<upgradable_mutex_type> lock(*self->mutex); \
  info.GetReturnValue().Set((type)self->object->name()); \
}

INFO_METHOD(get_free_memory, uint32_t, map_seg)
INFO_METHOD(get_size, uint32_t, map_seg)
INFO_METHOD(bucket_count, uint32_t, property_map)
INFO_METHOD(max_bucket_count, uint32_t, property_map)
INFO_METHOD(load_factor, float, property_map)
INFO_METHOD(max_load_factor, float, property_map)

NAN_METHOD(SharedMap::remove_shared_mutex) {
  bip::shared_memory_object::remove(MUTEX_NAME);
}

void SharedMap::reify_mutex() {
  // Find or create the mutex.
  bip::shared_memory_object shm;
  try {
    shm = bip::shared_memory_object(bip::open_only, MUTEX_NAME, bip::read_write);
    mutex_region = bip::mapped_region(shm, bip::read_write);
  } catch(bip::interprocess_exception &ex){
    if (ex.get_error_code() == 7) { // Need to create one
      bip::shared_memory_object::remove(MUTEX_NAME);
      shm = bip::shared_memory_object(bip::create_only, MUTEX_NAME, bip::read_write);
      shm.truncate(sizeof (upgradable_mutex_type));
      mutex_region = bip::mapped_region(shm, bip::read_write);
      new (mutex_region.get_address()) upgradable_mutex_type;
    } else {
      ostringstream error_stream;
      error_stream << "Can't open mutex: " << ex.what();
      Nan::ThrowError(error_stream.str().c_str());
      return;
    }      
  }

  mutex = static_cast<upgradable_mutex_type *>(mutex_region.get_address());
  
  // Trial lock
  try {
    LOCKINFO("3");
    bip::scoped_lock<upgradable_mutex_type> lock(*mutex, boost::get_system_time() + boost::posix_time::seconds(1));
    if (lock == 0) { // Didn't grab. May be messed up.
      new (mutex_region.get_address()) upgradable_mutex_type;
      LOCKINFO("MESSED UP");
    }      
    LOCKINFO("3 SUCCESS");
  } catch (bip::lock_exception &ex) {
    if (ex.get_error_code() == 15) { // Need to init the lock area
      new (mutex_region.get_address()) upgradable_mutex_type;
    } else {
      ostringstream error_stream;
      error_stream << "Bad shared mutex: " << ex.what();
      Nan::ThrowError(error_stream.str().c_str());
      return;
    }
  }  
}

NAN_METHOD(SharedMap::Create) {
  if (!info.IsConstructCall()) {
    Nan::ThrowError("Create must be called as a constructor.");
    return;
  }
  Nan::Utf8String filename(info[0]->ToString());
  size_t file_size = (int)info[1]->Int32Value();
  file_size *= 1024;
  size_t initial_bucket_count = (int)info[2]->Int32Value();
  size_t max_file_size = (int)info[3]->Int32Value();
  max_file_size *= 1024;
  SharedMap *d = new SharedMap();
  if (file_size == 0) {
    file_size = DEFAULT_FILE_SIZE;
  }
  // Don't open it too small.
  if (file_size < MINIMUM_FILE_SIZE) {
    file_size = 500;
    max_file_size = max(file_size, max_file_size);
  }
  if (max_file_size == 0) {
    max_file_size = DEFAULT_MAX_SIZE;
  }

  // Default to 1024 buckets
  if (initial_bucket_count == 0) {
    initial_bucket_count = 1024;
  }

  try {
    d->map_seg = new bip::managed_mapped_file(bip::open_or_create,string(*filename).c_str(), file_size);
    d->property_map = d->map_seg->find_or_construct<PropertyHash>("properties")
      (initial_bucket_count, hasher(), s_equal_to(), d->map_seg->get_segment_manager());
  } catch(bip::interprocess_exception &ex){
    ostringstream error_stream;
    error_stream << "Can't open file " << *filename << ": " << ex.what();
    Nan::ThrowError(error_stream.str().c_str());
    return;
  }

  d->reify_mutex();
  d->readonly = false;
  d->setFilename(*filename);
  d->file_size = file_size;
  d->max_file_size = max_file_size;
  d->Wrap(info.This());
  info.GetReturnValue().Set(info.This());
}

NAN_METHOD(SharedMap::Open) {
  if (!info.IsConstructCall()) {
    Nan::ThrowError("Open must be called as a constructor.");
    return;
  }

  Nan::Utf8String filename(info[0]->ToString());
  SharedMap *d = new SharedMap();

  struct stat buf;
  int s = stat(*filename, &buf);
  if (s == -1 || !S_ISREG(buf.st_mode) || buf.st_size == 0) {
    ostringstream error_stream;
    error_stream << *filename;
    if (s == -1) {
      error_stream << ": " << strerror(errno);
    } else if (!S_ISREG(buf.st_mode)) {
      error_stream << " is not a regular file.";
    } else {
      error_stream << " is an empty file.";
    }
    Nan::ThrowError(error_stream.str().c_str());
    return;
  }

  try {
    d->map_seg = new bip::managed_mapped_file(bip::open_read_only, string(*filename).c_str());
    if (d->map_seg->get_size() != (unsigned long)buf.st_size) {
      ostringstream error_stream;
      error_stream << "File " << *filename << " appears to be corrupt (1).";
      Nan::ThrowError(error_stream.str().c_str());
      return;
    }
    auto find_map = d->map_seg->find<PropertyHash>("properties");
    d->property_map = find_map.first;
    if (d->property_map == NULL) {
      ostringstream error_stream;
      error_stream << "File " << *filename << " appears to be corrupt (2).";
      Nan::ThrowError(error_stream.str().c_str());
      return;
    }
  } catch(bip::interprocess_exception &ex){
    ostringstream error_stream;
    error_stream << "Can't open file " << *filename << ": " << ex.what();
    Nan::ThrowError(error_stream.str().c_str());
    return;
  }
  d->reify_mutex();
  d->readonly = true;
  d->setFilename(*filename);
  d->Wrap(info.This());
  info.GetReturnValue().Set(info.This());
}

void SharedMap::setFilename(string fn_string) {
  file_name = fn_string;
}

void SharedMap::grow(size_t size) {
  LOCKINFO("4");
  bip::scoped_lock<upgradable_mutex_type> lock(*mutex);
  LOCKINFO("4 SUCCESS");
  grow_private(size);
}

void SharedMap::grow_private(size_t size) {
  file_size += size;
  if (file_size > max_file_size) {
    throw FileTooLarge();
  }
  map_seg->flush();
  delete map_seg;
  bip::managed_mapped_file::grow(file_name.c_str(), size);
  map_seg = new bip::managed_mapped_file(bip::open_only, file_name.c_str());
  property_map = map_seg->find<PropertyHash>("properties").first;
  closed = false;
}

struct CloseWorker : public Nan::AsyncWorker {
  bip::managed_mapped_file *map_seg;
  string file_name;
  upgradable_mutex_type *mutex;
  CloseWorker(Nan::Callback *callback, bip::managed_mapped_file *map_seg,
              string file_name, upgradable_mutex_type *mutex)
    : AsyncWorker(callback), map_seg(map_seg), file_name(file_name), mutex(mutex) {}
  virtual void Execute() { // May run in a separate thread
    bip::scoped_lock<upgradable_mutex_type> lock(*mutex);
    bip::managed_mapped_file::shrink_to_fit(file_name.c_str());
    map_seg->flush();
    delete map_seg;
  }
};

NAN_METHOD(SharedMap::Close) {
  auto self = Nan::ObjectWrap::Unwrap<SharedMap>(info.This());
  auto callback = new Nan::Callback(info[0].As<v8::Function>());
  auto closer = new CloseWorker(callback, self->map_seg, self->file_name, self->mutex);

  if (info[0]->IsFunction()) { // Close asynchronously
    if (self->closed) {
      v8::Local<v8::Value> argv[1] = {Nan::Error("Attempted to close a closed object.")};
      callback->Call(1, argv);
      return;
    }
    AsyncQueueWorker(closer);
  } else {
    if (self->closed) {
      Nan::ThrowError("Attempted to close a closed object.");
      return;
    }
    closer->Execute();
  }  
  self->map_seg = NULL;
  self->closed = true;
}

NAN_METHOD(SharedMap::isClosed) {
  auto self = Nan::ObjectWrap::Unwrap<SharedMap>(info.This());
  info.GetReturnValue().Set(self->closed);
}

NAN_METHOD(SharedMap::isOpen) {
  auto self = Nan::ObjectWrap::Unwrap<SharedMap>(info.This());
  info.GetReturnValue().Set(!self->closed);
}

NAN_METHOD(SharedMap::isData) {
  auto value = info[0];
  if (value->IsFunction()) {
    bool success = Nan::GetRealNamedProperty(value->ToObject(),
                                             Nan::New("name").ToLocalChecked()
                                             ).ToLocal(&value);
    if (!success) {
      value = info[0];
    }
  }
  bool result = true;
  if (value->IsString()) {
    result = !isMethod(*Nan::Utf8String(value->ToString()));
  }
  info.GetReturnValue().Set(result);
}

NAN_METHOD(SharedMap::writeUnlock) {
  auto self = Nan::ObjectWrap::Unwrap<SharedMap>(info.This());
  self->inWriteLock = false;
  self->mutex->unlock();
  LOCKINFO("5 SUCCESS");
}


NAN_METHOD(SharedMap::writeLock) {
  auto self = Nan::ObjectWrap::Unwrap<SharedMap>(info.This());
  auto callback = new Nan::Callback(info[0].As<v8::Function>());
  auto func = Nan::New<v8::Function>(writeUnlock);
  // Bind function to this object
  auto bind = Nan::Get(func, Nan::New("bind").ToLocalChecked()).ToLocalChecked();
  v8::Local<v8::Value> argvl[2] = {func, info.This()};
  auto boundFunc = Nan::Call(bind.As<v8::Function>(), func, 2, argvl).ToLocalChecked();
  // Send it as callback.
  v8::Local<v8::Value> argv[1] = {boundFunc};

  self->mutex->lock();
  self->inWriteLock = true;
  LOCKINFO("5");
  callback->Call(1, argv);
}

v8::Local<v8::Function> SharedMap::init_methods(v8::Local<v8::FunctionTemplate> f_tpl) {
  Nan::SetPrototypeMethod(f_tpl, "close", Close);
  Nan::SetPrototypeMethod(f_tpl, "isClosed", isClosed);
  Nan::SetPrototypeMethod(f_tpl, "isOpen", isOpen);
  Nan::SetPrototypeMethod(f_tpl, "isData", isData);
  Nan::SetPrototypeMethod(f_tpl, "writeLock", writeLock);
  Nan::SetPrototypeMethod(f_tpl, "remove_shared_mutex", remove_shared_mutex);
  Nan::SetPrototypeMethod(f_tpl, "get_free_memory", get_free_memory);
  Nan::SetPrototypeMethod(f_tpl, "get_size", get_size);
  Nan::SetPrototypeMethod(f_tpl, "bucket_count", bucket_count);
  Nan::SetPrototypeMethod(f_tpl, "max_bucket_count", max_bucket_count);
  Nan::SetPrototypeMethod(f_tpl, "load_factor", load_factor);
  Nan::SetPrototypeMethod(f_tpl, "max_load_factor", max_load_factor);

  auto proto = f_tpl->PrototypeTemplate();
  Nan::SetNamedPropertyHandler(proto, PropGetter, PropSetter, PropQuery, PropDeleter, PropEnumerator,
                               Nan::New<v8::String>("prototype").ToLocalChecked());

  auto inst = f_tpl->InstanceTemplate();
  inst->SetInternalFieldCount(1);
  Nan::SetNamedPropertyHandler(inst, PropGetter, PropSetter, PropQuery, PropDeleter, PropEnumerator,
                               Nan::New<v8::String>("instance").ToLocalChecked());
  Nan::SetIndexedPropertyHandler(inst, IndexGetter, IndexSetter, IndexQuery, IndexDeleter, IndexEnumerator,
                                 Nan::New<v8::String>("instance").ToLocalChecked());
  auto fun = Nan::GetFunction(f_tpl).ToLocalChecked();
  constructor().Reset(fun);
  return fun;
}

NAN_MODULE_INIT(SharedMap::Init) {
  // The mmap creator class
  v8::Local<v8::FunctionTemplate> create_tpl = Nan::New<v8::FunctionTemplate>(Create);
  create_tpl->SetClassName(Nan::New("CreateMmap").ToLocalChecked());
  auto create_fun = init_methods(create_tpl);
  Nan::Set(target, Nan::New("Create").ToLocalChecked(), create_fun);

  // The mmap opener class
  v8::Local<v8::FunctionTemplate> open_tpl = Nan::New<v8::FunctionTemplate>(Open);
  open_tpl->SetClassName(Nan::New("OpenMmap").ToLocalChecked());
  auto open_fun = init_methods(open_tpl);
  Nan::Set(target, Nan::New("Open").ToLocalChecked(), open_fun);
}

NODE_MODULE(mmap_object, SharedMap::Init)
