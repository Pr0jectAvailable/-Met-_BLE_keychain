# Быстрая настройка

## Прошивка

pio run -t upload


## Создание ключа backend (это я для себя делал, хз как вы будете реализовывать)

cd firmware/Met-keychain/tools
python gen_age_key.py

## Генерация QR кода и ключей брелков

cd firmware/Met-keychain/tools
python provision.py --count 2 --start 1 --out ./batch

count - сколько
start - с какого номера начинаем

Если надо узнать ключи
```
 python decrypt_keys.py --in ./batch/backend_keys.csv.age \
    --key ./keys/age_private.key --out ./backend_keys.csv
```

## Прошивка брелков

python flash_keys.py --list 
Даёт лист доступных брелков (шить через пк с bluetooth)

python flash_keys.py --csv-age ./batch/backend_keys.csv.age --row 3 \
    --age-key ./keys/age_private.key --address AA:BB:CC:DD:EE:FF --reset

row - какая строка из ключей брелков берётся
address - адрес устройства которое получили через "python flash_keys.py --list"

## Чё дальше

Как тестировать я покажу на видосе