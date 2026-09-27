package ru.vitska.powermon.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import ru.vitska.powermon.model.MonitorViewModel

/**
 * The raw console, for everything the two structured screens do not cover.
 *
 * `help` is here rather than on the Configure screen because CLI.md puts it at about
 * 1.5 KB, near the truncation cap -- it is a thing to read, not a thing to parse.
 */
@Composable
fun ConsoleScreen(vm: MonitorViewModel) {
    val lines by vm.console.collectAsState()
    val busy by vm.busy.collectAsState()
    var entry by remember { mutableStateOf("") }
    val listState = rememberLazyListState()
    val clipboard = LocalClipboardManager.current

    LaunchedEffect(lines.size) {
        if (lines.isNotEmpty()) listState.animateScrollToItem(lines.size - 1)
    }

    Column(Modifier.fillMaxSize().padding(12.dp)) {
        Card(Modifier.fillMaxWidth().weight(1f)) {
            SelectionContainer {
                LazyColumn(
                    state = listState,
                    modifier = Modifier.fillMaxSize().padding(10.dp),
                ) {
                    items(lines) { l ->
                        Text(
                            l,
                            fontFamily = FontFamily.Monospace,
                            fontSize = 12.sp,
                            color = if (l.startsWith("> ")) {
                                MaterialTheme.colorScheme.primary
                            } else {
                                MaterialTheme.colorScheme.onSurface
                            },
                        )
                    }
                }
            }
        }

        Spacer(Modifier.padding(4.dp))

        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            OutlinedTextField(
                value = entry,
                onValueChange = { entry = it },
                label = { Text("command") },
                singleLine = true,
                modifier = Modifier.weight(1f),
            )
            Spacer(Modifier.width(8.dp))
            Button(
                // One command at a time: the device runs them on a single worker task
                // behind the sensor mutex, so the send button waits its turn.
                onClick = {
                    val c = entry.trim()
                    if (c.isNotEmpty()) {
                        vm.launchCommand(c)
                        entry = ""
                    }
                },
                enabled = !busy && entry.isNotBlank(),
            ) { Text("Send") }
        }

        Row(
            Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(4.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            TextButton(onClick = { vm.launchCommand("help") }) { Text("help") }
            TextButton(onClick = { vm.launchCommand("options") }) { Text("options") }
            TextButton(onClick = { vm.launchCommand("stats") }) { Text("stats") }
            TextButton(onClick = { clipboard.setText(AnnotatedString(lines.joinToString("\n"))) }) {
                Text("copy all")
            }
            TextButton(onClick = { vm.clearConsole() }) { Text("clear") }
        }

        Text(
            // Worth stating in the UI, since it is the one command whose behaviour
            // differs by transport and its refusal would otherwise look like a bug.
            "mon is local-only and refuses over BLE. Log lines never cross BLE either, " +
                "so this transcript carries only command output and telemetry.",
            style = MaterialTheme.typography.labelSmall,
        )
    }
}
