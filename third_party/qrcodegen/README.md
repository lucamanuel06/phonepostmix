# qrcodegen (vendored)

[QR Code generator library](https://www.nayuki.io/page/qr-code-generator-library) by
Project Nayuki, MIT licensed. Vendored rather than fetched so the build has one fewer
network dependency and so the exact bytes we compile are visible in this repository.

- Upstream: <https://github.com/nayuki/QR-Code-generator> (`cpp/qrcodegen.{hpp,cpp}`)
- Licence: MIT — see the header comment at the top of each file.
- Local modifications: none.

Used by `src/plugin/QrCodeComponent` to render the listen URL in the plugin editor.
