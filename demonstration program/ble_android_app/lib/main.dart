import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:path_provider/path_provider.dart';
import 'package:share_plus/share_plus.dart';

// Nordic UART Service — UUID, которые использует BLEUart из Adafruit
const String kServiceUuid = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
const String kRxCharUuid = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"; // write
const String kTxCharUuid = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"; // notify

/// Максимальная длина строки протокола в байтах (protocol_ble.txt: MAX_LINE).
const int kMaxLine = 200;

/// Размер страницы журнала (не зависит от MTU, protocol_ble.txt: п.5).
const int kDevicesPage = 10;

/// Таймаут ожидания терминатора END/ERR (protocol_ble.txt: 2 с).
const Duration kTxnTimeout = Duration(seconds: 2);

/// 16-бит UUID вещания ключа (прошивка: baseUuid16 = 0x0201).
/// В эфире передаётся little-endian (0x01, 0x02), но flutter_blue_plus
/// нормализует 16-бит UUID до полной 128-бит формы BLE base UUID.
const int kBaseUuid16 = 0x0201;
const String kBaseUuid16Full = "00000201-0000-1000-8000-00805F9B34FB";

/// Одна запись журнала.
///
/// Новый протокол передаёт компактный JSON с короткими ключами
/// (m — MAC, r — RSSI, t — время, n — имя). Для совместимости со старыми
/// прошивками поддерживаются и длинные ключи (mac/rssi/time/name).
class DeviceRecord {
  final String mac;
  final int rssi;
  final int time;
  final String name;

  const DeviceRecord({
    required this.mac,
    required this.rssi,
    required this.time,
    required this.name,
  });

  factory DeviceRecord.fromJson(Map<String, dynamic> j) {
    return DeviceRecord(
      mac: (j['m'] ?? j['mac'] ?? '').toString(),
      rssi: _asInt(j['r'] ?? j['rssi']),
      time: _asInt(j['t'] ?? j['time']),
      name: (j['n'] ?? j['name'] ?? '').toString(),
    );
  }

  Map<String, dynamic> toJson() => {
        'm': mac,
        'r': rssi,
        't': time,
        if (name.isNotEmpty) 'n': name,
      };

  DeviceRecord copyWith({String? name}) => DeviceRecord(
        mac: mac,
        rssi: rssi,
        time: time,
        name: name ?? this.name,
      );

  static int _asInt(dynamic v) {
    if (v is int) return v;
    if (v is num) return v.toInt();
    return int.tryParse(v?.toString() ?? '') ?? 0;
  }
}

/// Ошибка уровня протокола: терминатор `<id> ERR <код> [текст]`.
class ProtoException implements Exception {
  final String code;
  final String text;
  ProtoException(this.code, this.text);

  @override
  String toString() => 'ERR $code${text.isEmpty ? '' : ' $text'}';
}

/// Итог одной транзакции.
class _TxnOutcome {
  final List<String> data; // строки без <id>: "<TOKEN> [данные]"
  final int? expectedCount; // из END n=<N>
  final String? error; // строка ERR без id

  _TxnOutcome({required this.data, this.expectedCount, this.error});
}

/// Активная транзакция: id и сборщик строк.
class _ActiveTxn {
  final int id;
  final Completer<_TxnOutcome> completer = Completer<_TxnOutcome>();
  final List<String> data = [];
  _ActiveTxn(this.id);
}

void main() => runApp(const MyApp());

class MyApp extends StatelessWidget {
  const MyApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'BLE Tracker',
      theme: ThemeData(primarySwatch: Colors.blue),
      home: const HomePage(),
    );
  }
}

class HomePage extends StatefulWidget {
  const HomePage({super.key});

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  // ---------- Состояние подключения ----------
  BluetoothDevice? _device;
  BluetoothCharacteristic? _rxChar;
  BluetoothCharacteristic? _txChar;
  StreamSubscription? _scanSub;
  StreamSubscription? _dataSub;
  StreamSubscription? _connSub;

  bool _isScanning = false;
  bool _isConnected = false;
  bool _isConnecting = false;
  List<ScanResult> _scanResults = [];
  bool _onlyKeychain = false; // фильтр по 16-бит UUID ключа (0x0201)
  // Порог RSSI (dBm): показывать только устройства с сигналом не слабее.
  // -100 = без отсечения, -40 = только очень близкие.
  int _rssiThreshold = -100;

  // ---------- Приём ----------
  // Буфер байт до '\n'. Ограничен kMaxLine; переполнение сбрасывает буфер.
  final List<int> _rxBytes = [];

  // ---------- Транзакции ----------
  int _txId = 0;
  _ActiveTxn? _active;
  Future<void> _txnChain = Future.value();

  // Данные, полученные от MCU
  String _mcuName = "";
  String _mcuMac = "";
  String _proto = "";
  String _fw = "";
  int _mcuMtu = 0;

  // ---------- Журнал ----------
  List<DeviceRecord> _deviceRecords = [];
  int _devicesTotal = 0;
  bool _isFetchingDevices = false;

  // ---------- Логи BLE ----------
  final List<String> _bleLogs = [];
  static const int _maxBleLogs = 500;
  final ScrollController _bleLogsScroll = ScrollController();
  bool _bleLogsAutoScroll = true;

  // MTU по умолчанию (BLE 4.0) и фактический после согласования
  int _mtu = 23;
  int get _maxChunkSize => _mtu - 3; // 3 байта ATT-заголовка

  final TextEditingController _nameController = TextEditingController();
  final TextEditingController _macController = TextEditingController();

  // ---------- Lifecycle ----------
  @override
  void initState() {
    super.initState();
    _requestPermissions();
  }

  @override
  void dispose() {
    _scanSub?.cancel();
    _dataSub?.cancel();
    _connSub?.cancel();
    _device?.disconnect();
    _nameController.dispose();
    _macController.dispose();
    _bleLogsScroll.dispose();
    super.dispose();
  }

  // ---------- Логирование BLE ----------
  void _addBleLog(String direction, String message) {
    if (!mounted) return;
    final now = DateTime.now();
    final ts = "${_two(now.hour)}:${_two(now.minute)}:${_two(now.second)}"
        ".${now.millisecond.toString().padLeft(3, '0')}";
    setState(() {
      _bleLogs.add("$ts  $direction  $message");
      if (_bleLogs.length > _maxBleLogs) {
        _bleLogs.removeRange(0, _bleLogs.length - _maxBleLogs);
      }
    });
    if (_bleLogsAutoScroll) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (_bleLogsScroll.hasClients) {
          _bleLogsScroll.jumpTo(_bleLogsScroll.position.maxScrollExtent);
        }
      });
    }
  }

  // ---------- Разрешения ----------
  Future<void> _requestPermissions() async {
    await [
      Permission.bluetoothScan,
      Permission.bluetoothConnect,
      Permission.locationWhenInUse,
    ].request();
  }

  // ---------- Сканирование ----------
  Future<void> _startScan() async {
    setState(() {
      _scanResults = [];
      _isScanning = true;
    });

    if (await FlutterBluePlus.adapterState.first !=
        BluetoothAdapterState.on) {
      _showSnack("Включите Bluetooth");
      setState(() => _isScanning = false);
      return;
    }

    if (!(await FlutterBluePlus.isSupported)) {
      _showSnack("BLE не поддерживается устройством");
      setState(() => _isScanning = false);
      return;
    }

    await _scanSub?.cancel();
    await FlutterBluePlus.stopScan();

    _scanSub = FlutterBluePlus.scanResults.listen((results) {
      if (!mounted) return;
      _syncNamesFromAdvertisements(results);
      setState(() => _scanResults = results);
    });

    try {
      // flutter_blue_plus всегда сканирует активно: на Android это
      // SCAN_MODE_* + запрос SCAN_RSP, на iOS — scanForPeripherals.
      // Поэтому имя из scan response (ScanResponse.addName()) попадёт в
      // advName ниже. continuousUpdates нужен, чтобы поймать scan response,
      // пришедший отдельным пакетом уже после основного ADV.
      await FlutterBluePlus.startScan(
        timeout: const Duration(seconds: 15),
        androidScanMode: AndroidScanMode.lowLatency,
        continuousUpdates: true,
      );
    } catch (e) {
      _showSnack("Ошибка сканирования: $e");
      if (mounted) setState(() => _isScanning = false);
      return;
    }

    if (mounted) setState(() => _isScanning = false);
  }

  /// Проверяет, вещает ли устройство 16-бит UUID ключа (baseUuid16 = 0x0201).
  /// FBP может отдать UUID как в короткой 16-бит форме («0201»), так и в
  /// полной 128-бит base-форме, поэтому сравниваем нормализованный hex.
  bool _isKeychain(AdvertisementData adv) {
    for (final g in adv.serviceUuids) {
      final s = g.toString().toUpperCase().replaceAll(RegExp(r'[^0-9A-F]'), '');
      if (s == '0201') return true; // короткая 16-бит форма
      if (s.startsWith('00000201')) return true; // полная 128-бит base-форма
    }
    return false;
  }

  /// Приводит MAC к верхнему регистру без разделителей для сравнения.
  String _normMac(String s) =>
      s.toUpperCase().replaceAll(RegExp(r'[^0-9A-F]'), '');

  /// Обновляет имена известных устройств по свежим ADV.
  ///
  /// Если в журнале уже есть запись с таким же MAC, а в эфире пришло
  /// непустое имя (advName из ADV/scan response или platformName) —
  /// заменяем имя в записи, чтобы список показывал актуальное значение.
  void _syncNamesFromAdvertisements(List<ScanResult> results) {
    if (_deviceRecords.isEmpty) return;

    // MAC -> самое свежее непустое имя из эфира.
    final byMac = <String, String>{};
    for (final r in results) {
      final advName = r.advertisementData.advName;
      final name = advName.isNotEmpty ? advName : r.device.platformName;
      if (name.isEmpty) continue;
      byMac[_normMac(r.device.remoteId.toString())] = name;
    }
    if (byMac.isEmpty) return;

    bool changed = false;
    final updated = <DeviceRecord>[];
    for (final rec in _deviceRecords) {
      final fresh = byMac[_normMac(rec.mac)];
      if (fresh != null && fresh != rec.name) {
        updated.add(rec.copyWith(name: fresh));
        changed = true;
      } else {
        updated.add(rec);
      }
    }
    if (changed) _deviceRecords = updated;
  }

  /// Результаты сканирования с учётом фильтров «только ключи» и порога RSSI.
  /// Отсортированы по убыванию RSSI: сильные сигналы (близкие устройства)
  /// идут первыми.
  List<ScanResult> get _visibleScanResults {
    var list = _scanResults.where((r) => r.rssi >= _rssiThreshold);
    if (_onlyKeychain) {
      list = list.where((r) => _isKeychain(r.advertisementData));
    }
    return list.toList()..sort((a, b) => b.rssi.compareTo(a.rssi));
  }

  /// Согласование MTU с повтором и проверкой.
  ///
  /// Android: requestMtu шлёт ATT_EXCHANGE_MTU_REQ, FBP ждёт onMtuChanged
  /// и возвращает согласованное значение. iOS: requestMtu — no-op, MTU
  /// поднимается стеком сам, поэтому читаем device.mtuNow (FBP обновляет его
  /// из maximumWriteValueLength). Возвращает реальный MTU, не меньше 23.
  Future<int> _negotiateMtu(BluetoothDevice device) async {
    const int target = 247; // PROTO_MTU_REPORT на MCU
    for (int i = 0; i < 3; i++) {
      try {
        final int m = await device.requestMtu(target);
        if (m > 23) {
          debugPrint("MTU согласован: $m (попытка ${i + 1})");
          return m;
        }
      } catch (e) {
        debugPrint("requestMtu не удался (попытка ${i + 1}): $e");
      }
      // Даём стеку время применить/сообщить новый MTU (особенно iOS).
      await Future.delayed(const Duration(milliseconds: 300));
      final int cur = device.mtuNow;
      if (cur > 23) {
        debugPrint("MTU из стека: $cur");
        return cur;
      }
    }
    final int fallback = device.mtuNow;
    debugPrint("MTU остался $fallback (согласование не удалось)");
    return fallback > 23 ? fallback : 23;
  }

  // ---------- Подключение с retry ----------
  Future<void> _connect(BluetoothDevice device) async {
    if (_isConnecting) return;
    _isConnecting = true;
    if (mounted) setState(() => _isConnecting = true);

    const maxAttempts = 3;
    try {
      for (int attempt = 1; attempt <= maxAttempts; attempt++) {
        try {
          await FlutterBluePlus.stopScan();
          await _scanSub?.cancel();
          _scanSub = null;

          if (device.isConnected) {
            await device.disconnect();
            await Future.delayed(const Duration(milliseconds: 250));
          }

          await device.connect(
            timeout: const Duration(seconds: 20),
            autoConnect: false,
          );

          await Future.delayed(const Duration(milliseconds: 150));

          // ---- Согласование MTU (обязательно, ДО HELLO) ----
          // MCU в HELLO сообщает mtu=247 (PROTO_MTU_REPORT) и режет строки
          // по currentMtu()−3. Телефон обязан согласовать MTU до первой
          // команды, иначе строки длиннее 20 байт (напр. SET_NAME) падают
          // ещё в _writeLine(). _negotiateMtu() делает повтор и проверку.
          _mtu = await _negotiateMtu(device);
          debugPrint("Рабочий MTU: $_mtu (payload ${_mtu - 3})");

          _device = device;
          if (mounted) setState(() => _mcuMac = "");

          await _connSub?.cancel();
          _connSub = device.connectionState.listen((state) {
            if (state == BluetoothConnectionState.disconnected) {
              if (mounted) {
                setState(() {
                  _isConnected = false;
                  _device = null;
                  _rxChar = null;
                  _txChar = null;
                });
                _showSnack("Соединение потеряно");
              }
            }
          });

          final services = await device.discoverServices();

          BluetoothCharacteristic? rx;
          BluetoothCharacteristic? tx;
          for (final s in services) {
            if (s.uuid.toString().toUpperCase() == kServiceUuid) {
              for (final c in s.characteristics) {
                final u = c.uuid.toString().toUpperCase();
                if (u == kRxCharUuid) rx = c;
                if (u == kTxCharUuid) tx = c;
              }
            }
          }

          if (rx == null || tx == null) {
            throw Exception("NUS-характеристики не найдены");
          }

          _rxChar = rx;
          _txChar = tx;

          await _dataSub?.cancel();
          _rxBytes.clear();
          _dataSub = tx.onValueReceived.listen(_onDataReceived);
          await tx.setNotifyValue(true);

          if (mounted) {
            setState(() {
              _isConnected = true;
              _isConnecting = false;
            });
          }

          // ---- HELLO (handshake, proto=2) ----
          await _hello();

          break; // успех
        } catch (e) {
          debugPrint("Попытка $attempt не удалась: $e");
          if (attempt == maxAttempts) {
            _showSnack("Не удалось подключиться: $e");
            if (mounted) setState(() => _isConnecting = false);
            return;
          }
          await Future.delayed(const Duration(milliseconds: 700));
        }
      }
    } finally {
      _isConnecting = false;
    }
  }

  Future<void> _disconnect() async {
    _addBleLog("--", "Отключение");
    await _dataSub?.cancel();
    await _connSub?.cancel();
    _dataSub = null;
    _connSub = null;

    try {
      await _txChar?.setNotifyValue(false);
    } catch (_) {}

    await _device?.disconnect();

    if (mounted) {
      setState(() {
        _device = null;
        _rxChar = null;
        _txChar = null;
        _mtu = 23;
        _isConnected = false;
        _mcuName = "";
        _mcuMac = "";
        _proto = "";
        _fw = "";
        _mcuMtu = 0;
        _rxBytes.clear();
      });
    }
    _showSnack("Отключено");
  }

  // ==========================================================================
  //  ПРОТОКОЛ proto=2
  // ==========================================================================

  int _nextId() {
    _txId = (_txId >= 65535) ? 1 : _txId + 1;
    return _txId;
  }

  /// Сериализует транзакции: одновременно активна только одна.
  Future<T> _serialize<T>(Future<T> Function() action) {
    final completer = Completer<T>();
    _txnChain = _txnChain.then((_) async {
      try {
        completer.complete(await action());
      } catch (e, s) {
        completer.completeError(e, s);
      }
    });
    return completer.future;
  }

  /// Выполняет одну транзакцию: шлёт `<id> <cmd>`, собирает строки с этим
  /// id до терминатора END/ERR. При таймауте повторяет (для чтения —
  /// безопасно; для SET_* повтор допускается документом после таймаута).
  Future<List<String>> _transact(String cmd, {bool retryable = true}) {
    return _serialize(() => _transactOnce(cmd, retryable: retryable));
  }

  Future<List<String>> _transactOnce(
    String cmd, {
    required bool retryable,
  }) async {
    const maxAttempts = 3;
    Object? lastError;

    for (int attempt = 1; attempt <= maxAttempts; attempt++) {
      if (_rxChar == null) throw StateError("Нет характеристики записи");

      final id = _nextId();
      final active = _ActiveTxn(id);
      _active = active;
      _addBleLog("→ TX", "$id $cmd");

      try {
        await _writeLine("$id $cmd");
      } catch (e) {
        if (identical(_active, active)) _active = null;
        rethrow;
      }

      _TxnOutcome outcome;
      try {
        outcome = await active.completer.future.timeout(kTxnTimeout);
      } on TimeoutException {
        lastError = Exception("Таймаут ответа (id=$id)");
        _addBleLog("--", "Таймаут id=$id");
        if (retryable && attempt < maxAttempts) continue;
        throw lastError;
      } finally {
        if (identical(_active, active)) _active = null;
      }

      // ---- ERR: терминатор ошибки ----
      if (outcome.error != null) {
        final code = _errCode(outcome.error!);
        final text = _errText(outcome.error!);
        // BUSY/INTERNAL имеет смысл повторить.
        if (retryable &&
            attempt < maxAttempts &&
            (code == "BUSY" || code == "INTERNAL")) {
          _addBleLog("--", "$code, повтор id=$id");
          continue;
        }
        throw ProtoException(code, text);
      }

      // ---- END: проверка полноты n ----
      if (!_countOk(outcome)) {
        lastError = Exception(
            "Нарушена целостность: END n=${outcome.expectedCount}, "
            "получено ${outcome.data.length} строк");
        _addBleLog("--", "$lastError");
        if (retryable && attempt < maxAttempts) continue;
        throw lastError;
      }

      return outcome.data;
    }

    throw lastError ?? Exception("Транзакция не удалась");
  }

  /// Проверка количества строк по END `n=N`.
  ///
  /// В примерах протокола строка-заголовок `DEVS ... offset=... count=...`
  /// не входит в n (считаются только строки DEV). Также информационная
  /// строка HELLO не входит в n=0.
  bool _countOk(_TxnOutcome o) {
    if (o.expectedCount == null) return true;
    final n = o.expectedCount!;
    int payload = o.data.length;
    if (o.data.isNotEmpty &&
        o.data.first.startsWith("DEVS") &&
        o.data.first.contains("offset=")) {
      payload -= 1;
    }
    if (n == payload) return true;
    if (n == 0 && payload <= 1) return true; // HELLO-ответ
    return false;
  }

  String _errCode(String errLine) {
    final parts = errLine.split(' ');
    return parts.length >= 2 ? parts[1] : "INTERNAL";
  }

  String _errText(String errLine) {
    final parts = errLine.split(' ');
    return parts.length >= 3 ? parts.sublist(2).join(' ') : "";
  }

  /// Отправка одной строки. Команда обязана помещаться в MTU−3 и НЕ режется.
  Future<void> _writeLine(String line) async {
    final char = _rxChar;
    if (char == null) throw StateError("Нет характеристики записи");
    final data = utf8.encode("$line\n");
    if (data.length > _maxChunkSize) {
      throw Exception(
          "Команда длиннее MTU−3: ${data.length} > $_maxChunkSize байт "
          "(MTU=$_mtu). Согласуйте MTU или сократите команду.");
    }
    await char.write(data, withoutResponse: true);
  }

  // ---------- Приём и разбор ----------
  void _onDataReceived(List<int> data) {
    _addBleLog("← RX", _renderBytes(data));
    _rxBytes.addAll(data);

    while (true) {
      final nl = _rxBytes.indexOf(0x0A);
      if (nl < 0) {
        // Строка ещё не завершена. Контроль переполнения.
        if (_rxBytes.length > kMaxLine) {
          _rxBytes.clear();
          _addBleLog("--", "Переполнение RX (> $kMaxLine байт), сброс");
        }
        break;
      }
      final lineBytes = _rxBytes.sublist(0, nl);
      _rxBytes.removeRange(0, nl + 1);
      final line = utf8.decode(lineBytes, allowMalformed: true);
      _handleLine(line);
    }
  }

  void _handleLine(String line) {
    final trimmed = line.replaceAll('\r', '');
    if (trimmed.isEmpty) return;

    final sp = trimmed.indexOf(' ');
    if (sp <= 0) return; // нет ID

    final id = int.tryParse(trimmed.substring(0, sp));
    if (id == null) return;

    final active = _active;
    // Строки с чужим (или устаревшим) ID отбрасываются.
    if (active == null || active.id != id) return;

    final rest = trimmed.substring(sp + 1);

    if (rest == "END" || rest.startsWith("END ")) {
      final m = RegExp(r'^END\s+n=(\d+)$').firstMatch(rest);
      final n = m != null ? int.parse(m.group(1)!) : null;
      if (!active.completer.isCompleted) {
        active.completer.complete(
            _TxnOutcome(data: List.of(active.data), expectedCount: n));
      }
      return;
    }

    if (rest == "ERR" || rest.startsWith("ERR ")) {
      if (!active.completer.isCompleted) {
        active.completer.complete(
            _TxnOutcome(data: List.of(active.data), error: rest));
      }
      return;
    }

    active.data.add(rest);
  }

  // ---------- Команды ----------
  Future<void> _hello() async {
    try {
      final lines = await _transact("HELLO");
      for (final l in lines) {
        if (l.startsWith("HELLO")) {
          if (mounted) {
            setState(() {
              _proto = _kv(l, "proto") ?? "";
              _fw = _kv(l, "fw") ?? "";
              _mcuMtu = int.tryParse(_kv(l, "mtu") ?? "") ?? 0;
            });
          }
        }
      }
      _addBleLog("--",
          "HELLO: proto=$_proto fw=$_fw mcuMtu=$_mcuMtu (наш MTU=$_mtu)");
    } catch (e) {
      _addBleLog("--", "HELLO не удался: $e");
    }
  }

  String? _kv(String line, String key) {
    final m = RegExp('$key=([^ ]+)').firstMatch(line);
    return m?.group(1);
  }

  Future<void> _cmdGetName() async {
    try {
      final lines = await _transact("GET_NAME");
      for (final l in lines) {
        if (l.startsWith("NAME")) {
          final name = l.length > 5 ? l.substring(5).trim() : "";
          if (mounted) {
            setState(() {
              _mcuName = name;
              _nameController.text = name;
            });
          }
        }
      }
      _showSnack("Имя: ${_mcuName.isEmpty ? '—' : _mcuName}");
    } catch (e) {
      _showSnack("Ошибка GET_NAME: $e");
    }
  }

  Future<void> _cmdGetMac() async {
    try {
      final lines = await _transact("GET_MAC");
      for (final l in lines) {
        if (l.startsWith("MAC")) {
          final mac = l.length > 4 ? l.substring(4).trim() : "";
          if (mounted) setState(() => _mcuMac = mac);
        }
      }
      _showSnack("MAC: ${_mcuMac.isEmpty ? '—' : _mcuMac}");
    } catch (e) {
      _showSnack("Ошибка GET_MAC: $e");
    }
  }

  Future<void> _cmdSetName(String name) async {
    try {
      await _transact("SET_NAME $name");
      if (mounted) {
        setState(() {
          _mcuName = name;
          _nameController.text = name;
        });
      }
      _showSnack("Имя сохранено: $name");
    } catch (e) {
      _showSnack("Ошибка SET_NAME: $e");
    }
  }

  Future<void> _cmdSetMac(String mac) async {
    try {
      await _transact("SET_MAC $mac");
      _showSnack("MAC применён, MCU перезагружается");
    } catch (e) {
      _showSnack("Ошибка SET_MAC: $e");
    }
  }

  Future<void> _cmdClearDevices() async {
    try {
      await _transact("CLEAR_DEVICES");
      if (mounted) {
        setState(() {
          _deviceRecords = [];
          _devicesTotal = 0;
        });
      }
      _showSnack("Журнал очищен");
    } catch (e) {
      _showSnack("Ошибка CLEAR_DEVICES: $e");
    }
  }

  // ---------- Страничная загрузка журнала ----------
  Future<void> _fetchDevicesPaged() async {
    if (_isFetchingDevices) return;
    setState(() {
      _isFetchingDevices = true;
      _deviceRecords = [];
      _devicesTotal = 0;
    });

    try {
      // 1) Метаданные: DEVS total=<N>
      final meta = await _transact("GET_DEVICES");
      int? total;
      for (final l in meta) {
        if (l.startsWith("DEVS")) {
          total = int.tryParse(_kv(l, "total") ?? "");
        }
      }
      if (total == null) {
        throw Exception("Не получен total из DEVS");
      }
      _devicesTotal = total;

      if (total == 0) {
        _showSnack("Журнал пуст");
        return;
      }

      // 2) Страницы по kDevicesPage (10)
      final all = <DeviceRecord>[];
      int offset = 0;
      while (offset < total) {
        final lines = await _transact("GET_DEVICES $offset $kDevicesPage");
        int count = 0;
        for (final l in lines) {
          if (l.startsWith("DEVS")) {
            count = int.tryParse(_kv(l, "count") ?? "") ?? 0;
          } else if (l.startsWith("DEV ")) {
            final js = l.substring(4).trim();
            try {
              final map = json.decode(js) as Map<String, dynamic>;
              all.add(DeviceRecord.fromJson(map));
            } catch (_) {
              // битую строку пропускаем
            }
          }
        }
        if (mounted) setState(() => _deviceRecords = List.of(all));
        if (count == 0) break;
        offset += count;
      }

      _showSnack("Загружено ${all.length} из $total");
    } catch (e) {
      _showSnack("Ошибка загрузки журнала: $e");
    } finally {
      if (mounted) setState(() => _isFetchingDevices = false);
    }
  }

  // ---------- Журнал: файл ----------
  String _two(int n) => n.toString().padLeft(2, '0');

  String _formatLogForFile() {
    final now = DateTime.now();
    final buf = StringBuffer();

    buf.writeln("=== BLE Tracker log ===");
    buf.writeln("Saved:   ${now.year}-${_two(now.month)}-${_two(now.day)} "
        "${_two(now.hour)}:${_two(now.minute)}:${_two(now.second)}");
    buf.writeln("Device:  ${_device?.platformName ?? '?'}");
    buf.writeln("MAC:     ${_device?.remoteId.toString() ?? '?'}");
    buf.writeln("MCU MAC: ${_mcuMac.isEmpty ? '?' : _mcuMac}");
    buf.writeln("MCU name:${_mcuName.isEmpty ? '?' : _mcuName}");
    buf.writeln("Proto:   $_proto  fw=$_fw  mtu=$_mcuMtu");
    buf.writeln("Total:   $_devicesTotal");
    buf.writeln("----------------------");

    for (final r in _deviceRecords) {
      final name = r.name.isEmpty ? '(без имени)' : r.name;
      buf.writeln("${r.mac}   $name   RSSI: ${r.rssi}   t: ${r.time}");
    }

    buf.writeln("=== end ===");
    return buf.toString();
  }

  Future<void> _saveLogToFile() async {
    if (_deviceRecords.isEmpty) {
      _showSnack("Сначала нажмите «Обновить»");
      return;
    }
    try {
      final dir = await getApplicationDocumentsDirectory();
      final now = DateTime.now();
      final stamp = "${now.year}${_two(now.month)}${_two(now.day)}_"
          "${_two(now.hour)}${_two(now.minute)}${_two(now.second)}";
      final file = File("${dir.path}/tracker_log_$stamp.txt");
      await file.writeAsString(_formatLogForFile());

      await Share.shareXFiles(
        [XFile(file.path, mimeType: "text/plain")],
        subject: "BLE Tracker log $stamp",
        text: "Журнал посещённых устройств",
      );
      _showSnack("Файл готов: ${file.path.split('/').last}");
    } catch (e) {
      _showSnack("Ошибка сохранения: $e");
    }
  }

  // ---------- UI ----------
  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('BLE Tracker'),
        actions: [
          if (_isConnected)
            IconButton(
              icon: const Icon(Icons.link_off),
              tooltip: "Отключиться",
              onPressed: _disconnect,
            ),
        ],
      ),
      body: _isConnected ? _buildControlPanel() : _buildScanPanel(),
    );
  }

  Widget _buildScanPanel() {
    final results = _visibleScanResults;
    return Column(
      children: [
        Padding(
          padding: const EdgeInsets.all(12),
          child: ElevatedButton.icon(
            onPressed: _isScanning ? null : _startScan,
            icon: const Icon(Icons.search),
            label:
                Text(_isScanning ? "Сканирование..." : "Начать сканирование"),
            style:
                ElevatedButton.styleFrom(minimumSize: const Size.fromHeight(50)),
          ),
        ),
        Padding(
          padding: const EdgeInsets.symmetric(horizontal: 12),
          child: Row(
            children: [
              const Text("Только ключи (0x0201)"),
              Switch(
                value: _onlyKeychain,
                onChanged: (v) => setState(() => _onlyKeychain = v),
              ),
            ],
          ),
        ),
        Padding(
          padding: const EdgeInsets.symmetric(horizontal: 12),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(_rssiThreshold <= -100
                  ? "Сигнал: любой"
                  : "Сигнал: ≥ $_rssiThreshold dBm"),
              Slider(
                min: -100,
                max: -40,
                divisions: 60,
                value: _rssiThreshold.toDouble(),
                label: _rssiThreshold <= -100
                    ? "любой"
                    : "$_rssiThreshold dBm",
                onChanged: (v) =>
                    setState(() => _rssiThreshold = v.round()),
              ),
            ],
          ),
        ),
        Expanded(
          child: results.isEmpty
              ? const Center(child: Text("Устройства не найдены"))
              : ListView.builder(
                  itemCount: results.length,
                  itemBuilder: (context, i) {
                    final r = results[i];
                    final isKey = _isKeychain(r.advertisementData);
                    // advName объединяет основной ADV и scan response,
                    // поэтому полное имя ключа (ScanResponse.addName())
                    // появится здесь благодаря активному сканированию.
                    final advName = r.advertisementData.advName;
                    final platformName = r.device.platformName;
                    final name = advName.isNotEmpty
                        ? advName
                        : (platformName.isNotEmpty
                            ? platformName
                            : "(без имени)");
                    return ListTile(
                      leading: Icon(
                        isKey ? Icons.vpn_key : Icons.bluetooth,
                        color: isKey ? Colors.orange : null,
                      ),
                      title: Text(name),
                      subtitle: Text("${r.device.remoteId}  RSSI: ${r.rssi}"
                          "${isKey ? '  • ключ 0x0201' : ''}"),
                      onTap: () => _connect(r.device),
                    );
                  },
                ),
        ),
      ],
    );
  }

  Widget _buildControlPanel() {
    return DefaultTabController(
      length: 3,
      child: Column(
        children: [
          const TabBar(
            tabs: [
              Tab(icon: Icon(Icons.tune), text: "Управление"),
              Tab(icon: Icon(Icons.list_alt), text: "Журнал"),
              Tab(icon: Icon(Icons.terminal), text: "Логи BLE"),
            ],
          ),
          Expanded(
            child: TabBarView(
              children: [
                _buildControlTab(),
                _buildJournalTab(),
                _buildBleLogsTab(),
              ],
            ),
          ),
        ],
      ),
    );
  }

  // ---------- Вкладка «Управление» ----------
  Widget _buildControlTab() {
    return SingleChildScrollView(
      padding: const EdgeInsets.all(12),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Card(
            child: Padding(
              padding: const EdgeInsets.all(12),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text("Подключено: ${_device?.platformName ?? ''}",
                      style: const TextStyle(fontWeight: FontWeight.bold)),
                  const SizedBox(height: 6),
                  Text("BLE MAC: ${_device?.remoteId.toString() ?? '—'}"),
                  Text("MCU MAC: ${_mcuMac.isEmpty ? '—' : _mcuMac}"),
                  Text("Имя MCU: ${_mcuName.isEmpty ? '—' : _mcuName}"),
                  Text("Proto: ${_proto.isEmpty ? '—' : _proto}  "
                      "fw: ${_fw.isEmpty ? '—' : _fw}  "
                      "mtu(MCU): ${_mcuMtu == 0 ? '—' : _mcuMtu}"),
                  const SizedBox(height: 8),
                  Row(
                    children: [
                      Expanded(
                        child: ElevatedButton.icon(
                          onPressed: _cmdGetName,
                          icon: const Icon(Icons.badge),
                          label: const Text("Имя"),
                        ),
                      ),
                      const SizedBox(width: 8),
                      Expanded(
                        child: ElevatedButton.icon(
                          onPressed: _cmdGetMac,
                          icon: const Icon(Icons.memory),
                          label: const Text("MAC"),
                        ),
                      ),
                    ],
                  ),
                ],
              ),
            ),
          ),

          _card("Изменить имя", [
            TextField(
              controller: _nameController,
              decoration: const InputDecoration(
                labelText: "Новое имя",
                border: OutlineInputBorder(),
              ),
            ),
            const SizedBox(height: 8),
            ElevatedButton.icon(
              onPressed: () {
                final n = _nameController.text.trim();
                if (n.isEmpty) {
                  _showSnack("Введите имя");
                  return;
                }
                _cmdSetName(n);
              },
              icon: const Icon(Icons.save),
              label: const Text("Применить"),
            ),
          ]),

          _card("Изменить MAC (перезагрузка MCU)", [
            TextField(
              controller: _macController,
              decoration: const InputDecoration(
                labelText: "AA:BB:CC:DD:EE:FF",
                border: OutlineInputBorder(),
              ),
            ),
            const SizedBox(height: 8),
            ElevatedButton.icon(
              onPressed: () {
                final m = _macController.text.trim();
                if (m.isEmpty) {
                  _showSnack("Введите MAC");
                  return;
                }
                _cmdSetMac(m);
              },
              icon: const Icon(Icons.wifi_tethering),
              label: const Text("Применить и перезагрузить"),
            ),
          ]),
        ],
      ),
    );
  }

  // ---------- Вкладка «Журнал» ----------
  Widget _buildJournalTab() {
    return SingleChildScrollView(
      padding: const EdgeInsets.all(12),
      child: _card("Посещённые устройства", [
        Row(
          children: [
            Expanded(
              child: ElevatedButton.icon(
                onPressed: _isFetchingDevices ? null : _fetchDevicesPaged,
                icon: _isFetchingDevices
                    ? const SizedBox(
                        width: 18,
                        height: 18,
                        child: CircularProgressIndicator(strokeWidth: 2),
                      )
                    : const Icon(Icons.refresh),
                label: Text(_isFetchingDevices ? "Загрузка..." : "Обновить"),
              ),
            ),
            const SizedBox(width: 8),
            Expanded(
              child: ElevatedButton.icon(
                onPressed: _saveLogToFile,
                icon: const Icon(Icons.save_alt),
                label: const Text("Сохранить"),
                style:
                    ElevatedButton.styleFrom(backgroundColor: Colors.green),
              ),
            ),
          ],
        ),
        const SizedBox(height: 8),
        Text("Всего: $_devicesTotal, загружено: ${_deviceRecords.length}"),
        const SizedBox(height: 8),
        Text(_formatDeviceList()),
        const SizedBox(height: 8),
        ElevatedButton.icon(
          style: ElevatedButton.styleFrom(backgroundColor: Colors.red),
          onPressed: _cmdClearDevices,
          icon: const Icon(Icons.delete),
          label: const Text("Очистить журнал"),
        ),
      ]),
    );
  }

  String _formatDeviceList() {
    if (_deviceRecords.isEmpty) return "(пусто)";
    final buf = StringBuffer();
    for (final r in _deviceRecords) {
      final name = r.name.isEmpty ? '(без имени)' : r.name;
      buf.writeln("• ${r.mac}   $name   RSSI: ${r.rssi}");
    }
    return buf.toString();
  }

  // ---------- Вкладка «Логи BLE» ----------
  Widget _buildBleLogsTab() {
    return Padding(
      padding: const EdgeInsets.all(12),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Row(
            children: [
              Expanded(
                child: ElevatedButton.icon(
                  onPressed: () {
                    setState(() => _bleLogs.clear());
                    _showSnack("Логи очищены");
                  },
                  icon: const Icon(Icons.delete_sweep),
                  label: const Text("Очистить логи"),
                  style: ElevatedButton.styleFrom(backgroundColor: Colors.red),
                ),
              ),
              const SizedBox(width: 8),
              Expanded(
                child: Row(
                  children: [
                    const Text("Автоскролл"),
                    Switch(
                      value: _bleLogsAutoScroll,
                      onChanged: (v) =>
                          setState(() => _bleLogsAutoScroll = v),
                    ),
                  ],
                ),
              ),
            ],
          ),
          const SizedBox(height: 8),
          Expanded(
            child: Container(
              decoration: BoxDecoration(
                color: Colors.black87,
                borderRadius: BorderRadius.circular(6),
              ),
              padding: const EdgeInsets.all(8),
              child: _bleLogs.isEmpty
                  ? const Center(
                      child: Text(
                        "Пока нет записей",
                        style: TextStyle(color: Colors.white54),
                      ),
                    )
                  : ListView.builder(
                      controller: _bleLogsScroll,
                      itemCount: _bleLogs.length,
                      itemBuilder: (context, i) {
                        final line = _bleLogs[i];
                        Color color = Colors.white70;
                        if (line.contains("→ TX")) {
                          color = Colors.lightBlueAccent;
                        } else if (line.contains("← RX")) {
                          color = Colors.lightGreenAccent;
                        } else if (line.contains("--")) {
                          color = Colors.orangeAccent;
                        }
                        return Text(
                          line,
                          style: TextStyle(
                            color: color,
                            fontFamily: 'monospace',
                            fontSize: 12,
                          ),
                        );
                      },
                    ),
            ),
          ),
        ],
      ),
    );
  }

  Widget _card(String title, List<Widget> children) {
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(title, style: const TextStyle(fontWeight: FontWeight.bold)),
            const SizedBox(height: 8),
            ...children,
          ],
        ),
      ),
    );
  }

  String _renderBytes(List<int> data) {
    final txt = utf8.decode(data, allowMalformed: true);
    return txt.replaceAll('\r', '').replaceAll('\n', '\\n');
  }

  void _showSnack(String msg) {
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text(msg), duration: const Duration(seconds: 2)),
    );
  }
}
