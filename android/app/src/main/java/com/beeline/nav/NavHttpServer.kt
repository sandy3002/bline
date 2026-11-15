package com.beeline.nav

import android.util.Log
import java.io.BufferedReader
import java.io.InputStreamReader
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread

class NavHttpServer(private val port: Int = 8080) {
    private val running = AtomicBoolean(false)
    private var server: ServerSocket? = null

    fun start() {
        if (!running.compareAndSet(false, true)) return
        thread(name = "beeline-http", isDaemon = true) {
            try {
                server = ServerSocket(port)
                Log.i(TAG, "listening on :$port")
                while (running.get()) {
                    val socket = try {
                        server?.accept()
                    } catch (_: Exception) {
                        null
                    } ?: continue
                    thread(name = "beeline-http-client", isDaemon = true) {
                        handle(socket)
                    }
                }
            } catch (e: Exception) {
                Log.e(TAG, "server failed", e)
            }
        }
    }

    fun stop() {
        running.set(false)
        try {
            server?.close()
        } catch (_: Exception) {
        }
        server = null
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
            val ok = requestLine.startsWith("GET ") && (path == "/nav" || path.startsWith("/nav?"))
            val body = if (ok) NavStore.current.toJson() else "not found"
            val status = if (ok) "200 OK" else "404 Not Found"
            val type = if (ok) "application/json" else "text/plain"
            val bytes = body.toByteArray(Charsets.UTF_8)
            val response = buildString {
                append("HTTP/1.1 ").append(status).append("\r\n")
                append("Content-Type: ").append(type).append("; charset=utf-8\r\n")
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
    }

    companion object {
        private const val TAG = "NavHttpServer"
    }
}
