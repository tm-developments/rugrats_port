# make_icon.py OUT.ico LOGO.png OLD.ico : 16-64 px from OLD.ico (Tommy), 96-256 px the logo
# (res/logo_launchbox.png, LaunchBox Games Database) centred on a transparent square.
import struct, io, sys
from PIL import Image
logo = Image.open(sys.argv[2]).convert('RGBA')
bb = logo.getchannel('A').point(lambda a: 255 if a > 8 else 0).getbbox(); logo = logo.crop(bb)
src = Image.open(sys.argv[3])
def old(s):
    src.size = (s, s); return src.convert('RGBA').copy()
def premul_resize(im, size):
    # resize in premultiplied alpha so the transparent edge doesn't fringe
    import numpy as np
    a = np.asarray(im).astype(np.float64) / 255
    a[..., :3] *= a[..., 3:4]
    chans = [Image.fromarray((a[..., i] * 65535).astype('uint16').astype('int32'), 'I') for i in range(4)]
    out = [np.asarray(c.resize(size, Image.LANCZOS), dtype=np.float64) / 65535 for c in chans]
    r = np.dstack(out).clip(0, 1)
    al = r[..., 3:4]; rgb = np.where(al > 1e-4, r[..., :3] / np.maximum(al, 1e-4), 0).clip(0, 1)
    return Image.fromarray((np.dstack([rgb, al]) * 255 + .5).astype('uint8'), 'RGBA')
def logo_icon(s):
    pad = max(1, round(s * 0.02)); box = s - 2 * pad
    w, h = logo.size; k = min(box / w, box / h)
    nw, nh = max(1, round(w * k)), max(1, round(h * k))
    canvas = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    canvas.paste(premul_resize(logo, (nw, nh)), ((s - nw) // 2, (s - nh) // 2))
    return canvas
imgs = []
for s in (16, 24, 32, 48, 64, 96, 128, 256):
    imgs.append((s, logo_icon(s) if s >= 96 else old(s)))
def dib(im):
    s = im.size[0]
    px = im.transpose(Image.FLIP_TOP_BOTTOM).tobytes('raw', 'BGRA')
    maskrow = ((s + 31) // 32) * 4
    mask = b''
    a = im.transpose(Image.FLIP_TOP_BOTTOM).getchannel('A').tobytes()
    for y in range(s):
        bits = bytearray(maskrow)
        for x in range(s):
            if a[y * s + x] == 0: bits[x // 8] |= 0x80 >> (x % 8)
        mask += bytes(bits)
    return struct.pack('<IiiHHIIiiII', 40, s, 2 * s, 1, 32, 0, len(px) + len(mask), 0, 0, 0, 0) + px + mask
blobs = []
for s, im in imgs:
    if s == 256:
        b = io.BytesIO(); im.save(b, 'PNG', optimize=True); blobs.append(b.getvalue())
    else: blobs.append(dib(im))
out = struct.pack('<HHH', 0, 1, len(imgs)); off = 6 + 16 * len(imgs)
for (s, _), b in zip(imgs, blobs):
    out += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(b), off); off += len(b)
out += b''.join(blobs)
open(sys.argv[1], 'wb').write(out)



