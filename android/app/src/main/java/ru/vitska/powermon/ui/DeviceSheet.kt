package ru.vitska.powermon.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material3.AssistChip
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import ru.vitska.powermon.model.MonitorViewModel

/**
 * The device picker. Remembered boards and freshly scanned ones are one list, because to
 * the person holding the phone they are the same thing: boards they might want on screen.
 * What differs is whether there is a signal reading, which is also exactly what tells
 * them whether tapping it will work.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun DeviceSheet(vm: MonitorViewModel, onDismiss: () -> Unit) {
    val devices by vm.devices.collectAsState()
    val scanning by vm.scanning.collectAsState()
    val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)

    ModalBottomSheet(onDismissRequest = onDismiss, sheetState = sheetState) {
        Column(Modifier.padding(start = 20.dp, end = 20.dp, bottom = 28.dp)) {
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Text("Devices", style = MaterialTheme.typography.titleLarge)
                Spacer(Modifier.fillMaxWidth().weight(1f))
                if (scanning) {
                    CircularProgressIndicator(Modifier.height(18.dp))
                    Spacer(Modifier.padding(4.dp))
                    TextButton(onClick = { vm.stopScan() }) { Text("Stop") }
                } else {
                    TextButton(onClick = { vm.scan() }) { Text("Scan") }
                }
            }

            Text(
                "Tap a board to connect to it now. One connection at a time — tapping " +
                    "another board drops the current link. Scan finds boards not yet listed.",
                style = MaterialTheme.typography.bodySmall,
            )
            Spacer(Modifier.height(12.dp))

            if (devices.isEmpty()) {
                Text(
                    if (scanning) "Scanning..." else
                        "No boards yet. Hit Scan with a board powered up and in range.",
                    style = MaterialTheme.typography.bodyMedium,
                )
            }

            devices.forEach { d ->
                HorizontalDivider()
                Row(
                    Modifier
                        .fillMaxWidth()
                        .clickable(enabled = !d.connected) { vm.connectTo(d.address, d.name); onDismiss() }
                        .padding(vertical = 12.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Column(Modifier.weight(1f)) {
                        Text(d.name, style = MaterialTheme.typography.titleSmall)
                        Text(
                            d.address,
                            fontFamily = FontFamily.Monospace,
                            style = MaterialTheme.typography.bodySmall,
                        )
                        Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                            if (d.connected) {
                                Text("connected", style = MaterialTheme.typography.labelSmall,
                                    color = MaterialTheme.colorScheme.primary)
                            }
                            // A signal reading only exists while scanning. A saved board
                            // without one can still be tapped: the client dials it and
                            // keeps trying until it answers.
                            val signal = d.rssi?.let { "$it dBm" }
                                ?: if (scanning) "not seen yet" else null
                            if (signal != null) {
                                Text(signal, style = MaterialTheme.typography.labelSmall)
                            }
                            if (d.isRemote) {
                                Text("remote display — firmware update",
                                    style = MaterialTheme.typography.labelSmall,
                                    color = MaterialTheme.colorScheme.tertiary)
                            }
                            if (d.known) {
                                Text("saved", style = MaterialTheme.typography.labelSmall)
                            }
                        }
                    }
                    if (d.known) {
                        IconButton(onClick = { vm.forget(d.address) }) {
                            Icon(Icons.Filled.Close, contentDescription = "Forget " + d.name)
                        }
                    }
                }
            }

            HorizontalDivider()
            Spacer(Modifier.height(12.dp))
            AssistChip(
                onClick = { vm.disconnect() },
                label = { Text("Disconnect") },
            )
        }
    }
}
