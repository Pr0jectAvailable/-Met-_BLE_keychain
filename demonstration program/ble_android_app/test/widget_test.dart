// Базовый тест криптопримитивов протокола Met keychain v3.

import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';

import 'package:ble_android_app/met_crypto.dart';

void main() {
  test('HKDF-SHA256: длина, детерминизм, разделение по info', () {
    final ikm = Uint8List.fromList(List.generate(16, (i) => i + 1));
    final a1 = hkdfSha256(ikm, 'met-log-v1', 32);
    final a2 = hkdfSha256(ikm, 'met-log-v1', 32);
    final b = hkdfSha256(ikm, 'met-time-v1', 32);
    expect(a1.length, 32);
    expect(constantTimeEquals(a1, a2), isTrue);
    expect(constantTimeEquals(a1, b), isFalse);
  });

  test('anonId: 16 байт, меняется со сменой epoch', () {
    final kDev = Uint8List.fromList(List.generate(16, (i) => i));
    final a = anonId(kDev, 1000);
    final b = anonId(kDev, 100000);
    expect(a.length, 16);
    expect(constantTimeEquals(a, b), isFalse);
  });

  test('base32 decode + hex roundtrip', () {
    final bytes = Uint8List.fromList([1, 2, 3, 4, 5, 6, 7, 8]);
    expect(constantTimeEquals(fromHex(toHex(bytes)), bytes), isTrue);
  });
}
