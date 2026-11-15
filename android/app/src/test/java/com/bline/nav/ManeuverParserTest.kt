package com.bline.nav

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ManeuverParserTest {
    @Test
    fun turnLeftWithDistance() {
        val s = ManeuverParser.parse(
            "240 m",
            "Turn left onto MG Road",
            "12 min · 4.2 km",
            null
        )
        assertTrue(s.nav)
        assertEquals("turn-left", s.maneuver)
        assertEquals("240 m", s.distText)
        assertEquals("MG Road", s.street)
        assertEquals("4.2 km", s.remain)
    }

    @Test
    fun slightRight() {
        val s = ManeuverParser.parse("80 m", "Keep right toward Downtown", null, null)
        assertEquals("slight-right", s.maneuver)
    }

    @Test
    fun uturn() {
        val s = ManeuverParser.parse("50 m", "Make a U-turn", null, null)
        assertEquals("uturn", s.maneuver)
    }

    @Test
    fun roundaboutExit() {
        val s = ManeuverParser.parse("120 m", "Take the 2nd exit", null, null)
        assertEquals("roundabout", s.maneuver)
        assertTrue(s.street.contains("2nd", ignoreCase = true))
    }

    @Test
    fun arrive() {
        val s = ManeuverParser.parse("0 m", "You have arrived", null, null)
        assertEquals("arrive", s.maneuver)
    }

    @Test
    fun doesNotTreatMinutesAsMeters() {
        val s = ManeuverParser.parse(
            "Turn left onto Foo St",
            "12 min · 4.2 km",
            null,
            null
        )
        assertEquals("turn-left", s.maneuver)
        assertEquals("4.2 km", s.distText)
    }
}
