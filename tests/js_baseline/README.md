# Pure-JS reference implementations

Snapshots of `public/js/{editor,post-translate,tasfa-download,tagmap}.js` taken
before their compute paths moved into CWIST WASM kernels
(`src/utils/client_core.h`, `src/utils/tag_layout_core.h`).

They are **never served**. They exist only as the behavioural baseline for
`tests/test_wasm_js_baseline_parity.cjs` and `tests/test_tagmap_hit_parity.cjs`,
and as the input that `tools/build_client_core_js.py` patches to produce the
shipped scripts. Run `make wasm-client-core` after editing either side.
