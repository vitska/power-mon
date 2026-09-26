package ru.vitska.powermon.ui

import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import ru.vitska.powermon.ble.FirmwareReleases
import ru.vitska.powermon.ble.FwVersion
import ru.vitska.powermon.ble.Link
import ru.vitska.powermon.model.MonitorViewModel

/**
 * What the board runs, what is published, and the button between them.
 *
 * An update is offered only when the release is strictly newer than the board, by the
 * version numbers both carry. Reinstalling the same version or going back is possible,
 * but behind a dialog that says so -- the one-tap path only ever moves forward.
 */
@Composable
fun FirmwareScreen(vm: MonitorViewModel) {
    val link by vm.link.collectAsState()
    val fw by vm.firmware.collectAsState()
    val shake by vm.handshake.collectAsState()
    val context = LocalContext.current

    var confirmFlash by remember { mutableStateOf<String?>(null) }
    var pickedFile by remember { mutableStateOf<ByteArray?>(null) }

    LaunchedEffect(Unit) { if (!fw.checked && !fw.checking) vm.checkLatest() }

    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) {
            uri: Uri? ->
        if (uri != null) {
            pickedFile = context.contentResolver.openInputStream(uri)?.use { it.readBytes() }
            confirmFlash = "file"
        }
    }

    val device = FwVersion.parse(fw.ota?.version ?: shake.firmware)
    val latest = fw.latest?.version
    val newer = device != null && latest != null && latest > device
    val ready = link == Link.Ready && !fw.updating

    Column(
        Modifier.verticalScroll(rememberScrollState()).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        if (link != Link.Ready && !fw.updating) {
            Warn("Not connected. The board's firmware cannot be read or updated until it is.")
        }
        if (fw.unsupported) {
            Warn(
                "This board's firmware (${shake.firmware}) predates Bluetooth updates. " +
                    "Flash it once over USB with tools/flash.ps1; every update after that " +
                    "can come from here."
            )
        }
        fw.error?.let { Warn(it) }
        fw.notice?.let {
            Section("Done") { Text(it) }
        }

        Section("On the board") {
            KV("firmware", fw.ota?.version ?: shake.firmware.ifEmpty { "—" })
            fw.ota?.let { o ->
                KV("slot", o.running ?: "—")
                KV("state", if (o.onProbation) "on probation" else "confirmed")
                o.spareVersion?.let { KV("previous", it) }
                o.raw["build"]?.let { KV("build", it) }
            }
        }

        fw.ota?.takeIf { it.onProbation }?.let { o ->
            Section("New firmware on probation") {
                Text(
                    "The board is running an update that has not been confirmed. Unless it " +
                        "is, the board goes back to the previous firmware" +
                        (o.probationSeconds?.let { " in ${it / 60} min ${it % 60} s" } ?: "") +
                        ", or at the next reset.",
                    style = MaterialTheme.typography.bodyMedium,
                )
                Spacer(Modifier.height(8.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(onClick = { vm.confirmFirmware() }, enabled = ready) { Text("Keep it") }
                    if (o.canRollback) {
                        OutlinedButton(onClick = { vm.rollbackFirmware() }, enabled = ready) {
                            Text("Roll back")
                        }
                    }
                }
            }
        }

        Section("Published") {
            when {
                fw.checking -> Text("checking github.com/${FirmwareReleases.REPO}...")
                fw.latest != null -> {
                    KV("latest release", fw.latest!!.tag)
                    Text(
                        when {
                            device == null -> "The board's version is not known yet."
                            newer -> "Newer than the board's $device."
                            latest == device -> "The board is up to date."
                            else -> "Older than the board's $device."
                        },
                        style = MaterialTheme.typography.bodyMedium,
                    )
                }
                fw.checked -> Text("No release with ${FirmwareReleases.ASSET} is published yet.")
                else -> Text("Not checked.")
            }
            Spacer(Modifier.height(8.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                if (fw.latest != null) {
                    Button(
                        onClick = { if (newer) vm.updateFromRelease() else confirmFlash = "release" },
                        enabled = ready && !fw.unsupported,
                    ) { Text(if (newer) "Update to ${fw.latest!!.tag}" else "Reinstall…") }
                }
                OutlinedButton(onClick = { vm.checkLatest() }, enabled = !fw.checking) {
                    Text("Check again")
                }
            }
        }

        if (fw.updating) {
            Section("Updating") {
                Text(fw.phase ?: "", style = MaterialTheme.typography.bodyMedium)
                Spacer(Modifier.height(8.dp))
                val p = fw.progress
                if (p != null) {
                    LinearProgressIndicator(progress = { p }, modifier = Modifier.fillMaxWidth())
                } else {
                    LinearProgressIndicator(Modifier.fillMaxWidth())
                }
                Spacer(Modifier.height(8.dp))
                Text(
                    "Keep the phone near the board. If this is interrupted the board keeps " +
                        "running its current firmware.",
                    style = MaterialTheme.typography.labelMedium,
                )
                TextButton(onClick = { vm.cancelUpdate() }) { Text("Cancel") }
            }
        }

        Section("From a file") {
            Text(
                "Flash a bat-monitor.bin built locally (build/bat-monitor.bin). Its version " +
                    "and chip are checked before anything is sent.",
                style = MaterialTheme.typography.bodyMedium,
            )
            Spacer(Modifier.height(8.dp))
            OutlinedButton(
                onClick = { picker.launch(arrayOf("application/octet-stream", "*/*")) },
                enabled = ready && !fw.unsupported,
            ) { Text("Choose file…") }
        }
    }

    confirmFlash?.let { what ->
        AlertDialog(
            onDismissRequest = { confirmFlash = null; pickedFile = null },
            title = { Text(if (what == "file") "Flash this file?" else "Reinstall ${fw.latest?.tag}?") },
            text = {
                Text(
                    (if (what == "file") "The file replaces the board's firmware ($device). "
                    else "This is not newer than the board's $device. ") +
                        "The board restarts; if the new firmware does not come back and " +
                        "get confirmed, it rolls back on its own."
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    if (what == "file") pickedFile?.let { vm.updateFromFile(it) }
                    else vm.updateFromRelease()
                    confirmFlash = null
                    pickedFile = null
                }) { Text("Flash") }
            },
            dismissButton = {
                TextButton(onClick = { confirmFlash = null; pickedFile = null }) { Text("Cancel") }
            },
        )
    }
}
