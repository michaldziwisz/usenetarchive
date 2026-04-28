#define NOMINMAX
#ifndef UNICODE
#  define UNICODE
#endif
#ifndef _UNICODE
#  define _UNICODE
#endif

#include "ArchiveDownloader.hpp"

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <lzma.h>

namespace winbrowser
{
namespace
{

constexpr const wchar_t* ArchiveBaseHost = L"usenet.nereid.pl";
constexpr const wchar_t* SettingsFileName = L"winbrowser-download-folder.txt";

enum : int
{
    IDC_DOWNLOAD_FILTER = 52001,
    IDC_DOWNLOAD_LIST,
    IDC_DOWNLOAD_FOLDER_LABEL,
    IDC_DOWNLOAD_FOLDER_TEXT,
    IDC_DOWNLOAD_FOLDER_BUTTON,
    IDC_DOWNLOAD_STATUS_LABEL,
    IDC_DOWNLOAD_STATUS,
    IDC_DOWNLOAD_PROGRESS_LABEL,
    IDC_DOWNLOAD_PROGRESS,
    IDC_EXTRACT_PROGRESS_LABEL,
    IDC_EXTRACT_PROGRESS,
    IDC_DOWNLOAD_REFRESH,
    IDC_DOWNLOAD_START,
    IDC_DOWNLOAD_CANCEL,
};

enum : UINT
{
    WM_ARCHIVE_LIST_READY = WM_APP + 50,
    WM_ARCHIVE_PROGRESS = WM_APP + 51,
    WM_ARCHIVE_DONE = WM_APP + 52,
};

enum class WorkStage
{
    Download,
    Extract
};

struct RemoteArchiveEntry
{
    std::wstring group;
    std::wstring fileName;
    std::wstring urlPath;
    std::wstring sizeText;
    std::wstring dateText;
    uint64_t bytes = 0;
};

struct ListResult
{
    std::vector<RemoteArchiveEntry> entries;
    std::wstring error;
};

struct WorkProgress
{
    WorkStage stage = WorkStage::Download;
    uint64_t current = 0;
    uint64_t total = 0;
    std::wstring text;
};

struct WorkDone
{
    bool ok = false;
    bool cancelled = false;
    std::wstring archivePath;
    std::wstring error;
};

std::wstring Utf8ToWide( const std::string& text )
{
    if( text.empty() ) return {};

    int len = MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), int( text.size() ), nullptr, 0 );
    if( len <= 0 )
    {
        len = MultiByteToWideChar( CP_UTF8, 0, text.c_str(), int( text.size() ), nullptr, 0 );
    }
    if( len <= 0 ) return {};

    std::wstring ret( len, L'\0' );
    MultiByteToWideChar( CP_UTF8, 0, text.c_str(), int( text.size() ), ret.data(), len );
    return ret;
}

std::string WideToUtf8( const std::wstring& text )
{
    if( text.empty() ) return {};

    const auto len = WideCharToMultiByte( CP_UTF8, 0, text.c_str(), int( text.size() ), nullptr, 0, nullptr, nullptr );
    if( len <= 0 ) return {};

    std::string ret( len, '\0' );
    WideCharToMultiByte( CP_UTF8, 0, text.c_str(), int( text.size() ), ret.data(), len, nullptr, nullptr );
    return ret;
}

std::wstring GetEnvironmentString( const wchar_t* name )
{
    const auto len = GetEnvironmentVariableW( name, nullptr, 0 );
    if( len == 0 ) return {};

    std::wstring ret( len, L'\0' );
    const auto written = GetEnvironmentVariableW( name, ret.data(), len );
    if( written == 0 ) return {};
    ret.resize( written );
    return ret;
}

std::wstring JoinPath( const std::wstring& base, const std::wstring& name )
{
    if( base.empty() ) return name;
    if( name.empty() ) return base;
    if( base.back() == L'\\' || base.back() == L'/' ) return base + name;
    return base + L'\\' + name;
}

std::wstring GetConfigFolder()
{
    auto base = GetEnvironmentString( L"APPDATA" );
    if( base.empty() ) base = GetEnvironmentString( L"LOCALAPPDATA" );
    if( base.empty() ) base = L".";
    return JoinPath( base, L"UsenetArchive" );
}

std::wstring DefaultDownloadWorkingFolder()
{
    auto base = GetEnvironmentString( L"LOCALAPPDATA" );
    if( base.empty() ) base = GetEnvironmentString( L"APPDATA" );
    if( base.empty() ) base = L".";
    return JoinPath( JoinPath( base, L"UsenetArchive" ), L"Archives" );
}

bool EnsureDirectoryExists( const std::wstring& path )
{
    if( path.empty() ) return false;
    const auto result = SHCreateDirectoryExW( nullptr, path.c_str(), nullptr );
    return result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS || result == ERROR_FILE_EXISTS;
}

bool PathExists( const std::wstring& path )
{
    return GetFileAttributesW( path.c_str() ) != INVALID_FILE_ATTRIBUTES;
}

bool IsDirectory( const std::wstring& path )
{
    const auto attr = GetFileAttributesW( path.c_str() );
    return attr != INVALID_FILE_ATTRIBUTES && ( attr & FILE_ATTRIBUTE_DIRECTORY ) != 0;
}

bool MoveFocusToNextDialogItem( HWND hwnd, bool previous )
{
    const auto root = GetAncestor( hwnd, GA_ROOT );
    if( !root ) return false;

    const auto target = GetNextDlgTabItem( root, hwnd, previous ? TRUE : FALSE );
    if( !target || target == hwnd ) return false;

    SetFocus( target );
    return true;
}

uint64_t FileSize( const std::wstring& path )
{
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if( !GetFileAttributesExW( path.c_str(), GetFileExInfoStandard, &data ) ) return 0;
    ULARGE_INTEGER value = {};
    value.LowPart = data.nFileSizeLow;
    value.HighPart = data.nFileSizeHigh;
    return value.QuadPart;
}

std::wstring Trim( std::wstring text )
{
    while( !text.empty() && iswspace( text.front() ) ) text.erase( text.begin() );
    while( !text.empty() && iswspace( text.back() ) ) text.pop_back();
    return text;
}

std::string Trim( std::string text )
{
    while( !text.empty() && isspace( static_cast<unsigned char>( text.front() ) ) ) text.erase( text.begin() );
    while( !text.empty() && isspace( static_cast<unsigned char>( text.back() ) ) ) text.pop_back();
    return text;
}

bool EndsWith( const std::string& text, const char* suffix )
{
    const auto suffixLen = strlen( suffix );
    return text.size() >= suffixLen && text.compare( text.size() - suffixLen, suffixLen, suffix ) == 0;
}

bool EndsWith( const std::wstring& text, const wchar_t* suffix )
{
    const auto suffixLen = wcslen( suffix );
    return text.size() >= suffixLen && text.compare( text.size() - suffixLen, suffixLen, suffix ) == 0;
}

bool StartsWithCaseInsensitiveAscii( const char* text, const char* prefix )
{
    while( *prefix )
    {
        if( std::tolower( static_cast<unsigned char>( *text ) ) != std::tolower( static_cast<unsigned char>( *prefix ) ) )
        {
            return false;
        }
        text++;
        prefix++;
    }
    return true;
}

std::wstring ToLower( std::wstring text )
{
    for( auto& ch : text )
    {
        ch = wchar_t( towlower( ch ) );
    }
    return text;
}

std::wstring HtmlDecodeToWide( std::string text )
{
    std::string out;
    out.reserve( text.size() );
    for( size_t i=0; i<text.size(); i++ )
    {
        if( text.compare( i, 5, "&amp;" ) == 0 )
        {
            out += '&';
            i += 4;
        }
        else if( text.compare( i, 6, "&quot;" ) == 0 )
        {
            out += '"';
            i += 5;
        }
        else if( text.compare( i, 4, "&lt;" ) == 0 )
        {
            out += '<';
            i += 3;
        }
        else if( text.compare( i, 4, "&gt;" ) == 0 )
        {
            out += '>';
            i += 3;
        }
        else
        {
            out += text[i];
        }
    }
    return Utf8ToWide( out );
}

uint64_t ParseSizeText( const std::wstring& text )
{
    const auto utf8 = WideToUtf8( text );
    char* end = nullptr;
    const auto value = std::strtod( utf8.c_str(), &end );
    if( end == utf8.c_str() ) return 0;

    while( end && *end == ' ' ) end++;
    double multiplier = 1.0;
    if( end )
    {
        if( StartsWithCaseInsensitiveAscii( end, "KiB" ) ) multiplier = 1024.0;
        else if( StartsWithCaseInsensitiveAscii( end, "MiB" ) ) multiplier = 1024.0 * 1024.0;
        else if( StartsWithCaseInsensitiveAscii( end, "GiB" ) ) multiplier = 1024.0 * 1024.0 * 1024.0;
    }
    return uint64_t( value * multiplier );
}

std::wstring FormatBytes( uint64_t bytes )
{
    const wchar_t* units[] = { L"B", L"KiB", L"MiB", L"GiB" };
    double value = double( bytes );
    size_t unit = 0;
    while( value >= 1024.0 && unit + 1 < sizeof( units ) / sizeof( *units ) )
    {
        value /= 1024.0;
        unit++;
    }

    wchar_t buf[64];
    if( unit == 0 )
    {
        swprintf( buf, sizeof( buf ) / sizeof( *buf ), L"%llu %s", static_cast<unsigned long long>( bytes ), units[unit] );
    }
    else
    {
        swprintf( buf, sizeof( buf ) / sizeof( *buf ), L"%.1f %s", value, units[unit] );
    }
    return buf;
}

int ProgressPercent( uint64_t current, uint64_t total )
{
    if( total == 0 ) return 0;
    return int( std::min<uint64_t>( 100, current * 100 / total ) );
}

class WinHttpHandle
{
public:
    WinHttpHandle() = default;
    explicit WinHttpHandle( HINTERNET handle )
        : m_handle( handle )
    {
    }

    ~WinHttpHandle()
    {
        if( m_handle ) WinHttpCloseHandle( m_handle );
    }

    WinHttpHandle( const WinHttpHandle& ) = delete;
    WinHttpHandle& operator=( const WinHttpHandle& ) = delete;

    WinHttpHandle( WinHttpHandle&& other ) noexcept
        : m_handle( other.m_handle )
    {
        other.m_handle = nullptr;
    }

    WinHttpHandle& operator=( WinHttpHandle&& other ) noexcept
    {
        if( this != &other )
        {
            if( m_handle ) WinHttpCloseHandle( m_handle );
            m_handle = other.m_handle;
            other.m_handle = nullptr;
        }
        return *this;
    }

    operator HINTERNET() const { return m_handle; }
    bool Valid() const { return m_handle != nullptr; }

private:
    HINTERNET m_handle = nullptr;
};

bool OpenArchiveRequest( const std::wstring& path, WinHttpHandle& session, WinHttpHandle& connect, WinHttpHandle& request, std::wstring& error )
{
    session = WinHttpHandle( WinHttpOpen( L"WinBrowser/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0 ) );
    if( !session.Valid() )
    {
        error = L"Could not initialize WinHTTP.";
        return false;
    }
    WinHttpSetTimeouts( session, 15000, 15000, 60000, 60000 );

    connect = WinHttpHandle( WinHttpConnect( session, ArchiveBaseHost, INTERNET_DEFAULT_HTTPS_PORT, 0 ) );
    if( !connect.Valid() )
    {
        error = L"Could not connect to usenet.nereid.pl.";
        return false;
    }

    request = WinHttpHandle( WinHttpOpenRequest( connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE ) );
    if( !request.Valid() )
    {
        error = L"Could not create the HTTP request.";
        return false;
    }

    if( !WinHttpSendRequest( request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0 ) ||
        !WinHttpReceiveResponse( request, nullptr ) )
    {
        error = L"Could not receive data from usenet.nereid.pl.";
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof( status );
    if( !WinHttpQueryHeaders( request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &statusSize, nullptr ) ||
        status < 200 || status >= 300 )
    {
        error = L"Server returned HTTP status " + std::to_wstring( status ) + L".";
        return false;
    }

    return true;
}

uint64_t QueryContentLength( HINTERNET request )
{
    wchar_t buf[64] = {};
    DWORD len = sizeof( buf );
    if( !WinHttpQueryHeaders( request, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, buf, &len, nullptr ) )
    {
        return 0;
    }
    return std::wcstoull( buf, nullptr, 10 );
}

bool HttpGetToString( const std::wstring& path, std::string& output, std::atomic_bool& cancel, std::wstring& error )
{
    WinHttpHandle session;
    WinHttpHandle connect;
    WinHttpHandle request;
    if( !OpenArchiveRequest( path, session, connect, request, error ) ) return false;

    output.clear();
    for( ;; )
    {
        if( cancel.load() )
        {
            error = L"Cancelled.";
            return false;
        }

        DWORD available = 0;
        if( !WinHttpQueryDataAvailable( request, &available ) )
        {
            error = L"Could not query HTTP data availability.";
            return false;
        }
        if( available == 0 ) break;

        std::vector<char> buffer( available );
        DWORD read = 0;
        if( !WinHttpReadData( request, buffer.data(), available, &read ) )
        {
            error = L"Could not read HTTP response data.";
            return false;
        }
        output.append( buffer.data(), buffer.data() + read );
    }
    return true;
}

bool HttpDownloadToFile(
    const std::wstring& path,
    const std::wstring& outputPath,
    std::atomic_bool& cancel,
    const std::function<void( uint64_t, uint64_t )>& progress,
    std::wstring& error
)
{
    WinHttpHandle session;
    WinHttpHandle connect;
    WinHttpHandle request;
    if( !OpenArchiveRequest( path, session, connect, request, error ) ) return false;

    const auto total = QueryContentLength( request );
    FILE* out = _wfopen( outputPath.c_str(), L"wb" );
    if( !out )
    {
        error = L"Could not create the downloaded archive file.";
        return false;
    }

    uint64_t downloaded = 0;
    bool ok = true;
    for( ;; )
    {
        if( cancel.load() )
        {
            error = L"Cancelled.";
            ok = false;
            break;
        }

        DWORD available = 0;
        if( !WinHttpQueryDataAvailable( request, &available ) )
        {
            error = L"Could not query HTTP data availability.";
            ok = false;
            break;
        }
        if( available == 0 ) break;

        std::vector<char> buffer( std::min<DWORD>( available, 1024 * 1024 ) );
        while( available > 0 )
        {
            const auto chunk = std::min<DWORD>( available, DWORD( buffer.size() ) );
            DWORD read = 0;
            if( !WinHttpReadData( request, buffer.data(), chunk, &read ) )
            {
                error = L"Could not read downloaded archive data.";
                ok = false;
                break;
            }
            if( read == 0 ) break;
            if( fwrite( buffer.data(), 1, read, out ) != read )
            {
                error = L"Could not write the downloaded archive file.";
                ok = false;
                break;
            }
            downloaded += read;
            available -= read;
            progress( downloaded, total );
        }
        if( !ok ) break;
    }

    fclose( out );
    if( !ok ) DeleteFileW( outputPath.c_str() );
    return ok;
}

std::wstring ExtractCellText( const std::string& row, const char* marker )
{
    const auto cell = row.find( marker );
    if( cell == std::string::npos ) return {};
    const auto start = row.find( '>', cell );
    if( start == std::string::npos ) return {};
    const auto end = row.find( "</td>", start + 1 );
    if( end == std::string::npos ) return {};
    return Trim( HtmlDecodeToWide( row.substr( start + 1, end - start - 1 ) ) );
}

std::vector<RemoteArchiveEntry> ParseArchiveListing( const std::string& html )
{
    std::vector<RemoteArchiveEntry> entries;

    size_t pos = 0;
    while( true )
    {
        const auto rowStart = html.find( "<tr", pos );
        if( rowStart == std::string::npos ) break;
        const auto rowEnd = html.find( "</tr>", rowStart );
        if( rowEnd == std::string::npos ) break;
        const auto row = html.substr( rowStart, rowEnd - rowStart );
        pos = rowEnd + 5;

        const auto hrefMarker = std::string( "<a href=\"" );
        const auto hrefStart = row.find( hrefMarker );
        if( hrefStart == std::string::npos ) continue;
        const auto hrefValueStart = hrefStart + hrefMarker.size();
        const auto hrefEnd = row.find( '"', hrefValueStart );
        if( hrefEnd == std::string::npos ) continue;

        const auto href = Trim( row.substr( hrefValueStart, hrefEnd - hrefValueStart ) );
        if( !EndsWith( href, ".usenet.xz" ) ) continue;

        auto fileName = HtmlDecodeToWide( href );
        auto group = fileName;
        if( EndsWith( group, L".usenet.xz" ) )
        {
            group.resize( group.size() - wcslen( L".usenet.xz" ) );
        }

        RemoteArchiveEntry entry;
        entry.group = std::move( group );
        entry.fileName = std::move( fileName );
        entry.urlPath = L"/" + entry.fileName;
        entry.sizeText = ExtractCellText( row, "<td class=\"size\">" );
        entry.dateText = ExtractCellText( row, "<td class=\"date\">" );
        entry.bytes = ParseSizeText( entry.sizeText );
        entries.emplace_back( std::move( entry ) );
    }

    return entries;
}

bool DecompressXzToFile(
    const std::wstring& sourcePath,
    const std::wstring& destinationPath,
    std::atomic_bool& cancel,
    const std::function<void( uint64_t, uint64_t )>& progress,
    std::wstring& error
)
{
    FILE* in = _wfopen( sourcePath.c_str(), L"rb" );
    if( !in )
    {
        error = L"Could not open the downloaded xz archive.";
        return false;
    }

    FILE* out = _wfopen( destinationPath.c_str(), L"wb" );
    if( !out )
    {
        fclose( in );
        error = L"Could not create the unpacked archive file.";
        return false;
    }

    lzma_stream stream = LZMA_STREAM_INIT;
    lzma_ret ret = lzma_stream_decoder( &stream, UINT64_MAX, LZMA_CONCATENATED );
    if( ret != LZMA_OK )
    {
        fclose( out );
        fclose( in );
        error = L"Could not initialize xz decompression.";
        DeleteFileW( destinationPath.c_str() );
        return false;
    }

    const auto total = FileSize( sourcePath );
    std::vector<uint8_t> inBuffer( 256 * 1024 );
    std::vector<uint8_t> outBuffer( 256 * 1024 );
    uint64_t consumed = 0;
    bool ok = true;
    bool done = false;

    stream.avail_in = 0;
    stream.next_in = nullptr;

    while( !done )
    {
        if( cancel.load() )
        {
            error = L"Cancelled.";
            ok = false;
            break;
        }

        if( stream.avail_in == 0 )
        {
            const auto read = fread( inBuffer.data(), 1, inBuffer.size(), in );
            if( ferror( in ) )
            {
                error = L"Could not read the downloaded xz archive.";
                ok = false;
                break;
            }
            stream.next_in = inBuffer.data();
            stream.avail_in = read;
            consumed += read;
        }

        stream.next_out = outBuffer.data();
        stream.avail_out = outBuffer.size();
        const auto action = feof( in ) && stream.avail_in == 0 ? LZMA_FINISH : LZMA_RUN;
        ret = lzma_code( &stream, action );

        const auto produced = outBuffer.size() - stream.avail_out;
        if( produced != 0 && fwrite( outBuffer.data(), 1, produced, out ) != produced )
        {
            error = L"Could not write the unpacked archive file.";
            ok = false;
            break;
        }

        progress( consumed - stream.avail_in, total );

        if( ret == LZMA_STREAM_END )
        {
            done = true;
        }
        else if( ret != LZMA_OK )
        {
            error = L"The downloaded xz archive could not be decompressed.";
            ok = false;
            break;
        }
    }

    lzma_end( &stream );
    fclose( out );
    fclose( in );

    if( !ok )
    {
        DeleteFileW( destinationPath.c_str() );
        return false;
    }
    return true;
}

std::wstring BrowseForFolder( HWND owner, const wchar_t* title )
{
    IFileDialog* dialog = nullptr;
    if( FAILED( CoCreateInstance( CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS( &dialog ) ) ) )
    {
        return {};
    }

    DWORD options = 0;
    dialog->GetOptions( &options );
    dialog->SetOptions( options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_PICKFOLDERS );
    dialog->SetTitle( title );

    std::wstring ret;
    if( SUCCEEDED( dialog->Show( owner ) ) )
    {
        IShellItem* item = nullptr;
        if( SUCCEEDED( dialog->GetResult( &item ) ) )
        {
            PWSTR path = nullptr;
            if( SUCCEEDED( item->GetDisplayName( SIGDN_FILESYSPATH, &path ) ) )
            {
                ret = path;
                CoTaskMemFree( path );
            }
            item->Release();
        }
    }

    dialog->Release();
    return ret;
}

void AddColumn( HWND list, int index, const wchar_t* title, int width )
{
    LVCOLUMNW column = {};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<LPWSTR>( title );
    column.cx = width;
    column.iSubItem = index;
    ListView_InsertColumn( list, index, &column );
}

LRESULT CALLBACK DialogEditSubclassProc( HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR )
{
    switch( msg )
    {
    case WM_GETDLGCODE:
        if( wParam == VK_TAB ) return 0;
        if( wParam == VK_ESCAPE ) return DefSubclassProc( hwnd, msg, wParam, lParam ) | DLGC_WANTMESSAGE;
        return DefSubclassProc( hwnd, msg, wParam, lParam ) & ~DLGC_WANTTAB;
    case WM_KEYDOWN:
    case WM_CHAR:
        if( wParam == VK_TAB )
        {
            const auto previous = ( GetKeyState( VK_SHIFT ) & 0x8000 ) != 0;
            if( MoveFocusToNextDialogItem( hwnd, previous ) ) return 0;
        }
        if( wParam == VK_ESCAPE )
        {
            if( const auto root = GetAncestor( hwnd, GA_ROOT ) )
            {
                SendMessageW( root, WM_COMMAND, MAKEWPARAM( IDCANCEL, 0 ), 0 );
                return 0;
            }
        }
        break;
    default:
        break;
    }

    return DefSubclassProc( hwnd, msg, wParam, lParam );
}

class ArchiveDownloadDialog
{
public:
    ArchiveDownloadDialog( HWND owner, HINSTANCE instance, HFONT font, std::wstring workingFolder )
        : m_owner( owner )
        , m_instance( instance )
        , m_font( font )
        , m_workingFolder( workingFolder.empty() ? DefaultDownloadWorkingFolder() : std::move( workingFolder ) )
    {
    }

    ~ArchiveDownloadDialog()
    {
        m_cancel.store( true );
        JoinWorker();
    }

    std::wstring ShowModal( std::wstring& selectedWorkingFolder )
    {
        RegisterClass();

        m_hwnd = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
            L"WinBrowserArchiveDownloadDialog",
            L"Download Usenet Archive",
            WS_CAPTION | WS_SYSMENU | WS_SIZEBOX | WS_CLIPCHILDREN,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            860,
            620,
            m_owner,
            nullptr,
            m_instance,
            this
        );
        if( !m_hwnd ) return {};

        EnableWindow( m_owner, FALSE );
        ShowWindow( m_hwnd, SW_SHOW );
        UpdateWindow( m_hwnd );
        StartListRefresh();

        MSG msg = {};
        while( m_running && GetMessageW( &msg, nullptr, 0, 0 ) > 0 )
        {
            if( HandleKeyboardMessage( msg ) )
            {
                continue;
            }
            if( !IsDialogMessageW( m_hwnd, &msg ) )
            {
                TranslateMessage( &msg );
                DispatchMessageW( &msg );
            }
        }

        if( m_owner )
        {
            EnableWindow( m_owner, TRUE );
            SetActiveWindow( m_owner );
        }

        selectedWorkingFolder = m_workingFolder;
        return m_resultArchivePath;
    }

private:
    static void RegisterClass()
    {
        static bool registered = false;
        if( registered ) return;

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof( wc );
        wc.lpfnWndProc = &ArchiveDownloadDialog::StaticWindowProc;
        wc.hInstance = GetModuleHandleW( nullptr );
        wc.lpszClassName = L"WinBrowserArchiveDownloadDialog";
        wc.hCursor = LoadCursorW( nullptr, IDC_ARROW );
        wc.hbrBackground = reinterpret_cast<HBRUSH>( COLOR_WINDOW + 1 );
        registered = RegisterClassExW( &wc ) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    static LRESULT CALLBACK StaticWindowProc( HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        auto self = reinterpret_cast<ArchiveDownloadDialog*>( GetWindowLongPtrW( hwnd, GWLP_USERDATA ) );
        if( msg == WM_NCCREATE )
        {
            auto create = reinterpret_cast<LPCREATESTRUCTW>( lParam );
            self = reinterpret_cast<ArchiveDownloadDialog*>( create->lpCreateParams );
            SetWindowLongPtrW( hwnd, GWLP_USERDATA, LONG_PTR( self ) );
            self->m_hwnd = hwnd;
        }
        return self ? self->WindowProc( msg, wParam, lParam ) : DefWindowProcW( hwnd, msg, wParam, lParam );
    }

    LRESULT WindowProc( UINT msg, WPARAM wParam, LPARAM lParam )
    {
        switch( msg )
        {
        case WM_CREATE:
            return CreateControls() ? 0 : -1;
        case WM_SIZE:
            Layout();
            return 0;
        case WM_COMMAND:
            if( HandleCommand( LOWORD( wParam ), HIWORD( wParam ), reinterpret_cast<HWND>( lParam ) ) ) return 0;
            break;
        case WM_NOTIFY:
            if( HandleNotify( reinterpret_cast<NMHDR*>( lParam ) ) ) return 0;
            break;
        case WM_SYSCHAR:
            if( HandleMnemonic( wchar_t( wParam ) ) ) return 0;
            break;
        case WM_KEYDOWN:
            if( HandleDialogKey( UINT( wParam ) ) ) return 0;
            break;
        case WM_ARCHIVE_LIST_READY:
            OnListReady( std::unique_ptr<ListResult>( reinterpret_cast<ListResult*>( lParam ) ) );
            return 0;
        case WM_ARCHIVE_PROGRESS:
            OnProgress( std::unique_ptr<WorkProgress>( reinterpret_cast<WorkProgress*>( lParam ) ) );
            return 0;
        case WM_ARCHIVE_DONE:
            OnDownloadDone( std::unique_ptr<WorkDone>( reinterpret_cast<WorkDone*>( lParam ) ) );
            return 0;
        case WM_CLOSE:
            if( m_busy )
            {
                m_cancel.store( true );
                SetStatus( L"Cancelling...", true );
                return 0;
            }
            DestroyWindow( m_hwnd );
            return 0;
        case WM_NCDESTROY:
            m_running = false;
            m_hwnd = nullptr;
            return 0;
        default:
            break;
        }
        return DefWindowProcW( m_hwnd, msg, wParam, lParam );
    }

    bool CreateControls()
    {
        m_filterLabel = CreateWindowExW( 0, L"STATIC", L"&Filter groups:", WS_CHILD | WS_VISIBLE, 0, 0, 100, 22, m_hwnd, nullptr, m_instance, nullptr );
        m_filterEdit = CreateWindowExW( WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 100, 24, m_hwnd, reinterpret_cast<HMENU>( IDC_DOWNLOAD_FILTER ), m_instance, nullptr );
        SendMessageW( m_filterEdit, EM_SETCUEBANNER, TRUE, LPARAM( L"Filter groups" ) );
        m_list = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            WC_LISTVIEWW,
            L"Archive group list",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
            0,
            0,
            100,
            100,
            m_hwnd,
            reinterpret_cast<HMENU>( IDC_DOWNLOAD_LIST ),
            m_instance,
            nullptr
        );
        m_groupListLabel = CreateWindowExW( 0, L"STATIC", L"&Group list:", WS_CHILD | WS_VISIBLE, 0, 0, 100, 22, m_hwnd, nullptr, m_instance, nullptr );
        m_folderLabel = CreateWindowExW( 0, L"STATIC", L"&Working folder:", WS_CHILD | WS_VISIBLE, 0, 0, 100, 22, m_hwnd, nullptr, m_instance, nullptr );
        m_folderText = CreateWindowExW( WS_EX_CLIENTEDGE, L"EDIT", m_workingFolder.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 100, 24, m_hwnd, reinterpret_cast<HMENU>( IDC_DOWNLOAD_FOLDER_TEXT ), m_instance, nullptr );
        SendMessageW( m_folderText, EM_SETREADONLY, TRUE, 0 );
        m_folderButton = CreateWindowExW( 0, L"BUTTON", L"C&hange...", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 100, 28, m_hwnd, reinterpret_cast<HMENU>( IDC_DOWNLOAD_FOLDER_BUTTON ), m_instance, nullptr );
        m_statusLabel = CreateWindowExW( 0, L"STATIC", L"&Status:", WS_CHILD | WS_VISIBLE, 0, 0, 100, 20, m_hwnd, reinterpret_cast<HMENU>( IDC_DOWNLOAD_STATUS_LABEL ), m_instance, nullptr );
        m_statusText = CreateWindowExW( WS_EX_CLIENTEDGE, L"EDIT", L"Loading archive list...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, 0, 0, 100, 40, m_hwnd, reinterpret_cast<HMENU>( IDC_DOWNLOAD_STATUS ), m_instance, nullptr );
        m_downloadProgressLabel = CreateWindowExW( 0, L"STATIC", L"Download progress", WS_CHILD | WS_VISIBLE, 0, 0, 100, 20, m_hwnd, reinterpret_cast<HMENU>( IDC_DOWNLOAD_PROGRESS_LABEL ), m_instance, nullptr );
        m_downloadProgress = CreateWindowExW( 0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE, 0, 0, 100, 22, m_hwnd, reinterpret_cast<HMENU>( IDC_DOWNLOAD_PROGRESS ), m_instance, nullptr );
        m_extractProgressLabel = CreateWindowExW( 0, L"STATIC", L"Unpack progress", WS_CHILD | WS_VISIBLE, 0, 0, 100, 20, m_hwnd, reinterpret_cast<HMENU>( IDC_EXTRACT_PROGRESS_LABEL ), m_instance, nullptr );
        m_extractProgress = CreateWindowExW( 0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE, 0, 0, 100, 22, m_hwnd, reinterpret_cast<HMENU>( IDC_EXTRACT_PROGRESS ), m_instance, nullptr );
        m_refreshButton = CreateWindowExW( 0, L"BUTTON", L"&Refresh List", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 110, 30, m_hwnd, reinterpret_cast<HMENU>( IDC_DOWNLOAD_REFRESH ), m_instance, nullptr );
        m_startButton = CreateWindowExW( 0, L"BUTTON", L"&Download and Open", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 150, 30, m_hwnd, reinterpret_cast<HMENU>( IDC_DOWNLOAD_START ), m_instance, nullptr );
        m_cancelButton = CreateWindowExW( 0, L"BUTTON", L"&Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 90, 30, m_hwnd, reinterpret_cast<HMENU>( IDC_DOWNLOAD_CANCEL ), m_instance, nullptr );

        const HWND controls[] = {
            m_filterLabel, m_filterEdit, m_groupListLabel, m_list, m_folderLabel, m_folderText, m_folderButton, m_statusLabel, m_statusText,
            m_downloadProgressLabel, m_downloadProgress, m_extractProgressLabel, m_extractProgress, m_refreshButton, m_startButton, m_cancelButton
        };
        for( auto control : controls )
        {
            if( !control ) return false;
            SendMessageW( control, WM_SETFONT, WPARAM( m_font ), TRUE );
        }

        ListView_SetUnicodeFormat( m_list, TRUE );
        ListView_SetExtendedListViewStyleEx( m_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER );
        AddColumn( m_list, 0, L"Group", 420 );
        AddColumn( m_list, 1, L"Size", 110 );
        AddColumn( m_list, 2, L"Date", 160 );

        SendMessageW( m_downloadProgress, PBM_SETRANGE, 0, MAKELPARAM( 0, 100 ) );
        SendMessageW( m_extractProgress, PBM_SETRANGE, 0, MAKELPARAM( 0, 100 ) );
        SetWindowSubclass( m_filterEdit, &DialogEditSubclassProc, 0, 0 );
        SetWindowSubclass( m_folderText, &DialogEditSubclassProc, 0, 0 );
        SetWindowSubclass( m_statusText, &DialogEditSubclassProc, 0, 0 );

        SetFocus( m_filterEdit );
        Layout();
        UpdateButtons();
        return true;
    }

    int Scale( int value ) const
    {
        HDC dc = GetDC( m_hwnd );
        const auto dpi = dc ? GetDeviceCaps( dc, LOGPIXELSX ) : 96;
        if( dc ) ReleaseDC( m_hwnd, dc );
        return MulDiv( value, dpi == 0 ? 96 : dpi, 96 );
    }

    void Layout()
    {
        if( !m_hwnd ) return;

        RECT client = {};
        GetClientRect( m_hwnd, &client );

        const int gap = Scale( 8 );
        const int labelHeight = Scale( 20 );
        const int inputHeight = Scale( 26 );
        const int buttonHeight = Scale( 30 );
        const int progressHeight = Scale( 22 );
        const int width = int( client.right - client.left );
        const int height = int( client.bottom - client.top );

        const int filterLabelWidth = Scale( 90 );
        SetWindowPos( m_filterLabel, nullptr, gap, gap + Scale( 3 ), filterLabelWidth, labelHeight, SWP_NOZORDER );
        SetWindowPos( m_filterEdit, nullptr, gap + filterLabelWidth + gap, gap, std::max( 100, width - filterLabelWidth - gap * 3 ), inputHeight, SWP_NOZORDER );

        const int bottomArea = Scale( 224 );
        const int groupLabelTop = gap + inputHeight + gap;
        SetWindowPos( m_groupListLabel, nullptr, gap, groupLabelTop, std::max( 0, width - gap * 2 ), labelHeight, SWP_NOZORDER );

        const int listTop = groupLabelTop + labelHeight;
        const int listHeight = std::max( Scale( 150 ), height - listTop - bottomArea - gap );
        SetWindowPos( m_list, nullptr, gap, listTop, std::max( 0, width - gap * 2 ), listHeight, SWP_NOZORDER );

        int y = listTop + listHeight + gap;
        const int folderLabelWidth = Scale( 110 );
        const int folderButtonWidth = Scale( 96 );
        SetWindowPos( m_folderLabel, nullptr, gap, y + Scale( 3 ), folderLabelWidth, labelHeight, SWP_NOZORDER );
        SetWindowPos( m_folderText, nullptr, gap + folderLabelWidth + gap, y, std::max( 100, width - folderLabelWidth - folderButtonWidth - gap * 4 ), inputHeight, SWP_NOZORDER );
        SetWindowPos( m_folderButton, nullptr, width - folderButtonWidth - gap, y - Scale( 1 ), folderButtonWidth, buttonHeight, SWP_NOZORDER );

        y += inputHeight + gap;
        SetWindowPos( m_statusLabel, nullptr, gap, y, std::max( 0, width - gap * 2 ), labelHeight, SWP_NOZORDER );

        y += labelHeight;
        SetWindowPos( m_statusText, nullptr, gap, y, std::max( 0, width - gap * 2 ), Scale( 38 ), SWP_NOZORDER );

        y += Scale( 42 );
        SetWindowPos( m_downloadProgressLabel, nullptr, gap, y, std::max( 0, width - gap * 2 ), labelHeight, SWP_NOZORDER );
        y += labelHeight;
        SetWindowPos( m_downloadProgress, nullptr, gap, y, std::max( 0, width - gap * 2 ), progressHeight, SWP_NOZORDER );

        y += progressHeight + gap;
        SetWindowPos( m_extractProgressLabel, nullptr, gap, y, std::max( 0, width - gap * 2 ), labelHeight, SWP_NOZORDER );
        y += labelHeight;
        SetWindowPos( m_extractProgress, nullptr, gap, y, std::max( 0, width - gap * 2 ), progressHeight, SWP_NOZORDER );

        y += progressHeight + gap;
        const int cancelWidth = Scale( 90 );
        const int startWidth = Scale( 160 );
        const int refreshWidth = Scale( 120 );
        SetWindowPos( m_cancelButton, nullptr, width - cancelWidth - gap, y, cancelWidth, buttonHeight, SWP_NOZORDER );
        SetWindowPos( m_startButton, nullptr, width - cancelWidth - startWidth - gap * 2, y, startWidth, buttonHeight, SWP_NOZORDER );
        SetWindowPos( m_refreshButton, nullptr, width - cancelWidth - startWidth - refreshWidth - gap * 3, y, refreshWidth, buttonHeight, SWP_NOZORDER );
    }

    bool HandleDialogKey( UINT key )
    {
        if( key == VK_F5 )
        {
            StartListRefresh();
            return true;
        }
        if( key == VK_ESCAPE )
        {
            SendMessageW( m_hwnd, WM_COMMAND, MAKEWPARAM( IDCANCEL, 0 ), 0 );
            return true;
        }
        if( GetFocus() == m_filterEdit && key == VK_DOWN )
        {
            FocusGroupList();
            return true;
        }
        return false;
    }

    bool HandleKeyboardMessage( const MSG& msg )
    {
        if( msg.hwnd != m_hwnd && !IsChild( m_hwnd, msg.hwnd ) ) return false;

        switch( msg.message )
        {
        case WM_SYSCHAR:
            return HandleMnemonic( wchar_t( msg.wParam ) );
        case WM_KEYDOWN:
            return HandleDialogKey( UINT( msg.wParam ) );
        default:
            break;
        }

        return false;
    }

    bool HandleMnemonic( wchar_t key )
    {
        if( ( GetKeyState( VK_CONTROL ) & 0x8000 ) != 0 ) return false;

        switch( towlower( key ) )
        {
        case L'f':
            SetFocus( m_filterEdit );
            return true;
        case L'g':
            FocusGroupList();
            return true;
        case L'w':
            SetFocus( m_folderText );
            return true;
        case L'h':
            ChangeWorkingFolder();
            return true;
        case L's':
            SetFocus( m_statusText );
            return true;
        case L'r':
            StartListRefresh();
            return true;
        case L'd':
            StartSelectedDownload();
            return true;
        case L'c':
            SendMessageW( m_hwnd, WM_COMMAND, MAKEWPARAM( IDCANCEL, 0 ), 0 );
            return true;
        default:
            break;
        }
        return false;
    }

    void FocusGroupList()
    {
        if( ListView_GetItemCount( m_list ) > 0 && SelectedRow() < 0 )
        {
            ListView_SetItemState( m_list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED );
        }
        SetFocus( m_list );
    }

    bool HandleCommand( int id, int code, HWND control )
    {
        if( control == m_filterEdit && code == EN_CHANGE )
        {
            RebuildList();
            return true;
        }

        switch( id )
        {
        case IDOK:
        case IDC_DOWNLOAD_START:
            StartSelectedDownload();
            return true;
        case IDC_DOWNLOAD_REFRESH:
            StartListRefresh();
            return true;
        case IDC_DOWNLOAD_FOLDER_BUTTON:
            ChangeWorkingFolder();
            return true;
        case IDC_DOWNLOAD_CANCEL:
        case IDCANCEL:
            if( m_busy )
            {
                m_cancel.store( true );
                SetStatus( L"Cancelling...", true );
            }
            else
            {
                DestroyWindow( m_hwnd );
            }
            return true;
        default:
            break;
        }
        return false;
    }

    bool HandleNotify( NMHDR* hdr )
    {
        if( hdr->idFrom != IDC_DOWNLOAD_LIST ) return false;

        switch( hdr->code )
        {
        case LVN_ITEMCHANGED:
            UpdateButtons();
            return false;
        case NM_DBLCLK:
        case NM_RETURN:
            StartSelectedDownload();
            return true;
        default:
            break;
        }
        return false;
    }

    int SelectedRow() const
    {
        return ListView_GetNextItem( m_list, -1, LVNI_SELECTED );
    }

    void SetStatus( const std::wstring& text, bool announce = false )
    {
        SetWindowTextW( m_statusText, text.c_str() );
        SendMessageW( m_statusText, EM_SETSEL, 0, 0 );
        NotifyWinEvent( EVENT_OBJECT_VALUECHANGE, m_statusText, OBJID_CLIENT, CHILDID_SELF );
        if( announce )
        {
            NotifyWinEvent( EVENT_SYSTEM_ALERT, m_statusText, OBJID_CLIENT, CHILDID_SELF );
        }
    }

    void SetProgressLabel( HWND label, const wchar_t* name, int percent )
    {
        const auto text = std::wstring( name ) + L": " + std::to_wstring( percent ) + L"%";
        SetWindowTextW( label, text.c_str() );
    }

    bool DidProgressPercentChange( WorkStage stage, int percent )
    {
        auto& lastPercent = stage == WorkStage::Download ? m_lastDownloadDisplayedPercent : m_lastExtractDisplayedPercent;
        if( lastPercent == percent ) return false;
        lastPercent = percent;
        return true;
    }

    bool ShouldAnnounceProgress( WorkStage stage, int percent )
    {
        auto& lastPercent = stage == WorkStage::Download ? m_lastDownloadAnnouncedPercent : m_lastExtractAnnouncedPercent;
        if( percent >= 100 )
        {
            if( lastPercent < 100 )
            {
                lastPercent = 100;
                return true;
            }
            return false;
        }

        const int bucket = percent <= 0 ? 0 : ( percent / 10 ) * 10;
        if( lastPercent < 0 || bucket >= lastPercent + 10 )
        {
            lastPercent = bucket;
            return true;
        }

        return false;
    }

    void UpdateButtons()
    {
        const auto canStart = !m_busy && SelectedRow() >= 0 && !m_filtered.empty();
        EnableWindow( m_filterEdit, !m_busy );
        EnableWindow( m_list, !m_busy );
        EnableWindow( m_folderButton, !m_busy );
        EnableWindow( m_refreshButton, !m_busy );
        EnableWindow( m_startButton, canStart );
        SetWindowTextW( m_cancelButton, m_busy ? L"&Cancel" : L"&Close" );
    }

    void SetBusy( bool busy )
    {
        m_busy = busy;
        UpdateButtons();
    }

    void JoinWorker()
    {
        if( m_worker.joinable() )
        {
            m_worker.join();
        }
    }

    void StartListRefresh()
    {
        if( m_busy ) return;

        JoinWorker();
        m_cancel.store( false );
        SetBusy( true );
        SetStatus( L"Loading archive list from https://usenet.nereid.pl/ ...", true );
        ListView_DeleteAllItems( m_list );
        SendMessageW( m_downloadProgress, PBM_SETPOS, 0, 0 );
        SendMessageW( m_extractProgress, PBM_SETPOS, 0, 0 );
        SetProgressLabel( m_downloadProgressLabel, L"Download progress", 0 );
        SetProgressLabel( m_extractProgressLabel, L"Unpack progress", 0 );

        const auto hwnd = m_hwnd;
        m_worker = std::thread( [hwnd, this]() {
            auto result = std::make_unique<ListResult>();
            std::string html;
            if( HttpGetToString( L"/", html, m_cancel, result->error ) )
            {
                result->entries = ParseArchiveListing( html );
                if( result->entries.empty() )
                {
                    result->error = L"No downloadable *.usenet.xz archives were found on the listing page.";
                }
            }
            PostMessageW( hwnd, WM_ARCHIVE_LIST_READY, 0, LPARAM( result.release() ) );
        } );
    }

    void OnListReady( std::unique_ptr<ListResult> result )
    {
        JoinWorker();
        SetBusy( false );

        if( !result->error.empty() )
        {
            SetStatus( result->error, true );
            MessageBoxW( m_hwnd, result->error.c_str(), L"Archive List", MB_OK | MB_ICONERROR );
            return;
        }

        m_entries = std::move( result->entries );
        RebuildList();
        SetStatus( L"Loaded " + std::to_wstring( m_entries.size() ) + L" downloadable archive groups.", true );
    }

    void RebuildList()
    {
        const auto filter = ToLower( GetWindowTextString( m_filterEdit ) );

        ListView_DeleteAllItems( m_list );
        m_filtered.clear();

        for( size_t i=0; i<m_entries.size(); i++ )
        {
            if( !filter.empty() && ToLower( m_entries[i].group ).find( filter ) == std::wstring::npos )
            {
                continue;
            }

            const auto row = int( m_filtered.size() );
            m_filtered.emplace_back( i );

            LVITEMW item = {};
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = row;
            item.pszText = const_cast<LPWSTR>( m_entries[i].group.c_str() );
            item.lParam = LPARAM( i );
            ListView_InsertItem( m_list, &item );
            ListView_SetItemText( m_list, row, 1, const_cast<LPWSTR>( m_entries[i].sizeText.c_str() ) );
            ListView_SetItemText( m_list, row, 2, const_cast<LPWSTR>( m_entries[i].dateText.c_str() ) );
        }

        if( !m_filtered.empty() )
        {
            ListView_SetItemState( m_list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED );
        }
        UpdateButtons();
    }

    std::wstring GetWindowTextString( HWND wnd ) const
    {
        const auto len = GetWindowTextLengthW( wnd );
        std::wstring ret( len + 1, L'\0' );
        if( len > 0 ) GetWindowTextW( wnd, ret.data(), len + 1 );
        ret.resize( len );
        return ret;
    }

    void ChangeWorkingFolder()
    {
        const auto folder = BrowseForFolder( m_hwnd, L"Select working folder for downloaded Usenet archives" );
        if( folder.empty() ) return;

        if( !EnsureDirectoryExists( folder ) || !IsDirectory( folder ) )
        {
            MessageBoxW( m_hwnd, L"Could not use the selected folder.", L"Working Folder", MB_OK | MB_ICONERROR );
            return;
        }

        m_workingFolder = folder;
        SetWindowTextW( m_folderText, m_workingFolder.c_str() );
    }

    void StartSelectedDownload()
    {
        if( m_busy ) return;

        const auto row = SelectedRow();
        if( row < 0 || size_t( row ) >= m_filtered.size() ) return;
        const auto entry = m_entries[m_filtered[row]];

        if( !EnsureDirectoryExists( m_workingFolder ) || !IsDirectory( m_workingFolder ) )
        {
            MessageBoxW( m_hwnd, L"Choose a valid working folder first.", L"Working Folder", MB_OK | MB_ICONERROR );
            return;
        }

        auto outputFileName = entry.fileName;
        if( EndsWith( outputFileName, L".xz" ) )
        {
            outputFileName.resize( outputFileName.size() - 3 );
        }

        const auto outputPath = JoinPath( m_workingFolder, outputFileName );
        if( PathExists( outputPath ) )
        {
            const auto choice = MessageBoxW(
                m_hwnd,
                ( L"The archive is already unpacked in the working folder:\n" + outputPath + L"\n\nOpen the existing archive?" ).c_str(),
                L"Archive Already Exists",
                MB_YESNOCANCEL | MB_ICONQUESTION
            );
            if( choice == IDYES )
            {
                m_resultArchivePath = outputPath;
                DestroyWindow( m_hwnd );
                return;
            }
            if( choice != IDNO ) return;
        }

        const auto downloadFolder = JoinPath( m_workingFolder, L"_downloads" );
        if( !EnsureDirectoryExists( downloadFolder ) )
        {
            MessageBoxW( m_hwnd, L"Could not create the temporary download folder.", L"Download Archive", MB_OK | MB_ICONERROR );
            return;
        }

        const auto tempPath = JoinPath( downloadFolder, entry.fileName );

        JoinWorker();
        m_cancel.store( false );
        SetBusy( true );
        SendMessageW( m_downloadProgress, PBM_SETPOS, 0, 0 );
        SendMessageW( m_extractProgress, PBM_SETPOS, 0, 0 );
        SetProgressLabel( m_downloadProgressLabel, L"Download progress", 0 );
        SetProgressLabel( m_extractProgressLabel, L"Unpack progress", 0 );
        m_lastDownloadDisplayedPercent = -1;
        m_lastExtractDisplayedPercent = -1;
        m_lastDownloadAnnouncedPercent = -1;
        m_lastExtractAnnouncedPercent = -1;
        SetStatus( L"Downloading " + entry.group + L"...", true );

        const auto hwnd = m_hwnd;
        m_worker = std::thread( [hwnd, this, entry, tempPath, outputPath]() {
            auto done = std::make_unique<WorkDone>();

            const auto postProgress = [&]( WorkStage stage, uint64_t current, uint64_t total, const std::wstring& text ) {
                auto progress = std::make_unique<WorkProgress>();
                progress->stage = stage;
                progress->current = current;
                progress->total = total;
                progress->text = text;
                PostMessageW( hwnd, WM_ARCHIVE_PROGRESS, 0, LPARAM( progress.release() ) );
            };

            std::wstring error;
            const auto downloadOk = HttpDownloadToFile(
                entry.urlPath,
                tempPath,
                m_cancel,
                [&]( uint64_t current, uint64_t total ) {
                    const auto effectiveTotal = total ? total : entry.bytes;
                    postProgress( WorkStage::Download, current, effectiveTotal, L"Downloading " + entry.group + L": " + FormatBytes( current ) + L" of " + ( effectiveTotal ? FormatBytes( effectiveTotal ) : entry.sizeText ) );
                },
                error
            );

            if( !downloadOk )
            {
                done->cancelled = m_cancel.load();
                done->error = error;
                DeleteFileW( tempPath.c_str() );
                PostMessageW( hwnd, WM_ARCHIVE_DONE, 0, LPARAM( done.release() ) );
                return;
            }

            postProgress( WorkStage::Download, 1, 1, L"Download complete. Unpacking " + entry.group + L"..." );

            const auto extractOk = DecompressXzToFile(
                tempPath,
                outputPath,
                m_cancel,
                [&]( uint64_t current, uint64_t total ) {
                    postProgress( WorkStage::Extract, current, total, L"Unpacking " + entry.group + L": " + FormatBytes( current ) + L" of " + ( total ? FormatBytes( total ) : entry.sizeText ) );
                },
                error
            );

            DeleteFileW( tempPath.c_str() );

            if( !extractOk )
            {
                done->cancelled = m_cancel.load();
                done->error = error;
                PostMessageW( hwnd, WM_ARCHIVE_DONE, 0, LPARAM( done.release() ) );
                return;
            }

            done->ok = true;
            done->archivePath = outputPath;
            PostMessageW( hwnd, WM_ARCHIVE_DONE, 0, LPARAM( done.release() ) );
        } );
    }

    void OnProgress( std::unique_ptr<WorkProgress> progress )
    {
        int percent = 0;
        if( progress->stage == WorkStage::Download )
        {
            percent = ProgressPercent( progress->current, progress->total );
        }
        else
        {
            percent = ProgressPercent( progress->current, progress->total );
        }

        const auto percentChanged = DidProgressPercentChange( progress->stage, percent );
        if( percentChanged )
        {
            if( progress->stage == WorkStage::Download )
            {
                SendMessageW( m_downloadProgress, PBM_SETPOS, percent, 0 );
                SetProgressLabel( m_downloadProgressLabel, L"Download progress", percent );
            }
            else
            {
                SendMessageW( m_extractProgress, PBM_SETPOS, percent, 0 );
                SetProgressLabel( m_extractProgressLabel, L"Unpack progress", percent );
            }
        }

        const auto shouldAnnounce = ShouldAnnounceProgress( progress->stage, percent );
        if( percentChanged || shouldAnnounce )
        {
            SetStatus( progress->text, shouldAnnounce );
        }
    }

    void OnDownloadDone( std::unique_ptr<WorkDone> done )
    {
        JoinWorker();
        SetBusy( false );

        if( done->ok )
        {
            SendMessageW( m_downloadProgress, PBM_SETPOS, 100, 0 );
            SendMessageW( m_extractProgress, PBM_SETPOS, 100, 0 );
            SetProgressLabel( m_downloadProgressLabel, L"Download progress", 100 );
            SetProgressLabel( m_extractProgressLabel, L"Unpack progress", 100 );
            m_resultArchivePath = done->archivePath;
            DestroyWindow( m_hwnd );
            return;
        }

        SendMessageW( m_downloadProgress, PBM_SETPOS, 0, 0 );
        SendMessageW( m_extractProgress, PBM_SETPOS, 0, 0 );
        SetProgressLabel( m_downloadProgressLabel, L"Download progress", 0 );
        SetProgressLabel( m_extractProgressLabel, L"Unpack progress", 0 );
        SetStatus( done->cancelled ? L"Operation cancelled." : done->error, true );
        if( !done->cancelled )
        {
            MessageBoxW( m_hwnd, done->error.c_str(), L"Download Archive", MB_OK | MB_ICONERROR );
        }
    }

    HWND m_owner = nullptr;
    HINSTANCE m_instance = nullptr;
    HFONT m_font = nullptr;
    HWND m_hwnd = nullptr;
    HWND m_filterLabel = nullptr;
    HWND m_filterEdit = nullptr;
    HWND m_groupListLabel = nullptr;
    HWND m_list = nullptr;
    HWND m_folderLabel = nullptr;
    HWND m_folderText = nullptr;
    HWND m_folderButton = nullptr;
    HWND m_statusLabel = nullptr;
    HWND m_statusText = nullptr;
    HWND m_downloadProgressLabel = nullptr;
    HWND m_downloadProgress = nullptr;
    HWND m_extractProgressLabel = nullptr;
    HWND m_extractProgress = nullptr;
    HWND m_refreshButton = nullptr;
    HWND m_startButton = nullptr;
    HWND m_cancelButton = nullptr;

    std::vector<RemoteArchiveEntry> m_entries;
    std::vector<size_t> m_filtered;
    std::thread m_worker;
    std::atomic_bool m_cancel { false };
    bool m_busy = false;
    bool m_running = true;
    int m_lastDownloadDisplayedPercent = -1;
    int m_lastExtractDisplayedPercent = -1;
    int m_lastDownloadAnnouncedPercent = -1;
    int m_lastExtractAnnouncedPercent = -1;

    std::wstring m_workingFolder;
    std::wstring m_resultArchivePath;
};

}  // namespace

std::wstring LoadDownloadWorkingFolder()
{
    const auto path = JoinPath( GetConfigFolder(), SettingsFileName );
    FILE* file = _wfopen( path.c_str(), L"rb" );
    if( !file ) return DefaultDownloadWorkingFolder();

    std::string data;
    char buffer[4096];
    while( const auto read = fread( buffer, 1, sizeof( buffer ), file ) )
    {
        data.append( buffer, buffer + read );
    }
    fclose( file );

    auto folder = Trim( Utf8ToWide( data ) );
    return folder.empty() ? DefaultDownloadWorkingFolder() : folder;
}

void SaveDownloadWorkingFolder( const std::wstring& folder )
{
    const auto configFolder = GetConfigFolder();
    EnsureDirectoryExists( configFolder );
    FILE* file = _wfopen( JoinPath( configFolder, SettingsFileName ).c_str(), L"wb" );
    if( !file ) return;
    const auto utf8 = WideToUtf8( folder );
    fwrite( utf8.data(), 1, utf8.size(), file );
    fclose( file );
}

std::wstring ShowArchiveDownloadDialog(
    HWND owner,
    HINSTANCE instance,
    HFONT font,
    const std::wstring& initialWorkingFolder,
    std::wstring& selectedWorkingFolder
)
{
    ArchiveDownloadDialog dialog( owner, instance, font, initialWorkingFolder );
    return dialog.ShowModal( selectedWorkingFolder );
}

}  // namespace winbrowser
