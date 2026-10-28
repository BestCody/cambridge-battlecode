"""Slowly moves the mouse in a small circle so the computer stays awake.
 
Setup:   pip install pyautogui
Run:     python keep_awake.py
Stop:    Ctrl+C in the terminal (or slam the mouse into a screen corner,
         which triggers pyautogui's failsafe).
"""
 
import math
import time
 
import pyautogui
 
RADIUS = 50        # circle size in pixels
STEP_DELAY = 0.05  # seconds between steps (higher = slower)
STEPS = 120        # points per full circle
 
pyautogui.FAILSAFE = True
 
cx, cy = pyautogui.position()  # circle is centered where the mouse starts
 
print("Keeping awake... press Ctrl+C to stop.")
try:
    while True:
        for i in range(STEPS):
            angle = 2 * math.pi * i / STEPS
            x = cx + RADIUS * math.cos(angle) - RADIUS
            y = cy + RADIUS * math.sin(angle)
            pyautogui.moveTo(x, y)
            time.sleep(STEP_DELAY)
except KeyboardInterrupt:
    print("\nStopped.")