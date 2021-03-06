#include "OfflineMapDataProvider_P.h"
#include "OfflineMapDataProvider.h"

#include <cassert>
#if defined(_DEBUG) || defined(DEBUG)
#   include <chrono>
#endif

#include "OfflineMapDataTile.h"
#include "OfflineMapDataTile_P.h"
#include "ObfsCollection.h"
#include "ObfDataInterface.h"
#include "ObfMapSectionInfo.h"
#include "MapObject.h"
#include "Rasterizer.h"
#include "Utilities.h"
#include "Logging.h"

OsmAnd::OfflineMapDataProvider_P::OfflineMapDataProvider_P( OfflineMapDataProvider* owner_ )
    : owner(owner_)
    , _link(new Link(*this))
{
}

OsmAnd::OfflineMapDataProvider_P::~OfflineMapDataProvider_P()
{
}

void OsmAnd::OfflineMapDataProvider_P::obtainTile( const TileId tileId, const ZoomLevel zoom, std::shared_ptr<const OfflineMapDataTile>& outTile )
{
    // Check if there is a weak reference to that tile, and if that reference is still valid, use that
    std::shared_ptr<TileEntry> tileEntry;
    _tileReferences.obtainOrAllocateTileEntry(tileEntry, tileId, zoom, [](const TilesCollection<TileEntry>& collection, const TileId tileId, const ZoomLevel zoom) -> TileEntry*
        {
            return new TileEntry(collection, tileId, zoom);
        });

    // Only if tile entry has "Unknown" state proceed to "Requesting" state
    {
        QWriteLocker scopedLock(&tileEntry->stateLock);

        assert(tileEntry->state != TileState::Released);
        if(tileEntry->state == TileState::Undefined)
        {
            // Since tile is in undefined state, it will be processed right now,
            // so just change state to 'Loading' and continue execution
            tileEntry->state = TileState::Loading;
        }
        else if(tileEntry->state == TileState::Loading)
        {
            // If tile is in 'Loading' state, wait until it will become 'Loaded'
            while(tileEntry->state != TileState::Loading)
                tileEntry->_loadedCondition.wait(&tileEntry->stateLock);
        }
        else if(tileEntry->state == TileState::Loaded)
        {
            // If tile is already 'Loaded', just verify it's reference and return that
            assert(!tileEntry->_tile.expired());
            outTile = tileEntry->_tile.lock();
            return;
        }
    }

    // Obtain OBF data interface
    const auto& dataInterface = owner->obfsCollection->obtainDataInterface();

    // Get bounding box that covers this tile
    const auto tileBBox31 = Utilities::tileBoundingBox31(tileId, zoom);

    // Perform read-out
    QList< std::shared_ptr<const Model::MapObject> > duplicateMapObjects;
    QList< std::shared_ptr<const Model::MapObject> > mapObjects;
    MapFoundationType tileFoundation;
#if defined(_DEBUG) || defined(DEBUG)
    float dataFilter = 0.0f;
    const auto dataRead_Begin = std::chrono::high_resolution_clock::now();
#endif
    auto& dataCache = _dataCache[zoom];
    dataInterface->obtainMapObjects(&mapObjects, &tileFoundation, tileBBox31, zoom, nullptr,
#if defined(_DEBUG) || defined(DEBUG)
        [&dataCache, &duplicateMapObjects, tileBBox31, &dataFilter](const std::shared_ptr<const ObfMapSectionInfo>& section, const uint64_t id) -> bool
#else
        [&dataCache, &duplicateMapObjects, tileBBox31](const std::shared_ptr<const ObfMapSectionInfo>& section, const uint64_t id) -> bool
#endif
        {
#if defined(_DEBUG) || defined(DEBUG)
            const auto dataFilter_Begin = std::chrono::high_resolution_clock::now();
#endif

            // Save reference to duplicate map object
            {
                QReadLocker scopedLocker(&dataCache._mapObjectsMutex);

                const auto itDuplicateMapObject = dataCache._mapObjects.constFind(id);
                if(itDuplicateMapObject != dataCache._mapObjects.cend())
                {
                    const auto& mapObjectWeakRef = *itDuplicateMapObject;

                    if(const auto& mapObject = mapObjectWeakRef.lock())
                    {
                        // Not all duplicates should be used, since some may lay outside bbox
                        if(mapObject->intersects(tileBBox31))
                            duplicateMapObjects.push_back(mapObject);

#if defined(_DEBUG) || defined(DEBUG)
                        const auto dataFilter_End = std::chrono::high_resolution_clock::now();
                        const std::chrono::duration<float> dataRead_Elapsed = dataFilter_End - dataFilter_Begin;
                        dataFilter += dataRead_Elapsed.count();
#endif

                        return false;
                    }
                }
            }

#if defined(_DEBUG) || defined(DEBUG)
            const auto dataFilter_End = std::chrono::high_resolution_clock::now();
            const std::chrono::duration<float> dataRead_Elapsed = dataFilter_End - dataFilter_Begin;
            dataFilter += dataRead_Elapsed.count();
#endif

            return true;
        });

#if defined(_DEBUG) || defined(DEBUG)
    const auto dataRead_End = std::chrono::high_resolution_clock::now();
    const std::chrono::duration<float> dataRead_Elapsed = dataRead_End - dataRead_Begin;

    const auto dataIdsProcess_Begin = std::chrono::high_resolution_clock::now();
#endif

    // Append weak references to newly read map objects
    for(auto itMapObject = mapObjects.cbegin(); itMapObject != mapObjects.cend(); ++itMapObject)
    {
        const auto& mapObject = *itMapObject;

        // Add unique map object under lock to all zoom levels, for which this map object is valid
        assert(mapObject->level);
        for(int zoomLevel = mapObject->level->minZoom; zoomLevel <= mapObject->level->maxZoom; zoomLevel++)
        {
            auto& dataCache = _dataCache[zoomLevel];
            {
                QWriteLocker scopedLocker(&dataCache._mapObjectsMutex);

                if(!dataCache._mapObjects.contains(mapObject->id))
                    dataCache._mapObjects.insert(mapObject->id, mapObject);
            }
        }
    }
#if defined(_DEBUG) || defined(DEBUG)
    const auto dataIdsProcess_End = std::chrono::high_resolution_clock::now();
    const std::chrono::duration<float> dataIdsProcess_Elapsed = dataIdsProcess_End - dataIdsProcess_Begin;

    const auto dataProcess_Begin = std::chrono::high_resolution_clock::now();
#endif

    // Prepare data for the tile
    mapObjects << duplicateMapObjects;

    // Allocate and prepare rasterizer context
    bool nothingToRasterize = false;
    std::shared_ptr<RasterizerContext> rasterizerContext(new RasterizerContext(owner->rasterizerEnvironment));
    Rasterizer::prepareContext(*rasterizerContext, tileBBox31, zoom, tileFoundation, mapObjects, &nothingToRasterize);

#if defined(_DEBUG) || defined(DEBUG)
    const auto dataProcess_End = std::chrono::high_resolution_clock::now();
    const std::chrono::duration<float> dataProcess_Elapsed = dataProcess_End - dataProcess_Begin;
    LogPrintf(LogSeverityLevel::Info,
        "%d map objects (%d unique, %d shared) in %dx%d@%d: read %fs (filter-by-id %fs), process-ids %fs, process-content %fs",
        mapObjects.size() + duplicateMapObjects.size(), mapObjects.size(), duplicateMapObjects.size(),
        tileId.x, tileId.y, zoom,
        dataRead_Elapsed.count(), dataFilter, dataIdsProcess_Elapsed.count(), dataProcess_Elapsed.count());
#endif

    // Create tile
    const auto newTile = new OfflineMapDataTile(tileId, zoom, tileFoundation, mapObjects, rasterizerContext, nothingToRasterize);
    newTile->_d->_link = _link;
    newTile->_d->_refEntry = tileEntry;

    // Publish new tile
    outTile.reset(newTile);

    // Store weak reference to new tile and mark it as 'Loaded'
    {
        QWriteLocker scopedLock(&tileEntry->stateLock);

        assert(tileEntry->state == TileState::Loading);
        tileEntry->state = TileState::Loaded;
        tileEntry->_tile = outTile;

        // Notify that tile has been loaded
        tileEntry->_loadedCondition.wakeAll();
    }
}

