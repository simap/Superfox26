// Code point range lists as the configuration writes them: "U+0020-007E" or "U+2022", hexadecimal.

export function parseRanges(list: string[]): Set<number>
{
    const out = new Set<number>();
    for(const item of list)
    {
        const m = /^U\+([0-9A-Fa-f]{1,6})(?:-([0-9A-Fa-f]{1,6}))?$/.exec(item.trim());
        if(!m) throw new Error("bad code point range '" + item + "', expected U+XXXX or U+XXXX-YYYY");
        const a = parseInt(m[1], 16);
        const b = m[2] ? parseInt(m[2], 16) : a;
        if(b < a || b > 0x10ffff) throw new Error("bad code point range '" + item + "'");
        for(let c = a; c <= b; c++) out.add(c);
    }
    return out;
}

function hex(c: number): string
{
    return c.toString(16).toUpperCase().padStart(4, "0");
}

// The shortest range list for a set, for the manifest.
export function formatRanges(set: Iterable<number>): string[]
{
    const cps = [...set].sort((a, b) => a - b);
    const out: string[] = [];
    for(let i = 0; i < cps.length;)
    {
        let j = i;
        while(j + 1 < cps.length && cps[j + 1] === cps[j] + 1) j++;
        out.push(i === j ? "U+" + hex(cps[i]) : "U+" + hex(cps[i]) + "-" + hex(cps[j]));
        i = j + 1;
    }
    return out;
}
