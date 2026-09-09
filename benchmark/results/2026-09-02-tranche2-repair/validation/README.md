# Implementation validation

[core-ctest.txt](core-ctest.txt) preserves the full Release test run after the rendered-context
and final-only native-template fixes: **24 CTest groups passed**, including
23 benchmark Python tests. This run preceded the final restoration of the
first candidate's repair-prompt wording; it is not labeled as a test of the
later documentation/evidence commit.

The context regression exercises actual rendered overhead, reverse removal of
optional dependency bundles, shared dependencies, intact native call/result
pairs, and a pinned prompt that truly cannot fit. The chat-template regression
uses the native Qwen template and a final-only tool registry while retaining
historical memory calls; it accepts final and rejects a new memory call.

The final candidate was rebuilt in the clean evaluation checkout. Its context
and chat-template test executables passed before model evaluation. The separate
six-run atomic/quota development probe passed 6/6; the complete final 87-run
development matrix is the gate evidence and does not substitute those probe
successes for any matrix outcome.

After all holdout timing runs ended, the main checkout was rebuilt to the same
source and its two focused tests passed again. The output is retained in
[final-context-template-ctest.txt](final-context-template-ctest.txt). The clean
measured executable was unchanged.
