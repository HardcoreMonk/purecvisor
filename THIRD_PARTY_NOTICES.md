# Third-Party Notices

PureCVisor includes or references third-party components that remain under their
own licenses.

| Component | Use | License |
|-----------|-----|---------|
| Chart.js 4.4.4 | Web UI chart rendering (`ui/vendor/chart.umd.min.js`) | MIT |
| noVNC 1.5.0 | Web UI VNC client bundle (`ui/vendor/novnc/novnc.esm.js`) | MPL-2.0 |
| Coolicons 1.2.2 subset | Web UI SVG icon sprite (`ui/vendor/coolicons/`) | CC BY 4.0 |
| Pretendard | Self-hosted Web UI font CSS and font files (`ui/vendor/pretendard/`) | SIL Open Font License 1.1 |
| QRCode Generator for JavaScript (Kazuhiko Arase) | Web UI QR code rendering (`ui/vendor/qrcode/qrcode.js`) | MIT |
| Astro 7.2.4 | Public documentation static site builder (`site/package-lock.json`) | MIT |
| Starlight 0.41.7 | Public documentation toolkit (`site/package-lock.json`) | MIT |
| npm development dependencies | Accessibility and browser QA tooling (`package-lock.json`) | See each package entry in `package-lock.json` |

PureCVisor project code is licensed under the Apache License 2.0.
It does not change the license terms of these third-party components.

## Copyright notices and license texts

These notices are shipped in the deb package as
`/usr/share/doc/purecvisor-single/THIRD_PARTY_NOTICES.md` (the PureCVisor
license itself is `/usr/share/doc/purecvisor-single/copyright`).

### Chart.js (MIT)

Copyright (c) 2024 Chart.js Contributors. https://www.chartjs.org

### QRCode Generator for JavaScript (MIT)

Copyright (c) 2009 Kazuhiko Arase. http://www.d-project.com/
The word "QR Code" is a registered trademark of DENSO WAVE INCORPORATED.

### MIT License text (applies to Chart.js and QRCode Generator for JavaScript)

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

### noVNC (MPL-2.0)

`ui/vendor/novnc/novnc.esm.js` is an unmodified-logic bundle of `lib/rfb.js`
from @novnc/novnc 1.5.0. Source and license: https://github.com/novnc/noVNC
(tag v1.5.0); MPL-2.0 text: https://www.mozilla.org/MPL/2.0/

### Coolicons (CC BY 4.0)

Coolicons by Kryston Schwarze (https://github.com/krystonschwarze/coolicons),
version 1.2.2 subset, licensed under CC BY 4.0
(https://creativecommons.org/licenses/by/4.0/). The vendored sprite is a subset
of the upstream icons.

### Pretendard (SIL Open Font License 1.1)

Pretendard, https://github.com/orioncactus/pretendard, licensed under the SIL
Open Font License 1.1 (https://openfontlicense.org/). The fonts are bundled
and redistributed unmodified together with this notice.
