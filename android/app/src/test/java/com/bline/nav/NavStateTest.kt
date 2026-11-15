package com.bline.nav

import org.junit.Assert.assertTrue
import org.junit.Test

class NavStateTest {
    @Test
    fun testJsonSerialization() {
        val state = NavState(
            nav = true,
            maneuver = "turn-right",
            distText = "250 m",
            street = "MG Road",
            eta = "12:34",
            remain = "4.2 km",
            seq = 187,
            speedKmh = 68.5f,
            bearing = 182.4f,
            tripDistKm = 14.2f,
            elapsedS = 1540L,
            mapAvailable = true
        )
        val jsonStr = state.toJson()
        assertTrue(jsonStr.contains("\"nav\":true"))
        assertTrue(jsonStr.contains("\"maneuver\":\"turn-right\""))
        assertTrue(jsonStr.contains("\"dist_text\":\"250 m\""))
        assertTrue(jsonStr.contains("\"street\":\"MG Road\""))
        assertTrue(jsonStr.contains("\"eta\":\"12:34\""))
        assertTrue(jsonStr.contains("\"remain\":\"4.2 km\""))
        assertTrue(jsonStr.contains("\"seq\":187"))
        assertTrue(jsonStr.contains("\"speed_kmh\":68.5"))
        assertTrue(jsonStr.contains("\"bearing\":182.4"))
        assertTrue(jsonStr.contains("\"trip_dist_km\":14.2"))
        assertTrue(jsonStr.contains("\"elapsed_s\":1540"))
        assertTrue(jsonStr.contains("\"map_available\":true"))
    }
}
