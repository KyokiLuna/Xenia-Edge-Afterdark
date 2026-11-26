# Xenia Edge Afterdark: unfinished HDD foundation draft

This branch backs up 80 reviewed storage/profile source changes. It is not a checkpoint, release or completed HDD gate.

The draft passed 41 artifact-free syntax checks using current Release flags and the declared direct mbedtls target usage. It has not passed actual CMake regeneration, linking, full tests, native runtime or AppImage acceptance. Native storage controls remain inactive; intermediate durable flush, cached-file/model/Header successors, metadata consumers and PEC work remain incomplete.

The tested base is commit `de56dfafc101e7291867fbba9450d8363722f6df`. Its seven passing direct runs and interrupted Doritos test apply to that base, not to this draft. Other research documents retain their labeled source/build scope.

See [the active development branch](https://github.com/KyokiLuna/Xenia-Edge-Afterdark/tree/research/17559-baseline) for current status, build instructions and experimental prereleases. No new release is created by this backup. Existing original content, build products, actual checkout/index and the dirty FFmpeg submodule are preserved.
