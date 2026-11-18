// Reads and writes the sfnt container (TrueType and OpenType files): the table directory, cmap
// coverage, maxp glyph count and the name table. Pure functions on byte arrays, no I/O.

export interface Sfnt
{
    version: number; // 0x00010000 for TrueType outlines, 'OTTO' for CFF
    tables: Map<string, Uint8Array>;
}

export interface NameRecord
{
    platform: number;
    encoding: number;
    language: number;
    nameId: number;
    bytes: Uint8Array; // encoded as the platform requires
}

function view(b: Uint8Array): DataView
{
    return new DataView(b.buffer, b.byteOffset, b.byteLength);
}

function tagAt(d: DataView, off: number): string
{
    return String.fromCharCode(d.getUint8(off), d.getUint8(off + 1), d.getUint8(off + 2), d.getUint8(off + 3));
}

export function readSfnt(bytes: Uint8Array): Sfnt
{
    const d = view(bytes);
    if(bytes.length < 12) throw new Error("not an sfnt file: shorter than its header");
    const version = d.getUint32(0);
    if(version !== 0x00010000 && version !== 0x4f54544f && version !== 0x74727565)
        throw new Error("not an sfnt file: version 0x" + version.toString(16));
    const n      = d.getUint16(4);
    const tables = new Map<string, Uint8Array>();
    for(let i = 0; i < n; i++)
    {
        const rec = 12 + i * 16;
        const tag = tagAt(d, rec);
        const off = d.getUint32(rec + 8);
        const len = d.getUint32(rec + 12);
        if(off + len > bytes.length) throw new Error("table " + tag + " runs past the end of the file");
        tables.set(tag, bytes.subarray(off, off + len));
    }
    return {version, tables};
}

function checksum(b: Uint8Array): number
{
    let sum      = 0;
    const padded = b.length & 3 ? new Uint8Array((b.length + 3) & ~3) : b;
    if(padded !== b) padded.set(b);
    const d = view(padded);
    for(let i = 0; i < padded.length; i += 4) sum = (sum + d.getUint32(i)) >>> 0;
    return sum;
}

// Writes the tables in tag order, each 4 byte aligned, with their checksums and head's
// checkSumAdjustment.
export function writeSfnt(s: Sfnt): Uint8Array
{
    const tags = [...s.tables.keys()].sort();
    const n    = tags.length;
    let es     = 0;
    while((1 << (es + 1)) <= n) es++;
    const searchRange = (1 << es) * 16;
    let size          = 12 + n * 16;
    const offsets: number[] = [];
    for(const t of tags)
    {
        offsets.push(size);
        size += (s.tables.get(t)!.length + 3) & ~3;
    }
    const out = new Uint8Array(size);
    const d   = view(out);
    d.setUint32(0, s.version);
    d.setUint16(4, n);
    d.setUint16(6, searchRange);
    d.setUint16(8, es);
    d.setUint16(10, n * 16 - searchRange);
    let headOff = -1;
    tags.forEach((t, i) =>
    {
        let data = s.tables.get(t)!;
        if(t === "head")
        {
            data = data.slice();
            view(data).setUint32(8, 0);
            headOff = offsets[i];
        }
        const rec = 12 + i * 16;
        for(let k = 0; k < 4; k++) d.setUint8(rec + k, t.charCodeAt(k));
        d.setUint32(rec + 4, checksum(data));
        d.setUint32(rec + 8, offsets[i]);
        d.setUint32(rec + 12, data.length);
        out.set(data, offsets[i]);
    });
    if(headOff >= 0) d.setUint32(headOff + 8, (0xb1b0afba - checksum(out)) >>> 0);
    return out;
}

export function numGlyphs(s: Sfnt): number
{
    const maxp = s.tables.get("maxp");
    return maxp ? view(maxp).getUint16(4) : 0;
}

// Code points of the best Unicode cmap subtable: format 12 of (3,10) or (0,4..6), else format 4 of
// (3,1) or (0,0..3).
export function cmapCodepoints(s: Sfnt): Set<number>
{
    const out  = new Set<number>();
    const cmap = s.tables.get("cmap");
    if(!cmap) return out;
    const d = view(cmap);
    const n = d.getUint16(2);
    let best12 = -1, best4 = -1;
    for(let i = 0; i < n; i++)
    {
        const p   = d.getUint16(4 + i * 8);
        const e   = d.getUint16(6 + i * 8);
        const off = d.getUint32(8 + i * 8);
        const fmt = d.getUint16(off);
        if(fmt === 12 && ((p === 3 && e === 10) || (p === 0 && e >= 4))) best12 = off;
        if(fmt === 4 && ((p === 3 && e === 1) || (p === 0 && e <= 3))) best4 = off;
    }
    if(best12 >= 0)
    {
        const groups = d.getUint32(best12 + 12);
        for(let g = 0; g < groups; g++)
        {
            const r     = best12 + 16 + g * 12;
            const start = d.getUint32(r), end = d.getUint32(r + 4), gid = d.getUint32(r + 8);
            for(let c = start; c <= end; c++)
                if(gid + (c - start) !== 0) out.add(c);
        }
        return out;
    }
    if(best4 >= 0)
    {
        const segX2   = d.getUint16(best4 + 6);
        const ends    = best4 + 14;
        const starts  = ends + segX2 + 2;
        const deltas  = starts + segX2;
        const rangeOf = deltas + segX2;
        for(let i = 0; i < segX2 / 2; i++)
        {
            const end = d.getUint16(ends + i * 2), start = d.getUint16(starts + i * 2);
            const delta = d.getUint16(deltas + i * 2), ro = d.getUint16(rangeOf + i * 2);
            for(let c = start; c <= end && c !== 0xffff; c++)
            {
                let gid: number;
                if(ro === 0) gid = (c + delta) & 0xffff;
                else
                {
                    const at = rangeOf + i * 2 + ro + (c - start) * 2;
                    gid      = d.getUint16(at);
                    if(gid !== 0) gid = (gid + delta) & 0xffff;
                }
                if(gid !== 0) out.add(c);
            }
        }
    }
    return out;
}

export function readNames(s: Sfnt): NameRecord[]
{
    const name = s.tables.get("name");
    if(!name) return [];
    const d       = view(name);
    const count   = d.getUint16(2);
    const storage = d.getUint16(4);
    const out: NameRecord[] = [];
    for(let i = 0; i < count; i++)
    {
        const r   = 6 + i * 12;
        const len = d.getUint16(r + 8), off = d.getUint16(r + 10);
        out.push({
            platform: d.getUint16(r),
            encoding: d.getUint16(r + 2),
            language: d.getUint16(r + 4),
            nameId: d.getUint16(r + 6),
            bytes: name.slice(storage + off, storage + off + len),
        });
    }
    return out;
}

// A version 0 name table, records sorted as the format requires.
export function writeNames(records: NameRecord[]): Uint8Array
{
    const recs = [...records].sort((a, b) =>
        a.platform - b.platform || a.encoding - b.encoding || a.language - b.language || a.nameId - b.nameId);
    const storage = 6 + recs.length * 12;
    let strLen    = 0;
    for(const r of recs) strLen += r.bytes.length;
    const out = new Uint8Array(storage + strLen);
    const d   = view(out);
    d.setUint16(0, 0);
    d.setUint16(2, recs.length);
    d.setUint16(4, storage);
    let off = 0;
    recs.forEach((r, i) =>
    {
        const at = 6 + i * 12;
        d.setUint16(at, r.platform);
        d.setUint16(at + 2, r.encoding);
        d.setUint16(at + 4, r.language);
        d.setUint16(at + 6, r.nameId);
        d.setUint16(at + 8, r.bytes.length);
        d.setUint16(at + 10, off);
        out.set(r.bytes, storage + off);
        off += r.bytes.length;
    });
    return out;
}

// Unicode (platform 0) and Windows (platform 3) records are UTF-16BE. Macintosh Roman (1, 0) is
// decoded as Latin-1, which matches it for ASCII.
export function decodeName(r: NameRecord): string
{
    if(r.platform === 0 || r.platform === 3)
    {
        let s = "";
        for(let i = 0; i + 1 < r.bytes.length; i += 2) s += String.fromCharCode((r.bytes[i] << 8) | r.bytes[i + 1]);
        return s;
    }
    return String.fromCharCode(...r.bytes);
}

export function encodeName(platform: number, text: string): Uint8Array
{
    if(platform === 0 || platform === 3)
    {
        const out = new Uint8Array(text.length * 2);
        for(let i = 0; i < text.length; i++)
        {
            out[i * 2]     = text.charCodeAt(i) >> 8;
            out[i * 2 + 1] = text.charCodeAt(i) & 0xff;
        }
        return out;
    }
    for(let i = 0; i < text.length; i++)
        if(text.charCodeAt(i) > 0x7e) throw new Error("name '" + text + "' is not ASCII, the Macintosh record needs it");
    return new Uint8Array([...text].map((c) => c.charCodeAt(0)));
}
