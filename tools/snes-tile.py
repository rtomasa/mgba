import numpy as np
import PIL.Image
import sys
import struct

X_FLIP = 0x4000
Y_FLIP = 0x8000
PAL = 0x1000

im = PIL.Image.open(sys.argv[1])
tiles = np.array(np.split(np.array(np.split(np.array(im), 28, 0)), 32, 2))
tilemapRaw = bytearray(32 * 28 * 2)
tilemap = np.ndarray(shape=(28, 32), dtype='<u2', buffer=tilemapRaw)
tiledata = [[0] * 64]

for tid in np.mgrid[:32, :28].reshape(2, -1).T:
    tile = tiles[tuple(tid)]
    tid = tuple(np.flip(tid))
    data = list(tile.flatten())
    try:
        index = tiledata.index(data)
        tilemap[tid] = index | PAL
        continue
    except:
        pass

    data = list(np.flip(tile, 0).flatten())
    try:
        index = tiledata.index(data)
        tilemap[tid] = index | PAL | Y_FLIP
        continue
    except:
        pass

    data = list(np.flip(tile, 1).flatten())
    try:
        index = tiledata.index(data)
        tilemap[tid] = index | PAL | X_FLIP
        continue
    except:
        pass

    data = list(np.flip(np.flip(tile, 0), 1).flatten())
    try:
        index = tiledata.index(data)
        tilemap[tid] = index | PAL | X_FLIP | Y_FLIP
        continue
    except:
        pass

    data = list(tile.flatten())
    index = len(tiledata)
    tilemap[tid] = index | PAL
    tiledata.append(data)

chardataRaw = bytearray(len(tiledata) * 32)

for i, tile in enumerate(tiledata):
    for y in range(8):
        tileData = [0] * 4
        for x in range(8):
            t = tile[y * 8 + x]
            tileData[0] |= ((t & 1) >> 0) << (7 - x)
            tileData[1] |= ((t & 2) >> 1) << (7 - x)
            tileData[2] |= ((t & 4) >> 2) << (7 - x)
            tileData[3] |= ((t & 8) >> 3) << (7 - x)
        tileBase = i * 32 + y * 2
        #print(tileBase)
        chardataRaw[tileBase + 0x00] = tileData[0]
        chardataRaw[tileBase + 0x01] = tileData[1]
        chardataRaw[tileBase + 0x10] = tileData[2]
        chardataRaw[tileBase + 0x11] = tileData[3]

def printMem(mem):  
    for i in range(len(mem) // 16):
        line = mem[i * 16:(i + 1) * 16]
        print(', '.join([f'0x{b:02X}' for b in line]) + ',')

'''print('TILE MAP')
print('='*8)
printMem(tilemapRaw)
print('CHAR DATA')
print('='*9)
printMem(chardataRaw)'''

def to_argb1555(r, g, b, a):
    r = (r >> 3) & 31
    g = (g >> 3) & 31
    b = (b >> 3) & 31
    if (a == 0):
        return 1 << 15
    else:
        return (r << 10) + (g << 5) + (b << 0)

palette = set()

for (count, color) in im.convert('RGBA').getcolors():
    c = to_argb1555(*color)
    c = "".join(struct.pack("<H", c))
    palette.add(c)

#palette = list(reversed(sorted(palette)))
palette = list(palette)
print("Used", len(palette), "colors out of 16.")
print(palette)

