## Build

Linux or Windows, with the .NET SDK 10, against ProjectCane's File Extractor (its `File Extractor GUI.dll`, from the
ProjectCane release):

```sh
dotnet publish -c Release -r linux-x64 --self-contained -o pub -p:FileExtractorGui="/path/to/File Extractor GUI"
cp "/path/to/File Extractor GUI/Sly1BrxRebuilder" pub/
```

(`-r win-x64` and `Sly1BrxRebuilder.exe` on Windows.)

## Use

`SlyExtract <Sly 1 USA ISO> <output folder>` does what the File Extractor's Extract button does, without its window:
it calls the extractor's own services (levels into `Worlds`, rebuilt by Sly1BrxRebuilder beside it; audio and
`Cutscenes`). The ProjectCane runner's start script runs it on the first launch.
