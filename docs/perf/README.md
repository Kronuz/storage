# storage benchmarks

`examples/bench.cc` (`storage_bench`) measures write and read throughput and the
compression ratio per codec.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/storage_bench
```

`storage_bench` writes and reads 50,000 records of 256 bytes and 200 records of 1 MiB through one volume per codec, one thread, with a warm page cache. A raw row follows each of the none, LZ4 and zstd scenarios: the same records through liblz4 or libzstd and `pwrite`/`pread` alone, in one file with no headers, footers or checksums, one LZ4 block or zstd frame per record. It is the floor the engine's framing is measured against. The table gives MB/s of uncompressed data (write / read), the median of three runs, with Release builds and Apple clang 17; the ratio is record bytes over file bytes.

| Scenario | Apple M4 Pro | Intel i9-9980HK | ratio |
|---|---|---|---|
| small blobs, none | 191 / 190 | 113 / 71 | 0.75 |
| raw `pwrite`/`pread` | 149 / 862 | 74 / 282 | 1.00 |
| small blobs, lz4 | 141 / 173 | 81 / 69 | 0.98 |
| raw lz4 | 124 / 781 | 65 / 295 | 1.38 |
| small blobs, zstd | 59 / 55 | 38 / 46 | 1.23 |
| raw zstd | 53 / 129 | 30 / 94 | 1.70 |
| small blobs, deflate | 38 / 90 | 22 / 46 | 1.29 |
| large blobs, none | 346 / 4,876 | 321 / 1,029 | 0.77 |
| raw `pwrite`/`pread` | 501 / 12,809 | 1,584 / 5,407 | 1.00 |
| large blobs, lz4 | 427 / 1,949 | 321 / 658 | 1.84 |
| raw lz4 | 552 / 3,177 | 706 / 2,148 | 2.42 |
| large blobs, zstd | 454 / 1,262 | 280 / 557 | 3.81 |
| raw zstd | 513 / 1,561 | 348 / 1,145 | 4.95 |
| large blobs, deflate | 36 / 1,750 | 21 / 465 | 4.55 |

The engine reads slower than the raw floor in every row, by 1.2x (large zstd on the M4 Pro) to 5.3x (large uncompressed on the i9); small records read 2.0x to 4.5x slower. A single write row varies by up to 3x between runs on these hosts.
