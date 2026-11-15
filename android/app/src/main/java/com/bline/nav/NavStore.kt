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
        val next = parsed.copy(seq = seq.incrementAndGet())
        current = next
        listeners.forEach { it.onState(next) }
    }

    @Synchronized
    fun clear() {
        update(NavState(nav = false, seq = 0))
    }
}
