package ru.vitska.powermon

import android.Manifest
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.ui.graphics.Color
import ru.vitska.powermon.ui.AppRoot

class MainActivity : ComponentActivity() {

    /*
     * Bluetooth needs runtime permission, and which permissions depends on the API level:
     * 31+ splits scanning from connecting, and below that a scan is treated as a location
     * request. Asking for the wrong set fails silently with an empty scan, which is a
     * miserable thing to debug.
     */
    private val required: Array<String>
        get() = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }

    private val ask = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { /* The UI reflects the outcome by whether a scan finds anything. */ }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        ask.launch(required)

        setContent {
            val dark = isSystemInDarkTheme()
            // The same instrument palette as the project's other documents: cool
            // blue-grey neutrals with a signal-amber accent.
            val scheme = if (dark) {
                darkColorScheme(
                    primary = Color(0xFFE9A13B),
                    background = Color(0xFF10141A),
                    surface = Color(0xFF171D25),
                    error = Color(0xFFE8837A),
                )
            } else {
                lightColorScheme(
                    primary = Color(0xFF9C5D00),
                    background = Color(0xFFF4F6F8),
                    surface = Color(0xFFFFFFFF),
                    error = Color(0xFFA3322B),
                )
            }
            MaterialTheme(colorScheme = scheme) { AppRoot() }
        }
    }
}
