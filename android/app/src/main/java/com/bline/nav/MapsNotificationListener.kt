package com.bline.nav

import android.app.Notification
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification

class MapsNotificationListener : NotificationListenerService() {
    override fun onNotificationPosted(sbn: StatusBarNotification?) {
        if (sbn == null || sbn.packageName != MAPS_PACKAGE) return
        val parsed = parse(sbn.notification)
        val isNav = parsed.nav || sbn.notification.category == Notification.CATEGORY_NAVIGATION
        if (isNav) {
            NavStore.updateManeuver(if (parsed.nav) parsed else parsed.copy(nav = true))
        }
    }

    override fun onNotificationRemoved(sbn: StatusBarNotification?) {
        if (sbn == null || sbn.packageName != MAPS_PACKAGE) return
        val stillNavigating = activeNotifications?.any { n ->
            n.packageName == MAPS_PACKAGE &&
                n.notification.category == Notification.CATEGORY_NAVIGATION
        } == true
        if (!stillNavigating) NavStore.clear()
    }

    override fun onListenerConnected() {
        super.onListenerConnected()
        val maps = activeNotifications?.firstOrNull { n ->
            n.packageName == MAPS_PACKAGE &&
                (n.notification.category == Notification.CATEGORY_NAVIGATION)
        } ?: activeNotifications?.firstOrNull { it.packageName == MAPS_PACKAGE }
        if (maps != null) {
            val parsed = parse(maps.notification)
            if (parsed.nav || maps.notification.category == Notification.CATEGORY_NAVIGATION) {
                NavStore.updateManeuver(if (parsed.nav) parsed else parsed.copy(nav = true))
            }
        }
    }

    private fun parse(notification: Notification): NavState {
        val extras = notification.extras
        val title = extras.getCharSequence(Notification.EXTRA_TITLE)?.toString()
        val text = extras.getCharSequence(Notification.EXTRA_TEXT)?.toString()
        val sub = extras.getCharSequence(Notification.EXTRA_SUB_TEXT)?.toString()
        val big = extras.getCharSequence(Notification.EXTRA_BIG_TEXT)?.toString()
        val parsed = ManeuverParser.parse(title, text, sub, big)
        if (notification.category == Notification.CATEGORY_NAVIGATION && !parsed.nav) {
            return parsed.copy(nav = true)
        }
        return parsed
    }

    companion object {
        const val MAPS_PACKAGE = "com.google.android.apps.maps"
    }
}
