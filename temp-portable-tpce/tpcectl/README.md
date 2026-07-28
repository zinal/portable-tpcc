# tpcectl

Lightweight orchestrator for TPC-E fair-use testbed runs. Specification: [`docs/spec-orchestrator.md`](../../docs/spec-orchestrator.md).

## Build

```bash
cd tools/tpcectl
go build -o tpcectl ./cmd/tpcectl
```

The binary is self-contained (Go + embedded SSH via `golang.org/x/crypto/ssh`). No Ansible or Kubernetes required.

## Commands

| Command | Description |
| --- | --- |
| `validate` | Validate profile YAML (topology, partitions, DM uniqueness) |
| `plan` | Print planned actions and argv without side effects |
| `deploy` | Copy artifacts via SSH/SFTP or tar stream; write `deploy-manifest.json` on each host |
| `cleanup` | Remove manifest-managed paths (`--yes`; optional `--delete-runs --run-id`) |
| `schema` | Apply `create_tables*.sql` via PostgreSQL (`--recreate` to drop public schema) |
| `load` | Run Loader shards in parallel; apply deferred indexes/FKs when configured |
| `run` | Full lifecycle with `--skip-deploy/schema/load/collect` and `--no-stop` |
| `start` | Distribute run-config; start BH→MEE→DM/standalone→CE; set run-state `running` |
| `stop` | Normative shutdown from run-state (`CE→drain→DM→MEE→BH`) |
| `status` | PID aliveness and TCP listen checks for current run |
| `collect` | Download instance outputs plus `orchestrator/{profile,run-config,run-state}.json` |
| `smoke` | Short `run` with `--duration-sec` (default 120) and optional `--users` |

```bash
tpcectl validate -f profiles/smoke-single.yaml
tpcectl smoke   -f profiles/smoke-single.yaml --skip-deploy --skip-schema --skip-load
tpcectl run     -f profiles/lab-multi.yaml --skip-deploy --skip-schema --skip-load
tpcectl collect -f profiles/smoke-single.yaml
```

## Profiles

| File | Topology |
| --- | --- |
| [`profiles/smoke-single.yaml`](profiles/smoke-single.yaml) | `standalone_driver` + 1 BH + 1 MEE |
| [`profiles/lab-multi.yaml`](profiles/lab-multi.yaml) | 2 BH + 2 MEE + 1 DM + 2 CE (50k customers) |

## Runtime contract

- Global parameters: immutable `run-config.json` (see [`docs/examples/run-config.v1.json`](../../docs/examples/run-config.v1.json)).
- Per-instance parameters: command-line flags (`--instance`, `--role`, listen port, output directory, etc.).
- Runtime binaries must implement [`docs/spec-scalability.md`](../../docs/spec-scalability.md).

## Local state

Default state directory: `$XDG_STATE_HOME/tpcectl` or `~/.local/state/tpcectl`.

```
<state-dir>/runs/<run_id>/run-state.json
<state-dir>/profiles/<profile-id>/current-run.json
```

## SSH authentication

When `SSH_AUTH_SOCK` is set, `tpcectl` uses **ssh-agent** by default (including encrypted keys). Set `ssh.use_agent: false` in the profile to require a plaintext `private_key` file only.

Recursive `deploy` artifacts use **tar streaming** over SSH by default (`deploy.use_tar_upload: false` to fall back to per-file SFTP).

## Collect hooks

| Profile field | Purpose |
| --- | --- |
| `collect.post_command` | Shell hook after download (`$TPCECTL_COLLECT_DEST`) |
| `collect.egen_tester_export` | Optional EGenTester export hook (same env vars) |

Collected `orchestrator/profile.yaml` is redacted (inline `password`/`secret` values removed).

## Development

```bash
go test ./...
go vet ./...
```
