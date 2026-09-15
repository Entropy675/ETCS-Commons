# ETCS Web Shell host page

Serve this directory with the native ETCS HttpServer (COOP/COEP) together with:

- `etcs.js` (or extensionless `etcs` glue) + `etcs.wasm`
- `ShellProvider.wasm` (and optionally `WindowProvider.wasm`)

Rebuild ShellProvider / etcs with `EMSCRIPTEN=1` after the WebTerminal bridge
and ETCS `DL_EXTENSION=.wasm` patches.

The page terminal calls `etcs_web_shell_push_line` so `lsh::read_line` unblocks.
