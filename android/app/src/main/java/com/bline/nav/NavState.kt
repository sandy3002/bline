package com.bline.nav

import java.util.Locale

data class NavState(
    val nav: Boolean = false,
    val maneuver: String = "straight",
    val distText: String = "",
    val street: String = "",
    val eta: String = "",
    val remain: String = "",
    val seq: Long = 0,
    val speedKmh: Float = 0f,
    val bearing: Float = 0f,
    val tripDistKm: Float = 0f,
    val elapsedS: Long = 0L,
    val mapAvailable: Boolean = false
) {
    fun toJson(): String {
        val navVal = if (nav) "true" else "false"
        val mapAvailVal = if (mapAvailable) "true" else "false"
        return buildString {
            append("{")
            append("\"nav\":").append(navVal)
            append(",\"maneuver\":").append(jsonString(maneuver))
            append(",\"dist_text\":").append(jsonString(distText))
            append(",\"street\":").append(jsonString(street))
            append(",\"eta\":").append(jsonString(eta))
            append(",\"remain\":").append(jsonString(remain))
            append(",\"seq\":").append(seq)
            append(",\"speed_kmh\":").append(String.format(Locale.US, "%.1f", speedKmh))
            append(",\"bearing\":").append(String.format(Locale.US, "%.1f", bearing))
            append(",\"trip_dist_km\":").append(String.format(Locale.US, "%.1f", tripDistKm))
            append(",\"elapsed_s\":").append(elapsedS)
            append(",\"map_available\":").append(mapAvailVal)
            append("}")
        }
    }

    private fun jsonString(value: String): String {
        val escaped = value
            .replace("\\", "\\\\")
            .replace("\"", "\\\"")
            .replace("\n", "\\n")
            .replace("\r", "\\r")
        return "\"$escaped\""
    }
}
