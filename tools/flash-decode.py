# flash-decode.py - make a BS1 Scaleform movie renderable in FFDec, offline.
#
#   py tools\flash-decode.py <in.swf> <out.swf> [--bitmaps <dir>] [--local-imports]
#
# BS1's UI art is not in standard SWF bitmap tags. The movies carry it in a
# gameswf custom tag (code 512: u16 id, u16 width, u16 height, u16 format,
# u16 0, then the payload; format 0 = DXT1, 4 = DXT5, no mips), which FFDec
# does not know and draws as solid red. This rewrites each one as a standard
# DefineBitsLossless2 (premultiplied ARGB) with the same character id, so the
# shapes that fill from it render with the real art. --bitmaps also writes each
# decoded image as a PNG. --local-imports points ImportAssets at a sibling
# "sharedlibrary.swf" (HUDPC imports 93 symbols from "..\FlashMovies\
# sharedlibrary.swf", a path FFDec does not resolve) - decode sharedlibrary.swf
# into the same folder.
#
# Measured on BS1 Remastered (2026-10-15): sharedlibrary.swf 73 bitmaps (70 DXT5,
# 3 DXT1), HUDPC.swf 14. Everything this writes is game-derived: keep it in the
# model workspace (tools\tool-paths.ps1 model_workspace), never in the repo.
# Needs numpy; --bitmaps also needs Pillow.
import argparse, struct, sys, zlib

import numpy as np


def tags(buf):
    if buf[:3] != b'FWS':
        sys.exit('not an uncompressed SWF (FWS)')
    nbits = buf[8] >> 3
    p = 8 + (5 + 4 * nbits + 7) // 8 + 4
    while p < len(buf):
        code_len = struct.unpack_from('<H', buf, p)[0]
        code, ln, hp = code_len >> 6, code_len & 0x3f, p
        p += 2
        if ln == 0x3f:
            ln = struct.unpack_from('<I', buf, p)[0]
            p += 4
        yield code, hp, p, ln
        p += ln
        if code == 0:
            break


def rgb565(c):
    return np.stack([((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31],
                    -1).astype(np.int32)


def colour_blocks(b, dxt1):
    c0 = b[:, 0].astype(np.uint32) | (b[:, 1].astype(np.uint32) << 8)
    c1 = b[:, 2].astype(np.uint32) | (b[:, 3].astype(np.uint32) << 8)
    a, z = rgb565(c0), rgb565(c1)
    pal = np.zeros((len(c0), 4, 4), np.int32)
    pal[:, 0, :3], pal[:, 1, :3], pal[:, :, 3] = a, z, 255
    four = (c0 > c1) | (not dxt1)
    pal[:, 2, :3] = np.where(four[:, None], (2 * a + z) // 3, (a + z) // 2)
    pal[:, 3, :3] = np.where(four[:, None], (a + 2 * z) // 3, 0)
    if dxt1:
        pal[:, 3, 3] = np.where(four, 255, 0)
    idx = b[:, 4].astype(np.uint32) | (b[:, 5].astype(np.uint32) << 8) | \
        (b[:, 6].astype(np.uint32) << 16) | (b[:, 7].astype(np.uint32) << 24)
    sel = np.stack([(idx >> (2 * i)) & 3 for i in range(16)], 1)
    return np.take_along_axis(pal, sel[:, :, None].repeat(4, 2), 1)


def alpha_blocks(b):
    a0, a1 = b[:, 0].astype(np.int32), b[:, 1].astype(np.int32)
    pal = np.zeros((len(a0), 8), np.int32)
    pal[:, 0], pal[:, 1] = a0, a1
    big = a0 > a1
    for i in range(1, 7):
        six = ((7 - i) * a0 + i * a1) // 7
        four = ((5 - i) * a0 + i * a1) // 5 if i < 5 else (np.zeros_like(a0) if i == 5 else np.full_like(a0, 255))
        pal[:, i + 1] = np.where(big, six, four)
    bits = np.zeros(len(a0), np.uint64)
    for i in range(6):
        bits |= b[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
    sel = np.stack([((bits >> np.uint64(3 * i)) & np.uint64(7)).astype(np.int64) for i in range(16)], 1)
    return np.take_along_axis(pal, sel, 1)


def decode(data, w, h, fmt):
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    if fmt == 0:
        px = colour_blocks(np.frombuffer(data, np.uint8, bw * bh * 8).reshape(-1, 8), True)
    elif fmt == 4:
        blk = np.frombuffer(data, np.uint8, bw * bh * 16).reshape(-1, 16)
        px = colour_blocks(blk[:, 8:], False)
        px[:, :, 3] = alpha_blocks(blk[:, :8])
    else:
        return None
    img = px.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    return img[:h, :w].astype(np.uint8)


def lossless2(cid, img):
    a = img[:, :, 3:4].astype(np.uint16)
    rgb = (img[:, :, :3].astype(np.uint16) * a // 255).astype(np.uint8)
    body = struct.pack('<HBHH', cid, 5, img.shape[1], img.shape[0]) + \
        zlib.compress(np.concatenate([img[:, :, 3:4], rgb], 2).tobytes(), 6)
    return struct.pack('<HI', (36 << 6) | 0x3f, len(body)) + body


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('dst')
    ap.add_argument('--bitmaps')
    ap.add_argument('--local-imports', action='store_true')
    args = ap.parse_args()
    buf = open(args.src, 'rb').read()
    out, last, n, imports, skipped = bytearray(), 0, 0, 0, 0
    for code, hp, p, ln in tags(buf):
        if code == 512:
            cid, w, h, fmt = struct.unpack_from('<HHHH', buf, p)
            img = decode(buf[p + 10:p + ln], w, h, fmt)
            if img is None:
                skipped += 1
                continue
            out += buf[last:hp] + lossless2(cid, img)
            last, n = p + ln, n + 1
            if args.bitmaps:
                from PIL import Image
                Image.fromarray(img, 'RGBA').save(f'{args.bitmaps}/bmp_{cid}_{w}x{h}.png')
        elif code == 57 and args.local_imports:
            end = buf.index(b'\0', p)
            url = buf[p:end]
            name = url.replace(b'\\', b'/').rsplit(b'/', 1)[-1]
            if name != url:
                body = name + buf[end:p + ln]
                out += buf[last:hp] + struct.pack('<HI', (57 << 6) | 0x3f, len(body)) + body
                last, imports = p + ln, imports + 1
    out += buf[last:]
    struct.pack_into('<I', out, 4, len(out))
    open(args.dst, 'wb').write(out)
    print(f'{args.src}: {n} bitmap(s) decoded, {skipped} of an unknown format left, {imports} import(s) made local')


if __name__ == '__main__':
    main()
