import datetime
import cv2
import numpy as np
from threading import Lock
from flask import Flask, request, jsonify
import easyocr
import firebase_admin
from firebase_admin import credentials, db

# ==== Config ====
HOST = "0.0.0.0"
PORT = 5000

# ==== Firebase Init ====
cred = credentials.Certificate(
    r"C:\Users\HP\OneDrive\Documents\smart-attendance\Firebase-admin.json"
)
firebase_admin.initialize_app(cred, {
    'databaseURL': "https://drive-thru-smartattendance-default-rtdb.asia-southeast1.firebasedatabase.app"
})

# ==== Flask App ====
app = Flask(__name__)
reader = easyocr.Reader(['en'])
last_result = {"plate": "-", "time": "-", "method": "none"}
snapshots = []

# ==== OCR ====
def ocr_easyocr(img_bgr):
    gray = cv2.cvtColor(img_bgr, cv2.COLOR_BGR2GRAY)
    results = reader.readtext(gray)
    if not results:
        return "-"
    texts = [res[1] for res in results]
    return max(texts, key=len)

def detect_and_ocr(img_bgr):
    global last_result, snapshots
    plate = ocr_easyocr(img_bgr).replace(" ", "").upper()
    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    last_result = {"plate": plate, "time": now, "method": "EasyOCR"}
    snapshots.insert(0, {"time": now, "plate": plate, "img": img_bgr.copy()})
    snapshots = snapshots[:5]

    save_attendance("plate", plate, now)
    return last_result

# ==== Attendance Logic ====
def save_attendance(mode, key, timestamp):
    today = timestamp.split(" ")[0]
    time_now = timestamp.split(" ")[1]

    # semak pengguna dalam Firebase
    ref = db.reference(f"plates/{key}" if mode == "plate" else f"rfid_cards/{key}")
    user_data = ref.get()
    if not user_data:
        print(f"{mode.upper()} {key} tidak didaftarkan. Tiada data disimpan.")
        return

    uid = user_data.get("uid") or key
    name = user_data.get("name", "-")
    jabatan = user_data.get("jabatan", "-")
    plate = user_data.get("plate", key if mode == "plate" else "-")

    att_ref = db.reference(f"attendance/{today}/{uid}")
    att_data = att_ref.get()

    day_of_week = datetime.datetime.strptime(today, "%Y-%m-%d").weekday()
    check_time_dt = datetime.datetime.strptime(f"{today} {time_now}", "%Y-%m-%d %H:%M:%S")

    # shift logic
    if day_of_week < 4:  # isnin-khamis
        shift_name = "A" if check_time_dt.time() <= datetime.time(8, 0, 0) else "B"
        shift_start = datetime.datetime.strptime(
            f"{today} 08:00:00", "%Y-%m-%d %H:%M:%S"
        ) if shift_name == "A" else datetime.datetime.strptime(
            f"{today} 10:00:00", "%Y-%m-%d %H:%M:%S"
        )
        min_hours = 7
    elif day_of_week == 4:  # jumaat
        shift_name = "A"
        shift_start = datetime.datetime.strptime(
            f"{today} 08:00:00", "%Y-%m-%d %H:%M:%S"
        )
        min_hours = 4
    else:  # sabtu/ahad
        shift_name = "A"
        shift_start = datetime.datetime.strptime(
            f"{today} 08:00:00", "%Y-%m-%d %H:%M:%S"
        )
        min_hours = 5

    # ==== Checkin / Checkout ====
    if not att_data:
        # check-in pertama
        punctuality = "Punctual" if check_time_dt <= shift_start + datetime.timedelta(minutes=1) else "Late"
        att_ref.set({
            "name": name,
            "jabatan": jabatan,
            "uid": uid,
            "plate": plate,
            "shift": shift_name,
            "punctuality": punctuality,
            "checkin": time_now,
            "checkout": None,       # belum keluar
            "date": today
        })

    else:
        # sudah ada check-in → ini checkout
        checkin_time = datetime.datetime.strptime(
            f"{today} {att_data['checkin']}", "%Y-%m-%d %H:%M:%S"
        )
        checkout_time = check_time_dt
        if checkout_time < checkin_time:
            checkout_time += datetime.timedelta(days=1)

        delta = checkout_time - checkin_time
        hours = delta.seconds // 3600
        minutes = (delta.seconds % 3600) // 60
        worked_hours_str = f"{hours} hour {minutes} min"
        total_hours = delta.total_seconds() / 3600

        status = "Complete" if total_hours >= min_hours else "Incomplete"
        punctuality = att_data.get("punctuality", "Late")

        att_ref.update({
            "checkout": time_now,
            "workedHours": worked_hours_str,
            "status": status,
            "punctuality": punctuality,
            "shift": shift_name
        })

    # kemas kini rekod terkini
    latest_ref = db.reference("/latestPlate" if mode == "plate" else "/latestRFID")
    latest_ref.set({
        "plate": plate if mode == "plate" else None,
        "uid": uid if mode == "rfid" else None,
        "name": name,
        "date": today,
        "time": time_now,
        "timestamp": timestamp
    })

# ==== Flask Endpoints ====
@app.route("/")
def home():
    return "<h1>Smart Attendance Server Running</h1>"

@app.route("/upload", methods=["POST"])
def upload():
    img_bytes = request.get_data()
    nparr = np.frombuffer(img_bytes, np.uint8)
    img = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
    if img is None:
        return jsonify({"error": "decode failed"}), 400
    result = detect_and_ocr(img)
    return jsonify(result)

@app.route("/rfid", methods=["POST"])
def rfid():
    data = request.json
    uid = data.get("uid")
    if not uid:
        return jsonify({"error": "UID required"}), 400
    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    save_attendance("rfid", uid, now)
    return jsonify({"uid": uid, "time": now})

# ==== Run Server ====
if __name__ == "__main__":
    app.run(host=HOST, port=PORT, threaded=True)
