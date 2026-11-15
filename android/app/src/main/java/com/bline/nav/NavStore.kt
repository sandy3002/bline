package com.bline.nav

import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.atomic.AtomicLong

object NavStore {
    fun interface Listener {
        fun onState(state: NavState)
    }

    private val seq = AtomicLong(0)
    private val listeners = CopyOnWriteArrayList<Listener>()

    @Volatile
    var current: NavState = NavState()
        private set

    fun addListener(listener: Listener) {
        listeners.add(listener)
        listener.onState(current)
    }

    fun removeListener(listener: Listener) {
        listeners.remove(listener)
    }

    @Synchronized
    fun update(parsed: NavState) {
        val next = parsed.copy(
            seq = seq.incrementAndGet(),
            speedKmh = current.speedKmh,
            bearing = current.bearing,
            tripDistKm = current.tripDistKm,
            elapsedS = current.elapsedS,
            mapAvailable = current.mapAvailable
        )
        current = next
        listeners.forEach { it.onState(next) }
    }

    @Synchronized
    fun updateLocation(speedKmh: Float, bearing: Float, tripDistKm: Float, elapsedS: Long) {
        val next = current.copy(
            speedKmh = speedKmh,
            bearing = bearing,
            tripDistKm = tripDistKm,
            elapsedS = elapsedS,
            seq = seq.incrementAndGet()
        )
        current = next
        listeners.forEach { it.onState(next) }
    }

    @Synchronized
    fun updateManeuver(navState: NavState) {
        val next = current.copy(
            nav = navState.nav,
            maneuver = navState.maneuver,
            distText = navState.distText,
            street = navState.street,
            eta = navState.eta,
            remain = navState.remain,
            seq = seq.incrementAndGet()
        )
        current = next
        listeners.forEach { it.onState(next) }
    }

    @Synchronized
    fun updateManeuver(
        maneuver: String,
        distText: String,
        street: String,
        eta: String,
        remain: String,
        nav: Boolean = true
    ) {
        val next = current.copy(
            nav = nav,
            maneuver = maneuver,
            distText = distText,
            street = street,
            eta = eta,
            remain = remain,
            seq = seq.incrementAndGet()
        )
        current = next
        listeners.forEach { it.onState(next) }
    }

    @Synchronized
    fun updateMapAvailable(available: Boolean) {
        if (current.mapAvailable == available) return
        val next = current.copy(
            mapAvailable = available,
            seq = seq.incrementAndGet()
        )
        current = next
        listeners.forEach { it.onState(next) }
    }

    @Synchronized
    fun clear() {
        val next = current.copy(
            nav = false,
            maneuver = "straight",
            distText = "",
            street = "",
            eta = "",
            remain = "",
            seq = seq.incrementAndGet()
        )
        current = next
        listeners.forEach { it.onState(next) }
    }
}
