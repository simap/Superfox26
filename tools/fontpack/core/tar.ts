// Reads one member of an uncompressed ustar archive into memory. Member names are compared as
// strings and never used as file paths.

function field(b: Uint8Array, off: number, len: number): string
{
    let s = "";
    for(let i = off; i < off + len && b[i] !== 0; i++) s += String.fromCharCode(b[i]);
    return s;
}

export function tarMember(tar: Uint8Array, name: string): Uint8Array
{
    let longName: string | null = null;
    for(let off = 0; off + 512 <= tar.length;)
    {
        const head = tar.subarray(off, off + 512);
        if(head.every((x) => x === 0)) break;
        const size = parseInt(field(head, 124, 12).trim() || "0", 8);
        const type = String.fromCharCode(head[156]);
        const body = off + 512;
        let path   = field(head, 0, 100);
        const pre  = field(head, 345, 155);
        if(pre && field(head, 257, 5) === "ustar") path = pre + "/" + path;
        if(longName !== null)
        {
            path     = longName;
            longName = null;
        }
        if(type === "L") longName = field(tar, body, size);
        else if(type === "x")
        {
            // pax extended header: records "LEN key=value\n", of which only path matters here
            const text = new TextDecoder().decode(tar.subarray(body, body + size));
            const m    = /(?:^|\n)\d+ path=([^\n]*)\n/.exec(text);
            if(m) longName = m[1];
        }
        else if((type === "0" || type === "\0") && path === name) return tar.slice(body, body + size);
        off = body + ((size + 511) & ~511);
    }
    throw new Error("archive member '" + name + "' not found");
}
