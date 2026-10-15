# Прошивка брелока и привязка к телефону

Пошаговая инструкция: сборка и прошивка прошивки, генерация и заливка ключей,
сборка Android-приложения и привязка брелока к телефону.

Состав репозиториев:

- Прошивка (nRF52840, PlatformIO/Arduino): `firmware/Met-keychain`
- Скрипты провижининга и заливки ключей: `firmware/Met-keychain/tools`
- Android-приложение (Flutter): `demonstration program/ble_android_app`

Протокол и форматы — [`PROTOCOL.md`](PROTOCOL.md:1).

---

## 0. Что понадобится

- Плата: nice!nano / Adafruit Feather nRF52840 (см. [`platformio.ini`](platformio.ini:1)).
- ПК с Windows и установленным:
  - Python 3.10+
  - [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/) (`pip install platformio`)
  - [nRF Util](https://www.nordicsemi.com/Products/Development-tools/nRF-Util) с компонентом `nrf5sdk-tools` (для DFU-загрузки)
  - Flutter SDK 3.x + Android SDK
- Телефон Android 8.0+ с BLE.

Python-зависимости скриптов:

```
pip install bleak "qrcode[pil]" pyserial
```

- `bleak` — заливка ключей по BLE (NUS)
- `qrcode[pil]` — генерация PNG с QR
- `pyserial` — чтение UART-логов ([`scripts/serial_log.py`](scripts/serial_log.py:1))

Внешняя утилита [age](https://github.com/FiloSottile/age) (бинарник `age` /
`age-keygen`) — генерация ключей и шифрование CSV бэкенда:

```
scoop install age      # Windows (или: choco install age)
apt install age        # Linux
```

---

## 1. Прошивка микроконтроллера

### 1.1. Сборка

Из каталога прошивки:

```
cd firmware/Met-keychain
pio run
```

Сборка использует env `nicenano` ([`platformio.ini`](platformio.ini:1)): платформа `nordicnrf52`, Arduino, `upload_protocol = nrfutil`.

### 1.2. Вход платы в режим загрузчика (DFU)

У плат без SWD-отладчика (nice!nano, Feather) прошивка идёт через USB-загрузчик
Adafruit. Загрузиться в DFU можно двумя способами:

1. **Двойное нажатие кнопки Reset** на плате (быстро два раза). Плата появится
   как USB-устройство загрузчика.

2. Просто зашить (на некоторых платах работает)

### 1.3. Загрузка прошивки

```
pio run -t upload
```

Если PlatformIO не находит порт автоматически, укажите его явно:

```
pio run -t upload --upload-port COM3
```

После загрузки плата перезагрузится и начнёт рекламу. Непрошитый (не
провижиненный) брелок рекламируется как **pairing beacon** (`type=0x02`) с
номером устройства (см. [`PROTOCOL.md`](PROTOCOL.md:27)).

### 1.4. Отладочный монитор (опционально)

```
pio device monitor -b 115200
```

В отладочной сборке `ENABLE_SERIAL=1` ([`headers/config.h`](headers/config.h:8))
выводятся логи. В релизе Serial выключен для экономии батареи.

---

## 2. Генерация ключей и заливка в брелок

У каждого брелока уникальные ключи. Ключи генерируются на ПК и записываются в
брелок по BLE (Nordic UART Service). `K_dev` после заливки покидает устройство
только в зашифрованном CSV для бэкенда и **никогда** не передаётся по BLE
(см. [`task.txt`](task.txt:1), правила безопасности).

### 2.1. Вариант A — сгенерировать и залить одним шагом

Скрипт [`tools/flash_keys.py`](tools/flash_keys.py:1) умеет сгенерировать ключи,
найти брелок в эфире, залить ключи и вывести строку для бэкенда.

1. Убедитесь, что брелок включён и рекламируется как pairing beacon.
2. Посмотрите список найденных брелоков:

```
cd firmware/Met-keychain/tools
python flash_keys.py --list
```

Вывод: `ADDRESS devno=... rssi=... name=...`.

3. Сгенерируйте и залейте ключи для устройства с индексом `1`:

```
python flash_keys.py --gen --index 1 --address AA:BB:CC:DD:EE:FF
```

`--address` можно не указывать, если в эфире ровно один брелок — скрипт выберет
его сам (иначе требует явный адрес).

На выходе скрипт печатает:

- `PROVISION -> OK ...` — ключи записаны во флеш (`/keys.dat`)
- `PROV_VERIFY -> ... mac=...` — подтверждение, что `K_dev` сохранён
  (HMAC-SHA256(K_dev, nonce)[0:16]), проверяется на хосте
- `OK MET-00001 provisioned and verified`
- `BACKEND_ROW MET-00001,<k_dev_hex>,<k_pair_hex>` — строка для бэкенда
- `PAIR_URI met://pair?d=MET-00001&k=<base32>` — то, что кодируется в QR

4. **Сохраните** `BACKEND_ROW` и `PAIR_URI` в надёжном месте. `BACKEND_ROW`
   нужен бэкенду, `PAIR_URI` — для QR.

### 2.2. Ключ шифрования бэкенда (age)

Партия ключей шифруется публичным ключом бэкенда. Сгенерируйте пару ключей
age один раз скриптом [`tools/gen_age_key.py`](tools/gen_age_key.py:1):

```
cd firmware/Met-keychain/tools
python gen_age_key.py                 # -> ./keys/age_public.key
```

- `keys/age_public.key` — публичный ключ (`age1...`), передаётся в
  `provision.py` для шифрования.
- `keys/age_private.key` — приватный ключ (права `600`), нужен бэкенду для
  расшифровки. **Не коммитьте его** (уже в [`.gitignore`](.gitignore:1)).
- `--out ./keys` — другая папка, `--force` — перезаписать существующие ключи.

### 2.3. Вариант B — пакетная генерация для потока

Скрипт [`tools/provision.py`](tools/provision.py:1) генерирует ключи для партии,
делает PNG с QR на каждое устройство и шифрует CSV ключей публичным ключом
бэкенда.

```
cd firmware/Met-keychain/tools
python provision.py --count 10 --start 1 --out ./batch
```

Получатель берётся из `./keys/age_public.key` автоматически. Явно задать его
можно через `--recipient age1...` (age) или `--recipient gpg@example.com`
(gpg), а также `--recipient-file <path>` для другого файла с ключом.

- `--out ./batch` — каталог, куда кладутся `MET-000XX.png` (QR) и
  `backend_keys.csv.age` (зашифрованный CSV).

CSV содержит `dev_no, k_dev_hex, k_pair_hex, pair_uri`. Открытый CSV
**никогда не пишется на диск** — он формируется в памяти и сразу шифруется
(см. [`tools/provision.py`](tools/provision.py:72)).

### 2.4. Расшифровка CSV на бэкенде

Бэкенд расшифровывает `backend_keys.csv.age` приватным ключом
(`keys/age_private.key`):

```
python decrypt_keys.py --in ./batch/backend_keys.csv.age \
    --key ./keys/age_private.key --out ./backend_keys.csv
```

Скрипт [`tools/decrypt_keys.py`](tools/decrypt_keys.py:1) по умолчанию читает
ключ из `./keys/age_private.key`, а результат пишет рядом с входным файлом без
расширения `.age`. Флаг `--stdout` выводит CSV в stdout, не создавая файл.
Полученный CSV содержит все ключи партии — **удалите его после импорта**.

### 2.5. Заливка ключей из зашифрованного CSV

Прошить брелок можно прямо из `backend_keys.csv.age`, не расшифровывая файл на
диск: [`tools/flash_keys.py`](tools/flash_keys.py:194) расшифровывает нужную
строку в памяти (`row_from_age`).

```
python flash_keys.py --list
python flash_keys.py --csv-age ./batch/backend_keys.csv.age --row 3 \
    --age-key ./keys/age_private.key --address AA:BB:CC:DD:EE:FF
```

Приватный ключ по умолчанию берётся из `./keys/age_private.key`. Открытый CSV
на диск не пишется.

Залить конкретную строку из открытого CSV в брелок:

```
python flash_keys.py --csv ./plain.csv --row 3 --address AA:BB:CC:DD:EE:FF
```

Залить явные ключи:

```
python flash_keys.py --devno MET-00001 \
    --kdev 00112233445566778899aabbccddeeff \
    --kpair ffeeddccbbaa99887766554433221100 --address AA:BB:CC:DD:EE:FF
```

Если брелок уже привязан и его надо перепровижинить — добавьте `--reset`.
На непривязанном брелоке это просто `FACTORY_RESET`. На привязанном скрипт
переходит на аутентифицированный сброс `FACTORY_RESET_BEGIN` /
`FACTORY_RESET_FINISH` с доказательством знания `K_dev`
(см. [`PROTOCOL.md`](PROTOCOL.md:58)); `K_dev` по BLE не передаётся.

Скрипт сам перебирает `K_dev` из всех строк расшифрованного CSV (брелок могли
ранее прошить другой строкой), а если нужного ключа там нет — укажите его
явно: `--current-kdev <hex32>` (ключ, которым брелок был провижинен).

> Если и это не помогло (`ERR RESET_FAIL`) — брелок был прошит ключом, которого
> нет в CSV. Доказать знание чужого `K_dev` невозможно. DFU-прошивка (`-t upload`)
> не стирает InternalFS, поэтому `keys.dat` переживает перепрошивку. Без SWD
> одноразово залейте recovery-сборку, которая стирает ключи на старте:
>
> ```
> # плата в DFU (двойной Reset), затем:
> pio run -e recovery -t upload --upload-port COMx
> pio run -t upload --upload-port COMx   # вернуть обычную прошивку
> ```
>
> После этого брелок снова pairing beacon — повторите команду без `--reset`
> (или с `--reset`, теперь сработает обычный `FACTORY_RESET`). Полное стирание
> чипа через `--eraseall` требует SWD-проба.

### 2.6. Отвязка по последовательному порту (демо, без скриптов)

Для демонстрации отвязку можно сделать вообще без ПК-скриптов: прошивка
принимает команды прямо из последовательного порта (USB CDC и UART-зеркало,
115200). Команды **без аутентификации** — это демо-режим.

Подключитесь к порту (`pio device monitor -b 115200`) и введите строку:

```
UNBIND
```

Брелок снимает привязку к владельцу (`K_owner`, флаг `paired`), стирает журнал
встреч и RAM-часы, но **сохраняет провизьон** (`devNo`/`K_dev`/`K_pair`) и `seq`
— его можно привязать заново тем же `K_pair`. В лог выводится
`SERIAL CMD: owner unbound (provision kept)`.

```
FACTORY_RESET
```

Дополнительно стирает и провизьон (как `--reset` в `flash_keys.py`) — брелок
снова чистый pairing beacon.

```
HELP
```

Печатает список доступных serial-команд.

Реализация — [`handleSerialLine()`](src/main.cpp:34), опрос порта —
[`pollSerialCommand()`](src/main.cpp:66). Команды обрабатываются в `loop()`
и работают, пока в [`headers/config.h`](headers/config.h:8) включены
`ENABLE_SERIAL` и/или `ENABLE_UART_MIRROR`.

### 2.7. Полная отвязка владельца без владельца (`unbind.py`)

Скрипт [`tools/unbind.py`](tools/unbind.py:1) снимает привязку к владельцу
(`K_owner`, флаг `paired`), стирает журнал встреч и RAM-часы, но **сохраняет
провизьон** (`devNo`/`K_dev`/`K_pair`) и монотонный `seq`. Брелок возвращается
в режим pairing beacon и его можно привязать заново тем же `K_pair`.

В отличие от `UNPAIR` (раздел 5), этот путь **не требует владельца** и nonce
сервера: он доказывает знание `K_dev` (заводской/серверный секрет, по BLE не
передаётся) через `UNBIND_BEGIN` / `UNBIND_FINISH` — тем же примитивом, что и
`FACTORY_RESET_FINISH` (см. [`PROTOCOL.md`](PROTOCOL.md:58)). Пригодится, когда
ключ владельца потерян, а провизьон нужно сохранить.

1. Посмотрите список найденных брелоков (видны и pairing `type=0x02`, и
   анонимные `type=0x01` beacon'ы):

```
cd firmware/Met-keychain/tools
python unbind.py --list
```

Вывод: `ADDRESS pair|anon devno=... rssi=... name=...`.

2. Отвяжите, указав `K_dev` брелока (hex32) и адрес:

```
python unbind.py --kdev 00112233445566778899aabbccddeeff \
    --address AA:BB:CC:DD:EE:FF
```

`--address` можно не указывать, если в эфире ровно один брелок — скрипт выберет
его сам (иначе требует явный адрес).

3. Либо возьмите `K_dev` прямо из зашифрованного CSV (расшифровка в памяти, на
   диск не пишется):

```
python unbind.py --csv-age ./batch/backend_keys.csv.age --row 3 \
    --age-key ./keys/age_private.key --address AA:BB:CC:DD:EE:FF
```

Также можно указать строку из открытого CSV: `--csv ./plain.csv --row 3`.

При успехе скрипт печатает `UNBIND_FINISH -> OK dev=MET-00001` и
`OK owner unbound (provision kept)`. Если пришло `ERR UNBIND_FAIL` — передан не
тот `K_dev` (не совпадает с текущим на устройстве).

> Чтобы стереть вместе с привязкой и провизьон (`devNo`/`K_dev`/`K_pair`),
> используйте `--reset` в [`tools/flash_keys.py`](tools/flash_keys.py:1)
> (`FACTORY_RESET`/`FACTORY_RESET_BEGIN`/`FACTORY_RESET_FINISH`).

---

## 3. Сборка Android-приложения

Из каталога приложения:

```
cd "demonstration program/ble_android_app"
flutter pub get
```

Зависимости ([`pubspec.yaml`](../../demonstration%20program/ble_android_app/pubspec.yaml:30)):
`flutter_blue_plus`, `permission_handler`, `mobile_scanner`, `crypto`,
`shared_preferences`, `share_plus`, `path_provider`.

Запуск на подключённом телефоне (отладка):

```
flutter run
```

Сборка APK для установки вручную:

```
flutter build apk --release
```

Готовый файл: `build/app/outputs/flutter-apk/app-release.apk`. Перекиньте его на
телефон и установите (нужно разрешить установку из неизвестных источников).

### Разрешения

Приложение запрашивает BLE-разрешения в рантайме
(см. [`lib/main.dart`](../../demonstration%20program/ble_android_app/lib/main.dart:183)).
В манифесте уже объявлены
([`AndroidManifest.xml`](../../demonstration%20program/ble_android_app/android/app/src/main/AndroidManifest.xml:7)):

- `BLUETOOTH_SCAN` (Android 12+), `BLUETOOTH_CONNECT` (Android 12+)
- `BLUETOOTH`, `BLUETOOTH_ADMIN`, `ACCESS_FINE_LOCATION` (Android 11 и ниже)
- `CAMERA` (сканирование QR)

При первом запуске **разрешите** Bluetooth и Камеру, иначе сканирование и QR не
работают.

---

## 4. Привязка брелока к телефону

Порядок соответствует [`PROTOCOL.md`](PROTOCOL.md:97) (раздел «Server order
(pair)»). Приложение — это демонстрационный клиент, часть шагов (номер `nonce`,
проверка квитанции на сервере) выполняется вручную.

### 4.1. Подготовка nonce

Сервер при привязке резервирует брелок и выдаёт одноразовый `nonce`. В демо
приложении `nonce` вводится в поле вручную (вкладка «Управление»).

Штатно `nonce` выдаёт бэкенд. Для лабораторной проверки его можно сгенерировать
скриптом [`tools/gen_nonce.py`](tools/gen_nonce.py:1) — он печатает 16 случайных
байт в hex (тот же формат, что ждёт прошивка в `PAIR_FINISH`/`UNPAIR`):

```
cd firmware/Met-keychain/tools
python gen_nonce.py                 # один nonce (hex32)
python gen_nonce.py --count 5       # пять nonce
python gen_nonce.py --json          # JSON-массив
```

Полученный hex введите в поле **nonce** карточки «Привязка (K_pair)» (шаг 4.3)
или «Отвязка» (раздел 5). Тот же `nonce` отправьте на сервер вместе с
квитанцией — сервер проверяет квитанцию тем же значением.

### 4.2. Сканирование QR

1. Откройте приложение на телефоне.
2. Нажмите **QR-код** и наведите камеру на PNG из `./batch/MET-000XX.png`
   (или на `PAIR_URI` из вывода `flash_keys.py --gen`).
3. Приложение распознаёт URI `met://pair?d=MET-00001&k=<base32>`,
   заполнит номер устройства и `K_pair` и начнёт поиск брелока по номеру
   (см. [`lib/main.dart`](../../demonstration%20program/ble_android_app/lib/main.dart:248)).
4. Когда брелок найден (реклама `type=0x02` с этим номером), приложение
   подключится к нему по BLE (NUS).

Проверить подключение можно в логе: должна появиться строка `HELLO proto=3
fw=... dev=MET-00001 paired=false timeok=false mtu=...`.

### 4.3. Привязка (PAIR)

На вкладке «Управление» в карточке **«Привязка (K_pair)»**:

1. Поля **Номер устройства** и **K_pair** уже заполнены из QR.
2. Введите **nonce** сервера.
3. Поле **UTC** можно оставить пустым — подставится текущее время
   (`DateTime.now()`), либо введите точное значение в секундах.
4. Нажмите **PAIR**.

Приложение:

- генерирует случайный `K_owner` (16 байт),
- выполняет `PAIR_BEGIN` → получает `CHALLENGE`,
- отвечает `PAIR_FINISH <resp> <kowner> <nonce> <utc>`,
- получает `RECEIPT <hex32>` — квитанцию привязки
  (`receipt = HMAC-SHA256(K_rcpt, "paired" ‖ devNo ‖ nonce ‖ SHA256(K_owner))`,
  см. [`PROTOCOL.md`](PROTOCOL.md:85)).

`K_owner` и номер устройства сохраняются в `SharedPreferences` телефона
(см. [`lib/met_client.dart`](../../demonstration%20program/ble_android_app/lib/met_client.dart:121)).

Квитанция появится в логе (`RECEIPT ...`). **Отправьте её на сервер** — сервер
проверит её своим `K_dev` и подтвердит привязку. Без этого шага сервер не
считает брелок привязанным.

После успешной привязки брелок перестаёт рекламироваться как pairing beacon.
Пока телефон не передаст подписанное время (`TIME_SET`), брелок **молчит**
(не выпускает анонимный ID), см. [`PROTOCOL.md`](PROTOCOL.md:42).

### 4.4. Установка времени

1. На вкладке «Управление» в карточке **«Аутентификация (K_owner)»** нажмите
   **AUTH** (K_owner уже подставлен после привязки).
2. В поле **UTC** введите точное время (сек) и нажмите кнопку установки времени.
3. Для подписи времени демо-приложению нужен `K_dev` — введите его в поле
   «K_dev (демо)». В штатной схеме подпись времени выдаёт **сервер** (телефон
   только связной, см. [`task.txt`](task.txt:31)), но для лабораторной проверки
   приложение умеет посчитать `HMAC(K_time, utc ‖ nonce)` само
   (см. [`lib/main.dart`](../../demonstration%20program/ble_android_app/lib/main.dart:409)).

После `TIME_SET` брелок начинает выпускать анонимный ID (`type=0x01`).

---

## 5. Проверка после привязки

1. В логе приложения после повторного `HELLO` должно быть `paired=true`
   и `timeok=true`.
2. Вкладка **«Журнал»** → загрузка `LOG_GET` покажет записи встреч
   (появятся, когда брелок увидит другой брелок в эфире).
3. Вкладка **«Логи»** — весь обмен `<id> CMD` / `<id> TOKEN` / `<id> END|ERR`.
4. Отвязать: ввести `nonce` сервера в карточке **«Отвязка»** и нажать кнопку —
   брелок вернёт квитанцию отвязки, её тоже надо отправить на сервер.

---

## 6. Частые проблемы

| Симптом | Причина / решение |
|---------|-------------------|
| `pio run -t upload` не находит порт | Плата не в DFU. Двойной Reset, затем `--upload-port COMx`. Платы прошиваются без дополнительных скриптов. |
| `There is no debugger connected to the PC` | Цель `erase` в PlatformIO ждёт SWD-проб. Плату не нужно стирать для прошивки — просто `pio run -t upload` в DFU. Полное стирание чипа — только через SWD (`--eraseall`). |
| `flash_keys.py --list` ничего не находит | Брелок привязан (рекламируется молча или как `type=0x01`) либо далеко. Для перепровижининга нужен непривязанный брелок или `--reset`. |
| `ERR BUSY` в логе | Одновременно идёт другая транзакция — протокол однопоточный (см. [`PROTOCOL.md`](PROTOCOL.md:14)). |
| `ERR AUTH_FAIL` | Неверный `K_owner`. При 3 ошибках подряд — пауза 30 с (см. [`headers/config.h`](headers/config.h:56)). |
| Брелок не виден в скане после привязки | Нет валидного времени. Выполните `TIME_SET` (шаг 4.4). |
| QR не сканируется | Разрешите Камеру приложению; проверьте, что PNG из `./batch` не повреждён. |
| `No adapter` / пустой скан | Bluetooth выключен или не выданы BLE-разрешения. Разрешите Bluetooth/Камеру/Location. |
| Подпись времени не проходит | `K_dev` введён неверно, либо nonce не от брелока. Перечитайте `TIME_BEGIN` → `NONCE`. |

---

## 7. Безопасность (важно)

- `K_dev` **никогда** не передаётся по BLE и не хранится в телефоне владельца
  (см. [`task.txt`](task.txt:6)). В демо-поле «K_dev» — только для лабораторных
  тестов; в проде подпись времени и проверку квитанций делает сервер.
- `K_pair` и `K_owner` по воздуху не идут открыто — только challenge-response
  (HMAC-SHA256 со свежим nonce от аппаратного ГСЧ).
- Открытый CSV с ключами партии **не сохраняйте на диск, не шлите в мессенджеры,
  не коммитьте**. Он должен сразу шифроваться публичным ключом бэкенда
  (`age`/`gpg`) — см. [`tools/provision.py`](tools/provision.py:1).
- Приватный ключ age (`keys/age_private.key`, [`tools/gen_age_key.py`](tools/gen_age_key.py:1))
  храните только у бэкенда. Публичный ключ (`keys/age_public.key`) не секретен.
- После расшифровки CSV ([`tools/decrypt_keys.py`](tools/decrypt_keys.py:1))
  удалите открытый файл сразу после импорта в бэкенд.
- Каталоги `keys/`, `tools/batch/` и файлы `*.csv`, `*.csv.age`, `*.key`
  исключены из репозитория ([`.gitignore`](.gitignore:1)).
- Для релиза включите `APPROTECT` (защита чтения) и выключите отладочные логи
  (`ENABLE_SERIAL=0`).
