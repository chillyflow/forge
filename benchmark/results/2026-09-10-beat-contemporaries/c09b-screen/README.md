# Candidate-09 confirmation screen: outcome — signal not confirmed

Executed September 11, 2026. 14 runs, all retained: same frozen
candidate-09 runtime, same loop-budget policy, profile, and schedule as
c09, with new run ids. Preregistered bar (written before any run here):
confirm at >=3/12 Python with >=2 manifests covered, else close both the
success reading and the flag.

## Result

| Population | Result |
| --- | --- |
| Python (4 manifests × 3) | **1/12** — original 1/3; renamed, paraphrased, distractor 0/3 |
| Go (2 manifests × 1) | **2/2 — no regression** |

The bar is missed on both halves. The c09 success reading is closed along
with the flag.

## Joint assessment (qualitative, never pooled)

c09: 5/12 across 3 manifests. Confirmation: 1/12 across 1 manifest. Same
binary, profile, schedule — combined 6/24 (25%) against the c06 comparator's
2/12 (17%). The c09 high was favorable variance, consistent with the
campaign's established finding that run-to-run trajectories diverge under
fixed seed and temperature (GPU execution nondeterminism): the family
oscillates run to run, and c09 caught the up-swing. Nothing about the flag
explains a 5-to-1 swing between identical batches; no mechanism telemetry
(pass trajectories show no shared budget-guidance signature distinct from
ordinary passes) supports one either.

The token edge (−5.2% generated) was compositional (more early-finishing
passes in c09), and with success unmoved there is no token claim left.

The flag stays in the tree, default-off, G0-verified; it changes nothing
unless requested. No gate batch follows from either screen under any
outcome. Both screens stand immutable; neither is re-run.
