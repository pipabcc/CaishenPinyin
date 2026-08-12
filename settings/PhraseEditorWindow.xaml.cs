using System.IO;
using System.Windows;
using System.Windows.Controls;

namespace ShuruSettings;

public partial class PhraseEditorWindow : Window
{
    public CustomPhrase? Result { get; private set; }

    public PhraseEditorWindow(CustomPhrase? phrase = null)
    {
        InitializeComponent();
        if (phrase is null) return;

        DialogTitle.Text = "编辑自定义短语";
        CodeBox.Text = phrase.Code;
        PhraseBox.Text = phrase.Phrase;
        SelectPosition(phrase.Position);
    }

    private void SelectPosition(int position)
    {
        PositionBox.SelectedItem = PositionBox.Items
            .OfType<ComboBoxItem>()
            .FirstOrDefault(item => item.Content?.ToString() == position.ToString());
    }

    private void Save_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            var positionText = (PositionBox.SelectedItem as ComboBoxItem)?.Content?.ToString();
            if (!int.TryParse(positionText, out var position))
                throw new InvalidDataException("请选择候选位置。");
            Result = CustomPhraseStore.Validate(
                new CustomPhrase(CodeBox.Text, PhraseBox.Text, position));
            DialogResult = true;
        }
        catch (InvalidDataException ex)
        {
            ErrorText.Text = ex.Message;
        }
    }
}
