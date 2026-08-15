using System.Windows;
using System.Windows.Input;

namespace ShuruSettings;

public partial class ConfirmDialog : Window
{
    private ConfirmDialog(string title, string message, string confirmText)
    {
        InitializeComponent();
        WindowAppearance.EnableRoundedCorners(this);
        DialogTitle.Text = title;
        DialogMessage.Text = message;
        ConfirmButton.Content = confirmText;
    }

    public static bool Show(
        Window owner,
        string title,
        string message,
        string confirmText = "确认")
    {
        var dialog = new ConfirmDialog(title, message, confirmText)
        {
            Owner = owner
        };
        return dialog.ShowDialog() == true;
    }

    private void Confirm_Click(object sender, RoutedEventArgs e)
    {
        DialogResult = true;
        Close();
    }

    private void Cancel_Click(object sender, RoutedEventArgs e)
    {
        DialogResult = false;
        Close();
    }

    private void Window_PreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key != Key.Escape) return;
        DialogResult = false;
        Close();
        e.Handled = true;
    }
}
