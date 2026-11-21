# Compile version numbers (MALI_COMPILE_VERSION)

`MALI_COMPILE_VERSION` in `vk/src/mali_compile.c` is one global, monotonically
increasing counter for the shader compiler. It is part of the disk-cache key and
of the pipeline-cache UUID, so a wrong number leaves stale GPU code in users'
caches (the v19/v20 dynamic-buffer incident).

Rules:

1. Bump by exactly one (`n` -> `n + 1`) only when the generated shader binary or
   the serialized `struct mali_compiled` layout can change for the default
   configuration: NIR lowering, backend options, descriptor/push layouts that
   are hashed into the compiled output, or a Mesa patch series that can change
   codegen.
2. Edits that cannot change default output do **not** bump: input validation,
   cache bookkeeping/statistics, logging, comments, or pure refactors. Re-bless
   `vk/tests/compile-guard.txt` with `vk/tests/compile-guard.sh --bless` at the
   existing version instead.
3. An opt-in environment switch that changes output is only exempt when it also
   bypasses the disk cache (as `MALI_OPT_GCM` does). If the key cannot tell the
   modes apart, the switch needs a bump.
4. Never reuse a number and never pick one at branch-creation time. Open PRs are
   numbered in merge order: a PR takes `main + 1` when it is rebased onto main,
   and every later PR shifts up. The authoritative open-PR table is maintained
   by the version lane (`/tmp/oc/vers.md` during the sweep).
5. The version line and the re-blessed guard record go in the same commit; CI's
   `vk/check.sh` fails when the guard hash moved without the recorded version.
