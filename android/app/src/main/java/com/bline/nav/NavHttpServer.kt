package com.bline.nav

import android.util.Log
import java.io.BufferedReader
import java.io.InputStreamReader
import java.net.ServerSocket
import java.net.Socket
import java.util.Locale
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread

class NavHttpServer(
    private val port: Int = 8080,
    private val compositor: MapTileCompositor? = null
) {
    private val running = AtomicBoolean(false)
    private var server: ServerSocket? = null
    private val workerPool = Executors.newFixedThreadPool(3) { r ->
        Thread(r, "bline-http-worker").apply { isDaemon = true }
    }

    fun start() {
        if (!running.compareAndSet(false, true)) return
        thread(name = "bline-http-listener", isDaemon = true) {
            try {
                server = ServerSocket(port)
                Log.i(TAG, "listening on :$port")
                while (running.get()) {
                    val socket = try {
                        server?.accept()
                    } catch (_: Exception) {
                        null
                    } ?: continue

                    if (running.get()) {
                        workerPool.execute {
                            handle(socket)
                        }
                    } else {
                        try { socket.close() } catch (_: Exception) {}
                    }
                }
            } catch (e: Exception) {
                if (running.get()) {
                    Log.e(TAG, "server failed", e)
                }
            }
        }
    }

    fun stop() {
        running.set(false)
        try {
            server?.close()
        } catch (_: Exception) {}
        server = null
        workerPool.shutdownNow()
    }

    private fun handle(socket: Socket) {
        socket.use { sock ->
            sock.soTimeout = 2000
            val reader = BufferedReader(InputStreamReader(sock.getInputStream(), Charsets.UTF_8))
            val requestLine = reader.readLine() ?: return
            while (true) {
                val line = reader.readLine() ?: break
                if (line.isEmpty()) break
            }

            val path = requestLine.split(" ").getOrNull(1) ?: "/"
            val isGet = requestLine.startsWith("GET ")

            if (isGet && (path == "/map" || path.startsWith("/map?"))) {
                handleMapRequest(sock)
            } else if (isGet && (path == "/nav" || path.startsWith("/nav?"))) {
                handleNavRequest(sock)
            } else {
                handleNotFound(sock)
            }
        }
    }

    private fun sanitizeHeader(value: String): String {
        return value.replace("\r", " ").replace("\n", " ").trim()
    }

    private fun handleMapRequest(sock: Socket) {
        val jpegBytes = compositor?.getLatestJpeg()
        val out = sock.getOutputStream()
        if (jpegBytes != null && jpegBytes.isNotEmpty()) {
            val state = NavStore.current
            val headers = buildString {
                append("HTTP/1.1 200 OK\r\n")
                append("Content-Type: image/jpeg\r\n")
                append("Content-Length: ").append(jpegBytes.size).append("\r\n")
                append("Connection: close\r\n")
                append("Access-Control-Allow-Origin: *\r\n")
                append("X-Nav-Dist: ").append(sanitizeHeader(state.distText)).append("\r\n")
                append("X-Nav-Maneuver: ").append(sanitizeHeader(state.maneuver)).append("\r\n")
                append("X-Nav-Speed: ").append(String.format(Locale.US, "%.1f", state.speedKmh)).append("\r\n")
                append("X-Nav-Street: ").append(sanitizeHeader(state.street)).append("\r\n")
                append("X-Nav-Eta: ").append(sanitizeHeader(state.eta)).append("\r\n")
                append("\r\n")
            }.toByteArray(Charsets.UTF_8)

            out.write(headers)
            out.write(jpegBytes)
            out.flush()
        } else {
            val response = buildString {
                append("HTTP/1.1 503 Service Unavailable\r\n")
                append("Content-Type: text/plain\r\n")
                append("Content-Length: 0\r\n")
                append("Connection: close\r\n")
                append("Access-Control-Allow-Origin: *\r\n")
                append("\r\n")
            }.toByteArray(Charsets.UTF_8)
            out.write(response)
            out.flush()
        }
    }

    private fun handleNavRequest(sock: Socket) {
        val body = NavStore.current.toJson()
        val bytes = body.toByteArray(Charsets.UTF_8)
        val response = buildString {
            append("HTTP/1.1 200 OK\r\n")
            append("Content-Type: application/json; charset=utf-8\r\n")
            append("Content-Length: ").append(bytes.size).append("\r\n")
            append("Connection: close\r\n")
            append("Access-Control-Allow-Origin: *\r\n")
            append("\r\n")
        }.toByteArray(Charsets.UTF_8)
        val out = sock.getOutputStream()
        out.write(response)
        out.write(bytes)
        out.flush()
    }

    private fun handleNotFound(sock: Socket) {
        val body = "Not Found"
        val bytes = body.toByteArray(Charsets.UTF_8)
        val response = buildString {
            append("HTTP/1.1 404 Not Found\r\n")
            append("Content-Type: text/plain; charset=utf-8\r\n")
            append("Content-Length: ").append(bytes.size).append("\r\n")
            append("Connection: close\r\n")
            append("\r\n")
        }.toByteArray(Charsets.UTF_8)
        val out = sock.getOutputStream()
        out.write(response)
        out.write(bytes)
        out.flush()
    }

    companion object {
        private const val TAG = "NavHttpServer"
    }
}
