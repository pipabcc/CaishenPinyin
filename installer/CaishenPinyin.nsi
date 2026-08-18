Unicode true
ManifestDPIAware true
RequestExecutionLevel admin
SetCompressor /SOLID lzma
SetDatablockOptimize on
CRCCheck on

!ifndef PRODUCT_VERSION
  !error "PRODUCT_VERSION is required"
!endif
!ifndef PRODUCT_VERSION_NUMERIC
  !error "PRODUCT_VERSION_NUMERIC is required"
!endif
!ifndef PAYLOAD_DIR
  !error "PAYLOAD_DIR is required"
!endif
!ifndef DEPLOY_SCRIPT
  !error "DEPLOY_SCRIPT is required"
!endif
!ifndef APP_ICON
  !error "APP_ICON is required"
!endif
!ifndef OUTPUT_FILE
  !error "OUTPUT_FILE is required"
!endif
!ifndef ESTIMATED_SIZE_KB
  !define ESTIMATED_SIZE_KB 0
!endif

!define PRODUCT_NAME "财神拼音"
!define PRODUCT_PUBLISHER "Caishen IME"
!define PRODUCT_ARCH "win-x64"
!define PRODUCT_ID "CaishenPinyin"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_ID}"

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "nsDialogs.nsh"
!include "FileFunc.nsh"
!include "TextFunc.nsh"
!include "WordFunc.nsh"
!include "WinMessages.nsh"
!include "x64.nsh"

Name "${PRODUCT_NAME}"
Caption "${PRODUCT_NAME} 安装程序"
OutFile "${OUTPUT_FILE}"
InstallDir "$PROGRAMFILES64\CaishenPinyin"
InstallDirRegKey HKLM "${UNINSTALL_KEY}" "InstallLocation"
Icon "${APP_ICON}"
UninstallIcon "${APP_ICON}"
BrandingText "${PRODUCT_NAME} ${PRODUCT_VERSION}"
ShowInstDetails nevershow
ShowUninstDetails nevershow
XPStyle on

VIProductVersion "${PRODUCT_VERSION_NUMERIC}"
VIAddVersionKey /LANG=2052 "ProductName" "${PRODUCT_NAME}"
VIAddVersionKey /LANG=2052 "ProductVersion" "${PRODUCT_VERSION}"
VIAddVersionKey /LANG=2052 "CompanyName" "${PRODUCT_PUBLISHER}"
VIAddVersionKey /LANG=2052 "FileDescription" "${PRODUCT_NAME} 安装程序"
VIAddVersionKey /LANG=2052 "FileVersion" "${PRODUCT_VERSION}"
VIAddVersionKey /LANG=2052 "LegalCopyright" "Copyright (c) ${PRODUCT_PUBLISHER}"

!define MUI_ICON "${APP_ICON}"
!define MUI_UNICON "${APP_ICON}"
!define MUI_ABORTWARNING
!define MUI_UNABORTWARNING
!define MUI_FINISHPAGE_NOAUTOCLOSE
!define MUI_UNFINISHPAGE_NOAUTOCLOSE

Var DefaultInputState
Var DeleteUserDataState
Var InstallMode
Var InstalledVersion
Var DeploymentVersion
Var HasExistingInstall
Var PowerShellPath
Var ProgramDataPath
Var PageDialog
Var PageTitle
Var PageMode
Var PageBody
Var PagePath
Var PageCheckbox
Var PageNote
Var TitleFont
Var ModeFont
Var BodyFont
Var NoteFont

Page custom InstallOptionsCreate InstallOptionsLeave
!define MUI_PAGE_HEADER_TEXT "正在安装 ${PRODUCT_NAME}"
!define MUI_PAGE_HEADER_SUBTEXT "正在验证、复制并注册输入法组件"
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_TITLE "安装完成"
!define MUI_FINISHPAGE_TEXT "${PRODUCT_NAME} ${PRODUCT_VERSION} 已安装并注册。可从开始菜单打开设置，或使用 Win+Space 切换输入法。"
!insertmacro MUI_PAGE_FINISH

UninstPage custom un.UninstallOptionsCreate un.UninstallOptionsLeave
!define MUI_UNPAGE_HEADER_TEXT "正在卸载 ${PRODUCT_NAME}"
!define MUI_UNPAGE_HEADER_SUBTEXT "正在注销输入法并清理程序文件"
!insertmacro MUI_UNPAGE_INSTFILES
!define MUI_UNFINISHPAGE_TITLE "卸载完成"
!define MUI_UNFINISHPAGE_TEXT "${PRODUCT_NAME} 已从此电脑卸载。"
!insertmacro MUI_UNPAGE_FINISH

!insertmacro MUI_LANGUAGE "SimpChinese"

Function DetectExistingInstallation
  StrCpy $HasExistingInstall 0
  StrCpy $InstallMode "install"
  StrCpy $InstalledVersion ""

  IfFileExists "$INSTDIR\current" 0 detect_uninstaller
  FileOpen $0 "$INSTDIR\current" r
  IfErrors detect_uninstaller
  FileRead $0 $1
  FileClose $0
  ${TrimNewLines} $1 $1
  StrCpy $InstalledVersion $1
  IfFileExists "$INSTDIR\versions\$1\ShuruIme.dll" 0 detect_uninstaller
  StrCpy $HasExistingInstall 1
  ClearErrors
  GetDLLVersion "$INSTDIR\versions\$1\ShuruIme.dll" $R0 $R1
  IfErrors existing_unknown
  IntOp $R2 $R0 >> 16
  IntOp $R3 $R0 & 0xFFFF
  IntOp $R4 $R1 >> 16
  IntOp $R5 $R1 & 0xFFFF
  StrCpy $InstalledVersion "$R2.$R3.$R4.$R5"
  ${VersionCompare} "${PRODUCT_VERSION_NUMERIC}" "$InstalledVersion" $R6
  ${If} $R6 == 2
    MessageBox MB_ICONSTOP|MB_OK "检测到较新的已安装版本 $InstalledVersion。为保护现有安装，本安装包不会执行降级。" /SD IDOK
    Abort
  ${ElseIf} $R6 == 0
    StrCpy $InstallMode "repair"
  ${Else}
    StrCpy $InstallMode "upgrade"
  ${EndIf}
  Return

  detect_uninstaller:
  IfFileExists "$INSTDIR\Uninstall.exe" 0 detect_done
  StrCpy $HasExistingInstall 1
  StrCpy $InstallMode "repair"
  Return

  existing_unknown:
  StrCpy $InstallMode "repair"

  detect_done:
FunctionEnd

Function .onInit
  ${IfNot} ${IsNativeAMD64}
    MessageBox MB_ICONSTOP|MB_OK "${PRODUCT_NAME} 仅支持 64 位 Windows。" /SD IDOK
    Abort
  ${EndIf}
  SetRegView 64
  SetShellVarContext all
  StrCpy $PowerShellPath "$WINDIR\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
  ExpandEnvStrings $ProgramDataPath "%ProgramData%"
  StrCpy $DefaultInputState ${BST_CHECKED}
  ${GetParameters} $0
  ClearErrors
  ${GetOptions} $0 "/NODEFAULTIME" $1
  ${IfNot} ${Errors}
    StrCpy $DefaultInputState ${BST_UNCHECKED}
  ${EndIf}
  Call DetectExistingInstallation
  StrCpy $DeploymentVersion "${PRODUCT_VERSION}"
  ${If} $InstallMode == "repair"
    System::Call 'kernel32::GetTickCount() i .r0'
    StrCpy $DeploymentVersion "${PRODUCT_VERSION}-repair-$0"
  ${EndIf}
FunctionEnd

Function InstallOptionsCreate
  !insertmacro MUI_HEADER_TEXT "准备安装 ${PRODUCT_NAME}" "确认安装选项后开始部署"
  nsDialogs::Create 1018
  Pop $PageDialog
  ${If} $PageDialog == error
    Abort
  ${EndIf}
  SetCtlColors $PageDialog 0x171717 0xFFFFFF
  CreateFont $TitleFont "Microsoft YaHei UI" 18 600
  CreateFont $ModeFont "Microsoft YaHei UI" 10 600
  CreateFont $BodyFont "Microsoft YaHei UI" 9 400
  CreateFont $NoteFont "Microsoft YaHei UI" 8 400

  ${NSD_CreateLabel} 0 0 100% 24u "安装 ${PRODUCT_NAME}"
  Pop $PageTitle
  SendMessage $PageTitle ${WM_SETFONT} $TitleFont 0
  SetCtlColors $PageTitle 0x171717 0xFFFFFF

  ${If} $InstallMode == "upgrade"
    StrCpy $0 "升级安装"
    StrCpy $1 "将从 $InstalledVersion 升级到 ${PRODUCT_VERSION}，并保留现有设置与个人数据。"
  ${ElseIf} $InstallMode == "repair"
    StrCpy $0 "修复安装"
    StrCpy $1 "将重新验证并部署 ${PRODUCT_VERSION}，现有设置与个人数据保持不变。"
  ${Else}
    StrCpy $0 "全新安装"
    StrCpy $1 "将安装输入法核心、内置皮肤和自包含设置中心。"
  ${EndIf}

  ${NSD_CreateLabel} 0 30u 100% 14u "$0 · ${PRODUCT_VERSION} · ${PRODUCT_ARCH}"
  Pop $PageMode
  SendMessage $PageMode ${WM_SETFONT} $ModeFont 0
  SetCtlColors $PageMode 0xA16207 0xFFFFFF

  ${NSD_CreateLabel} 0 48u 100% 28u "$1 设置中心已包含 .NET 8 桌面运行时，目标电脑无需另行安装。"
  Pop $PageBody
  SendMessage $PageBody ${WM_SETFONT} $BodyFont 0
  SetCtlColors $PageBody 0x404040 0xFFFFFF

  ${NSD_CreateLabel} 0 80u 100% 16u "安装位置：$INSTDIR"
  Pop $PagePath
  SendMessage $PagePath ${WM_SETFONT} $BodyFont 0
  SetCtlColors $PagePath 0x525252 0xFFFFFF

  ${NSD_CreateCheckbox} 0 98u 100% 16u "设为默认输入法"
  Pop $PageCheckbox
  SendMessage $PageCheckbox ${WM_SETFONT} $BodyFont 0
  ${NSD_SetState} $PageCheckbox $DefaultInputState
  SetCtlColors $PageCheckbox 0x171717 0xFFFFFF

  ${NSD_CreateLabel} 0 116u 100% 18u "安装需要管理员权限。当前安装包未签名，Windows 可能显示“未知发布者”。"
  Pop $PageNote
  SendMessage $PageNote ${WM_SETFONT} $NoteFont 0
  SetCtlColors $PageNote 0x737373 0xFFFFFF

  GetDlgItem $0 $HWNDPARENT 1
  SendMessage $0 ${WM_SETTEXT} 0 "STR:立即安装"
  nsDialogs::Show
FunctionEnd

Function InstallOptionsLeave
  ${NSD_GetState} $PageCheckbox $DefaultInputState
FunctionEnd

Section "安装 ${PRODUCT_NAME}" SEC_INSTALL
  SetShellVarContext all
  SetRegView 64
  InitPluginsDir
  SetOutPath "$PLUGINSDIR\payload"
  File /r "${PAYLOAD_DIR}\*"
  SetOutPath "$PLUGINSDIR"
  File /oname=install_ime.ps1 "${DEPLOY_SCRIPT}"

  CreateDirectory "$INSTDIR"

  StrCpy $2 '"$PowerShellPath" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$PLUGINSDIR\install_ime.ps1" -Action Install -DllPath "$PLUGINSDIR\payload\ShuruIme.dll" -SettingsPath "$PLUGINSDIR\payload" -PackagePath "$PLUGINSDIR\payload\data\lexicon" -InstallRoot "$INSTDIR" -DataRoot "$ProgramDataPath\CaishenPinyin\data\lexicon" -UserDataRoot "$LOCALAPPDATA\CaishenPinyin" -Version "$DeploymentVersion" -SigningPolicy Off'
  ${If} $DefaultInputState == ${BST_CHECKED}
    StrCpy $2 '$2 -SetDefaultInputMethod'
  ${EndIf}
  DetailPrint "执行经过校验的安装事务"
  nsExec::ExecToLog $2
  Pop $3
  ${If} $3 != 0
    ${If} $HasExistingInstall == 0
      Delete "$INSTDIR\Uninstall.exe"
      RMDir /REBOOTOK "$INSTDIR"
    ${EndIf}
    SetErrorLevel $3
    MessageBox MB_ICONSTOP|MB_OK "安装事务失败（错误码 $3）。现有版本已尽可能恢复，请查看 $INSTDIR\logs 中的部署日志。" /SD IDOK
    Abort
  ${EndIf}

  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayName" "${PRODUCT_NAME}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayVersion" "${PRODUCT_VERSION}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "Publisher" "${PRODUCT_PUBLISHER}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\versions\$DeploymentVersion\ShuruSettings.exe,0"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKLM "${UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "EstimatedSize" ${ESTIMATED_SIZE_KB}
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoRepair" 1
SectionEnd

Function un.onInit
  ${IfNot} ${IsNativeAMD64}
    MessageBox MB_ICONSTOP|MB_OK "${PRODUCT_NAME} 卸载程序仅支持 64 位 Windows。" /SD IDOK
    Abort
  ${EndIf}
  SetRegView 64
  SetShellVarContext all
  StrCpy $PowerShellPath "$WINDIR\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
  ExpandEnvStrings $ProgramDataPath "%ProgramData%"
  StrCpy $DeleteUserDataState ${BST_UNCHECKED}
  ${GetParameters} $0
  ClearErrors
  ${GetOptions} $0 "/DELETEUSERDATA" $1
  ${IfNot} ${Errors}
    StrCpy $DeleteUserDataState ${BST_CHECKED}
  ${EndIf}
FunctionEnd

Function un.UninstallOptionsCreate
  !insertmacro MUI_HEADER_TEXT "卸载 ${PRODUCT_NAME}" "选择是否同时移除个人数据"
  nsDialogs::Create 1018
  Pop $PageDialog
  ${If} $PageDialog == error
    Abort
  ${EndIf}
  SetCtlColors $PageDialog 0x171717 0xFFFFFF
  CreateFont $TitleFont "Microsoft YaHei UI" 18 600
  CreateFont $ModeFont "Microsoft YaHei UI" 10 600
  CreateFont $BodyFont "Microsoft YaHei UI" 9 400
  CreateFont $NoteFont "Microsoft YaHei UI" 8 400

  ${NSD_CreateLabel} 0 0 100% 24u "卸载 ${PRODUCT_NAME}"
  Pop $PageTitle
  SendMessage $PageTitle ${WM_SETFONT} $TitleFont 0
  SetCtlColors $PageTitle 0x171717 0xFFFFFF

  ${NSD_CreateLabel} 0 30u 100% 16u "移除输入法组件和程序文件"
  Pop $PageMode
  SendMessage $PageMode ${WM_SETFONT} $ModeFont 0
  SetCtlColors $PageMode 0xA16207 0xFFFFFF

  ${NSD_CreateLabel} 0 52u 100% 30u "卸载程序会注销财神拼音，并且仅在当前默认输入法仍为财神拼音时恢复安装前的默认值。"
  Pop $PageBody
  SendMessage $PageBody ${WM_SETFONT} $BodyFont 0
  SetCtlColors $PageBody 0x404040 0xFFFFFF

  ${NSD_CreateCheckbox} 0 88u 100% 16u "同时删除设置、皮肤、剪贴板记录和词库数据"
  Pop $PageCheckbox
  SendMessage $PageCheckbox ${WM_SETFONT} $BodyFont 0
  ${NSD_SetState} $PageCheckbox $DeleteUserDataState
  SetCtlColors $PageCheckbox 0x991B1B 0xFFFFFF

  ${NSD_CreateLabel} 0 110u 100% 24u "默认不勾选，以保留个人数据和用户自行放入的 rime-moqi-zh.gram。删除后无法由卸载程序恢复。"
  Pop $PageNote
  SendMessage $PageNote ${WM_SETFONT} $NoteFont 0
  SetCtlColors $PageNote 0x737373 0xFFFFFF

  GetDlgItem $0 $HWNDPARENT 1
  SendMessage $0 ${WM_SETTEXT} 0 "STR:开始卸载"
  nsDialogs::Show
FunctionEnd

Function un.UninstallOptionsLeave
  ${NSD_GetState} $PageCheckbox $DeleteUserDataState
  ${If} $DeleteUserDataState == ${BST_CHECKED}
    MessageBox MB_ICONEXCLAMATION|MB_YESNO|MB_DEFBUTTON2 "确定同时删除全部个人数据和词库数据吗？此操作无法撤销。" /SD IDNO IDYES +2
    Abort
  ${EndIf}
FunctionEnd

Section "Uninstall"
  SetShellVarContext all
  SetRegView 64
  InitPluginsDir
  SetOutPath "$PLUGINSDIR"
  File /oname=install_ime.ps1 "${DEPLOY_SCRIPT}"

  StrCpy $2 '"$PowerShellPath" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$PLUGINSDIR\install_ime.ps1" -Action Uninstall -InstallRoot "$INSTDIR" -DataRoot "$ProgramDataPath\CaishenPinyin\data\lexicon" -UserDataRoot "$LOCALAPPDATA\CaishenPinyin" -SigningPolicy Off'
  ${If} $DeleteUserDataState == ${BST_CHECKED}
    StrCpy $2 '$2 -DeleteUserData'
  ${EndIf}
  DetailPrint "执行经过校验的卸载事务"
  nsExec::ExecToLog $2
  Pop $3
  ${If} $3 != 0
    SetErrorLevel $3
    MessageBox MB_ICONSTOP|MB_OK "卸载事务失败（错误码 $3）。程序文件和注册状态未完全移除，请查看 $INSTDIR\logs 中的部署日志。" /SD IDOK
    Abort
  ${EndIf}

  DeleteRegKey HKLM "${UNINSTALL_KEY}"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir /r /REBOOTOK "$INSTDIR"
  ${If} $DeleteUserDataState == ${BST_CHECKED}
    RMDir /r /REBOOTOK "$ProgramDataPath\CaishenPinyin"
    RMDir /r /REBOOTOK "$LOCALAPPDATA\CaishenPinyin"
  ${EndIf}
SectionEnd
