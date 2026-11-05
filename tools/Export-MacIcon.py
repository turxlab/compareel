from pathlib import Path
import shutil, subprocess, tempfile
from PIL import Image, ImageDraw, ImageOps

# macOS icons follow Apple's grid: an 824 px rounded square centred on a 1024 px canvas. Since macOS 26
# an icon without that shape is shrunk onto a grey plate, so the logo sits on the app.svg plate colour.
root = Path(__file__).resolve().parents[1]
assets = root / 'src/qt/branding'
canvas, plate, radius, logo = 1024, 824, 185, 560
scale = 4  # supersampled corners
mask = Image.new('L', (canvas*scale, canvas*scale))
offset = (canvas-plate)//2*scale
ImageDraw.Draw(mask).rounded_rectangle((offset, offset, offset+plate*scale-1, offset+plate*scale-1), radius*scale, fill=255)
mask = mask.resize((canvas, canvas), Image.Resampling.LANCZOS)
top, bottom = (0x22, 0x2e, 0x40), (0x14, 0x1b, 0x26)
gradient = Image.new('RGB', (1, canvas))
for y in range(canvas):
    t = y/(canvas-1)
    gradient.putpixel((0, y), tuple(round(a+(b-a)*t) for a, b in zip(top, bottom)))
icon = Image.new('RGBA', (canvas, canvas))
icon.paste(gradient.resize((canvas, canvas)), (0, 0), mask)
with Image.open(assets / 'compareel-logo.png') as source:
    scaled = ImageOps.contain(source.convert('RGBA'), (logo, logo), Image.Resampling.LANCZOS)
icon.alpha_composite(scaled, ((canvas-scaled.width)//2, (canvas-scaled.height)//2))
with tempfile.TemporaryDirectory() as temp:
    iconset = Path(temp) / 'app.iconset'; iconset.mkdir()
    for size in (16, 32, 128, 256, 512):
        icon.resize((size, size), Image.Resampling.LANCZOS).save(iconset / f'icon_{size}x{size}.png')
        icon.resize((size*2, size*2), Image.Resampling.LANCZOS).save(iconset / f'icon_{size}x{size}@2x.png')
    subprocess.run(['iconutil', '-c', 'icns', str(iconset), '-o', str(assets / 'app.icns')], check=True)
icon.resize((256, 256), Image.Resampling.LANCZOS).save(Path(tempfile.gettempdir()) / 'compareel-icns-preview.png')
