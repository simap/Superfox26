// Command line shell of the font pack tool. Reads fontpack.json, downloads the pinned sources into
// the cache and checks their SHA-256, calls the core (core/pack.ts) and writes or checks the pack.
//
// usage: node fontpack.ts build [--config FILE]   write the pack to the configured output directory
//        node fontpack.ts check [--config FILE]   exit 1 when the pack on disk differs from a fresh build
//
// Downloads are data only: they are read into memory, hashed and parsed, and nothing in the cache is
// executed or unpacked to disk.

import {mkdir, readdir, readFile, rm, writeFile} from "node:fs/promises";
import {existsSync} from "node:fs";
import {basename, dirname, join, resolve} from "node:path";
import {fileURLToPath} from "node:url";
import {gunzipSync} from "node:zlib";

import {buildPack, requiredInputs, sha256Hex} from "./core/pack.ts";
import type {PackConfig, SourceConfig} from "./core/pack.ts";
import {createSubsetter} from "./core/subset.ts";
import {tarMember} from "./core/tar.ts";

const HERE = dirname(fileURLToPath(import.meta.url));

async function sourceBytes(name: string, src: SourceConfig, cfgDir: string, cacheDir: string): Promise<Uint8Array>
{
    let bytes: Uint8Array;
    if(src.url)
    {
        if(!src.sha256) throw new Error("source " + name + ": a url needs a sha256");
        const dir  = join(cacheDir, name);
        const file = join(dir, basename(new URL(src.url).pathname));
        if(!existsSync(file))
        {
            console.log("downloading " + src.url);
            const res = await fetch(src.url, {redirect: "follow"});
            if(!res.ok) throw new Error(src.url + ": HTTP " + res.status);
            const got = new Uint8Array(await res.arrayBuffer());
            const h   = await sha256Hex(got);
            if(h !== src.sha256) throw new Error(src.url + ": SHA-256 " + h + ", the configuration pins " + src.sha256);
            await mkdir(dir, {recursive: true});
            await writeFile(file, got);
        }
        bytes = new Uint8Array(await readFile(file));
    }
    else if(src.path)
        bytes = new Uint8Array(await readFile(resolve(cfgDir, src.path)));
    else
        throw new Error("source " + name + " has neither url nor path");
    if(src.sha256)
    {
        const h = await sha256Hex(bytes);
        if(h !== src.sha256) throw new Error("source " + name + ": SHA-256 " + h + ", the configuration pins " + src.sha256);
    }
    return bytes;
}

async function loadInputs(cfg: PackConfig, cfgDir: string): Promise<Map<string, Uint8Array>>
{
    const cacheDir = resolve(cfgDir, cfg.cache);
    const raw      = new Map<string, Uint8Array>();
    const inputs   = new Map<string, Uint8Array>();
    for(const [key, source, member] of requiredInputs(cfg))
    {
        const src = cfg.sources[source];
        if(!src) throw new Error("unknown source " + source);
        if(!raw.has(source))
        {
            const b = await sourceBytes(source, src, cfgDir, cacheDir);
            raw.set(source, src.archive === "tar.gz" ? new Uint8Array(gunzipSync(b)) : b);
        }
        const data = raw.get(source)!;
        if(src.archive === "tar.gz")
        {
            if(!member) throw new Error("source " + source + " is an archive, the entry needs a member");
            inputs.set(key, tarMember(data, member));
        }
        else
        {
            if(member) throw new Error("source " + source + " is a single file, the entry has a member");
            inputs.set(key, data);
        }
    }
    return inputs;
}

async function main(): Promise<number>
{
    const args = process.argv.slice(2);
    const cmd  = args[0];
    const ci   = args.indexOf("--config");
    const cfgPath = resolve(ci >= 0 ? args[ci + 1] : join(HERE, "fontpack.json"));
    if(cmd !== "build" && cmd !== "check")
    {
        console.error("usage: node fontpack.ts build|check [--config FILE]");
        return 2;
    }
    const cfgDir = dirname(cfgPath);
    const cfg    = JSON.parse(await readFile(cfgPath, "utf8")) as PackConfig;
    const outDir = resolve(cfgDir, cfg.output);

    const wasmUrl = import.meta.resolve("harfbuzzjs/dist/harfbuzz-subset.wasm");
    const hbPkg   = JSON.parse(await readFile(join(HERE, "node_modules", "harfbuzzjs", "package.json"), "utf8"));
    const hb      = await createSubsetter(await readFile(fileURLToPath(wasmUrl)));
    const inputs  = await loadInputs(cfg, cfgDir);
    const pack    = await buildPack(cfg, inputs, hb, {tool: "tools/fontpack", subsetter: "harfbuzzjs " + hbPkg.version});

    // Files a previous pack wrote and this one does not: those the old manifest lists.
    const stale: string[] = [];
    const oldManifest     = join(outDir, "manifest.json");
    if(existsSync(oldManifest))
    {
        const old = JSON.parse(await readFile(oldManifest, "utf8"));
        for(const x of [...(old.faces ?? []), ...(old.licenses ?? [])])
            if(!pack.files.has(x.file)) stale.push(x.file);
    }

    if(cmd === "build")
    {
        await mkdir(outDir, {recursive: true});
        for(const [name, bytes] of pack.files) await writeFile(join(outDir, name), bytes);
        for(const name of stale) await rm(join(outDir, name), {force: true});
        for(const line of pack.report) console.log(line);
        console.log("written to " + outDir);
        return 0;
    }

    let bad = 0;
    const present = new Set(existsSync(outDir) ? await readdir(outDir) : []);
    for(const [name, bytes] of pack.files)
    {
        if(!present.has(name))
        {
            console.log("missing: " + name);
            bad++;
            continue;
        }
        const disk = new Uint8Array(await readFile(join(outDir, name)));
        if(disk.length !== bytes.length || !disk.every((x, i) => x === bytes[i]))
        {
            console.log("differs: " + name);
            bad++;
        }
    }
    for(const name of stale)
        if(present.has(name))
        {
            console.log("stale: " + name);
            bad++;
        }
    console.log(bad ? bad + " files do not match the configuration, run: node fontpack.ts build"
                    : "the pack in " + outDir + " matches the configuration, " + pack.files.size + " files");
    return bad ? 1 : 0;
}

main().then((code) => process.exit(code), (e: Error) =>
{
    console.error("fontpack: " + e.message);
    process.exit(1);
});
