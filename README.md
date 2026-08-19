# CFD-SIMULATOR

A 2D Reynolds-Averaged Navier-Stokes solver for NACA airfoil sections, with an
interactive viewer.

**Status: Phase 10 — full RANS simulation of NACA four-digit sections at
realistic Reynolds numbers, with k-ω SST.**

The application generates NACA four-digit sections, builds a structured C-grid
around them, solves the steady incompressible Navier-Stokes equations on them
with a finite-volume SIMPLE algorithm, and reads the wall quantities back off
the solution: surface pressure, *C<sub>p</sub>*, wall shear stress,
*C<sub>f</sub>*, near-wall velocity, and where the boundary layer separates.
Type a designation such as `NACA 2412`, tick **Generate mesh**, then
**Initialise flow**, then press **Run**.

Lift, drag, pitching moment and their coefficients are then obtained by
integrating that surface solution — pressure and viscous stress, around the
contour — at any angle of attack, and a sweep over a range of incidences
produces a polar and writes it to CSV.

Every point of that polar is a separate converged Navier-Stokes solve. Nothing
is fitted, interpolated between angles, or taken from a lift-curve formula.

Separation is detected from the sign of the computed wall shear, never assumed
or hard-coded. If the solution does not reverse, the application says the
surface is attached.

The solver is **laminar**. There is no turbulence model, so results at the
Reynolds numbers real aerofoils operate at (10⁶ and above) are not physically
meaningful — a steady laminar solution does not exist there. At low Reynolds
numbers the solver is validated against exact solutions to a fraction of a
percent (see [Validation](#validation)).

No comparison has been made with wind-tunnel data or another CFD code.

---

## Requirements

| | |
|---|---|
| Compiler | C++20 (tested with Apple Clang 17) |
| Build system | CMake ≥ 3.24 |
| Graphics | OpenGL 3.2 core profile |

Third-party dependencies are downloaded automatically at configure time and
pinned by SHA-256; no package manager is required. An internet connection is
needed for the first configure only.

## Build

```sh
cmake -S . -B build
cmake --build build
```

Useful options:

```sh
# Headless: core library and tests only, no window system needed.
cmake -S . -B build -DCFD_BUILD_APP=OFF

# Treat compiler warnings as errors (recommended before committing).
cmake -S . -B build -DCFD_WARNINGS_AS_ERRORS=ON

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
```

| Option | Default | Effect |
|---|---|---|
| `CFD_BUILD_APP` | `ON` | Build the GUI shell and the `cfd_sim` executable |
| `CFD_BUILD_TESTS` | `ON` | Build the unit test suite |
| `CFD_WARNINGS_AS_ERRORS` | `OFF` | Add `-Werror` to first-party targets |

The default build type for single-config generators is `RelWithDebInfo`.

## Test

```sh
ctest --test-dir build
```

Add `--output-on-failure` to see diagnostics from failing tests.

## Run

```sh
./build/bin/cfd_sim
```

| Flag | Purpose |
|---|---|
| `-h`, `--help` | Usage summary |
| `-V`, `--version` | Version, build type, compiler |
| `--section NAME` | Load a section at startup, e.g. `--section "NACA 2412"` |
| `--mesh LEVEL` | Mesh at startup: `coarse`, `medium` or `fine` |
| `--flow` | Initialise the flow at startup (implies a mesh) |
| `--field NAME` | Shown scalar: `velocity`, `vx`, `vy`, `pressure`, `cp`, `vorticity`, `k`, `omega`, `mut`, `divergence` |
| `--solve` | Start the solver running at startup (implies `--flow`) |
| `--reynolds N` | Reynolds number based on the chord |
| `--viscosity MU` | Dynamic viscosity in Pa.s; the Reynolds number then follows from it |
| `--intensity I` | Freestream turbulence intensity u′/U |
| `--eddy-ratio R` | Freestream μ_t/μ |
| `--length-scale L` | Turbulent length scale in metres, instead of the ratio |
| `--alpha DEG` | Angle of attack in degrees |
| `--polar A:B:S` | Sweep incidence from A to B in steps of S degrees, write the polar and exit |
| `--polar-csv FILE` | Where the sweep writes its CSV (default `polar.csv`) |
| `--frame-stats N` | Log a frame-time summary (mean and worst) every N frames |
| `--max-frames N` | Stop after N frames; makes a timing run a bounded measurement |
| `--log-level LEVEL` | `trace`, `debug`, `info`, `warn`, `error`, `critical`, `off` |
| `--self-check` | Headless subsystem check, then exit (this is what CTest runs) |
| `--screenshot FILE` | Render a few frames, save the window as a BMP, exit |
| `--screenshot-frames N` | Frames to render first (default 3); a large value with `--solve` captures a converged run |

### Generating a section

Type a four-digit designation into **Section** in the Geometry panel. The
`NACA` prefix and any whitespace are optional, so `NACA 2412`, `naca2412` and
`2412` are equivalent. The shape updates as you type; while the input is
incomplete the last valid section stays on screen and the problem is reported
beneath the field.

The four digits are maximum camber in percent of chord, its position in tenths
of chord, and maximum thickness in percent of chord. So `2412` is 2% camber at
40% chord and 12% thick, while `0012` is symmetric and 12% thick.

You can also set the number of points per surface, the chord length, and
whether the trailing edge uses the standard (slightly blunt) polynomial or the
adjusted closed-trailing-edge form. The closed form is the default because it
is the one the mesher can wrap.

### Generating a mesh

Tick **Generate mesh** in the Mesh panel. A structured C-grid is built around
the section: grid lines wrap the nose like the letter C, then run downstream on
both sides of a cut along the wake.

| Control | Meaning |
|---|---|
| Resolution | `Coarse` / `Medium` / `Fine` — sets surface, wake and normal point counts and the near-wall spacing |
| Upstream / Downstream / Vertical | Domain extent in chord lengths; upstream and vertical are measured from the leading edge, downstream from the trailing edge |
| First layer | Height of the first cell off the wall, as a fraction of chord |
| Grid / Boundaries | Show the interior grid lines and the coloured boundary faces |
| Fit domain | Frame the whole domain rather than the section |

Boundaries are coloured by the condition they will carry: **wall** (white),
**wake cut** (amber), **far field** (blue) and **outlet** (green).

Defaults are 12 chords upstream, 25 downstream and 12 vertically.

| Resolution | Cells | Wall faces | First layer |
|---|---|---|---|
| Coarse | ~8,700 | 158 | 1×10⁻³ c |
| Medium | ~30,500 | 318 | 3×10⁻⁴ c |
| Fine | ~105,000 | 638 | 1×10⁻⁴ c |

A mesh needs a closed trailing edge, since the wake cut has to start from a
single point. Selecting the open trailing edge reports this rather than
producing a broken grid.

When the whole domain is on screen, drawing every grid line is neither fast nor
informative, so the view is decimated and the panel says so ("Drawing every
*n*th grid line"). Zoom in and the full mesh is drawn.

### Initialising a flow

Tick **Initialise flow**. Every cell is filled with the undisturbed stream and
the boundary conditions are applied to every face.

| Input | Meaning |
|---|---|
| Speed | Freestream velocity magnitude, m/s |
| Incidence | Angle of attack in degrees, on a slider spanning ±20°; positive pitches the nose up |
| Density | kg/m³, constant (the flow is treated as incompressible) |
| Reynolds | ρUc/μ — the viscosity is *derived* from this, not set directly |
| Pressure | Reference static pressure, Pa (gauge) |

**Sweeping incidence.** Angle of attack gets a slider rather than a value box,
because it is the one freestream quantity that is swept: an aerofoil is
interesting precisely for how it behaves as the angle changes. The slider spans
±20°, marked at zero, with a button beside it to return there; ctrl-click takes
a typed value.

Changing it rotates the freestream vector, which re-classifies every far-field
face as inflow or outflow, rebuilds the boundary conditions, and feeds through
to the surface pressures, skin friction, separation station and streamlines.
The geometry and the mesh do not move — the section stays put and the stream
arrives at an angle, which is the correct way round: rotating the mesh would
change the discretisation as well as the physics.

Two behaviours make the slider usable rather than merely present:

- **It continues from the field already solved** instead of restarting from the
  undisturbed stream. A converged steady solution does not depend on what it
  was started from, so the answer at 8° is a far better guess for 9° than a
  uniform flow is — and it costs a few hundred iterations instead of a few
  thousand. A regression test pins this down: solving 12° cold and solving it
  from a converged 6° field agree station for station.
- **A converged run resumes itself.** If the solve had finished and you move the
  slider, it starts iterating again towards the new answer. A run you paused
  deliberately stays paused, and **Reset** always returns to the undisturbed
  stream rather than to whatever was carried over.

The Solve panel marks a continued run as such next to the iteration count, so a
case that converged in 200 iterations from a neighbouring solution is not
mistaken for one that converged in 200 from scratch.

The Reynolds number is an input rather than an output because it is the one
dimensionless group that decides the character of the flow: two flows at the
same Re over the same shape are the same flow, whatever the actual speed and
scale. Specifying a real air viscosity instead would pin Re to whatever the
geometry and speed happened to produce.

**Boundary conditions**

| Condition | Imposes | Takes from the interior |
|---|---|---|
| Inlet | velocity | pressure |
| Outlet | pressure | velocity |
| Far field | acts as inlet or outlet per face, by the sign of u·n | the other |
| No-slip wall | velocity = 0, both components | pressure |
| Internal | nothing — the wake cut is interpolated across | both |

Each is colour-coded in the viewport, with the far field split by whether the
stream is entering or leaving that face — the part you cannot work out by
looking at the geometry. The panel reports how many faces ended up carrying
each condition.

You cannot impose everything at a boundary: fixing both velocity and pressure
over-determines the problem. Conversely, if no boundary sets a pressure the
incompressible pressure field is only determined up to a constant, and the
configuration is rejected rather than left to fail later.

**Field views**: velocity magnitude, either velocity component, pressure, or
divergence. Signed quantities use a diverging colour map centred on zero;
unsigned ones use viridis. Both are perceptually uniform, and a rainbow map is
deliberately avoided because it invents boundaries the data does not have.

### Solving

Press **Run** in the Solve panel. The solver iterates a few times per rendered
frame, so the window stays responsive and the flow can be watched developing.
**Step** advances one iteration; **Reset** returns to the initialised field.

| Control | Meaning |
|---|---|
| Relax u / Relax p | Under-relaxation factors for momentum and pressure |
| Convection | `Upwind` (robust, first-order) or `Second-order upwind` |
| Iters/frame | Outer iterations per redraw |

The convergence plot shows log₁₀ of the residuals. Residuals are the only
honest measure of convergence: a field that stops changing while its residuals
sit at 10⁻² has stalled, not converged. A run that hits the iteration limit
says so and states plainly that the field shown is not a solution.

**Under-relaxation.** SIMPLE deliberately drops a term when forming the
pressure correction, so the correction it produces is too large and must be
damped. The defaults (0.5 and 0.2) are more cautious than the textbook 0.7 and
0.3, because the aerofoil C-grid — 75° of non-orthogonality, cells thousands of
times longer than they are thick — diverges at the textbook values. The
Cartesian validation cases use the faster pair.

**What this solver is not.** It is steady and laminar. Pointed at an aerofoil
at Re = 10⁶ it will iterate without converging, because no steady laminar
solution exists there; real flow at that Reynolds number is turbulent. Use a
low `--reynolds` for a physically meaningful laminar result until a turbulence
model is added.

### Surface results

Once a flow exists, the **Surface** panel reads the wall quantities straight
off the field. Nothing here is fitted, assumed or hard-coded.

| Quantity | Definition | How it is obtained |
|---|---|---|
| Surface pressure | *p* at the wall, Pa | the adjacent cell's pressure, extrapolated along the face normal |
| Pressure coefficient | *C<sub>p</sub>* = (*p* − *p*<sub>∞</sub>) / ½ρ*U*∞² | from the above |
| Wall shear stress | *τ*<sub>w</sub> = *μ* ∂*u*<sub>t</sub>/∂*n* | wall-parallel velocity in the first cell, over the wall distance |
| Skin friction | *C<sub>f</sub>* = *τ*<sub>w</sub> / ½ρ*U*∞² | from the above |
| Near-wall speed | \|**u**\| in the first cell | directly |

The two plots are drawn in the conventional way: *C<sub>p</sub>* with an
inverted vertical axis so suction points upwards, *C<sub>f</sub>* with a marked
zero line, both against *x/c*, upper surface in blue and lower in orange.
*C<sub>f</sub>* is scaled from *x/c* = 0.05 outwards, because skin friction is
singular where the boundary layer starts and scaling to that peak would flatten
the whole rest of the chord; the panel's table always reports the true extremes.

**Separation** is *found*, not assumed. Wall shear is signed along the surface
tangent, and the tangent is referenced to the computed stagnation point rather
than to the geometric leading edge — upstream of stagnation the fluid genuinely
runs forward, and calling that "reversed" would report separation where there
is none. A station is separated where the sign flips from positive to negative;
the location is interpolated between the two stations either side. If the
solution does not reverse, the panel says the surface is attached.

In the viewport, the section is coloured by wall shear — green where the
near-wall flow still runs downstream, red where it has reversed, brighter with
larger magnitude — with the separation point ringed and labelled and the
stagnation point marked. **Streamlines** can be traced through the field by
RK4 integration, seeded from a rake upstream of the section; they are the
clearest picture of a separated region, and they cross the wake cut correctly
because the tracer walks the mesh through the cut's face pairs.

### Aerodynamic forces

Once a surface solution exists the **Forces** panel integrates it, live:

| Quantity | Symbol | Formed as |
|---|---|---|
| Lift coefficient | *C<sub>l</sub>* | *L* / (*q c*) |
| Drag coefficient | *C<sub>d</sub>* | *D* / (*q c*) |
| Moment coefficient | *C<sub>m</sub>* | *M* / (*q c*<sup>2</sup>), nose-up positive |
| Efficiency | *L/D* | *C<sub>l</sub>* / *C<sub>d</sub>* |

The fluid touches the section only at its surface, so the entire aerodynamic
force is what happens there: pressure pushing inwards along the normal, and
wall shear dragging along the skin.

**F** = ∮ ( −*p* **n** + *τ*<sub>w</sub> **t** ) d*s*  
*M* = ∮ ( **r** − **r**<sub>ref</sub> ) × ( −*p* **n** + *τ*<sub>w</sub> **t** ) d*s*

The integral runs over the wall faces themselves — each station carries its own
face length — so it is taken on exactly the surface the solver imposed no-slip
on, not on an approximation of it. Freestream pressure is subtracted first: it
integrates to nothing around a closed body, but leaving it in would form a small
answer as the difference of large numbers.

Drag and pressure drag are reported separately from friction drag, which is the
most diagnostic split available: form drag climbing away from skin friction is
what separation looks like as a number.

**Arbitrary angle of attack.** The section and the mesh never move; incidence is
applied by turning the oncoming stream, and lift and drag are defined relative
to that stream. So the body-axis force is rotated by the incidence to get wind
axes — one rotation, and nothing else in the pipeline changes. Rotating the mesh
instead would change the discretisation along with the physics.

**Moment reference.** The quarter chord by convention, and adjustable on a
slider. For a thin section in attached flow the aerodynamic centre sits very
close to *x/c* = 0.25, which makes the moment about it nearly independent of
incidence — a property of the section rather than a number that moves with every
degree of alpha.

Two sparklines trace *C<sub>l</sub>* and *C<sub>d</sub>* against extraction
number. A coefficient still drifting once the residuals have flattened has not
converged, whatever the residuals say, and only a trace shows that.

### Angle-of-attack sweeps and polars

One solve gives one number at one incidence, which is almost never the question.
What a section is chosen by is how those numbers behave as incidence changes:
how steeply lift builds, where the lift curve stops being straight, how fast
drag grows once it does, and where the best lift-to-drag ratio sits. The
**Polar** panel sweeps a range and produces those curves.

Set a start, an end and a step; the panel says how many solves that implies
before you commit to them. **Run sweep** works through the angles, showing the
flow developing at each one, with a progress bar and a live point count. It can
be stopped at any time and keeps the points already gathered.

Four curves are drawn against angle of attack: *C<sub>l</sub>*, *C<sub>d</sub>*,
*C<sub>m</sub>* and *L/D*. The panel reports the best lift-to-drag ratio and the
incidence it occurs at — the single most-asked question of a polar — and warns
if any point failed to converge.

**Every point is a real solve.** The sweep sets the incidence and hands the
solver exactly the work it would do for a single point. There is no fitting, no
interpolation between angles and no empirical lift-curve slope anywhere in it.
Where a solve fails to converge, its row says so rather than quietly reporting
whatever the iteration was holding.

**Continuation between points** is on by default: each angle starts from the
previous angle's converged field instead of the undisturbed stream. A degree of
incidence is a small perturbation, so this is dramatically cheaper — in the
sweep below the first point needed 5,000 iterations and the last needed 827 —
and it reaches the same answer, which the surface tests pin down directly. It
can be switched off, because continuation is also how hysteresis would be
introduced if the flow ever had more than one steady state at an incidence.

Unattended, from the command line:

```sh
cfd_sim --section "NACA 0012" --mesh coarse --reynolds 500 \
        --polar 0:18:2 --polar-csv naca0012_re500.csv
```

The window closes when the sweep finishes.

**CSV output.** The conditions go in as `#` comment lines above the header,
which pandas, R, gnuplot and most spreadsheets can be told to skip, and which
keep the file self-describing when it is opened a month later:

```
# cfd_simulator aerodynamic polar
# section,NACA 0012
# mesh,Coarse
# reynolds,500
# freestream_speed_mps,50
# chord_m,1
# moment_reference_xc,0.25
# continued_between_points,yes
# every row is a separate Navier-Stokes solve; nothing is interpolated
alpha_deg,cl,cd,cd_pressure,cd_friction,cm,l_over_d,separation_upper_xc,separation_lower_xc,converged,iterations,continuity_residual
```

The sweeps above are checked in at
[`examples/naca0012_re500_polar.csv`](examples/naca0012_re500_polar.csv) (laminar)
and [`examples/naca0012_re1e6_sst_polar.csv`](examples/naca0012_re1e6_sst_polar.csv)
(k-omega SST at Re = 10⁶).

Separation columns are left empty where the surface stayed attached, rather than
carrying a −1 that invites being plotted. `status` and `iterations` travel with
every row: a polar that silently mixes converged and unconverged points is worse
than no polar. `status` is `converged`, `iteration-limit` or `diverged` — a run
that ran out of iterations ended somewhere near an answer and one that blew up
did not, and collapsing both to "not converged" loses the distinction that
decides whether the row is worth looking at.

**Sweeping downwards.** `--polar 18:0:2` walks the range in reverse. That is not
a convenience: continuing each point from the previous one is exactly how
hysteresis would appear if the flow had more than one steady state at an
incidence, and the only way to look for it is to sweep down and compare with
sweeping up. Doing so on NACA 0012 at Re = 500 found none — the two directions
agree to better than 0.6% at every angle, which is the convergence-tolerance
scatter rather than a physical difference.

It also found two real defects, which is the usual reward for running a check
you expect to pass: see [`ISSUES.md`](ISSUES.md) #54 and #56.

**A full sweep**, NACA 0012 at Re = 500 on the coarse C-grid, produced by
`--polar 0:18:2` — ten points, about twelve minutes unattended:

| α | C<sub>l</sub> | C<sub>d</sub> | C<sub>d</sub> pressure | C<sub>d</sub> friction | C<sub>m</sub> c/4 | L/D | Separation x/c | Iterations |
|---|---|---|---|---|---|---|---|---|
| 0° | +0.0000 | 0.1859 | 0.0514 | 0.1345 | −0.0000 | — | attached | 5000 *(not converged)* |
| 2° | +0.1112 | 0.1880 | 0.0542 | 0.1338 | +0.0023 | 0.59 | attached | 2822 |
| 4° | +0.2211 | 0.1943 | 0.0625 | 0.1318 | +0.0038 | 1.14 | attached | 1652 |
| 6° | +0.3259 | 0.2046 | 0.0762 | 0.1284 | +0.0044 | 1.59 | 0.919 | 1022 |
| 8° | +0.4225 | 0.2192 | 0.0952 | 0.1240 | +0.0040 | 1.93 | 0.687 | 827 |
| 10° | +0.5083 | 0.2381 | 0.1189 | 0.1191 | +0.0026 | 2.14 | 0.514 | 763 |
| 12° | +0.5816 | 0.2608 | 0.1467 | 0.1141 | −0.0001 | 2.23 | 0.387 | 775 |
| 14° | +0.6428 | 0.2871 | 0.1779 | 0.1092 | −0.0040 | **2.24** | 0.295 | 816 |
| 16° | +0.6965 | 0.3172 | 0.2126 | 0.1046 | −0.0098 | 2.20 | 0.228 | 908 |
| 18° | +0.7439 | 0.3507 | 0.2505 | 0.1002 | −0.0172 | 2.12 | 0.178 | 926 |

Read across it, five things are happening at once, and every one of them is a
consequence of the same event — the separated region marching up the suction
side from the trailing edge to within 0.18c of the nose:

- **The lift-curve slope decays monotonically.** Per 2° step: 0.111, 0.110,
  0.105, 0.097, 0.086, 0.073, 0.061, 0.054, 0.047. That is progressive stall,
  and no curve fit produced it.
- **Pressure drag grows five-fold**, 0.051 → 0.251, and overtakes friction drag
  between 8° and 10°.
- **Friction drag falls steadily**, 0.134 → 0.100: separated flow drags on the
  skin far less than attached flow.
- **L/D peaks at 14°** and turns over — the design point, found rather than
  assumed.
- **C<sub>m</sub> about the quarter chord stays within 0.004 of zero while the
  flow is largely attached** and drifts nose-down only once most of the suction
  side is separated.

Against the individually-solved Phase 6 values, the sweep agrees to within about
half a percent (C<sub>l</sub> at 4°, 8° and 12°: 0.38%, 0.57%, 0.15%), the
difference being that each point stops at its own convergence tolerance from a
different starting field.

Continuation is what makes this practical: the first point needs 5,000
iterations — it is the zero-incidence case that does not converge — and every
point after the second settles in 760 to 930.

### Turbulence

Everything up to Phase 7 was laminar, which meant every result sat at Re = 500 —
three orders of magnitude below where aerofoils operate — because no *steady
laminar* solution exists up there. The solver is now Reynolds-averaged.

**The closure problem.** Averaging the Navier-Stokes equations is linear in
every term but convection, which is quadratic. Averaging it leaves an extra
piece, ∇·(ρ⟨**u'u'**⟩) — the **Reynolds stresses** — and nothing determines
them. Deriving a transport equation for ⟨u'u'⟩ introduces ⟨u'u'u'⟩, and so on
without end. The averaged equations are not a closed system, and no manipulation
makes them one. A turbulence model is a *guess* at those stresses in terms of
quantities the mean flow already has.

**The interface.** `TurbulenceModel` mentions neither k, ω nor ε: a model owns
whatever equations it needs and exposes only an eddy viscosity, its own
residuals, and the y+ it achieved. The coupling to the rest of the solver is one
line — `field_.viscosity[c] = molecular[c] + eddy[c]` — because the momentum
assembly already read a per-cell effective viscosity. Nothing in it was changed
for this phase.

`LaminarModel` is the control: a closure that supplies nothing. Solving with it
attached is **bit-identical** to solving with no model at all, which is the test
that the solver depends on the abstraction rather than on which model is
present.

**k-ω SST** is the first concrete model — the 2003 form with standard constants,
F1/F2 blending, cross-diffusion, the shear-stress limiter, the production
limiter, and Menter's low-Reynolds wall treatment. There is no wall function, so
it needs y+ of order 1; the model reports the y+ it achieved so that being used
outside its range is visible rather than silent.

**The exact equations implemented, and every simplification made, are written
out in [`include/cfd/solver/KOmegaSST.hpp`](include/cfd/solver/KOmegaSST.hpp)** —
the equations of the code, not of the paper. The five documented simplifications
are: the −⅔ρk part of the Boussinesq stress absorbed into pressure; the ω
production unlimited while the k production is limited; first-order upwind
convection for k and ω; no transition model; and no compressibility, buoyancy or
curvature corrections.

**Independent verification.** Decaying homogeneous turbulence is the one
configuration where these equations have a closed-form solution — no walls, no
shear, so they collapse to ODEs in x/U giving ω = ω₀/(1+βω₀x/U) and
k = k₀(1+βω₀x/U)^(−β*/β). The solver reproduces it to **1.5% in ω and 1.7% in k**
over a decade of decay, which tests the destruction terms and their balance
against something other than themselves.

**Monitoring.** k, ω and μ_t ranges are reported in the periodic log line and the
Solve panel, along with y+ and μ_t/μ. The panel warns if k reaches its floor
anywhere, since that means positivity is being maintained by clipping rather
than by the discretisation.

```sh
cfd_sim --section "NACA 0012" --mesh fine --reynolds 1e6 \
        --turbulence sst --first-layer 2.5e-5 --polar 0:18:2
```

`--first-layer` matters: y+ ≈ 1 at Re = 10⁶ needs about 2.5×10⁻⁵ chords, far
finer than any resolution preset. At that setting the runs below achieved
y+ = 1.2–1.3 and μ_t/μ up to ~140.

### Viewport controls

| Input | Action |
|---|---|
| Left or middle drag | Pan |
| Scroll wheel | Zoom about the cursor |
| `F` | Reset view |

Panel dividers are draggable; panels can be toggled from the **View** menu.

## Layout

```
CFD-SIMULATOR/
├── CMakeLists.txt              Top-level build: options, warnings, subdirectories
├── cmake/
│   ├── CompilerWarnings.cmake  Strict warning set for first-party targets
│   ├── Dependencies.cmake      Hash-pinned FetchContent declarations
│   └── Version.hpp.in          Template for the generated version header
├── include/cfd/                Public headers
│   ├── core/                   BuildInfo, Error/Result, Log, Vec2
│   ├── geom/                   Naca4, Airfoil
│   ├── mesh/                   Mesh, CGrid
│   ├── flow/                   Freestream, FlowField, BoundaryConditions
│   ├── solver/                 LinearSystem, Gradient, SimpleSolver
│   ├── post/                   SurfaceData, Streamlines
│   └── app/                    Application, Camera2D
├── src/
│   ├── main.cpp                CLI parsing and startup
│   ├── core/                   cfd_core — no GUI, no CFD, no platform code
│   ├── geom/                   cfd_geometry — NACA equations and discretisation
│   ├── mesh/                   cfd_mesh — domain, C-grid, finite-volume metrics
│   ├── flow/                   cfd_flow — state, boundary conditions, divergence
│   ├── solver/                 cfd_solver — discretisation, linear algebra, SIMPLE
│   ├── post/                   cfd_post — surface quantities, separation, streamlines
│   └── app/                    cfd_app — GLFW, OpenGL and Dear ImGui live here only
└── tests/                      GoogleTest suite, registered with CTest
```

### Module boundaries

```
cfd_sim ─▶ cfd_app ─▶ cfd_solver ─▶ cfd_flow ─▶ cfd_mesh ─▶ cfd_geometry ─▶ cfd_core
                │                      ▲
                ├──▶ cfd_post ─────────┘
                │
                └──▶ Dear ImGui, GLFW, OpenGL
```

`cfd_core` depends on nothing but the standard library; each layer above adds
exactly one concern. `cfd_app` is the only module that knows GLFW, OpenGL or
Dear ImGui exist. Solver code added in later phases sits at the mesh level or
below, so the numerics remain buildable and testable on a machine with no
display — which is what `-DCFD_BUILD_APP=OFF` verifies.

## Validation

The geometry tests check three different kinds of property:

- **Exact construction identities** — the midpoint of corresponding upper and
  lower surface points lies on the camber line; the line joining them is
  perpendicular to the camber line; a symmetric section's surfaces are exact
  mirror images.
- **Agreement with the designation** — thickness, camber and their chordwise
  positions are measured back out of the generated points and compared with
  what the digits specify. The camber line reaches exactly `m` at exactly `p`.
- **Agreement with calculus** — the area enclosed by the discretised contour,
  computed with the shoelace formula, is compared against the analytic integral
  of the thickness polynomial, including a refinement study confirming the
  error falls quadratically as points are added.

The thickness polynomial is also checked against the tabulated NACA 0012
ordinate of 0.06002c at 30% chord.

The mesh tests check the properties a finite-volume solver depends on:

- **Connectivity** — Euler's formula `V − E + F = 2`; every interior face
  referenced by exactly two cells and every boundary face by one; owners,
  neighbours and node indices all in range and consistent.
- **Validity** — every cell area strictly positive, no inverted cells at any
  resolution or on any section.
- **Metrics** — face normals are unit vectors pointing out of their owner; the
  outward area vectors of every cell sum to zero; cell areas recovered from
  their faces by the divergence theorem match the shoelace areas.
- **Boundaries** — wall, wake cut, far field and outlet faces are identified
  and counted, sit where the options say they should, and the two sides of the
  wake cut are matched, coincident and oppositely oriented.
- **Refinement** — cell count, wall resolution and near-wall spacing all move
  monotonically with resolution, and the total domain area converges to the
  analytic area of the domain.

The flow tests check the freestream derivations, the field initialisation, and
what each boundary condition actually imposes — that wall faces really are at
zero velocity, that outlets carry the reference pressure, that the far field
splits into inflow and outflow, and that wake-cut faces are interpolated rather
than clamped.

The sharpest of them is the divergence check. A uniform velocity has zero
divergence analytically, and discretely the net flux out of a cell is
`u · Σ nA = u · 0 = 0`, because the outward area vectors of a closed cell sum
to zero. So one assertion exercises the mesh metrics, the face interpolation
and the flux sign convention at once. With the no-slip wall switched back on
the balance must fail, and only in the cells touching the wall — which is not a
defect but the reason a solver is needed.

### Solver validation

The solver is checked against flows whose answers are known in closed form,
which is what turns "does this look like a flow" into a number.

| Case | What it tests | Result |
|---|---|---|
| Uniform flow | Consistency of convection, pressure gradient and boundary conditions | exact to 1×10⁻¹⁵ |
| Couette | Diffusion and the no-slip wall, with convection identically zero | exact to 8×10⁻¹⁵ |
| Poiseuille | Diffusion against a pressure gradient | profile within **0.06%**, dp/dx within **0.10%** of −8μu_max/H² |
| Blasius flat plate | A real convection–diffusion balance | skin friction within **1.6–3.7%** of 0.664/√Re_x |

Mass conservation is demonstrated throughout: the global imbalance closes to
5×10⁻¹⁶ of the through-flow, and the maximum cell divergence falls to 10⁻¹¹.

The C-grid case is guarded by a regression test that runs the solver on the
aerofoil and requires the residuals to fall and the flow to stay physically
plausible — that test exists because getting it stable forced two real fixes,
recorded in `JOURNAL.md`.

### Surface and separation validation

Surface extraction is checked two ways. Against constructed fields, where the
answer is known exactly: a uniform pressure gives *C<sub>p</sub>* = (p − p∞)/q
at every station, a linear wall-normal velocity profile gives the wall shear
its own analytic gradient predicts, and a reversed profile flags every station
as reversed. And against solved fields, where the answer is known by symmetry
or by physics:

| Case | Expected | Result |
|---|---|---|
| NACA 0012, α = 0° | Upper and lower distributions are mirror images; nothing separates | matched to 10⁻³; zero reversed stations |
| NACA 0012, α = 10° | Stagnation moves aft along the lower surface; the suction side separates and the pressure side does not | stagnation at x/c = 0.005, separation on the upper surface only |
| NACA 0012, α = 12° cold vs. continued from 6° | The converged solution does not depend on the starting field | Cp and Cf agree to 5×10⁻³ per station; separation and stagnation to 0.02 c |

Run at Re = 500 on the coarse C-grid, separation moves forward as incidence
increases, which is the behaviour that matters:

| Incidence | Upper-surface separation |
|---|---|
| 0°, 2°, 4°, 6° | attached |
| 8° | x/c = 0.69 |
| 12° | x/c = 0.39 |

None of that is imposed anywhere; it falls out of the sign of the computed wall
shear. The plotted *C<sub>f</sub>* crosses zero at the same station the panel
reports, which is the check that the number and the picture agree.

No comparison has been made against wind-tunnel data or another CFD code.

### Known limitation: zero incidence converges poorly

At exactly α = 0 the continuity residual falls to a few times 10⁻⁴ and then
oscillates in a band rather than continuing down, while both momentum residuals
keep falling monotonically. It is not resolution: coarse and medium meshes
behave the same way. It is not the non-orthogonal correction: raising the
corrector count from 1 to 3 does not help. Cases at incidence converge normally
— α = 2° in 3,322 iterations, α = 4° in 1,837, α = 8° in 1,208, α = 12° in
1,131, all to 10⁻⁶ — and the trouble fades in rather than switching on: α = 0.5°
reached 3.8×10⁻⁷ in continuity and missed the tolerance only on momentum, at
1.5×10⁻⁶.

The fields produced at α = 0 are still symmetric, attached and free of
checkerboarding, and the residual band corresponds to a mass imbalance of
around 0.02–0.1% of the inflow. It is a convergence defect, not a wrong answer,
and it is unresolved. `JOURNAL.md` §6 records what was ruled out.

### Polar validation

Seventeen tests cover the parts of a sweep that can be checked without a window
— which are the parts where a mistake would be silent:

| Check | Why it matters |
|---|---|
| 0→18 step 2 gives exactly ten angles, ending on 18 | `(18−0)/2` can evaluate a hair under 9; truncating would silently drop the last angle, and 0→16 is a perfectly plausible-looking polar |
| 0→18 step 0.1 gives 181 angles, ending exactly on 18 | angles are computed as `start + i·step`, never accumulated |
| 0→5 step 2 stops at 4 | a step that does not divide the range must not overshoot |
| Non-positive step, end below start, absurd point counts | refused rather than started: `0:20:0.001` is 20,001 solves |
| Best L/D ignores unconverged points | an unconverged point is not a design point |
| Every CSV row has exactly the header's twelve fields | including rows whose separation columns are empty |
| Unconverged points are marked in the file | the row admits it |
| Written file reads back identical | no encoding surprise |

The sweep's own state machine is driven by the UI and has no automated test —
it needs a window. It has been run end to end repeatedly, and its coefficients
agree with the individually-solved values in the table above to about half a
percent, the difference being that each point stops at its own convergence
tolerance from a different starting field.

### Force validation

The exact checks impose a field whose force is known in closed form and require
the integral to reproduce it to round-off:

| Check | Why it is exact | Result |
|---|---|---|
| Uniform pressure exerts no net force | ∮ **n** d*s* = 0 for a closed contour | 10⁻¹² |
| …at any reference level | the constant cancels | 10⁻⁹ |
| Still fluid exerts no friction | no velocity gradient, no shear | 10⁻¹⁴ |
| Pressure + friction = total | the split must be a split | 10⁻¹² |
| Wind axes preserve force magnitude | a rotation cannot change a length | 10⁻¹² |
| Moving the reference by **d** shifts the moment by **d** × **F** | the definition of a moment | 10⁻¹² |

The first is the sharpest instrument available on this code: any error in a
normal, a face length or a sign shows up there and nowhere else.

The solved checks run the real thing:

| Case | Expected | Result |
|---|---|---|
| NACA 0012, α = 0° | no lift, no moment, positive drag | C<sub>l</sub> = 4×10⁻⁵, |C<sub>m</sub>| < 10⁻⁵ |
| α = ±8° | lift reverses exactly with the mirror image | C<sub>l</sub> equal and opposite |
| α = 2° → 8° | more incidence, more lift | C<sub>l</sub> 0.111 → 0.425 |
| 700 vs 1400 iterations | the answer settles as the solve does | within 2×10⁻² |

**Sweeping incidence**, NACA 0012 at Re = 500 on the coarse C-grid:

| α | C<sub>l</sub> | C<sub>d</sub> | C<sub>d</sub> pressure | C<sub>d</sub> friction | C<sub>m</sub> c/4 | L/D |
|---|---|---|---|---|---|---|
| 0° | +0.00004 | 0.1859 | 0.0514 | 0.1345 | −0.0000 | — |
| 2° | +0.1111 | 0.1881 | 0.0542 | 0.1339 | +0.0023 | 0.59 |
| 4° | +0.2220 | 0.1949 | 0.0627 | 0.1322 | +0.0037 | 1.14 |
| 8° | +0.4249 | 0.2201 | 0.0956 | 0.1245 | +0.0039 | 1.93 |
| 12° | +0.5807 | 0.2608 | 0.1466 | 0.1142 | +0.0001 | 2.23 |

Four things in that table are worth reading, none of which is asserted anywhere
in the code:

- **The lift-curve slope falls off.** 0.0555 per degree from 0° to 4°, 0.039
  from 8° to 12°. That is the onset of stall, and it tracks separation moving
  forward from x/c = 0.69 to 0.39 over the same range.
- **Pressure drag nearly triples** while separation spreads, 0.051 → 0.147.
- **Friction drag falls** as it does, 0.134 → 0.114: separated flow drags on the
  skin far less than attached flow.
- **C<sub>m</sub> about the quarter chord stays within 0.004 of zero** at every
  incidence. For a symmetric section the aerodynamic centre is at the quarter
  chord, so that is precisely what it should do — an independent check on the
  moment integration that no single-incidence test could give.

One external cross-check, not a test because an aerofoil is not a flat plate:
laminar flat-plate theory gives a total skin-friction coefficient of
2 × 1.328/√Re = 0.119 at Re = 500. The computed friction drag at zero incidence
is 0.134, about 13% higher — the direction and size expected for a 12% thick
section with 2% more wetted area and favourable-gradient acceleration over the
forward half.

The lift-curve slope is roughly half the thin-aerofoil value of 2π per radian.
That is expected at Re = 500, where the boundary layer is a substantial
fraction of the section thickness and decambers it, and it is a reminder that
these are laminar low-Reynolds-number answers, not aerofoil-handbook ones.

## Performance

The solver runs on a thread of its own. One SIMPLE outer iteration on the fine
C-grid — 105,410 cells, 211,777 faces — takes around 180 ms, and running five of
those inside the frame callback redrew the window roughly once a second: panning,
zooming and even dragging a splitter stopped responding exactly when the
application was working hardest.

Frame times while a solve is running, measured with `--frame-stats`:

| Case | Before | After |
|---|---|---|
| Coarse (8,658 cells), solving | 80.8 ms (12 fps) | **0.47 ms** |
| Fine (105,410 cells), solving | 905 ms (1 fps) | **1.76 ms** |
| Fine, rendering only | 11.5 ms | **3.74 ms** |

Four changes, in the order they mattered:

1. **The solver moved off the render thread.** It iterates on its own copy of
   the field and publishes complete snapshots under a mutex; the UI moves them
   out, so the render thread pays a pointer swap rather than a copy. The mesh
   became a `shared_ptr<const Mesh>` so that regenerating the grid cannot pull
   it out from under a running solve.
2. **Solve mode is capped at 60 fps.** With the solver on its own thread,
   redrawing faster buys nothing and takes a core away from the solve. Vsync
   normally imposes a similar limit but does not when the window is hidden or
   occluded — which is when spinning is both most wasteful and least visible.
3. **The shaded field is cached in a texture.** It was two triangles per cell
   re-emitted every frame — 210,820 triangles at the fine resolution — to
   produce an identical picture. It is now drawn once into an offscreen target
   and blitted as a single quad, rebuilt only when the mesh, the field values,
   the scalar shown, the colour range, the camera or the viewport changes.
4. **Grid lines are cached and streamline tracing is throttled.** The projected
   grid lines are rebuilt only when the mesh or the view changes. Streamlines
   integrate thousands of RK4 steps and the field arrives from the solver many
   times a second, so while a solve runs they are refreshed on a timer and
   brought fully up to date the moment it stops.

The solve itself is unchanged and produces bit-identical results: NACA 0012 at
Re = 500 and α = 8° still converges in 1,208 iterations to a continuity residual
of 9.968×10⁻⁷ with separation at x/c = 0.6923.

The panel reports whether the field image was redrawn or reused, because a
cached image that is silently wrong and one that is correctly cached look
identical.

### Grid convergence and external validation

The project's first comparison against values from outside the repository, and
its first proper grid-convergence study. Both come from the same runs: NACA
0012, α = 4°, Re = 10⁶, k-ω SST, on three grids refining by about 1.87 in each
direction. The first-layer height is held fixed at 2.5×10⁻⁵ c across all three
deliberately — y+ must stay of order 1 for the wall treatment, so only the
surface, wake and normal point counts refine. All three achieved y+ = 1.2–1.3.

| Quantity | Coarse (8,658) | Medium (30,530) | Fine (105,410) | Observed order | Richardson extrapolation |
|---|---|---|---|---|---|
| C<sub>l</sub> | 0.3572 | 0.3842 | 0.4042 | **0.48** | 0.4619 |
| C<sub>d</sub> | 0.0366 | 0.0260 | 0.0198 | 0.87 | 0.0113 |
| C<sub>d</sub> pressure | 0.0291 | 0.0180 | 0.0114 | 0.84 | 0.0019 |
| C<sub>d</sub> friction | 0.0075 | 0.0080 | 0.0084 | **0.29** | 0.0105 |

**Read the order column before the extrapolation column.** Drag and pressure
drag converge at order ≈0.85, which is what the first-order upwind convection
scheme predicts — those two are behaving as the discretisation says they should,
and their extrapolations are meaningful. Lift and friction drag converge at 0.48
and 0.29, well *below* the formal order, which means they are **not in the
asymptotic range on these grids**. A Richardson extrapolation from a low apparent
order inflates the correction badly, so those two extrapolated figures are
reported for completeness and should not be treated as converged values.

**How close the solver is.**

| | Fine grid | Extrapolated | Reference | Verdict |
|---|---|---|---|---|
| C<sub>l</sub> at 4° | 0.4042 | 0.4619 | ≈0.42–0.44 (thin-aerofoil slope 2π/rad = 0.110/deg; measured NACA 0012 slopes are 0.105–0.11/deg) | the reference sits **between** the two — bracketed, not pinned |
| C<sub>d</sub> friction | 0.0084 | 0.0105 | 0.0093 (flat plate, 0.074/Re<sup>0.2</sup>, both sides) | again **bracketed** |
| C<sub>d</sub> total | 0.0198 | 0.0113 | ≈0.008–0.012 for this section at this Reynolds number, upper end for fully turbulent flow from the leading edge | extrapolation **inside the range**; the fine grid is 65% high |
| C<sub>d</sub> pressure | 0.0114 | 0.0019 | ≈0 for an attached section at 4° | **correct in magnitude** |

The pressure-drag result is the firmest. On the coarse grid it was 0.0291 —
larger than the whole real drag — and it extrapolates to 0.0019 at a credible
order. **94% of it is discretisation error**, not modelling error: numerical
diffusion from the first-order scheme thickens the boundary layer and wake, and
a thicker wake is form drag.

**And where it is not close.** The extrapolations for drag agree; the grids
themselves do not, and for lift nothing agrees to better than being bracketed.
The fine grid — 105,410 cells, the largest this project runs — is 6% low in
C<sub>l</sub> and 65% high in C<sub>d</sub>. With first-order convection the
error falls as h<sup>0.87</sup> at best, so reaching 5% in drag would need
roughly a 20-fold refinement in each direction. **The solver converges towards
the right answer at a resolution it cannot reach.** The fix is a
higher-order convection scheme, not a bigger mesh: second-order upwind exists in
the code and does not converge on these cases (continuity stalls around 0.2), so
its results are not quoted. Making it work is the highest-value numerical work
outstanding.

Three further caveats, stated because they bear on how much weight the table
carries:

- The comparison is against standard reference *values and ranges* for NACA 0012
  — thin-aerofoil theory, flat-plate skin friction, and the accepted drag band
  for this section at this Reynolds number. **No wind-tunnel dataset has been
  digitised into this repository**, so this is not a point-by-point comparison
  against measured data.
- The medium point stopped on its iteration limit at a continuity residual of
  4.7×10⁻⁶ rather than reaching 10⁻⁶; coarse and fine both converged. Its forces
  were stable to well under a percent over the last 2,000 iterations, so it is
  used in the fit, but it is the weakest of the three points and the fitted
  orders are sensitive to it.
- An earlier version of this table, produced before the wall-treatment bug in
  [`ISSUES.md`](ISSUES.md) #60 was found, agreed *better* with the references —
  C<sub>l</sub> within 2% — while the model was silently running its k-ε branch
  against the wall. Better agreement from a wrong model is a caution worth
  recording: the corrected solver reports a less flattering result, and that is
  the one to believe.

### The polar at Re = 10⁶

`--polar 0:18:2` on the medium grid with k-ω SST, ten points, nine converged to
10⁻⁶:

| α | C<sub>l</sub> | C<sub>d</sub> | C<sub>d</sub> pressure | C<sub>d</sub> friction | C<sub>m</sub> c/4 | L/D |
|---|---|---|---|---|---|---|
| 0° | −0.0008 | 0.0205 | 0.0120 | 0.0084 | +0.0001 | — |
| 2° | 0.1953 | 0.0219 | 0.0135 | 0.0083 | +0.0019 | 8.9 |
| 4° | 0.3839 | 0.0260 | 0.0180 | 0.0080 | +0.0048 | 14.8 |
| 6° | 0.5545 | 0.0334 | 0.0259 | 0.0076 | +0.0090 | **16.6** |
| 8° | 0.7067 | 0.0440 | 0.0370 | 0.0070 | +0.0138 | 16.0 |
| 10° | 0.8280 | 0.0584 | 0.0520 | 0.0064 | +0.0182 | 14.2 |
| 12° | 0.9084 | 0.0782 | 0.0725 | 0.0057 | +0.0172 | 11.6 |
| 14° | **0.9350** | 0.1077 | 0.1029 | 0.0049 | +0.0040 | 8.7 |
| 16° | 0.9105 | 0.1510 | 0.1470 | 0.0041 | −0.0210 | 6.0 |
| 18° | 0.8615 | 0.2086 | 0.2052 | 0.0034 | −0.0506 | 4.1 |

**The section stalls.** Lift rises almost linearly to a maximum of 0.935 at 14°
and then falls — 0.911 at 16°, 0.862 at 18°. That is a genuine C<sub>l,max</sub>
and post-stall lift loss, and nothing in the code knows it should happen: it
falls out of the turbulence model letting the boundary layer separate under an
adverse gradient. The laminar solver could never produce it, because at Re = 500
the flow separates almost immediately and lift never gets the chance to build.

Four things in that table are physics, none of them asserted anywhere:

- **C<sub>l</sub> = −0.0008 at zero incidence** on a symmetric section.
- **The lift curve bends over and breaks** at 14°.
- **C<sub>m</sub> reverses sign at the stall**, +0.017 at 12° to −0.021 at 16°:
  the nose-down pitch break that a stalling section is known for, as the centre
  of pressure moves aft when the suction peak collapses.
- **Friction drag falls monotonically** while pressure drag grows by a factor of
  seventeen — the separated region spreading forward, exactly the trade seen in
  the laminar polars but now at a realistic Reynolds number.

Against published values for NACA 0012 at this Reynolds number:

| | Computed | Reference | |
|---|---|---|---|
| Lift-curve slope near 0° | 0.098/deg | 0.105–0.11/deg | ~8–11% low |
| Stall angle | 14° | ≈14–16° | **in range** |
| C<sub>l,max</sub> | 0.935 | ≈1.1–1.3 | 15–30% low |
| C<sub>d</sub> at 0° | 0.0205 | ≈0.008 | 2.5× high |

The shape is right and the magnitudes are not, in the direction the grid study
predicts: this is the medium grid, which that study shows is 65% high in drag
and short of the asymptotic range in lift. The same numerical diffusion that
inflates drag also thickens the boundary layer and caps peak lift, so a low
C<sub>l,max</sub> and a high C<sub>d</sub> are one error, not two.

### Running a RANS case

Everything the simulation takes as input, and where it is set:

| Input | UI | Command line |
|---|---|---|
| Section (any valid NACA four-digit) | Geometry panel | `--section "NACA 4412"` |
| Angle of attack | Flow panel slider, ±20° | `--alpha 4` |
| Freestream speed | Flow panel | — |
| Density | Flow panel | — |
| Reynolds number | Flow panel | `--reynolds 1e6` |
| Dynamic viscosity | Flow panel (0 = derive from Re) | `--viscosity 1.8e-5` |
| Turbulence intensity | Flow panel | `--intensity 0.01` |
| Turbulence length scale | Flow panel (0 = use μ_t/μ) | `--length-scale 0.01` |
| Freestream μ_t/μ | Flow panel | `--eddy-ratio 5` |
| Closure | Solve panel | `--turbulence sst` |
| Wall spacing | Mesh panel | `--first-layer 2.5e-5` |

**Reynolds number and viscosity are two ways of saying one thing.** By default
the Reynolds number is the input and viscosity follows, which is how aerofoil
work is normally posed. Giving a viscosity inverts that: μ is taken as stated
and the Reynolds number becomes the derived quantity, for anyone modelling a
specific fluid at a specific speed.

**Turbulence intensity and length scale likewise.** k comes from the intensity,
k = 1.5(IU)², and ω from *either* a length scale, ω = √k/(β*^¼L), *or* an
eddy-viscosity ratio. Downstream of a grid or screen the eddy size is the
quantity anyone actually knows; for a clean tunnel there is no such length and a
small μ_t/μ is the honest way to say the freestream is not doing the mixing.

> **Changing the Reynolds number means changing the wall spacing too.** y+ scales
> as y·Re^0.9, so a mesh that resolves the viscous sublayer at Re = 10⁵ has
> y+ ≈ 10 at Re = 10⁷ and the low-Reynolds wall treatment is no longer valid.
> Holding `--first-layer` fixed across two decades of Reynolds number measured an
> 11% fall in friction drag where theory gives 60% — that was the mesh going
> stale, not the physics. As a starting point, 2.5×10⁻⁵ chords gives y+ ≈ 1.2 at
> Re = 10⁶, and scales as Re^−0.9 from there. The Solve panel reports the y+
> actually achieved.

### What can be shown

| View | What it is for |
|---|---|
| Velocity magnitude, u, v | The flow itself |
| Pressure | Raw, in Pa |
| **Cp** | (p − p∞)/q — the same field made comparable between speeds and scales |
| **Vorticity** | ∂v/∂x − ∂u/∂y. Zero in irrotational flow, so it picks out the boundary layer and the wake and nothing else |
| **k** | Turbulent kinetic energy |
| **ω** | Specific dissipation, on a log₁₀ scale because it spans six decades between the wall and the freestream |
| **μ_t/μ** | How hard the turbulence model is mixing, and where |
| Divergence | Should be zero; any structure is error |

Wall shear is drawn along the section itself, coloured by sign and magnitude with
separation ringed, and Cp and Cf are plotted against x/c in the Surface panel —
both from Phase 5.

**The colour range is taken from the 2nd and 98th percentiles**, not the extremes.
Several of these fields are almost all background with a thin, intense feature in
them: vorticity is enormous in the first cell off the wall and negligible
elsewhere, ω spans six decades over the same distance. Scaling to min and max
gives those few cells the entire colour map and renders everything else a flat
wash — the first attempt at the k, ω and μ_t views was uniformly blank. Trimmed
cells still saturate at the ends of the map rather than being hidden.

## Dependencies## Dependencies

| Library | Version | Role | Fetched |
|---|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.92.9b | Immediate-mode GUI | configure time |
| [GLFW](https://github.com/glfw/glfw) | 3.5.1 | Window, OpenGL context, input | configure time |
| [GoogleTest](https://github.com/google/googletest) | 1.17.0 | Unit tests | configure time |
| OpenGL | 3.2 core | Rendering | system |

Each is pinned to a release tarball plus a SHA-256 hash, so a configure either
reproduces the exact same sources or fails loudly.

## Roadmap

Phases 0 to 10 are complete. Later phases build on them in order:

1. ~~NACA 4-digit geometry generation~~ — done
2. ~~Mesh generation around the section~~ — done
3. ~~Navier-Stokes discretisation~~ — done (laminar, SIMPLE)
4. ~~Viscous flow around an aerofoil: Cp, Cf, wall shear, separation~~ — done
5. ~~Force and moment integration (lift, drag, moment coefficients)~~ — done
6. ~~Angle-of-attack sweeps and polars~~ — done
7. ~~Reynolds averaging and a k-ω SST closure~~ — done
8. ~~Full RANS around NACA sections, with every input and field exposed~~ — done
9. A convection scheme that converges at second order — the single largest source of error left
10. Transition modelling; comparison against digitised wind-tunnel data
11. Vortex structures and post-stall behaviour

## Documentation

[`JOURNAL.md`](JOURNAL.md) is a technical journal explaining what each phase
implements, why it is built that way, and the concepts behind it.

[`ISSUES.md`](ISSUES.md) is the flat list of every defect found, its root cause
and whether it is fixed — so "what is currently wrong with this program" can be
answered without reading eight chapters. Open issues and known limitations are
at the top.

## License

Not yet chosen.
