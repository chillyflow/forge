# Forge agent instructions

## Serena semantic tooling

This repository is configured to use Serena as a project-scoped Codex MCP server. Preserve and use the existing configuration in `.codex/config.toml` and `.serena/project.yml`.

### Normal use

- When Serena tools are available, call Serena's `initial_instructions` tool before the first other Serena tool in a task.
- Prefer Serena's symbol overview, declaration, implementation, reference, diagnostics, rename, and symbol-body tools for semantic C/C++ and Python work. Use ordinary Codex file and shell tools for operations intentionally excluded by Serena's `codex` context.
- Do not reinstall, reinitialize, upgrade, or rebuild Serena's index on every task. Do maintenance only when Serena is unavailable, its configuration changed, relevant build metadata changed, or the index is demonstrably stale.
- Keep both `cpp` and `python` in `.serena/project.yml`. Add another language server if future tracked source introduces another language that needs semantic navigation.

### Compilation database maintenance

- Forge's normal Visual Studio build does not emit `compile_commands.json`. Serena's clangd support uses the separate ignored Ninja analysis build under `.scratch/serena-build`.
- Run `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/configure-serena.ps1` after changing CMake targets, source lists, compiler flags, generated dependencies, or toolchain configuration.
- The script must continue using `FORGE_WITH_LLAMA=OFF`, tests enabled, and `CMAKE_EXPORT_COMPILE_COMMANDS=ON`, then copy the generated database to the repository root.
- Never commit `compile_commands.json`, `.cache/`, `.serena/cache/`, `.serena/logs/`, `.serena/memories/`, or `.serena/project.local.yml`.

### Fresh-machine recovery

If `serena` is missing, restore it with:

```powershell
uv tool install -p 3.13 serena-agent
```

Run `serena init --language-backend LSP` only when the user-level Serena configuration is absent. Do not run `serena setup codex`; this repository intentionally uses the project-scoped `.codex/config.toml` and a global registration would duplicate it.

Then run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/configure-serena.ps1
serena project health-check .
serena project index . --log-level WARNING
codex mcp get serena
```

The health check must pass for the configured C/C++ and Python language servers, and `codex mcp get serena` must report the server as enabled with the `codex` context.

### Updating Serena

Upgrade deliberately, not automatically:

```powershell
uv tool upgrade serena-agent
serena --version
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/configure-serena.ps1
serena project health-check .
serena project index . --log-level WARNING
codex mcp get serena
```

After an upgrade, compare current Serena client/configuration documentation with `.codex/config.toml` and `.serena/project.yml`; adjust renamed flags or schema fields, rerun all checks above, and update this file if the maintenance procedure changed. After changing `.codex/config.toml`, start a new Codex task so the MCP tool list reloads.
