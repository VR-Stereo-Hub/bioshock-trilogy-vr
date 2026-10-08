// ExportScripts.cs - batch UnrealScript exporter for the local reverse-engineering
// corpus (tools/uscript/<game>/). Drives Eliot.UELib (the library shipped inside
// UE Explorer 1.6.2) directly, because UE Explorer's own `-console -export=scripts`
// exited 0 and wrote nothing for Dishonored's packages, and driving UELib from
// PowerShell blows the stack (an uncatchable StackOverflowException in the ETS
// while it walks the object graph). Ported from the Dishonored VR mod (logic unchanged);
// built by tools\uscript-export\build.ps1, run by tools\uscript-export.ps1.
// BS1's BakedScripts\pc\*.U (Vengeance, package version 142) export with it as-is.
//
// ONE PROCESS PER PACKAGE, by design: a StackOverflowException cannot be
// caught in .NET, so a package that trips one must be able to die alone and
// be recorded, without taking the batch or the session with it.
//
// Output: <outDir>\<PackageName>\<ClassName>.uc
// Prints one machine-readable summary line: RESULT <pkg> classes=N failed=M
//
// The corpus this produces is GAME-DERIVED and is never committed - see
// CLAUDE.md's hard rule and .gitignore on tools/uscript/.
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using UELib;
using UELib.Core;

internal static class ExportScripts
{
    private static string SafeName(string name)
    {
        var sb = new StringBuilder(name.Length);
        foreach (char c in name)
            sb.Append(Array.IndexOf(Path.GetInvalidFileNameChars(), c) >= 0 ? '_' : c);
        var s = sb.ToString().Trim();
        return s.Length == 0 ? "_unnamed" : s;
    }

    private static int Main(string[] args)
    {
        if (args.Length < 2)
        {
            Console.Error.WriteLine("usage: ExportScripts <package> <outDir> [--inventory|--diag]");
            return 2;
        }
        string pkgPath = args[0];
        string outRoot = args[1];
        bool diag = Array.IndexOf(args, "--diag") >= 0;
        bool inventoryOnly = Array.IndexOf(args, "--inventory") >= 0;

        UnrealPackage pkg;
        try
        {
            pkg = UnrealLoader.LoadPackage(pkgPath, FileAccess.Read);
            if (pkg == null)
            {
                Console.Out.WriteLine("RESULT " + Path.GetFileName(pkgPath) + " classes=0 failed=0 note=load-returned-null");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Console.Out.WriteLine("RESULT " + Path.GetFileName(pkgPath) + " classes=0 failed=0 note=load-threw:" + ex.GetType().Name);
            return 3;
        }

        // INVENTORY line, always, before any early return: what this package is,
        // whether it claims script, and whether it is compressed (which decides
        // whether ANY of it is readable without a decompression pass first).
        {
            uint cf = 0; int ch = -1; uint pf = 0;
            try { cf = pkg.Summary.CompressionFlags; } catch { }
            try { ch = pkg.Summary.CompressedChunks == null ? -1 : pkg.Summary.CompressedChunks.Count; } catch { }
            try { pf = pkg.Summary.PackageFlags; } catch { }
            Console.Out.WriteLine("INV " + Path.GetFileName(pkgPath) +
                                  " packageFlags=0x" + pf.ToString("X8") +
                                  " containsScript=" + (((pf & 0x00200000u) != 0) ? "1" : "0") +
                                  " compressionFlags=" + cf + " compressedChunks=" + ch);
        }
        if (inventoryOnly)
        {
            try { pkg.Dispose(); } catch { }
            return 0;
        }

        // Only packages whose header advertises script are worth deserializing;
        // the rest are meshes, textures and levels and cost minutes for nothing.
        bool containsScript = false;
        try { containsScript = (pkg.Summary.PackageFlags & 0x00200000u) != 0; }
        catch { containsScript = true; }   // unknown: try anyway rather than skip silently
        if (!containsScript)
        {
            Console.Out.WriteLine("RESULT " + Path.GetFileName(pkgPath) + " classes=0 failed=0 note=no-script-flag");
            try { pkg.Dispose(); } catch { }
            return 0;
        }

        try
        {
            pkg.InitializePackage(UnrealPackage.InitFlags.All);
        }
        catch (Exception ex)
        {
            Console.Out.WriteLine("RESULT " + Path.GetFileName(pkgPath) + " classes=0 failed=0 note=init-threw:" + ex.GetType().Name);
            return 4;
        }
        // InitializePackage(All) leaves pkg.Objects empty on these packages, so
        // ask for the export objects explicitly. Belt and braces: the export
        // TABLE is the authority below either way.
        try { pkg.InitializeExportObjects(UnrealPackage.InitFlags.All); }
        catch { }

        if (diag)
        {
            var hist = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
            int withObject = 0, classExports = 0;
            var netTypes = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
            foreach (var it in pkg.Exports)
            {
                string cn;
                try { cn = it.ClassName ?? "(null)"; } catch { cn = "(threw)"; }
                if (string.IsNullOrEmpty(cn)) cn = "(empty=Class?)";
                int c; hist.TryGetValue(cn, out c); hist[cn] = c + 1;
                if (cn == "Class" || cn == "(empty=Class?)") classExports++;
                UObject o = null;
                try { o = it.Object; } catch { }
                if (o != null)
                {
                    withObject++;
                    string tn = o.GetType().Name;
                    int t; netTypes.TryGetValue(tn, out t); netTypes[tn] = t + 1;
                }
            }
            int chunks = -1;
            uint cflags = 0;
            try { cflags = pkg.Summary.CompressionFlags; } catch { }
            try { chunks = pkg.Summary.CompressedChunks == null ? -1 : pkg.Summary.CompressedChunks.Count; } catch { }
            Console.Out.WriteLine("DIAG exports=" + pkg.Exports.Count + " objectsList=" + (pkg.Objects == null ? -1 : pkg.Objects.Count) +
                                  " withObject=" + withObject + " classExports=" + classExports +
                                  " compressionFlags=" + cflags + " compressedChunks=" + chunks +
                                  " names=" + pkg.Names.Count + " imports=" + pkg.Imports.Count);
            var keys = new List<string>(hist.Keys);
            keys.Sort(delegate (string a, string b) { return hist[b].CompareTo(hist[a]); });
            for (int i = 0; i < keys.Count && i < 15; i++)
                Console.Out.WriteLine("DIAG   exportClass " + keys[i] + " = " + hist[keys[i]]);
            var tkeys = new List<string>(netTypes.Keys);
            tkeys.Sort(delegate (string a, string b) { return netTypes[b].CompareTo(netTypes[a]); });
            for (int i = 0; i < tkeys.Count && i < 15; i++)
                Console.Out.WriteLine("DIAG   netType " + tkeys[i] + " = " + netTypes[tkeys[i]]);
            try { pkg.Dispose(); } catch { }
            return 0;
        }

        string pkgName = SafeName(Path.GetFileNameWithoutExtension(pkgPath));
        string outDir = Path.Combine(outRoot, pkgName);

        int ok = 0, failed = 0;
        var used = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        // The EXPORT TABLE is the authority, not pkg.Objects: on these packages
        // InitializePackage leaves that list empty, and an export whose class is
        // "Class" is a script class whether or not UELib materialised it into a
        // typed UClass for us.
        var targets = new List<UObject>();
        foreach (var it in pkg.Exports)
        {
            string cn;
            try { cn = it.ClassName; } catch { continue; }
            if (!string.IsNullOrEmpty(cn) && cn != "Class") continue;
            UObject o = null;
            try { o = it.Object; } catch { }
            if (o != null) targets.Add(o);
        }

        foreach (var obj in targets)
        {
            var dec = obj as IUnrealDecompilable;
            if (dec == null) continue;
            string text;
            try
            {
                obj.BeginDeserializing();
                text = dec.Decompile();
            }
            catch (Exception ex)
            {
                failed++;
                try
                {
                    Directory.CreateDirectory(outDir);
                    string fn = Path.Combine(outDir, SafeName(obj.Name) + ".decompile-error.txt");
                    File.WriteAllText(fn, "decompile failed: " + ex.GetType().Name + ": " + ex.Message + Environment.NewLine + ex.StackTrace);
                }
                catch { }
                continue;
            }
            if (string.IsNullOrEmpty(text)) { failed++; continue; }
            var cls = obj;

            try
            {
                Directory.CreateDirectory(outDir);
                string baseName = SafeName(cls.Name);
                string name = baseName;
                int n = 2;
                while (!used.Add(name)) { name = baseName + "_" + n.ToString(); n++; }
                File.WriteAllText(Path.Combine(outDir, name + ".uc"), text, new UTF8Encoding(false));
                ok++;
            }
            catch (Exception)
            {
                failed++;
            }
        }

        try { pkg.Dispose(); } catch { }
        Console.Out.WriteLine("RESULT " + Path.GetFileName(pkgPath) + " classes=" + ok + " failed=" + failed);
        return 0;
    }
}
