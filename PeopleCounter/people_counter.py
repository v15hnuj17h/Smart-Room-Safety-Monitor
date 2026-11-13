"""
people_counter.py - opens a live camera window with YOLO person detections and a people count,
and publishes the count to Firebase (/rooms/<ROOM_ID>/occupancy) for the dashboard and the Uno LCD.

Setup (once):
    pip install ultralytics opencv-python firebase-admin
    Firebase console -> Project settings -> Service accounts -> "Generate new private key"
    Save the JSON next to this script as serviceAccountKey.json  (keep it private, never commit it)

Run:
    python people_counter.py              # window + Firebase
    python people_counter.py --local      # window only (no Firebase) - good for testing the model
    python people_counter.py --camera 1   # other webcam, video file or stream URL
    python people_counter.py --no-window  # headless
Press q in the window to quit.
"""
import argparse
import statistics
import time
from collections import deque

import cv2
from ultralytics import YOLO

 
MODEL_PATH      = r"models/yolo26n.pt"
ROOM_ID         = "room1"                       
DATABASE_URL    = "https://gas-sensor-ventilation-system-default-rtdb.asia-southeast1.firebasedatabase.app"    
SERVICE_ACCOUNT = "serviceAccountKey.json"

CONFIDENCE      = 0.45      
SMOOTH_FRAMES   = 15        
HEARTBEAT_S     = 2.0       
MIN_GAP_S       = 0.5       


def person_class_ids(model):
    """Class ids named 'person' in the model (works for COCO and for custom models)."""
    ids = [i for i, name in model.names.items() if str(name).lower() == "person"]
    if not ids:
        print(f"WARNING: no class called 'person' in this model {dict(model.names)}; counting ALL detections.")
        return None
    return ids


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--camera", default="0", help="webcam index, video file or stream URL")
    ap.add_argument("--local", action="store_true", help="do not use Firebase (window only)")
    ap.add_argument("--no-window", action="store_true", help="do not open the preview window")
    args = ap.parse_args()
    source = int(args.camera) if args.camera.isdigit() else args.camera

    ref = None
    if not args.local:
        import firebase_admin
        from firebase_admin import credentials, db
        firebase_admin.initialize_app(credentials.Certificate(SERVICE_ACCOUNT),
                                      {"databaseURL": DATABASE_URL})
        ref = db.reference(f"rooms/{ROOM_ID}/occupancy")

    model = YOLO(MODEL_PATH)
    classes = person_class_ids(model)

    cap = cv2.VideoCapture(source)
    if not cap.isOpened():
        raise SystemExit(f"Cannot open camera/source: {args.camera}")

    if not args.no_window:
        cv2.namedWindow("camera", cv2.WINDOW_NORMAL)

    recent = deque(maxlen=SMOOTH_FRAMES)
    last_sent_count, last_sent_at = None, 0.0

    print("Counting people" + ("" if ref is None else f" -> rooms/{ROOM_ID}/occupancy") + "  (q = quit)")
    try:
        while True:
            ok, frame = cap.read()
            if not ok:
                 
                print("Camera read failed - not publishing; retrying...")
                time.sleep(1.0)
                continue

            res = model.predict(frame, classes=classes, conf=CONFIDENCE, verbose=False)[0]
            raw_count = len(res.boxes)
            recent.append(raw_count)
            count = int(round(statistics.median(recent)))

             
            now = time.time()
            changed = count != last_sent_count
            due = (now - last_sent_at) >= HEARTBEAT_S
            if (changed and (now - last_sent_at) >= MIN_GAP_S) or due:
                if ref is not None:
                    try:
                        ref.set({"count": count, "ts": int(now), "source": "laptop-yolo"})
                    except Exception as e:       
                        print("Firebase write failed:", e)
                if changed:
                    print(f"People in room: {count}")
                last_sent_count, last_sent_at = count, now

             
            if not args.no_window:
                shown = res.plot()               
                cv2.rectangle(shown, (0, 0), (260, 44), (0, 0, 0), -1)
                cv2.putText(shown, f"People: {count}", (10, 32),
                            cv2.FONT_HERSHEY_SIMPLEX, 1.0, (0, 255, 0), 2)
                cv2.imshow("camera", shown)
                if cv2.waitKey(1) & 0xFF == ord("q"):
                    break
    except KeyboardInterrupt:
        pass
    finally:
        cap.release()
        cv2.destroyAllWindows()
        print("Stopped. The dashboard will show the camera as offline shortly.")


if __name__ == "__main__":
    main()