# CFD-SIMULATOR

A 2D Reynolds-Averaged Navier-Stokes solver for NACA airfoil sections, with an
interactive viewer.

**Status: Phase 5 — laminar viscous flow around an aerofoil, with surface
pressure, skin friction and computed separation. No turbulence model yet.**

The application generates NACA four-digit sections, builds a structured C-grid
around them, solves the steady incompressible Navier-Stokes equations on them
with a finite-volume SIMPLE algorithm, and reads the wall quantities back off
the solution: surface pressure, *C<sub>p</sub>*, wall shear stress,
*C<sub>f</sub>*, near-wall velocity, and where the boundary layer separates.
Type a designation such as `NACA 2412`, tick **Generate mesh**, then
**Initialise flow**, then press **Run**.

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
| `--field NAME` | Shown scalar: `velocity`, `vx`, `vy`, `pressure`, `divergence` |
| `--solve` | Start the solver running at startup (implies `--flow`) |
| `--reynolds N` | Reynolds number based on the chord |
| `--alpha DEG` | Angle of attack in degrees |
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
| Incidence | Angle of attack in degrees; positive pitches the nose up |
| Density | kg/m³, constant (the flow is treated as incompressible) |
| Reynolds | ρUc/μ — the viscosity is *derived* from this, not set directly |
| Pressure | Reference static pressure, Pa (gauge) |

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

## Dependencies

| Library | Version | Role | Fetched |
|---|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.92.9b | Immediate-mode GUI | configure time |
| [GLFW](https://github.com/glfw/glfw) | 3.5.1 | Window, OpenGL context, input | configure time |
| [GoogleTest](https://github.com/google/googletest) | 1.17.0 | Unit tests | configure time |
| OpenGL | 3.2 core | Rendering | system |

Each is pinned to a release tarball plus a SHA-256 hash, so a configure either
reproduces the exact same sources or fails loudly.

## Roadmap

Phases 0 to 5 are complete. Later phases build on them in order:

1. ~~NACA 4-digit geometry generation~~ — done
2. ~~Mesh generation around the section~~ — done
3. ~~Navier-Stokes discretisation~~ — done (laminar, SIMPLE)
4. ~~Viscous flow around an aerofoil: Cp, Cf, wall shear, separation~~ — done
5. Force and moment integration (lift, drag, moment coefficients)
6. Reynolds averaging (RANS)
7. k-ω SST turbulence closure
8. Vortex structures and stall behaviour

## Documentation

[`JOURNAL.md`](JOURNAL.md) is a technical journal explaining what each phase
implements, why it is built that way, and the concepts behind it.

## License

Not yet chosen.
