import hub75
import machine
import time

WIDTH = 64
HEIGHT = 31

display = hub75.Hub75(WIDTH, HEIGHT)
display.fill(0)
display.text("Hello", 0, 0, display.color(255, 0, 0))
display.refresh()

while True:
    display.text("Updated!", 0, 8, display.color(0, 255, 0))
    display.refresh()
    time.sleep(10)
