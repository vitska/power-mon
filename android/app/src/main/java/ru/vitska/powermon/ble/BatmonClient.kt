package ru.vitska.powermon.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.util.Log
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull

enum class Link { Idle, Scanning, Connecting, Discovering, Ready, Failed }

/** A board seen in a scan. */
data class Discovered(val address: String, val name: String, val rssi: Int)

/**
 * One BLE connection at a time, to whichever board was picked, exposing the console as
 * suspend calls.
 *
 * The two rules from CLI.md that shape everything here:
 *
 *   - ONE COMMAND AT A TIME. The device runs commands on a single worker task behind a
 *     sensor mutex. Commands are therefore serialised through a channel, and each waits
 *     for its 0x04 before the next is written.
 *   - SUBSCRIBE BEFORE SENDING. Output to an unsubscribed client is discarded, not
 *     buffered, and the command still executes -- so `Ready` is not reached until the
 *     notification descriptor write has actually completed.
 */
@SuppressLint("MissingPermission")
class BatmonClient(private val context: Context, private val scope: CoroutineScope) {

    private val tag = "BatmonClient"

    private val _link = MutableStateFlow(Link.Idle)
    val link = _link.asStateFlow()

    private val _deviceName = MutableStateFlow<String?>(null)
    val deviceName = _deviceName.asStateFlow()

    private val _deviceAddress = MutableStateFlow<String?>(null)
    val deviceAddress = _deviceAddress.asStateFlow()

    /** Boards seen since the current scan started, most recently seen name winning. */
    private val _found = MutableStateFlow<List<Discovered>>(emptyList())
    val found = _found.asStateFlow()

    private val _scanning = MutableStateFlow(false)
    val scanning = _scanning.asStateFlow()

    private val _records = MutableSharedFlow<Record>(extraBufferCapacity = 256)
    val records = _records.asSharedFlow()

    private val _log = MutableSharedFlow<String>(extraBufferCapacity = 256)
    val log = _log.asSharedFlow()

    private val _mtu = MutableStateFlow(23)
    val mtu = _mtu.asStateFlow()

    private var gatt: BluetoothGatt? = null
    private var rx: BluetoothGattCharacteristic? = null
    private var tx: BluetoothGattCharacteristic? = null

    private var awaiting: CompletableDeferred<Response>? = null

    private val parser = StreamParser(
        onRecord = { r -> _records.tryEmit(r) },
        onResponse = { r -> awaiting?.complete(r); awaiting = null },
        onLog = { l -> _log.tryEmit(l) },
    )

    /** Serialises commands; capacity is deliberate back-pressure, not a queue to fill. */
    private data class CommandJob(val cmd: String, val reply: CompletableDeferred<Response?>)
    private val jobs = Channel<CommandJob>(capacity = 8)

    private val manager: BluetoothManager? =
        context.getSystemService(BluetoothManager::class.java)
    private val adapter: BluetoothAdapter? get() = manager?.adapter

    init {
        scope.launch { pump() }
    }

    // ----------------------------------------------------------------- scanning

    private var scanCb: ScanCallback? = null
    private var scanTimer: Job? = null
    /** When set, a scan result at this address connects instead of just being listed. */
    private var autoConnectTo: String? = null
    /** With no history to honour there is no ambiguity to respect: take the first hit. */
    private var connectFirstFound = false

    /**
     * Starts a discovery scan. Nothing connects on its own unless [autoConnectTo] names an
     * address, which is how the remembered board is picked up at startup: a scan hit proves
     * it is in range, where a direct connect to a powered-down board just stalls.
     */
    fun startScan(
        autoConnectTo: String? = null,
        connectFirstFound: Boolean = false,
        timeoutMs: Long = 20_000,
    ) {
        val scanner = adapter?.bluetoothLeScanner ?: run {
            _link.value = Link.Failed
            _log.tryEmit("Bluetooth is off or unavailable")
            return
        }
        stopScan()
        this.autoConnectTo = autoConnectTo
        this.connectFirstFound = connectFirstFound
        _found.value = emptyList()
        _scanning.value = true
        if (_link.value != Link.Ready) _link.value = Link.Scanning

        /*
         * Filter by NAME, not by service UUID. CLI.md warns that the 128-bit NUS UUID
         * lives in the SCAN RESPONSE, not the advertisement, because a 16-byte UUID plus
         * the name does not fit in 31 bytes -- and a ScanFilter on service UUID misses
         * scan-response-only UUIDs on a good number of Android stacks.
         */
        val cb = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                val dev = result.device ?: return
                val name = dev.name ?: result.scanRecord?.deviceName ?: return
                if (!name.startsWith(Nus.NAME_PREFIX)) return
                note(Discovered(dev.address, name, result.rssi))
                val wanted = dev.address.equals(this@BatmonClient.autoConnectTo, true)
                if (wanted || this@BatmonClient.connectFirstFound) {
                    this@BatmonClient.autoConnectTo = null
                    this@BatmonClient.connectFirstFound = false
                    connect(dev.address)
                }
            }

            override fun onBatchScanResults(results: MutableList<ScanResult>) {
                results.forEach { onScanResult(ScanSettings.CALLBACK_TYPE_ALL_MATCHES, it) }
            }

            override fun onScanFailed(errorCode: Int) {
                Log.w(tag, "scan failed $errorCode")
                _scanning.value = false
                if (_link.value == Link.Scanning) _link.value = Link.Failed
                _log.tryEmit("Scan failed, code $errorCode")
            }
        }
        scanCb = cb
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        scanner.startScan(null, settings, cb)

        // A scan left running is a battery cost and, past 30 minutes, gets throttled by
        // the framework anyway. Stop on our own terms.
        scanTimer = scope.launch {
            delay(timeoutMs)
            if (_scanning.value) {
                stopScan()
                if (_link.value == Link.Scanning) _link.value = Link.Idle
            }
        }
    }

    private fun note(d: Discovered) {
        val rest = _found.value.filterNot { it.address.equals(d.address, ignoreCase = true) }
        _found.value = (rest + d).sortedByDescending { it.rssi }
    }

    fun stopScan() {
        connectFirstFound = false
        autoConnectTo = null
        scanTimer?.cancel()
        scanTimer = null
        scanCb?.let { adapter?.bluetoothLeScanner?.stopScan(it) }
        scanCb = null
        _scanning.value = false
    }

    // ----------------------------------------------------------------- connection

    /**
     * Connects to one board by address, dropping whatever link is open. Switching devices
     * goes through here, so there is exactly one place that tears the old one down.
     */
    fun connect(address: String) {
        val dev = try {
            adapter?.getRemoteDevice(address)
        } catch (e: IllegalArgumentException) {
            _log.tryEmit("Not a usable address: $address")
            null
        } ?: run { _link.value = Link.Failed; return }

        stopScan()
        closeGatt()
        _deviceAddress.value = dev.address
        _deviceName.value = dev.name ?: _found.value
            .firstOrNull { it.address.equals(dev.address, ignoreCase = true) }?.name
        _link.value = Link.Connecting
        gatt = dev.connectGatt(context, false, gattCb, BluetoothDevice.TRANSPORT_LE)
    }

    fun disconnect() {
        stopScan()
        closeGatt()
        _link.value = Link.Idle
    }

    private fun closeGatt() {
        // Any command still waiting on this link will never see its 0x04.
        awaiting?.cancel()
        awaiting = null
        gatt?.disconnect()
        gatt?.close()
        gatt = null
        rx = null
        tx = null
        _mtu.value = 23
    }

    private val gattCb = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (g !== gatt) return                       // a link we already replaced
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                _link.value = Link.Discovering
                // 512 to match the device's preferred ATT MTU; it decides the chunk size
                // for every notification, so asking early is worth it.
                g.requestMtu(512)
            } else {
                awaiting?.cancel()
                awaiting = null
                rx = null
                tx = null
                _link.value = Link.Idle
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            _mtu.value = mtu
            g.discoverServices()
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val svc = g.getService(Nus.SERVICE)
            if (svc == null) {
                _log.tryEmit("No Nordic UART service on this device")
                _link.value = Link.Failed
                return
            }
            rx = svc.getCharacteristic(Nus.RX)
            tx = svc.getCharacteristic(Nus.TX)
            val t = tx
            if (rx == null || t == null) {
                _log.tryEmit("NUS present but RX/TX characteristics missing")
                _link.value = Link.Failed
                return
            }
            g.setCharacteristicNotification(t, true)
            val cccd = t.getDescriptor(
                java.util.UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
            )
            if (cccd == null) {
                _log.tryEmit("TX has no CCCD; cannot subscribe")
                _link.value = Link.Failed
                return
            }
            writeDescriptorCompat(g, cccd, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
        }

        override fun onDescriptorWrite(
            g: BluetoothGatt,
            d: BluetoothGattDescriptor,
            status: Int,
        ) {
            // Only now is output actually reaching us, so only now is the link usable.
            _link.value = if (status == BluetoothGatt.GATT_SUCCESS) Link.Ready else Link.Failed
        }

        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            c: BluetoothGattCharacteristic,
            value: ByteArray,
        ) {
            if (c.uuid == Nus.TX) parser.feed(value)
        }

        @Deprecated("Pre-33 delivery path")
        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            c: BluetoothGattCharacteristic,
        ) {
            @Suppress("DEPRECATION")
            if (c.uuid == Nus.TX) parser.feed(c.value ?: return)
        }
    }

    private fun writeDescriptorCompat(
        g: BluetoothGatt,
        d: BluetoothGattDescriptor,
        value: ByteArray,
    ) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeDescriptor(d, value)
        } else {
            @Suppress("DEPRECATION")
            d.value = value
            @Suppress("DEPRECATION")
            g.writeDescriptor(d)
        }
    }

    // ----------------------------------------------------------------- commands

    /**
     * Runs one command and waits for its terminator. Returns null on timeout, which is
     * distinct from a non-zero exit: one means the device never answered, the other means
     * it answered "no".
     */
    suspend fun send(cmd: String): Response? {
        if (_link.value != Link.Ready) return null
        val reply = CompletableDeferred<Response?>()
        jobs.send(CommandJob(cmd, reply))
        return reply.await()
    }

    private suspend fun pump() {
        for (job in jobs) {
            val r = withContext(Dispatchers.IO) { execute(job.cmd) }
            job.reply.complete(r)
        }
    }

    private suspend fun execute(cmd: String): Response? {
        val g = gatt ?: return null
        val c = rx ?: return null

        val deferred = CompletableDeferred<Response>()
        awaiting = deferred
        parser.expect(cmd)

        val payload = (cmd + "\n").toByteArray()
        val chunk = (_mtu.value - 3).coerceAtLeast(20)
        var off = 0
        while (off < payload.size) {
            val take = minOf(chunk, payload.size - off)
            val slice = payload.copyOfRange(off, off + take)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                g.writeCharacteristic(c, slice, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT)
            } else {
                @Suppress("DEPRECATION")
                c.value = slice
                @Suppress("DEPRECATION")
                g.writeCharacteristic(c)
            }
            off += take
        }

        val result = try {
            withTimeoutOrNull(Timeouts.forCommand(cmd)) { deferred.await() }
        } catch (e: CancellationException) {
            // The link went away under us, which is not this coroutine being cancelled.
            null
        }
        if (result == null) {
            awaiting = null
            _log.tryEmit("no reply: $cmd")
        }
        return result
    }
}
