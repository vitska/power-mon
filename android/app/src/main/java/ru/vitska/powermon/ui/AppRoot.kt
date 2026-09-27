package ru.vitska.powermon.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.size
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Bluetooth
import androidx.compose.material.icons.filled.Build
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.SystemUpdate
import androidx.compose.material.icons.filled.Terminal
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.viewmodel.compose.viewModel
import ru.vitska.powermon.ble.Link
import ru.vitska.powermon.ble.Nus
import ru.vitska.powermon.model.MonitorViewModel

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AppRoot(canScan: Boolean, vm: MonitorViewModel = viewModel()) {
    var tab by remember { mutableIntStateOf(0) }
    var showDevices by remember { mutableStateOf(false) }
    val link by vm.link.collectAsState()
    val name by vm.deviceName.collectAsState()
    val scanning by vm.scanning.collectAsState()

    /*
     * Reconnect to the board used last, but only once the Bluetooth permissions are
     * actually held -- a scan started without them fails silently with an empty result,
     * which looks exactly like a board that is switched off.
     */
    LaunchedEffect(canScan) {
        if (canScan) vm.resumeLastOrScan()
    }

    // Opening the picker lists the saved boards at once. It scans only when there is
    // nothing to list; finding a new board is what the sheet's Scan button is for.
    val openDevices = {
        if (vm.devices.value.isEmpty()) vm.scan()
        showDevices = true
    }

    Scaffold(
        topBar = {
            TopAppBar(
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.background,
                ),
                title = {
                    Column {
                        Text(name ?: "power-mon", style = MaterialTheme.typography.titleMedium)
                        Text(
                            when (link) {
                                Link.Idle -> if (scanning) "scanning" else "not connected"
                                Link.Scanning -> "scanning for batmon boards"
                                // A remote display answers only while its update screen
                                // is open; say so, or the retrying looks like a fault.
                                Link.Connecting ->
                                    if (name?.startsWith(Nus.REMOTE_PREFIX) == true)
                                        "waiting — open Settings › FIRMWARE UPDATE on the remote"
                                    else "connecting — retrying until the board answers"
                                Link.Discovering -> "discovering services"
                                Link.Ready -> "ready"
                                Link.Failed -> "failed"
                            },
                            style = MaterialTheme.typography.labelSmall,
                        )
                    }
                },
                actions = {
                    if (link == Link.Ready) {
                        TextButton(onClick = { vm.disconnect() }) { Text("Disconnect") }
                    }
                    IconButton(onClick = openDevices) {
                        Icon(Icons.Filled.Bluetooth, contentDescription = "Devices")
                    }
                },
            )
        },
        bottomBar = {
            val au by vm.appUpdate.collectAsState()
            Box(Modifier.fillMaxWidth()) {
                CompactTabBar(
                    tab = tab,
                    onSelect = { tab = it },
                    items = listOf(
                        Triple("Monitor", Icons.Filled.Info, 0),
                        Triple("Configure", Icons.Filled.Build, 1),
                        Triple("Console", Icons.Filled.Terminal, 2),
                        Triple("Firmware", Icons.Filled.SystemUpdate, 3),
                    ),
                )
                // In the bar's own corner, not a row of its own -- so it adds no height.
                Text(
                    "v" + au.currentVersion,
                    fontSize = 9.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.align(Alignment.BottomEnd).padding(end = 4.dp, bottom = 2.dp),
                )
            }
        },
    ) { pad ->
        Column(Modifier.fillMaxSize().padding(pad)) {
            when (tab) {
                0 -> MonitorScreen(vm, openDevices)
                1 -> ConfigureScreen(vm)
                2 -> ConsoleScreen(vm)
                else -> FirmwareScreen(vm)
            }
        }
    }

    if (showDevices) {
        DeviceSheet(vm) { showDevices = false; vm.stopScan() }
    }
}

/**
 * A plain Row standing in for Material3's NavigationBar, which reserves a fixed 80dp
 * regardless of content -- more than a 4-tab switcher needs, and not something its own
 * API lets a caller shrink. This one is as tall as its icon+label actually are, so the
 * screen above it keeps more of the height for values worth looking at.
 */
@Composable
private fun CompactTabBar(
    tab: Int,
    onSelect: (Int) -> Unit,
    items: List<Triple<String, androidx.compose.ui.graphics.vector.ImageVector, Int>>,
) {
    Row(
        Modifier.fillMaxWidth().height(52.dp),
        horizontalArrangement = Arrangement.SpaceEvenly,
    ) {
        items.forEach { (label, icon, index) ->
            val selected = tab == index
            val color = if (selected) MaterialTheme.colorScheme.primary
                        else MaterialTheme.colorScheme.onSurfaceVariant
            Column(
                Modifier.weight(1f).fillMaxHeight()
                    .clickable { onSelect(index) },
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.Center,
            ) {
                Icon(icon, contentDescription = label, tint = color, modifier = Modifier.size(20.dp))
                Text(label, color = color, fontSize = 10.sp)
            }
        }
    }
}
