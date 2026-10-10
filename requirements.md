### Description and Goals
This projects is for an interactive installation that will play an mp3 audio file when a person is in front of it. 

### Hardware
The installation uses an ESP32-S3 Devkit C board, a Sharp digital distance sensor GP2Y0D02YK0F (active HIGH - output HIGH while an object is in range, per the datasheet), and an DY-SV8F mp3 player.

ESP32 Pin connections:
GPIO 14 - mp3 player TX
GPIO 13 - mp3 player RX
GPIO 12 - mp3 player Busy
GPIO 9 - distance sensor Output (open-collector - pin to be pulled-up in software, to 3.3V rather than the sensor's 5V)

Note: In order to protect the ESP32 pin, a 22k resistor is to be tied between GPIO 9 and GND.

### Software
The MCU continuously reads the sensor output to determine whether there is an object within its range. If there has been an object for more than 2 seconds, the MCU sends a command to the mp3 module to start playing the file on it. There is only one mp3 file on the module's memory and it is named 00000.mp3. The audio file will play for as long as there is an object within the sensor range and for 4 seconds after that - i.e. the audio is to play for 4 seconds after the object has exited the sensor range. 

If the object remains in front of the sensor even after the mp3 file has ended, the MCU is to wait for 3 seconds after the file ends and if after these 3 seconds the object is still there, the playback is to begin again.

The communication with the MP3 modules is to follow the rules set out for a companion project that is available at ~/Projects/tutrakan-storyteller. This includes retries and confirmation logic.

The onboard RGB LED is to be used for showing the status as follows:
- blinking yellow at 25ms ON 500ms OFF intervals while idle
- solid yellow when an object is in front of the sensor but 2 second wait time has not elapsed
- blinking blue at 50ms ON 500ms OFF intervals when the mp3 file is being played
- solid blue in the wait time after a person has left or the mp3 file has finished playing
- solid red for 1 second if there is an error in the communication with the mp3 module (e.g. retries have been exhausted). After 1 second, the MCU goes back in idle state.
