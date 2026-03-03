<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/docs/images/logo_light_full.svg">
  <source media="(prefers-color-scheme: light)" srcset="docs/docs/images/logo_dark_full.svg">
  <img alt="RayforceDB Cover" src="docs/docs/images/logo_dark_full.svg">
</picture>

<p>&nbsp;</p>

[![License](https://img.shields.io/badge/License-MIT-yellow?style=flat)](LICENSE)
[![Language](https://img.shields.io/badge/Language-C-blue?logo=c&style=flat)](https://en.wikipedia.org/wiki/C_(programming_language))
[![Tests](https://img.shields.io/badge/Tests-passing-success?logo=github&style=flat)](https://RayforceDB.github.io/rayforce/tests_report/)
[![Release](https://img.shields.io/badge/Release-latest-blue?logo=github&style=flat)](https://github.com/RayforceDB/rayforce/releases)
[![Documentation](https://img.shields.io/badge/Documentation-latest-blue?logo=github&style=flat)](https://RayforceDB.github.io/rayforce/)
[![Linux](https://img.shields.io/badge/Linux-supported-green?logo=linux&logoColor=white&style=flat)]()
[![macOS](https://img.shields.io/badge/macOS-supported-green?logo=apple&style=flat)]()
[![Windows](https://img.shields.io/badge/Windows-supported-green?logo=microsoft&style=flat)]()
[![WASM](https://img.shields.io/badge/WASM-supported-green?logo=webassembly&style=flat)]()

A high-performance columnar vector database written in pure C. RayforceDB combines the power of columnar storage with SIMD vectorization to deliver fast analytics on time-series and big data workloads.

## Features

- **Columnar storage** with vectorized operations for analytical workloads
- **Minimal footprint**: <1Mb binary, zero dependencies
- **Cross-platform**: Linux, macOS, Windows, WebAssembly
- **Simple query language**: Lisp-like Rayfall syntax, no complex SQL
- **Custom memory management**: Parallel lockfree buddy allocator optimized for analytical workloads

## Quick Start

```bash
git clone https://github.com/RayforceDB/rayforce.git
cd rayforce
make release
./rayforce
```

## Use Cases

- Financial analytics and high-frequency trading data
- IoT sensor data and time-series monitoring
- Real-time analytics and streaming data
- Embedded systems and edge computing
- Data science and exploratory analysis
- LLMs and semantic retrieval

## Building

```bash
make debug      # Debug build with sanitizers
make release    # Optimized production build
make tests      # Run test suite
make bench      # Run benchmark suite
```

## Documentation

- [Full Documentation](https://RayforceDB.github.io/rayforce/)
- [Test Reports](https://RayforceDB.github.io/rayforce/tests_report/)

## Python bindings

Rayforce has powerful [Python bindings](https://github.com/RayforceDB/rayforce-py) (contributions are welcome)

## Contributing

Contributions are welcome! You can help by:

- Reporting bugs and requesting features via [GitHub Issues](https://github.com/RayforceDB/rayforce/issues)
- Submitting pull requests
- Creating example scripts and use cases
- Improving documentation

## Development Partnership

RayforceDB is jointly developed with and sponsored by **[Lynx](https://www.lynxtrading.com/)**.

This partnership has been instrumental in making RayforceDB a mature, production-ready database system. Lynx Capital's active involvement in development and their commitment to innovative open-source technologies in the financial sector has enabled RayforceDB to reach its full potential.
