# Issues

Every defect found in this project, what caused it, and whether it is fixed.

`JOURNAL.md` explains each phase as a narrative; this file is the flat list, so
that "what is currently wrong with this program" can be answered without
reading eight chapters. Three sections:

- **[Open](#open)** — real defects, not yet fixed.
- **[Limitations](#limitations)** — things the program does not do. Scope, not
  bugs, but listed because a limitation you have to discover for yourself is
  indistinguishable from a defect.
- **[Fixed](#fixed)** — with root cause. Kept rather than deleted: most of them
  are the only record of *why* a piece of code is shaped the way it is, and
  several were found by a later phase exercising an earlier one.

Severity is about consequence, not effort:

| | |
|---|---|
| **Wrong answer** | Produces a number that is incorrect and does not say so |
| **Blocked** | Cannot produce an answer at all |
| **Degraded** | Correct, but slower, less accurate or less usable than it should be |
| **Cosmetic** | Visual or ergonomic only |

---

## Open

### #62 — The finest grid still diverges once before settling
**Severity:** degraded · **Found:** Phase 8 · **Area:** solver

Seeding omega from the wall distance ([#58](#fixed)) removed the startup blow-up
on the coarse and medium grids and pushed it from iteration ~84 to ~168 on the
fine one, but did not eliminate it there. The fine C-grid at Re = 10⁶ still
diverges once and is recovered by the Phase 7 cold retry, after which it
converges normally to 10⁻⁶ in 1,367 iterations.

The answer is unaffected - a retry that converges is as good as a first attempt
that converges - but relying on a rescue is not the same as not needing one, and
on a grid finer still it may not recover at all.

**Likely cause.** The fine grid has cell aspect ratios above 10⁵ near the wall.
The turbulence equations are under-relaxed at 0.5 by default, which is evidently
not cautious enough there while the mean flow is still establishing itself.

**Fix direction.** Ramp the turbulence relaxation over the first few hundred
iterations, or hold the model off entirely until the mean-flow residuals have
dropped a decade. Both are standard; neither has been tried here.

---

## Limitations

Not defects. Listed so they are not mistaken for any.

| | |
|---|---|
| **Laminar only** | There is no turbulence model. At the Reynolds numbers real aerofoils operate at (10⁶+) no steady laminar solution exists, and the solver will iterate without converging. Every result in this repository is at Re = 500, three orders of magnitude below reality, and none of it is comparable with handbook data. |
| **Steady only** | The solver looks for a steady state. Genuinely unsteady flow — vortex shedding, buffet, dynamic stall — is outside what it can represent, and would show up as non-convergence rather than as an answer. |
| **2D only** | No spanwise effects, no tip vortices, no finite-wing induced drag. Forces are per unit span. |
| **Incompressible** | Constant density. No transonic effects; no Mach number anywhere in the code. |
| **NACA four-digit sections only** | The geometry generator implements one family. |
| **No comparison against external data** | Nothing here has been checked against a wind tunnel or another CFD code. Every validation is either an exact identity, an analytic solution, or an internal consistency check. |
| **Coarse-grid results throughout** | See [#3](#3--no-grid-convergence-study-of-the-force-coefficients). |
| **Built and run on one platform only** | macOS 15.7, arm64, Apple Clang 17. Not a known defect: the platform-specific code is isolated behind `__APPLE__` and `_WIN32` guards, GLFW and OpenGL are confined to `cfd_app`, and `-DCFD_BUILD_APP=OFF` builds the whole numerical stack headlessly and passes all 251 tests with `-Werror`. But no other compiler or operating system has been compiled against, so portability is a design intention with one data point behind it. |

---

## Fixed

### Phase 0 — foundation

| # | Issue | Root cause | Fix |
|---|---|---|---|
| 7 | No CMake or Homebrew on the machine | — | Installed CMake and Ninja as user-local pip packages rather than touching system directories |
| 8 | Dear ImGui 1.92.9b has no docking | Docking lives on a separate branch | Built the tiled layout explicitly with pinned windows and hand-written splitters. Suits the CAD-like design goal better than floating panels anyway |
| 9 | Splitters came out 8× too wide | ImGui enforces `style.WindowMinSize` (32×32) on every window, including a 4 px splitter | Pushed `ImGuiStyleVar_WindowMinSize` to (1, 1) around the splitter |
| 10 | `gl.h` and `gl3.h` both included | GLFW picks its own OpenGL header unless told not to — the legacy `gl.h` on macOS — while `Application.cpp` includes core-profile `gl3.h` | `GLFW_INCLUDE_NONE`, set in the source and in the target's compile definitions. **Surfaced only under `-Werror`**, because it originates in a system header |
| 11 | Silent `double` → `float` promotion in viewport code | ImGui works in `float`, the camera in `double` | Explicit `static_cast<double>` so the widening is visible. Exactly what `-Wdouble-promotion` exists for |
| 12 | The initial view fit produced an absurd zoom | `frameBox()` needs the viewport's pixel size, which does not exist until the first frame is laid out | A `viewInitialized` flag: fit on the first frame with a real size, then leave the camera alone |
| 13 | `BeginTable`/`EndTable` mispairing | `EndTable` must not be called when `BeginTable` returns false (clipped or collapsed), but callers called it unconditionally | Helper returns `bool`; every call site honours it |
| 14 | Misleading frame-time readout | With event-driven idling, the inter-frame interval measures user inactivity, not cost | Measure CPU per frame, sampled before the vsync-blocking swap |
| 15 | Screenshots came back as bare wallpaper | macOS `screencapture` returns the desktop without window contents unless the process holds Screen Recording permission | Added `--screenshot`, which reads the window's own framebuffer with `glReadPixels`. Needs no OS permission and works over SSH. **This is how #16 was found** |
| 16 | Scale bar overlapped the tick labels; the x axis ran through the empty-state message like a strikethrough | — | Both repositioned. Found immediately by the first working screenshot |
| 17 | GoogleTest 1.18.0 was three days old | — | Pinned 1.17.0. A foundation should not sit on a release that has had no time to shake out |

### Phase 1 — geometry

| # | Issue | Root cause | Fix |
|---|---|---|---|
| 18 | Two tests failed and **the code was right** | On a cambered section with an open trailing edge, y<sub>t</sub>(1) ≠ 0 and the camber line is inclined, so the perpendicular offset displaces the two TE points along the chord by ±y<sub>t</sub>(1)·sinθ — 8.4×10⁻⁵ c for NACA 2412, matching the observed failure exactly. Only their *midpoint* is at x = c | Rewrote the assertions to the midpoint identity, and added a test pinning the straddle to its predicted magnitude. Recorded because the instinct on a red test is to change the code |
| 19 | `AddPolyline` silently reinterpreting arguments | ImGui 1.92.8 swapped `thickness` and `flags` | Nothing to fix — `IMGUI_DISABLE_OBSOLETE_FUNCTIONS` made it a hard compile error naming the exact call rather than a silent misread of `ImDrawFlags_Closed` as a line width |
| 20 | Airfoil fill bridged across the camber | `AddConvexPolyFilled` was used on a shape that is not convex | Switched to `AddConcavePolyFilled` — later replaced entirely, see #21 |
| 21 | **Fill spilled outside the outline** on high-camber sections (NACA 9410, 9640) — user-reported | Not a geometry error: the contour was verified to have zero self-intersections. ImGui's `AddConcavePolyFilled` ear-clips an arbitrary polygon, which is O(n²) and numerically fragile on a shape as thin and finely sampled as an aerofoil | Replaced with exact strip triangulation between corresponding upper and lower points. No search, no heuristics, linear in the point count — the section is *defined* as two surfaces at the same stations, so the region between them tiles into quads exactly. AA disabled on the fill so adjacent triangles meet exactly |
| 22 | Duplicate library on the link line | `cfd_tests` named both `cfd::core` and `cfd::geometry`, and geometry already carries core `PUBLIC` | Link what you directly use and let propagation do the rest |
| 23 | Viewport flashed empty on every keystroke | Regeneration ran on each keystroke, and "2412" passes through "2", "24", "241" | Separated "the section shown" from "the current input": a failed parse updates the error message and leaves the previous geometry on screen |

### Phase 2 — mesh

| # | Issue | Root cause | Fix |
|---|---|---|---|
| 24 | **Folded cells** (small negative areas) on the forward lower surface | Not adjacent rays crossing — the concave radius there is 0.7 c and the fold was at 0.037 c, twenty times closer. The *ray itself* doubled back, because for **P**(r) = **P**₀ + r·**d**(r) the line reverses once r·\|dθ/dr\| > 1. Two causes: a normalised lerp between unit vectors sweeps at up to 2·tan(Δ/2), and blending over linear r is **scale-invariant**, so widening the blend zone does nothing at all | Slerp (constant angular rate, so the sweep rate is exactly Δ) and blending over log r (so r·dw/dr can be shrunk by widening the *ratio* of radii). Span chosen as 2.5·\|Δ\| |
| 25 | Remaining folds at the trailing edge, **getting worse with refinement** (2 cells at Medium, 41 at Fine) | A curvature limiter capped the blend length correctly, then eight averaging passes smoothed it — and **averaging can lift a value back above the limit its own curvature demands**. The limiter was right and the smoothing threw it away | Replaced with a Lipschitz sweep: forward and backward min-propagation at a bounded rate. Smooth *and* never exceeds the limit. Zero inverted cells at every resolution |
| 26 | Far-field cells with aspect ratio 10⁴ | The outer wake boundary inherited the trailing edge's 10⁻⁴ c clustering, so far-field cells were 10⁻⁴ wide and a chord tall — out where the flow is uniform and nothing needs resolving. Looked tidy (vertical wake grid lines) and was a trap | Gave the outer boundary its own gentle distribution. Grid lines now shear by a fraction of a percent instead of staying exactly vertical. **Found two phases later, by the solver** — see #29 |

### Phase 3 — flow state

| # | Issue | Root cause | Fix |
|---|---|---|---|
| 27 | A tolerance below one ULP | `ViscosityScalesWithChord` compared values around 1.8×10⁻⁴ to an absolute 1e-20, where one ULP is 2.7×10⁻²⁰ — the check had silently become exact equality. GoogleTest diagnosed it precisely | Relative tolerances, and audited every other tight absolute tolerance in the file |
| 28 | `--flow` framed the whole domain, putting the section at three pixels | `--flow` implies a mesh, so it inherited the mesh flag's "fit the domain" behaviour | Only `--mesh` on its own means "show me the domain" |

### Phase 4 — solver

| # | Issue | Root cause | Fix |
|---|---|---|---|
| 29 | **The aerofoil C-grid diverged to NaN** within twenty iterations | Two independent causes. (a) The wake cut is stored as two coincident boundary faces; their fluxes were counted in the mass balance but the pressure equation had no coefficient linking the cells either side, so continuity across the cut was unenforceable. (b) The mesh defect in #26 | (a) `oppositeCell` — one shared notion of "the cell on the far side of this face" — with the matrix product and Gauss-Seidel taught to follow it. The pair stays symmetric, so conjugate gradient still applies. (b) Fixed the mesh |
| 30 | Diverged at the textbook relaxation factors 0.7/0.3 | Not a defect: under-relaxation compensates for the term SIMPLE drops, and how much is needed depends on how badly conditioned the mesh is | Defaults changed to 0.5/0.2. Converging slowly is recoverable, diverging is not; the Cartesian validation cases explicitly opt into the faster pair |
| 31 | Uniform flow "failed to converge" while reporting residuals of 8×10⁻¹⁶ | The normalisation Σ\|a<sub>P</sub>φ<sub>P</sub>\| is **identically zero** for the y-momentum equation of a purely-x flow, so round-off was being divided by nothing | Normalise by Σ\|a<sub>P</sub>\| times a velocity scale shared by both components, which cannot vanish |
| 32 | **A 25% skin-friction error that was the test setup, not the solver** | The flat plate's leading edge was placed exactly at the inlet, where u = U and u = 0 meet at a point. That singularity corrupted the whole boundary layer. The tell: the freestream itself read 1.065 U regardless of domain height, where blockage predicts 1.008 | Added a slip section upstream — standard practice. Freestream went to 1.0006 and c<sub>f</sub> to within 3.4%. Uniform flow and Couette were exact to 10⁻¹⁵ and Poiseuille to 0.06%, so the solver was demonstrably fine; the case should have been suspected far earlier |
| 33 | A gradient test that compared two zeros | `ConvergesForANonLinearField` used x² + y². On a Cartesian mesh the face-centre value of x² *is* its average over the face, so Green-Gauss is exact | Replaced with a trigonometric field, which has genuine truncation error. Now confirms second-order convergence |
| 34 | Stale colour range: every solved value saturated and the pressure field rendered as garish wedges | The range was computed only in `updateFlow`, so it still held the values for the *uniform* initialisation | Extracted `refreshFieldRange`, called wherever the field changes. It looked like a broken solution and was a stale legend |

### Phase 5 — surface quantities

| # | Issue | Root cause | Fix |
|---|---|---|---|
| 35 | **Four lower-surface stations flagged as reversed at α = 10°**, reporting separation on the pressure side of an aerofoil at incidence — which does not happen | The tangent was referenced to the geometric leading edge. At incidence the flow divides on the *lower surface* some way back, and between the nose and that point the fluid legitimately runs forwards to get round it | Reference the tangent to the **computed stagnation point** — where the boundary layer actually starts — and flag the stations either side of it as `nearStagnation` and exclude them, since where the flow divides the sign has no meaning |
| 36 | 80 stations on one surface, 79 on the other; a symmetric section at zero incidence failed its own mirror test | The split walked wall *faces* and started a new surface at the face with the smallest x — but that face belongs to **both** surfaces, so whichever list claimed it was offset from the other by half a cell | Split at the leading-edge **node**, the point the two faces share. Symmetry then held to 2×10⁻⁶ |
| 37 | The stagnation face's tangent pointed the wrong way for the surface owning it | The face sits at the boundary of both surfaces, and the general sign rule resolved it toward the other one | Resolved toward the owning surface |
| 38 | Streamlines died on entering the wake | The cell-walking locator followed `face.neighbour`, which is −1 on a wake-cut face | Walk through `mesh::oppositeCell`. The accompanying test then failed for a *different* reason — it passed a hint on the far side of the section, and the walk would have had to pass through solid body to reach it. That is correct behaviour, so the contract is now documented: *a distant hint is worse than none* |
| 39 | The console filled with one identical separation line per frame | The surface is re-extracted on every solver publish | Log only when the station appears, disappears, or moves by more than 0.005 c |
| 40 | The C<sub>f</sub> plot's zero crossing was invisible | C<sub>f</sub> is singular at the nose — it grows like 1/√x — so scaling to that peak squashes the whole rest of the chord into two pixels, hiding the one feature the plot exists to show | Scale from x/c = 0.05 outwards, clamp out-of-range points to the frame rather than dropping them, say so in the caption, and keep reporting the true extremes in the table. Hiding the data would have been the wrong fix; saying which data set the axis is not |

### Phase 6 — forces

| # | Issue | Root cause | Fix |
|---|---|---|---|
| 41 | Force integration is exposed to catastrophic cancellation | ∮**n** d*s* = 0 for a closed contour, so adding a constant to the pressure changes the force by nothing — mathematically. Numerically, at Re = 500 the reference pressure can be far larger than its variation over the section, so integrating the raw value forms a small answer as the difference of large numbers | Subtract the freestream pressure first, so every term is the size of the answer. Guarded by a test that requires a uniform pressure to produce zero net force at *any* reference level |

### Performance interlude

| # | Issue | Root cause | Fix |
|---|---|---|---|
| 42 | **The window redrew about once a second while solving** — panning, zooming and even dragging a splitter stopped responding | `drawFrame` called `SimpleSolver::iterate()` five times in a row, ~180 ms each on the fine grid | The solver runs on its own thread and publishes complete snapshots under a mutex; the UI moves them out, so the render thread pays a pointer swap. 905 ms → 5.6 ms per frame |
| 43 | A running solve could be left iterating on a freed mesh | `SimpleSolver` holds a bare mesh pointer, and the UI thread is free to regenerate the grid at any moment — changing resolution mid-solve is an obvious thing to do | `MeshState::mesh` became `shared_ptr<const Mesh>`; the worker holds it for as long as it might read from it. A per-worker copy was rejected because rebuilds happen on every nudge of the incidence slider |
| 44 | `--solve` on the command line never started the solver | The worker became the authority on "is it running", and the CLI set the UI's mirror of that flag before any worker existed | The rebuild path treats "the UI wants to run" and "the worker was running" as the same request |
| 45 | The loop free-ran at ~117 fps during a solve, taking a core from it | Vsync normally caps this, but not when the window is hidden or occluded — precisely when spinning is most wasteful and least visible | Wait out the remainder of a 60 Hz interval with `glfwWaitEventsTimeout`, so input still returns early |
| 46 | The 60 fps cap delivered 41 fps | The interval was measured from the *end* of the previous frame, so it was added on top of the frame's own cost | Measure from the start of the previous frame |
| 47 | **The shaded field drew at half scale in a ragged patch** in the middle of the viewport | The offscreen target is sized in framebuffer pixels and the camera scale is in ImGui points; on a Retina display those differ by 2×. The ragged edge was the giveaway: cells were culled against the *camera's* visible bounds while being drawn into a texture covering twice that area, so the cull boundary appeared inside the picture | Carry the scale in framebuffer pixels throughout. The shape of the artefact named the bug faster than reading the code would have |

### Phase 8 — turbulence

| # | Issue | Root cause | Fix |
|---|---|---|---|
| 3 | **No grid-convergence study of the force coefficients** | The coefficients were only ever shown to settle in *iteration*, never in *mesh resolution*, and every number quoted came from a single grid | Run at three grids refining by 1.87 in each direction and fit an observed order. Result: **C<sub>d</sub> and pressure drag converge at order 0.84–0.87**, matching the first-order convection scheme, and pressure drag extrapolates to 0.0019 from 0.0291 on the coarse grid - so 94% of it was discretisation error. **C<sub>l</sub> and friction drag converge at 0.48 and 0.29**, below the formal order, so those two are *not* in the asymptotic range on any grid this project can run and their extrapolations are not trustworthy. That is itself the answer the study was asked for, and it is documented in `README.md` |
| 59 | **The model segfaulted in the application but not in tests** | `KOmegaSST` cached a pointer to the solver's `FaceConditions`. `SimpleSolver` is *movable*, and the application moves it into the worker thread after `initialise` - leaving the model pointing at a moved-from vector. The tests never moved the solver, so they passed. The interface's own comment said models must not hold pointers into solver state that later moves, and the first implementation behind it did exactly that | Boundary conditions travel through `TurbulenceContext` every iteration, like everything else the model needs |
| 58 | **A turbulent solve diverged in its first hundred iterations** | k and omega were initialised to freestream values *everywhere*, including inside the boundary layer where omega should be five or six orders of magnitude larger. The first iterations saw an enormous wall strain rate against a tiny omega, production ran away, and k exploded before the wall condition had been applied once. Reliably fatal at Re = 10⁶ on the medium and fine grids; the Phase 7 cold retry (#54) rescued it, which is how it stayed hidden | Seed omega from the analytic sublayer solution at `initialise`, taking the larger of that and the freestream value. **Improved, not eliminated** - see [#62](#62--the-finest-grid-still-diverges-once-before-settling). The converged answer is unchanged, which is the right outcome for a change to the starting field |
| 60 | **omega was ten times too large in the wall cell, silently disabling SST's blending** | Menter's ω = 60ν/β₁d² is the value imposed at the *wall face*, deliberately over-large as a robust Dirichlet condition where omega is infinite. The first *cell* takes the viscous asymptote ω = 6ν/β₁d². With 60 in the cell, the blending term 500ν/(d²ω) collapses to the constant 500β₁/60 = 0.625, pinning F1 at tanh(0.625⁴) = 0.15 in the sublayer where it should be 1 - so **the model ran its k-epsilon branch hard against the wall, which is exactly what SST exists to prevent**. It still converged and still produced plausible coefficients, which is what made it dangerous | The near-wall cell takes the viscous asymptote blended with the log-layer form, √(ω_vis² + ω_log²). Caught by the F1 test, not by inspection; friction drag moved 8.5% toward flat-plate theory once fixed |
| 61 | k was imposed one cell out into the fluid | The wall value of k was written into the near-wall *cell* as well as onto the wall face. The fluctuations vanish *at the wall* - a condition on the face, which the transport equation already applies - so overwriting the cell killed the turbulence in the very cell the omega wall treatment reads k from | The cell value is solved; only the face is imposed |

### Solver correctness

| # | Issue | Root cause | Fix |
|---|---|---|---|
| 1 | **Zero incidence never converged**: continuity fell to a few times 10⁻⁴ and then oscillated in a band forever, while momentum converged normally. Mesh-independent, unaffected by the non-orthogonal corrector count, and it faded in as incidence grew rather than switching on | The far-field boundary condition was **a discontinuous function of the solution**. A far-field face acts as an inlet or an outlet depending on which way the stream crosses it, and the solver decided that every iteration from the raw sign of the current flux. On a face the flow runs *parallel* to, that flux hovers around zero, so the face flipped between imposing a velocity and imposing a pressure from one iteration to the next — two different discretisations, alternating. At exactly zero incidence that is the entire top and bottom of the C-grid: **222 of 222 far-field faces**. No amount of iterating can settle a problem whose boundary conditions change under it | A dead band a thousandth of the freestream flux wide, computed once from the geometry and the oncoming stream rather than from the current iterate. A face is an outlet only when fluid is unambiguously leaving; far from the body the flow *is* the freestream, so imposing it on a tangential face is both well posed and correct. Continuity now falls monotonically and α = 0 converges in 6,061 iterations to 3.3×10⁻⁷ — where it used to sit at 2.4×10⁻⁴ indefinitely. C<sub>l</sub> and C<sub>m</sub> come out at zero to five decimals. Guarded by a test that fails against the old behaviour |
| 5 | Wall shear was only first-order accurate | τ<sub>w</sub> was the first cell's wall-parallel speed over its distance from the wall. C<sub>f</sub>, the friction drag and the separation station all inherited an error proportional to the first-layer height | The gradient now comes from a parabola fitted through three known points — zero at the wall and the first two cells out — which is second-order accurate on the graded spacing a boundary-layer mesh always has. The second cell is found by geometry (the owner's face whose outward normal points most nearly away from the wall), so it does not depend on the grid being structured. Guarded by a test on a profile *a·d + b·d²*, whose wall gradient is exactly *a*: the one-sided difference is 50% out on it, the parabola within 1% |

### Phase 7 — polars

| # | Issue | Root cause | Fix |
|---|---|---|---|
| 48 | A sweep would have recorded the *previous* angle's forces and skipped straight on | "The solver has stopped" and "the solver has not started yet" are indistinguishable by the same test | A separate `Starting` phase that waits for the solver to actually be running before `Solving` is allowed to look for it stopping — with a frame budget, because silently waiting forever is the worst failure for something that takes minutes anyway |
| 49 | A finished sweep silently started another solve | Restoring the pre-sweep incidence triggers the Phase 6 behaviour where a *converged* run picks itself back up when its freestream changes — right for a nudge of the slider, wrong here | Clear `converged` before restoring the angle. Found by reading my own code while the sweep ran |
| 51 | A descending sweep was refused, so hysteresis could not be checked | `sweepAngles` rejected an end below the start. But continuation between points is exactly the mechanism by which hysteresis would appear if the flow had more than one steady state at an incidence, and the way to look for that is to sweep down and compare with up — which the code made impossible | `step` is now a magnitude and the direction comes from which end is which. Two tests: a descending sweep runs, and up and down over the same range visit exactly the same angles, so the comparison measures the flow rather than the sweep |
| 52 | The sweep sequencing had no automated test | It was a state machine inside the application layer, so exercising it needed a window | Extracted the decision — given the phase, whether the solver is running, and whether it failed, what happens next — into `nextSweepAction` in `cfd_post`, which the sweep now calls. Seven tests, including the one for the bug it exists to prevent: in `Starting` with the solver not yet running the answer must be `Wait`, never `RecordPoint` |
| 57 | **An interrupted sweep lost everything, silently** | The window can be closed from outside the program - by the user, the window manager, or the platform when something else takes the display. Running two windowed instances at once is enough to do it. The main loop simply ended and the solved points went with it: no CSV, no message, and an exit code of 0 that looked like success | On exit with a sweep still running, the points already solved are written out, the interruption is logged and the process exits non-zero. Found by running two sweeps concurrently, which is also worth knowing: **run windowed sweeps one at a time** |
| 56 | **The same sweep gave different answers on different runs** | A race between the solver thread and the renderer. The worker clears its running flag and *then* takes a lock to publish the final field; a reader checking the flag directly can land in between, see "stopped", poll nothing, and conclude the run ended on whatever field it was holding — one publication interval stale. In a sweep that stale field is carried into the next angle as its initial guess, which is enough to change the trajectory: one run diverged at 4° and the next converged from identical inputs | The UI reads "is it still running" from the snapshot rather than from the atomic, so the field and the state always describe the same instant. That is what a snapshot is for. Two consecutive descending sweeps now produce byte-identical CSVs |
| 53 | **Divergence was the only silent outcome** | Convergence and the iteration limit both logged; a blow-up set a flag and said nothing. A run that had destroyed its own field looked, from the log, exactly like one that had simply stopped — which is how #54 stayed hidden until a descending sweep was run | The worker logs a warning naming the iteration and the residuals |
| 54 | **A descending sweep diverged at 4°, recording C<sub>l</sub> = −1.06 and C<sub>d</sub> = 15.7** | Found by the descending sweep that #51 made possible. Continuing *downwards* through the onset of separation is a much harsher perturbation than continuing upwards — the field has to re-attach rather than separate further — and the solve blew up. The diverged field was then carried into the next angle as its initial guess, spreading the damage | A point whose continued solve diverges is retried once from the undisturbed stream; if even that diverges, the next angle starts cold rather than inheriting the wreckage. Continuation stays the default because it is what makes a sweep affordable — it just no longer poisons what follows |
| 55 | "Not converged" conflated two different failures | A run that hit its iteration limit is somewhere near an answer; one that diverged is not near anything. The CSV's `converged,yes/no` column could not tell them apart, so a diverged row looked like a merely-slow one | Replaced with a `status` column carrying `converged`, `iteration-limit` or `diverged` |
| 50 | `0:18:2` would have produced nine angles, not ten | `(18 − 0)/2` can evaluate a hair under 9, and truncating drops the last angle silently — 0 to 16 is a perfectly plausible-looking polar | Round rather than truncate when the remainder is within 10⁻⁹, and compute each angle as `start + i·step` rather than accumulating, so a 181-point sweep still lands exactly on 18 |

---

## Not defects

Recorded because time was spent on them.

**The screenshot harness lied twice.** A long capture came back reporting the
section as NACA 2412 when 0012 was asked for, and another showed the designation
field reading "NACA 0" mid-edit. Neither reproduced. The window is real and takes
real focus, so stray input events reach it; there is exactly one writer to that
buffer besides ImGui. Ten minutes went into suspecting the option plumbing,
which was innocent.

**The far-field split at exactly zero incidence is 322/108, not symmetric.** At
α = 0 the top and bottom straight sections of the outer boundary have normals
exactly (0, ±1), so u·n = 0 — neither inflow nor outflow. The classification
treats those as outflow, which sets a pressure rather than forcing a velocity.
That is the safe choice for a tangential far-field face.

**`maxThicknessPosition` reads 0.297, not 0.300.** The NACA equations are exact;
the point list is a sampling of them, and the stations do not land on the true
maximum. Knowing which reported numbers are exact and which are sampled is a
habit worth having before a solver starts reporting residuals.
