# Monocypher source

This directory contains the unmodified core and optional Ed25519/SHA-512
sources from [Monocypher 4.0.3](https://monocypher.org/download/). The source
archive is `https://monocypher.org/download/monocypher-4.0.3.tar.gz`; its
upstream-published SHA-512 is:

```
40904ada5c7ee4f7741733e38b69a30a4b0561cbffba5ffe7c2dce16136d540251ec0d9056ff606510d3b5b708fb8a40db7e0870d4a0b2dc17ba2bfb880f8965
```

`src/monocypher.h` is the core API. Include
`src/optional/monocypher-ed25519.h` for SHA-512 and Ed25519. The combined
grom/MagiskInit build compiles both source files into `libmonocypher` and
links the library to `magiskinit`. Unused functions may be removed by
link-time optimization. No cryptographic format or credential processing is
implemented by this dependency addition.

The upstream license is in [LICENCE.md](LICENCE.md): BSD-2-Clause OR CC0-1.0.
