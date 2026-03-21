# sa-tromp — Equihash 192,7 GPU Miner

A GPU miner for Zcash (and compatible coins) using the Equihash 192,7
proof-of-work algorithm. Written in OpenCL, targeting Intel integrated GPUs
via Beignet or Intel NEO drivers. Supports benchmark mode and Stratum pool mining.

## Quick start

```
make
./sa-tromp 1        # mine 1 nonce (test/benchmark)
./sa-tromp          # benchmark mode: mine continuously (default: 100000 nonces)
```

## Pool mining (Stratum)

```
./sa-tromp -s stratum+tcp://pool.example.com:3333 -u YOUR_ADDRESS -w WORKER
```

## Usage

```
./sa-tromp [options] [nonces]

Options:
  -p N       select OpenCL platform index (default: 0)
  -s URL     Stratum pool URL, e.g. stratum+tcp://pool.example.com:3333
  -u USER    pool username (usually your wallet address)
  -w WORKER  pool worker name (default: worker1)

Arguments:
  nonces     number of nonces to mine in benchmark mode (default: 100000)
```

## Performance

Tested on Intel UHD 630 (Coffee Lake iGPU, ~5.8 GB shared RAM):

| Driver  | Platform | sol/s |
|---------|----------|-------|
| Beignet | `-p 0`   | ~0.65 |
| NEO 21.40 | `-p 1` | ~0.46 |

Beignet is recommended on this hardware — it outperforms NEO 21.40 on the
blake2b Stage 0 kernel. Newer NEO versions (22.x+) may close the gap.

## OpenCL driver setup (Intel)

sa-tromp auto-detects Beignet vs NEO at startup and prints the driver name.

### Beignet (recommended)

```
sudo apt-get install beignet-dev
```

`beignet-dev` includes the compile-time headers and pulls in `beignet-opencl-icd`
(the runtime) as a dependency — one command is all you need.

Tested: Beignet 1.3 on Ubuntu 21.04.

### Intel NEO (intel-opencl-icd)

```
mkdir intelopencldebs && cd intelopencldebs

wget https://github.com/intel/intel-graphics-compiler/releases/download/igc-1.0.8744/intel-igc-core_1.0.8744_amd64.deb
wget https://github.com/intel/intel-graphics-compiler/releases/download/igc-1.0.8744/intel-igc-opencl_1.0.8744_amd64.deb
wget https://github.com/intel/compute-runtime/releases/download/21.40.21182/intel-opencl_21.40.21182_amd64.deb
wget https://github.com/intel/compute-runtime/releases/download/21.40.21182/intel-ocloc_21.40.21182_amd64.deb
wget https://github.com/intel/compute-runtime/releases/download/21.40.21182/intel-gmmlib_21.2.1_amd64.deb
wget https://github.com/intel/compute-runtime/releases/download/21.40.21182/intel-level-zero-gpu_1.2.21182_amd64.deb

sudo dpkg -i *.deb
sudo apt-get install -f
```

Select NEO explicitly with `-p 1`. Note: NEO 20.x fails to compile the kernel
due to an `i128` backend error — use NEO 21.40 or newer.

### Selecting a platform

Platform index order depends on your system. The driver name is printed at
startup so you can confirm which is active:

```
./sa-tromp -p 0    # check output: [opencl] <driver name>
./sa-tromp -p 1    # try the other platform
```

## System requirements

- **RAM**: At least 4 GB free. If hugepages are configured (e.g. for xmrig),
  they lock physical memory and will cause OOM when the GPU also allocates.
  Disable or reduce hugepages before running:
  ```
  sudo sysctl vm.nr_hugepages=0
  ```
- **User groups**: Your user must be in the `video` and `render` groups to
  access the GPU without root:
  ```
  sudo usermod -aG video,render $USER
  # then log out and back in
  ```

## Compilation

```
make
```

The resulting binary works with both Beignet and NEO at runtime.

To force a clean rebuild:
```
make clean && make
```

## Implementation

sa-tromp implements the Tromp bucket algorithm for Equihash 192,7:

- **Stage 0**: GPU generates 2^25 BLAKE2b hashes, distributed into 2^20 buckets
- **Stages 1–7**: GPU collision stages — each stage XORs colliding pairs and
  stores references back to parents (no full input lists stored)
- **Stage 7**: CPU extracts solution candidates and verifies them

The kernel is in [input.cl](input.cl). The host driver is [sa-tromp.c](sa-tromp.c).
Stratum client is in [stratum.c](stratum.c).

## License

The MIT License (MIT)
Copyright (c) 2016 Marc Bevand

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
