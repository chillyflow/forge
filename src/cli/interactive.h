#ifndef FORGE_CLI_INTERACTIVE_H
#define FORGE_CLI_INTERACTIVE_H
#include "forge/forge.h"

/* The caller loads the model once and retains ownership. This REPL creates an
 * agent per user request while sharing one bounded, in-memory conversation. */
forge_status fg_cli_interactive(const forge_agent_config *, size_t history_bytes,
                                size_t history_turns, forge_event_fn, void *event_userdata,
                                forge_error *);
#endif
