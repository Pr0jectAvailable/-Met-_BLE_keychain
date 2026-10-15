import 'dart:async';
import 'dart:math';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';

import 'package:mobile_scanner/mobile_scanner.dart';

import 'met_client.dart';
import 'met_crypto.dart';

void main() => runApp(const MetApp());

class MetApp extends StatelessWidget {
  const MetApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Met Keychain',
      theme: ThemeData(primarySwatch: Colors.indigo),
      home: const HomePage(),
    );
  }
}

class MetPairUri {
  final String devNo;
  final String kPairB32;
  const MetPairUri({required this.devNo, required this.kPairB32});
}

MetPairUri? parsePairUri(String raw) {
  try {
    final u = Uri.parse(raw.trim());
    if (u.scheme != 'met' || u.host != 'pair') return null;
    final d = u.queryParameters['d'];
    final k = u.queryParameters['k'];
    if (d == null || k == null || d.isEmpty || k.isEmpty) return null;
    return MetPairUri(devNo: d, kPairB32: k);
  } catch (_) {
    return null;
  }
}

class QrScanPage extends StatefulWidget {
  const QrScanPage({super.key});
  @override
  State<QrScanPage> createState() => _QrScanPageState();
}

class _QrScanPageState extends State<QrScanPage> {
  final MobileScannerController _controller = MobileScannerController();
  bool _handled = false;

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  void _onDetect(BarcodeCapture capture) {
    if (_handled) return;
    for (final b in capture.barcodes) {
      final v = b.rawValue;
      if (v != null && v.startsWith('met://')) {
        _handled = true;
        Navigator.of(context).pop(v);
        return;
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Сканирование QR')),
      body: MobileScanner(controller: _controller, onDetect: _onDetect),
    );
  }
}

class HomePage extends StatefulWidget {
  const HomePage({super.key});

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  // ---------- Сканирование ----------
  StreamSubscription? _scanSub;
  bool _isScanning = false;
  List<ScanResult> _scanResults = [];
  bool _onlyMet = false;
  int _rssiThreshold = -100;

  // ---------- Подключение ----------
  MetClient? _client;
  BluetoothDevice? _device;
  bool _isConnecting = false;

  // ---------- Сессия ----------
  bool _authed = false;
  String? _kOwnerHex;
  String? _deviceNumber;
  Uint8List? _kDev; // только если пользователь ввёл вручную (демо/тест)

  // ---------- Журнал ----------
  List<JournalRecord> _records = [];
  int _journalTotal = 0;
  bool _fetching = false;

  // ---------- Логи ----------
  final List<String> _logs = [];
  static const int _maxLogs = 500;
  final ScrollController _logsScroll = ScrollController();
  bool _autoScroll = true;

  final _devNoCtrl = TextEditingController();
  final _kPairCtrl = TextEditingController();
  final _kOwnerCtrl = TextEditingController();
  final _kDevCtrl = TextEditingController();
  final _nonceCtrl = TextEditingController();
  final _timeSigCtrl = TextEditingController();
  final _batteryCtrl = TextEditingController();

  @override
  void initState() {
    super.initState();
    _requestPermissions();
    _loadCreds();
  }

  @override
  void dispose() {
    _scanSub?.cancel();
    _client?.disconnect();
    _logsScroll.dispose();
    _devNoCtrl.dispose();
    _kPairCtrl.dispose();
    _kOwnerCtrl.dispose();
    _kDevCtrl.dispose();
    _nonceCtrl.dispose();
    _timeSigCtrl.dispose();
    _batteryCtrl.dispose();
    super.dispose();
  }

  // ---------- Утилиты ----------
  void _addLog(String dir, String msg) {
    if (!mounted) return;
    final now = DateTime.now();
    final ts = '${_two(now.hour)}:${_two(now.minute)}:${_two(now.second)}'
        '.${now.millisecond.toString().padLeft(3, '0')}';
    setState(() {
      _logs.add('$ts  $dir  $msg');
      if (_logs.length > _maxLogs) {
        _logs.removeRange(0, _logs.length - _maxLogs);
      }
    });
    if (_autoScroll) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (_logsScroll.hasClients) {
          _logsScroll.jumpTo(_logsScroll.position.maxScrollExtent);
        }
      });
    }
  }

  String _two(int n) => n.toString().padLeft(2, '0');

  void _snack(String msg) {
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text(msg), duration: const Duration(seconds: 2)),
    );
  }

  Future<void> _requestPermissions() async {
    await [
      Permission.bluetoothScan,
      Permission.bluetoothConnect,
      Permission.locationWhenInUse,
    ].request();
  }

  Future<void> _loadCreds() async {
    final owner = await MetCredentials.kOwnerHex;
    final dev = await MetCredentials.deviceNumber;
    final pair = await MetCredentials.kPairHex;
    if (!mounted) return;
    setState(() {
      _kOwnerHex = owner;
      _deviceNumber = dev;
      if (owner != null) _kOwnerCtrl.text = owner;
      if (dev != null) _devNoCtrl.text = dev;
      if (pair != null) _kPairCtrl.text = pair;
    });
  }

  // ---------- Сканирование ----------
  Future<void> _startScan() async {
    setState(() {
      _scanResults = [];
      _isScanning = true;
    });
    if (await FlutterBluePlus.adapterState.first !=
        BluetoothAdapterState.on) {
      _snack('Включите Bluetooth');
      setState(() => _isScanning = false);
      return;
    }
    await _scanSub?.cancel();
    await FlutterBluePlus.stopScan();
    while (FlutterBluePlus.isScanningNow) {
      await Future.delayed(const Duration(milliseconds: 100));
    }
    _scanSub = FlutterBluePlus.scanResults.listen((r) {
      if (!mounted) return;
      setState(() => _scanResults = r);
    });
    try {
      await FlutterBluePlus.startScan(
        timeout: const Duration(seconds: 15),
        androidScanMode: AndroidScanMode.lowLatency,
        continuousUpdates: true,
      );
    } catch (e) {
      _snack('Ошибка сканирования: $e');
    }
    if (mounted) setState(() => _isScanning = false);
  }

  MetAdvertisement? _metAdv(ScanResult r) {
    final mf = r.advertisementData.manufacturerData;
    if (mf.isEmpty) return null;
    return parseMetAdvertisement(mf);
  }

  List<ScanResult> get _visible {
    var list = _scanResults.where((r) => r.rssi >= _rssiThreshold);
    if (_onlyMet) list = list.where((r) => _metAdv(r) != null);
    return list.toList()..sort((a, b) => b.rssi.compareTo(a.rssi));
  }

  // ---------- QR-подключение ----------
  Future<void> _scanQr() async {
    final data = await Navigator.of(context).push<String>(
      MaterialPageRoute(builder: (_) => const QrScanPage()),
    );
    if (data == null) return;
    final parsed = parsePairUri(data);
    if (parsed == null) {
      _snack('Неверный QR: $data');
      return;
    }
    setState(() {
      _devNoCtrl.text = parsed.devNo;
      _kPairCtrl.text = parsed.kPairB32;
    });
    await MetCredentials.saveKPair(parsed.kPairB32);
    _snack('QR: ${parsed.devNo}. Подключаюсь...');
    await _connectByDeviceNumber(parsed.devNo);
  }

  /// Ищет в эфире брелок с номером из QR и подключается к нему.
  Future<void> _connectByDeviceNumber(String devNo) async {
    if (_isConnecting) return;
    // До 3 циклов «скан → подключение»: на Android первый скан сразу после
    // предыдущего часто не даёт ни одного пакета, а первый connect рвётся
    // на GATT. Раньше это требовало 3 ручных нажатий — теперь повторяем сами.
    const maxCycles = 3;
    for (var cycle = 0; cycle < maxCycles; cycle++) {
      if (!mounted) return;
      final found = await _scanForDevice(devNo);
      if (found != null) {
        final ok = await _connect(found);
        if (ok) {
          if (mounted) setState(() => _isScanning = false);
          return;
        }
      }
      // Перед повтором даём стеку BLE полностью освободить устройство.
      await Future.delayed(const Duration(milliseconds: 800));
    }
    if (mounted) {
      setState(() => _isScanning = false);
      _snack('Устройство $devNo не найдено');
    }
  }

  /// Один проход сканирования. Возвращает найденное устройство или null.
  /// Выходит сразу после находки, не дожидаясь таймаута скана.
  Future<BluetoothDevice?> _scanForDevice(String devNo) async {
    await _scanSub?.cancel();
    await FlutterBluePlus.stopScan();
    while (FlutterBluePlus.isScanningNow) {
      await Future.delayed(const Duration(milliseconds: 100));
    }
    if (await FlutterBluePlus.adapterState.first !=
        BluetoothAdapterState.on) {
      _snack('Включите Bluetooth');
      return null;
    }
    setState(() {
      _scanResults = [];
      _isScanning = true;
    });
    BluetoothDevice? target;
    // Fallback: после привязки брелок рекламируется как anonymous (без devNo),
    // поэтому точного совпадения по номеру может не быть. Запоминаем любое
    // Met-устройство на случай, если по devNo за весь скан так и не нашли.
    BluetoothDevice? fallback;
    final completer = Completer<BluetoothDevice?>();
    _scanSub = FlutterBluePlus.scanResults.listen((r) {
      if (!mounted) return;
      setState(() => _scanResults = r);
      if (target != null) return;
      for (final res in r) {
        final adv = _metAdv(res);
        if (adv == null) continue;
        if (adv.kind == AdvKind.pairing && adv.deviceNumber == devNo) {
          target = res.device;
          if (!completer.isCompleted) completer.complete(res.device);
          break;
        }
        // Привязанное устройство (anonymous) или pairing с другим номером.
        fallback ??= res.device;
      }
    });
    try {
      // Не await: ждём находку через completer и выходим раньше таймаута.
      FlutterBluePlus.startScan(
        timeout: const Duration(seconds: 8),
        androidScanMode: AndroidScanMode.lowLatency,
        continuousUpdates: true,
      );
    } catch (e) {
      _snack('Ошибка сканирования: $e');
    }
    final found = await completer.future
        .timeout(const Duration(seconds: 10), onTimeout: () => null);
    // Гасим скан сразу, чтобы не тянуть 15 с и не мешать connect().
    await _scanSub?.cancel();
    await FlutterBluePlus.stopScan();
    while (FlutterBluePlus.isScanningNow) {
      await Future.delayed(const Duration(milliseconds: 100));
    }
    return found ?? target ?? fallback;
  }

  // ---------- Подключение ----------
  Future<bool> _connect(BluetoothDevice device) async {
    if (_isConnecting) return false;
    setState(() => _isConnecting = true);
    await _scanSub?.cancel();
    await FlutterBluePlus.stopScan();
    while (FlutterBluePlus.isScanningNow) {
      await Future.delayed(const Duration(milliseconds: 100));
    }
    // Небольшая пауза, чтобы стек BLE успел перейти из сканирования
    // в подключение — иначе первый connect часто отваливается.
    await Future.delayed(const Duration(milliseconds: 300));

    MetClient? client;
    try {
      // До трёх попыток: на Android первый connect() сразу после скана
      // иногда рвётся на этапе GATT, повтор обычно проходит сразу.
      for (var attempt = 0; attempt < 3; attempt++) {
        client = MetClient(device: device, onLog: _addLog);
        try {
          await client.connect();
          await client.hello();
          break;
        } catch (e) {
          await client.disconnect();
          client = null;
          if (attempt < 2) {
            _addLog('--', 'Повтор подключения ${attempt + 2}/3...');
            await Future.delayed(const Duration(milliseconds: 600));
            continue;
          }
          rethrow;
        }
      }
      final c = client!;
      _device = device;
      _client = c;
      _authed = false;
      _addLog('--',
          'HELLO proto=${c.info.proto} fw=${c.info.fw} '
          'dev=${c.info.dev} paired=${c.info.paired} '
          'timeok=${c.info.timeOk} mtu=${c.info.mtu}');
      setState(() {});
      return true;
    } catch (e) {
      _snack('Не удалось подключиться: $e');
      await client?.disconnect();
      return false;
    } finally {
      if (mounted) setState(() => _isConnecting = false);
    }
  }

  Future<void> _disconnect() async {
    await _client?.disconnect();
    setState(() {
      _client = null;
      _device = null;
      _authed = false;
      _records = [];
      _journalTotal = 0;
    });
    _snack('Отключено');
  }

  MetClient get _c => _client!;

  // ---------- Команды ----------
  Future<void> _auth() async {
    final hex = _kOwnerCtrl.text.trim();
    if (hex.isEmpty) {
      _snack('Введите K_owner (hex)');
      return;
    }
    try {
      await _c.auth(fromHex(hex));
      setState(() => _authed = true);
      _snack('Аутентификация пройдена');
    } catch (e) {
      _snack('AUTH: $e');
    }
  }

  /// Генерирует случайный K_owner (16 байт) и подставляет в поле.
  void _randomizeKOwner() {
    final hex = toHex(_randomBytes(16));
    setState(() => _kOwnerCtrl.text = hex);
    _snack('Сгенерирован новый K_owner (16 байт)');
  }

  Future<void> _pair() async {
    final devNo = _devNoCtrl.text.trim();
    final kPair = _kPairCtrl.text.trim();
    final nonce = _nonceCtrl.text.trim();
    if (devNo.isEmpty || kPair.isEmpty || nonce.isEmpty) {
      _snack('Заполните номер устройства, K_pair и nonce сервера');
      return;
    }
    // Время берём автоматически с телефона.
    final utc = DateTime.now().millisecondsSinceEpoch ~/ 1000;
    try {
      final kPairBytes = _decodeKey(kPair);
      final kOwner = _randomBytes(16);
      final receipt = await _c.pairFinish(
        kPair: kPairBytes,
        kOwner: kOwner,
        nonce: fromHex(nonce),
        utc: utc,
      );
      await MetCredentials.saveOwner(
        deviceNumber: devNo,
        kOwnerHex: toHex(kOwner),
      );
      setState(() {
        _kOwnerHex = toHex(kOwner);
        _deviceNumber = devNo;
        _kOwnerCtrl.text = toHex(kOwner);
      });
      _addLog('--', 'RECEIPT $receipt  (отправьте на сервер)');
      _snack('Привязка выполнена, квитанция в логе');
    } catch (e) {
      _snack('PAIR: $e');
    }
  }

  Future<void> _timeSet() async {
    final kDev = _resolveKDev();
    if (kDev == null) {
      _snack('Нужен K_dev (демо-поле) для подписи времени');
      return;
    }
    // Время берём автоматически с телефона.
    final utc = DateTime.now().millisecondsSinceEpoch ~/ 1000;
    try {
      if (!_authed) await _ensureAuth();
      // Получаем nonce от брелока, подпись считаем ключом K_time.
      final begin = await _c.transact('TIME_BEGIN');
      Uint8List nonce = Uint8List(0);
      for (final l in begin) {
        if (l.startsWith('NONCE ')) {
          nonce = fromHex(l.substring(6).trim());
        }
      }
      final sig = timeSignature(deriveKTime(kDev), utc, nonce);
      await _c.transact('TIME_SET $utc ${toHex(sig)}');
      _snack('Время установлено');
      await _c.hello();
      setState(() {});
    } catch (e) {
      _snack('TIME_SET: $e');
    }
  }

  Future<void> _fetchJournal() async {
    if (_fetching) return;
    setState(() {
      _fetching = true;
      _records = [];
    });
    try {
      if (!_authed) await _ensureAuth();
      const page = 10;
      var offset = 0;
      var total = 0;
      final all = <JournalRecord>[];
      while (true) {
        final res = await _c.logGet(offset: offset, count: page);
        total = res.total;
        all.addAll(res.records);
        if (mounted) setState(() => _records = List.of(all));
        if (res.records.isEmpty) break;
        offset += res.records.length;
        if (offset >= total) break;
      }
      _journalTotal = total;
      _snack('Загружено ${all.length} из $total');
    } catch (e) {
      _snack('LOG_GET: $e');
    } finally {
      if (mounted) setState(() => _fetching = false);
    }
  }

  Future<void> _ackAll() async {
    if (_records.isEmpty) {
      _snack('Сначала загрузите журнал');
      return;
    }
    final maxSeq = _records.map((r) => r.seq).reduce((a, b) => a > b ? a : b);
    try {
      final removed = await _c.logAck(maxSeq);
      _snack('Удалено записей: $removed');
      setState(() => _records = []);
    } catch (e) {
      _snack('LOG_ACK: $e');
    }
  }

  Future<void> _battery() async {
    try {
      if (!_authed) await _ensureAuth();
      final b = await _c.battery();
      _batteryCtrl.text = '${b.pct}%  ${b.mv} mV';
      setState(() {});
    } catch (e) {
      _snack('BATTERY: $e');
    }
  }

  Future<void> _unpair() async {
    final nonce = _nonceCtrl.text.trim();
    if (nonce.isEmpty) {
      _snack('Введите nonce сервера');
      return;
    }
    try {
      if (!_authed) await _ensureAuth();
      final receipt = await _c.unpair(fromHex(nonce));
      await MetCredentials.clearOwner();
      setState(() {
        _kOwnerHex = null;
        _authed = false;
        _records = [];
      });
      _addLog('--', 'RECEIPT $receipt  (отправьте на сервер)');
      _snack('Отвязано, квитанция в логе');
    } catch (e) {
      _snack('UNPAIR: $e');
    }
  }

  Future<void> _ensureAuth() async {
    final hex = _kOwnerCtrl.text.trim();
    if (hex.isEmpty) throw Exception('нет K_owner');
    await _c.auth(fromHex(hex));
    if (mounted) setState(() => _authed = true);
  }

  // ---------- Крипто-помощники ----------
  Uint8List? _resolveKDev() {
    if (_kDev != null) return _kDev;
    final hex = _kDevCtrl.text.trim();
    if (hex.isEmpty) return null;
    try {
      final b = fromHex(hex);
      _kDev = b;
      return b;
    } catch (_) {
      return null;
    }
  }

  /// Ключ может быть задан как hex или как base32 (из QR).
  Uint8List _decodeKey(String s) {
    final clean = s.replaceAll(RegExp(r'[^0-9a-fA-F]'), '');
    final looksHex = clean.length == s.length && clean.length % 2 == 0;
    if (looksHex) return fromHex(s);
    return base32Decode(s);
  }

  // 32-bit LCG arithmetic: exact on both the VM and JavaScript (64-bit
  // literals are not representable as JS numbers).
  static final _rng = Random.secure();
  Uint8List _randomBytes(int n) {
    final out = Uint8List(n);
    for (var i = 0; i < n; i++) {
      out[i] = _rng.nextInt(256);
    }
    return out;
  }

  // ---------- UI ----------
  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Met Keychain'),
        actions: [
          if (_client != null)
            IconButton(
              icon: const Icon(Icons.link_off),
              tooltip: 'Отключиться',
              onPressed: _disconnect,
            ),
        ],
      ),
      body: _client == null ? _buildScanPanel() : _buildConnectedPanel(),
    );
  }

  Widget _buildScanPanel() {
    final results = _visible;
    return Column(
      children: [
        Padding(
          padding: const EdgeInsets.all(12),
          child: Row(
            children: [
              Expanded(
                child: ElevatedButton.icon(
                  onPressed: _isScanning ? null : _startScan,
                  icon: const Icon(Icons.search),
                  label: Text(
                      _isScanning ? 'Сканирование...' : 'Начать сканирование'),
                  style: ElevatedButton.styleFrom(
                      minimumSize: const Size.fromHeight(50)),
                ),
              ),
              const SizedBox(width: 8),
              Expanded(
                child: ElevatedButton.icon(
                  onPressed: _scanQr,
                  icon: const Icon(Icons.qr_code_scanner),
                  label: const Text('QR-код'),
                  style: ElevatedButton.styleFrom(
                      minimumSize: const Size.fromHeight(50)),
                ),
              ),
            ],
          ),
        ),
        Padding(
          padding: const EdgeInsets.symmetric(horizontal: 12),
          child: Row(
            children: [
              const Text('Только Met'),
              Switch(
                value: _onlyMet,
                onChanged: (v) => setState(() => _onlyMet = v),
              ),
              const Spacer(),
              Text(_rssiThreshold <= -100 ? 'RSSI: любой' : 'RSSI ≥ $_rssiThreshold'),
            ],
          ),
        ),
        Padding(
          padding: const EdgeInsets.symmetric(horizontal: 12),
          child: Slider(
            min: -100,
            max: -40,
            divisions: 60,
            value: _rssiThreshold.toDouble(),
            onChanged: (v) => setState(() => _rssiThreshold = v.round()),
          ),
        ),
        Expanded(
          child: results.isEmpty
              ? const Center(child: Text('Устройства не найдены'))
              : ListView.builder(
                  itemCount: results.length,
                  itemBuilder: (context, i) {
                    final r = results[i];
                    final adv = _metAdv(r);
                    final name = adv?.kind == AdvKind.pairing
                        ? (adv?.deviceNumber ?? '(pairing)')
                        : adv?.kind == AdvKind.anonymous
                            ? 'Met (anon)'
                            : (r.device.platformName.isEmpty
                                ? '(без имени)'
                                : r.device.platformName);
                    return ListTile(
                      leading: Icon(
                        adv != null ? Icons.vpn_key : Icons.bluetooth,
                        color: adv?.kind == AdvKind.pairing
                            ? Colors.orange
                            : adv != null
                                ? Colors.green
                                : null,
                      ),
                      title: Text(name),
                      subtitle: Text('${r.device.remoteId}  RSSI: ${r.rssi}'
                          '${adv != null ? '  v${adv.version}' : ''}'),
                      onTap: () => _connect(r.device),
                    );
                  },
                ),
        ),
      ],
    );
  }

  Widget _buildConnectedPanel() {
    return DefaultTabController(
      length: 3,
      child: Column(
        children: [
          const TabBar(
            tabs: [
              Tab(icon: Icon(Icons.tune), text: 'Управление'),
              Tab(icon: Icon(Icons.list_alt), text: 'Журнал'),
              Tab(icon: Icon(Icons.terminal), text: 'Логи'),
            ],
          ),
          Expanded(
            child: TabBarView(
              children: [
                _buildControlTab(),
                _buildJournalTab(),
                _buildLogsTab(),
              ],
            ),
          ),
        ],
      ),
    );
  }

  Widget _card(String title, List<Widget> children) => Card(
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

  Widget _field(TextEditingController c, String label,
          {bool obscure = false}) =>
      Padding(
        padding: const EdgeInsets.only(bottom: 8),
        child: TextField(
          controller: c,
          obscureText: obscure,
          decoration: InputDecoration(
            labelText: label,
            border: const OutlineInputBorder(),
            isDense: true,
          ),
        ),
      );

  Widget _buildControlTab() {
    final info = _c.info;
    return SingleChildScrollView(
      padding: const EdgeInsets.all(12),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          _card('Устройство', [
            Text('BLE MAC: ${_device?.remoteId.toString() ?? '—'}'),
            Text('Номер: ${info.dev.isEmpty ? '—' : info.dev}'),
            Text('Proto: ${info.proto}  fw: ${info.fw}'),
            Text('Привязан: ${info.paired ? 'да' : 'нет'}   '
                'Время: ${info.timeOk ? 'ок' : 'нет'}   MTU: ${info.mtu}'),
            Text('Владелец: ${_authed ? 'аутентифицирован' : 'нет'}'),
            Text('Сохранено: ${_kOwnerHex ?? '—'}'
                '${_deviceNumber == null ? '' : '  ($_deviceNumber)'}'),
          ]),

          _card('Аутентификация (K_owner)', [
            _field(_kOwnerCtrl, 'K_owner (hex)'),
            Row(
              children: [
                Expanded(
                  child: ElevatedButton.icon(
                    onPressed: _auth,
                    icon: const Icon(Icons.lock_open),
                    label: const Text('AUTH'),
                  ),
                ),
                const SizedBox(width: 8),
                OutlinedButton.icon(
                  onPressed: _randomizeKOwner,
                  icon: const Icon(Icons.casino),
                  label: const Text('RANDOM'),
                ),
              ],
            ),
          ]),

          _card('Привязка (K_pair)', [
            _field(_devNoCtrl, 'Номер устройства (MET-00001)'),
            _field(_kPairCtrl, 'K_pair (hex или base32 из QR)'),
            _field(_nonceCtrl, 'Nonce от сервера (hex)'),
            ElevatedButton.icon(
              onPressed: _pair,
              icon: const Icon(Icons.key),
              label: const Text('PAIR'),
            ),
          ]),

          _card('Часы (подпись сервера)', [
            _field(_kDevCtrl, 'K_dev (hex, только для демо)'),
            const Text('В демо приложение само считает HMAC(K_time, utc‖nonce). '
                'В продакшене подпись приходит с сервера.',
                style: TextStyle(fontSize: 12, color: Colors.black54)),
            const SizedBox(height: 8),
            ElevatedButton.icon(
              onPressed: _timeSet,
              icon: const Icon(Icons.schedule),
              label: const Text('TIME_SET'),
            ),
          ]),

          _card('Питание и настройки', [
            _field(_batteryCtrl, 'Батарея'),
            Wrap(
              spacing: 8,
              runSpacing: 8,
              children: [
                ElevatedButton.icon(
                  onPressed: _battery,
                  icon: const Icon(Icons.battery_full),
                  label: const Text('BATTERY'),
                ),
                ElevatedButton(
                  onPressed: () => _c.setRssi(-70),
                  child: const Text('SET_RSSI -70'),
                ),
                ElevatedButton(
                  onPressed: () => _c.setAdvInterval(500),
                  child: const Text('ADV 500ms'),
                ),
                ElevatedButton(
                  onPressed: () => _c.setAdvTx(0),
                  child: const Text('ADV_TX 0dBm'),
                ),
                ElevatedButton(
                  onPressed: () => _c.setScan(600, 10000),
                  child: const Text('SCAN 600/10000'),
                ),
              ],
            ),
          ]),

          _card('Отвязка', [
            const Text('Используется nonce сервера из поля выше.'),
            const SizedBox(height: 8),
            ElevatedButton.icon(
              onPressed: _unpair,
              icon: const Icon(Icons.link_off),
              label: const Text('UNPAIR'),
              style: ElevatedButton.styleFrom(backgroundColor: Colors.red),
            ),
          ]),
        ],
      ),
    );
  }

  Widget _buildJournalTab() {
    return SingleChildScrollView(
      padding: const EdgeInsets.all(12),
      child: _card('Журнал встреч', [
        Row(
          children: [
            Expanded(
              child: ElevatedButton.icon(
                onPressed: _fetching ? null : _fetchJournal,
                icon: _fetching
                    ? const SizedBox(
                        width: 18,
                        height: 18,
                        child: CircularProgressIndicator(strokeWidth: 2))
                    : const Icon(Icons.refresh),
                label: Text(_fetching ? 'Загрузка...' : 'Обновить'),
              ),
            ),
            const SizedBox(width: 8),
            Expanded(
              child: ElevatedButton.icon(
                onPressed: _ackAll,
                icon: const Icon(Icons.cloud_done),
                label: const Text('LOG_ACK'),
                style: ElevatedButton.styleFrom(backgroundColor: Colors.green),
              ),
            ),
          ],
        ),
        const SizedBox(height: 8),
        Text('Всего: $_journalTotal, загружено: ${_records.length}'),
        const SizedBox(height: 8),
        if (_records.isEmpty)
          const Text('(пусто)')
        else
          ..._records.map((r) {
            final t1 = DateTime.fromMillisecondsSinceEpoch(r.tFirst * 1000)
                .toUtc()
                .toIso8601String();
            return Padding(
              padding: const EdgeInsets.symmetric(vertical: 4),
              child: Text(
                'seq=${r.seq}  id=${toHex(r.id).substring(0, 16)}…  '
                't=$t1  rssi=${r.rssi}  n=${r.count}',
                style: const TextStyle(fontSize: 12, fontFamily: 'monospace'),
              ),
            );
          }),
      ]),
    );
  }

  Widget _buildLogsTab() {
    return Padding(
      padding: const EdgeInsets.all(12),
      child: Column(
        children: [
          Row(
            children: [
              Expanded(
                child: ElevatedButton.icon(
                  onPressed: () {
                    setState(() => _logs.clear());
                    _snack('Логи очищены');
                  },
                  icon: const Icon(Icons.delete_sweep),
                  label: const Text('Очистить'),
                  style: ElevatedButton.styleFrom(backgroundColor: Colors.red),
                ),
              ),
              const SizedBox(width: 8),
              const Text('Автоскролл'),
              Switch(
                value: _autoScroll,
                onChanged: (v) => setState(() => _autoScroll = v),
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
              child: _logs.isEmpty
                  ? const Center(
                      child: Text('Пока нет записей',
                          style: TextStyle(color: Colors.white54)))
                  : ListView.builder(
                      controller: _logsScroll,
                      itemCount: _logs.length,
                      itemBuilder: (context, i) {
                        final line = _logs[i];
                        Color color = Colors.white70;
                        if (line.contains('→ TX')) {
                          color = Colors.lightBlueAccent;
                        } else if (line.contains('← RX') ||
                            line.contains('RECEIPT')) {
                          color = Colors.lightGreenAccent;
                        } else if (line.contains('--')) {
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
}
