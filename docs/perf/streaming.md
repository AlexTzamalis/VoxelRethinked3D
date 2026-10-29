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
max) and searched the caves again (0.51 ms average, 1.8 ms max). The
follow up commit makes both incremental or asynchronous.
