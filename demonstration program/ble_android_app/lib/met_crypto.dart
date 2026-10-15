import 'dart:convert';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';

/// Конкатенация байтовых последовательностей.
Uint8List concatBytes(List<List<int>> parts) {
  var total = 0;
  for (final p in parts) {
    total += p.length;
  }
  final out = Uint8List(total);
  var o = 0;
  for (final p in parts) {
    out.setRange(o, o + p.length, p);
    o += p.length;
  }
  return out;
}

Uint8List u16be(int v) => Uint8List.fromList([(v >> 8) & 0xFF, v & 0xFF]);

Uint8List u32be(int v) => Uint8List.fromList(
    [(v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF]);

Uint8List u64be(int v) {
  final b = Uint8List(8);
  for (var i = 0; i < 8; i++) {
    b[i] = (v >> (8 * (7 - i))) & 0xFF;
  }
  return b;
}

Uint8List sha256Bytes(List<int> data) =>
    Uint8List.fromList(sha256.convert(data).bytes);

Uint8List hmacSha256(List<int> key, List<int> data) =>
    Uint8List.fromList(Hmac(sha256, key).convert(data).bytes);

/// HKDF-SHA256 (RFC 5869) с пустой солью.
Uint8List hkdfSha256(List<int> ikm, String info, int length) {
  final prk = hmacSha256(Uint8List(32), ikm);
  final infoBytes = utf8.encode(info);
  final out = <int>[];
  var t = <int>[];
  var counter = 1;
  while (out.length < length) {
    t = hmacSha256(prk, concatBytes([t, infoBytes, [counter]]));
    out.addAll(t);
    counter++;
  }
  return Uint8List.fromList(out.sublist(0, length));
}

Uint8List deriveKLog(List<int> kDev) => hkdfSha256(kDev, 'met-log-v1', 32);
Uint8List deriveKTime(List<int> kDev) => hkdfSha256(kDev, 'met-time-v1', 32);
Uint8List deriveKRcpt(List<int> kDev) => hkdfSha256(kDev, 'met-pair-v1', 32);

/// Анонимный ID: HMAC-SHA256(K_dev, epoch_be4)[0..15].
Uint8List anonId(List<int> kDev, int utcSeconds) {
  final epoch = utcSeconds ~/ 900;
  final mac = hmacSha256(kDev, u32be(epoch));
  return Uint8List.sublistView(mac, 0, 16);
}

/// tag = HMAC-SHA256(K_log, devNo ‖ seq_be4 ‖ id ‖ tFirst_be4 ‖ tLast_be4 ‖ rssi ‖ count_be2)[0..15]
Uint8List journalTag({
  required List<int> kLog,
  required String deviceNumber,
  required int seq,
  required List<int> id,
  required int tFirst,
  required int tLast,
  required int rssi,
  required int count,
}) {
  final data = concatBytes([
    utf8.encode(deviceNumber),
    u32be(seq),
    id,
    u32be(tFirst),
    u32be(tLast),
    [rssi & 0xFF],
    u16be(count),
  ]);
  return Uint8List.sublistView(hmacSha256(kLog, data), 0, 16);
}

/// receipt = HMAC-SHA256(K_rcpt, "paired" ‖ devNo ‖ nonce ‖ SHA256(K_owner))
Uint8List pairReceipt({
  required List<int> kRcpt,
  required String deviceNumber,
  required List<int> nonce,
  required List<int> kOwner,
}) {
  final data = concatBytes([
    utf8.encode('paired'),
    utf8.encode(deviceNumber),
    nonce,
    sha256Bytes(kOwner),
  ]);
  return hmacSha256(kRcpt, data);
}

/// receipt = HMAC-SHA256(K_rcpt, "unpaired" ‖ devNo ‖ nonce)
Uint8List unpairReceipt({
  required List<int> kRcpt,
  required String deviceNumber,
  required List<int> nonce,
}) {
  final data = concatBytes([
    utf8.encode('unpaired'),
    utf8.encode(deviceNumber),
    nonce,
  ]);
  return hmacSha256(kRcpt, data);
}

/// HMAC(K_time, utc_be4 ‖ nonce[0..4]) — подпись времени сервером.
/// Прошивка использует только первые 4 байта nonce (см. TIME_SET в
/// ble_uart_module.cpp: msg = utc_be4 ‖ s_challenge[0..4], длина 8).
Uint8List timeSignature(List<int> kTime, int utcSeconds, List<int> nonce) =>
    hmacSha256(kTime, concatBytes([u32be(utcSeconds), nonce.sublist(0, 4)]));

String toHex(List<int> b) =>
    b.map((x) => x.toRadixString(16).padLeft(2, '0')).join();

Uint8List fromHex(String s) {
  final clean = s.replaceAll(RegExp(r'[^0-9a-fA-F]'), '');
  final out = Uint8List(clean.length ~/ 2);
  for (var i = 0; i < out.length; i++) {
    out[i] = int.parse(clean.substring(i * 2, i * 2 + 2), radix: 16);
  }
  return out;
}

/// Base32 (RFC 4648, без padding).
Uint8List base32Decode(String input) {
  const alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567';
  final s = input.toUpperCase().replaceAll(RegExp(r'[^A-Z2-7]'), '');
  var bits = 0;
  var value = 0;
  final out = <int>[];
  for (final ch in s.split('')) {
    final idx = alphabet.indexOf(ch);
    if (idx < 0) continue;
    value = (value << 5) | idx;
    bits += 5;
    if (bits >= 8) {
      bits -= 8;
      out.add((value >> bits) & 0xFF);
    }
  }
  return Uint8List.fromList(out);
}

/// Сравнение без утечки по времени.
bool constantTimeEquals(List<int> a, List<int> b) {
  if (a.length != b.length) return false;
  var diff = 0;
  for (var i = 0; i < a.length; i++) {
    diff |= a[i] ^ b[i];
  }
  return diff == 0;
}
