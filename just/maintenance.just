# just/maintenance.just — Maintenance: clean, caches, dependency update.
#
# Destructive recipes here name exactly what they will remove and confirm before doing it, unless
# invoked with --yes for non-interactive use.

# Remove build output. Names what it will remove and confirms; --yes for CI.
maintenance-clean *args:
    @just _not-implemented maintenance-clean 2.1.5

# Regenerate THIRD_PARTY.md from deps/manifest.toml.
maintenance-deps:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    python3 tools/deps/attribution.py

# Fail if THIRD_PARTY.md no longer matches deps/manifest.toml. Run by CI on every pull request.
maintenance-deps-check:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    python3 tools/deps/attribution.py --check

# Prove a disabled feature fetches, builds and links nothing. Configures twice; minutes on a cold cache.
maintenance-deps-test *args:
    #!/usr/bin/env bash
    set -euo pipefail
    cd "{{root}}"
    python3 tools/deps/test_gating.py {{args}}
