import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'met_crypto.dart';

/// Nordic UART Service (NUS) — транспорт протокола v3.
const String kNusServiceUuid = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
const String kNusRxCharUuid = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"; // write
const String kNusTxCharUuid = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"; // notify

const int kMaxLine = 256;
const Duration kTxnTimeout = Duration(seconds: 3);

/// Тип рекламного пакета (PROTOCOL.md §Advertising).
enum AdvKind { anonymous, pairing, unknown }

/// Разобранный advertising-пакет ключа.
class MetAdvertisement {
  final AdvKind kind;
  final int version;
  final Uint8List? anonId; // для type=0x01
  final String? deviceNumber; // для type=0x02

  const MetAdvertisement({
    required this.kind,
    this.version = 0,
    this.anonId,
    this.deviceNumber,
  });
}

/// Разбирает manufacturer data (company id 0xFFFF) в MetAdvertisement.
/// Возвращает null, если это не наш формат.
MetAdvertisement? parseMetAdvertisement(Map<int, List<int>> manufacturerData) {
  final raw = manufacturerData[0xFFFF];
  if (raw == null || raw.length < 5) return null;
  // 'M' 'e' 't'
  if (raw[0] != 0x4D || raw[1] != 0x65 || raw[2] != 0x74) return null;
  final version = raw[3];
  final type = raw[4];
  final payload = raw.sublist(5);

  if (type == 0x01) {
    if (payload.length < 16) return null;
    return MetAdvertisement(
      kind: AdvKind.anonymous,
      version: version,
      anonId: Uint8List.fromList(payload.sublist(0, 16)),
    );
  }
  if (type == 0x02) {
    final devNo = utf8.decode(payload, allowMalformed: true).trim();
    return MetAdvertisement(
      kind: AdvKind.pairing,
      version: version,
      deviceNumber: devNo,
    );
  }
  return MetAdvertisement(kind: AdvKind.unknown, version: version);
}

/// Одна запись журнала (протокол v3).
class JournalRecord {
  final int seq;
  final Uint8List id;
  final int tFirst;
  final int tLast;
  final int rssi;
  final int count;
  final Uint8List tag;

  const JournalRecord({
    required this.seq,
    required this.id,
    required this.tFirst,
    required this.tLast,
    required this.rssi,
    required this.count,
    required this.tag,
  });
}

/// Ошибка уровня протокола: терминатор `<id> ERR <code> [text]`.
class MetProtoException implements Exception {
  final String code;
  final String text;
  MetProtoException(this.code, this.text);

  @override
  String toString() => 'ERR $code${text.isEmpty ? '' : ' $text'}';
}

class _TxnOutcome {
  final List<String> data;
  final int? expectedCount;
  final String? error;
  _TxnOutcome({required this.data, this.expectedCount, this.error});
}

class _ActiveTxn {
  final int id;
  final Completer<_TxnOutcome> completer = Completer<_TxnOutcome>();
  final List<String> data = [];
  _ActiveTxn(this.id);
}

/// Состояние сессии с брелоком.
class MetInfo {
  String proto = '';
  String fw = '';
  String dev = '';
  bool paired = false;
  bool timeOk = false;
  int mtu = 0;
}

/// Хранилище учётных данных владельца (K_owner, номер устройства).
class MetCredentials {
  static const _kOwnerKey = 'met.k_owner';
  static const _kDevKey = 'met.dev_no';
  static const _kPairKey = 'met.k_pair';

  /// K_owner в hex (может быть null, если брелок ещё не привязан).
  static Future<String?> get kOwnerHex async {
    final sp = await SharedPreferences.getInstance();
    return sp.getString(_kOwnerKey);
  }

  static Future<String?> get deviceNumber async {
    final sp = await SharedPreferences.getInstance();
    return sp.getString(_kDevKey);
  }

  static Future<String?> get kPairHex async {
    final sp = await SharedPreferences.getInstance();
    return sp.getString(_kPairKey);
  }

  static Future<void> saveOwner({
    required String deviceNumber,
    required String kOwnerHex,
  }) async {
    final sp = await SharedPreferences.getInstance();
    await sp.setString(_kOwnerKey, kOwnerHex);
    await sp.setString(_kDevKey, deviceNumber);
  }

  static Future<void> saveKPair(String kPairHex) async {
    final sp = await SharedPreferences.getInstance();
    await sp.setString(_kPairKey, kPairHex);
  }

  static Future<void> clearOwner() async {
    final sp = await SharedPreferences.getInstance();
    await sp.remove(_kOwnerKey);
  }
}

/// Клиент протокола Met keychain v3 поверх BLE UART.
class MetClient {
  final void Function(String direction, String message) onLog;
  final BluetoothDevice device;

  BluetoothCharacteristic? _rx;
  BluetoothCharacteristic? _tx;
  StreamSubscription? _dataSub;
  StreamSubscription? _connSub;

  final List<int> _rxBytes = [];
  int _txId = 0;
  _ActiveTxn? _active;
  Future<void> _chain = Future.value();

  int _mtu = 23;
  int get maxChunkSize => _mtu - 3;

  final MetInfo info = MetInfo();

  MetClient({required this.device, required this.onLog});

  bool get isConnected => _rx != null && _tx != null;

  /// Подключение, обнаружение NUS и согласование MTU.
  Future<void> connect() async {
    await device.connect(timeout: const Duration(seconds: 20));
    // Ждём подтверждения состояния connected: сразу после connect() стек
    // Android ещё не готов к GATT-операциям, из-за чего первый
    // discoverServices()/requestMtu() часто падает.
    try {
      await device.connectionState
          .firstWhere((s) => s == BluetoothConnectionState.connected)
          .timeout(const Duration(seconds: 5));
    } catch (_) {}
    _connSub = device.connectionState.listen((s) {
      if (s == BluetoothConnectionState.disconnected) {
        onLog('--', 'Соединение потеряно');
      }
    });

    // Сначала обнаружение сервисов, затем MTU: requestMtu до discoverServices
    // на Android нестабилен и может рвать соединение.
    // Обнаружение сервисов с ретраями — первый вызов может пройти до того,
    // как GATT готов, и вернуть пустой список.
    List<BluetoothService> services = const [];
    for (var i = 0; i < 3; i++) {
      try {
        services = await device.discoverServices();
        if (services.isNotEmpty) break;
      } catch (e) {
        onLog('--', 'discoverServices попытка ${i + 1}: $e');
      }
      await Future.delayed(const Duration(milliseconds: 400));
    }
    BluetoothCharacteristic? rx;
    BluetoothCharacteristic? tx;
    for (final s in services) {
      if (s.uuid.toString().toUpperCase() == kNusServiceUuid) {
        for (final c in s.characteristics) {
          final u = c.uuid.toString().toUpperCase();
          if (u == kNusRxCharUuid) rx = c;
          if (u == kNusTxCharUuid) tx = c;
        }
      }
    }
    if (rx == null || tx == null) {
      throw Exception('NUS-характеристики не найдены');
    }
    _mtu = await _negotiateMtu();
    _rx = rx;
    _tx = tx;
    _rxBytes.clear();
    _dataSub = tx.onValueReceived.listen(_onData);
    await tx.setNotifyValue(true);
    // Даём стеку BLE завершить подписку и открыть поток, иначе первый
    // HELLO (write) может уйти в никуда сразу после подключения.
    await Future.delayed(const Duration(milliseconds: 250));
  }

  Future<void> disconnect() async {
    await _dataSub?.cancel();
    await _connSub?.cancel();
    _dataSub = null;
    _connSub = null;
    try {
      await _tx?.setNotifyValue(false);
    } catch (_) {}
    _rx = null;
    _tx = null;
    await device.disconnect();
  }

  Future<int> _negotiateMtu() async {
    const target = 247;
    for (var i = 0; i < 3; i++) {
      try {
        final m = await device.requestMtu(target);
        if (m > 23) return m;
      } catch (_) {}
      await Future.delayed(const Duration(milliseconds: 250));
      final cur = device.mtuNow;
      if (cur > 23) return cur;
    }
    final fb = device.mtuNow;
    return fb > 23 ? fb : 23;
  }

  // ---------- Приём ----------
  void _onData(List<int> data) {
    _rxBytes.addAll(data);
    while (true) {
      final nl = _rxBytes.indexOf(0x0A);
      if (nl < 0) {
        if (_rxBytes.length > kMaxLine) {
          _rxBytes.clear();
          onLog('--', 'Переполнение RX, сброс');
        }
        break;
      }
      final lineBytes = _rxBytes.sublist(0, nl);
      _rxBytes.removeRange(0, nl + 1);
      final line = utf8.decode(lineBytes, allowMalformed: true).replaceAll('\r', '');
      if (line.isNotEmpty) {
        onLog('← RX', line);
        _handleLine(line);
      }
    }
  }

  void _handleLine(String line) {
    final sp = line.indexOf(' ');
    if (sp <= 0) return;
    final id = int.tryParse(line.substring(0, sp));
    if (id == null) return;
    final active = _active;
    if (active == null || active.id != id) return;
    final rest = line.substring(sp + 1);

    if (rest == 'END' || rest.startsWith('END ')) {
      final m = RegExp(r'^END\s+n=(\d+)$').firstMatch(rest);
      final n = m != null ? int.parse(m.group(1)!) : null;
      if (!active.completer.isCompleted) {
        active.completer.complete(
            _TxnOutcome(data: List.of(active.data), expectedCount: n));
      }
      return;
    }
    if (rest == 'ERR' || rest.startsWith('ERR ')) {
      if (!active.completer.isCompleted) {
        active.completer.complete(
            _TxnOutcome(data: List.of(active.data), error: rest));
      }
      return;
    }
    active.data.add(rest);
  }

  // ---------- Транзакции ----------
  int _nextId() {
    _txId = (_txId >= 65535) ? 1 : _txId + 1;
    return _txId;
  }

  Future<T> _serialize<T>(Future<T> Function() action) {
    final completer = Completer<T>();
    _chain = _chain.then((_) async {
      try {
        completer.complete(await action());
      } catch (e, s) {
        completer.completeError(e, s);
      }
    });
    return completer.future;
  }

  Future<List<String>> transact(String cmd, {bool retryable = false}) =>
      _serialize(() => _transactOnce(cmd, retryable: retryable));

  Future<List<String>> _transactOnce(String cmd,
      {required bool retryable}) async {
    const maxAttempts = 3;
    Object? lastError;

    for (var attempt = 1; attempt <= maxAttempts; attempt++) {
      if (_rx == null) throw StateError('Нет характеристики записи');
      final id = _nextId();
      final active = _ActiveTxn(id);
      _active = active;
      onLog('→ TX', '$id $cmd');

      try {
        await _writeLine('$id $cmd');
      } catch (e) {
        if (identical(_active, active)) _active = null;
        rethrow;
      }

      _TxnOutcome outcome;
      try {
        outcome = await active.completer.future.timeout(kTxnTimeout);
      } on TimeoutException {
        if (identical(_active, active)) _active = null;
        lastError = Exception('Таймаут ответа (id=$id)');
        onLog('--', 'Таймаут id=$id');
        if (retryable && attempt < maxAttempts) continue;
        throw lastError;
      } finally {
        if (identical(_active, active)) _active = null;
      }

      if (outcome.error != null) {
        final parts = outcome.error!.split(' ');
        final code = parts.length >= 2 ? parts[1] : 'INTERNAL';
        final text = parts.length >= 3 ? parts.sublist(2).join(' ') : '';
        if (retryable &&
            attempt < maxAttempts &&
            (code == 'BUSY' || code == 'INTERNAL')) {
          onLog('--', '$code, повтор id=$id');
          continue;
        }
        throw MetProtoException(code, text);
      }

      return outcome.data;
    }
    throw lastError ?? Exception('Транзакция не удалась');
  }

  Future<void> _writeLine(String line) async {
    final char = _rx;
    if (char == null) throw StateError('Нет характеристики записи');
    final data = utf8.encode('$line\n');
    if (data.length > maxChunkSize) {
      throw Exception(
          'Команда длиннее MTU−3: ${data.length} > $maxChunkSize (MTU=$_mtu)');
    }
    // write with response: без подтверждения BLE-стек может молча потерять
    // команду сразу после подключения — отсюда таймауты и нестабильность.
    await char.write(data, withoutResponse: false);
  }

  // ---------- Команды ----------
  Future<void> hello() async {
    // retryable: первая команда после подписки может потеряться на BLE-стеке.
    final lines = await transact('HELLO', retryable: true);
    var ok = false;
    for (final l in lines) {
      if (l.startsWith('HELLO')) {
        info.proto = _kv(l, 'proto') ?? '';
        info.fw = _kv(l, 'fw') ?? '';
        info.dev = _kv(l, 'dev') ?? '';
        info.paired = (_kv(l, 'paired') ?? '0') == '1';
        info.timeOk = (_kv(l, 'timeok') ?? '0') == '1';
        info.mtu = int.tryParse(_kv(l, 'mtu') ?? '') ?? 0;
        ok = true;
      }
    }
    // Пустой/неразобранный ответ => соединение «сырое», пусть _connect повторит.
    if (!ok) throw Exception('HELLO без ответа устройства');
  }

  /// AUTH (владелец): challenge-response с K_owner.
  Future<void> auth(List<int> kOwner) async {
    final lines = await transact('AUTH_BEGIN');
    final challenge = _challenge(lines);
    // Прошивка ждёт ровно 16 байт (parseHex resp16) и сравнивает первые 16
    // байт HMAC — полный 32-байтный ответ даёт BAD_ARG resp16.
    final resp = hmacSha256(kOwner, challenge).sublist(0, 16);
    await transact('AUTH ${toHex(resp)}');
  }

  /// PAIR_FINISH: передаёт K_owner, nonce сервера и подписанное время.
  Future<String> pairFinish({
    required List<int> kPair,
    required List<int> kOwner,
    required List<int> nonce,
    required int utc,
  }) async {
    final begin = await transact('PAIR_BEGIN');
    final challenge = _challenge(begin);
    // Прошивка ожидает ровно 16 байт (см. parseHex в ble_uart_module.cpp)
    // и сравнивает только первые 16 байт HMAC (memcmp(...,16)).
    final resp = hmacSha256(kPair, challenge).sublist(0, 16);
    final lines = await transact(
      'PAIR_FINISH ${toHex(resp)} ${toHex(kOwner)} ${toHex(nonce)} $utc',
    );
    final receipt = _receipt(lines);
    return receipt;
  }

  /// TIME_SET (владелец): передаёт UTC и HMAC(K_time, utc ‖ nonce).
  Future<void> timeSet({
    required List<int> kDev,
    required int utc,
  }) async {
    final begin = await transact('TIME_BEGIN');
    final nonce = _nonce(begin);
    final kTime = deriveKTime(kDev);
    final sig = timeSignature(kTime, utc, nonce);
    await transact('TIME_SET $utc ${toHex(sig)}');
  }

  /// LOG_GET: страница записей журнала.
  Future<({int total, List<JournalRecord> records})> logGet({
    int? offset,
    int? count,
  }) async {
    final cmd = (offset == null || count == null)
        ? 'LOG_GET'
        : 'LOG_GET $offset $count';
    final lines = await transact(cmd);
    var total = 0;
    final out = <JournalRecord>[];
    for (final l in lines) {
      if (l.startsWith('LOG total=')) {
        total = int.tryParse(_kv(l, 'total') ?? '') ?? 0;
      } else if (l.startsWith('ENC ')) {
        final rec = _parseEnc(l.substring(4).trim());
        if (rec != null) out.add(rec);
      }
    }
    return (total: total, records: out);
  }

  /// LOG_ACK: подтверждение приёма до seq (телефон шлёт после OK сервера).
  Future<int> logAck(int seq) async {
    final lines = await transact('LOG_ACK $seq');
    for (final l in lines) {
      if (l.startsWith('ACKED')) {
        return int.tryParse(_kv(l, 'removed') ?? '') ?? 0;
      }
    }
    return 0;
  }

  Future<({int mv, int pct})> battery() async {
    final lines = await transact('BATTERY');
    var mv = 0;
    var pct = 0;
    for (final l in lines) {
      if (l.startsWith('BAT')) {
        mv = int.tryParse(_kv(l, 'mv') ?? '') ?? 0;
        pct = int.tryParse(_kv(l, 'pct') ?? '') ?? 0;
      }
    }
    return (mv: mv, pct: pct);
  }

  Future<void> setRssi(int dbm) => transact('SET_RSSI $dbm');
  Future<void> setAdvInterval(int ms) => transact('SET_ADV_INTERVAL $ms');
  Future<void> setAdvTx(int dbm) => transact('SET_ADV_TX $dbm');
  Future<void> setScan(int windowMs, int periodMs) =>
      transact('SET_SCAN $windowMs $periodMs');

  /// UNPAIR: принимает nonce сервера, возвращает квитанцию.
  Future<String> unpair(List<int> nonce) async {
    final lines = await transact('UNPAIR ${toHex(nonce)}');
    return _receipt(lines);
  }

  // ---------- Разбор ответов ----------
  String? _kv(String line, String key) {
    final m = RegExp('$key=([^ ]+)').firstMatch(line);
    return m?.group(1);
  }

  Uint8List _challenge(List<String> lines) {
    for (final l in lines) {
      if (l.startsWith('CHALLENGE ')) {
        return fromHex(l.substring('CHALLENGE '.length).trim());
      }
    }
    throw Exception('Не получен CHALLENGE');
  }

  Uint8List _nonce(List<String> lines) {
    for (final l in lines) {
      if (l.startsWith('NONCE ')) {
        return fromHex(l.substring('NONCE '.length).trim());
      }
    }
    throw Exception('Не получен NONCE');
  }

  String _receipt(List<String> lines) {
    for (final l in lines) {
      if (l.startsWith('RECEIPT ')) {
        return l.substring('RECEIPT '.length).trim();
      }
    }
    throw Exception('Не получен RECEIPT');
  }

  /// Формат строки ENC: seq id tFirst tLast rssi count tag
  JournalRecord? _parseEnc(String s) {
    final p = s.split(RegExp(r'\s+'));
    if (p.length < 7) return null;
    try {
      return JournalRecord(
        seq: int.parse(p[0]),
        id: fromHex(p[1]),
        tFirst: int.parse(p[2]),
        tLast: int.parse(p[3]),
        rssi: int.parse(p[4]),
        count: int.parse(p[5]),
        tag: fromHex(p[6]),
      );
    } catch (_) {
      return null;
    }
  }
}
