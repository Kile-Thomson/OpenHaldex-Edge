# Third-Party Notices

OpenHaldex Edge is a fork of
[Forbes-Automotive/OpenHaldex-C6](https://github.com/Forbes-Automotive/OpenHaldex-C6)
and is distributed under the **MIT License**, the same licence as upstream. See
[`LICENSE.md`](LICENSE.md).

- Portions original to Forbes Automotive are MIT, copyright Forbes Automotive.
- Edge's own additions are contributed under the same MIT terms, copyright
  Kile Thomson. Edge adds no extra conditions to the licence.
- Portions that came from other projects keep their original licences, listed
  below. Where one of those licences differs from the MIT License, the original
  licence governs the portions it applies to.

This file records the projects Edge's lineage derives from, the components taken
from each, and the licence or permission basis relied on. It exists so the
attribution obligations that come with those portions travel with every copy.

---

## 1. Original OpenHaldex (Generation 1)

- **Author / Upstream:** ABangingDonk
- **Project:** OpenHaldex / OpenHaldexT4 - https://github.com/ABangingDonk/OpenHaldexT4
- **License:** No license file is published in the upstream repository, so no
  explicit redistribution grant is on record for this work.
- **Relationship:** The OpenHaldex lineage started from ABangingDonk's original
  OpenHaldex codebase for Generation 1 Haldex control. Gen1 control concepts and
  code descend from this work.

> **Attribution note:** the ABangingDonk OpenHaldexT4 repository does not carry a
> LICENSE file, so the permission basis for redistributing Gen1-derived code is not
> formally documented upstream. Edge credits the original author and does not claim
> a grant that is not on record. Anyone redistributing Gen1-derived portions should
> confirm the permission basis with the original author; if a grant cannot be
> established, the affected portions should be treated accordingly.

---

## 2. OpenHaldex-S3 (SpringfieldVW / Chris "meatro")

- **Author / Upstream:** Chris (GitHub: meatro)
- **Project:** OpenHaldex-S3 - https://github.com/meatro/OpenHaldex-S3
- **License:** MIT License (full text below)
- **Relationship:** On 2026-02-08, upstream commit `a4dc321` ("S3 port onto C6")
  imported work derived from OpenHaldex-S3 into the C6 lineage. Derived / ported
  components include, in whole or in part:
  - CAN analysis / GVRET / SavvyCAN-style tooling (CAN View); Edge carries this as
    the analyzer path in `src/OpenHaldexC6_Analyzer.cpp`
  - portions of the map editor / Expert mode, web UI structure and API control, as
    credited in the upstream README

> MIT requires that the copyright notice and permission notice below be retained in
> all copies or substantial portions of the derived code. These portions remain
> available under MIT.

### MIT License (OpenHaldex-S3)

```
MIT License

Copyright (c) 2026 SpringfieldVW.com

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

## 3. Forbes Automotive original work

Portions original to Forbes Automotive are the base Edge builds on. They include
the Gen2 / Gen4 / Gen5 reverse engineering and implementation, the live-data
readout, the low-power sleep system, Long Learn and the other Forbes-authored code,
and the hardware design files (Gerbers, schematics, PCB layouts and enclosure
files). Pull requests merged upstream keep their authors' credit: louij2 (Home WiFi
bridge mode, Backup & Restore, Bench Mode, OTA release channels) and danati
(Bluetooth LE for the DashCAN app). All of it is distributed under the **MIT
License**; see [`LICENSE.md`](LICENSE.md).

Edge is a personal project. It is not affiliated with or endorsed by Forbes
Automotive. For assembled hardware, official firmware and support, see the upstream
project and [forbes-automotive.com](https://forbes-automotive.com/).

---

## 4. Edge additions

Edge's own code, web UI, tests and tooling are contributed under the same MIT
terms as upstream, copyright Kile Thomson. Two Edge changes were adopted upstream
with credit: the per-corner slip geometry and the ESP_14 Launch PWM floor.

---

## Summary

- **Edge:** open source under the MIT License, same as upstream, no extra terms.
  Commercial use and redistribution are permitted subject to keeping the copyright
  and licence notices.
- **Upstream lineage:** the ABangingDonk (Gen1) and meatro/OpenHaldex-S3 (analyzer /
  GVRET / SavvyCAN tooling) portions keep their original licence position; the MIT
  notice for the S3-derived portions is preserved above as MIT requires.
- **No warranty.** See the disclaimer in [`README.md`](README.md) and the warranty
  terms in [`LICENSE.md`](LICENSE.md).
