// The font pack core: from source font bytes and the configuration to the output files, the
// manifest and a report. No file system, network or Node API, so it can run in a browser. The
// shell (fontpack.ts) loads the inputs and writes the outputs.

import {formatRanges, parseRanges} from "./ranges.ts";
import {cmapCodepoints, decodeName, encodeName, numGlyphs, readNames, readSfnt, writeNames, writeSfnt} from "./sfnt.ts";
import type {NameRecord} from "./sfnt.ts";
import type {Subsetter} from "./subset.ts";

export interface SourceConfig
{
    url?: string; // a pinned release download
    path?: string; // or a local file, relative to the configuration file
    sha256?: string; // required with url, checked with path when given
    archive: "tar.gz" | "none";
    license: string;
    // names the source's licence reserves: no output name record may contain them (OFL section 3)
    reservedNames?: string[];
}

export interface FaceConfig
{
    slot: string;
    file: string;
    source: string;
    member?: string; // file inside the archive
    ranges: string; // a key of PackConfig.ranges
    // code points that every one of these faces maps are left out
    excludeCoveredByAll?: string[];
    hinting: boolean;
    dropTables: string[];
    nameIds: number[] | "all";
    // a new family name, required when the source declares a Reserved Font Name
    rename?: {family: string};
}

export interface LicenseConfig
{
    file: string;
    source: string;
    member?: string;
    header?: string[]; // lines written before the licence text
    copyrightOf?: string; // slot whose copyright notice (name ID 0) follows the header
}

export interface PackConfig
{
    output: string;
    cache: string;
    ranges: Record<string, string[]>;
    sources: Record<string, SourceConfig>;
    faces: FaceConfig[];
    licenses: LicenseConfig[];
}

export interface FaceResult
{
    slot: string;
    file: string;
    size: number;
    sha256: string;
    glyphs: number;
    codepoints: number;
    requested: number;
    missing: number; // requested code points the source does not map
    family: string;
    style: string;
    source: {
        name: string;
        url?: string;
        path?: string;
        sha256?: string;
        member?: string;
        fileSha256: string;
        family: string;
        version: string;
        license: string;
    };
    options: {
        ranges: string;
        rangeList: string[];
        excludeCoveredByAll?: string[];
        hinting: boolean;
        dropTables: string[];
        nameIds: number[] | "all";
        rename?: {family: string};
    };
    // the code points the subset maps, and those the ranges ask for that the source lacks
    mapped: string[];
    notInSource: string[];
}

export interface PackResult
{
    files: Map<string, Uint8Array>;
    manifest: unknown;
    report: string[];
}

// The key under which the shell passes a source file's bytes.
export function inputKey(source: string, member?: string): string
{
    return member ? source + ":" + member : source;
}

// Every input the configuration needs, as [key, source, member].
export function requiredInputs(cfg: PackConfig): [string, string, string | undefined][]
{
    const seen = new Map<string, [string, string, string | undefined]>();
    for(const x of [...cfg.faces, ...cfg.licenses]) seen.set(inputKey(x.source, x.member), [inputKey(x.source, x.member), x.source, x.member]);
    return [...seen.values()];
}

export async function sha256Hex(b: Uint8Array): Promise<string>
{
    const d = new Uint8Array(await crypto.subtle.digest("SHA-256", b));
    return [...d].map((x) => x.toString(16).padStart(2, "0")).join("");
}

function nameOf(recs: NameRecord[], id: number): string
{
    const r = recs.find((x) => x.nameId === id && x.platform === 3) ?? recs.find((x) => x.nameId === id);
    return r ? decodeName(r) : "";
}

// Replaces the family in name IDs 1, 3, 4, 6, 16 and 17 and keeps the style of ID 2.
function renameRecords(recs: NameRecord[], family: string, version: string): NameRecord[]
{
    const style  = nameOf(recs, 2) || "Regular";
    const full   = style === "Regular" ? family : family + " " + style;
    const ps     = family.replace(/[^A-Za-z0-9]/g, "") + (style === "Regular" ? "" : "-" + style.replace(/[^A-Za-z0-9]/g, ""));
    const values = new Map<number, string>([
        [1, family], [3, full + "; " + version + "; subset"], [4, full], [6, ps], [16, family], [17, style],
    ]);
    return recs.map((r) => values.has(r.nameId) ? {...r, bytes: encodeName(r.platform, values.get(r.nameId)!)} : r);
}

function checkReserved(recs: NameRecord[], reserved: string[], file: string): void
{
    // ID 0 is the copyright notice, which the licence requires to stay. 7 is a trademark notice,
    // 13 and 14 the licence.
    for(const r of recs)
    {
        if([0, 7, 13, 14].includes(r.nameId)) continue;
        const text = decodeName(r);
        for(const n of reserved)
            if(text.toLowerCase().includes(n.toLowerCase()))
                throw new Error(file + ": name ID " + r.nameId + " '" + text + "' contains the Reserved Font Name " + n);
    }
}

// JSON with two space indents and arrays of numbers or strings on one line.
function toJson(v: unknown, ind: string): string
{
    if(Array.isArray(v))
    {
        if(v.every((x) => typeof x !== "object" || x === null)) return JSON.stringify(v).replaceAll('","', '", "');
        return "[\n" + v.map((x) => ind + "  " + toJson(x, ind + "  ")).join(",\n") + "\n" + ind + "]";
    }
    if(v && typeof v === "object")
    {
        const ents = Object.entries(v).filter(([, x]) => x !== undefined);
        return "{\n" + ents.map(([k, x]) => ind + "  " + JSON.stringify(k) + ": " + toJson(x, ind + "  ")).join(",\n") +
               "\n" + ind + "}";
    }
    return JSON.stringify(v);
}

export async function buildPack(cfg: PackConfig, inputs: Map<string, Uint8Array>, hb: Subsetter,
                                tool: Record<string, string>): Promise<PackResult>
{
    const files   = new Map<string, Uint8Array>();
    const covered = new Map<string, Set<number>>();
    const faces: FaceResult[] = [];
    const report: string[]    = [];
    for(const f of cfg.faces)
    {
        const src = cfg.sources[f.source];
        if(!src) throw new Error(f.slot + ": unknown source " + f.source);
        const rangeList = cfg.ranges[f.ranges];
        if(!rangeList) throw new Error(f.slot + ": unknown range set " + f.ranges);
        const input = inputs.get(inputKey(f.source, f.member));
        if(!input) throw new Error(f.slot + ": no input for " + inputKey(f.source, f.member));
        if(files.has(f.file)) throw new Error(f.slot + ": file " + f.file + " appears twice");

        const srcFont  = readSfnt(input);
        const srcNames = readNames(srcFont);
        const srcCmap  = cmapCodepoints(srcFont);
        const want     = parseRanges(rangeList);
        if(f.excludeCoveredByAll)
        {
            const sets = f.excludeCoveredByAll.map((s) =>
            {
                const c = covered.get(s);
                if(!c) throw new Error(f.slot + ": excludeCoveredByAll names " + s + ", which is not built before it");
                return c;
            });
            for(const c of [...want])
                if(sets.every((s) => s.has(c))) want.delete(c);
        }
        const requested = [...want].filter((c) => srcCmap.has(c));
        const reserved  = src.reservedNames ?? [];
        if(reserved.length && !f.rename)
            throw new Error(f.slot + ": source " + f.source + " declares Reserved Font Names " + reserved.join(", ") +
                            ", so a subset needs rename");

        let out  = hb.subset(input, {
            unicodes: requested,
            hinting: f.hinting,
            dropTables: f.dropTables,
            nameIds: f.nameIds,
            allNameLanguages: false,
            layoutClosure: !f.dropTables.includes("GSUB"),
        });
        let font = readSfnt(out);
        let recs = readNames(font);
        if(f.rename)
        {
            recs = renameRecords(recs, f.rename.family, nameOf(srcNames, 5));
            font.tables.set("name", writeNames(recs));
            out  = writeSfnt(font);
            font = readSfnt(out);
        }
        checkReserved(recs, reserved, f.file);
        const mapped = cmapCodepoints(font);
        for(const c of requested)
            if(!mapped.has(c)) throw new Error(f.file + ": U+" + c.toString(16).toUpperCase() + " was asked for and is not in the subset");
        covered.set(f.slot, mapped);
        files.set(f.file, out);
        const r: FaceResult = {
            slot: f.slot,
            file: f.file,
            size: out.length,
            sha256: await sha256Hex(out),
            glyphs: numGlyphs(font),
            codepoints: mapped.size,
            requested: want.size,
            missing: want.size - requested.length,
            family: nameOf(recs, 1),
            style: nameOf(recs, 2),
            source: {
                name: f.source,
                url: src.url,
                path: src.path,
                sha256: src.sha256,
                member: f.member,
                fileSha256: await sha256Hex(input),
                family: nameOf(srcNames, 1),
                version: nameOf(srcNames, 5),
                license: src.license,
            },
            options: {
                ranges: f.ranges,
                rangeList,
                excludeCoveredByAll: f.excludeCoveredByAll,
                hinting: f.hinting,
                dropTables: f.dropTables,
                nameIds: f.nameIds,
                rename: f.rename,
            },
            mapped: formatRanges(mapped),
            notInSource: formatRanges([...want].filter((c) => !srcCmap.has(c))),
        };
        faces.push(r);
        report.push([r.slot.padEnd(18), r.file.padEnd(13), String(r.size).padStart(7) + " bytes",
                     String(r.glyphs).padStart(5) + " glyphs", String(r.codepoints).padStart(5) + " code points",
                     r.missing ? r.missing + " asked for and not in the source" : ""].join("  ").trimEnd());
    }

    const licenses: {file: string; size: number; sha256: string; source: string; member?: string}[] = [];
    for(const l of cfg.licenses)
    {
        const text = inputs.get(inputKey(l.source, l.member));
        if(!text) throw new Error(l.file + ": no input for " + inputKey(l.source, l.member));
        let head = (l.header ?? []).join("\n");
        if(l.copyrightOf)
        {
            const face = files.get(cfg.faces.find((f) => f.slot === l.copyrightOf)?.file ?? "");
            if(!face) throw new Error(l.file + ": copyrightOf names " + l.copyrightOf + ", which is not a face");
            head += (head ? "\n" : "") + nameOf(readNames(readSfnt(face)), 0);
        }
        const body = head ? new TextEncoder().encode(head + "\n\n") : new Uint8Array(0);
        const out  = new Uint8Array(body.length + text.length);
        out.set(body);
        out.set(text, body.length);
        if(files.has(l.file)) throw new Error(l.file + " appears twice");
        files.set(l.file, out);
        licenses.push({file: l.file, size: out.length, sha256: await sha256Hex(out), source: l.source, member: l.member});
    }

    let total = 0;
    for(const b of files.values()) total += b.length;
    report.push("total " + files.size + " files, " + total + " bytes");
    const manifest = {
        generator: tool,
        totalBytes: total,
        faces,
        licenses,
    };
    files.set("manifest.json", new TextEncoder().encode(toJson(manifest, "") + "\n"));
    return {files, manifest, report};
}
