from pathlib import Path
from PIL import Image, ImageDraw, ImageOps

root = Path(__file__).resolve().parents[1]
assets = root / 'src/qt/branding'
sizes = (16, 24, 32, 48, 64, 128, 256, 512)
with Image.open(assets / 'compareel-logo.png') as source:
    source.load()
    assert source.mode == 'RGBA' and source.getchannel('A').getextrema() == (0, 255)
    for size in sizes:
        inner = max(1, round(size * 0.94))
        scaled = ImageOps.contain(source, (inner, inner), Image.Resampling.LANCZOS)
        image = Image.new('RGBA', (size, size))
        image.alpha_composite(scaled, ((size-scaled.width)//2, (size-scaled.height)//2))
        image.save(assets / f'app-{size}.png')
with Image.open(assets / 'app-256.png') as source:
    source.save(assets / 'app.ico', sizes=[(s,s) for s in sizes if s <= 256],
                append_images=[Image.open(assets / f'app-{s}.png') for s in sizes if s < 256])
with Image.open(assets / 'app.ico') as icon:
    assert icon.ico.sizes() == {(s,s) for s in sizes if s <= 256}
    for size in sizes:
        if size > 256:
            continue
        frame = icon.ico.getimage((size, size))
        assert frame.getpixel((0, 0))[3] == 0 and frame.getchannel('A').getextrema() == (0, 255)
preview = Image.new('RGB', (800, 340), '#f0f3f7')
draw = ImageDraw.Draw(preview)
draw.rectangle((400,0,800,340), fill='#111720')
for offset in (0,400):
    with Image.open(assets / 'app-256.png') as image:
        preview.paste(image, (offset+72, 8), image)
    x = offset+84
    for size in (16,24,32,48):
        with Image.open(assets / f'app-{size}.png') as image:
            preview.paste(image, (x, 280), image)
        x += size+12
preview.save(assets / 'preview.png')
print('Raster PNG exports and seven transparent ICO resolutions verified.')
