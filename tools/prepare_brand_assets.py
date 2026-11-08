"""Package the operator's unchanged artwork into platform icon sizes/formats.
No generative redraw, background removal, retouching, cropping or stretching.
"""
from pathlib import Path
import hashlib
import json
from PIL import Image, ImageOps

ROOT=Path(__file__).resolve().parent.parent
ASSETS=ROOT/'assets/brand'

def square(source,size):
    image=ImageOps.contain(source,(size,size),Image.Resampling.LANCZOS)
    canvas=Image.new('RGB',(size,size),'white')
    canvas.paste(image,((size-image.width)//2,(size-image.height)//2))
    return canvas

def main():
    source=Image.open(ASSETS/'source.png').convert('RGB')
    for size in (32,48,72,96,144,192,256,512):
        square(source,size).save(ASSETS/f'icon-{size}.png',optimize=True)
    square(source,256).save(ASSETS/'heimao.ico',sizes=[(n,n) for n in (16,24,32,48,64,128,256)])
    manifest={'source':'operator supplied artwork; unchanged source.png','source_dimensions':source.size,
        'operation':'aspect-fit into white square; platform resize and ICO encoding only',
        'files':{f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in ASSETS.iterdir() if f.is_file()}}
    (ASSETS/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf-8')
    print('Packaged original artwork for Android, web, and Windows icons')

if __name__=='__main__':main()
