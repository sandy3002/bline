package com.bline.nav

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Matrix
import android.graphics.Paint
import android.util.Log
import okhttp3.Cache
import okhttp3.OkHttpClient
import okhttp3.Request
import java.io.ByteArrayOutputStream
import java.io.File
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference
import kotlin.math.PI
import kotlin.math.floor
import kotlin.math.ln
import kotlin.math.tan

class MapTileCompositor(private val context: Context) {
    private val client: OkHttpClient
    private val executor = Executors.newSingleThreadExecutor { r ->
        Thread(r, "bline-tile-compositor").apply { isDaemon = true }
    }

    private val latestJpeg = AtomicReference<ByteArray?>(null)
    private val isRendering = AtomicBoolean(false)

    // Reusable bitmaps & canvases to prevent GC churn
    private val intermediateWidth = 480
    private val intermediateHeight = 480
    private val riderCanvasX = 240f
    private val riderCanvasY = 280f

    private val intermediateBitmap = Bitmap.createBitmap(intermediateWidth, intermediateHeight, Bitmap.Config.RGB_565)
    private val intermediateCanvas = Canvas(intermediateBitmap)

    private val outputBitmap = Bitmap.createBitmap(240, 240, Bitmap.Config.RGB_565)
    private val outputCanvas = Canvas(outputBitmap)

    private val paint = Paint(Paint.FILTER_BITMAP_FLAG or Paint.ANTI_ALIAS_FLAG)
    private val bgPaint = Paint().apply {
        color = Color.rgb(25, 26, 26) // MapTiler dark base tone
        style = Paint.Style.FILL
    }

    private val transformMatrix = Matrix()

    @Volatile
    private var pendingLat: Double? = null
    @Volatile
    private var pendingLon: Double? = null
    @Volatile
    private var pendingSpeedKmh: Float = 0f
    @Volatile
    private var pendingBearing: Float = 0f

    init {
        val cacheDir = File(context.cacheDir, "tiles")
        if (!cacheDir.exists()) {
            cacheDir.mkdirs()
        }
        val cache = Cache(cacheDir, 250L * 1024 * 1024) // 250 MB
        client = OkHttpClient.Builder()
            .cache(cache)
            .connectTimeout(3, TimeUnit.SECONDS)
            .readTimeout(3, TimeUnit.SECONDS)
            .build()
    }

    fun getLatestJpeg(): ByteArray? = latestJpeg.get()

    fun updatePosition(lat: Double, lon: Double, speedKmh: Float, latchedBearing: Float) {
        pendingLat = lat
        pendingLon = lon
        pendingSpeedKmh = speedKmh
        pendingBearing = latchedBearing

        if (isRendering.compareAndSet(false, true)) {
            executor.execute {
                try {
                    drainRenderQueue()
                } finally {
                    isRendering.set(false)
                }
            }
        }
    }

    private fun drainRenderQueue() {
        while (true) {
            val lat = pendingLat ?: break
            val lon = pendingLon ?: break
            val speed = pendingSpeedKmh
            val bearing = pendingBearing

            // Clear pending so we know if a newer coordinate arrives during rendering
            pendingLat = null
            pendingLon = null

            renderFrame(lat, lon, speed, bearing)

            if (pendingLat == null) {
                break
            }
        }
    }

    private fun renderFrame(lat: Double, lon: Double, speedKmh: Float, latchedBearing: Float) {
        // Speed-dependent zoom mapping: Z16 <= 40 km/h, Z15 > 40 km/h
        val zoom = if (speedKmh <= 40f) 16 else 15

        // Slippy Mercator math
        val n = 1 shl zoom
        val latRad = Math.toRadians(lat)
        val exactTileX = (lon + 180.0) / 360.0 * n
        val exactTileY = (1.0 - ln(tan(latRad) + 1.0 / kotlin.math.cos(latRad)) / PI) / 2.0 * n

        val worldPixelX = exactTileX * 256.0
        val worldPixelY = exactTileY * 256.0

        val minWorldX = worldPixelX - riderCanvasX
        val maxWorldX = worldPixelX + (intermediateWidth - riderCanvasX)
        val minWorldY = worldPixelY - riderCanvasY
        val maxWorldY = worldPixelY + (intermediateHeight - riderCanvasY)

        val minTileX = floor(minWorldX / 256.0).toInt()
        val maxTileX = floor(maxWorldX / 256.0).toInt()
        val minTileY = floor(minWorldY / 256.0).toInt()
        val maxTileY = floor(maxWorldY / 256.0).toInt()

        // Clear intermediate canvas
        intermediateCanvas.drawRect(
            0f, 0f,
            intermediateWidth.toFloat(),
            intermediateHeight.toFloat(),
            bgPaint
        )

        var renderedAnyTile = false
        val apiKey = BuildConfig.MAPTILER_API_KEY

        for (tx in minTileX..maxTileX) {
            for (ty in minTileY..maxTileY) {
                val tileBitmap = fetchTile(zoom, tx, ty, apiKey)
                if (tileBitmap != null) {
                    val drawX = (tx * 256.0 - minWorldX).toFloat()
                    val drawY = (ty * 256.0 - minWorldY).toFloat()
                    intermediateCanvas.drawBitmap(tileBitmap, drawX, drawY, paint)
                    tileBitmap.recycle()
                    renderedAnyTile = true
                }
            }
        }

        // Affine rotation: rotate around rider coordinates and place rider at (120, 160)
        transformMatrix.reset()
        transformMatrix.postTranslate(-riderCanvasX, -riderCanvasY)
        transformMatrix.postRotate(-latchedBearing)
        transformMatrix.postTranslate(120f, 160f)

        outputCanvas.drawRect(0f, 0f, 240f, 240f, bgPaint)
        outputCanvas.drawBitmap(intermediateBitmap, transformMatrix, paint)

        val stream = ByteArrayOutputStream()
        outputBitmap.compress(Bitmap.CompressFormat.JPEG, 75, stream)
        val jpegBytes = stream.toByteArray()

        latestJpeg.set(jpegBytes)
        NavStore.updateMapAvailable(renderedAnyTile)
    }

    private fun fetchTile(zoom: Int, x: Int, y: Int, apiKey: String): Bitmap? {
        val maxTile = (1 shl zoom) - 1
        if (x < 0 || x > maxTile || y < 0 || y > maxTile) return null

        val url = "https://api.maptiler.com/maps/basic-v2-dark/$zoom/$x/$y.png?key=$apiKey"
        val request = Request.Builder()
            .url(url)
            .header("User-Agent", "BlineNav/1.0")
            .build()

        return try {
            client.newCall(request).execute().use { response ->
                if (!response.isSuccessful) {
                    Log.w(TAG, "Tile $zoom/$x/$y HTTP ${response.code}")
                    return null
                }
                val body = response.body ?: return null
                val bytes = body.bytes()
                BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
            }
        } catch (e: Exception) {
            Log.w(TAG, "Failed to load tile $zoom/$x/$y: ${e.message}")
            null
        }
    }

    fun shutdown() {
        executor.shutdownNow()
    }

    companion object {
        private const val TAG = "MapTileCompositor"
    }
}
