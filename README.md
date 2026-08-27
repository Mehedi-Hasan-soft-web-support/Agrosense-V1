# AgroSense Node

An ESP32 field node that measures light, air temperature, humidity and soil moisture, shows them on a small OLED, and streams them to a Supabase database. A single HTML page gives you live values and lets you download the whole record as Excel or CSV.

Built at the Embedded Systems Research Center, Daffodil International University.

---

## Contents

- [What is in this repository](#what-is-in-this-repository)
- [Hardware](#hardware)
- [Wiring](#wiring)
- [Part 1: Set up Supabase](#part-1-set-up-supabase)
- [Part 2: Connect the node to Wi-Fi](#part-2-connect-the-node-to-wi-fi)
- [Part 3: Upload the firmware](#part-3-upload-the-firmware)
- [Part 4: Use the web dashboard](#part-4-use-the-web-dashboard)
- [Calibrating the soil sensor](#calibrating-the-soil-sensor)
- [Keeping Supabase awake](#keeping-supabase-awake)
- [Troubleshooting](#troubleshooting)
- [Data dictionary](#data-dictionary)

---

## What is in this repository

| File | What it does |
|---|---|
| `agrosense_esp32.ino` | Firmware for the ESP32 node |
| `supabase_full_setup.sql` | Creates every table, view, policy and function. Run this first |
| `supabase_delete_addon.sql` | Optional. Lets the website delete readings |
| `agrosense_simple.html` | Live dashboard and dataset download |
| `supabase-keepalive.yml` | GitHub Actions cron that stops the free project from pausing |

---

## Hardware

| Part | Notes |
|---|---|
| ESP32 DevKit v1 | Any 30 or 38 pin board. **Not** ESP32-CAM, its pins are taken by the camera |
| BH1750 | Ambient light, I2C, address `0x23` |
| DHT11 | Air temperature and humidity |
| Capacitive soil moisture v2.0 | Use the capacitive type, not the resistive one. Resistive probes corrode within weeks |
| SSD1306 OLED 0.96 inch | I2C, address `0x3C` |
| 10k resistor | Pull-up for the DHT11 data line |
| 5V 2A supply | A weak phone charger causes random reboots during Wi-Fi transmission |

---

## Wiring

```
BH1750     VCC -> 3V3    GND -> GND    SDA -> GPIO21    SCL -> GPIO22    ADDR -> GND
OLED       VCC -> 3V3    GND -> GND    SDA -> GPIO21    SCL -> GPIO22
DHT11      VCC -> 3V3    GND -> GND    DATA -> GPIO4    (10k between DATA and 3V3)
SOIL       VCC -> 3V3    GND -> GND    AOUT -> GPIO34
```

BH1750 and the OLED share one I2C bus. That is normal, they have different addresses.

**GPIO34 is not optional.** The soil sensor must sit on an ADC1 pin. ADC2 pins (0, 2, 4, 12 to 15, 25 to 27) stop working the moment Wi-Fi turns on, and `analogRead` will return garbage. ADC1 pins are 32, 33, 34, 35, 36 and 39.

GPIO34 is input only, so it cannot power anything. That is fine here, the sensor takes power from 3V3.

---

## Part 1: Set up Supabase

1. Create a project at [supabase.com](https://supabase.com). Choose the region closest to you, Singapore is usually fastest from Bangladesh.
2. Open **SQL Editor**, click **New query**, paste all of `supabase_full_setup.sql`, and press **Run**.
3. Open **Settings > API** and copy two things:
   - **Project URL**, looks like `https://abcdefgh.supabase.co`
   - **anon public** key, a long string starting with `eyJ...`
4. Paste both into the firmware and into the HTML file.

The `anon` key is designed to be public, but keep row level security on. The setup SQL allows insert and select only, so nobody can change or erase your data with it.

### If you want the website to delete readings

Run `supabase_delete_addon.sql` as well. Skip this if you do not need it, since it lets anyone holding the key erase readings.

---

## Part 2: Connect the node to Wi-Fi

Open `agrosense_esp32.ino` and edit these two lines near the top:

```cpp
#define WIFI_SSID     "admin"
#define WIFI_PASS     "12345678"
```

Both are case sensitive. `MyWiFi` and `mywifi` are different networks.

### Rules the ESP32 cannot get around

**2.4 GHz only.** The ESP32 has no 5 GHz radio. If your router broadcasts one name for both bands, most routers will still let the ESP32 join the 2.4 GHz side. If your 5 GHz network has a separate name such as `Home_5G`, do not use it.

**No captive portals.** University and cafe networks that show a login page in the browser will not work. The ESP32 connects to the access point but never gets past the login screen. Use a phone hotspot instead.

**Password length.** WPA2 needs 8 characters or more. An open network with no password works too, just leave `WIFI_PASS` as `""`.

**Special characters.** If your password contains a backslash or a double quote, escape it: `"pa\\ss"` or `"pa\"ss"`.

### Using a phone hotspot

This is the easiest option for demos and field work.

1. On Android, open **Settings > Hotspot** and set the band to **2.4 GHz**. Newer phones default to 5 GHz and the ESP32 will never find them.
2. Give the hotspot a simple name with no emoji and no spaces.
3. Put that name and password into the firmware.

On iPhone, turn on **Maximise Compatibility** in the Personal Hotspot settings. That switches the hotspot to 2.4 GHz.

### Checking that it worked

Open **Tools > Serial Monitor** in the Arduino IDE and set the speed to **115200**. On boot you should see:

```
=== AgroSense Node v1.2.0 ===
[BH1750] OK
[WiFi] connecting.....
[WiFi] connected  IP=192.168.0.105  RSSI=-54
[NTP] synced (UTC)
[READ] lux=850.5  T=29.4C  RH=68%  soil=2100 (59.4%)  rssi=-54
[UP] 1 rows OK  (queue left = 0)
```

The OLED also shows `NET` in the top right corner when the node is online, and `OFF` when it is not.

### Reading the signal strength

`RSSI` tells you how good the link is.

| RSSI | Meaning |
|---|---|
| -30 to -60 dBm | Strong |
| -60 to -75 dBm | Usable |
| Below -75 dBm | Weak, expect dropped uploads |

If the node sits far from the router, move it closer or add a repeater. The node keeps up to 15 minutes of readings on board and pushes the backlog when the link returns, so short outages do not lose data.

---

## Part 3: Upload the firmware

### Install the board support

1. Open **File > Preferences**.
2. In **Additional Board Manager URLs**, add:
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
3. Open **Tools > Board > Boards Manager**, search for `esp32`, install the Espressif package.

### Install the libraries

Open **Tools > Manage Libraries** and install:

- `BH1750` by Christopher Laws
- `DHT sensor library` by Adafruit
- `Adafruit Unified Sensor` by Adafruit
- `Adafruit SSD1306` by Adafruit
- `Adafruit GFX Library` by Adafruit

### Board settings

| Setting | Value |
|---|---|
| Board | ESP32 Dev Module |
| Upload Speed | **115200** |
| Flash Frequency | 40 MHz |
| Partition Scheme | Default, or Minimal SPIFFS if you run out of space |
| Port | Whichever COM port appears when you plug the board in |

Upload speed matters. The default 921600 fails on many cables and produces `A fatal error occurred: The chip stopped responding`.

### Upload

Press upload. If it stalls on `Connecting......`, hold the **BOOT** button on the board until `Writing at 0x...` appears, then let go.

The sketch folder name must match the `.ino` file name. If the folder is called `cam_module_code` and the file is `agrosense_esp32.ino`, the IDE will refuse to open it.

---

## Part 4: Use the web dashboard

Open `agrosense_simple.html` in any browser. No server, no install. Edit these two lines inside the file first:

```javascript
const URL_ = "https://YOUR-PROJECT.supabase.co";
const KEY  = "your-anon-key";
```

### Live values

The four cards at the top refresh every 5 seconds. **Refresh now** forces an immediate update.

The dot beside the node name is green while readings are arriving and turns orange if nothing has come in for two minutes.

### Downloading a dataset

1. Set **Node**, **From** and **To**. The quick buttons fill in the last hour, day or week.
2. Set **Max rows**. At a 2 second upload interval, one day is about 43,000 rows.
3. Press **Load readings**.
4. Press **Download Excel** or **Download CSV**.

The export carries two time columns, one in your local clock and one in UTC. Keep the UTC column when you publish the dataset, since local time is ambiguous across time zones and daylight saving.

CSV files are written with a UTF-8 byte order mark, so Bengali text opens correctly in Excel.

### Deleting readings

Only works after you run `supabase_delete_addon.sql`.

| Control | Removes |
|---|---|
| **Delete** on a row | That one reading |
| **Delete this window** | Everything in the loaded date range |
| **Delete all data** | Every reading for the node in the Node box |

If the Node box is empty, **Delete all data** clears every node in the database. The confirmation dialog says which case you are in.

None of this can be undone. Download the data before you clear it.

---

## Calibrating the soil sensor

Every capacitive probe reads differently. Do this once per sensor.

1. Upload the firmware and open the Serial Monitor.
2. Hold the probe in dry air and note the `soil=` raw number. That is your dry value, usually near 3200.
3. Put the probe in a glass of water up to the marked line, not past it, and note the raw number. That is your wet value, usually near 1350.
4. Put both into the firmware and upload again:

```cpp
const int SOIL_DRY_RAW = 3200;
const int SOIL_WET_RAW = 1350;
```

Do not submerge the probe above the white line printed on the board. The electronics above that line are not waterproof.

For field work, calibrate in the actual soil rather than water. Water gives you the theoretical maximum, but saturated soil reads lower, so a water calibration makes every field reading look drier than it is.

---

## Keeping Supabase awake

A free Supabase project pauses after **7 days with no API or database activity**. Logging into the dashboard does not count, only real requests do.

Three layers protect against this:

1. **The node itself.** It uploads constantly and sends a keep-alive ping every 6 hours. As long as the node runs, the project stays awake.
2. **GitHub Actions.** `supabase-keepalive.yml` calls the `ping` function every 12 hours, which covers the node being switched off. Add `SUPABASE_URL` and `SUPABASE_KEY` as repository secrets under **Settings > Secrets and variables > Actions**.
3. **A third party cron.** Point cron-job.org or UptimeRobot at the same endpoint every 12 hours as a backup.

Test the ping by hand:

```bash
curl -X POST "https://YOUR-PROJECT.supabase.co/rest/v1/rpc/ping" \
  -H "apikey: YOUR_ANON_KEY" \
  -H "Authorization: Bearer YOUR_ANON_KEY" \
  -H "Content-Type: application/json" \
  -d '{"p_source":"manual"}'
```

A paused project does not lose data. Open the dashboard and press **Restore**, it comes back in a minute or two.

GitHub disables scheduled workflows in repositories with no activity for 60 days. The workflow makes a small commit on the first of each month to avoid this.

### Storage

At a 2 second interval the node writes about 43,000 rows a day, roughly 250 MB a month with indexes. The free tier gives you 500 MB, so run the cleanup weekly:

```sql
select public.purge_old_readings(14);   -- keeps the last 14 days
```

For long deployments, sampling every 2 seconds but uploading every 10 seconds gives the same data resolution with a fifth of the requests. Set `SENSOR_INTERVAL = 2000` and `UPLOAD_INTERVAL = 10000`.

---

## Troubleshooting

### Upload problems

| Message | Cause and fix |
|---|---|
| `Could not open COM7, the port is busy` | Serial Monitor is open somewhere. Close it, or restart the IDE |
| `The semaphore timeout period has expired` | Cable or driver. Try a different data cable and a rear USB 2.0 port |
| `The chip stopped responding` after `Changing baud rate` | Upload speed too high. Set it to 115200 |
| No COM port at all | Missing driver. Check whether the board uses CH340 or CP2102 and install that driver |
| Stuck on `Connecting......` | Hold BOOT during upload |

### The node runs but nothing reaches Supabase

Read the Serial Monitor. The HTTP code tells you what is wrong.

| Code | Meaning |
|---|---|
| `401` | Wrong anon key |
| `404` | Table missing. Run `supabase_full_setup.sql` |
| `409` | The `device_id` in the firmware does not match any row in `devices` |
| `-1` or `-100` | No Wi-Fi, or TLS failed |

### Sensor problems

| Symptom | Cause |
|---|---|
| `[BH1750] FAIL` | Check SDA and SCL, and that ADDR goes to GND |
| Temperature and humidity always `nan` | DHT11 wiring, or the 10k pull-up is missing |
| Temperature repeats for several rows | Normal. The DHT11 only produces a new value about once per second |
| Soil percent stuck at 0 or 100 | Calibration values are wrong, or the sensor is on an ADC2 pin |
| OLED blank | Try address `0x3D` instead of `0x3C` |

### Website problems

| Symptom | Cause |
|---|---|
| Table empty, no error | No readings in the selected window. Widen the dates |
| `401` in the browser console | Wrong key in the HTML file |
| Delete does nothing and reports nothing | Delete policy missing. Run `supabase_delete_addon.sql` |
| Old value shown as live | Hard refresh with Ctrl+Shift+R |

---

## Data dictionary

Table `readings`:

| Column | Unit | Notes |
|---|---|---|
| `id` | | Auto increment, use this to order by insertion |
| `device_id` | | Node identifier |
| `lux` | lx | BH1750, 0 to 65535 |
| `temperature` | °C | DHT11, ±2 °C accuracy |
| `humidity` | %RH | DHT11, ±5 %RH accuracy |
| `soil_raw` | | Raw ADC, 0 to 4095 |
| `soil_percent` | % | Calibrated from raw |
| `rssi` | dBm | Wi-Fi signal at the moment of the reading |
| `uptime_s` | s | Seconds since the node last booted |
| `buffered` | | `true` if the row waited in the offline queue |
| `recorded_at` | | When the node took the reading, UTC |
| `created_at` | | When the server stored it, UTC |

`recorded_at` and `created_at` differ when a row came out of the offline buffer. Use `recorded_at` for analysis.

DHT11 accuracy is the limiting factor in this build. If your work needs better than ±2 °C, the DHT22 or SHT31 drops in with only a change to `DHT_TYPE`.

---

## License

MIT

## Author

Md. Mehedi Hasan, Embedded Systems Research Center, Daffodil International University.
