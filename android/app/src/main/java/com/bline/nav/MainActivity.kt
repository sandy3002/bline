package com.bline.nav

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.PowerManager
import android.provider.Settings
import android.widget.Button
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import java.util.Locale

class MainActivity : AppCompatActivity() {
    private lateinit var statusText: TextView
    private lateinit var stateText: TextView
    private lateinit var toggleBtn: Button

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { permissions ->
        val fineGranted = permissions[Manifest.permission.ACCESS_FINE_LOCATION] == true
        val coarseGranted = permissions[Manifest.permission.ACCESS_COARSE_LOCATION] == true

        if (fineGranted || coarseGranted) {
            startNavService()
        } else {
            Toast.makeText(this, "Location permission is required for navigation telemetry", Toast.LENGTH_LONG).show()
        }
        refreshStatus()
    }

    private val listener = NavStore.Listener { state ->
        runOnUiThread {
            stateText.text = state.toJson()
            refreshStatus()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        statusText = findViewById(R.id.statusText)
        stateText = findViewById(R.id.stateText)
        toggleBtn = findViewById(R.id.btnToggle)

        findViewById<Button>(R.id.btnNotifAccess).setOnClickListener {
            startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
        }
        findViewById<Button>(R.id.btnBattery).setOnClickListener {
            requestBatteryExemption()
        }
        toggleBtn.setOnClickListener { toggleService() }
    }

    override fun onResume() {
        super.onResume()
        NavStore.addListener(listener)
        refreshStatus()
    }

    override fun onPause() {
        NavStore.removeListener(listener)
        super.onPause()
    }

    private fun toggleService() {
        if (NavForegroundService.running) {
            NavForegroundService.stop(this)
            toggleBtn.postDelayed({ refreshStatus() }, 300)
        } else {
            checkPermissionsAndStart()
        }
    }

    private fun checkPermissionsAndStart() {
        val needed = mutableListOf<String>()
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION) != PackageManager.PERMISSION_GRANTED) {
            needed.add(Manifest.permission.ACCESS_FINE_LOCATION)
        }
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_COARSE_LOCATION) != PackageManager.PERMISSION_GRANTED) {
            needed.add(Manifest.permission.ACCESS_COARSE_LOCATION)
        }
        if (Build.VERSION.SDK_INT >= 33 &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            needed.add(Manifest.permission.POST_NOTIFICATIONS)
        }

        if (needed.isEmpty()) {
            startNavService()
        } else {
            permissionLauncher.launch(needed.toTypedArray())
        }
    }

    private fun startNavService() {
        NavForegroundService.start(this)
        toggleBtn.postDelayed({ refreshStatus() }, 300)
    }

    private fun refreshStatus() {
        val notif = hasNotificationAccess()
        val batt = isIgnoringBattery()
        val svc = NavForegroundService.running
        val hasLoc = ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED
        val state = NavStore.current

        statusText.text = buildString {
            append(if (svc) "Service running on :8080" else "Service stopped")
            append("\nNotification access: ").append(if (notif) "granted" else "missing")
            append("\nLocation permission: ").append(if (hasLoc) "granted" else "missing")
            append("\nBattery optimization: ").append(if (batt) "ignored" else "active")
            append("\nSpeed: ").append(String.format(Locale.US, "%.1f km/h", state.speedKmh))
            append(" | Bearing: ").append(String.format(Locale.US, "%.1f°", state.bearing))
            append("\nTrip: ").append(String.format(Locale.US, "%.1f km", state.tripDistKm))
            append(" | Map: ").append(if (state.mapAvailable) "Ready" else "No frame")
        }
        toggleBtn.text = if (svc) "Stop service" else "Start service"
    }

    private fun hasNotificationAccess(): Boolean {
        val enabled = NotificationManagerCompat.getEnabledListenerPackages(this)
        return enabled.contains(packageName)
    }

    private fun isIgnoringBattery(): Boolean {
        val pm = getSystemService(PowerManager::class.java)
        return pm.isIgnoringBatteryOptimizations(packageName)
    }

    private fun requestBatteryExemption() {
        val intent = Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS).apply {
            data = Uri.parse("package:$packageName")
        }
        try {
            startActivity(intent)
        } catch (_: Exception) {
            startActivity(Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS))
        }
    }
}
