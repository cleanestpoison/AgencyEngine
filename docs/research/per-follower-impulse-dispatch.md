# Per-follower impulse dispatch

Accessed 2026-08-20.

## Question

Should a due lens ask once about the whole party, as it does now, or issue one independent LLM request per follower?

## Recommendation

Do **not** replace the current party-level ask with an all-follower fan-out by default. Fan-out is a different product behavior, not a mechanical prompt optimization: it changes one party decision into up to `N` independent decisions, multiplies request bursts and downstream work, changes the probability of forced impulses, and can cue several companions into the same opening.

The idea is still worth testing for a real weakness: a party prompt can under-attend followers or favor their serialized order. The lowest-risk alternative is a **focal-follower ask**: retain one request per due lens, rotate which follower is eligible to speak, include the shared scene and a compact roster for targets, and render full private context only for the focal follower. Adopt it only if paired evaluations show better grounding or speaker fairness. This preserves the cost and one-result shape, but deliberately trades “best impulse anywhere in the party now” for “this companion gets an independent turn under the lens.”

## Current behavior

Repository evidence:

- `src/Director.cpp:1001-1103` builds one context containing every present follower, each follower's recent thoughts and ledger, plus one shared player event tail.
- `agencyengine_impulse_base.prompt:231-318` renders every follower's personality, background, aspirations, standing, thoughts, ledger, and memories.
- `src/Director.cpp:1313-1561` sends one request for one due lens. The response names one speaker; silence is also valid. Therefore the current unit is **one party-level choice with at most one impulse**, not one impulse for every follower.
- `src/Director.cpp:2535-2598` may dispatch several different due lenses in a pass, but each lens still receives one party request.
- Lens cadence is global per lens. `StampAsk`/`ReleaseAsk` use one `inFlight` bit and one deadline for the lens (`src/Director.cpp:1262-1310`).
- The pending store can hold one impulse per companion per lens, so storage itself can represent fan-out results (`include/PendingImpulse.h:113-124`). The delivery layer is not party-arbitrated, however: `PumpPendingCues` collects every eligible companion cue and fires all of them in the same pass (`src/Director.cpp:1985-2138`).
- A carried impulse may immediately cause a second LLM call for a private thought (`include/Settings.h:175-180`; `src/Director.cpp:776-782`) and later receives independent resolution calls. More carries amplify more than the initial impulse requests.

One recorded two-follower run used a 33,341-byte context JSON, of which the shared 40-event player tail was 32,879 bytes (`bug-analysis/issue-feels-like-banter/AgencyEngine.log:154-159`). This is one observed case, not a general token measurement, but it shows that a naive per-follower split can duplicate a large shared suffix even when follower-specific event tails are small.

## What fan-out changes

Let:

- `N` = present followers;
- `S` = shared prompt and scene input (instructions, world/player state, player event tail);
- `F_i` = follower `i`'s private context;
- `C_i` = whether follower `i` produces a carry;
- `g` = 1 when private-thought generation is enabled, otherwise 0.

Approximate input work for one lens:

- Current party ask: `S + ΣF_i`, one completion, at most one carry.
- Naive per-follower fan-out: `N×S + ΣF_i`, `N` completions, up to `N` carries.

Immediate LLM-call count:

- Current: `1 + g×C`, where `C` is 0 or 1.
- Fan-out: `N + g×ΣC_i`, before later resolution checks.

Provider prompt caching can discount a repeated common prefix on supported model/provider combinations, but it does not make requests free and cannot be assumed here. OpenRouter documents model-specific support, minimum token thresholds, automatic versus explicit caching, provider stickiness, and cache-read pricing differences. AgencyEngine's SkyrimNet variant can instead target an arbitrary endpoint or local model (`manifest.yaml:31-78`), and the public custom-prompt interface exposes neither cache controls nor usage details to AgencyEngine. Simultaneously dispatched fan-out requests may also race before a first cache write is reusable. Treat caching as an observed optimization, not an invariant.

SkyrimNet documents `SendCustomPromptToLLM` as an asynchronous queued task and reports only whether it was accepted; its public contract does not specify custom-prompt concurrency, ordering, or queue capacity. OpenRouter likewise documents that request limits vary and that an upstream provider can return HTTP 429. Therefore fan-out adds a backend-dependent burst risk, especially for local endpoints, without a concurrency guarantee AgencyEngine can safely design around.

### Forced-impulse probability

The prompt rolls the configured forced chance once per ask (`agencyengine_impulse_base.prompt:62-80`). At the default `p = 0.20`, independent per-follower asks produce:

| Followers | At least one forced result | At least two forced results | Expected forced results |
|---:|---:|---:|---:|
| 1 | 20.0% | 0.0% | 0.2 |
| 2 | 36.0% | 4.0% | 0.4 |
| 3 | 48.8% | 10.4% | 0.6 |
| 4 | 59.0% | 18.1% | 0.8 |
| 5 | 67.2% | 26.3% | 1.0 |

Thus preserving the setting unchanged makes larger parties structurally louder. If all results carry, multiple companions can receive cues together. If only one result may carry, the implementation needs a barrier and an explicit arbitration rule; “first callback wins” would turn network/model latency into character selection. Waiting for every result and discarding all but one spends the fan-out cost without defining a trustworthy comparison score.

### Cadence semantics

Current interval/cooldown answers “when may this lens ask the party again?” Fan-out forces a choice:

1. **One global lens clock, all followers fan out when due.** Same wall-clock cadence, `N` times the initial calls, and potentially `N` carries. One carry applies the cooldown to everybody.
2. **Per-follower lens clocks.** Independent autonomy, but roughly `N` times the steady-state asks and a substantially larger clock/interface surface.
3. **One global clock, one rotating focal follower.** Same ask budget, but a follower is considered only every roughly `N` lens intervals. The interval remains a party cost knob, not a per-follower responsiveness promise.

None is behaviorally equivalent to the current design.

## Quality evidence

Two primary studies support testing the attention concern, but do not prove AgencyEngine currently fails:

- Eicher and Irgolic, *Reducing Selection Bias in Large Language Models* (2024), measure list-selection bias and report a strong primacy effect whose structure depends on model and object type. AgencyEngine asks the model to select a speaker from a serialized follower list, so order sensitivity is a plausible risk.
- Liu et al., *Lost in the Middle: How Language Models Use Long Contexts* (TACL 2024), find that retrieval performance can degrade when relevant information sits in the middle of long contexts, including explicitly long-context models. AgencyEngine places complete follower sections sequentially in a long rendered prompt, so middle-follower evidence may receive less reliable use.

Applicability is limited: the papers test controlled selection/retrieval tasks and older model families, while an impulse is a creative, constrained judgment. They justify a paired evaluation, not an architectural switch.

Per-follower focus has genuine potential advantages:

- removes competition among speaker candidates;
- reduces positional/list-selection bias;
- can render one follower's evidence more prominently and with less unrelated private context;
- gives explicit fairness if the scheduler rotates focal followers.

It also loses useful information:

- the current model can compare candidates and choose the strongest impulse rather than forcing a weak companion-local answer;
- Relationship impulses can target another companion and depend on party-wide standing;
- independent requests can produce duplicate or mutually awkward topics because they do not coordinate;
- local silence decisions tend to answer “does this follower have anything?” rather than the current “is anything in the party worth interrupting the day for?”

## Alternatives

### A. Keep the party ask; rotate follower order

Smallest experiment and mitigation. Cyclically rotate the serialized follower order per lens ask. This preserves one call, one result, cross-companion comparison, cadence, and delivery. It reduces persistent first-position advantage but does not guarantee equal attention.

### B. One focal follower per ask

Best candidate if the quality problem is demonstrated. The prompt interface should separate:

- `candidate`: one fully rendered possible speaker;
- `party`: compact names/IDs and only context needed for valid targets;
- shared scene/player context.

The lens asks only whether `candidate` has an impulse. Rotate candidates deterministically, preferably skipping a companion already carrying that lens where doing so does not starve them. Keep one request and one result. Document that per-follower exposure slows as party size grows.

### C. Fan out to every follower and carry every valid result

Only choose this if the desired behavior is explicitly “every companion independently gets a chance on each lens deadline.” It requires:

- a per-lens batch state with an outstanding-response count, not one `inFlight` bit;
- one batch-level forced-roll policy or a party-size-adjusted probability;
- partial queue-failure and timeout handling;
- deterministic completion independent of callback order;
- party-level cue arbitration so several companions do not all seize the same opening;
- explicit cooldown semantics;
- cost/UI counters measured per follower request;
- load tests against cloud and local endpoints.

### D. Party selector followed by a focal writer

A first call chooses a candidate, then a focused call writes the impulse. This preserves global prioritization and improves writing focus, but it always costs two calls and introduces selector/writer disagreement. The current one-call module already performs both jobs; use this only if evaluation isolates writing quality, rather than candidate attention, as the failure.

## Verification experiment

Before changing production semantics:

1. Capture 30-50 real snapshots across every lens and party sizes 1, 2, 3, and 4+. Capture the fully rendered prompt where possible; `lastContextJson` alone omits decorator-produced profiles, standings, world knowledge, and memories.
2. Run the same fixed model/config on paired variants:
   - current order;
   - cyclically permuted/reversed current order;
   - one focused request per follower, with forcing disabled;
   - separately, a forced-output set to compare quality when silence is unavailable.
3. Blind-score observable contracts: cited `why_now` exists in stamped state, no invented facts, not a live reaction/current undertaking, no ledger repeat, correct lens kind, speaker fit, target validity, and stage-direction contract.
4. Measure speaker-selection changes under permutation, quiet/carry rate per follower, duplicate topics across focused outputs, input/output tokens, elapsed time, rejected/failed requests, generated-thought calls, resolution calls, and cues released in one quiet opening.
5. Decision rule: first try order rotation if permutation sensitivity is the main defect. Choose focal rotation only if it materially improves grounded valid outputs or fairness at the accepted slower per-follower exposure. Choose full fan-out only if multiple simultaneous independent impulses are itself the desired behavior and the cadence/cue redesign is accepted.

## Primary sources

- AgencyEngine source and prompt files cited inline in this repository.
- SkyrimNet, `SendCustomPromptToLLM` public Papyrus contract: <https://github.com/MinLL/SkyrimNet-GamePlugin/blob/main/Source/Scripts/SkyrimNetApi.psc#L147-L181>
- J. E. Eicher and R. F. Irgolic, “Reducing Selection Bias in Large Language Models,” arXiv:2402.01740: <https://arxiv.org/abs/2402.01740>
- N. F. Liu et al., “Lost in the Middle: How Language Models Use Long Contexts,” arXiv:2307.03172 / TACL 2024: <https://arxiv.org/abs/2307.03172>
- OpenRouter limits documentation: <https://openrouter.ai/docs/api-reference/limits>
- OpenRouter prompt-caching documentation: <https://openrouter.ai/docs/guides/best-practices/prompt-caching>
