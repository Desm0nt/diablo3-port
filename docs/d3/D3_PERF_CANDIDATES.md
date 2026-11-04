# Diablo III: какие GPU/CPU-оптимизации DOOM 2016 подходят D3

Дата: 02.11.2026. Ветка с переключателями: **`d3-perf`** (отдельный worktree, от `d3` c3576b8),
коммит `6e19f90`. Все новые переключатели **по умолчанию выключены**: без `dm/env` поведение и SPIR-V те же, что в `d3`.
Сборка проверена отдельно (`work/build-android-perf`, цель `dm`, NDK 29, `DM_GAME=d3`): без ошибок и без новых предупреждений.
APK не собирался — `tools/build_apk.ps1` собирает из основного дерева; для A/B нужно влить `d3-perf` в `d3` (или cherry-pick `6e19f90`).

## Исходная картина (Mi 14 Ultra, Turnip 27, `TU_DEBUG=sysmem`, Акт 1, логи `nh1/vA/dflt1/pA1.dm.log`)

`fps 60` (vsync), `gpu_ms 9–11.3`, `submit_cpu_ms ≈ 5`, `draws ≈ 477`, `passes = 53`, `ops 512`, окно 1600×900 RGBA8.
Разбор `NVN PERF pass_ends` / `pass_end_lines` (на кадр):

| Причина конца прохода | Сколько | Что это |
|---|---|---|
| `op5` = `OP_REPORT_COUNTER` (L1438, `Commands` не-draw) | **16** | samples-passed отчёты (`ops=16/4/16`: 16 сбросов, 4 timestamp, 16 samples). Каждый — конец прохода **и 2 полных барьера** (`barriers op5=32`) |
| `op26` = `DRAW_IDX`: чтение образа (L1649) | 23 | read-break: `why=2` (образ записан после барьера) и `why=6` (читается вложение ещё открытого прохода). По логу `NVN read break` — это цепочки пост-эффектов (bloom 400×225 serial 11↔17, 800×450, 1024×1024 serial 31/32, 1600×900): следующий draw всё равно рисует в другую цель → естественные концы, не потеря |
| `op9`/`op10` = `CLEAR_COLOR`/`CLEAR_DS` (L5085) | 7 + 3 | clear, открывающий свой проход (`vkCmdClearAttachments` в начале прохода) |
| `op26`: смена цели (L4851) | 3 | естественные |
| `op61` present | 1 | — |

Полных барьеров: `op5=32`, `op26=23`, `op61=2` — **57 на кадр**, 32 из них — от samples-отчётов.
Прочее: `uploads` в геймплее 0, `texdirty checks=0`, `storage_snapshots=0`, `dispatches=0`, `quad/warp_rewrites=0`,
UBO снапшотятся (2.4 МБ/кадр, CPU), `occlusion=47` прогонов запросов на кадр.

Шейдеры: офлайн-перепись всех 1 323 бандлов (`work/shaders-vk`) той же цепочкой, что у хоста (nan_minmax → ldc_fold →
forward temps → раунд 3 DOOM), инструмент `tools/d3_spv_census.cpp` (команда сборки в шапке файла). Все переписанные модули
(NAN_SAT + LDC_CANON + F2I + TEXCOORD_NAN=0 вместе) проходят `spirv-val --target-env vulkan1.1` (NDK 29) — **1 323 / 1 323**.
`xbg_spirv_ldc.cpp` из ветки DOOM даёт **побайтно тот же** `ldc_fold` + forward temps, что текущий D3 (все 1 323 модуля).

| Паттерн (SPIR-V после forward temps) | VS (544) | PS (779) | в геймплее (141 загруженный: 64 VS / 77 PS) |
|---|---|---|---|
| `FClamp(x, 0, 1)` (= Maxwell `.SAT`) — сейчас все становятся `NClamp` | 3 541 | 3 149 | 312 VS / **255 PS** (≈3.3 на PS) |
| `FMin`/`FMax` → `NMin`/`NMax` (NaN-проверки) | 4 319 | 900 | 360 / 125 |
| `Select(IsNan(c),0,c)` на координатах сэмплов | 0 | 2 601 (766 модулей) | **246 на 245 сэмплов** — защищён почти каждый сэмпл |
| UBO-сдвиги для `LDC_CANON` | 7 132 (203 модуля, 3 564 слияния) | 0 | 300 |
| HSET2 half-флаги (`HALFFLAG_FOLD`) | 0 | 0 | 0 (`UnpackHalf2x16` нет вообще) |
| `Trunc → ConvertFToU` (`F2I_NOTRUNC`) | 2 | 4 | — |
| VS пишут `gl_PointSize = 1.0` (`NO_PSIZE`) | 544 / 544 | — | 64 |
| discard / subgroup / производные / запись в образы / атомики | 0 | 585 / 0 / 0 / 0 / 0 | — |

PS у D3 маленькие (в среднем ~180 инструкций SPIR-V у загруженных в геймплее), так что NaN-обвязка — заметная доля ALU.

## Рейтинг кандидатов

| # | Кандидат | Откуда (DOOM) | Доказательство в D3 | Ожидаемый выигрыш | Риск | Трудозатраты | Статус |
|---|---|---|---|---|---|---|---|
| 1 | **`DM_GFX_REPORT_INPASS=1`** — samples-отчёт закрывает прогон occlusion-запроса и пишет timestamp **внутри** прохода | новое для D3; обобщение fast path timestamp-отчётов (D2R/DOOM: «timestamp не рвёт проход») | 16 концов прохода + 32 полных барьера на кадр из 53 / 57 | −16 проходов, −32 барьера; оценка **0.3–1.0 мс GPU** в sysmem (WFI + flush/invalidate кэшей на каждый барьер), больше при GMEM | низкий: семантика точная (сумма запросов читается на CPU при ретайре слота; `vkCmdEndQuery`/`vkCmdWriteTimestamp` внутри прохода легальны) | ~10 строк | **портировано, выкл** |
| 2 | **`DM_GFX_NAN_SAT=1`** — `FClamp(x,0,1)` остаётся (NIR `fsat`, бесплатный `(sat)` на ir3) вместо `NClamp` | perf6 30.10, DOOM: main pass −24 %, по умолчанию вкл | 6 690 насыщений в 1 113 модулях, 100 % `FClamp` — насыщения; ~3.3 на PS в геймплее | ~3–4 ALU на насыщение → ~5–8 % ALU пиксельных шейдеров; **0.2–0.8 мс** | низкий: точно (fsat: NaN → 0, как `.SAT`) | ~10 строк | **портировано, выкл** |
| 3 | **`DM_GFX_LAZY_CLEAR=1`** — полный clear, открывающий проход, становится `loadOp=CLEAR` двойника render pass | раунд 3c (v0.4.3 вкл), DOOM: ~4 clear/кадр сливалось | 10 clear-проходов/кадр (`op9=7`, `op10=3`) | clear через load op вместо quad-clear; Turnip не выключает LRZ из-за «CmdClearAttachments at draw 0»; **0.1–0.5 мс**, больше если LRZ включится на основном проходе | низкий–средний: DOOM без артефактов; двойник отличается только load ops | ~60 строк (код DOOM почти дословно) | **портировано, выкл** |
| 4 | **`TU_DEBUG=` (авто) / `TU_DEBUG=gmem`** после №1+№3 | DOOM 30.10: sysmem по умолчанию (на 740 −20 % GPU, на 750 одинаково) | D3 — 470 draw, много смешивания/частиц на 1600×900 → GMEM может выиграть по пропускной способности; но 53 прохода (с концами-загрузками) убивают GMEM, поэтому только после №1/№3 | неизвестно, ± (может быть самым большим) | только производительность | 0 (env) | env, проверить |
| 5 | `DM_GFX_LDC_CANON=1` | раунд 3a | 7 132 сдвига / 203 модуля (только VS: скиннинг), 3 564 слияния | VS-стоимость у D3 мала; **< 0.2 мс** | низкий: точно | копия `xbg_spirv_ldc.cpp` DOOM | **портировано, выкл** |
| 6 | `DM_GFX_TEXCOORD_NAN=0` — снять NaN-защиту координат | раунд 3f, DOOM: выигрыша нет (39.6 vs 39.5), остался выкл | 2 601 защита / 766 модулей; 246 на 245 сэмплов в геймплее (IsNan+Select на каждый) | **0–0.3 мс**; у D3 шейдеры короче, доля больше, чем у DOOM | **не точно**: меняется только результат NaN-координат (драйвер решает) | копия `xbg_spirv_peep.cpp` | **портировано, выкл** |
| 7 | `DM_GFX_NAN_MINMAX=0` (уже есть в дереве) — убрать и NaN-проверки min/max | DOOM: «следующий шаг», не включён | 5 219 `NMin/NMax` | ещё ~2–4 ALU на каждый; 0.1–0.4 мс | средний: D2R видела чёрные блоки от NaN | 0 (env) | env, A/B со скриншотами |
| 8 | `DM_GFX_F2I_NOTRUNC=1` | раунд 3e | 6 мест в 4 модулях | ≈ 0 | нет | бесплатно (тот же файл) | портировано, выкл |
| 9 | `DM_GFX_NO_PSIZE=1` — VS без `gl_PointSize` | раунд 3e | 544 / 544 VS пишут `PointSize=1.0` | мало: −1 экспорт на вершину при ~470 draw | низкий | средне: второй модуль на VS + выбор по пайплайну + ключи прогрева `!np` | не портировано (ниже порога «дёшево») |
| 10 | `DM_GFX_SHADER_RAWCACHE=3` (кэш/зеркало бандлов вне FUSE) | раунд 5a | в геймплее 141 загрузка шейдеров за прогон, `read=1.2–2 мс` каждая с FUSE при `reused=1` | рывки, не средний GPU | низкий | средне (~150 строк + JNI `DM_FILES_DIR`) | не портировано, кандидат на «рывки» |
| 11 | `DM_OPMODE=0` (режим «в руках»: игра сама рисует 720p) как «лёгкий режим» | DOOM: handheld-пресет — только «light mode» в ⚙ | D3 переключает разрешение по `GetOperationMode` (D3_PORT.md) | ~−40 % пикселей; главный рычаг для слабых телефонов | качество картинки | 0 (env) / пункт ⚙ | не проверено на телефоне |
| 12 | `DM_GFX_PASS_PROF=1` (диагностика, уже в дереве) | perf3 | per-pass GPU (`NVN GPASS`) у D3 ещё ни разу не снимался | — (нужно, чтобы понять, куда идут 10 мс) | — | 0 | env |

### Не применимо к D3 (и почему)

| DOOM | Почему нет |
|---|---|
| `DM_GFX_HALFFLAG_FOLD` | 0 совпадений: в шейдерах D3 нет `UnpackHalf2x16`/HSET2-флагов |
| `DM_GFX_SKIP_DEAD_BOX` | опирается на «игра не читает samples»; D3 читает 16 samples-отчётов/кадр → сломало бы результаты запросов |
| `DM_GFX_PRESENT_FLIP_FOLD` | flip-blit есть только при NVN `LOWER_LEFT`; D3 ставит `UPPER_LEFT` (`taafix::viewport_ll()` = 0), лишнего blit нет |
| `DM_GFX_GS_PASSTHRU(_VARY)`, `DM_GFX_GSCULL_FIX` | GS в D3 нет (0 из 1 323) |
| `DM_GFX_QUAD_DERIVATIVES`, `DM_GFX_WARP32/WARP_MODE` | 0 subgroup-операций и 0 производных в SPIR-V (`quad/warp_rewrites=0` в логе) |
| `DM_GFX_EARLY_Z` | уже в дереве (по умолчанию 1), но у D3 нет FS с записью в образы/SSBO/атомиками — переписывать нечего |
| `DM_GFX_STORAGE_CHAIN`, `LAZY_STORAGE`, `INDEX_TAIL_CLAMP`, `GPU_INDIRECT` | нет записей SSBO, compute и indirect (`dispatches=0`, `storage_snapshots=0`) |
| `DM_DIRTY_BLOCKS`, `DM_DIRTY_PROTECT_BGEN`, `PROTECT_CHURN`, `PARTIAL_PROLOGUE`, `PARTIAL_TEX`, VT-переключатели | у D3 нет VT-кэшей; в геймплее `texdirty checks=0`, загрузок текстур 0 |
| `DM_GFX_DESWZ_DIRECT` | только загрузки уровня (в геймплее 0 загрузок) — не кадр |
| `DM_GFX_SLOTS=3`, `DM_GFX_CHUNK_MID` | D3 сознательно идёт одним сабмитом на кадр (`IDLE_RETIRE=2`, `sync_q=0`, CPU 5 мс < GPU 10 мс); перекрытие уже есть |
| `DM_GFX_UBO_VIEWCACHE` | работает для zero-copy UBO, а D3 снапшотит UBO (`ZC_UBO_MIN=1`); имеет смысл только после A/B `DM_GFX_ZC_UBO_MIN=0` |
| `DM_GFX_STATEGEN`, `PERF_VERBOSE`, `ARENA_KEEP` | CPU/память; поток трансляции 5 мс — не узкое место |
| `DM_AUDREN_CATCHUP=0` | D3 использует `AudioOut`, а не audio renderer |
| `DM_GFX_RELAXED` (fp16) | в DOOM сломано (почти чёрная сцена), отклонено |
| `DM_GFX_ANISO_MAX`, `ZC_CPU`, `ARENA_UNCACHED` | DOOM A/B: эффекта нет (14.8–14.98 мс ≈ база) |
| `tbdr_pass_merge/load_dead/store_dead.py` | офлайн-анализ load/store для GMEM; при `TU_DEBUG=sysmem` load/store почти бесплатны. Вернуться, если №4 покажет выигрыш GMEM |
| `DM_GFX_DEPTH_FEEDBACK=2` | уже в дереве и работает: `depth_feedback_reads=17` на кадр — чтения глубины открытого прохода без разрыва; 17 разрывов «attach» — цветовые вложения (естественные концы цепочек пост-эффектов) |

## Что портировано (ветка `d3-perf`, коммит `6e19f90`)

Файлы: `dm/gfx/vulkan/engine/xbg_game.cpp`, `xbg_spirv_ldc.cpp/.h` (= версия DOOM, с `xbg_spirv_ldc_canon`),
`xbg_spirv_peep.cpp/.h` (= DOOM; `f2i_notrunc`, `texcoord_unguard`; `halfflag_fold` и `strip_pointsize` есть, но не подключены),
`dm/gfx/vulkan/CMakeLists.txt` (+ `xbg_spirv_peep.cpp`).

| Переключатель (`dm/env`) | По умолч. | Что делает |
|---|---|---|
| `DM_GFX_REPORT_INPASS=1` | 0 | `OP_REPORT_COUNTER` типа SAMPLES_PASSED: `end_occlusion_query()` + timestamp в слоте без `end_open_pass`/`full_barrier`; новый timestamp-пул (раз на 64) по-прежнему закрывает проход |
| `DM_GFX_LAZY_CLEAR=1` | 0 | код DOOM r3c: `lazy_clear_open/add/flush/begin`, двойники render pass по маске load ops, stencil-only clear не ставит `depth_cleared` |
| `DM_GFX_NAN_SAT=1` | 0 | в `nan_minmax_swap`: `FClamp(x, 0.0, 1.0)` не меняется на `NClamp` (нужен `DM_GFX_NAN_MINMAX=1`, APK ставит) |
| `DM_GFX_LDC_CANON=1` | 0 | `xbg_spirv_ldc_canon` после forward temps |
| `DM_GFX_F2I_NOTRUNC=1` | 0 | `xbg_spirv_f2i_notrunc` |
| `DM_GFX_TEXCOORD_NAN=0` | 1 (защита есть) | `xbg_spirv_texcoord_unguard` — **не точно** |

Лог при старте: `NVN d3-perf pass switches: …`, `NVN d3-perf shader switches: …`. Раз в окно PERF (если что-то включено):
`NVN PERF d3perf nan_sat_kept= ldc_canon= canon_merges= f2i_notrunc= texcoord_unguarded= inpass_reports= lazy_clear folded= flushed= appended=`
(первые пять — накопительные по загруженным модулям, остальные — на кадр). Шейдерные переключатели работают в рантайме при
загрузке/прогреве (`tidy_rewrite`, оба загрузчика), **отдельный каталог шейдеров не нужен** (`work/shaders-vk` как есть).

## План A/B для основного агента (телефон)

Подготовка: влить `d3-perf` в `d3` (поведение по умолчанию не меняется), собрать APK как обычно. Метрика — `gpu_ms` (fps упирается
в vsync 60), плюс `passes` и барьеры. Разброс `gpu_ms` между прогонами 9–11.3 мс, поэтому каждый вариант — **2 прогона
вперемежку с базой** (база, X, база, X). Шейдерные переключатели меняют SPIR-V → Vulkan pipeline cache промахивается: перед
замером один прогон-прогрев на вариант (пайплайны кэшируются, повторно не компилируются).

```sh
bash tools/gp_ab.sh d3p_base                                         # база
bash tools/gp_ab.sh d3p_inpass  DM_GFX_REPORT_INPASS=1               # №1
bash tools/gp_ab.sh d3p_lazy    DM_GFX_LAZY_CLEAR=1                  # №3
bash tools/gp_ab.sh d3p_passes  DM_GFX_REPORT_INPASS=1 DM_GFX_LAZY_CLEAR=1
bash tools/gp_ab.sh d3p_sat     DM_GFX_NAN_SAT=1                     # №2 (прогрев + 2 замера)
bash tools/gp_ab.sh d3p_canon   DM_GFX_LDC_CANON=1                   # №5
bash tools/gp_ab.sh d3p_texnan  DM_GFX_TEXCOORD_NAN=0                # №6 (не точно)
bash tools/gp_ab.sh d3p_nomm    DM_GFX_NAN_MINMAX=0                  # №7 (уже в дереве)
EX="DM_GFX_REPORT_INPASS=1 DM_GFX_LAZY_CLEAR=1 DM_GFX_NAN_SAT=1 DM_GFX_LDC_CANON=1 DM_GFX_F2I_NOTRUNC=1"
bash tools/gp_ab.sh d3p_exact   $EX                                  # все точные
bash tools/gp_ab.sh d3p_auto    $EX TU_DEBUG=                        # №4: Turnip сам выбирает GMEM/sysmem
bash tools/gp_ab.sh d3p_gmem    $EX TU_DEBUG=gmem                    # №4: принудительно GMEM
bash tools/gp_ab.sh d3p_prof    DM_GFX_PASS_PROF=1                   # №12 диагностика; разбор:
#   python <репозиторий DOOM Mobile>/tools/gpass_report.py work/logs/d3p_prof.dm.log
bash tools/gp_ab.sh d3p_lrz     DM_GFX_LAZY_CLEAR=1 TU_DEBUG=sysmem,perf   # проверка LRZ: в logcat не должно быть
#   "Disabling LRZ because 'CmdClearAttachments'" (сравнить с TU_DEBUG=sysmem,perf без LAZY_CLEAR)
```

Что сравнивать (кроме медиан `gp_ab.sh` — `fps submit_cpu_ms gpu_ms sync_q draws`):

```sh
f=work/logs/<name>.dm.log
grep "PERF v=1" $f | tail -15 | grep -o "passes=[0-9.]*" | head -1           # 53 → ~37 с REPORT_INPASS
grep "PERF pass_ends" $f | tail -1 | grep -o "op5=[0-9.]*\|barriers.*"       # op5 16 → 0, barriers op5 32 → 0
grep "PERF d3perf" $f | tail -1                                              # inpass_reports≈16, lazy folded≈?, flushed
grep "PERF pass_ends" $f | tail -1 | grep -o "occlusion=[0-9.]* reports=[0-9.]* samples=[0-9]*"   # samples ≈ как в базе
```

Ожидаемо: `REPORT_INPASS` — `passes` 53 → ~37, `barriers op5` 32 → 0, `inpass_reports=16.0`, `samples=` того же порядка, что
в базе (не 0); `LAZY_CLEAR` — `lazy_clear folded` ≈ 5–10, `flushed` мало; `NAN_SAT` — `nan_sat_kept` > 0, `nan_minmax=` в
строке `quad_rewrites` меньше.

Визуальные проверки (`tools/shots.sh <name> 95 <ENV…>` с тем же `DM_KEYS`, сравнить кадры с базой):
- **REPORT_INPASS**: то, что D3 решает по occlusion-запросам — силуэт/обводка героя за стенами и деревьями, вспышки/ореолы
  источников света, пропадание объектов при перекрытии. Пройти героем за здание/дерево у Нового Тристрама; сравнить с базой.
  Мигание или «всегда видно / никогда не видно» = регрессия.
- **LAZY_CLEAR**: первый кадр после смены цели — нет мусора/прошлого кадра на краях, тени и пост-эффекты (bloom 400×225) без
  «залипания»; меню и инвентарь (UI-проходы с clear).
- **NAN_SAT / NAN_MINMAX=0**: чёрные/белые точки и блоки на ярких эффектах (огонь, заклинания, частицы, вода) и на UI.
- **TEXCOORD_NAN=0**: искажения/дисторсия (жар, порталы), декали и пост-эффекты — нет ли пикселей «из угла текстуры».
- **TU_DEBUG=gmem/авто**: только скорость (картинка та же); смотреть и `gpu_ms`, и длинные кадры (`[长帧]`).

Порядок: сначала `d3p_prof` (куда идут 10 мс), потом №1 → №3 → №2 по отдельности, затем `d3p_exact` и №4 поверх него;
неточные (№6, №7) — последними и только со скриншотами. Что даст выигрыш без артефактов — включать по умолчанию для D3
в `jni_android.c` (как в DOOM: `setenv(..., "1", 0)`).
