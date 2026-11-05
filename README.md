tickrate-tests
==============

Tests for the `tickrate` branches of q2trek/q2pro, q2trek/openffa and
q2trek/opentdm: exact frame times at every server rate from 10 to 60 Hz, mixed
clients and servers, and the game code that has to keep real time when a world
step is shorter than 100 ms.

The harnesses link the real engine and game code. Nothing is copied from the
forks: each run builds the checkouts you point it at, and the upstream peers
and negative controls come from their `master` branch.

Layout
------

* `engine/scheduler` drives a dedicated server (q2proded objects with `main`
  renamed) on a virtual clock: frame deadlines, catch-up after a hitch, usercmd
  time allowance, event clearing, `sv_fps` rounding.
* `engine/client` drives the client with a listen server: the frame timeline
  T(n) = floor(n * 1000 / R), the `CLS_EXACTFPS` handshake, rate requests, the
  rates an upstream client or a client without variable FPS gets, loopback
  batches, demos. `capability.c` checks the handshake encoding on its own.
  With `CL_SERVE`/`CL_CONNECT` it runs as separate processes over UDP on
  127.0.0.1 (the fleet stages). A second client build with the packetdup
  hack compiled in runs only the loopback packetdup check.
  `remote_lerp_test.c` checks the smoothing of other players
  (`cl_remote_lerp`) on its own: history, delay, teleports, stops, rate
  changes, senders that jitter.
* `engine/server/status_n.py` asks a fork server at each rate for its
  serverinfo (`sv_fps` is published) and for `status n` (the pacing table
  and the recommended client packet rate, with one bare protocol 34 client
  connected).
* `mods/api3` loads OpenFFA or OpenTDM through the game API and checks hurt
  triggers, crushers, movers and riders, weapon timing, OpenTDM HUD timers, at
  every rate and under several usercmd schedules. The `engine` variant links
  the q2pro collision code and a generated BSP.
* `run.sh` builds everything and runs the stages, `controls.txt` lists the
  negative controls.

Expected rates
--------------

A server at R Hz with `sv_fps` set writes frames to each client at a rate that
depends on the client:

* a fork client, or any client of an upstream server: gcd(R, u), where u is
  the rate the client asks for, clipped like `sv_fps`;
* an upstream client of a fork server: the highest rate up to that which
  divides both R and 1000;
* a client built without variable FPS, or one on protocol 34 or 35: 10.

So a fork server at 60 Hz sends 60, 20 and 10 frames per second to these
three; at 50 Hz 50, 50 and 10; at 30 Hz 30, 10 and 10. The engine rounds
`sv_fps` down to a multiple of 10 within 10..60 (25 runs at 20, 70 at 60, 5 at
10), and the mods refuse a value that isn't such a multiple.

Requirements
------------

Linux, gcc, meson, ninja, python3, git, and the Quake 2 data: a directory with
`baseq2/pak*.pak` (the retail paks or the demo pak). The engine stages load a
deathmatch map from it.

Running
-------

    Q2_DATA=/path/to/quake2 ./run.sh ../q2pro ../openffa ../opentdm

The three arguments are checkouts with the `tickrate` branch checked out and a
`master` (or `origin/master`) branch. `RATES` limits the server rates (default
`10 20 30 40 50 60`; any other list is a partial run), `Q2_MAP` picks the map
(default `q2dm1`), `OUT` is where builds and logs go (default
`$TMPDIR/tickrate-tests`).

Every stage ends PASS, FAIL or NOT_RUN (a missing input, with the reason on the
first line of its log in `$OUT/log`). The exit code is 0 only when every stage
passed. `--only REGEX` and a shorter `RATES` run a subset for debugging and exit
3 at best: never green.

    Q2_DATA=/path/to/quake2 ./run.sh --negative ../q2pro ../openffa ../opentdm

runs the negative controls instead: each one copies a fork file, breaks the
behavior under test with a sed script (or takes the file from `master`),
rebuilds the harness with it and expects a scenario to fail an assertion. A
crash, a timeout, a game error or a failed precondition doesn't count as a
catch.

Neither mode writes to the checkouts; the run fails if their `git status`
changes.

Not covered: the start times of temporary entities and alias model animation
in `src/client/tent.c` and `entities.c` (they now take `cl.prevservertime`)
are visual and have no test; the interpolation that uses the same value is
checked by the client stages.

The tests were ported from the author's local test suite.

License
-------

GPL-2.0, as the code they link. See `LICENSE`.
