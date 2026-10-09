# Launcher icons

The current review icon is **bioshock-vr-medallion.png / .ico**. The owner
requested the circular brass medallion, then specified the actual weathered
silver BIOSHOCK wordmark treatment, with VR incorporated beneath it **inside
the same plaque**. The final
image follows those supplied references using the built-in image generation
tool. It is an adaptation of BioShock artwork, not a newly invented trademark.
The BioShock name and artwork belong to their respective owners.

The supplied reference files are not included in the repository. The final
generated PNG is unmodified, with genuine alpha. The ICO contains the seven
sizes below. The two intermediate lettering attempts were superseded.

Three original transparent PNGs generated for the launcher on 2026-10-08 using
the built-in image generation tool. No game assets were extracted or edited.
The source PNGs are unmodified. `tools/launcher-icons.ps1` converts each into
a Windows ICO with 16, 24, 32, 48, 64, 128 and 256 pixel entries.

* **A, Bathysphere helmet**: brass diving helmet with a teal VR-like window.
  Initial alternative, superseded by the requested medallion.
* **B, Rapture beacon**: lighthouse and waves inside a brass medallion.
* **C, Rapture VR crest**: stepped Art Deco towers integrated with a teal visor.

The executable uses the six F10 background/control textures directly from
`assets/ui/f10`; these icon files are only for the application/taskbar/shortcut.

## Generation prompts

All three calls used `transparent_background: true`, with no reference image.

The selected medallion used three image-edit stages, also with
`transparent_background: true`. The owner supplied the medallion reference and
the high-resolution game wordmark. The game's logo reference was the primary
source in the final composition. Both saved images were inspected before use.

### Selected medallion, base stage

Use case: precise-object-edit. Asset type: Windows launcher icon. Input image 1 is the user's reference and edit target. Recreate this brass BIOSHOCK Art Deco medallion at crisp high resolution, keeping its round geometric engraved disk, stepped rectangular central nameplate, small perimeter studs, aged bronze-gold material and nearly front-facing composition very close to the reference. The requested change is to add the exact text "VR" prominently centered immediately beneath the exact name "BIOSHOCK". Make BIOSHOCK and VR two clear, balanced lines, with VR in matching embossed Art Deco lettering; adjust only the lower central decorative area as needed to give VR room. Preserve the overall medallion silhouette and the reference's restrained antique brass palette. The letters must be accurately spelled, legible and distinct, with brighter brass edges and dark recessed interiors. Do not add a headset, lighthouse, characters or any additional words. Single centered emblem, square canvas, close framing with a small transparent safety margin on all sides. Remove the black backdrop: genuine transparent background outside the emblem, preserve alpha, no opaque background tile or outside cast shadow. This is an icon used down to 32 pixels, so make the VR letters large enough to remain recognizable and avoid fragile added detail.

### Selected medallion, wordmark composition

Use case: compositing. Create the final BioShock VR launcher icon from these inputs. Image 1 is the PRIMARY exact source graphic: the official BIOSHOCK wordmark plaque, with its normal rounded S, distinctive broad letter proportions, enormous black extruded shadows toward the upper left, scratched and mottled cold silver letter faces, chipped silver outline, rusty green/orange background panels, and silver Rapture skyline under the letters. Preserve this source plaque and the BIOSHOCK lettering as faithfully as possible: it should look like the same graphic placed into the icon, NOT an alternate font, NOT reinterpreted Art Deco lettering, NOT a new angular S, NOT thin gold-outline type. Image 2 supplies ONLY the round brass medallion body, its circular engraved Art Deco frame, perimeter studs, and the placement of VR. REPLACE image 2's whole central BIOSHOCK banner with the faithful plaque from image 1. The intact source plaque should span almost the full width of the round medallion, occupying its center, retaining its silver skyline and entire source lettering. Below that plaque, add a separate clear centered 'VR' in matching weathered cold-silver letters with strong deep black extruded shadows, comfortably within the lower portion of the brass medallion. Keep BIOSHOCK spelled exactly and VR spelled exactly. Preserve the overall circular medallion silhouette from image 2. No extra words. Do not copy image 2's BIOSHOCK letter shapes. Single centered icon on a square canvas, close framing, small transparent margin, real alpha around the whole emblem, no black background rectangle and no presentation mockup.

### Selected medallion, VR integrated into the plaque

Use case: precise-object-edit. Image 1 is the current BioShock VR medallion icon to edit. Image 2 is the exact BIOSHOCK wordmark reference and must continue to govern the typography. Make one specific correction: INCORPORATE THE VR LETTERS INSIDE THE SAME CENTRAL PLAQUE AS BIOSHOCK. Extend the central framed metal plaque downward as necessary so that ONE continuous chipped silver perimeter frame encloses both the BIOSHOCK line above and the centered VR line below. VR must share the same rusted green/orange metal background and dark extruded shadows within that plaque; it must not float outside the frame on the circular brass ornament. Rearrange the skyline motifs within the lower plaque to flank VR and leave its lettering clear. Preserve the BIOSHOCK wordmark from image 1 faithfully: same normal rounded S, broad letter proportions, cold weathered silver faces and large upper-left black extrusions, as in image 2. Do not redesign or re-letter BIOSHOCK. Keep the round brass Art Deco medallion and perimeter studs behind the combined BIOSHOCK VR plaque. No separate VR badge, no separate caption, no VR text below the outer plaque border, no extra text. Preserve genuine transparency around the emblem. Center the whole emblem on the square canvas with a small uniform transparent safety margin so no perimeter stud is clipped. This is one finished icon, not a presentation sheet.

### A

Use case: logo-brand. Create one premium Windows application icon for a BioShock Remastered VR mod launcher, option A 'Bathysphere helmet'. A bold front-facing antique deep-sea diving helmet made of worn golden brass, with a single wide luminous sea-teal viewing window subtly resembling a VR headset. Strong thick silhouettes, elegant 1930s Rapture Art Deco metal construction, restrained patina, warm ivory highlights. Centered single icon with generous transparent padding. Legible at 32x32 pixels, no tiny rivet clutter, no text, no lettering, no watermark, no background, no shadow outside the icon. Square image. Rich illustrated game-launcher quality, not flat emoji. Transparent background.

### B

Use case: logo-brand. Create one premium Windows application icon for a BioShock Remastered VR mod launcher, option B 'Rapture beacon'. A tall 1930s Art Deco lighthouse in polished aged brass emerging above two broad stylized sea-teal waves, within a thick elegant octagonal brass medallion. The lighthouse beam forms two subtle angular ivory rays in the upper half. Cohesive engraved 3D illustrated game launcher icon, elegant Rapture underwater-city mood. Bold highly legible silhouette at 32x32 pixels. Centered single icon, generous transparent padding. Palette antique brass, deep teal, sea-green, ivory highlights. No lettering, no words, no logos, no watermark, no background or cast shadow. Square image, transparent background.

### C

Use case: logo-brand. One premium Windows application icon for a BioShock Remastered VR launcher, option C 'Rapture VR crest'. Make a beautifully crafted compact symmetrical 1930s Art Deco badge: a broad stylized modern VR visor in sea-teal enamel with golden brass outline, integrated into the bottom of three bold stepped Rapture skyscraper silhouettes rising above it. The visor and buildings form a single unified crest. Rich illustrated dimensional metallic app icon with restrained worn brass patina, warm ivory edge highlights, dark teal glass. Very strong distinct silhouette that remains legible at 32 pixels, few broad elements, visually simpler than a diving helmet. No text, no lettering, no watermark, no background, no surrounding shadow. Center one icon on a transparent square canvas with generous transparent padding.
