using System;

namespace ShuruSettings;

internal static class ClipboardPasteInputProtocol
{
    // "CPIM" marks only the Ctrl+V sequence emitted by our paste helper.
    internal const uint Marker = 0x4350494D;
    internal static UIntPtr MarkerValue => new(Marker);
}
