# Shared Memory Objects

[![Build Status](https://travis-ci.org/allenluce/mmap-object.svg?branch=master)](https://travis-ci.org/allenluce/mmap-object)

Super-fast file-based sharing of Javascript objects among multiple
processes.

This module maps Javascript objects into shared memory for
simultaneous access by different Node processes running on the same
machine. Shared memory is loaded
via [mmap](https://en.wikipedia.org/wiki/Mmap).  Object access is
mediated by Boost's unordered map class so object property access are
speedy.

Data is lazily loaded piece-by-piece as needed so opening even a huge
file takes no time at all.

There are two modes:

## Unshared Write-only Mode

A single process creates a new file which is mapped to a Javascript
object. Setting properties on this object writes those properties to
the file. You *can* read from the object within this mode but sharing
an object in write-only mode with other processes is certain to result
in crashes.

## Shared Read-only mode

Open an existing file for reading. Multiple processes can safely open
this file. Opening is lightning fast and only a single copy remains in
memory.

## Requirements

Binaries are provided for OSX and Linux for various node versions
(check the releases page to see which). If a binary is not provided
for your platform, you will need Boost and and a C++11 compliant
compiler (like GCC 4.8 or better) to build the module.

## Installation

    npm install mmap-object

## Usage

```javascript
// Write a file
const Shared = require('mmap-object')

const shared_object = new Shared.Create('filename')

shared_object['new_key'] = 'some value'
shared_object.new_property = 'some other value'

// Erase a key
delete shared_object['new_key']

shared_object.close()

// Read a file
const read_only_shared_object = new Shared.Open('filename')

console.log(`My value is ${read_only_shared_object.new_key}`)
```

## API

### new Create(path, [file_size], [initial_bucket_count], [max_file_size])

Creates a new file mapped into shared memory. Returns an object that
provides access to the shared memory. Throws an exception on error.

__Arguments__

* `path` - The path of the file to create
* `file_size` - *Optional* The initial size of the file in
  kilobytes. If more space is needed, the file will automatically be
  grown to a larger size. Minimum is 500 bytes. Defaults to 5
  megabytes.
* `initial_bucket_count` - *Optional* The number of buckets to
  allocate initially. This is passed to the underlying
  [Boost unordered_map](http://www.boost.org/doc/libs/1_38_0/doc/html/boost/unordered_map.html).
  Defaults to 1024. Set this to the number of keys you expect to write.
* `max_file_size` - *Optional* The largest the file is allowed to grow
  in kilobites. If data is added beyond this limit, an exception is
  thrown.  Defaults to 5 gigabytes.

__Example__

```js
// Create a 500K map for 300 objects.
const obj = new Shared.Create('/tmp/sharedmem', 500, 300)
```

### new Open(path)

Maps an existing file into shared memory. Returns an object that
provides read-only access to the object contained in the file. Throws
an exception on error. Any number of processes can open the same file
but only a single copy will reside in memory. Uses `mmap` under the
covers, so only those parts of the file that are actually accessed
will be loaded.

__Arguments__

* `path` - The path of the file to open

__Example__

```js
// Open up that shared file
const obj = new Shared.Open('/tmp/sharedmem')
```

### close()

Unmaps a previously created or opened file. If the file was most
recently opened with `Create()`, `close()` will first shrink the file
to remove any unneeded space that may have been allocated.

It's important to close your unused shared files in long-running
processes. Not doing so keeps shared memory from being freed.

The closing of very large objects (a few gigabytes and up) may take
some time (hundreds to thousands of milliseconds). To prevent blocking
the main thread, pass a callback to `close()`. The call to `close()`
will return immediately while the callback will be called after the
underlying `munmap()` operation completes. Any error will be given as
the first argument to the callback.

__Example__

```js
obj.close(function (err) {
  if (err) {
    console.error(`Error closing object: ${err}`)
  }
})
```

### isData()

When iterating, use `isData()` to tell if a particular key is real
data or one of the underlying methods on the shared object:

```js
const obj = new Shared.Open('/tmp/sharedmem')

for (let key in obj) {
  if (obj.isData(key)) { // Only show actual data
      console.log(key + ': ' + obj[key])
  }
}
```


### isOpen()

Return true if this object is currently open.

### isClosed()

Return true if this object has been closed.

### get_free_memory()

Number of bytes of free storage left in the shared object file.

### get_size()

The size of the storage in the shared object file, in bytes.

### bucket_count()

The number of buckets currently allocated in the underlying hash structure.

### max_bucket_count()

The maximum number of buckets that can be allocated in the underlying hash structure.

### load_factor()

The average number of elements per bucket.

### max_load_factor()

The current maximum load factor.

## Unit tests

    npm test

## Limitations

_It is strongly recommended_ to pass in the number of keys you expect
to write when creating the object with `Create`. If you don't do this,
the object will resize as you fill it up. This can be a very
time-consuming process and can result in fragmentation within the
shared memory object and a larger final file size.

Object values may be only string or number values. Attempting to set
a different type value results in an exception.

Symbols are not supported as properties.

## Publishing a binary release

To make a new binary release:

- Edit package.json. Increment the `version` property.
- `node-pre-gyp rebuild`
- `node-pre-gyp package`
- `node-pre-gyp-github publish`
- `npm publish`

You will need a `NODE_PRE_GYP_GITHUB_TOKEN` with `repo:status`,
`repo_deployment` and `public_repo` access to the target repo. You'll
also need write access to the npm repo.

## MSVS build prerequisites

Set up [Boost](http://www.boost.org/).

Set BOOST_ROOT environment variable.

```
bootstrap
b2 --build-type=complete
```
