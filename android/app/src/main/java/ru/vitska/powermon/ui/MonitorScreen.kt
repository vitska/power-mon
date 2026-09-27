package ru.vitska.powermon.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.Button
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import ru.vitska.powermon.ble.Chemistries
import ru.vitska.powermon.ble.FirmwareTarget
import ru.vitska.powermon.ble.Link
import ru.vitska.powermon.model.MonitorViewModel

private fun f(v: Double?, dp: Int, unit: String = ""): String =
    if (v == null) "—" else String.format("%.${dp}f%s", v, unit)

@Composable
fun MonitorScreen(vm: MonitorViewModel, onPickDevice: () -> Unit = {}) {
    val t by vm.telemetry.collectAsState()
    val shake by vm.handshake.collectAsState()
    val link by vm.link.collectAsState()
    val scanning by vm.scanning.collectAsState()
    val cfg by vm.config.collectAsState()
    val history by vm.history.collectAsState()

    Column(
        Modifier.verticalScroll(rememberScrollState()).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        if (link != Link.Ready) {
            // Without this the panel is a wall of dashes with no way forward: the way in
            // is the device picker, so say so where the dashes are.
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(16.dp)) {
                    Text(
                        if (scanning) "Looking for boards" else "No board connected",
                        style = MaterialTheme.typography.titleSmall,
                    )
                    Spacer(Modifier.height(4.dp))
                    Text(
                        if (scanning) {
                            "Scanning for anything advertising as batmon-XXXX."
                        } else {
                            "Pick a board to monitor. The last one used is reconnected " +
                                "automatically when the app starts."
                        },
                        style = MaterialTheme.typography.bodySmall,
                    )
                    Spacer(Modifier.height(8.dp))
                    Button(onClick = onPickDevice) { Text("Devices") }
                }
            }
        }

        if (FirmwareTarget.forDeviceName(vm.deviceName.collectAsState().value) ==
            FirmwareTarget.REMOTE && link == Link.Ready
        ) {
            Warn(
                "This is a batmon remote display, not a monitor: it has no telemetry of " +
                    "its own. Only the Firmware tab applies to it."
            )
        }
        if (shake.mismatch) {
            // CLI.md: refuse to drive a protocol you do not know rather than guess.
            Warn(
                "Protocol ${shake.protocol} — this app speaks 3. Fields may be missing " +
                    "or misread; update one side."
            )
        }
        if (t.saturated) {
            // The whole reason the diagnostics group exists: sat=1 means the current
            // reading is a range limit, not a measurement.
            Warn("Shunt channel SATURATED (${t.pga}) — current readings are a range limit, not a measurement.")
        }

        // State of charge gets the space, as it does on the device's own panel.
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp)) {
                // Which curve the percentage comes from: a LiFePO4 pack gauged on the
                // lead-acid curve reads nonsense, and this is where that would show.
                val chem = Chemistries.byKey(cfg.str("battery.chem"))
                Text(
                    "STATE OF CHARGE" +
                        (chem?.let { "  ·  ${it.short} ${cfg.str("battery.cells") ?: "?"}S" } ?: ""),
                    style = MaterialTheme.typography.labelSmall,
                )
                Row(verticalAlignment = Alignment.Bottom) {
                    Text(
                        f(t.socPct, 1),
                        fontSize = 56.sp,
                        fontWeight = FontWeight.SemiBold,
                        fontFamily = FontFamily.Monospace,
                    )
                    Text(" %", style = MaterialTheme.typography.titleMedium)
                }
                Spacer(Modifier.height(8.dp))
                LinearProgressIndicator(
                    progress = { ((t.socPct ?: 0.0) / 100.0).toFloat().coerceIn(0f, 1f) },
                    modifier = Modifier.fillMaxWidth(),
                )
                Spacer(Modifier.height(8.dp))
                Text(
                    "${f(t.chargeAh, 2)} Ah   ·   ${t.state}",
                    style = MaterialTheme.typography.bodyMedium,
                )
            }
        }

        if (link == Link.Ready) {
            HistoryChart(history)
        }

        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Metric("VOLTS", f(t.volts, 3), Modifier.weight(1f))
            Metric("AMPS", f(t.amps, 4), Modifier.weight(1f))
        }
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Metric("WATTS", f(t.watts, 2), Modifier.weight(1f))
            Metric("RATE", t.fastHz?.let { String.format("%.1f Hz", it) } ?: "—",
                Modifier.weight(1f))
        }

        Section("Gauge") {
            KV("OCV estimate", f(t.ocvV, 3, " V"))
            KV("Peukert factor", f(t.peukert, 3))
            KV("Charge", f(t.chargeAh, 3, " Ah"))
            KV("State", t.state)
        }

        Section("Diagnostics") {
            KV("Shunt drop", f(t.shuntMv, 3, " mV"))
            KV("Range", t.pga.ifBlank { "—" })
            KV("Saturated", if (t.saturated) "YES" else "no")
        }

        Section("Environment") {
            // Nulls are meaningful here: an empty CSV field means no sensor, and a BMP280
            // has no humidity channel at all.
            KV("Temperature", f(t.tempC, 2, " °C"))
            KV("Humidity", t.humidPct?.let { f(it, 1, " %RH") } ?: "not available")
            KV("Pressure", f(t.pressHpa, 2, " hPa"))
        }

        if (shake.firmware.isNotBlank()) {
            Section("Device") {
                KV("Firmware", shake.firmware)
                KV("Protocol", shake.protocol?.toString() ?: "—")
                KV("MAC", shake.mac)
            }
        }
    }
}

@Composable
private fun Metric(label: String, value: String, modifier: Modifier = Modifier) {
    Card(modifier) {
        Column(Modifier.padding(14.dp)) {
            Text(label, style = MaterialTheme.typography.labelSmall)
            Text(
                value,
                // A signed four-decimal current ("-0.0216") is two characters longer
                // than anything else shown here and wraps at the display size; step
                // down rather than truncate, since the low-current digits are the
                // ones worth reading.
                fontSize = if (value.length > 6) 21.sp else 26.sp,
                fontFamily = FontFamily.Monospace,
                fontWeight = FontWeight.Medium,
                maxLines = 1,
                softWrap = false,
            )
        }
    }
}

@Composable
fun Section(title: String, content: @Composable () -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(14.dp)) {
            Text(title, style = MaterialTheme.typography.titleSmall)
            Spacer(Modifier.height(6.dp))
            content()
        }
    }
}

/**
 * A short pair sits on one line, label left / value right. A pair too long for that
 * (a long label, or a value with a parenthetical unit conversion) stacks instead --
 * label above, value below -- rather than let a Row split the value mid-word across
 * lines because there was no room left for it.
 */
@Composable
fun KV(k: String, v: String) {
    if (k.length + v.length <= 26) {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            Text(k, style = MaterialTheme.typography.bodyMedium)
            Text(v, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodyMedium)
        }
    } else {
        Column(Modifier.fillMaxWidth().padding(vertical = 2.dp)) {
            Text(k, style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            Text(v, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodyMedium)
        }
    }
}

@Composable
fun Warn(text: String) {
    Card(
        Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.errorContainer),
    ) {
        Text(
            text,
            Modifier.padding(14.dp),
            color = MaterialTheme.colorScheme.onErrorContainer,
            style = MaterialTheme.typography.bodyMedium,
        )
    }
}
