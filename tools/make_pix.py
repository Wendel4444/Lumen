# Generates docs/pix.png: a static Pix BR Code (EMV QR) for donations.
# Usage: python tools/make_pix.py
import os, qrcode
from PIL import Image, ImageDraw, ImageFont

KEY, NAME, CITY = "delsanvfx@gmail.com", "LUMEN", "BRASIL"

def f(tag, value): return f"{tag}{len(value):02d}{value}"

def crc16(data: str) -> str:
    crc = 0xFFFF
    for b in data.encode():
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return f"{crc:04X}"

payload = (f("00", "01") + f("26", f("00", "br.gov.bcb.pix") + f("01", KEY)) + f("52", "0000") +
           f("53", "986") + f("58", "BR") + f("59", NAME) + f("60", CITY) + f("62", f("05", "***")) + "6304")
payload += crc16(payload)

qr = qrcode.QRCode(error_correction=qrcode.constants.ERROR_CORRECT_M, box_size=10, border=3)
qr.add_data(payload); qr.make(fit=True)
code = qr.make_image(fill_color=(40, 16, 90), back_color="white").convert("RGB")

W = code.width + 40
img = Image.new("RGB", (W, code.width + 130), "white")
img.paste(code, (20, 70))
d = ImageDraw.Draw(img)
def font(sz, bold=False):
    for n in (("segoeuib.ttf" if bold else "segoeui.ttf"), "arial.ttf"):
        try: return ImageFont.truetype(n, sz)
        except OSError: pass
    return ImageFont.load_default()
def center(y, text, ft, fill):
    w = d.textlength(text, font=ft); d.text(((W - w) / 2, y), text, font=ft, fill=fill)
center(18, "Apoie o Lumen via Pix", font(30, True), (84, 34, 178))
center(code.width + 78, KEY, font(24), (60, 60, 70))

out = os.path.join(os.path.dirname(__file__), "..", "docs", "pix.png")
img.save(out)
print(payload)
