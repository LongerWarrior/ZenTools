# ZenTools

ZenTools extracts cooked packages (.uasset/.uexp) from the IoStore container files (.ucas/.utoc + .pak).

Works on UE5.1 (Use `-ZenPackageVersion=Initial`) and 5.2 and should support legacy 5.1 archives too.

## Build:

1. Clone UE 5.2 branch.
2. Clone this repository into `Engine/Source/Programs/ZenTools`.
3. Run Setup.bat and GenerateProjectFiles.bat
4. For correct 5.1 build you need to apply the following patch to `\Engine\Source\Runtime\CoreUObject\Private\UObject\PackageFileSummary.cpp`:
```diff
diff --git a/Engine/Source/Runtime/CoreUObject/Private/UObject/PackageFileSummary.cpp b/Engine/Source/Runtime/CoreUObject/Private/UObject/PackageFileSummary.cpp
index ff88ab491..e33d5fb06 100644
--- a/Engine/Source/Runtime/CoreUObject/Private/UObject/PackageFileSummary.cpp
+++ b/Engine/Source/Runtime/CoreUObject/Private/UObject/PackageFileSummary.cpp
@@ -433,7 +433,7 @@ void operator<<(FStructuredArchive::FSlot Slot, FPackageFileSummary& Sum)
                        Sum.PayloadTocOffset = INDEX_NONE;
                }

-               if (BaseArchive.IsSaving() || Sum.GetFileVersionUE() >= EUnrealEngineObjectUE5Version::DATA_RESOURCES)
+               if (BaseArchive.IsSaving() && Sum.GetFileVersionUE() >= EUnrealEngineObjectUE5Version::DATA_RESOURCES)
                {
                        Record << SA_VALUE(TEXT("DataResourceOffset"), Sum.DataResourceOffset);
                }
```
And `FFileHelper::SaveArrayToFile` signature to supports TArrayView64


## Usage:

`ZenTools ExtractPackages <ContainerFolderPath> <ExtractionDir> [-Aes=<MainAesKey>] [-EncryptionKeys=<KeyFile>] [-ZenPackageVersion=<Initial/DataResourceTable/Latest>] [-SkipBulkData] [-PackageFilter=<Package/Path/Filter>] [-Filter=<FilterFile>]`

- `ContainerFolderPath` - Path to the folder containing the container files (.ucas/.utoc + .pak).
- `ExtractionDir` - Path to the folder where the extracted packages will be saved.
- `MainAesKey` - Main AES key for the game.
- `KeyFile` - Path to the file containing encryption keys.
- `ZenPackageVersion` - Version of the ZenPackage format to use. Default is `Latest`. `Initial` for UE5.1
- `SkipBulkData` - Skip extracting bulk data.
- `PackageFilter` - Filters packages to extract by path (use `!` at the start of the line to exclude packages)
Example: `/Game/Path/To/Package` or `!/Game/Path/To/Package` to exclude
- `Filter` - Path to the file containing package paths to extract. One path per line, use `!` at the start of the line to exclude packages.

If your game has encrypted paks, you must provide a keys.json, in the following format:

```json
{
  "KeyGUID1": "KeyHex1",
  "KeyGUID2": "KeyHex2"
}
```

Obviously if your game only has one encryption key, you only need to specify one entry.

**Example:**

`ZenTools.exe ExtractPackages "D:\SteamLibrary\steamapps\common\somegame\projectname\Content\Paks" "D:\somegame\Output" -EncryptionKeys="D:\somegame\keys.json"`

Since the game in the above example needs an AES key, this is the following `keys.json` file:

```json
{
  "00000000-0000-0000-0000-000000000000": "DEADBEEFCAFEDEADBEEFCAFEDEADBEEFCAFEDEADBEEFCAFEDEADBEEFCAFEDEAD"
}
```
or use `-AES=DEADBEEFCAFEDEADBEEFCAFEDEADBEEFCAFEDEADBEEFCAFEDEADBEEFCAFEDEAD` instead.
