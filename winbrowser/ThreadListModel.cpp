#include "ThreadListModel.hpp"

#include <algorithm>

#include "../libuat/Archive.hpp"
#include "../libuat/PersistentStorage.hpp"

ThreadListModel::ThreadListModel()
    : m_archive( nullptr )
    , m_storage( nullptr )
{
}

void ThreadListModel::Reset( const Archive& archive, PersistentStorage& storage )
{
    m_archive = &archive;
    m_storage = &storage;

    const auto messageCount = archive.NumberOfMessages();
    m_visible.clear();
    m_visible.reserve( archive.NumberOfTopLevel() );
    m_visibleIndex.assign( messageCount, -1 );
    m_depthCache.assign( messageCount, -1 );
    m_expanded.assign( messageCount, 0 );
    m_visited.assign( messageCount, -1 );

    const auto topLevel = archive.GetTopLevel();
    for( uint64_t i=0; i<topLevel.size; i++ )
    {
        m_visible.emplace_back( topLevel.ptr[i] );
    }
    RebuildVisibleIndex( 0 );
}

int ThreadListModel::VisibleRowOf( uint32_t messageIndex ) const
{
    if( messageIndex >= m_visibleIndex.size() ) return -1;
    return m_visibleIndex[messageIndex];
}

bool ThreadListModel::CanExpand( uint32_t messageIndex ) const
{
    return m_archive && m_archive->GetTotalChildrenCount( messageIndex ) > 1;
}

bool ThreadListModel::IsExpanded( uint32_t messageIndex ) const
{
    return messageIndex < m_expanded.size() && m_expanded[messageIndex] != 0;
}

bool ThreadListModel::Expand( uint32_t messageIndex, bool recursive )
{
    if( !CanExpand( messageIndex ) || IsExpanded( messageIndex ) ) return false;

    const auto row = VisibleRowOf( messageIndex );
    if( row < 0 ) return false;

    m_expanded[messageIndex] = 1;

    std::vector<uint32_t> inserted;
    inserted.reserve( std::min<uint32_t>( m_archive->GetTotalChildrenCount( messageIndex ) - 1, 4096 ) );
    CollectVisibleChildren( messageIndex, recursive, inserted );
    if( inserted.empty() ) return false;

    m_visible.insert( m_visible.begin() + row + 1, inserted.begin(), inserted.end() );
    RebuildVisibleIndex( row + 1 );
    return true;
}

bool ThreadListModel::Collapse( uint32_t messageIndex )
{
    if( !IsExpanded( messageIndex ) ) return false;

    const auto row = VisibleRowOf( messageIndex );
    if( row < 0 ) return false;

    m_expanded[messageIndex] = 0;

    const auto subtreeEnd = messageIndex + m_archive->GetTotalChildrenCount( messageIndex );
    size_t start = row + 1;
    size_t end = start;
    while( end < m_visible.size() && m_visible[end] < subtreeEnd )
    {
        m_visibleIndex[m_visible[end]] = -1;
        end++;
    }
    if( start == end ) return false;

    m_visible.erase( m_visible.begin() + start, m_visible.begin() + end );
    RebuildVisibleIndex( start );
    return true;
}

int ThreadListModel::Depth( uint32_t messageIndex )
{
    if( m_depthCache[messageIndex] != -1 ) return m_depthCache[messageIndex];

    const auto parent = m_archive->GetParent( messageIndex );
    m_depthCache[messageIndex] = parent == -1 ? 0 : Depth( uint32_t( parent ) ) + 1;
    return m_depthCache[messageIndex];
}

ThreadListRowData ThreadListModel::GetRowData( size_t row )
{
    const auto messageIndex = MessageAt( row );
    return ThreadListRowData {
        messageIndex,
        m_archive->GetTotalChildrenCount( messageIndex ),
        Depth( messageIndex ),
        CanExpand( messageIndex ),
        IsExpanded( messageIndex ),
        WasVisited( messageIndex )
    };
}

bool ThreadListModel::MarkVisited( uint32_t messageIndex )
{
    char unpack[2048];
    m_archive->UnpackMsgId( m_archive->GetMessageId( messageIndex ), unpack );
    const auto changed = m_storage->MarkVisited( unpack );
    m_visited[messageIndex] = 1;
    return changed;
}

bool ThreadListModel::WasVisited( uint32_t messageIndex )
{
    auto& state = m_visited[messageIndex];
    if( state != -1 ) return state != 0;

    char unpack[2048];
    m_archive->UnpackMsgId( m_archive->GetMessageId( messageIndex ), unpack );
    state = m_storage->WasVisited( unpack ) ? 1 : 0;
    return state != 0;
}

void ThreadListModel::RebuildVisibleIndex( size_t start )
{
    for( size_t i=start; i<m_visible.size(); i++ )
    {
        m_visibleIndex[m_visible[i]] = int32_t( i );
    }
}

void ThreadListModel::CollectVisibleChildren( uint32_t parent, bool recursive, std::vector<uint32_t>& out )
{
    const auto children = m_archive->GetChildren( parent );
    for( uint64_t i=0; i<children.size; i++ )
    {
        const auto child = children.ptr[i];
        out.emplace_back( child );

        if( recursive && CanExpand( child ) )
        {
            m_expanded[child] = 1;
        }

        if( IsExpanded( child ) )
        {
            CollectVisibleChildren( child, recursive, out );
        }
    }
}
