package com.bline.nav

data class NavState(
    val nav: Boolean = false,
    val maneuver: String = "straight",
    val distText: String = "",
    val street: String = "",
    val eta: String = "",
    val remain: String = "",
    val seq: Long = 0
) {
    fun toJson(): String {
        val navVal = if (nav) "true" else "false"
        return buildString {
            append("{")
            append("\"nav\":").append(navVal)
            append(",\"maneuver\":").append(jsonString(maneuver))
            append(",\"dist_text\":").append(jsonString(distText))
            append(",\"street\":").append(jsonString(street))
            append(",\"eta\":").append(jsonString(eta))
            append(",\"remain\":").append(jsonString(remain))
            append(",\"seq\":").append(seq)
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
