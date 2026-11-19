# Contributing to Haptic-Sense

Thanks for your interest. Haptic-Sense: STM32N6 + uT-Kernel 3.0 haptic hazard-warning firmware.

## Getting started

Firmware lives in `firmware/` (STM32CubeIDE project); building it needs STM32CubeIDE and two ST packages that are not redistributed here (see the README, "Build & flash"). Host-side Python tools are in `tools/`; optional dependencies are listed in `tools/requirements.txt`.

## Testing

None of this needs the board or the ARM toolchain:

```
make -C tests/host                                # C unit tests, gcc + ASan/UBSan
make -C tests/host SAN=0                          # same without sanitizers
python3 -m unittest discover -s tests/tools -v    # data-tool tests, standard library only
ruff check .                                      # lint for tools/ and tests/
```

CI runs these plus a Markdown link check. The host tests say nothing about RTOS timing or preemption; firmware changes need the STM32N6570-DK.

## Pull requests

- Open an issue first for larger changes.
- Keep PRs focused and fill in the pull request template.
- Follow `.editorconfig` and match the style of surrounding code.
- Firmware changes must say which board they were tested on.

## Conduct

Participation is governed by the [Code of Conduct](CODE_OF_CONDUCT.md).
