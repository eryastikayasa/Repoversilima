# RepoVersilima - Clean Architecture V2

RepoVersiempat is reference only. The new data path is rebuilt without changing the proven hardware/system components.

## Locked components

- `audio_hal`
- `wifi_manager`
- `web_config`
- `uart_control`
- `display`
- `wakeword`

These components are not responsible for Gemini protocol state.

## New data path

```text
MIC
  -> audio_hal
  -> audio_engine/audio_input
  -> Gemini session
  -> websocket transport
  -> Gemini protocol

Gemini protocol
  -> websocket transport
  -> audio_engine/audio_output
  -> audio_hal
  -> SPEAKER
```

## Ownership rules

1. `audio_hal` owns hardware only.
2. `audio_engine` owns PCM flow only.
3. `websocket` owns network transport only.
4. `gemini` owns Gemini protocol/session semantics.
5. `main.cpp` only boots and orchestrates components.
6. Wake-word remains a separate proven component and is not embedded into WebSocket or Gemini.

The Gemini URI/session configuration is intentionally not hard-coded in `main.cpp` yet.
