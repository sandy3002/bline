package com.bline.nav

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.Build
import android.os.Bundle
import android.os.IBinder
import android.util.Log
import androidx.core.app.ActivityCompat
import androidx.core.app.NotificationCompat
import java.util.ArrayDeque

class NavForegroundService : Service() {
    private lateinit var compositor: MapTileCompositor
    private lateinit var http: NavHttpServer
    private var locationManager: LocationManager? = null

    private var startTimestampMs: Long = 0L
    private var totalTripMeters: Float = 0f
    private var lastLocation: Location? = null

    // Doppler bearing latch state
    private var latchedBearing: Float = 0f
    private val lowSpeedPoints = ArrayDeque<Location>()

    private val locationListener = object : LocationListener {
        override fun onLocationChanged(location: Location) {
            handleLocationUpdate(location)
        }

        @Deprecated("Deprecated in Java")
        override fun onStatusChanged(provider: String?, status: Int, extras: Bundle?) {}
        override fun onProviderEnabled(provider: String) {}
        override fun onProviderDisabled(provider: String) {}
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        running = true
        startTimestampMs = System.currentTimeMillis()

        compositor = MapTileCompositor(applicationContext)
        http = NavHttpServer(8080, compositor)

        createChannel()
        val notification = buildNotification("Serving /nav and /map on port 8080")
        val serviceType = if (Build.VERSION.SDK_INT >= 34) {
            ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC or ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION
        } else {
            0
        }
        startForeground(NOTIF_ID, notification, serviceType)

        http.start()
        NavStore.addListener(listener)
        startLocationUpdates()
    }

    private fun startLocationUpdates() {
        locationManager = getSystemService(LocationManager::class.java)
        if (ActivityCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED) {
            try {
                locationManager?.requestLocationUpdates(
                    LocationManager.GPS_PROVIDER,
                    500L,
                    0f,
                    locationListener
                )
            } catch (e: Exception) {
                Log.e(TAG, "Failed to start GPS_PROVIDER updates", e)
            }
        } else {
            Log.w(TAG, "ACCESS_FINE_LOCATION not granted")
        }
    }

    private fun handleLocationUpdate(loc: Location) {
        val now = System.currentTimeMillis()
        val speedKmh = loc.speed * 3.6f

        // Trip distance calculation
        lastLocation?.let { prev ->
            val dist = prev.distanceTo(loc)
            if (dist > 0f) {
                totalTripMeters += dist
            }
        }
        lastLocation = loc

        // Doppler bearing latch filter
        val speedMps = loc.speed
        val minSpeedMps = 5.0f / 3.6f // 5 km/h threshold

        if (speedMps >= minSpeedMps && loc.hasBearing()) {
            latchedBearing = loc.bearing
            lowSpeedPoints.clear()
        } else {
            // Speed < 5 km/h: freeze latchedBearing, check 10-meter rolling displacement window
            lowSpeedPoints.addLast(Location(loc))

            // Prune points older than 10 seconds
            val cutoff = now - 10_000L
            while (lowSpeedPoints.isNotEmpty() && lowSpeedPoints.first.time < cutoff) {
                lowSpeedPoints.removeFirst()
            }

            // Check if displacement in rolling window exceeds 10 meters
            val oldest = lowSpeedPoints.peekFirst()
            if (oldest != null) {
                val disp = oldest.distanceTo(loc)
                if (disp >= 10.0f) {
                    var calculated = oldest.bearingTo(loc)
                    if (calculated < 0f) {
                        calculated += 360f
                    }
                    latchedBearing = calculated
                    lowSpeedPoints.clear()
                    lowSpeedPoints.addLast(Location(loc))
                }
            }
        }

        val elapsedS = (now - startTimestampMs) / 1000L
        val tripDistKm = totalTripMeters / 1000f

        NavStore.updateLocation(
            speedKmh = speedKmh,
            bearing = latchedBearing,
            tripDistKm = tripDistKm,
            elapsedS = elapsedS
        )

        compositor.updatePosition(
            lat = loc.latitude,
            lon = loc.longitude,
            speedKmh = speedKmh,
            latchedBearing = latchedBearing
        )
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            stopSelf()
            return START_NOT_STICKY
        }
        return START_STICKY
    }

    override fun onDestroy() {
        try {
            locationManager?.removeUpdates(locationListener)
        } catch (_: Exception) {}
        NavStore.removeListener(listener)
        http.stop()
        compositor.shutdown()
        running = false
        super.onDestroy()
    }

    private val listener = NavStore.Listener { state ->
        val text = if (state.nav) {
            "${state.maneuver}  ${state.distText}  ${state.street}".trim()
        } else {
            "Waiting for Google Maps navigation"
        }
        val nm = getSystemService(NotificationManager::class.java)
        nm.notify(NOTIF_ID, buildNotification(text))
    }

    private fun createChannel() {
        val nm = getSystemService(NotificationManager::class.java)
        val channel = NotificationChannel(
            CHANNEL_ID,
            getString(R.string.channel_name),
            NotificationManager.IMPORTANCE_LOW
        )
        channel.description = getString(R.string.channel_desc)
        nm.createNotificationChannel(channel)
    }

    private fun buildNotification(text: String): Notification {
        val launch = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_stat_nav)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(text)
            .setOngoing(true)
            .setContentIntent(launch)
            .setOnlyAlertOnce(true)
            .build()
    }

    companion object {
        const val CHANNEL_ID = "bline_nav"
        const val NOTIF_ID = 42
        const val ACTION_STOP = "com.bline.nav.STOP"
        private const val TAG = "NavForegroundService"

        @Volatile
        var running: Boolean = false
            private set

        fun start(context: Context) {
            val intent = Intent(context, NavForegroundService::class.java)
            context.startForegroundService(intent)
        }

        fun stop(context: Context) {
            val intent = Intent(context, NavForegroundService::class.java).setAction(ACTION_STOP)
            context.startService(intent)
        }
    }
}
