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
    Console.WriteLine("settings_logic: OK");
    return 0;
}
finally { Directory.Delete(dir, true); }
