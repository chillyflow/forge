#ifndef FORGE_VERIFICATION_H
#define FORGE_VERIFICATION_H
#include "forge/forge.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Index, plan and execute staged Go/Python verification in a new recorded session.
 * config->model is unused and may be NULL. Command execution requires allow_exec
 * or an affirmative PROCESS policy callback, exactly as for run_command. Policy
 * arguments also contain workspace-relative cwd and the validation stage.
 * Network/filesystem isolation is NOT supplied by this API.
 *
 * Stop on the first failed command, policy denial, timeout or cancellation.
 * An empty path list requests broad verification. The caller owns *report_json
 * (when non-NULL) even on failure and releases it with forge_free. FORGE_OK with
 * applicable=false means no indexed Go/Python target exists, not that tests passed.
 * A blocked plan is an explicit verification failure, not an inapplicable plan.
 * Applicable checks snapshot regular workspace inputs before and after running
 * (100,000 files, 2 GiB total; root .git/.forge directories excluded). A changed
 * or incomplete snapshot cannot pass. Symlinks and special files are rejected.
 * Report/session artifacts include the exact plan and per-command evidence.
 */
forge_status forge_verify_workspace(const forge_agent_config *, const char *const *changed_paths,
                                    size_t path_count, forge_event_fn, void *, char **report_json,
                                    forge_error *);

#ifdef __cplusplus
}
#endif
#endif
