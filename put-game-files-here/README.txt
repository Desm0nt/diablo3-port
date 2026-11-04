Diablo III Mobile - put your game files here
============================================

Put these things into this folder (any file names are fine):

  1. Diablo III: Eternal Collection base game   title 01001B300B9BE000 v0         NSP, NSZ, XCI or XCZ
  2. Diablo III update v1441792 (patch 2.7.7)   title 01001B300B9BE800 v1441792   NSP, NSZ, XCI or XCZ
                                                (an XCI that already contains this update counts as both 1 and 2)
  3. prod.keys                                  the keys of YOUR OWN console (dumped with Lockpick_RCM)

Optional - language packs (DLC), only if you want the game in that language:

  4. Russian Language Pack                      title 01001B300B9BF00A (index 10)
     United Kingdom English Language Pack       title 01001B300B9BF001 (index 1)
     (other language packs of the game work the same way; the cosmetic DLC is not needed and is skipped)

Split dumps from a FAT32 SD card work too: the folder with 00, 01, ... parts, or name.xc0 + name.xc1 ...

Then run setup:
  Windows:        double-click setup.bat (one folder up)
  Linux / macOS:  ./setup.sh  (one folder up)

It prepares work/Diablo3Mobile with romfs, dm/out, dm/shaders (and aoc/<index> for language packs); copy that folder to
the phone as Download/Diablo3Mobile (setup prints the adb commands, or use: setup push).

Your files are only read; nothing is copied, changed or uploaded. Everything in this folder except this
README is ignored by Git. Dump the game and the keys from a Switch you own: they are never shared by this
project. Step-by-step guide: docs/INSTALL.md (Russian: docs/INSTALL.ru.md).

----------------------------------------------------------------------------------------------------------

Diablo III Mobile - положите сюда файлы игры
============================================

Положите в эту папку (имена файлов любые):

  1. Базовая игра Diablo III: Eternal Collection   title 01001B300B9BE000 v0         NSP, NSZ, XCI или XCZ
  2. Обновление v1441792 (патч 2.7.7)              title 01001B300B9BE800 v1441792   NSP, NSZ, XCI или XCZ
                                                   (XCI, в котором уже есть это обновление, считается сразу за 1 и 2)
  3. prod.keys                                     ключи ВАШЕЙ консоли (снятые через Lockpick_RCM)

Необязательно - языковые пакеты (DLC), если нужна игра на этом языке:

  4. Russian Language Pack                         title 01001B300B9BF00A (индекс 10)
     United Kingdom English Language Pack          title 01001B300B9BF001 (индекс 1)
     (другие языковые пакеты игры - так же; косметические DLC не нужны и пропускаются)

Подходят и разбитые дампы с карты FAT32: папка с частями 00, 01, ... или name.xc0 + name.xc1 ...

Затем запустите установку:
  Windows:        двойной щелчок по setup.bat (на уровень выше)
  Linux / macOS:  ./setup.sh  (на уровень выше)

Получится папка work/Diablo3Mobile (romfs, dm/out, dm/shaders и aoc/<индекс> для языковых пакетов); скопируйте её на
телефон как Download/Diablo3Mobile (установка напечатает команды adb, или: setup push).

Файлы только читаются: ничего не копируется, не меняется и не отправляется. Подробная инструкция:
docs/INSTALL.ru.md (по-английски: docs/INSTALL.md).
