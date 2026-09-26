"""Compile selected shadPS4/GoldHEN XML patches into out/patches.bin for the loader.

Patch addresses are PS4 virtual addresses (eboot base 0x400000); the loader's
image places eboot vaddr 0 at image offset 0. Only literal writes are supported
(bytes, bytes16/32/64, float32/64, utf8, utf16); pattern ("mask") patches are rejected.
"""
import argparse
import struct
import xml.etree.ElementTree as ET
from pathlib import Path

EBOOT_BASE=0x400000
# BB_FPS presets: patch names from patches/Bloodborne.xml (app version 01.09).
FPS_PRESETS={'30':[],'60':['60 FPS++'],'90':['90 FPS++'],'uncap':['Uncap FPS++']}
# Upscaler presets (bbport.ini "preset", the in-game menu): output / render size ratio. The game
# then renders at 1920x1080 / ratio and the port's temporal upscaler restores the output size.
OUTPUT_SIZE=(1920,1080)
PRESET_SCALES=[1.0,1.5,1.7,2.0,3.0]
# The community resolution patch this is derived from: its "mov eax/ecx, imm32" render width
# (0x500) and height (0x2D0) immediates are replaced; its other lines (UI coordinate space,
# aspect) are kept.
RESOLUTION_TEMPLATE='Resolution Patch 1280x720 (16:9)'


def read_settings(path):
    settings={}
    if path.exists():
        for line in path.read_text().splitlines():
            key,sep,value=line.partition('=')
            if sep and not line.startswith('#'): settings[key.strip()]=value.strip()
    return settings


def render_size(settings,override=''):
    """Render resolution for the upscaler preset, or None for native."""
    if override:
        w,h=(int(v) for v in override.lower().split('x'))
        return (w,h)
    if settings.get('upscaler','fsr3')=='off': return None
    preset=int(settings.get('preset','0') or 0)
    scale=PRESET_SCALES[max(0,min(preset,len(PRESET_SCALES)-1))]
    if scale==1.0: return None
    # Even sizes (the game has half-resolution buffers).
    return tuple(max(2,round(v/scale/2)*2) for v in OUTPUT_SIZE)


def resolution_writes(xml,size,app_version,segments):
    writes=compile_patches(xml,[RESOLUTION_TEMPLATE],app_version,segments)
    out=[]
    for offset,data in writes:
        if len(data)==4 and data[0] in (0xB8,0xB9):
            imm=int.from_bytes(data[1:4],'little')
            if imm==0x500: data=bytes([data[0]])+size[0].to_bytes(3,'little')
            elif imm==0x2D0: data=bytes([data[0]])+size[1].to_bytes(3,'little')
        out.append((offset,data))
    return out


def eboot_segments(elf):
    phoff,=struct.unpack_from('<Q',elf,0x20)
    phentsize,phnum=struct.unpack_from('<HH',elf,0x36)
    segments=[]
    for i in range(phnum):
        kind,_,_,vaddr,_,_,memsz,_=struct.unpack_from('<IIQQQQQQ',elf,phoff+i*phentsize)
        if kind==1: segments.append((vaddr,vaddr+memsz))
    return segments


def encode(line):
    kind,value=line.get('Type'),line.get('Value')
    if kind=='bytes': return bytes.fromhex(value.replace(' ',''))
    if kind in ('bytes16','bytes32','bytes64'):
        return int(value,0).to_bytes(int(kind[5:])//8,'little')
    if kind=='float32': return struct.pack('<f',float(value))
    if kind=='float64': return struct.pack('<d',float(value))
    if kind=='utf8': return value.encode()+b'\0'
    if kind=='utf16': return value.encode('utf-16-le')+b'\0\0'
    raise ValueError(f'unsupported patch type {kind!r}')


def compile_patches(xml, names, app_version, segments):
    found={}
    for meta in ET.parse(xml).getroot().iter('Metadata'):
        if meta.get('Name') in names and meta.get('AppVer')==app_version and meta.get('AppElf','eboot.bin')=='eboot.bin':
            found[meta.get('Name')]=meta
    missing=[n for n in names if n not in found]
    if missing: raise ValueError(f'patches not found for app version {app_version}: {missing}')
    writes=[]
    for name in names:
        for line in found[name].iter('Line'):
            offset=int(line.get('Address'),0)-EBOOT_BASE
            data=encode(line)
            if not any(start<=offset and offset+len(data)<=end for start,end in segments):
                raise ValueError(f'{name}: address {line.get("Address")} is outside the eboot')
            writes.append((offset,data))
    return writes


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--xml',type=Path,default=Path(__file__).parent/'patches/Bloodborne.xml')
    p.add_argument('--fps',choices=sorted(FPS_PRESETS),default='uncap')
    p.add_argument('--extra',default='',help='additional patch names, separated by ";"')
    p.add_argument('--app-version',default='01.09')
    p.add_argument('--out',type=Path,default=Path(__file__).parent/'out')
    p.add_argument('--settings',type=Path,default=Path(__file__).parent/'bbport.ini')
    p.add_argument('--render-res',default='',help='render resolution WxH (overrides the preset)')
    a=p.parse_args()
    names=FPS_PRESETS[a.fps]+[n.strip() for n in a.extra.split(';') if n.strip()]
    segments=eboot_segments((a.out/'eboot.elf').read_bytes())
    writes=compile_patches(a.xml,names,a.app_version,segments)
    size=render_size(read_settings(a.settings),a.render_res)
    if size:
        writes+=resolution_writes(a.xml,size,a.app_version,segments)
        print(f'Patches: render resolution {size[0]}x{size[1]} (upscaled to {OUTPUT_SIZE[0]}x{OUTPUT_SIZE[1]})')
    blob=struct.pack('<8sQ',b'BBPATCH1',len(writes))
    for offset,data in writes: blob+=struct.pack('<QQ',offset,len(data))+data
    (a.out/'patches.bin').write_bytes(blob)
    print(f'Patches: FPS preset {a.fps}; {len(writes)} writes from {names or "none"}')


if __name__=='__main__':
    main()
