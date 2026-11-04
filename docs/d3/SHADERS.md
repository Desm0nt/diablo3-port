# Diablo III (Switch): офлайн-шейдеры (NVN Maxwell -> SPIR-V -> XGS1)

Шейдеры переводятся только офлайн; хост грузит `<key>.xgs` (XGS1 v1, `pack_game_shaders.py`) по ключу ниже. Конвейер — копия XC2
(заметки о шейдерах порта Xenoblade 2, не опубликованы) со своим сканером вместо nxnative `shader_split.py` (профиля D3 у nxnative нет, а контейнер простой).

## Где шейдеры

* Только в SNO-файлах `Shaders\*.shd` (группа SHADER_SNO; `ShaderMap\*.shm` — лишь таблицы сопоставления материалов, 64–76 байт).
  База — `CPKs/ClientCommon.cpk` (1 564 файла), переопределения — `Update/CPKs/Patch2_6_5` (244), `2_6_7` (246), `2_6_8` (246, = 2_6_7;
  244 из них = 2_6_5), `2_7_6` (3 новых). Слитый вид: **1 569 файлов** (ClientCommon 1 320 + 2_6_8 246 + 2_7_6 3).
* Скан магии control-секции `0x98761234` по всем членам всех 37 CPK (`d3_cpk.py scan`, `work/logs/scan_ctlmagic.tsv`): кроме .shd —
  только 3 случайных совпадения в FSB5-звуке. В `main`/`sdk` control-секций нет (в main одна случайная `0x12345678`).
* Компиляции во время работы нет: в ExeFS только main и sdk (без subsdk libglslc), main не импортирует `glslc*`; в sdk есть лишь
  аллокаторная обвязка SDK (`glslc_GetAllocator`, `glslc_Alloc`, `glslc_Free`, …). Строки `nvnProgramSetShaders`, `nvnProgramSetShadersExt`,
  `nvnDeviceSetIntermediateShaderCache` — обычный загрузчик процедур NVN.

Формат .shd: SNO-заголовок (0xDEADBEEF, …), сериализованная рефлексия (HLSLcc: имена cbuffer-ов `CBMain`, `hlslcc_mtx4x4…`, входы
`in_POSITION0`, варьинги `vs_TEXCOORD0` — шейдеры написаны на HLSL и прогнаны HLSLcc -> GLSL -> GLSLC офлайн) и для каждого прохода две
стадии: **vertex, затем pixel**, каждая как сырая GLSLC control-секция, сразу за которой идёт её code area:

```
control: magic 0x98761234; +0x20 своя длина (у всех 0x850); +0x700/+0x704 c1 = [start, end) внутри code area; +0x704 = длина code area
code   : control + 0x850: заголовок 0x30 (0x12345678) + SPH 0x50 + инструкции + c1
```

Проходов в файле: 1 (1 269 файлов), 2 (269), 3 (30), 4 (1). Порядок стадий во всех файлах строго `vs, ps, vs, ps…`; программа =
соседняя пара (vs, ps). Подтверждено переводом: проверка интерфейсов по всем 1 901 программам (входы ps против выходов vs) — 0 расхождений.

## Ключ (байт в байт как `xbg_shader_program_key`, dm/gfx/vulkan/engine/xbg_shader_key.c)

```
n = u32 @ control+0x20; L = n если 0x760 <= n < 0x880, иначе 0x880; nc = u32 @ control+0x704
key = hex(sha256(control[0:L] || code[0:nc])[0:8])      (только control, если code == NULL, nc == 0 или nc >= 1<<24)
```

У D3 n = 0x850 во всех 3 802 записях, поэтому L = 0x850 и хвост за control-секцией в ключ не попадает; nc = длина code area.
1 323 различных control-секции и 1 323 ключа: ни одна control-секция не встречается с разным кодом (неоднозначности, как у Monolith,
нет). `tools/d3_shader_jobs.py` считает ключ дважды для каждой записи — из байтов .shd и заново из записанного `rec/<key>.rec` тем же
способом, что и хост (control найдена по магии, code перед ней) — **всё совпало**; одинаковые ключи из разных файлов побайтно равны.
Допущение: игра отдаёт NVN control-секцию и code area без изменений (как они лежат в .shd) — код копируется в memory pool как есть.

## Глубина и ориджин (D3-специфика)

Сразу после `nvnDeviceInitialize` игра вызывает `nvnDeviceSetWindowOriginMode(dev, 1 = UPPER_LEFT)` и
`nvnDeviceSetDepthMode(dev, 1 = NEAR_IS_ZERO)` безусловно (main+0xe7738…0xe7764, указатели процедур загружаются в main+0xe27a4/+0xe27c0).
Клип-z уже [0, w], как в Vulkan, поэтому XC2-ремап near=-w (`z' = z/2 + w/2`) для D3 **выключен**: `d3_shader_jobs.py` пишет в
`shader_facts.tsv` `{"depth_zero": true}` для каждого vs (544), `dmsh_vk.job_for` вливает факт в задание транслятора
(`D3_DEPTH_ZERO=0` — вернуть ремап). Проверено: бандл vs с фактом и без отличается (без ремапа SPIR-V короче). Ориджин upper-left
совпадает с `OriginUpperLeft` транслятора; вьюпорт/намотка — забота хоста.

## Команды

```sh
sh tools/d3_shaders_vk.sh                   # всё: work/cpk-shaders (если нет) -> work/shaders-jobs -> транслятор -> work/shaders-vk (~25 с)
EXTRACT=1 sh tools/d3_shaders_vk.sh         # заново извлечь .shd из слитого вида
ALL=1 EXTRACT=1 sh tools/d3_shaders_vk.sh   # все хранимые версии .shd (work/cpk-shaders-all/<архив>/): +41 vs из базовых версий, которые патчи заменили
sh tools/d3_shaders_vk.sh out k1 k2 @keys.txt
```

| шаг | команда | выход |
|---|---|---|
| extract | `python tools/d3_cpk.py mextract work/romfs work/cpk-shaders '*.shd'` | Shaders/*.shd, _sources.tsv (~7 с) |
| jobs | `python tools/d3_shader_jobs.py work/cpk-shaders work/shaders-jobs` | rec/, jobs.tsv, programs.tsv, shader_map.tsv, shader_facts.tsv, xfb.tsv, report.txt |
| translator | `python tools/build_dmsh_xlate_d3.py` | work/build/dmsh_xlate_d3/bin/dmsh_xlate.exe (исходник = `dm/gfx/tools/dmsh_xlate_xc2`, без изменений) |
| bundles | `XLATE=… SPIRV_OPT=… SPIRV_VAL=… J=12 python dm/gfx/tools/dmsh_vk_xc2.py work/shaders-jobs work/shaders-vk` | <key>.xgs, _vk_build.txt/.json, _chain.json |

Формат `jobs.tsv`/`programs.tsv`/`rec` тот же, что у `xc2_shader_jobs.py`, поэтому `dmsh_vk_xc2.py` работает без правок
(в `_chain.json` поле `xc2_shader_jobs.py` = `missing` — это нормально).

## Счёт (2026-11-02)

| | записей | ключей | vs | ps | gs | cs |
|---|---|---|---|---|---|---|
| слитый вид (то, что грузит игра) | 3 802 | 1 323 | 544 | 779 | 0 | 0 |
| все версии (ALL=1) | 5 270 | 1 364 | 585 | 779 | 0 | 0 |

* Программ (проходов vs+ps): 1 901, различных пар 1 141; ключей вне программ — 0; у всех 544 vs есть ps-партнёры.
* Трансляция: **1 323 / 1 323 ok, 0 ошибок** (translate / spirv-opt / spirv-val vulkan1.1 / export / pack), 17 с при J=12; все файлы —
  XGS1, имя = ключ, множество имён = jobs.tsv; повторная сборка одного ключа побайтно равна бандлу. Бандлы 31,1 МБ.
* Нет: geometry/tess/compute, transform feedback (strides +0x6dc = 0 у всех vs), separate sampler, image, bindless. Текстуры — только
  комбинированные хэндлы c2 0x20 + 8r (2 394 дескриптора); uniform 1 756, c1 560.
* c1: 0 или 256 байт (vs: 118/425, один vs — 512; ps: 645/134); 3 стадии индексируют c1 динамически -> c1 дополнен до 64 КиБ.
* Сообщения: одна стадия, ps `7d07ee7a91c6f85d` (`PostFX Copy Depth MS*.shd`): «texture handle 8 … no type in the facts, using 2D» —
  тип запрашивается для textureSize; итоговый SPIR-V всё равно `OpTypeImage 2D MS=1` (тип взят из `OpImageFetch … Sample`), так что
  дескриптор — 2D multisample, как и нужно.

## Открытые вопросы

1. Типы вершинных атрибутов и выходов ps — по умолчанию float (GLSL нет); целочисленные форматы согласует хост.
2. «Поздний патч выигрывает» — по смыслу и по порядку загрузки, сама процедура поиска имени по смонтированным CPK не разобрана. Для
   шейдеров риск только в 41 vs базовой версии (их можно собрать `ALL=1` — ключи разные, бандлы не мешают друг другу).
3. Ключ предполагает, что игра передаёт control/code без модификаций; если хост увидит промахи по ключам — логировать
   `own`/`nc` и первые байты и сравнить с `work/shaders-jobs/shader_map.tsv`.

## Файлы

* `tools/d3_cpk.py`, `tools/d3cpklib.py` (+ `d3cpklib.LICENSE`), `tools/d3_shader_jobs.py`, `tools/build_dmsh_xlate_d3.py`, `tools/d3_shaders_vk.sh`
* Данные: `work/cpk-shaders/` (+ `_sources.tsv`), `work/cpk-shaders-all/`, `work/shaders-jobs/`, `work/shaders-vk/`, `work/build/dmsh_xlate_d3/`,
  логи `work/logs/verify_all.txt`, `work/logs/scan_ctlmagic.tsv`; списки членов всех CPK — `work/cpk-lists/*.tsv`

## Атрибуция

D3CPKUnpack (https://github.com/RedMadKnight/D3CPKUnpack, MIT, Copyright (c) 2026 Krzysztof Janiszewski): ридер CPK. nxnative
(https://github.com/seedds/nxnative, MIT, seedds): схема ключа, раскладка c2-хэндлов; nx2android (MIT): раскладка xfb. Транслятор и
`dmsh_vk_*.py` — цепочка DOOM/XC2 этого репозитория поверх Ryujinx.
