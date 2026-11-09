# A5XX (Adreno 506) register references

The same ground truth the A6XX port uses (`a6xx.xml.h` etc., from Mesa's build)
for the sakura GPU — an Adreno 506, `qcom,chipid = <0x5000600>`, `qcom,kgsl-3d0@1c00000`.

## Files

| file | what it is |
|---|---|
| `a5xx.xml` | A5XX registers (from `src/freedreno/registers/adreno/a5xx.xml`) |
| `adreno/adreno_pm4.xml` | the command processor's packets (type 4 / type 7) |
| `adreno/adreno_common.xml` | shared definitions |
| `freedreno_copyright.xml` | imported header comment |
| `a5xx.xml.h`, `adreno_pm4.xml.h`, `adreno_common.xml.h` | the generated headers (Mesa's `gen_header.py` `c-defines`) |
| `gen_header.py` | Mesa's header generator, fetched unchanged from `src/freedreno/registers/gen_header.py` (MIT) |

## Regenerating the headers (they are generated, not hand-written)

```sh
cd tools/mesa
python3 a5xx/gen_header.py --rnn a5xx --xml a5xx/a5xx.xml c-defines > a5xx/a5xx.xml.h
python3 a5xx/gen_header.py --rnn a5xx --xml a5xx/adreno/adreno_pm4.xml c-defines > a5xx/adreno_pm4.xml.h
python3 a5xx/gen_header.py --rnn a5xx --xml a5xx/adreno/adreno_common.xml c-defines > a5xx/adreno_common.xml.h
```

(`--rnn` is the reference dir that `<import file=.../>` lines are resolved
against; keep the layout above.)

## What differs from A6XX (the port's work list)

- **Command processor**: A5XX has no `CP_MEMCPY` (A6xx+). Copies use
  `CP_BLIT` or `CP_MEM_TO_MEM`; `CP_MEM_WRITE`/`CP_INDIRECT_BUFFER` are the
  same. No `CP_SET_MARKER` class tricks; `CP_EVENT_WRITE` differs.
- **Registers**: `A5XX_*` names (`a5xx.xml.h`), e.g. `REG_A5XX_RBBM_STATUS
  0x4f5`, `REG_A5XX_CP_RB_*` at 0x800+.
- **Texture layout**: fd5 (`fdl5_layout` in Mesa), not `fdl6_layout` — pitch
  and tiling rules differ; `texture.hpp` needs a 5-series layout twin.
- **Compute dispatch**: different `SP_CS_*`/IBO descriptor layout; likely fewer
  SSBO slots; the 640-bit two-IBO table scheme of A6XX does not exist.
- **Renderer**: A5XX sysmem render sequence (turnip `tu5_*`/fd5) with its own
  `RB_MRT[0]` etc. words.
- **Compiler**: ir3 for the 506 (a5xx variant; different register file caps).
