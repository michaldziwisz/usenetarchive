#ifndef __WINBROWSER_THREADLISTMODEL_HPP__
#define __WINBROWSER_THREADLISTMODEL_HPP__

#include <cstddef>
#include <stdint.h>
#include <vector>

class Archive;
class PersistentStorage;

struct ThreadListRowData
{
    uint32_t messageIndex;
    uint32_t subtreeSize;
    int depth;
    bool expandable;
    bool expanded;
    bool visited;
};

class ThreadListModel
{
public:
    ThreadListModel();

    void Reset( const Archive& archive, PersistentStorage& storage );

    size_t VisibleCount() const { return m_visible.size(); }
    uint32_t MessageAt( size_t row ) const { return m_visible[row]; }
    int VisibleRowOf( uint32_t messageIndex ) const;

    bool CanExpand( uint32_t messageIndex ) const;
    bool IsExpanded( uint32_t messageIndex ) const;
    bool Expand( uint32_t messageIndex, bool recursive );
    bool Collapse( uint32_t messageIndex );

    int Depth( uint32_t messageIndex );
    ThreadListRowData GetRowData( size_t row );

    bool MarkVisited( uint32_t messageIndex );
    bool WasVisited( uint32_t messageIndex );

private:
    void RebuildVisibleIndex( size_t start );
    void CollectVisibleChildren( uint32_t parent, bool recursive, std::vector<uint32_t>& out );

    const Archive* m_archive;
    PersistentStorage* m_storage;

    std::vector<uint32_t> m_visible;
    std::vector<int32_t> m_visibleIndex;
    std::vector<int32_t> m_depthCache;
    std::vector<int8_t> m_expanded;
    std::vector<int8_t> m_visited;
};

#endif
