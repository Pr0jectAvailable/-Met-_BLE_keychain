# Met keychain — BLE protocol (v3)

Firmware: `FW_VERSION`. Transport: BLE UART (NUS), line-based.

## Framing

```
phone -> MCU : <id> <CMD> [args]\n
MCU  -> phone: <id> <TOKEN> ...\n     (0..N data lines)
               <id> END n=<N>\n        success terminator
               <id> ERR <code> [text]\n failure terminator
```

`id` — transaction id (1..65535), echoed. One transaction at a time; a second
command while busy returns `ERR BUSY`.

## Crypto primitives

- `HMAC-SHA256`, `SHA256`, `HKDF-SHA256` (RFC 5869, empty salt).
- `K_dev` (16 B) never leaves the device.
- Derived keys:
  - `K_log  = HKDF(K_dev, "met-log-v1")`  (32 B) — journal signatures
  - `K_time = HKDF(K_dev, "met-time-v1")` (32 B) — time signatures
  - `K_rcpt = HKDF(K_dev, "met-pair-v1")` (32 B) — pairing receipts
- Anonymous ID: `HMAC-SHA256(K_dev, epoch_be4)[0..15]`, `epoch = floor(utc/900)`.

## Advertising

Manufacturer data (company id `0xFFFF`):

```
FF FF | 'M' 'e' 't' | ver(1) | type | payload
```

- `type=0x01` anonymous: payload = anon ID (16 B). MAC rotates with ID.
- `type=0x02` pairing:    payload = device number (ASCII, e.g. `MET-00001`).

Advertising modes:

- unpaired -> `type=0x02` (pairing beacon).
- paired and time valid -> `type=0x01` (anonymous ID, rotating MAC).
- paired but time invalid (fresh boot, no phone yet) -> **silent**: no
  advertising until the phone supplies signed UTC time via `TIME_SET`.

### Clock

The device has no RTC. It keeps a RAM clock seeded by the phone
(`TIME_SET` sets `utc`). The clock advances with `millis()` and is **not**
persisted: after any reboot it is invalid, so the device stays silent until
the phone connects and re-supplies signed time. Encounter timestamps
(`tFirst`/`tLast`) are taken from this clock.

## Commands

| CMD | Auth | Args | Response |
|-----|------|------|----------|
| `HELLO` | — | — | `HELLO proto fw dev paired timeok mtu` |
| `FACTORY_RESET` | — | — | `OK` — clears K_dev/K_pair/K_owner (refused if paired) |
| `FACTORY_RESET_BEGIN` | — | — | `NONCE <hex16>` — step 1 of authenticated reset |
| `FACTORY_RESET_FINISH` | K_dev | `<hmac32>` | `OK` — step 2; `HMAC-SHA256(K_dev, nonce)`, wipes provision |
| `AUTH_BEGIN` | — | — | `CHALLENGE <hex16>` |
| `AUTH` | — | `<resp16>` | `OK` \| `ERR AUTH_FAIL` |
| `PAIR_BEGIN` | K_pair | — | `CHALLENGE <hex16>` |
| `PAIR_FINISH` | K_pair | `<resp16> <kowner16> <nonce16> <utc>` | `RECEIPT <hex32>` |
| `TIME_BEGIN` | owner | — | `NONCE <hex16>` |
| `TIME_SET` | owner | `<utc> <hmac32>` | `OK` |
| `LOG_GET` | owner | `[offset count]` | `LOG total..`, `ENC ...` |
| `LOG_ACK` | owner | `<seq>` | `ACKED <removed>` |
| `BATTERY` | owner | — | `BAT mv= pct=` |
| `SET_RSSI` | owner | `<dbm>` | `OK` |
| `SET_ADV_INTERVAL` | owner | `<ms>` | `OK` |
| `SET_ADV_TX` | owner | `<dbm>` | `OK` |
| `SET_SCAN` | owner | `<window_ms> <period_ms>` | `OK` |
| `UNPAIR` | owner | `<nonce16>` | `RECEIPT <hex32>` |

"owner" = must pass `AUTH` (challenge-response with `K_owner`) in the session
before the command. `K_owner`/`K_pair` are never sent in clear.

### Journal record tag

```
tag = HMAC-SHA256(K_log, devNo ‖ seq_be4 ‖ id ‖ tFirst_be4 ‖ tLast_be4 ‖ rssi ‖ count_be2)[0..15]
```

`seq` monotonically grows, persisted in flash, never reset (not on reboot, not
on unpair). Server verifies tag and that `seq` exceeds previously accepted ones.

### Pairing receipt

```
receipt = HMAC-SHA256(K_rcpt, "paired" ‖ devNo ‖ nonce ‖ SHA256(K_owner))
```

### Unpair receipt

```
receipt = HMAC-SHA256(K_rcpt, "unpaired" ‖ devNo ‖ nonce)
```

## Server order (pair)

1. App reserves device + obtains nonce from server (server checks free).
2. BLE pairing (`PAIR_BEGIN` -> `PAIR_FINISH`), device returns receipt.
3. App sends receipt to server; server verifies with `K_dev`.
If step 2 fails the server releases the reservation after 10 minutes.

Unpair: nonce from server -> `UNPAIR` -> receipt -> server.
