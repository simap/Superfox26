// HarfBuzz's subsetter from harfbuzzjs (hb-subset compiled to WebAssembly, harfbuzz-subset.wasm),
// called through its raw C exports. The module has no imports, so the same code runs in Node and
// in a browser. The caller supplies the wasm bytes.

// hb-subset.h
const HB_MEMORY_MODE_READONLY       = 1;
const HB_SUBSET_FLAGS_NO_HINTING    = 0x1;
const HB_SUBSET_FLAGS_NOTDEF_OUTLINE = 0x40;
const HB_SUBSET_FLAGS_NO_LAYOUT_CLOSURE = 0x200;
const HB_SUBSET_SETS_DROP_TABLE_TAG = 3;
const HB_SUBSET_SETS_NAME_ID        = 4;
const HB_SUBSET_SETS_NAME_LANG_ID   = 5;
const HB_SUBSET_SETS_LAYOUT_FEATURE_TAG = 6;

export interface SubsetOptions
{
    unicodes: Iterable<number>;
    // false strips TrueType instructions, fpgm, prep, cvt and the hint tables
    hinting: boolean;
    // table tags removed from the output, in addition to HarfBuzz's own default list
    dropTables: string[];
    // name IDs kept, "all" for every record
    nameIds: number[] | "all";
    // keep name records in every language, else US English only
    allNameLanguages: boolean;
    // glyphs reachable through GSUB from the kept ones are kept too
    layoutClosure: boolean;
}

interface HbExports
{
    memory: WebAssembly.Memory;
    malloc(n: number): number;
    free(p: number): void;
    _initialize(): void;
    hb_blob_create(data: number, len: number, mode: number, user: number, destroy: number): number;
    hb_blob_destroy(b: number): void;
    hb_blob_get_data(b: number, len: number): number;
    hb_blob_get_length(b: number): number;
    hb_face_create(blob: number, index: number): number;
    hb_face_destroy(f: number): void;
    hb_face_reference_blob(f: number): number;
    hb_set_add(s: number, v: number): void;
    hb_set_clear(s: number): void;
    hb_set_invert(s: number): void;
    hb_subset_input_create_or_fail(): number;
    hb_subset_input_destroy(i: number): void;
    hb_subset_input_unicode_set(i: number): number;
    hb_subset_input_set(i: number, which: number): number;
    hb_subset_input_get_flags(i: number): number;
    hb_subset_input_set_flags(i: number, f: number): void;
    hb_subset_or_fail(face: number, input: number): number;
}

export interface Subsetter
{
    subset(font: Uint8Array, o: SubsetOptions): Uint8Array;
}

function hbTag(t: string): number
{
    if(t.length !== 4) throw new Error("table tag '" + t + "' is not 4 characters");
    return ((t.charCodeAt(0) << 24) | (t.charCodeAt(1) << 16) | (t.charCodeAt(2) << 8) | t.charCodeAt(3)) >>> 0;
}

export async function createSubsetter(wasm: BufferSource): Promise<Subsetter>
{
    const {instance} = await WebAssembly.instantiate(wasm, {});
    const hb         = instance.exports as unknown as HbExports;
    hb._initialize();
    return {
        subset(font: Uint8Array, o: SubsetOptions): Uint8Array
        {
            const input = hb.hb_subset_input_create_or_fail();
            if(!input) throw new Error("hb_subset_input_create_or_fail failed");
            const buf = hb.malloc(font.length);
            new Uint8Array(hb.memory.buffer, buf, font.length).set(font);
            const blob = hb.hb_blob_create(buf, font.length, HB_MEMORY_MODE_READONLY, 0, 0);
            const face = hb.hb_face_create(blob, 0);
            hb.hb_blob_destroy(blob);
            let result: Uint8Array | null = null;
            try
            {
                const uni = hb.hb_subset_input_unicode_set(input);
                for(const c of o.unicodes) hb.hb_set_add(uni, c);

                const drop = hb.hb_subset_input_set(input, HB_SUBSET_SETS_DROP_TABLE_TAG);
                for(const t of o.dropTables) hb.hb_set_add(drop, hbTag(t));

                const names = hb.hb_subset_input_set(input, HB_SUBSET_SETS_NAME_ID);
                hb.hb_set_clear(names);
                if(o.nameIds === "all") hb.hb_set_invert(names);
                else for(const id of o.nameIds) hb.hb_set_add(names, id);
                if(o.allNameLanguages)
                {
                    const langs = hb.hb_subset_input_set(input, HB_SUBSET_SETS_NAME_LANG_ID);
                    hb.hb_set_clear(langs);
                    hb.hb_set_invert(langs);
                }
                // Every layout feature, so a kept GSUB or GPOS keeps all of its lookups.
                const feats = hb.hb_subset_input_set(input, HB_SUBSET_SETS_LAYOUT_FEATURE_TAG);
                hb.hb_set_clear(feats);
                hb.hb_set_invert(feats);

                let flags = hb.hb_subset_input_get_flags(input) | HB_SUBSET_FLAGS_NOTDEF_OUTLINE;
                if(!o.hinting) flags |= HB_SUBSET_FLAGS_NO_HINTING;
                if(!o.layoutClosure) flags |= HB_SUBSET_FLAGS_NO_LAYOUT_CLOSURE;
                hb.hb_subset_input_set_flags(input, flags >>> 0);

                const sub = hb.hb_subset_or_fail(face, input);
                if(!sub) throw new Error("hb_subset_or_fail failed");
                const out = hb.hb_face_reference_blob(sub);
                const len = hb.hb_blob_get_length(out);
                const at  = hb.hb_blob_get_data(out, 0);
                if(len) result = new Uint8Array(hb.memory.buffer, at, len).slice();
                hb.hb_blob_destroy(out);
                hb.hb_face_destroy(sub);
            }
            finally
            {
                hb.hb_subset_input_destroy(input);
                hb.hb_face_destroy(face);
                hb.free(buf);
            }
            if(!result) throw new Error("the subset is empty");
            return result;
        },
    };
}
