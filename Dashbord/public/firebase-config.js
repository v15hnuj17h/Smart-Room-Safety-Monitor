
export const firebaseConfig = {
  apiKey: "AIzaSyCVME5ndD944xFx0XLfGaFmvcOBAxyHCrU",
  authDomain: "gas-sensor-ventilation-system.firebaseapp.com",
  databaseURL: "https://gas-sensor-ventilation-system-default-rtdb.asia-southeast1.firebasedatabase.app",
  projectId: "gas-sensor-ventilation-system",
  storageBucket: "gas-sensor-ventilation-system.firebasestorage.app",
  messagingSenderId: "487994351190",
  appId: "1:487994351190:web:7016ce5d8f031fbecae5f7"
};

export const DEVICE_ID = "room1";
export const ROOM_ID = "room1";

export const THRESHOLDS = {
  warnOn: 300,
  warnOff: 270,
  dangerOn: 500,
  dangerOff: 450,
  max: 1023,
};

export const OFFLINE_AFTER_S = 15;