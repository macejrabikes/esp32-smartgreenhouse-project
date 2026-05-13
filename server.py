import json
import ssl
import threading
import os
from datetime import datetime, timezone
from typing import Optional
from contextlib import asynccontextmanager

import paho.mqtt.client as mqtt
from fastapi import FastAPI, HTTPException
from fastapi.responses import HTMLResponse
from fastapi.middleware.cors import CORSMiddleware
import uvicorn

# =====================================================
# KONFIGURÁCIA – číta sa zo Application Settings v Azure
# (Settings → Configuration → Application settings)
# =====================================================
MQTT_BROKER   = os.getenv("MQTT_BROKER",   "1d9c5feaaa414759afcb34e04d9dedb6.s1.eu.hivemq.cloud")
MQTT_PORT     = int(os.getenv("MQTT_PORT", "8883"))
MQTT_USER     = os.getenv("MQTT_USER",     "esp32")
MQTT_PASS     = os.getenv("MQTT_PASS",     "ESP32heslo123")
MQTT_CLIENT   = os.getenv("MQTT_CLIENT",   "SmartGreenhouse_Server_001")

TOPIC_SENSORS  = "smartgreenhouse/sensors"
TOPIC_STATUS   = "smartgreenhouse/status"
TOPIC_CMD_LED  = "smartgreenhouse/cmd/led"
TOPIC_CMD_PUMP = "smartgreenhouse/cmd/pump"
TOPIC_CMD_FAN  = "smartgreenhouse/cmd/fan"
TOPIC_CMD_AUTO = "smartgreenhouse/cmd/auto"

OFFLINE_TIMEOUT_S = 30

# =====================================================
# STAV SYSTÉMU
# =====================================================
import threading as _threading
_state_lock = _threading.Lock()

class PlantState:
    def __init__(self):
        self.sensors = {
            "soil": 0, "soilPct": 0, "soilDesc": "–",
            "water": 0, "waterPct": 0,
            "temp": 0.0, "humidity": 0.0,
            "pump": False, "pumpSpeed": 100, "fan": False, "fanSpeed": 0,
            "led": True, "auto": True, "waterLow": False
        }
        self.history: list = []
        self.last_seen: Optional[datetime] = None
        self.online: bool = False
        self.status_log: list = []

    def update_sensors(self, data: dict):
        with _state_lock:
            self.sensors.update(data)
            self.last_seen = datetime.now(timezone.utc)
            self.online = True
            entry = {**data, "time": self.last_seen.isoformat()}
            self.history.append(entry)
            if len(self.history) > 100:
                self.history.pop(0)

    def add_status(self, status: str):
        with _state_lock:
            self.status_log.append({
                "status": status,
                "time": datetime.now(timezone.utc).isoformat()
            })
            if len(self.status_log) > 50:
                self.status_log.pop(0)

    def check_online_timeout(self):
        with _state_lock:
            if self.last_seen and self.online:
                delta = (datetime.now(timezone.utc) - self.last_seen).total_seconds()
                if delta > OFFLINE_TIMEOUT_S:
                    self.online = False

    def get_state_snapshot(self) -> dict:
        with _state_lock:
            return {
                "sensors":    dict(self.sensors),
                "online":     self.online,
                "last_seen":  self.last_seen.isoformat() if self.last_seen else None,
                "status_log": list(self.status_log[-10:])
            }

    def get_history_snapshot(self) -> list:
        with _state_lock:
            return list(self.history)


plant = PlantState()
mqtt_client: Optional[mqtt.Client] = None

# =====================================================
# MQTT
# =====================================================
def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print(f"[MQTT] Pripojený na {MQTT_BROKER}:{MQTT_PORT} (TLS)")
        client.subscribe(TOPIC_SENSORS)
        client.subscribe(TOPIC_STATUS)
        for topic in (TOPIC_CMD_PUMP, TOPIC_CMD_FAN, TOPIC_CMD_LED, TOPIC_CMD_AUTO):
            client.publish(topic, payload=None, retain=True)
    else:
        print(f"[MQTT] Chyba pripojenia rc={rc}")

def on_message(client, userdata, msg):
    topic = msg.topic
    try:
        data = json.loads(msg.payload.decode())
    except Exception as e:
        print(f"[MQTT] Chyba parsovania správy z {topic}: {e}")
        return

    if topic == TOPIC_SENSORS:
        plant.update_sensors(data)
        print(
            f"[SENSOR] Teplota:{data.get('temp')}°C  "
            f"Pôda:{data.get('soilPct')}%  "
            f"Voda:{data.get('waterPct')}%  "
            f"Pump:{data.get('pump')}  Fan:{data.get('fan')}"
        )
    elif topic == TOPIC_STATUS:
        status = data.get("status", "")
        plant.add_status(status)
        print(f"[STATUS] {status}")
        if status == "online":
            with _state_lock:
                plant.online = True

def on_disconnect(client, userdata, rc):
    print(f"[MQTT] Odpojený (rc={rc})")
    with _state_lock:
        plant.online = False

def start_mqtt():
    global mqtt_client
    client = mqtt.Client(client_id=MQTT_CLIENT, protocol=mqtt.MQTTv311)

    tls_ctx = ssl.create_default_context()
    tls_ctx.check_hostname = False
    tls_ctx.verify_mode = ssl.CERT_NONE
    client.tls_set_context(tls_ctx)

    client.username_pw_set(MQTT_USER, MQTT_PASS)
    client.on_connect    = on_connect
    client.on_message    = on_message
    client.on_disconnect = on_disconnect
    client.reconnect_delay_set(min_delay=2, max_delay=30)

    try:
        client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
    except Exception as e:
        print(f"[MQTT] Nepodarilo sa pripojiť: {e}")

    mqtt_client = client
    client.loop_forever()

def send_command(topic: str, payload: dict) -> bool:
    if mqtt_client is None:
        return False
    try:
        result = mqtt_client.publish(topic, json.dumps(payload))
        return result.rc == mqtt.MQTT_ERR_SUCCESS
    except Exception as e:
        print(f"[MQTT] Chyba pri publikovaní na {topic}: {e}")
        return False

# =====================================================
# FASTAPI
# =====================================================
@asynccontextmanager
async def lifespan(app: FastAPI):
    t = threading.Thread(target=start_mqtt, daemon=True)
    t.start()
    # Azure App Service nastavuje PORT cez env
    port = int(os.getenv("PORT", 8000))
    print(f"[SERVER] Spustený, port={port}")
    yield

app = FastAPI(title="Smart Greenhouse API", lifespan=lifespan)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

# =====================================================
# API ENDPOINTY
# =====================================================

@app.get("/api/state")
def get_state():
    plant.check_online_timeout()
    return plant.get_state_snapshot()


@app.get("/api/history")
def get_history():
    return {"history": plant.get_history_snapshot()}


@app.post("/api/led")
def control_led(on: bool, r: int = 0, g: int = 150, b: int = 80, brightness: int = 20):
    if not (0 <= r <= 255 and 0 <= g <= 255 and 0 <= b <= 255):
        raise HTTPException(status_code=422, detail="RGB hodnoty musia byť 0–255")
    brightness = max(0, min(brightness, 30))
    ok = send_command(TOPIC_CMD_LED, {"on": on, "r": r, "g": g, "b": b, "brightness": brightness})
    return {"ok": ok, "sent": {"on": on, "r": r, "g": g, "b": b, "brightness": brightness}}


@app.post("/api/pump")
def control_pump(on: bool, speed: int = 100):
    if on and plant.sensors.get("waterLow", False):
        raise HTTPException(status_code=409, detail="Nelze zapnúť čerpadlo – nízka hladina vody!")
    speed = max(10, min(100, speed))
    ok = send_command(TOPIC_CMD_PUMP, {"on": on, "speed": speed})
    return {"ok": ok, "speed": speed}


@app.post("/api/fan")
def control_fan(on: bool, speed: int = 80):
    speed = max(0, min(100, speed))
    ok = send_command(TOPIC_CMD_FAN, {"on": on, "speed": speed})
    return {"ok": ok, "speed": speed}


@app.post("/api/auto")
def control_auto(on: bool):
    ok = send_command(TOPIC_CMD_AUTO, {"on": on})
    return {"ok": ok}


@app.get("/", response_class=HTMLResponse)
def dashboard():
    html_path = os.path.join(os.path.dirname(__file__), "index.html")
    try:
        with open(html_path, "r", encoding="utf-8") as f:
            return f.read()
    except FileNotFoundError:
        raise HTTPException(status_code=404, detail="index.html nenájdený")


# =====================================================
# SPUSTENIE
# Azure App Service číta PORT z env, preto 0.0.0.0 + PORT
# =====================================================
if __name__ == "__main__":
    port = int(os.getenv("PORT", 8000))
    uvicorn.run(app, host="0.0.0.0", port=port, reload=False)
