# Third-Party Notices

OpenHaldex-C6-Edge ("this fork") is a personal, non-commercial firmware fork of
[Forbes-Automotive/OpenHaldex-C6](https://github.com/Forbes-Automotive/OpenHaldex-C6).

**This fork is distributed under the Forbes Automotive Source-Available License
(FASL) v1.0** - see [`LICENSE.md`](LICENSE.md). The FASL is preserved unchanged from
upstream and governs redistribution and use of this fork as a whole.

The codebase this fork descends from incorporates and derives from third-party work
that was licensed separately. Those upstream portions retain their original licenses.
Where a conflict exists between the FASL and an upstream license, **the upstream
license governs the upstream portions it applies to.**

This file records the upstream projects, the components derived from them, and the
license/permission basis relied upon. It exists so that the attribution obligations
carried by those upstream portions travel with this fork.

---

## 1. Original OpenHaldex (Generation 1)

- **Author / Upstream:** ABangingDonk
- **Project:** OpenHaldex / OpenHaldexT4 - https://github.com/ABangingDonk/OpenHaldexT4
- **Relationship:** The OpenHaldex lineage started from ABangingDonk's original
  OpenHaldex codebase for Generation 1 Haldex control. Gen1 control concepts and
  code descend from this work.

---

## 2. OpenHaldex-S3 (SpringfieldVW / Chris "meatro")

- **Author / Upstream:** Chris (GitHub: meatro)
- **Project:** OpenHaldex-S3 - https://github.com/meatro/OpenHaldex-S3
- **License:** MIT License (see full text below)
- **Relationship:** The CAN analysis / GVRET / SavvyCAN-style tooling (the analyzer
  mode - a passive CAN bridge exposing a GVRET-over-TCP interface for SavvyCAN)
  derives from work in OpenHaldex-S3, which entered the C6 lineage via the
  "S3 port onto C6" import upstream. This fork carries that analyzer path
  (`src/OpenHaldexC6_Analyzer.cpp`).

> MIT requires that the copyright notice and permission notice below be retained in
> all copies or substantial portions of the derived code. These portions remain
> available under MIT.

### MIT License (OpenHaldex-S3)

```
MIT License

Copyright (c) OpenHaldex-S3 contributors (SpringfieldVW / Chris "meatro")

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
```

---

## 3. Forbes Automotive Original Work

Portions original to Forbes Automotive - including the Gen2 / Gen4 / Gen5 reverse
engineering and implementation, the MQB UDS live-data readout, the low-power sleep
system, and other Forbes-authored code - form the base this fork builds on.

Upstream distributes those portions under the terms stated in the upstream project.
Redistribution of **this fork** as a whole is governed by the FASL v1.0
([`LICENSE.md`](LICENSE.md)); the Forbes Automotive copyright is preserved unchanged.
This fork is unofficial and is not affiliated with or endorsed by Forbes Automotive.
For official, supported firmware and hardware, see the upstream project and
[forbes-automotive.com](https://forbes-automotive.com/).

---

## Summary

- **This fork:** source-available under FASL v1.0, personal and non-commercial.
- **Upstream lineage attributions:** the ABangingDonk (Gen1) and meatro/OpenHaldex-S3
  (analyzer / GVRET / SavvyCAN tooling) portions retain their original licenses; the
  MIT permission notice for the S3-derived portions is preserved above as MIT requires.
- **No warranty.** See the disclaimer in [`README.md`](README.md) and the warranty
  terms in [`LICENSE.md`](LICENSE.md).
