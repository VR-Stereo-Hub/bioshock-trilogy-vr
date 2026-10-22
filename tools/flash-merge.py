# flash-merge.py - put the symbols a BS1 UI movie imports INTO it, so FFDec renders it whole.
#
#   ffdec-cli -swf2xml <decoded>\sharedlibrary.swf shared.xml
#   ffdec-cli -swf2xml <decoded>\HUDRadial.swf radial.xml
#   py tools\flash-merge.py shared.xml radial.xml merged.xml
#   ffdec-cli -xml2swf merged.xml HUDRadial_merged.swf
#
# Decode both movies with tools\flash-decode.py first (FFDec cannot draw BS1's DXT
# bitmaps). FFDec 26.2.1 does not follow ImportAssets when it renders, so a HUD
# movie's frame comes out without its bars, frames and icons - they live in
# sharedlibrary.swf. This copies every imported symbol, and everything those
# symbols reference, from the library into the movie (library ids kept), moves
# the movie's own ids up by 4000 so nothing collides, and drops the imports.
# In the merged movie a root clip's id is its original id + 4000.
#
# Works on FFDec's XML (2.2): ids are the attributes named in REF below. Output is
# game-derived: model workspace only, never the repo.
import sys
import copy
import xml.etree.ElementTree as ET

OFFSET = 4000
REF = ('characterId', 'characterID', 'spriteId', 'shapeId', 'bitmapId', 'fontId', 'fontID', 'buttonId', 'textID')
NOT_DEFINITIONS = ('DefineFontNameTag', 'DefineFontAlignZonesTag', 'DefineScalingGridTag',
                   'DefineButtonCxformTag', 'DefineButtonSoundTag')


def defined_id(item):
    t = item.get('type', '')
    if not t.startswith('Define') or t in NOT_DEFINITIONS:
        return None
    for k in ('spriteId', 'shapeId', 'characterID', 'characterId', 'fontId', 'fontID', 'buttonId'):
        if item.get(k) is not None:
            return int(item.get(k))
    return None


def refs(el):
    for e in el.iter():
        for k in REF:
            v = e.get(k)
            if v is not None and v != '65535':  # 65535 = no bitmap
                yield int(v)


def main():
    shared_xml, movie_xml, out_xml = sys.argv[1:4]
    stags = ET.parse(shared_xml).getroot().find('tags')
    sdef, sorder, sexp = {}, {}, {}
    for i, it in enumerate(stags):
        d = defined_id(it)
        if d is not None:
            sdef[d], sorder[d] = it, i
        if it.get('type') == 'ExportAssetsTag':
            for a, b in zip(it.find('tags'), it.find('names')):
                sexp[b.text] = int(a.text)

    root = ET.parse(movie_xml).getroot()
    mtags = root.find('tags')
    imap, missing = {}, []
    for it in mtags:
        if it.get('type') == 'ImportAssetsTag':
            for a, b in zip(it.find('tags'), it.find('names')):
                if b.text in sexp:
                    imap[int(a.text)] = sexp[b.text]
                else:
                    missing.append(b.text)

    need, todo = set(imap.values()), list(imap.values())
    while todo:
        c = todo.pop()
        for r in refs(sdef[c]) if c in sdef else ():
            if r not in need and r in sdef:
                need.add(r)
                todo.append(r)
    names = [it for it in stags if it.get('type') == 'DefineFontNameTag' and int(it.get('fontId', -1)) in need]

    def remap(n):
        return imap[n] if n in imap else n + OFFSET

    kept = []
    for it in mtags:
        if it.get('type') == 'ImportAssetsTag':
            continue
        for e in it.iter():
            for k in REF:
                v = e.get(k)
                if v is not None and v != '65535':
                    e.set(k, str(remap(int(v))))
        if it.get('type') == 'ExportAssetsTag':
            for a in it.find('tags'):
                a.text = str(remap(int(a.text)))
        kept.append(it)
    defs = [copy.deepcopy(sdef[c]) for c in sorted(need, key=lambda c: sorder[c])] + \
        [copy.deepcopy(e) for e in names]
    first = 1 if kept and kept[0].get('type') == 'SetBackgroundColorTag' else 0
    for c in list(mtags):
        mtags.remove(c)
    for c in kept[:first] + defs + kept[first:]:
        mtags.append(c)
    ET.ElementTree(root).write(out_xml, encoding='UTF-8', xml_declaration=True)
    print(f'{len(imap)} import(s) resolved, {len(need)} library symbol(s) copied, '
          f'{len(missing)} not exported by the library: {", ".join(missing) or "-"}')


if __name__ == '__main__':
    main()
