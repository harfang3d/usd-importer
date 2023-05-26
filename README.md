# USD to HARFANG 3D Importer

This tool, `usd_importer`, is a USD to HARFANG 3D converter. This document outlines the main parameters and usage of this command-line tool.

## Version

USD to GS Converter 3.2.5 (f0b82d43186b058be90b4518e2a444cd8581ecf2)

## Command Line Usage

```sh
USD->GS Converter 3.2.5 (f0b82d43186b058be90b4518e2a444cd8581ecf2)
No input file
Usage: usd_importer [-out|-o (val)] [-base-resource-path (val)] [-name (val)] [-prefix (val)] [-all-policy
                     (val)] [-geometry-policy (val)] [-material-policy (val)] [-texture-policy (val)]
                     [-scene-policy (val)] [-anim-policy (val)] [-geometry-scale (val)] [-finalizer-script
                     (val)] [-shader|-s (val)] [-recalculate-normal] [-recalculate-tangent] [-detect-geometry-instances]
                     [-anim-to-file] [-quiet|-q] <input>

-out                      : Output directory
-base-resource-path       : Transform references to assets in this directory to be relative
-name                     : Specify the output scene name
-prefix                   : Specify the file system prefix from which relative assets are to be loaded from
-all-policy               : All file output policy (skip, overwrite, rename or skip_always) [default=skip]
-geometry-policy          : Geometry file output policy (skip, overwrite, rename or skip_always) [default=skip]
-material-policy          : Material file output policy (skip, overwrite, rename or skip_always) [default=skip]
-texture-policy           : Texture file output policy (skip, overwrite, rename or skip_always) [default=skip]
-scene-policy             : Scene file output policy (skip, overwrite, rename or skip_always) [default=skip]
-anim-policy              : Animation file output policy (skip, overwrite, rename or skip_always) (note: only applies when saving animations to their own file) [default=skip]
-geometry-scale           : Factor used to scale exported geometries
-finalizer-script         : Path to the Lua finalizer script
-shader                   : Material pipeline shader [default=core/shader/pbr.hps]
-recalculate-normal       : Recreate the vertex normals of exported geometries
-recalculate-tangent      : Recreate the vertex tangent frames of exported geometries
-detect-geometry-instances: Detect and optimize geometry instances
-anim-to-file             : Scene animations will be exported to separate files and not embedded in scene
-quiet                    : Quiet log, only log errors
input                     : Input FBX file to convert
```
