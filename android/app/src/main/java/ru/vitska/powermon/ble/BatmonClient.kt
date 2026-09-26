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
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull

enum class Link { Idle, Scanning, Connecting, Discovering, Ready, Failed }

/** Write outcomes that are not ATT codes from the device. */
const val GATT_LINK_LOST = -1
const val GATT_NOT_STARTED = -2
const val GATT_TIMEOUT = -3

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
    /** Absent on firmware older than BLE updates; see [otaSupported]. */
    private var ota: BluetoothGattCharacteristic? = null

    /**
     * Android allows one GATT operation in flight per connection; a second write issued
     * before the first completes is simply refused. Commands and firmware chunks share
     * the link, so every write goes through this lock and waits for its callback.
     */
    private val writeLock = Mutex()
    private var writeDone: CompletableDeferred<Int>? = null

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
                // "The first board found" means a monitor: a remote display in update
                // mode is only ever connected to on purpose.
                val first = this@BatmonClient.connectFirstFound &&
                    !name.startsWith(Nus.REMOTE_PREFIX)
                if (wanted || first) {
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
     * The board the user has chosen, or null after an explicit disconnect. While set, a
     * link that drops for any reason -- the board rebooting after an update, walking out
     * of range, a stack hiccup -- is re-established without anyone asking.
     */
    private var wanted: String? = null
    private var reconnectJob: Job? = null
    private var attempts = 0
    private var refreshedThisLink = false

    /**
     * Connects to one board by address, straight away: stops any scan and dials it
     * directly. A board picked from the list is already known, so there is nothing a scan
     * would add except delay. Switching devices goes through here too, so there is exactly
     * one place that tears the old link down.
     */
    fun connect(address: String) {
        stopScan()
        reconnectJob?.cancel()
        closeGatt()
        wanted = address
        attempts = 0
        _deviceName.value = _found.value
            .firstOrNull { it.address.equals(address, ignoreCase = true) }?.name
            ?: _deviceName.value.takeIf { _deviceAddress.value.equals(address, true) }
        dial(address)
    }

    /** Drops the link and stops trying to get it back. */
    fun disconnect() {
        wanted = null
        reconnectJob?.cancel()
        stopScan()
        closeGatt()
        _link.value = Link.Idle
    }

    private fun dial(address: String) {
        val dev = try {
            adapter?.getRemoteDevice(address)
        } catch (e: IllegalArgumentException) {
            _log.tryEmit("Not a usable address: $address")
            null
        } ?: run { wanted = null; _link.value = Link.Failed; return }

        _deviceAddress.value = dev.address
        if (_deviceName.value == null) _deviceName.value = dev.name
        _link.value = Link.Connecting
        // autoConnect=false: a direct connect, which is fast when the board is there. When
        // it is not, the attempt times out after ~30 s and onConnectionStateChange brings
        // us back here via redial().
        gatt = dev.connectGatt(context, false, gattCb, BluetoothDevice.TRANSPORT_LE)
    }

    /** After a drop or a failed attempt: try the same board again, backing off to 5 s. */
    private fun redial() {
        val addr = wanted ?: return
        attempts++
        _link.value = Link.Connecting
        reconnectJob?.cancel()
        reconnectJob = scope.launch {
            delay(minOf(attempts, 5) * 1_000L)
            if (wanted == addr && gatt == null) {
                if (attempts == 1 || attempts % 5 == 0) _log.tryEmit("reconnecting (attempt $attempts)")
                dial(addr)
            }
        }
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
        ota = null
        writeDone?.complete(GATT_LINK_LOST)
        _mtu.value = 23
    }

    private val gattCb = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (g !== gatt) return                       // a link we already replaced
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                attempts = 0
                refreshedThisLink = false
                _link.value = Link.Discovering
                // 512 to match the device's preferred ATT MTU; it decides the chunk size
                // for every notification, so asking early is worth it.
                g.requestMtu(512)
            } else {
                // close(), not just forget: Android has a small fixed pool of GATT clients,
                // and a dropped one left open is how status 133 starts appearing.
                closeGatt()
                if (wanted != null) redial() else _link.value = Link.Idle
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
            // Android caches a device's services, and a cache taken before the firmware had
            // the OTA characteristic hides it for good. Once per link, if it is missing,
            // drop the cache and discover again; older firmware just repeats the answer.
            if (svc.getCharacteristic(Nus.OTA) == null && !refreshedThisLink) {
                refreshedThisLink = true
                val refreshed = runCatching {
                    g.javaClass.getMethod("refresh").invoke(g) as Boolean
                }.getOrDefault(false)
                if (refreshed && g.discoverServices()) return
            }
            rx = svc.getCharacteristic(Nus.RX)
            tx = svc.getCharacteristic(Nus.TX)
            ota = svc.getCharacteristic(Nus.OTA)
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

        override fun onCharacteristicWrite(
            g: BluetoothGatt,
            c: BluetoothGattCharacteristic,
            status: Int,
        ) {
            writeDone?.complete(status)
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

    // ----------------------------------------------------------------- writes

    /**
     * One acknowledged write. Returns the GATT status: 0 on success, the device's ATT
     * error code when it refused, [GATT_LINK_LOST] when there is no link, or
     * [GATT_TIMEOUT] when no acknowledgement came at all.
     */
    private suspend fun write(c: BluetoothGattCharacteristic, value: ByteArray): Int =
        writeLock.withLock {
            val g = gatt ?: return GATT_LINK_LOST
            val done = CompletableDeferred<Int>()
            writeDone = done
            val started = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                g.writeCharacteristic(
                    c, value, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                ) == android.bluetooth.BluetoothStatusCodes.SUCCESS
            } else {
                @Suppress("DEPRECATION")
                c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                @Suppress("DEPRECATION")
                c.value = value
                @Suppress("DEPRECATION")
                g.writeCharacteristic(c)
            }
            val status = if (!started) GATT_NOT_STARTED
            else withTimeoutOrNull(5_000) { done.await() } ?: GATT_TIMEOUT
            writeDone = null
            status
        }

    // ----------------------------------------------------------------- firmware update

    /** True when the connected firmware has the update characteristic. */
    val otaSupported: Boolean get() = ota != null

    /** Largest image payload per write: the ATT payload less the 4-byte offset. */
    val otaChunk: Int get() = (_mtu.value - 3 - 4).coerceAtLeast(16)

    /** Writes one firmware chunk; see [write] for the status. */
    suspend fun writeOta(value: ByteArray): Int {
        val c = ota ?: return GATT_LINK_LOST
        return withContext(Dispatchers.IO) { write(c, value) }
    }

    /**
     * A short connection interval for the transfer: one acknowledged write per
     * connection event makes the interval the whole speed limit. Back to balanced
     * afterwards -- telemetry at 10 Hz does not need 7.5 ms.
     */
    fun fastLink(fast: Boolean) {
        gatt?.requestConnectionPriority(
            if (fast) BluetoothGatt.CONNECTION_PRIORITY_HIGH
            else BluetoothGatt.CONNECTION_PRIORITY_BALANCED
        )
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
        val c = rx ?: return null

        val deferred = CompletableDeferred<Response>()
        awaiting = deferred
        parser.expect(cmd)

        val payload = (cmd + "\n").toByteArray()
        val chunk = (_mtu.value - 3).coerceAtLeast(20)
        var off = 0
        while (off < payload.size) {
            val take = minOf(chunk, payload.size - off)
            val st = write(c, payload.copyOfRange(off, off + take))
            if (st != BluetoothGatt.GATT_SUCCESS) {
                awaiting = null
                _log.tryEmit("write failed ($st): $cmd")
                return null
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
