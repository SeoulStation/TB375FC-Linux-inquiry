# Evidence index

| Question / fact | Files |
|---|---|
| Full chronology and current unresolved state | [Korean diary](TB375FC-development-diary-20261124.ko.md) |
| FIRST original result vs separate completion | [Summary](evidence/summary.json) |
| FIRST observed write pair | [Trace](evidence/first/trace.txt), [profile](evidence/first/profile.txt), [stats](evidence/first/stats.txt), [viewer](evidence/first/viewer.txt) |
| SECOND 875 transfer pairs / 1,750 events | [Trace](evidence/second/trace.txt), [profile](evidence/second/profile.txt), [stats](evidence/second/stats.txt) |
| SECOND phase markers and DRM restore | [Viewer](evidence/second/viewer.txt), [phase analysis](analysis/phase-analysis.md) |
| 80 checksum messages, FW recovery/update | [Kernel stream](evidence/second/kernel-stream.raw), [count corrections](analysis/spi-analysis-corrections.md) |
| Reader timing / record sequence coverage | [Stream metadata](evidence/second/kernel-stream-meta.json), [phase analysis JSON](analysis/phase-analysis.json) |
| Before/after snapshots | [Before](evidence/second/kernel-before.txt), [after](evidence/second/kernel-after.txt) |
| ELF change scope and remaining callsites | [Scope JSON](analysis/source-scope.json), [source discussion](analysis/source-candidates.md) |
| FD before first IRQ grid write | [Packet windows](analysis/spi-analysis-packet-windows.md), [trace](evidence/second/trace.txt) |
| Source selection and exact copied bytes | [Provenance](PROVENANCE.json), [code scope](code/README.md) |

The continuous stream is textual `/dev/kmsg` output despite the `.raw` suffix. Kernel snapshots and raw stream include diagnostic kernel addresses. The metadata records software acquisition times; it does not establish optical synchronization or physical causality. Historical analysis references some local-only paths and files; only files listed in this bundle are included.
