# Serial bridge

Runs the console against the machine where the browser has no port picker, such
as Claude's built-in browser, for headless debugging. Not used in normal runs.

1. Close anything else holding the port, then start the bridge:
   `python -u com_bridge.py COM8` (needs pyserial; the default is COM8).
2. Serve the page with `pnpm demo` and open it.
3. Run `shim.js` in the page, then press Connect. Repeat this step after a reload.

The bridge keeps the port open across reloads, so a job on the machine survives
them. It serves one client, on localhost only, and stop it to free the port.

This drives the real machine: page buttons and jobs move it as usual.
