package ru.vitska.powermon.ble

import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import org.json.JSONObject

/**
 * A firmware version, MAJOR.MINOR.PATCH with an optional pre-release suffix, compared the
 * way semver does: numerically, and a pre-release ("0.5.0-m2") before its release
 * ("0.5.0"). Anything unparseable is null rather than guessed at -- offering an
 * "update" on the strength of a string comparison is how a board gets downgraded.
 */
data class FwVersion(val major: Int, val minor: Int, val patch: Int, val pre: String) :
    Comparable<FwVersion> {

    override fun compareTo(other: FwVersion): Int =
        compareValuesBy(this, other, { it.major }, { it.minor }, { it.patch })
            .takeIf { it != 0 }
            ?: when {
                pre == other.pre -> 0
                pre.isEmpty() -> 1
                other.pre.isEmpty() -> -1
                else -> pre.compareTo(other.pre)
            }

    override fun toString() = "$major.$minor.$patch" + if (pre.isEmpty()) "" else "-$pre"

    companion object {
        private val RE = Regex("""^v?(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?(?:\+.*)?$""")

        fun parse(s: String?): FwVersion? {
            val m = RE.matchEntire(s?.trim() ?: return null) ?: return null
            val (a, b, c, pre) = m.destructured
            return FwVersion(a.toInt(), b.toInt(), c.toInt(), pre)
        }
    }
}

/**
 * Which of the two firmwares a device runs, and so which image and release series fit
 * it. The monitor is an ESP32-C6; the remote display (remote/) a classic ESP32. Each
 * checks an image against its own project name as well, so sending the wrong one is
 * refused twice -- here, before any byte goes, and on the device.
 */
enum class FirmwareTarget(
    val label: String,
    val project: String,
    val chipId: Int,
    val chipName: String,
    val tagPrefix: String,
    val asset: String,
) {
    MONITOR("battery monitor", "bat-monitor", 13, "ESP32-C6", "v", "bat-monitor.bin"),
    REMOTE("remote display", "batmon-remote", 0, "ESP32", "remote-v", "batmon-remote.bin");

    companion object {
        /** A remote display in update mode advertises as batmon-remote-XXXX. */
        fun forDeviceName(name: String?): FirmwareTarget =
            if (name?.startsWith(Nus.REMOTE_PREFIX) == true) REMOTE else MONITOR
    }
}

/**
 * A firmware image, checked before a single byte goes to the device.
 *
 * ESP-IDF images carry their own identity at fixed offsets: a 24-byte image header whose
 * chip ID says which chip it was built for, then the first segment's 8-byte header, then
 * the app descriptor -- version and project name among it. So the version offered is
 * read from the image itself, not from a release title that could say anything.
 */
class FirmwareImage private constructor(
    val bytes: ByteArray,
    val version: String,
    val project: String,
) {
    val parsedVersion: FwVersion? get() = FwVersion.parse(version)

    val sha256Hex: String by lazy {
        MessageDigest.getInstance("SHA-256").digest(bytes)
            .joinToString("") { "%02x".format(it) }
    }

    companion object {
        private const val IMAGE_MAGIC = 0xE9
        private const val DESC_OFFSET = 32
        private const val DESC_MAGIC = 0xABCD5432L

        /** Returns the image, or throws with a sentence saying what is wrong with it. */
        fun parse(bytes: ByteArray, target: FirmwareTarget): FirmwareImage {
            require(bytes.size > DESC_OFFSET + 112) { "too short to be a firmware image" }
            require((bytes[0].toInt() and 0xFF) == IMAGE_MAGIC) {
                "not an ESP32 app image (a merged or bootloader .bin will not work here)"
            }
            val chip = u16(bytes, 12)
            require(chip == target.chipId) {
                "built for chip id $chip, not the ${target.label}'s ${target.chipName}"
            }
            require(u32(bytes, DESC_OFFSET) == DESC_MAGIC) { "no app descriptor in the image" }
            val version = cstr(bytes, DESC_OFFSET + 16, 32)
            val project = cstr(bytes, DESC_OFFSET + 48, 32)
            require(project == target.project) {
                "image is '$project', not ${target.project} -- the ${target.label} needs ${target.asset}"
            }
            return FirmwareImage(bytes, version, project)
        }

        private fun u16(b: ByteArray, o: Int) =
            (b[o].toInt() and 0xFF) or ((b[o + 1].toInt() and 0xFF) shl 8)

        private fun u32(b: ByteArray, o: Int): Long =
            (u16(b, o).toLong()) or (u16(b, o + 2).toLong() shl 16)

        private fun cstr(b: ByteArray, o: Int, n: Int): String {
            val end = (o until o + n).firstOrNull { b[it] == 0.toByte() } ?: (o + n)
            return String(b, o, end - o, Charsets.US_ASCII)
        }
    }
}

/** A published firmware release: where it is and what it claims to be. */
data class Release(
    val tag: String,
    val name: String,
    val assetUrl: String,
    val size: Long,
    val target: FirmwareTarget,
) {
    val version: FwVersion? get() = FwVersion.parse(tag.removePrefix(target.tagPrefix))
}

/**
 * Releases live on GitHub, as README.md "Versioning" describes, in two series: the
 * monitor's `vX.Y.Z` with `bat-monitor.bin`, and the remote display's `remote-vX.Y.Z`
 * with `batmon-remote.bin`. Only monitor releases are ever GitHub's "latest", so this
 * lists recent releases and takes the newest of the wanted series rather than asking
 * for "latest". Blocking calls -- run them off the main thread.
 */
object FirmwareReleases {
    const val REPO = "vitska/power-mon"

    /** The newest published release of `target`'s series that has its image, or null. */
    fun latest(target: FirmwareTarget): Release? {
        val c = open("https://api.github.com/repos/$REPO/releases?per_page=30")
        c.setRequestProperty("Accept", "application/vnd.github+json")
        return try {
            if (c.responseCode != 200) {
                throw java.io.IOException("GitHub answered HTTP ${c.responseCode}")
            }
            val list = org.json.JSONArray(c.inputStream.bufferedReader().readText())
            (0 until list.length())
                .map { list.getJSONObject(it) }
                .filter { !it.optBoolean("draft") && !it.optBoolean("prerelease") }
                .mapNotNull { parse(it, target) }
                // The API sorts by creation date; the version number is what counts.
                .maxWithOrNull(compareBy(nullsFirst()) { it.version })
        } finally {
            c.disconnect()
        }
    }

    fun download(r: Release): ByteArray {
        val c = open(r.assetUrl)
        c.setRequestProperty("Accept", "application/octet-stream")
        return try {
            if (c.responseCode != 200) throw java.io.IOException("download: HTTP ${c.responseCode}")
            c.inputStream.readBytes()
        } finally {
            c.disconnect()
        }
    }

    private fun parse(j: JSONObject, target: FirmwareTarget): Release? {
        val tag = j.optString("tag_name")
        // "v1.2.3" is a monitor tag and "remote-v1.2.3" a remote one; requiring a digit
        // straight after the prefix keeps the two series from matching each other.
        if (!tag.startsWith(target.tagPrefix) ||
            tag.getOrNull(target.tagPrefix.length)?.isDigit() != true
        ) return null
        val assets = j.optJSONArray("assets") ?: return null
        for (i in 0 until assets.length()) {
            val a = assets.getJSONObject(i)
            if (a.optString("name") == target.asset) {
                return Release(
                    tag = tag,
                    name = j.optString("name"),
                    assetUrl = a.getString("browser_download_url"),
                    size = a.optLong("size"),
                    target = target,
                )
            }
        }
        return null
    }

    private fun open(url: String): HttpURLConnection =
        (URL(url).openConnection() as HttpURLConnection).apply {
            connectTimeout = 10_000
            readTimeout = 30_000
            instanceFollowRedirects = true // release assets redirect to a CDN
        }
}

/** What the device's `ota status` said. Keys as CLI.md §6 lists them. */
data class OtaStatus(val raw: Map<String, String>) {
    val version: String? get() = raw["version"]
    val running: String? get() = raw["running"]
    val onProbation: Boolean get() = raw["state"] == "probation"
    val probationSeconds: Int? get() = raw["probation_s"]?.toIntOrNull()
    val canRollback: Boolean get() = raw["rollback"] == "1"
    val spareVersion: String? get() = raw["spare.version"]?.takeIf { it != "none" }

    companion object {
        fun parse(lines: List<String>) = OtaStatus(ConfigState.parse(lines).raw)
    }
}

/** The device's refusals of a firmware chunk, as ATT codes. CLI.md §6. */
object OtaWriteError {
    fun describe(status: Int): String = when (status) {
        0x80 -> "the device has no update in progress"
        0x81 -> "a chunk went missing or arrived twice"
        0x82 -> "more data than announced"
        0x83 -> "the device was busy"
        0x84 -> "the device could not write it to flash"
        0x05, 0x0F -> "the link is not paired, and the device requires it (ble pair bonded)"
        GATT_LINK_LOST -> "the link dropped"
        GATT_TIMEOUT -> "the device stopped acknowledging"
        GATT_NOT_STARTED -> "Android refused to send the write"
        else -> "GATT status $status"
    }
}
