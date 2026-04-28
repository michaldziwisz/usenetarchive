#ifndef WINBROWSER_ARCHIVEDOWNLOADER_HPP
#define WINBROWSER_ARCHIVEDOWNLOADER_HPP

#include <windows.h>

#include <string>

namespace winbrowser
{

std::wstring LoadDownloadWorkingFolder();
void SaveDownloadWorkingFolder( const std::wstring& folder );

std::wstring ShowArchiveDownloadDialog(
    HWND owner,
    HINSTANCE instance,
    HFONT font,
    const std::wstring& initialWorkingFolder,
    std::wstring& selectedWorkingFolder
);

}  // namespace winbrowser

#endif
