# class_schedule_tracker
A low power esp32 s3 display that shows you when your next period when lunch and how many days left of school with additional features like date and time


Arduino settings
<img width="1680" height="2512" alt="image" src="https://github.com/user-attachments/assets/6db91feb-529f-4e36-b68f-c07f709b7230" />

A low-power ESP32-S3 display that shows:

Next period countdown

Time until lunch

Days left of school

Current date & time

Additional features:

Battery voltage monitoring

Wi-Fi signal strength indicator

Screen on/off toggle via button

Elegant, compact UI with progress bars and cards

📥 Installation

Download the TFT library

git clone https://github.com/Xinyuan-LilyGO/T-Display-S3.git
cd T-Display-S3/lib
cp -r * ~/Documents/Arduino/libraries/

Open Arduino IDE

Select board: "ESP32S3 Dev Module" (downgraded to version 2.0.13 in Boards Manager)

Configure:

Flash Frequency: 80 MHz

PSRAM: Enabled (if available)

🚀 Usage

Connect your ESP32-S3 display via USB.

Upload the ClassScheduleTracker.ino sketch.

Hold IO14 (the button) to toggle the screen on/off.

Enjoy automatic cycling through:

Next period, lunch, end-of-day countdowns

Date & 12 hr clock

Days left of the school year progress

🎨 Customization

Bell schedule: Edit the schedule[] array in the sketch.

School dates: Adjust schoolStart and schoolEnd in the code.

Color themes: Modify the color constants (e.g., TFT_CYAN, TFT_YELLOW).

Layout: Tweak STATUS_H, margins, and corner radii for personal taste.

🛠️ Tips & Troubleshooting

Must downgrade ESP32 board package to v2.0.13; later versions may break compatibility.

Ensure only one TFT_eSPI library is installed under Documents/Arduino/libraries.

If the display is blank, press IO14 to turn it on.

For accurate battery readings, wire the battery sense to GPIO 4 with a 1:1 divider.

📄
