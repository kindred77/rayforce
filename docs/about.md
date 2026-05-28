---
title: "About — Rayforce"
description: "About Rayforce — an embeddable analytics and graph engine in pure C17 by Anton Kundenko. Project background, design principles, license, and community."
template: about.html
hide:
  - navigation
  - toc
  - footer
---

# About Rayforce

Rayforce is an embeddable analytics and graph engine written in pure C17 with zero external dependencies. It was built to close the gap between systems that excel at columnar analytics and systems that excel at graph traversal — by treating both as operations on the same query DAG.

## Why it exists

The typical analytics stack splits work across two runtimes: a columnar engine for tabular aggregation and a separate library or service for graph traversal. Each has its own data model, query language, and memory layout. Joining them means serializing data between processes, paying that cost on every step, and accepting that the optimizer can never see the full pipeline. Rayforce was built on the bet that one engine — one optimizer, one execution model, one memory allocator — can do both faster than the seam between two.

## Design principles

- **Zero external dependencies.** The whole engine is pure C17 with the standard library only. No third-party allocator, no Boost, no Arrow. The single header `rayforce.h` is the entire public API.
- **One DAG for everything.** Filters, joins, aggregations, window functions, and graph traversals all build into the same lazy operation DAG. The optimizer rewrites the full graph before execution begins.
- **Morsel-driven execution.** Element-wise operations fuse into single-pass pipelines that operate on 1024-element morsels and stay L1-resident. Pipeline breakers — joins, sort, group-by, window — materialize once and hand off cleanly.
- **Custom memory model.** A buddy allocator with slab caches replaces malloc; per-thread heaps remove allocator contention; deferred cross-heap merge handles worker-thread results without locks on the hot path.
- **Embeddable first.** Rayforce is a library you link, not a server you deploy. The C API is small enough to wrap from any language with an FFI.

## The author

Rayforce is built by **[Anton Kundenko](https://www.linkedin.com/in/anton-kundenko-a22a3667)**, a software engineer with a background in database internals, systems programming, and query optimization. The project is developed in the open under the MIT license — issues, pull requests, and design discussion are welcome on [GitHub](https://github.com/RayforceDB/rayforce) and the project's [Zulip community](https://rayforcedb.zulipchat.com/).

## License

Rayforce is open source under the [MIT License](https://github.com/RayforceDB/rayforce/blob/master/LICENSE). You may use, modify, and distribute it freely, including in commercial products. Source and releases are on [GitHub](https://github.com/RayforceDB/rayforce).
