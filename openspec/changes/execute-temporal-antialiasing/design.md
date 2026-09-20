# Design

`FrameAssembly` already owns `TemporalFramework`, so it also owns the two device images represented by that framework's history declaration. On a temporal frame it imports the image last completed as `temporal_previous` and the other as `temporal_history`. `ForwardFrame` declares fragment reads from scene color, velocity, depth, and previous history, and a color-attachment write to current history.

`FrameRecorder` records the temporal pass with a fullscreen graphics pipeline. The fragment shader reprojects the previous image with velocity, bounds-checks the coordinate, clamps it to a 3x3 current-color neighborhood, and blends it only when the frame block says history is valid. The next post-process pass binds `resources.temporal_history`, so the temporal output reaches the visible image.

History becomes valid and the ping-pong index advances only after graph execution succeeds. A first frame, camera cut, projection change, resolution change, or teleport sets the history weight to zero. A device-free structural assembly retains transient temporal resources so existing null tests can inspect the graph without claiming execution.
