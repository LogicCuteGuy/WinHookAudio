# Pure DLL Worker, no engine process

v10.1 uses a Worker thread inside the Master DLL driven by Master_Tick, with close-Master equals silence, because the DAW must remain the single host and a separate engine process would add lifecycle and IPC complexity.
