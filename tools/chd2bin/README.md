## Build

Linux, against libchdr (https://github.com/rtissera/libchdr, built static):

```sh
cmake -S libchdr -B libchdr/build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DINSTALL_STATIC_LIBS=ON && cmake --build libchdr/build
cc -O2 -Ilibchdr/include -o chd2bin chd2bin.c libchdr/build/libchdr-static.a libchdr/build/deps/lzma-*/libchdr-lzma.a \
   libchdr/build/deps/zstd-*/libzstd.a libchdr/build/deps/miniz-*/libminiz.a -lm
```

Windows: the same with MinGW-w64.

## Use

`chd2bin in.chd out.bin` writes a CD CHD's tracks back as the original BIN (2352-byte sectors, subchannel and the
CHD's 4-frame track padding dropped) and prints each track. A one-track disc comes out byte-identical to its Redump
dump (Crash Team Racing USA, checked by SHA-1), which is how CTR High Octane's `ctr-u.bin` was made from the package's CHD.
Tracks with pregaps not stored in the CHD are refused.
