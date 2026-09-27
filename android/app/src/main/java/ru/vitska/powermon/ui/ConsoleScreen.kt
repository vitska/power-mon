package ru.vitska.powermon.ui

import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import ru.vitska.powermon.model.MonitorViewModel

/** The raw console: sent commands and their responses, selectable/copyable, and a
 *  command box to send more. Nothing else. */
@Composable
fun ConsoleScreen(vm: MonitorViewModel) {
    val lines by vm.console.collectAsState()
    val busy by vm.busy.collectAsState()
    var entry by remember { mutableStateOf("") }
    var wrap by remember { mutableStateOf(true) }
    val listState = rememberLazyListState()

    LaunchedEffect(lines.size) {
        if (lines.isNotEmpty()) listState.animateScrollToItem(lines.size - 1)
    }

    Column(Modifier.fillMaxSize().padding(12.dp)) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text("wrap", style = MaterialTheme.typography.labelMedium)
            Switch(checked = wrap, onCheckedChange = { wrap = it })
        }

        Card(Modifier.fillMaxWidth().weight(1f)) {
            SelectionContainer {
                LazyColumn(
                    state = listState,
                    modifier = Modifier.fillMaxSize().padding(10.dp),
                ) {
                    items(lines) { l ->
                        val sent = l.startsWith("> ")
                        ConsoleLine(l, sent, wrap)
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
    }
}

/** A sent command is bold and in the accent color, with a blank line above it so each
 *  exchange reads as its own block; a response line is plain. */
@Composable
private fun ConsoleLine(text: String, sent: Boolean, wrap: Boolean) {
    if (sent) Spacer(Modifier.height(6.dp))
    val content: @Composable () -> Unit = {
        Text(
            text,
            fontFamily = FontFamily.Monospace,
            fontSize = 12.sp,
            fontWeight = if (sent) FontWeight.Bold else FontWeight.Normal,
            color = if (sent) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurface,
            softWrap = wrap,
            overflow = if (wrap) TextOverflow.Clip else TextOverflow.Visible,
            maxLines = if (wrap) Int.MAX_VALUE else 1,
        )
    }
    if (wrap) {
        content()
    } else {
        Row(Modifier.horizontalScroll(rememberScrollState())) { content() }
    }
}
