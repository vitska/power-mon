package ru.vitska.powermon.ui

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TextButton
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Build
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.Terminal
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.collectAsState
import androidx.compose.ui.Modifier
import androidx.lifecycle.viewmodel.compose.viewModel
import ru.vitska.powermon.ble.Link
import ru.vitska.powermon.model.MonitorViewModel

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AppRoot(vm: MonitorViewModel = viewModel()) {
    var tab by remember { mutableIntStateOf(0) }
    val link by vm.link.collectAsState()
    val name by vm.deviceName.collectAsState()

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Column {
                        Text(name ?: "power-mon", style = MaterialTheme.typography.titleMedium)
                        Text(
                            when (link) {
                                Link.Idle -> "not connected"
                                Link.Scanning -> "scanning for batmon…"
                                Link.Connecting -> "connecting"
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
                    } else {
                        TextButton(onClick = { vm.connect() }) { Text("Connect") }
                    }
                },
            )
        },
        bottomBar = {
            NavigationBar {
                NavigationBarItem(
                    selected = tab == 0,
                    onClick = { tab = 0 },
                    icon = { Icon(Icons.Filled.Info, null) },
                    label = { Text("Monitor") },
                )
                NavigationBarItem(
                    selected = tab == 1,
                    onClick = { tab = 1 },
                    icon = { Icon(Icons.Filled.Build, null) },
                    label = { Text("Configure") },
                )
                NavigationBarItem(
                    selected = tab == 2,
                    onClick = { tab = 2 },
                    icon = { Icon(Icons.Filled.Terminal, null) },
                    label = { Text("Console") },
                )
            }
        },
    ) { pad ->
        Column(Modifier.fillMaxSize().padding(pad)) {
            when (tab) {
                0 -> MonitorScreen(vm)
                1 -> ConfigureScreen(vm)
                else -> ConsoleScreen(vm)
            }
        }
    }
}
