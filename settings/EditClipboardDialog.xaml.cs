using System.Windows;

namespace ShuruSettings
{
    public partial class EditClipboardDialog : Window
    {
        public string EditedContent => ContentEditor.Text;

        public EditClipboardDialog(string initialText)
        {
            InitializeComponent();
            WindowAppearance.EnableRoundedCorners(this);
            ContentEditor.Text = initialText ?? string.Empty;
            Loaded += (_, _) =>
            {
                ContentEditor.Focus();
                ContentEditor.SelectAll();
            };
        }

        private void Save_Click(object sender, RoutedEventArgs e)
        {
            DialogResult = true;
            Close();
        }

        private void Cancel_Click(object sender, RoutedEventArgs e)
        {
            DialogResult = false;
            Close();
        }
    }
}
