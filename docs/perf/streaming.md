# Streaming performance

Measured on the development machine (Windows 11, MSVC 19.51, RTX 4060,
16 hardware threads, 14 job workers), release build. Section 11 describes
the streaming manager. Runs use the demo world (8 chunk layers, blocks 0 to
127) at the default render distance of 16 chunks and vertical distance 8,
in a 1280 x 720 window with present mode Immediate:

```
voxel_rethinked --frames 20000 --exec "set Video.PresentMode Immediate" --exec "autofly 300"
```

`autofly` flies the camera level in its view direction at the given speed
in blocks per second (a debug autopilot until the benchmark flight, G9.2).
At exit the game logs the streaming main thread time outside burst mode.

## E9.7 Streaming manager (2026-10-29)

### World entry (burst mode)

| Milestone | Time from entering LoadingWorld |
|-----------|---------------------------------|
| Every chunk within 8 chunks of the spawn meshed ("playable", target 2.0 s) | 255 ms |
| Everything in range loaded and meshed, burst mode ends (about 10,100 chunks loaded, 8,900 meshed) | 461 to 521 ms |

### Fast flight, 300 blocks per second (about 19 chunks per second)

Streaming main thread time per frame (StreamingManager::update, outside
burst mode), 20,000 frames:

| Build | Average | Max | Frames over 1 ms |
|-------|---------|-----|------------------|
| Before the job queue change below | 0.079 to 0.084 ms | 1.11 to 4.65 ms | 1 to 2 |
| After | 0.062 to 0.079 ms | 0.74 to 0.78 ms | 0 |

The spikes before came from single calls, not from the budgeted loops:
one JobSystem::schedule or one snapshot taking over 1 ms. Idle workers
swept every queue mutex of every priority (about 75 per sweep), so the main
thread pushing a job sometimes waited for a mutex. Queues now keep an
atomic copy of their size and pops skip empty queues without locking.

Throughput during the flight (overlay page "Streaming and Jobs"): about
5,000 chunks loaded and 4,700 meshed per second; request to mesh on the GPU
64 ms at the median, 76 ms for 90 percent; workers about 20 percent busy.

The headless test "Fast flight keeps every update near the main thread
budget" moves two chunks per update at render distance 8 (far faster than
streaming can follow): max update 0.51 ms in release.

### Whole frames while streaming

With chunks arriving every frame, the frame rate dropped from 1786 FPS
average (1% low 934) standing still to 1204 to 1357 FPS (1% low 359 to 449)
in the flight. Timing probes in the chunk renderer showed why: every chunk
change rebuilt and uploaded the whole draw list (0.55 ms average, 3.5 ms
max) and searched the caves again (0.51 ms average, 1.8 ms max).

The second E9.7 commit changes both:

- The draw list has a region per pass. A chunk adds or removes only its own
  entries (removal moves the region's last entry into the gap), translucent
  entries are sorted again only when the camera changes chunk or a chunk
  with translucent quads changes, and only changed ranges are copied to the
  GPU through a small staging ring.
- The cave search runs on a job; the main thread copies the chunk list and
  keeps using the previous result, with every chunk visible until the result
  for the camera's current chunk is in.

Timing probes in the same flight afterwards (per call, release):

| Work | Before | After |
|------|--------|-------|
| Draw list per chunk change: full rebuild, now flushDraws | 0.55 ms average, 3.5 ms max | 0.004 ms average |
| Translucent sort (about 12 percent of frames) | part of the rebuild | 0.06 ms average, 0.55 ms max |
| Cave search on the main thread, now cave bits only | 0.51 ms average, 1.8 ms max | 0.008 ms average |
| recordCulling in all | | 0.033 ms average |

Whole frames, 20,000 frames each, release:

| Run | Average FPS | 1% low FPS |
|-----|-------------|------------|
| Standing still, before | 1786 to 1964 | 934 to 1279 |
| Standing still, after | 1965 to 1985 | 1183 to 1319 |
| Flight at 300 blocks per second, before | 1204 to 1357 | 359 to 449 |
| Flight, after the cave search moved to a job | 1222 to 1598 | 487 to 579 |
| Flight, after both changes | 1135 to 1694 | 558 to 741 |

Allocations per frame during the flight went from 14 to 54 down to 1.

### Main thread stalls

Over about 200,000 flight frames after these changes, the streaming update
stayed under 1 ms except for 0 to 5 updates per 20,000 (at most 0.03
percent), most of them 1.2 to 2.7 ms and once 5.8 ms. In every slow update
logged with per step timings, a single tiny operation had stalled: one chunk
snapshot (normally 3 to 5 us) or one job schedule call took 0.2 to 1.9 ms. A probe that only read the clock 200 times
per frame (about 5 us) found the same 0.18 ms gaps during the flight (9 in
15,000 frames) and almost none standing still (2), so the main thread itself
is interrupted while the machine is busy; the streaming work is not what
takes the time. Running the workers below normal thread priority made no
measurable difference and was not kept.
