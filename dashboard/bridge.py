from __future__ import annotations

import atexit
import asyncio
import json
import re
import threading
from collections import deque
from io import BytesIO
from pathlib import Path
from queue import Empty, Queue

import edge_tts
import serial
from flask import (
    Flask,
    Response,
    jsonify,
    request,
    send_file,
    send_from_directory,
    stream_with_context,
)
from serial.tools import list_ports


DASHBOARD_DIR = Path(__file__).resolve().parent
BAUD_RATE = 115200
app = Flask(__name__)
_tts_lock = threading.Lock()
_spanish_tts_voices: list[dict[str, str]] | None = None


def spanish_tts_voices() -> list[dict[str, str]]:
    global _spanish_tts_voices
    with _tts_lock:
        if _spanish_tts_voices is None:
            available = asyncio.run(edge_tts.list_voices())
            _spanish_tts_voices = [
                {
                    "id": voice["ShortName"],
                    "name": voice["FriendlyName"],
                    "locale": voice["Locale"],
                }
                for voice in available
                if voice["Locale"].lower().startswith("es")
            ]
        return list(_spanish_tts_voices)


async def synthesize_speech(text: str, voice: str) -> bytes:
    communicator = edge_tts.Communicate(text, voice)
    audio = bytearray()
    async for chunk in communicator.stream():
        if chunk["type"] == "audio":
            audio.extend(chunk["data"])
    if not audio:
        raise RuntimeError("El servicio de voz no devolvio audio.")
    return bytes(audio)


class SerialBridge:
    def __init__(self) -> None:
        self._lock = threading.RLock()
        self._connection: serial.Serial | None = None
        self._port: str | None = None
        self._reader_thread: threading.Thread | None = None
        self._stop = threading.Event()
        self._subscribers: set[Queue[dict[str, object]]] = set()
        self._history: deque[dict[str, object]] = deque(maxlen=400)

    def state(self) -> dict[str, str | bool | None]:
        with self._lock:
            return {"connected": self._connection is not None, "port": self._port}

    def connect(self, device: str) -> dict[str, str | bool | None]:
        with self._lock:
            if self._connection is not None:
                if self._port == device:
                    return self.state()
                raise RuntimeError(f"Ya hay una conexion activa en {self._port}.")

            connection = serial.Serial(
                port=device,
                baudrate=BAUD_RATE,
                timeout=0.2,
                write_timeout=2,
            )
            self._connection = connection
            self._port = device
            self._stop.clear()
            self._reader_thread = threading.Thread(
                target=self._read_serial,
                args=(connection,),
                daemon=True,
            )
            self._reader_thread.start()

        self._publish({"type": "state", "connected": True, "port": device})
        return self.state()

    def disconnect(self) -> dict[str, str | bool | None]:
        with self._lock:
            connection = self._connection
            device = self._port
            self._connection = None
            self._port = None
            self._stop.set()
            if connection is not None:
                connection.close()

        if device is not None:
            self._publish({"type": "state", "connected": False, "port": device})
        return self.state()

    def send(self, command: str) -> None:
        with self._lock:
            if self._connection is None or not self._connection.is_open:
                raise RuntimeError("No hay un puerto serie conectado.")
            self._connection.write(command.encode("ascii"))
            self._connection.flush()

    def subscribe(self) -> tuple[Queue[dict[str, object]], list[dict[str, object]]]:
        subscriber: Queue[dict[str, object]] = Queue(maxsize=256)
        with self._lock:
            history = list(self._history)
            self._subscribers.add(subscriber)
        return subscriber, history

    def unsubscribe(self, subscriber: Queue[dict[str, object]]) -> None:
        with self._lock:
            self._subscribers.discard(subscriber)

    def _publish(self, event: dict[str, object]) -> None:
        with self._lock:
            self._history.append(event)
            subscribers = tuple(self._subscribers)

        for subscriber in subscribers:
            if subscriber.full():
                try:
                    subscriber.get_nowait()
                except Empty:
                    pass
            try:
                subscriber.put_nowait(event)
            except Exception:
                pass

    def _read_serial(self, connection: serial.Serial) -> None:
        while not self._stop.is_set():
            try:
                raw_line = connection.readline()
            except serial.SerialException as error:
                if not self._stop.is_set():
                    self._publish({"type": "error", "message": str(error)})
                    self.disconnect()
                return

            if raw_line:
                message = raw_line.decode("utf-8", errors="replace").strip()
                if message:
                    self._publish({"type": "serial", "message": message})


bridge = SerialBridge()
atexit.register(bridge.disconnect)


@app.get("/")
def index() -> Response:
    return send_from_directory(DASHBOARD_DIR, "index.html")


@app.get("/api/ports")
def ports() -> Response:
    return jsonify(
        [
            {"device": item.device, "description": item.description}
            for item in list_ports.comports()
        ]
    )


@app.get("/api/status")
def status() -> Response:
    return jsonify(bridge.state())


@app.get("/api/voices")
def voices() -> tuple[Response, int] | Response:
    try:
        return jsonify(spanish_tts_voices())
    except Exception:
        app.logger.exception("No se pudieron cargar las voces online.")
        return (
            jsonify(
                {
                    "error": "No se pudieron cargar las voces. Comprueba la conexion a Internet del ordenador."
                }
            ),
            502,
        )


@app.post("/api/speech")
def speech() -> tuple[Response, int] | Response:
    body = request.get_json(silent=True)
    if not isinstance(body, dict):
        return jsonify({"error": "La solicitud de voz no es valida."}), 400
    text = body.get("text")
    voice = body.get("voice")
    if not isinstance(text, str) or not text.strip() or len(text) > 500:
        return jsonify({"error": "El texto debe tener entre 1 y 500 caracteres."}), 400
    if not isinstance(voice, str):
        return jsonify({"error": "Selecciona una voz en español."}), 400

    try:
        allowed_voices = {item["id"] for item in spanish_tts_voices()}
        if voice not in allowed_voices:
            return jsonify({"error": "La voz seleccionada ya no esta disponible. Recarga la pagina."}), 400
        audio = asyncio.run(synthesize_speech(text.strip(), voice))
        return send_file(BytesIO(audio), mimetype="audio/mpeg", download_name="aviso.mp3")
    except Exception:
        app.logger.exception("No se pudo generar el audio de voz.")
        return (
            jsonify(
                {
                    "error": "No se pudo generar la voz online. Comprueba la conexion a Internet e intenta de nuevo."
                }
            ),
            502,
        )


@app.post("/api/connect")
def connect() -> tuple[Response, int] | Response:
    device = str((request.get_json(silent=True) or {}).get("port", ""))
    available_ports = {item.device for item in list_ports.comports()}
    if device not in available_ports:
        return jsonify({"error": "El puerto seleccionado no esta disponible."}), 400
    try:
        return jsonify(bridge.connect(device))
    except (serial.SerialException, OSError, RuntimeError) as error:
        return jsonify({"error": str(error)}), 409


@app.post("/api/disconnect")
def disconnect() -> Response:
    return jsonify(bridge.disconnect())


@app.post("/api/send")
def send() -> tuple[Response, int] | Response:
    command = str((request.get_json(silent=True) or {}).get("data", ""))
    enrollment = re.fullmatch(r"e(\d{1,3})\n", command)
    if command not in ("l\n", "i\n") and (
        enrollment is None or not 1 <= int(enrollment.group(1)) <= 127
    ):
        return jsonify({"error": "Comando no permitido."}), 400
    try:
        bridge.send(command)
        return jsonify({"sent": True})
    except (serial.SerialException, OSError, RuntimeError) as error:
        return jsonify({"error": str(error)}), 409


@app.get("/api/events")
def events() -> Response:
    subscriber, history = bridge.subscribe()

    def generate():
        try:
            for event in history:
                yield f"data: {json.dumps(event, ensure_ascii=False)}\n\n"
            while True:
                try:
                    event = subscriber.get(timeout=15)
                    yield f"data: {json.dumps(event, ensure_ascii=False)}\n\n"
                except Empty:
                    yield ": keep-alive\n\n"
        finally:
            bridge.unsubscribe(subscriber)

    return Response(
        stream_with_context(generate()),
        mimetype="text/event-stream",
        headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"},
    )


if __name__ == "__main__":
    print("Consola R503 disponible en http://localhost:8000")
    print("En el movil, sustituye localhost por la IP local de este PC.")
    app.run(host="0.0.0.0", port=8000, threaded=True, use_reloader=False)