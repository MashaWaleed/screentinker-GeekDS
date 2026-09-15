# Vendored front-end libraries

Third-party libraries committed directly to the repo (not fetched from a CDN or built
from npm) so self-hosted / air-gapped instances work with no external dependency and no
build step.

**Anything added here ships in the release tarball**, so it must carry its licence notice —
a minified bundle usually has its headers stripped, which is exactly when the notice has to
be kept as a separate file next to it. Record the licence below and add a `<name>.LICENSE`.

## redoc.standalone.js
- **Library:** Redoc — renders the OpenAPI reference served at `/docs`.
- **Version:** 2.3.9
- **Licence:** MIT — Copyright (c) 2015-present, Rebilly, Inc. Full text in
  [`redoc.LICENSE`](redoc.LICENSE). The bundle itself carries no header (stripped by the
  upstream minifier), which is why the notice is kept separately.
- **Source:** https://cdn.redoc.ly/redoc/v2.3.9/bundles/redoc.standalone.js
- **Why committed:** the API reference must render on offline instances — no CDN, no build step.
- **Regenerate / update:**
  ```sh
  curl -sL https://cdn.redoc.ly/redoc/v2.3.9/bundles/redoc.standalone.js \
    -o frontend/vendor/redoc.standalone.js
  # drop the trailing sourcemap comment (the .map is intentionally not vendored)
  sed -i '/sourceMappingURL=redoc.standalone.js.map/d' frontend/vendor/redoc.standalone.js
  ```

## pdfjs/
- **Library:** pdf.js (`pdfjs-dist`) — renders an uploaded PDF to one image per page, in the
  uploader's browser, so the pages reach the server as ordinary PNGs. The server never parses a
  PDF and carries no PDF dependency; see `frontend/js/components/pdf-pages.js`.
- **Version:** 6.3.289
- **Licence:** Apache-2.0 — Copyright Mozilla Foundation. Full text in
  [`pdfjs/pdfjs.LICENSE`](pdfjs/pdfjs.LICENSE). The three optional image codecs under `wasm/`
  each carry their own permissive notice beside them: OpenJPEG (BSD-2-Clause, `openjpeg.LICENSE`),
  the JBIG2 decoder (Apache-2.0, `jbig2.LICENSE`) and qcms colour management (MIT, `qcms.LICENSE`).
  All are on the licence gate's ALLOW list. `quickjs-eval.wasm` (PDF form scripting) is
  deliberately NOT vendored — page rendering does not need it.
- **Files:** `pdf.min.mjs` (API), `pdf.worker.min.mjs` (loaded as a same-origin Web Worker),
  `wasm/{openjpeg,jbig2,qcms_bg}.wasm` (JPEG 2000 / JBIG2 / ICC, hit by scanned PDFs).
- **Why committed:** offline instances and no build step, same as Redoc. It is loaded lazily —
  the dashboard imports it only when someone actually uploads a PDF, so the 1.7 MB never lands
  on a session that does not.
- ⚠️ The dashboard CSP needs `'wasm-unsafe-eval'` in `script-src` for the codecs (it allows
  WebAssembly compilation only, NOT `eval`). Worker and wasm are same-origin, so `'self'` covers them.
- **Regenerate / update:**
  ```sh
  cd "$(mktemp -d)" && npm pack pdfjs-dist@6.3.289 --silent && tar xzf pdfjs-dist-*.tgz
  V=<repo>/frontend/vendor/pdfjs
  cp package/build/pdf.min.mjs package/build/pdf.worker.min.mjs "$V"/
  cp package/wasm/{openjpeg,jbig2,qcms_bg}.wasm "$V"/wasm/
  cp package/LICENSE "$V"/pdfjs.LICENSE
  cat package/wasm/LICENSE_OPENJPEG package/wasm/LICENSE_PDFJS_OPENJPEG > "$V"/wasm/openjpeg.LICENSE
  cat package/wasm/LICENSE_JBIG2    package/wasm/LICENSE_PDFJS_JBIG2    > "$V"/wasm/jbig2.LICENSE
  cat package/wasm/LICENSE_QCMS     package/wasm/LICENSE_PDFJS_QCMS     > "$V"/wasm/qcms.LICENSE
  sed -i '/^\/\/# sourceMappingURL=/d' "$V"/pdf.min.mjs "$V"/pdf.worker.min.mjs
  ```
  Then bump the version in this section and in `pdf-pages.js` (it pins the version for the
  worker path).
