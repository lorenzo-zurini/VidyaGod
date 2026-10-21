// SlyExtract ISO OUTDIR: ProjectCane's File Extractor without its window. It calls the extractor's own services
// (from its shipped DLL) exactly as the GUI's Extract button does: the levels (Worlds, rebuilt by Sly1BrxRebuilder
// beside this program) and the media (audio, Cutscenes).
using File_Extractor_GUI.Services;
if (args.Length != 2) { Console.Error.WriteLine("usage: SlyExtract <Sly 1 USA ISO> <output folder>"); return 2; }
var det = await Sly1IsoDetector.DetectAsync(args[0]);
if (!det.IsValid) { Console.Error.WriteLine("not the expected disc: " + det.Message); return 1; }
var progress = new Progress<Sly1ExtractionProgress>(p => Console.WriteLine($"{p.Percentage:F0}%"));
await new Sly1ExtractionService().ExtractLevelsAsync(args[0], args[1], det.ExecutableOffset, progress);
await new Sly1MediaExtractionService().ExtractAsync(args[0], args[1], det.ExecutableOffset, progress, CancellationToken.None);
Console.WriteLine("done: " + det.Serial);
return 0;
