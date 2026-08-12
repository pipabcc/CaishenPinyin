using ShuruSettings;

var dir = Path.Combine(Path.GetTempPath(), "facai-settings-test-" + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(dir);
try
{
    var path = Path.Combine(dir, "settings.ini");
    var defaults = SettingsStore.Load(path);
    if (!defaults.LearningEnabled || defaults.ContentLogging || !defaults.FuzzyEnabled || defaults.CandidateCount != 9) return 1;
    File.WriteAllText(path, "CandidateCount=99\nCandidateFontSize=no\nContentLogging=maybe\nFuzzyEnabled=0\n");
    var fallback = SettingsStore.Load(path);
    if (fallback.CandidateCount != 9 || fallback.CandidateFontSize != 19 || fallback.ContentLogging || fallback.FuzzyEnabled) return 2;
    var expected = new AppSettings(true, false, true, true, false, true, false, true, false, 5, 24);
    SettingsStore.Save(expected, path);
    if (SettingsStore.Load(path) != expected || Directory.GetFiles(dir, "*.tmp-*").Length != 0) return 3;

    var phrasePath = Path.Combine(dir, "custom_phrases.txt");
    var phrases = new[]
    {
        new CustomPhrase("SDS", "深度思考", 1),
        new CustomPhrase("sds", "认真思考", 2),
        new CustomPhrase("ss", "延长思考", 9)
    };
    CustomPhraseStore.Save(phrases, phrasePath);
    var loadedPhrases = CustomPhraseStore.Load(phrasePath);
    if (loadedPhrases.Count != 3 || loadedPhrases[0] != phrases[0] with { Code = "sds" } ||
        loadedPhrases[2].Position != 9 || Directory.GetFiles(dir, "*.tmp-*").Length != 0) return 4;

    File.AppendAllText(phrasePath, "bad-code!\t忽略\t1\n");
    if (CustomPhraseStore.Load(phrasePath).Count != 3) return 5;
    try { CustomPhraseStore.Import(phrasePath); return 6; }
    catch (InvalidDataException) { }

    try
    {
        CustomPhraseStore.Save(new[]
        {
            new CustomPhrase("dup", "重复", 1),
            new CustomPhrase("DUP", "重复", 2)
        }, phrasePath);
        return 7;
    }
    catch (InvalidDataException) { }
    Console.WriteLine("settings_logic: OK");
    return 0;
}
finally { Directory.Delete(dir, true); }
