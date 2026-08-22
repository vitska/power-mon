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
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.coroutines.CoroutineScope

enum class Link { Idle, Scanning, Connecting, Discovering, Ready, Failed }

/**
 * One BLE connection to one board, exposing the console as suspend calls.
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
    private data class Job(val cmd: String, val reply: CompletableDeferred<Response?>)
    private val jobs = Channel<Job>(capacity = 8)

    private val manager: BluetoothManager? =
        context.getSystemService(BluetoothManager::class.java)
    private val adapter: BluetoothAdapter? get() = manager?.adapter

    init {
        scope.launch { pump() }
    }

    // ----------------------------------------------------------------- scanning

    private var scanCb: ScanCallback? = null

    fun startScan() {
        val scanner = adapter?.bluetoothLeScanner ?: run {
            _link.value = Link.Failed
            _log.tryEmit("Bluetooth is off or unavailable")
            return
        }
        stopScan()
        _link.value = Link.Scanning

        /*
         * Filter by NAME, not by service UUID. CLI.md warns that the 128-bit NUS UUID
         * lives in the SCAN RESPONSE, not the advertisement, because a 16-byte UUID plus
         * the name does not fit in 31 bytes -- and a ScanFilter on service UUID misses
         * scan-response-only UUIDs on a good number of Android stacks.
         */
        val cb = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                val name = result.device?.name ?: result.scanRecord?.deviceName ?: return
                if (!name.startsWith(Nus.NAME_PREFIX)) return
                stopScan()
                connect(result.device, name)
            }

            override fun onScanFailed(errorCode: Int) {
                Log.w(tag, "scan failed $errorCode")
                _link.value = Link.Failed
                _log.tryEmit("Scan failed, code $errorCode")
            }
        }
        scanCb = cb
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        scanner.startScan(null, settings, cb)
    }

    fun stopScan() {
        scanCb?.let { adapter?.bluetoothLeScanner?.stopScan(it) }
        scanCb = null
    }

    // ----------------------------------------------------------------- connection

    private fun connect(device: BluetoothDevice, name: String) {
        _deviceName.value = name
        _link.value = Link.Connecting
        gatt = device.connectGatt(context, false, gattCb, BluetoothDevice.TRANSPORT_LE)
    }

    fun disconnect() {
        stopScan()
        gatt?.disconnect()
        gatt?.close()
        gatt = null
        rx = null
        tx = null
        _link.value = Link.Idle
    }

    private val gattCb = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                _link.value = Link.Discovering
                // 512 to match the device's preferred ATT MTU; it decides the chunk size
                // for every notification, so asking early is worth it.
                g.requestMtu(512)
            } else {
                _link.value = Link.Idle
                rx = null
                tx = null
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
        jobs.send(Job(cmd, reply))
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

        val result = withTimeoutOrNull(Timeouts.forCommand(cmd)) { deferred.await() }
        if (result == null) {
            awaiting = null
            _log.tryEmit("timeout: $cmd")
        }
        return result
    }
}
