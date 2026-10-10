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
const String kRxCharUuid  = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"; // write
const String kTxCharUuid  = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"; // notify

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
  // ---------- Поля состояния ----------
  BluetoothDevice? _device;
  BluetoothCharacteristic? _rxChar;
  StreamSubscription? _scanSub;
  StreamSubscription? _dataSub;
  StreamSubscription? _connSub;

  bool _isScanning = false;
  bool _isConnected = false;
  List<ScanResult> _scanResults = [];

  String _rxBuffer = "";
  String _deviceName = "";
  String _deviceListJson = "";
  String _deviceMac = "";      // MAC, полученный от MCU командой GET_MAC
  bool _nameRetried = false;   // защита от зацикливания при retry

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
    super.dispose();
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

    if (!(await FlutterBluePlus.isSupported)) {
      _showSnack("BLE не поддерживается устройством");
      setState(() => _isScanning = false);
      return;
    }

    await FlutterBluePlus.stopScan();
    _scanSub = FlutterBluePlus.scanResults.listen((results) {
      setState(() => _scanResults = results);
    });

    await FlutterBluePlus.startScan(
      timeout: const Duration(seconds: 15),
      androidScanMode: AndroidScanMode.lowLatency,
    );
    setState(() => _isScanning = false);
  }

  // ---------- Подключение с retry ----------
  Future<void> _connect(BluetoothDevice device) async {
    const maxAttempts = 4;

    for (int attempt = 1; attempt <= maxAttempts; attempt++) {
      try {
        await FlutterBluePlus.stopScan();
        await Future.delayed(const Duration(milliseconds: 500));

        if (device.isConnected) {
          await device.disconnect();
          await Future.delayed(const Duration(milliseconds: 300));
        }

        await device.connect(
          timeout: const Duration(seconds: 15),
          autoConnect: false,
        );
        await Future.delayed(const Duration(milliseconds: 200));

        try {
          final mtu = await device.requestMtu(512);
          debugPrint("MTU установлен: $mtu");
          await Future.delayed(const Duration(milliseconds: 200)); // Ждём применения
        } catch (e) {
          debugPrint("Ошибка установки MTU: $e");
        }

        _device = device;
        setState(() => _deviceMac = "");
        _connSub = device.connectionState.listen((state) {
          setState(() =>
              _isConnected = state == BluetoothConnectionState.connected);
        });

        final services = await device.discoverServices();
        bool found = false;
        for (final service in services) {
          if (service.uuid.toString().toLowerCase() ==
              kServiceUuid.toLowerCase()) {
            found = true;
            for (final c in service.characteristics) {
              final cuuid = c.uuid.toString().toLowerCase();
              if (cuuid == kRxCharUuid.toLowerCase()) {
                _rxChar = c;
              } else if (cuuid == kTxCharUuid.toLowerCase()) {
                await c.setNotifyValue(true);
                _dataSub = c.onValueReceived.listen(_onDataReceived);
              }
            }
          }
        }

        if (!found) throw Exception("NUS-сервис не найден");

        // ---- WARM-UP: прогоняем PING/PONG, пока BLE UART не оживёт ----
        _showSnack("Инициализация канала...");
        await _warmUp();

        _showSnack("✓ Подключено к ${device.platformName} (попытка $attempt)");
        return;
      } catch (e) {
        debugPrint("Connect attempt $attempt failed: $e");
        try { await device.disconnect(); } catch (_) {}

        await _dataSub?.cancel();
        await _connSub?.cancel();
        _dataSub = null;
        _connSub = null;

        if (attempt == maxAttempts) {
          _showSnack("✗ Не удалось подключиться после $maxAttempts попыток");
          return;
        }
        _showSnack("Попытка $attempt не удалась, повторяем...");
        await Future.delayed(Duration(milliseconds: 800 + attempt * 400));
      }
    }
  }

  Future<void> _warmUp() async {
      for (int i = 0; i < 8; i++) {
        try {
          await _rxChar!.write(utf8.encode("PING\n"));
        } catch (e) {
          debugPrint("warm-up write error: $e");
        }
        // Ждём больше — Samsung даёт до 500 мс задержки
        await Future.delayed(const Duration(milliseconds: 300));
        if (_rxBuffer.contains("PONG")) {
          debugPrint("Warm-up OK on try ${i + 1}");
          _rxBuffer = "";
          return;
        }
      }
      debugPrint("Warm-up failed — proceeding anyway");
      _rxBuffer = "";
  }

  // ---------- Приём данных ----------
  void _onDataReceived(List<int> data) {
    _rxBuffer += utf8.decode(data, allowMalformed: true);

    while (_rxBuffer.contains('\n')) {
      final idx = _rxBuffer.indexOf('\n');
      final line = _rxBuffer.substring(0, idx).trim();
      _rxBuffer = _rxBuffer.substring(idx + 1);
      if (line.isNotEmpty) _handleMessage(line);
    }
  }

  void _handleMessage(String msg) {
  if (msg.startsWith('[')) {
    setState(() => _deviceListJson = msg);
    _showSnack("← Список устройств получен");
    return;
  }

  if (msg.startsWith("NAME:")) {
      final name = msg.substring(5).trim();
      setState(() {
        _deviceName = name;
        _nameController.text = name;
      });
      _showSnack("← Имя: ${name.isEmpty ? '(пусто)' : name}");
      return;
  }

  if (msg.startsWith("MAC:")) {
    final mac = msg.substring(4).trim();
    setState(() => _deviceMac = mac);
    _showSnack("← MAC: $mac");
    return;
  }

  if (msg.startsWith("OK:")) {
    final payload = msg.substring(3).trim();
    if (payload.isNotEmpty &&
        payload != "cleared" &&
        payload != "rebooting") {
      setState(() {
        _deviceName = payload;
        _nameController.text = payload;
      });
      _showSnack("✓ Имя сохранено: $payload");
    } else if (payload == "cleared") {
      setState(() => _deviceListJson = "");
      _showSnack("✓ Журнал очищен");
    } else if (payload == "rebooting") {
      _showSnack("✓ Перезагрузка MCU...");
    } else {
      _showSnack("✓ OK");
    }
    return;
  }

  if (msg.startsWith("ERROR")) {
    _showSnack("✗ $msg");
    return;
  }

  _showSnack("← $msg");
}

  // ---------- Отправка команды ----------
  Future<void> _sendCommand(String cmd, {String? label}) async {
    if (_rxChar == null) {
      _showSnack("Не подключено");
      return;
    }

    try {
      final data = utf8.encode("$cmd\n");
      const chunkSize = 20; // Безопасный размер для MTU 23

      // Отправляем данные частями
      for (int i = 0; i < data.length; i += chunkSize) {
        final end = (i + chunkSize < data.length) ? i + chunkSize : data.length;
        final chunk = data.sublist(i, end);

        // Без response для скорости, либо с ним для надёжности (но медленнее)
        await _rxChar!.write(chunk, withoutResponse: true);
        
        // Небольшая пауза, чтобы MCU успел обработать
        if (end < data.length) {
          await Future.delayed(const Duration(milliseconds: 20));
        }
      }
      _showSnack("→ Отправлено: ${label ?? cmd}");
    } catch (e) {
      _showSnack("✗ Ошибка отправки: $e");
    }
  }

  Future<void> _disconnect() async {
    await _dataSub?.cancel();
    await _connSub?.cancel();
    await _device?.disconnect();
    setState(() {
      _deviceMac = "";
      _nameRetried = false;
      _device = null;
      _rxChar = null;
      _isConnected = false;
      _deviceName = "";
      _deviceListJson = "";
      _rxBuffer = "";
    });
    _showSnack("Отключено");
  }

  void _showSnack(String msg) {
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text(msg), duration: const Duration(seconds: 2)),
    );
  }

  // ---------- Работа с журналом ----------
  String _two(int n) => n.toString().padLeft(2, '0');

  String _formatLogForFile() {
    final now = DateTime.now();
    final buf = StringBuffer();

    buf.writeln("=== BLE Tracker log ===");
    buf.writeln("Saved:   ${now.year}-${_two(now.month)}-${_two(now.day)} "
        "${_two(now.hour)}:${_two(now.minute)}:${_two(now.second)}");
    buf.writeln("Device:  ${_device?.platformName ?? '?'}");
    buf.writeln("MAC:     ${_device?.remoteId.toString() ?? '?'}");
    buf.writeln("Name:    ${_deviceName.isEmpty ? '?' : _deviceName}");
    buf.writeln("----------------------");

    try {
      final data = json.decode(_deviceListJson) as List;
      for (final d in data) {
        buf.writeln("${d['mac']}   RSSI: ${d['rssi']}   t: ${d['time']}");
      }
    } catch (_) {
      buf.writeln(_deviceListJson);
    }

    buf.writeln("=== end ===");
    return buf.toString();
  }

  Future<void> _saveLogToFile() async {
    if (_deviceListJson.isEmpty) {
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
      _showSnack("✓ Файл готов: ${file.path.split('/').last}");
    } catch (e) {
      _showSnack("✗ Ошибка сохранения: $e");
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
        Expanded(
          child: _scanResults.isEmpty
              ? const Center(child: Text("Устройства не найдены"))
              : ListView.builder(
                  itemCount: _scanResults.length,
                  itemBuilder: (context, i) {
                    final r = _scanResults[i];
                    final advName = r.advertisementData.advName;
                    final platformName = r.device.platformName;
                    final name = advName.isNotEmpty
                        ? advName
                        : (platformName.isNotEmpty
                            ? platformName
                            : "(без имени)");
                    return ListTile(
                      leading: const Icon(Icons.bluetooth),
                      title: Text(name),
                      subtitle: Text(
                          "${r.device.remoteId}  RSSI: ${r.rssi}"),
                      onTap: () => _connect(r.device),
                    );
                  },
                ),
        ),
      ],
    );
  }

  Widget _buildControlPanel() {
    return SingleChildScrollView(
      padding: const EdgeInsets.all(12),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          // ---- Инфо о подключении ----
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
                  Text("MCU MAC: ${_deviceMac.isEmpty ? '—' : _deviceMac}"),
                  Text("Имя MCU: ${_deviceName.isEmpty ? '—' : _deviceName}"),
                  const SizedBox(height: 8),
                  Row(
                    children: [
                      Expanded(
                        child: ElevatedButton.icon(
                          onPressed: () =>
                              _sendCommand("GET_NAME", label: "GET_NAME"),
                          icon: const Icon(Icons.badge),
                          label: const Text("Имя"),
                        ),
                      ),
                      const SizedBox(width: 8),
                      Expanded(
                        child: ElevatedButton.icon(
                          onPressed: () =>
                              _sendCommand("GET_MAC", label: "GET_MAC"),
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

          // ---- Изменить имя ----
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
                _sendCommand("SET_NAME:$n", label: "SET_NAME:$n");
              },
              icon: const Icon(Icons.save),
              label: const Text("Применить"),
            ),
          ]),

          // ---- Журнал ----
          _card("Посещённые устройства", [
            Row(
              children: [
                Expanded(
                  child: ElevatedButton.icon(
                    onPressed: () =>
                        _sendCommand("GET_DEVICES", label: "GET_DEVICES"),
                    icon: const Icon(Icons.refresh),
                    label: const Text("Обновить"),
                  ),
                ),
                const SizedBox(width: 8),
                Expanded(
                  child: ElevatedButton.icon(
                    onPressed: _saveLogToFile,
                    icon: const Icon(Icons.save_alt),
                    label: const Text("Сохранить"),
                    style: ElevatedButton.styleFrom(
                        backgroundColor: Colors.green),
                  ),
                ),
              ],
            ),
            const SizedBox(height: 8),
            Text(_deviceListJson.isEmpty
                ? "(пусто)"
                : _prettyJson(_deviceListJson)),
            const SizedBox(height: 8),
            ElevatedButton.icon(
              style:
                  ElevatedButton.styleFrom(backgroundColor: Colors.red),
              onPressed: () =>
                  _sendCommand("CLEAR_DEVICES", label: "CLEAR_DEVICES"),
              icon: const Icon(Icons.delete),
              label: const Text("Очистить журнал"),
            ),
          ]),

          // ---- MAC ----
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
                _sendCommand("SET_MAC:$m", label: "SET_MAC:$m");
              },
              icon: const Icon(Icons.wifi_tethering),
              label: const Text("Применить и перезагрузить"),
            ),
          ]),
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

  String _prettyJson(String raw) {
    try {
      final data = json.decode(raw) as List;
      final buf = StringBuffer();
      for (final d in data) {
        buf.writeln("• ${d['mac']}   RSSI: ${d['rssi']}");
      }
      return buf.toString();
    } catch (_) {
      return raw;
    }
  }
}