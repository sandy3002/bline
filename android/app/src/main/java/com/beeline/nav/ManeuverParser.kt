package com.beeline.nav

object ManeuverParser {
    private val DIST_RE = Regex(
        """(\d+(?:[.,]\d+)?)\s*(kilometers?|km|miles?|mi|meters?|m|feet|ft|yards?|yd)\b""",
        RegexOption.IGNORE_CASE
    )
    private val ETA_CLOCK_RE = Regex("""\b(\d{1,2}:\d{2}\s*(?:AM|PM)?)\b""", RegexOption.IGNORE_CASE)
    private val ETA_MIN_RE = Regex("""\b(\d+)\s*min(?:ute)?s?\b""", RegexOption.IGNORE_CASE)
    private val EXIT_RE = Regex("""\b(?:take\s+the\s+)?(\d+(?:st|nd|rd|th)|first|second|third|fourth)\s+exit\b""", RegexOption.IGNORE_CASE)
    private val ONTO_RE = Regex("""\b(?:onto|on to|toward|towards|on)\s+(.+)$""", RegexOption.IGNORE_CASE)

    fun parse(title: String?, text: String?, subText: String?, bigText: String?): NavState {
        val parts = listOfNotNull(title, text, subText, bigText)
            .map { it.trim() }
            .filter { it.isNotEmpty() }
        if (parts.isEmpty()) return NavState(nav = false)

        val hay = parts.joinToString(" \n ")
        val lower = hay.lowercase()

        if (!looksLikeNavigation(lower)) {
            return NavState(nav = false)
        }

        val maneuver = detectManeuver(lower)
        val distances = DIST_RE.findAll(hay).map { it.value.replace(",", ".") }.toList()
        val distText = distances.firstOrNull().orEmpty().ifBlank {
            title?.takeIf { DIST_RE.containsMatchIn(it) } ?: ""
        }
        val remain = when {
            distances.size >= 2 -> distances.last()
            else -> ""
        }
        val eta = ETA_CLOCK_RE.find(hay)?.groupValues?.get(1)
            ?: ETA_MIN_RE.find(hay)?.value.orEmpty()
        val street = extractStreet(text ?: bigText ?: title ?: "", maneuver, hay)

        return NavState(
            nav = true,
            maneuver = maneuver,
            distText = distText,
            street = street,
            eta = eta,
            remain = remain
        )
    }

    private fun looksLikeNavigation(lower: String): Boolean {
        val hints = listOf(
            "turn ", " onto", "continue", "destination", "exit",
            "roundabout", "u-turn", "uturn", "keep left", "keep right",
            "head ", "merge", "arrive", "navigat", "slight", "sharp"
        )
        if (hints.any { lower.contains(it) }) return true
        return DIST_RE.containsMatchIn(lower) &&
            (lower.contains("min") || lower.contains("km") || lower.contains("arrive"))
    }

    private fun detectManeuver(lower: String): String {
        when {
            containsAny(lower, "u-turn", "uturn", "u turn", "make a u") -> return "uturn"
            containsAny(lower, "sharp right") -> return "sharp-right"
            containsAny(lower, "sharp left") -> return "sharp-left"
            containsAny(lower, "slight right", "keep right", "bear right", "veer right") -> return "slight-right"
            containsAny(lower, "slight left", "keep left", "bear left", "veer left") -> return "slight-left"
            containsAny(lower, "roundabout", "rotary", "traffic circle") || EXIT_RE.containsMatchIn(lower) -> return "roundabout"
            containsAny(lower, "you have arrived", "you've arrived", "arrive at destination", "destination on the") -> return "arrive"
            containsAny(lower, "turn right", "right onto", "right turn", "right toward", "right towards") -> return "turn-right"
            containsAny(lower, "turn left", "left onto", "left turn", "left toward", "left towards") -> return "turn-left"
            containsAny(lower, "continue", "straight", "head north", "head south", "head east", "head west", "merge") -> return "straight"
        }
        return "straight"
    }

    private fun extractStreet(instruction: String, maneuver: String, hay: String): String {
        if (maneuver == "roundabout") {
            val exit = EXIT_RE.find(hay)?.value
            if (!exit.isNullOrBlank()) return exit.replaceFirstChar { it.uppercase() }
        }
        val onto = ONTO_RE.find(instruction.trim())
        if (onto != null) {
            return onto.groupValues[1]
                .replace(DIST_RE, "")
                .replace(ETA_CLOCK_RE, "")
                .trim(' ', ',', '.', '-', '·')
        }
        val cleaned = instruction
            .replace(DIST_RE, "")
            .replace(ETA_CLOCK_RE, "")
            .trim(' ', ',', '.', '-', '·')
        return cleaned.take(48)
    }

    private fun containsAny(hay: String, vararg needles: String): Boolean {
        return needles.any { hay.contains(it) }
    }
}
