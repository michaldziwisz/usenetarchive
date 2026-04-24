#define NOMINMAX
#ifndef UNICODE
#  define UNICODE
#endif
#ifndef _UNICODE
#  define _UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <Richedit.h>
#include <objbase.h>
#include <shobjidl.h>
#include <shellapi.h>

#ifdef GetMessage
#  undef GetMessage
#endif

#include <algorithm>
#include <cwctype>
#include <memory>
#include <string>
#include <time.h>
#include <utility>
#include <vector>

#include "../common/ExpandingBuffer.hpp"
#include "../common/String.hpp"
#include "../libuat/Archive.hpp"
#include "../libuat/Galaxy.hpp"
#include "../libuat/PersistentStorage.hpp"
#include "../libuat/SearchEngine.hpp"

#include "ThreadListModel.hpp"

namespace
{

constexpr uint32_t InvalidMessage = ~uint32_t( 0 );

enum : int
{
    TabBrowse = 0,
    TabSearch = 1,
};

enum : int
{
    IDC_MAIN_TAB = 100,
    IDC_BROWSE_PANEL,
    IDC_SEARCH_PANEL,
    IDC_THREAD_LIST,
    IDC_SEARCH_LABEL,
    IDC_SEARCH_EDIT,
    IDC_SEARCH_BUTTON,
    IDC_SEARCH_HINT,
    IDC_RESULTS_LIST,
    IDC_DETAILS_LABEL,
    IDC_DETAILS_EDIT,
    IDC_BODY_LABEL,
    IDC_BODY_EDIT,
    IDC_STATUS,
};

enum : int
{
    ID_FILE_OPEN_FILE = 40001,
    ID_FILE_OPEN_FOLDER,
    ID_FILE_EXIT,
    ID_VIEW_BROWSE,
    ID_VIEW_SEARCH,
    ID_VIEW_TOGGLE_HEADERS,
    ID_NAV_FOCUS_LIST,
    ID_NAV_FOCUS_DETAILS,
    ID_NAV_FOCUS_BODY,
    ID_SEARCH_FOCUS,
    ID_SEARCH_EXECUTE,
    ID_HELP_SHORTCUTS,
};

enum : UINT
{
    WM_APP_SYNC_THREAD_PREVIEW = WM_APP + 1,
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

std::string NormalizeLineEndings( const std::string& text )
{
    std::string ret;
    ret.reserve( text.size() + 64 );

    char prev = '\0';
    for( const auto ch : text )
    {
        if( ch == '\n' && prev != '\r' )
        {
            ret += '\r';
        }
        ret += ch;
        prev = ch;
    }
    return ret;
}

std::wstring WideWithWindowsLineEndings( const std::string& text )
{
    return Utf8ToWide( NormalizeLineEndings( text ) );
}

std::wstring FormatDateTime( time_t value )
{
    if( value == 0 ) return L"Unknown";

    tm local = {};
#ifdef _MSC_VER
    localtime_s( &local, &value );
#else
    const auto localTime = localtime( &value );
    if( !localTime ) return L"Unknown";
    local = *localTime;
#endif

    wchar_t buf[64];
    if( wcsftime( buf, sizeof( buf ) / sizeof( *buf ), L"%Y-%m-%d %H:%M:%S", &local ) == 0 )
    {
        return L"Unknown";
    }
    return buf;
}

std::string FindHeaderValue( const char* message, const char* header )
{
    const auto headerLen = strlen( header );
    std::string value;
    bool collecting = false;

    auto line = message;
    while( *line )
    {
        auto end = line;
        while( *end && *end != '\n' ) end++;

        auto trimmedEnd = end;
        if( trimmedEnd > line && *( trimmedEnd - 1 ) == '\r' )
        {
            trimmedEnd--;
        }

        if( trimmedEnd == line )
        {
            break;
        }

        if( ( *line == ' ' || *line == '\t' ) && collecting )
        {
            auto continuation = line;
            while( continuation < trimmedEnd && ( *continuation == ' ' || *continuation == '\t' ) )
            {
                continuation++;
            }
            if( !value.empty() ) value += ' ';
            value.append( continuation, trimmedEnd );
        }
        else
        {
            collecting = false;
            if( size_t( trimmedEnd - line ) > headerLen + 1 && strnicmpl( line, header, int( headerLen ) ) == 0 && line[headerLen] == ':' )
            {
                auto start = line + headerLen + 1;
                while( start < trimmedEnd && ( *start == ' ' || *start == '\t' ) )
                {
                    start++;
                }
                value.assign( start, trimmedEnd );
                collecting = true;
            }
        }

        line = *end ? end + 1 : end;
    }

    return value;
}

const char* FindBodyStart( const char* message )
{
    if( const auto crlf = strstr( message, "\r\n\r\n" ) )
    {
        return crlf + 4;
    }
    if( const auto lf = strstr( message, "\n\n" ) )
    {
        return lf + 2;
    }
    return message;
}

std::wstring PairToWideString( const std::pair<const char*, uint64_t>& value )
{
    return Utf8ToWide( std::string( value.first, value.second ) );
}

bool StartsWithCaseInsensitive( const std::wstring& text, const wchar_t* prefix )
{
    const auto prefixLen = wcslen( prefix );
    if( text.size() < prefixLen ) return false;

    for( size_t i=0; i<prefixLen; i++ )
    {
        if( std::towlower( text[i] ) != std::towlower( prefix[i] ) )
        {
            return false;
        }
    }

    return true;
}

std::wstring GetWindowTextString( HWND wnd )
{
    const auto len = GetWindowTextLengthW( wnd );
    std::wstring ret( len + 1, L'\0' );
    if( len > 0 ) GetWindowTextW( wnd, ret.data(), len + 1 );
    ret.resize( len );
    return ret;
}

void SetListViewSelection( HWND list, int row )
{
    ListView_SetItemState( list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED );
    if( row >= 0 )
    {
        ListView_SetItemState( list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED );
        ListView_EnsureVisible( list, row, FALSE );
    }
}

int GetSingleSelectedRow( HWND list )
{
    return ListView_GetNextItem( list, -1, LVNI_SELECTED );
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

bool HandleEscapeForControl( HWND hwnd )
{
    const auto root = GetAncestor( hwnd, GA_ROOT );
    if( !root ) return true;

    if( GetDlgCtrlID( hwnd ) == IDC_BODY_EDIT )
    {
        SendMessageW( root, WM_COMMAND, MAKEWPARAM( ID_NAV_FOCUS_LIST, 0 ), 0 );
    }
    return true;
}

LRESULT CALLBACK ReadOnlyPaneSubclassProc( HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR )
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
            if( MoveFocusToNextDialogItem( hwnd, previous ) )
            {
                return 0;
            }
        }
        if( wParam == VK_ESCAPE )
        {
            HandleEscapeForControl( hwnd );
            return 0;
        }
        break;
    default:
        break;
    }

    return DefSubclassProc( hwnd, msg, wParam, lParam );
}

LRESULT CALLBACK EscapeKeySubclassProc( HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR )
{
    switch( msg )
    {
    case WM_GETDLGCODE:
        if( wParam == VK_ESCAPE ) return DefSubclassProc( hwnd, msg, wParam, lParam ) | DLGC_WANTMESSAGE;
        break;
    case WM_KEYDOWN:
    case WM_CHAR:
        if( wParam == VK_ESCAPE )
        {
            HandleEscapeForControl( hwnd );
            return 0;
        }
        break;
    default:
        break;
    }

    return DefSubclassProc( hwnd, msg, wParam, lParam );
}

LRESULT CALLBACK ForwardPanelMessagesSubclassProc( HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR )
{
    switch( msg )
    {
    case WM_COMMAND:
    case WM_NOTIFY:
        if( const auto parent = GetParent( hwnd ) )
        {
            return SendMessageW( parent, msg, wParam, lParam );
        }
        break;
    default:
        break;
    }

    return DefSubclassProc( hwnd, msg, wParam, lParam );
}

}  // namespace

class WinBrowserWindow
{
public:
    explicit WinBrowserWindow( std::string initialPath )
        : m_initialPath( std::move( initialPath ) )
    {
        m_storage.Preload();
    }

    ~WinBrowserWindow()
    {
        if( m_accel )
        {
            DestroyAcceleratorTable( m_accel );
        }
    }

    HWND Handle() const { return m_hwnd; }
    HACCEL Accelerators() const { return m_accel; }

    bool Create( HINSTANCE instance, int showCmd )
    {
        m_instance = instance;

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof( wc );
        wc.lpfnWndProc = &WinBrowserWindow::StaticWindowProc;
        wc.hInstance = instance;
        wc.lpszClassName = L"UsenetArchiveWinBrowser";
        wc.hCursor = LoadCursorW( nullptr, IDC_ARROW );
        wc.hbrBackground = reinterpret_cast<HBRUSH>( COLOR_WINDOW + 1 );
        wc.hIcon = LoadIconW( nullptr, IDI_APPLICATION );
        wc.hIconSm = LoadIconW( nullptr, IDI_APPLICATION );
        wc.style = CS_HREDRAW | CS_VREDRAW;
        if( !RegisterClassExW( &wc ) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS )
        {
            return false;
        }

        m_font = reinterpret_cast<HFONT>( GetStockObject( DEFAULT_GUI_FONT ) );
        m_accel = CreateAccelerators();

        m_hwnd = CreateWindowExW(
            WS_EX_CONTROLPARENT,
            wc.lpszClassName,
            L"Usenet Archive Browser",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            1400,
            900,
            nullptr,
            CreateMainMenu(),
            instance,
            this
        );
        if( !m_hwnd ) return false;

        ShowWindow( m_hwnd, showCmd );
        UpdateWindow( m_hwnd );
        return true;
    }

private:
    static LRESULT CALLBACK StaticWindowProc( HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        auto self = reinterpret_cast<WinBrowserWindow*>( GetWindowLongPtrW( hwnd, GWLP_USERDATA ) );
        if( msg == WM_NCCREATE )
        {
            auto create = reinterpret_cast<LPCREATESTRUCTW>( lParam );
            self = reinterpret_cast<WinBrowserWindow*>( create->lpCreateParams );
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
            if( !CreateControls() ) return -1;
            m_storage.WaitPreload();
            FinishStartup();
            return 0;
        case WM_SIZE:
            Layout();
            return 0;
        case WM_COMMAND:
            if( HandleCommand( LOWORD( wParam ), HIWORD( wParam ), reinterpret_cast<HWND>( lParam ) ) ) return 0;
            break;
        case WM_NOTIFY:
            if( HandleNotify( reinterpret_cast<NMHDR*>( lParam ) ) ) return 0;
            break;
        case WM_APP_SYNC_THREAD_PREVIEW:
            SyncSelectedThreadPreview();
            return 0;
        case WM_SETFOCUS:
            FocusPrimaryControl();
            return 0;
        case WM_CLOSE:
            PersistState();
            DestroyWindow( m_hwnd );
            return 0;
        case WM_DESTROY:
            PostQuitMessage( 0 );
            return 0;
        default:
            break;
        }

        return DefWindowProcW( m_hwnd, msg, wParam, lParam );
    }

    HACCEL CreateAccelerators()
    {
        const ACCEL entries[] = {
            { FCONTROL | FVIRTKEY, 'O', ID_FILE_OPEN_FILE },
            { FCONTROL | FSHIFT | FVIRTKEY, 'O', ID_FILE_OPEN_FOLDER },
            { FCONTROL | FVIRTKEY, '1', ID_VIEW_BROWSE },
            { FCONTROL | FVIRTKEY, '2', ID_VIEW_SEARCH },
            { FCONTROL | FVIRTKEY, 'F', ID_SEARCH_FOCUS },
            { FVIRTKEY, VK_F5, ID_SEARCH_EXECUTE },
            { FCONTROL | FVIRTKEY, 'L', ID_NAV_FOCUS_LIST },
            { FCONTROL | FVIRTKEY, 'D', ID_NAV_FOCUS_DETAILS },
            { FCONTROL | FVIRTKEY, 'B', ID_NAV_FOCUS_BODY },
            { FCONTROL | FVIRTKEY, 'H', ID_VIEW_TOGGLE_HEADERS },
        };
        return CreateAcceleratorTableW( const_cast<LPACCEL>( entries ), int( sizeof( entries ) / sizeof( *entries ) ) );
    }

    HMENU CreateMainMenu()
    {
        HMENU menu = CreateMenu();

        HMENU fileMenu = CreatePopupMenu();
        AppendMenuW( fileMenu, MF_STRING, ID_FILE_OPEN_FILE, L"&Open File...\tCtrl+O" );
        AppendMenuW( fileMenu, MF_STRING, ID_FILE_OPEN_FOLDER, L"Open &Folder...\tCtrl+Shift+O" );
        AppendMenuW( fileMenu, MF_SEPARATOR, 0, nullptr );
        AppendMenuW( fileMenu, MF_STRING, ID_FILE_EXIT, L"E&xit" );
        AppendMenuW( menu, MF_POPUP, UINT_PTR( fileMenu ), L"&File" );

        HMENU viewMenu = CreatePopupMenu();
        AppendMenuW( viewMenu, MF_STRING, ID_VIEW_BROWSE, L"&Browse Tab\tCtrl+1" );
        AppendMenuW( viewMenu, MF_STRING, ID_VIEW_SEARCH, L"&Search Tab\tCtrl+2" );
        AppendMenuW( viewMenu, MF_SEPARATOR, 0, nullptr );
        AppendMenuW( viewMenu, MF_STRING, ID_VIEW_TOGGLE_HEADERS, L"Show Full &Headers\tCtrl+H" );
        AppendMenuW( menu, MF_POPUP, UINT_PTR( viewMenu ), L"&View" );

        HMENU navMenu = CreatePopupMenu();
        AppendMenuW( navMenu, MF_STRING, ID_NAV_FOCUS_LIST, L"Focus &List\tCtrl+L" );
        AppendMenuW( navMenu, MF_STRING, ID_NAV_FOCUS_DETAILS, L"Focus &Summary\tCtrl+D" );
        AppendMenuW( navMenu, MF_STRING, ID_NAV_FOCUS_BODY, L"Focus &Body\tCtrl+B" );
        AppendMenuW( menu, MF_POPUP, UINT_PTR( navMenu ), L"&Navigate" );

        HMENU helpMenu = CreatePopupMenu();
        AppendMenuW( helpMenu, MF_STRING, ID_HELP_SHORTCUTS, L"&Shortcuts" );
        AppendMenuW( menu, MF_POPUP, UINT_PTR( helpMenu ), L"&Help" );

        return menu;
    }

    bool CreateControls()
    {
        LoadLibraryW( L"Msftedit.dll" );

        m_tab = CreateWindowExW(
            0,
            WC_TABCONTROLW,
            L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            0,
            0,
            100,
            100,
            m_hwnd,
            reinterpret_cast<HMENU>( IDC_MAIN_TAB ),
            m_instance,
            nullptr
        );
        if( !m_tab ) return false;
        SendMessageW( m_tab, WM_SETFONT, WPARAM( m_font ), TRUE );
        SetWindowSubclass( m_tab, &EscapeKeySubclassProc, 0, 0 );

        TCITEMW item = {};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<LPWSTR>( L"Browse" );
        TabCtrl_InsertItem( m_tab, TabBrowse, &item );
        item.pszText = const_cast<LPWSTR>( L"Search" );
        TabCtrl_InsertItem( m_tab, TabSearch, &item );

        m_browsePanel = CreateWindowExW(
            WS_EX_CONTROLPARENT,
            L"STATIC",
            nullptr,
            WS_CHILD | WS_VISIBLE,
            0,
            0,
            100,
            100,
            m_hwnd,
            reinterpret_cast<HMENU>( IDC_BROWSE_PANEL ),
            m_instance,
            nullptr
        );
        m_searchPanel = CreateWindowExW(
            WS_EX_CONTROLPARENT,
            L"STATIC",
            nullptr,
            WS_CHILD,
            0,
            0,
            100,
            100,
            m_hwnd,
            reinterpret_cast<HMENU>( IDC_SEARCH_PANEL ),
            m_instance,
            nullptr
        );
        SetWindowSubclass( m_browsePanel, &ForwardPanelMessagesSubclassProc, 0, 0 );
        SetWindowSubclass( m_searchPanel, &ForwardPanelMessagesSubclassProc, 0, 0 );

        m_threadList = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            WC_LISTVIEWW,
            L"Thread list",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
            0,
            0,
            100,
            100,
            m_browsePanel,
            reinterpret_cast<HMENU>( IDC_THREAD_LIST ),
            m_instance,
            nullptr
        );
        SetWindowSubclass( m_threadList, &EscapeKeySubclassProc, 0, 0 );

        m_searchLabel = CreateWindowExW( 0, L"STATIC", L"Search query:", WS_CHILD | WS_VISIBLE, 0, 0, 100, 24, m_searchPanel, reinterpret_cast<HMENU>( IDC_SEARCH_LABEL ), m_instance, nullptr );
        m_searchEdit = CreateWindowExW( WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 100, 24, m_searchPanel, reinterpret_cast<HMENU>( IDC_SEARCH_EDIT ), m_instance, nullptr );
        m_searchButton = CreateWindowExW( 0, L"BUTTON", L"Search", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 90, 24, m_searchPanel, reinterpret_cast<HMENU>( IDC_SEARCH_BUTTON ), m_instance, nullptr );
        m_searchHint = CreateWindowExW( 0, L"STATIC", L"Syntax: +must  -exclude  from:author  subject:topic  \"exact phrase\"  prefix*", WS_CHILD | WS_VISIBLE, 0, 0, 100, 40, m_searchPanel, reinterpret_cast<HMENU>( IDC_SEARCH_HINT ), m_instance, nullptr );
        m_resultsList = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            WC_LISTVIEWW,
            L"Search results",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
            0,
            0,
            100,
            100,
            m_searchPanel,
            reinterpret_cast<HMENU>( IDC_RESULTS_LIST ),
            m_instance,
            nullptr
        );
        SetWindowSubclass( m_searchEdit, &EscapeKeySubclassProc, 0, 0 );
        SetWindowSubclass( m_searchButton, &EscapeKeySubclassProc, 0, 0 );
        SetWindowSubclass( m_resultsList, &EscapeKeySubclassProc, 0, 0 );

        m_detailsLabel = CreateWindowExW( 0, L"STATIC", L"Message summary", WS_CHILD | WS_VISIBLE, 0, 0, 100, 24, m_hwnd, reinterpret_cast<HMENU>( IDC_DETAILS_LABEL ), m_instance, nullptr );
        m_detailsEdit = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
            0,
            0,
            100,
            100,
            m_hwnd,
            reinterpret_cast<HMENU>( IDC_DETAILS_EDIT ),
            m_instance,
            nullptr
        );
        SendMessageW( m_detailsEdit, EM_SETREADONLY, TRUE, 0 );
        SetWindowSubclass( m_detailsEdit, &ReadOnlyPaneSubclassProc, 0, 0 );

        m_bodyLabel = CreateWindowExW( 0, L"STATIC", L"Message body", WS_CHILD | WS_VISIBLE, 0, 0, 100, 24, m_hwnd, reinterpret_cast<HMENU>( IDC_BODY_LABEL ), m_instance, nullptr );
        m_bodyEdit = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            MSFTEDIT_CLASS,
            L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
            0,
            0,
            100,
            100,
            m_hwnd,
            reinterpret_cast<HMENU>( IDC_BODY_EDIT ),
            m_instance,
            nullptr
        );
        SendMessageW( m_bodyEdit, EM_SETREADONLY, TRUE, 0 );
        SendMessageW( m_bodyEdit, EM_AUTOURLDETECT, TRUE, 0 );
        SendMessageW( m_bodyEdit, EM_SETEVENTMASK, 0, ENM_LINK );
        SetWindowSubclass( m_bodyEdit, &ReadOnlyPaneSubclassProc, 0, 0 );

        m_status = CreateWindowExW(
            0,
            STATUSCLASSNAMEW,
            L"",
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0,
            0,
            0,
            0,
            m_hwnd,
            reinterpret_cast<HMENU>( IDC_STATUS ),
            m_instance,
            nullptr
        );

        const HWND controls[] = {
            m_browsePanel, m_searchPanel, m_threadList, m_searchLabel, m_searchEdit, m_searchButton, m_searchHint,
            m_resultsList, m_detailsLabel, m_detailsEdit, m_bodyLabel, m_bodyEdit, m_status
        };
        for( auto wnd : controls )
        {
            if( !wnd ) return false;
            SendMessageW( wnd, WM_SETFONT, WPARAM( m_font ), TRUE );
        }

        SendMessageW( m_searchEdit, EM_SETCUEBANNER, TRUE, LPARAM( L"Search the archive..." ) );

        ConfigureListViews();
        UpdateViewMenuState();
        UpdateTabVisibility();
        UpdateStatusText( L"Open a packaged archive file or archive directory to begin browsing." );
        ClearDisplayedMessage();
        return true;
    }

    void ConfigureListViews()
    {
        const auto apply = []( HWND list ) {
            ListView_SetUnicodeFormat( list, TRUE );
            ListView_SetExtendedListViewStyleEx( list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER );
        };
        apply( m_threadList );
        apply( m_resultsList );

        AddColumn( m_threadList, 0, L"Subject", 520 );
        AddColumn( m_threadList, 1, L"Author", 180 );
        AddColumn( m_threadList, 2, L"Date", 160 );

        AddColumn( m_resultsList, 0, L"Subject", 420 );
        AddColumn( m_resultsList, 1, L"Author", 180 );
        AddColumn( m_resultsList, 2, L"Date", 160 );
        AddColumn( m_resultsList, 3, L"Rank", 70 );
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

    void FinishStartup()
    {
        std::string initial = m_initialPath;
        if( initial.empty() )
        {
            initial = m_storage.ReadLastOpenArchive();
        }
        if( !initial.empty() )
        {
            LoadPath( initial );
        }
        else
        {
            UpdateTitle();
        }
    }

    void Layout()
    {
        if( !m_hwnd ) return;

        RECT client = {};
        GetClientRect( m_hwnd, &client );

        SendMessageW( m_status, WM_SIZE, 0, 0 );
        RECT statusRect = {};
        GetWindowRect( m_status, &statusRect );
        const auto statusHeight = statusRect.bottom - statusRect.top;
        client.bottom -= statusHeight;
        const int clientWidth = int( client.right );
        const int clientHeight = int( client.bottom );

        const int gap = Scale( 8 );
        const int leftMin = Scale( 320 );
        const int rightMin = Scale( 420 );

        int leftWidth = clientWidth * 42 / 100;
        leftWidth = std::max( leftWidth, leftMin );
        leftWidth = std::min( leftWidth, std::max( leftMin, clientWidth - rightMin - gap * 2 ) );
        leftWidth = std::max( leftWidth, 0 );

        SetWindowPos( m_tab, nullptr, gap, gap, leftWidth, std::max( 0, clientHeight - gap * 2 ), SWP_NOZORDER );

        RECT tabRect = {};
        GetWindowRect( m_tab, &tabRect );
        MapWindowPoints( nullptr, m_hwnd, reinterpret_cast<POINT*>( &tabRect ), 2 );
        RECT pageRect = tabRect;
        TabCtrl_AdjustRect( m_tab, FALSE, &pageRect );
        const int pageWidth = std::max( 0, int( pageRect.right - pageRect.left ) );
        const int pageHeight = std::max( 0, int( pageRect.bottom - pageRect.top ) );

        SetWindowPos( m_browsePanel, nullptr, int( pageRect.left ), int( pageRect.top ), pageWidth, pageHeight, SWP_NOZORDER );
        SetWindowPos( m_searchPanel, nullptr, int( pageRect.left ), int( pageRect.top ), pageWidth, pageHeight, SWP_NOZORDER );

        RECT panel = {};
        GetClientRect( m_browsePanel, &panel );
        SetWindowPos( m_threadList, nullptr, 0, 0, int( panel.right ), int( panel.bottom ), SWP_NOZORDER );

        RECT searchPanel = {};
        GetClientRect( m_searchPanel, &searchPanel );
        const int searchPanelWidth = int( searchPanel.right );
        const int searchPanelHeight = int( searchPanel.bottom );
        const int labelHeight = Scale( 20 );
        const int inputHeight = Scale( 26 );
        const int hintHeight = Scale( 34 );
        const int buttonWidth = Scale( 90 );
        const int labelWidth = Scale( 90 );
        const int innerGap = Scale( 6 );

        SetWindowPos( m_searchLabel, nullptr, 0, 0, labelWidth, labelHeight, SWP_NOZORDER );
        SetWindowPos( m_searchEdit, nullptr, labelWidth + innerGap, 0, std::max( 100, searchPanelWidth - labelWidth - buttonWidth - innerGap * 2 ), inputHeight, SWP_NOZORDER );
        SetWindowPos( m_searchButton, nullptr, std::max( 0, searchPanelWidth - buttonWidth ), 0, buttonWidth, inputHeight, SWP_NOZORDER );
        SetWindowPos( m_searchHint, nullptr, 0, inputHeight + innerGap, searchPanelWidth, hintHeight, SWP_NOZORDER );
        SetWindowPos( m_resultsList, nullptr, 0, inputHeight + hintHeight + innerGap * 2, searchPanelWidth, std::max( 0, searchPanelHeight - inputHeight - hintHeight - innerGap * 2 ), SWP_NOZORDER );

        const int rightX = gap + leftWidth + gap;
        const int rightWidth = std::max( 0, clientWidth - rightX - gap );
        const int labelY = gap;
        const int summaryY = labelY + labelHeight;
        const int summaryHeight = Scale( 130 );
        const int bodyLabelY = summaryY + summaryHeight + innerGap;
        const int bodyY = bodyLabelY + labelHeight;

        SetWindowPos( m_detailsLabel, nullptr, rightX, labelY, rightWidth, labelHeight, SWP_NOZORDER );
        SetWindowPos( m_detailsEdit, nullptr, rightX, summaryY, rightWidth, summaryHeight, SWP_NOZORDER );
        SetWindowPos( m_bodyLabel, nullptr, rightX, bodyLabelY, rightWidth, labelHeight, SWP_NOZORDER );
        SetWindowPos( m_bodyEdit, nullptr, rightX, bodyY, rightWidth, std::max( 0, clientHeight - bodyY - gap ), SWP_NOZORDER );
    }

    int Scale( int value ) const
    {
        HDC dc = GetDC( m_hwnd );
        const auto dpi = dc ? GetDeviceCaps( dc, LOGPIXELSX ) : 96;
        if( dc ) ReleaseDC( m_hwnd, dc );
        return MulDiv( value, dpi == 0 ? 96 : dpi, 96 );
    }

    bool HandleCommand( int id, int code, HWND control )
    {
        if( control == m_searchButton && code == BN_CLICKED )
        {
            ExecuteSearch();
            return true;
        }

        const auto focus = GetFocus();

        switch( id )
        {
        case IDOK:
            if( focus == m_threadList || focus == m_detailsEdit )
            {
                SetFocus( m_bodyEdit );
                return true;
            }
            if( focus == m_resultsList )
            {
                ActivateSelectedSearchResult();
                return true;
            }
            if( focus == m_searchEdit || focus == m_searchButton )
            {
                ExecuteSearch();
                return true;
            }
            break;
        case IDCANCEL:
            if( focus == m_bodyEdit )
            {
                FocusPrimaryControl();
            }
            return true;
        case ID_FILE_OPEN_FILE:
            OpenPathDialog( false );
            return true;
        case ID_FILE_OPEN_FOLDER:
            OpenPathDialog( true );
            return true;
        case ID_FILE_EXIT:
            SendMessageW( m_hwnd, WM_CLOSE, 0, 0 );
            return true;
        case ID_VIEW_BROWSE:
            SetActiveTab( TabBrowse, true );
            return true;
        case ID_VIEW_SEARCH:
            SetActiveTab( TabSearch, true );
            return true;
        case ID_VIEW_TOGGLE_HEADERS:
            m_showFullHeaders = !m_showFullHeaders;
            UpdateViewMenuState();
            UpdateMessagePane();
            return true;
        case ID_NAV_FOCUS_LIST:
            FocusPrimaryControl();
            return true;
        case ID_NAV_FOCUS_DETAILS:
            SetFocus( m_detailsEdit );
            return true;
        case ID_NAV_FOCUS_BODY:
            SetFocus( m_bodyEdit );
            return true;
        case ID_SEARCH_FOCUS:
            SetActiveTab( TabSearch, false );
            SetFocus( m_searchEdit );
            return true;
        case ID_SEARCH_EXECUTE:
            ExecuteSearch();
            return true;
        case ID_HELP_SHORTCUTS:
            ShowShortcuts();
            return true;
        default:
            break;
        }
        return false;
    }

    bool HandleNotify( NMHDR* hdr )
    {
        if( hdr->idFrom == IDC_MAIN_TAB && hdr->code == TCN_SELCHANGE )
        {
            SetActiveTab( TabCtrl_GetCurSel( m_tab ), true );
            return true;
        }

        if( hdr->idFrom == IDC_THREAD_LIST )
        {
            switch( hdr->code )
            {
            case LVN_ITEMCHANGED:
                HandleThreadSelectionChanged( reinterpret_cast<const NMLISTVIEW*>( hdr ) );
                return true;
            case LVN_KEYDOWN:
                return HandleThreadKeyDown( reinterpret_cast<const NMLVKEYDOWN*>( hdr ) );
            case NM_CLICK:
            case NM_SETFOCUS:
                PostSyncThreadPreview();
                break;
            case NM_DBLCLK:
            case NM_RETURN:
                SetFocus( m_bodyEdit );
                return true;
            default:
                break;
            }
        }

        if( hdr->idFrom == IDC_RESULTS_LIST )
        {
            switch( hdr->code )
            {
            case LVN_ITEMCHANGED:
                HandleSearchSelectionChanged( reinterpret_cast<const NMLISTVIEW*>( hdr ) );
                return true;
            case LVN_KEYDOWN:
                return HandleSearchKeyDown( reinterpret_cast<const NMLVKEYDOWN*>( hdr ) );
            case NM_DBLCLK:
            case NM_RETURN:
                ActivateSelectedSearchResult();
                return true;
            default:
                break;
            }
        }

        if( hdr->idFrom == IDC_BODY_EDIT && hdr->code == EN_LINK )
        {
            HandleBodyLink( reinterpret_cast<ENLINK*>( hdr ) );
            return true;
        }

        return false;
    }

    std::wstring BuildThreadItemText( size_t row )
    {
        const auto data = m_threadModel.GetRowData( row );

        auto subject = Utf8ToWide( m_archive->GetSubject( data.messageIndex ) );
        if( subject.empty() ) subject = L"(no subject)";

        std::wstring text( size_t( std::max( data.depth, 0 ) ) * 2, L' ' );
        text += subject;
        if( data.expandable )
        {
            text += data.expanded ? L" [expanded]" : L" [collapsed]";
        }
        if( !data.visited )
        {
            text += L" [unread]";
        }
        return text;
    }

    uint32_t GetThreadMessage( int row ) const
    {
        if( row < 0 || size_t( row ) >= m_threadModel.VisibleCount() ) return InvalidMessage;
        return m_threadModel.MessageAt( size_t( row ) );
    }

    void UpdateThreadRow( int row )
    {
        const auto message = GetThreadMessage( row );
        if( message == InvalidMessage ) return;

        LVITEMW item = {};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = row;
        item.iSubItem = 0;

        const auto text = BuildThreadItemText( size_t( row ) );
        item.pszText = const_cast<LPWSTR>( text.c_str() );
        item.lParam = LPARAM( message );

        if( ListView_GetItemCount( m_threadList ) <= row )
        {
            ListView_InsertItem( m_threadList, &item );
        }
        else
        {
            ListView_SetItem( m_threadList, &item );
        }

        auto author = Utf8ToWide( m_archive->GetRealName( message ) );
        auto date = FormatDateTime( m_archive->GetDate( message ) );
        ListView_SetItemText( m_threadList, row, 1, author.data() );
        ListView_SetItemText( m_threadList, row, 2, date.data() );
    }

    void RebuildThreadTree()
    {
        ListView_DeleteAllItems( m_threadList );
        if( !m_archive ) return;

        for( size_t row=0; row<m_threadModel.VisibleCount(); row++ )
        {
            UpdateThreadRow( int( row ) );
        }
    }

    void EnsureThreadPathVisible( uint32_t message )
    {
        if( !m_archive || message == InvalidMessage || message >= m_archive->NumberOfMessages() ) return;

        std::vector<uint32_t> path;
        for( auto parent = m_archive->GetParent( message ); parent != -1; parent = m_archive->GetParent( uint32_t( parent ) ) )
        {
            path.emplace_back( uint32_t( parent ) );
        }
        std::reverse( path.begin(), path.end() );

        bool changed = false;
        for( const auto current : path )
        {
            if( m_threadModel.CanExpand( current ) && !m_threadModel.IsExpanded( current ) )
            {
                changed |= m_threadModel.Expand( current, false );
            }
        }

        if( changed )
        {
            RebuildThreadTree();
        }

        const auto row = m_threadModel.VisibleRowOf( message );
        if( row >= 0 )
        {
            ListView_EnsureVisible( m_threadList, row, FALSE );
        }
    }

    void ExpandThreadSubtree( uint32_t message )
    {
        if( !m_threadModel.CanExpand( message ) ) return;
        if( m_threadModel.Expand( message, true ) )
        {
            RebuildThreadTree();
        }
    }

    void RefreshThreadItemText( uint32_t message )
    {
        const auto row = m_threadModel.VisibleRowOf( message );
        if( row >= 0 )
        {
            UpdateThreadRow( row );
        }
    }

    void PostSyncThreadPreview()
    {
        PostMessageW( m_hwnd, WM_APP_SYNC_THREAD_PREVIEW, 0, 0 );
    }

    void SyncSelectedThreadPreview()
    {
        if( m_ignoreThreadSelection ) return;

        const auto row = GetSingleSelectedRow( m_threadList );
        const auto message = GetThreadMessage( row );
        if( message == InvalidMessage || message == m_selectedMessage ) return;

        DisplayMessage( message, true );
    }

    void HandleThreadSelectionChanged( const NMLISTVIEW* info )
    {
        if( m_ignoreThreadSelection ) return;
        if( ( info->uChanged & LVIF_STATE ) == 0 ) return;
        if( ( info->uNewState & ( LVIS_SELECTED | LVIS_FOCUSED ) ) == 0 ) return;

        const auto message = GetThreadMessage( info->iItem );
        if( message == InvalidMessage ) return;
        if( message != m_selectedMessage ) DisplayMessage( message, true );
    }

    void HandleSearchSelectionChanged( const NMLISTVIEW* info )
    {
        if( m_ignoreSearchSelection ) return;
        if( ( info->uChanged & LVIF_STATE ) == 0 ) return;
        if( ( info->uNewState & ( LVIS_SELECTED | LVIS_FOCUSED ) ) == 0 ) return;
        if( info->iItem < 0 || size_t( info->iItem ) >= m_searchData.results.size() ) return;

        const auto message = m_searchData.results[info->iItem].postid;
        if( message != m_selectedMessage ) DisplayMessage( message, true );
    }

    bool HandleThreadKeyDown( const NMLVKEYDOWN* info )
    {
        const auto row = GetSingleSelectedRow( m_threadList );
        const auto message = GetThreadMessage( row );
        if( message == InvalidMessage ) return false;

        switch( info->wVKey )
        {
        case VK_RIGHT:
            if( ( GetKeyState( VK_CONTROL ) & 0x8000 ) != 0 )
            {
                ExpandThreadSubtree( message );
                SelectThreadMessage( message, false );
                return true;
            }
            if( m_threadModel.CanExpand( message ) )
            {
                if( !m_threadModel.IsExpanded( message ) )
                {
                    if( m_threadModel.Expand( message, false ) )
                    {
                        RebuildThreadTree();
                        SelectThreadMessage( message, false );
                    }
                }
                else if( row + 1 < int( m_threadModel.VisibleCount() ) )
                {
                    const auto child = GetThreadMessage( row + 1 );
                    if( child != InvalidMessage && m_archive->GetParent( child ) == int32_t( message ) )
                    {
                        m_ignoreThreadSelection = true;
                        SetListViewSelection( m_threadList, row + 1 );
                        m_ignoreThreadSelection = false;
                        DisplayMessage( child, true );
                    }
                }
                return true;
            }
            break;
        case VK_LEFT:
        {
            if( m_threadModel.IsExpanded( message ) )
            {
                if( m_threadModel.Collapse( message ) )
                {
                    RebuildThreadTree();
                    SelectThreadMessage( message, false );
                }
                return true;
            }
            const auto parent = m_archive->GetParent( message );
            if( parent != -1 )
            {
                SelectThreadMessage( uint32_t( parent ), false );
                return true;
            }
            break;
        }
        case VK_UP:
        case VK_DOWN:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
            PostSyncThreadPreview();
            break;
        case VK_MULTIPLY:
            ExpandThreadSubtree( message );
            SelectThreadMessage( message, false );
            return true;
        case VK_RETURN:
            SetFocus( m_bodyEdit );
            return true;
        default:
            break;
        }
        return false;
    }

    bool HandleSearchKeyDown( const NMLVKEYDOWN* info )
    {
        if( info->wVKey == VK_RETURN )
        {
            ActivateSelectedSearchResult();
            return true;
        }
        return false;
    }

    void HandleBodyLink( const ENLINK* link )
    {
        if( link->msg == WM_KEYUP )
        {
            if( link->wParam != VK_RETURN && link->wParam != VK_SPACE ) return;
        }
        else if( link->msg != WM_LBUTTONUP )
        {
            return;
        }

        TEXTRANGEW range = {};
        std::vector<wchar_t> text( link->chrg.cpMax - link->chrg.cpMin + 1 );
        range.chrg = link->chrg;
        range.lpstrText = text.data();
        SendMessageW( m_bodyEdit, EM_GETTEXTRANGE, 0, LPARAM( &range ) );

        std::wstring target( text.data() );
        if( target.empty() ) return;
        if( TryOpenInternalNewsLink( target ) ) return;

        ShellExecuteW( m_hwnd, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL );
    }

    bool TryOpenInternalNewsLink( const std::wstring& target )
    {
        if( !m_archive ) return false;
        if( !StartsWithCaseInsensitive( target, L"news:" ) ) return false;

        const auto msgid = target.substr( 5 );
        if( msgid.empty() || msgid.find( L'/' ) != std::wstring::npos ) return false;

        const auto utf8 = WideToUtf8( msgid );
        if( utf8.empty() ) return false;

        uint8_t packed[2048];
        m_archive->PackMsgId( utf8.c_str(), packed );
        const auto index = m_archive->GetMessageIndex( packed );
        if( index == -1 ) return false;

        SetActiveTab( TabBrowse, false );
        RevealMessage( uint32_t( index ) );
        SelectThreadMessage( uint32_t( index ), true );
        return true;
    }

    void ShowShortcuts()
    {
        MessageBoxW(
            m_hwnd,
            L"Ctrl+O  Open packaged archive file\n"
            L"Ctrl+Shift+O  Open archive folder or galaxy folder\n"
            L"Ctrl+1 / Ctrl+2  Switch between Browse and Search tabs\n"
            L"Ctrl+F  Focus search box\n"
            L"F5  Run search\n"
            L"Ctrl+L / Ctrl+D / Ctrl+B  Focus thread list, summary, or message body\n"
            L"Ctrl+H  Toggle full raw headers in the body pane\n"
            L"Right Arrow  Expand selected thread item or move to first reply\n"
            L"Ctrl+Right Arrow  Expand selected subtree recursively\n"
            L"Left Arrow  Collapse selected thread item or move to parent\n"
            L"Enter on thread list  Move focus to message body\n"
            L"Enter on search results  Open result in Browse tab",
            L"Keyboard Shortcuts",
            MB_OK | MB_ICONINFORMATION
        );
    }

    void OpenPathDialog( bool folder )
    {
        auto path = BrowseForPath( folder );
        if( path.empty() ) return;
        LoadPath( WideToUtf8( path ) );
    }

    std::wstring BrowseForPath( bool folder )
    {
        IFileDialog* dialog = nullptr;
        if( FAILED( CoCreateInstance( CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS( &dialog ) ) ) )
        {
            return {};
        }

        DWORD options = 0;
        dialog->GetOptions( &options );
        options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
        if( folder )
        {
            options |= FOS_PICKFOLDERS;
        }
        dialog->SetOptions( options );
        dialog->SetTitle( folder ? L"Open archive or galaxy folder" : L"Open packaged archive file" );

        if( !folder )
        {
            const COMDLG_FILTERSPEC filters[] = {
                { L"Usenet archive files", L"*.usenet;*.uarc;*.pkg;*.*" },
            };
            dialog->SetFileTypes( UINT( sizeof( filters ) / sizeof( *filters ) ), filters );
        }

        std::wstring ret;
        if( SUCCEEDED( dialog->Show( m_hwnd ) ) )
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

    bool LoadPath( const std::string& path )
    {
        PersistState();

        auto galaxy = std::unique_ptr<Galaxy>( Galaxy::Open( path ) );
        std::shared_ptr<Archive> archive;
        std::string activeArchivePath = path;

        if( galaxy )
        {
            const auto& available = galaxy->GetAvailableArchives();
            if( available.empty() )
            {
                ShowError( L"The selected galaxy does not contain any available archives." );
                return false;
            }

            auto active = m_storage.ReadLastOpenGalaxyArchive();
            if( active < 0 || active >= int( galaxy->GetNumberOfArchives() ) || !galaxy->IsArchiveAvailable( active ) )
            {
                active = available.front();
            }
            archive = galaxy->GetArchive( active );
            activeArchivePath = galaxy->GetArchiveFilename( active );
        }
        else
        {
            archive.reset( Archive::Open( path ) );
        }

        if( !archive )
        {
            ShowError( L"The selected path is not a valid archive or galaxy." );
            return false;
        }

        m_galaxy = std::move( galaxy );
        m_archive = std::move( archive );
        m_sourcePath = path;
        m_activeArchivePath = activeArchivePath;
        m_searchEngine = std::make_unique<SearchEngine>( *m_archive );
        m_searchData.results.clear();
        m_searchData.matched.clear();
        ListView_DeleteAllItems( m_resultsList );

        m_threadModel.Reset( *m_archive, m_storage );
        RebuildThreadTree();

        RestoreHistoryOrSelectFirst();
        UpdateTitle();
        UpdateStatusText(
            L"Loaded archive with " + std::to_wstring( m_archive->NumberOfMessages() ) +
            L" messages across " + std::to_wstring( m_archive->NumberOfTopLevel() ) +
            L" top-level threads. Right expands, Left collapses, Ctrl+F starts search."
        );
        return true;
    }

    void RestoreHistoryOrSelectFirst()
    {
        m_selectedMessage = InvalidMessage;
        ClearDisplayedMessage();

        uint32_t target = InvalidMessage;
        if( m_storage.ReadArticleHistory( m_activeArchivePath.c_str() ) )
        {
            const auto& history = m_storage.GetArticleHistory();
            if( !history.empty() && m_archive->NumberOfMessages() != 0 )
            {
                target = std::min<uint32_t>( uint32_t( m_archive->NumberOfMessages() - 1 ), history.back() );
            }
        }

        if( target == InvalidMessage && m_archive->NumberOfTopLevel() > 0 )
        {
            target = m_archive->GetTopLevel().ptr[0];
        }

        if( target != InvalidMessage )
        {
            RevealMessage( target );
            SelectThreadMessage( target, false );
        }
    }

    void RevealMessage( uint32_t message )
    {
        EnsureThreadPathVisible( message );
    }

    void SelectThreadMessage( uint32_t message, bool focus )
    {
        if( message == InvalidMessage ) return;

        EnsureThreadPathVisible( message );
        const auto row = m_threadModel.VisibleRowOf( message );
        if( row < 0 ) return;

        m_ignoreThreadSelection = true;
        SetListViewSelection( m_threadList, row );
        m_ignoreThreadSelection = false;

        DisplayMessage( message, true );
        if( focus ) FocusThreadList();
    }

    void ActivateSelectedSearchResult()
    {
        const auto row = GetSingleSelectedRow( m_resultsList );
        if( row < 0 || size_t( row ) >= m_searchData.results.size() ) return;

        const auto message = m_searchData.results[row].postid;
        SetActiveTab( TabBrowse, false );
        RevealMessage( message );
        SelectThreadMessage( message, true );
    }

    void DisplayMessage( uint32_t message, bool addHistory )
    {
        if( !m_archive || message == InvalidMessage || message >= m_archive->NumberOfMessages() ) return;

        const auto previous = m_selectedMessage;
        m_selectedMessage = message;
        m_threadModel.MarkVisited( message );

        if( addHistory )
        {
            const auto& history = m_storage.GetArticleHistory();
            if( history.empty() || history.back() != message )
            {
                m_storage.AddToHistory( message );
            }
        }

        UpdateMessagePane();
        if( previous != InvalidMessage ) RefreshThreadItemText( previous );
        RefreshThreadItemText( message );
    }

    void UpdateMessagePane()
    {
        if( !m_archive || m_selectedMessage == InvalidMessage )
        {
            ClearDisplayedMessage();
            return;
        }

        const auto raw = m_archive->GetMessage( m_selectedMessage, m_messageBuffer );
        if( !raw )
        {
            ClearDisplayedMessage();
            UpdateStatusText( L"Selected message could not be loaded." );
            return;
        }
        const auto date = FormatDateTime( m_archive->GetDate( m_selectedMessage ) );
        const auto from = std::string( m_archive->GetFrom( m_selectedMessage ) );
        const auto realName = std::string( m_archive->GetRealName( m_selectedMessage ) );

        char unpack[2048];
        m_archive->UnpackMsgId( m_archive->GetMessageId( m_selectedMessage ), unpack );

        auto root = m_selectedMessage;
        while( m_archive->GetParent( root ) != -1 )
        {
            root = uint32_t( m_archive->GetParent( root ) );
        }

        std::string summary =
            "Subject: " + std::string( m_archive->GetSubject( m_selectedMessage ) ) + "\n" +
            "From: " + realName + " <" + from + ">\n" +
            "Date: " + WideToUtf8( date ) + "\n" +
            "Message-ID: " + std::string( unpack ) + "\n" +
            "Newsgroups: " + FindHeaderValue( raw, "newsgroups" ) + "\n" +
            "Archive: " + m_activeArchivePath + "\n" +
            "Thread size: " + std::to_string( m_archive->GetTotalChildrenCount( root ) );

        const auto body = m_showFullHeaders ? std::string( raw ) : std::string( FindBodyStart( raw ) );

        SetWindowTextW( m_detailsEdit, WideWithWindowsLineEndings( summary ).c_str() );
        SetWindowTextW( m_bodyEdit, WideWithWindowsLineEndings( body ).c_str() );

        UpdateTitle();
    }

    void ClearDisplayedMessage()
    {
        SetWindowTextW( m_detailsEdit, L"No message selected." );
        SetWindowTextW( m_bodyEdit, L"" );
    }

    void ExecuteSearch()
    {
        if( !m_archive || !m_searchEngine ) return;

        const auto queryWide = GetWindowTextString( m_searchEdit );
        const auto query = WideToUtf8( queryWide );
        if( query.empty() )
        {
            UpdateStatusText( L"Enter a query to search the archive." );
            return;
        }

        m_searchData = m_searchEngine->Search(
            query.c_str(),
            SearchEngine::SF_AdjacentWords |
            SearchEngine::SF_FuzzySearch |
            SearchEngine::SF_SetLogic
        );

        ListView_DeleteAllItems( m_resultsList );

        for( size_t i=0; i<m_searchData.results.size(); i++ )
        {
            const auto& result = m_searchData.results[i];
            LVITEMW item = {};
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = int( i );
            const auto subject = Utf8ToWide( m_archive->GetSubject( result.postid ) );
            item.pszText = const_cast<LPWSTR>( subject.c_str() );
            item.lParam = LPARAM( result.postid );
            ListView_InsertItem( m_resultsList, &item );

            auto author = Utf8ToWide( m_archive->GetRealName( result.postid ) );
            auto date = FormatDateTime( m_archive->GetDate( result.postid ) );
            auto rank = std::to_wstring( int( result.rank * 100.f ) ) + L"%";
            ListView_SetItemText( m_resultsList, int( i ), 1, author.data() );
            ListView_SetItemText( m_resultsList, int( i ), 2, date.data() );
            ListView_SetItemText( m_resultsList, int( i ), 3, rank.data() );
        }

        if( !m_searchData.results.empty() )
        {
            m_ignoreSearchSelection = true;
            SetListViewSelection( m_resultsList, 0 );
            m_ignoreSearchSelection = false;
            DisplayMessage( m_searchData.results[0].postid, true );
            UpdateStatusText( L"Search returned " + std::to_wstring( m_searchData.results.size() ) + L" results." );
        }
        else
        {
            UpdateStatusText( L"No search results for: " + queryWide );
        }
    }

    void PersistState()
    {
        if( !m_archive || m_activeArchivePath.empty() ) return;

        m_storage.WriteArticleHistory( m_activeArchivePath.c_str() );
        if( m_galaxy )
        {
            m_storage.WriteLastOpenArchive( m_sourcePath.c_str() );
            m_storage.WriteLastOpenGalaxyArchive( m_galaxy->GetActiveArchive() );
        }
        else
        {
            m_storage.WriteLastOpenArchive( m_activeArchivePath.c_str() );
        }
    }

    void SetActiveTab( int tab, bool focusPrimary )
    {
        tab = std::clamp<int>( tab, TabBrowse, TabSearch );
        m_currentTab = tab;
        TabCtrl_SetCurSel( m_tab, tab );
        UpdateTabVisibility();
        UpdateViewMenuState();
        if( focusPrimary ) FocusPrimaryControl();
    }

    void UpdateTabVisibility()
    {
        ShowWindow( m_browsePanel, m_currentTab == TabBrowse ? SW_SHOW : SW_HIDE );
        ShowWindow( m_searchPanel, m_currentTab == TabSearch ? SW_SHOW : SW_HIDE );
    }

    void UpdateViewMenuState()
    {
        auto menu = GetMenu( m_hwnd );
        if( !menu ) return;

        CheckMenuRadioItem( menu, ID_VIEW_BROWSE, ID_VIEW_SEARCH, m_currentTab == TabBrowse ? ID_VIEW_BROWSE : ID_VIEW_SEARCH, MF_BYCOMMAND );
        CheckMenuItem( menu, ID_VIEW_TOGGLE_HEADERS, MF_BYCOMMAND | ( m_showFullHeaders ? MF_CHECKED : MF_UNCHECKED ) );
    }

    void FocusPrimaryControl()
    {
        if( m_currentTab == TabBrowse )
        {
            FocusThreadList();
        }
        else
        {
            if( !m_searchData.results.empty() )
            {
                SetFocus( m_resultsList );
            }
            else
            {
                SetFocus( m_searchEdit );
            }
        }
    }

    void FocusThreadList()
    {
        auto row = m_selectedMessage != InvalidMessage ? m_threadModel.VisibleRowOf( m_selectedMessage ) : -1;
        if( row < 0 )
        {
            row = GetSingleSelectedRow( m_threadList );
        }

        if( row >= 0 )
        {
            m_ignoreThreadSelection = true;
            SetListViewSelection( m_threadList, row );
            m_ignoreThreadSelection = false;
        }

        SetFocus( m_threadList );
    }

    void UpdateStatusText( const std::wstring& text )
    {
        SendMessageW( m_status, SB_SETTEXTW, 0, LPARAM( text.c_str() ) );
    }

    void UpdateTitle()
    {
        std::wstring title = L"Usenet Archive Browser";
        if( m_archive )
        {
            title += L" - ";
            title += PairToWideString( m_archive->GetArchiveName() );
            if( m_selectedMessage != InvalidMessage )
            {
                title += L" - ";
                title += Utf8ToWide( m_archive->GetSubject( m_selectedMessage ) );
            }
        }
        SetWindowTextW( m_hwnd, title.c_str() );
    }

    void ShowError( const std::wstring& message )
    {
        MessageBoxW( m_hwnd, message.c_str(), L"Usenet Archive Browser", MB_OK | MB_ICONERROR );
    }

    HINSTANCE m_instance = nullptr;
    HWND m_hwnd = nullptr;
    HWND m_tab = nullptr;
    HWND m_browsePanel = nullptr;
    HWND m_searchPanel = nullptr;
    HWND m_threadList = nullptr;
    HWND m_searchLabel = nullptr;
    HWND m_searchEdit = nullptr;
    HWND m_searchButton = nullptr;
    HWND m_searchHint = nullptr;
    HWND m_resultsList = nullptr;
    HWND m_detailsLabel = nullptr;
    HWND m_detailsEdit = nullptr;
    HWND m_bodyLabel = nullptr;
    HWND m_bodyEdit = nullptr;
    HWND m_status = nullptr;
    HFONT m_font = nullptr;
    HACCEL m_accel = nullptr;

    PersistentStorage m_storage;
    std::unique_ptr<Galaxy> m_galaxy;
    std::shared_ptr<Archive> m_archive;
    std::unique_ptr<SearchEngine> m_searchEngine;
    ThreadListModel m_threadModel;
    SearchData m_searchData;
    ExpandingBuffer m_messageBuffer;

    std::string m_initialPath;
    std::string m_sourcePath;
    std::string m_activeArchivePath;
    uint32_t m_selectedMessage = InvalidMessage;
    bool m_showFullHeaders = false;
    bool m_ignoreThreadSelection = false;
    bool m_ignoreSearchSelection = false;
    int m_currentTab = TabBrowse;
};

int WINAPI wWinMain( HINSTANCE instance, HINSTANCE, PWSTR, int showCmd )
{
    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof( icc );
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx( &icc );

    if( FAILED( CoInitializeEx( nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE ) ) )
    {
        return 1;
    }

    int argc = 0;
    auto argv = CommandLineToArgvW( GetCommandLineW(), &argc );
    std::string initialPath;
    if( argv && argc > 1 )
    {
        initialPath = WideToUtf8( argv[1] );
    }
    if( argv ) LocalFree( argv );

    WinBrowserWindow app( std::move( initialPath ) );
    if( !app.Create( instance, showCmd ) )
    {
        CoUninitialize();
        return 1;
    }

    MSG msg = {};
    while( GetMessageW( &msg, nullptr, 0, 0 ) > 0 )
    {
        if( !TranslateAcceleratorW( app.Handle(), app.Accelerators(), &msg ) &&
            !IsDialogMessageW( app.Handle(), &msg ) )
        {
            TranslateMessage( &msg );
            DispatchMessageW( &msg );
        }
    }

    CoUninitialize();
    return int( msg.wParam );
}
