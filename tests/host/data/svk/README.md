# Test spectra from spectravibe-toolkit

Spectra in this folder are converted (to one count per line, with `#time` and `#cal` header lines)
from the AtomSpectra fixtures of
[spectravibe-toolkit](https://github.com/VibeEngineering-LLC/spectravibe-toolkit)
(`detectors/AtomSpectra/data/fixtures/`). The nuclide line table in `main/isoid.c` is derived from
its `data/nuclides.json` (ENSDF/NuDat data). Both are used under the MIT License — see `LICENSE`
in this folder (Copyright (c) 2026 SpectraVibe).

| File | Original fixture |
|---|---|
| cs137.txt | Cs137_0_см.xml |
| kcl.txt | KCl__в_домике.xml |
| medallion.txt | Медальон.xml |
| radon.txt | Spectrum-2024-12-13_20-50-58-ДПР_радона.txt |
| blueberry.txt | Bq-2024-08-03_13-38-15-Черника_Баларусь.xml |
| altai_marinelli.txt | Алтайское_Зло_в_домике_маринелли_294_6г.xml |
| dski_gs5050.txt | Дски_лаба_GS5050_8k.xml |
| bg_cs137.txt | Фон_Cs137_0_см.xml |
| bg_kcl.txt | Фон_KCl__в_домике.xml |
| bg_office.txt | Фон_кабинет_8192к_01-01-2025.xml |
