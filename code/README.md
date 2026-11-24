# Working-code scope

These are byte-identical selected working files. `PROVENANCE.json` records the source-relative paths and SHA256. They are review material, not a complete build or a ready-to-run device package.

| Directory | Purpose and limits |
|---|---|
| `viewer/` | Native DRM photo viewer source referenced by the saved phase analysis. Contains exact 2944×1840/30Hz mode gating, activation/restore and phase logging. Needs Linux DRM headers and the separate photo asset. |
| `second-collector/` | Generated C/headers from the latest SECOND source tree: trace collection, before/after kmsg, continuous kmsg reader, lifecycle and cleanup. `qualification.h` preserves the historical boot ID, file hashes and guards. This is source selection, not a claim that every binary can be rebuilt from this directory alone. |
| `module-analysis/` | PC ELF candidate builder, existing ELF parser and the prior grid lock assembly wrapper. The builder refers to local absolute paths and requires exact parent module bytes that are excluded. It describes the one restore-callsite omission; it does not contain the complete vendor touch driver. |

The collector has additional framework/build dependencies outside this selection. Do not treat its historical readiness macros as authorization for a new run. No boot images, kernel modules, firmware payloads, flashing scripts or service-authentication material are included.

Sources preserve their existing notices. This bundle does not assign a new blanket license to code or claim ownership of vendor/third-party material. Exact vendor source matching the installed ELF remains a diagnostic gap.
