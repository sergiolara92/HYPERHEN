# R50 game callback provenance

Adapted from the user-supplied TacHen-source_2.zip. Original GPL notices and
credited etaHEN/HDE64 relocation code are retained. This private integration
imports only the game callback, bounded module/import readers, transactional
publication and trampoline helpers. TacHen UI, firmware tables, input capture
and menu are not imported. HyperHEN adapts the staged module path and uses its
own process-generation validation and ptrace credentials. The plugin attach
helper is private and does not change the shared ShellUI ptrace runtime.
