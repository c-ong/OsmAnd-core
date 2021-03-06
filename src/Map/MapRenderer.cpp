#include "MapRenderer.h"

#include <cassert>

#include <QMutableMapIterator>

#include <SkBitmap.h>
#include <SkImageDecoder.h>

#include "IMapBitmapTileProvider.h"
#include "IMapElevationDataProvider.h"
#include "IMapSymbolProvider.h"
#include "IRetainedMapTile.h"
#include "RenderAPI.h"
#include "EmbeddedResources.h"
#include "Logging.h"
#include "Utilities.h"

OsmAnd::MapRenderer::MapRenderer()
    : _taskHostBridge(this)
    , currentConfiguration(_currentConfiguration)
    , _currentConfigurationInvalidatedMask(0xFFFFFFFF)
    , currentState(_currentState)
    , _currentStateOutdated(true)
    , _invalidatedRasterLayerResourcesMask(0)
    , _invalidatedElevationDataResources(false)
    , _invalidatedSymbolsResources(false)
    , tiledResources(_tiledResources)
    , _renderThreadId(nullptr)
    , _workerThreadId(nullptr)
    , renderAPI(_renderAPI)
{
    // Number of workers should be determined in runtime (exclude worker and main threads):
    const auto idealThreadCount = qMax(QThread::idealThreadCount() - 2, 1);
    _requestWorkersPool.setMaxThreadCount(idealThreadCount);

    // Create all tiled resources
    for(auto resourceType = 0u; resourceType < TiledResourceTypesCount; resourceType++)
    {
        auto collection = new TiledResources(static_cast<TiledResourceType>(resourceType));
        _tiledResources[resourceType].reset(collection);
    }

    // Fill-up default state
    for(auto layerId = 0u; layerId < RasterMapLayersCount; layerId++)
        setRasterLayerOpacity(static_cast<RasterMapLayerId>(layerId), 1.0f);
    setElevationDataScaleFactor(1.0f, true);
    setFieldOfView(16.5f, true);
    setDistanceToFog(400.0f, true);
    setFogOriginFactor(0.36f, true);
    setFogHeightOriginFactor(0.05f, true);
    setFogDensity(1.9f, true);
    setFogColor(FColorRGB(1.0f, 0.0f, 0.0f), true);
    setSkyColor(FColorRGB(140.0f / 255.0f, 190.0f / 255.0f, 214.0f / 255.0f), true);
    setAzimuth(0.0f, true);
    setElevationAngle(45.0f, true);
    const auto centerIndex = 1u << (ZoomLevel::MaxZoomLevel - 1);
    setTarget(PointI(centerIndex, centerIndex), true);
    setZoom(0, true);
}

OsmAnd::MapRenderer::~MapRenderer()
{
    releaseRendering();

    _taskHostBridge.onOwnerIsBeingDestructed();
}

bool OsmAnd::MapRenderer::setup( const MapRendererSetupOptions& setupOptions )
{
    // We can not change setup options renderer once rendering has been initialized
    if(_isRenderingInitialized)
        return false;

    _setupOptions = setupOptions;

    return true;
}

void OsmAnd::MapRenderer::setConfiguration( const MapRendererConfiguration& configuration_, bool forcedUpdate /*= false*/ )
{
    QWriteLocker scopedLocker(&_configurationLock);

    const bool colorDepthForcingChanged = (_requestedConfiguration.limitTextureColorDepthBy16bits != configuration_.limitTextureColorDepthBy16bits);
    const bool atlasTexturesUsageChanged = (_requestedConfiguration.altasTexturesAllowed != configuration_.altasTexturesAllowed);
    const bool elevationDataResolutionChanged = (_requestedConfiguration.heixelsPerTileSide != configuration_.heixelsPerTileSide);
    const bool texturesFilteringChanged = (_requestedConfiguration.texturesFilteringQuality != configuration_.texturesFilteringQuality);
    const bool paletteTexturesUsageChanged = (_requestedConfiguration.paletteTexturesAllowed != configuration_.paletteTexturesAllowed);

    bool invalidateRasterTextures = false;
    invalidateRasterTextures = invalidateRasterTextures || colorDepthForcingChanged;
    invalidateRasterTextures = invalidateRasterTextures || atlasTexturesUsageChanged;
    invalidateRasterTextures = invalidateRasterTextures || paletteTexturesUsageChanged;

    bool invalidateElevationData = false;
    invalidateElevationData = invalidateElevationData || elevationDataResolutionChanged;

    bool update = forcedUpdate;
    update = update || invalidateRasterTextures;
    update = update || invalidateElevationData;
    update = update || texturesFilteringChanged;
    if(!update)
        return;

    _requestedConfiguration = configuration_;
    uint32_t mask = 0;
    if(colorDepthForcingChanged)
        mask |= ConfigurationChange::ColorDepthForcing;
    if(atlasTexturesUsageChanged)
        mask |= ConfigurationChange::AtlasTexturesUsage;
    if(elevationDataResolutionChanged)
        mask |= ConfigurationChange::ElevationDataResolution;
    if(texturesFilteringChanged)
        mask |= ConfigurationChange::TexturesFilteringMode;
    if(paletteTexturesUsageChanged)
        mask |= ConfigurationChange::PaletteTexturesUsage;
    if(invalidateRasterTextures)
    {
        for(int layerId = static_cast<int>(RasterMapLayerId::BaseLayer); layerId < RasterMapLayersCount; layerId++)
            invalidateRasterLayerResources(static_cast<RasterMapLayerId>(layerId));
    }
    if(invalidateElevationData)
    {
        invalidateElevationDataResources();
    }
    invalidateCurrentConfiguration(mask);
}

void OsmAnd::MapRenderer::invalidateCurrentConfiguration(const uint32_t changesMask)
{
    _currentConfigurationInvalidatedMask = changesMask;

    // Since our current configuration is invalid, frame is also invalidated
    invalidateFrame();
}

bool OsmAnd::MapRenderer::updateCurrentConfiguration()
{
    uint32_t bitIndex = 0;
    while(_currentConfigurationInvalidatedMask)
    {
        if(_currentConfigurationInvalidatedMask & 0x1)
            validateConfigurationChange(static_cast<ConfigurationChange>(1 << bitIndex));

        bitIndex++;
        _currentConfigurationInvalidatedMask >>= 1;
    }

    return true;
}

bool OsmAnd::MapRenderer::initializeRendering()
{
    bool ok;

    // Before doing any initialization, we need to allocate and initialize render API
    auto apiObject = allocateRenderAPI();
    if(!apiObject)
        return false;
    _renderAPI.reset(apiObject);

    ok = preInitializeRendering();
    if(!ok)
        return false;

    ok = doInitializeRendering();
    if(!ok)
        return false;

    ok = postInitializeRendering();
    if(!ok)
        return false;

    // Once rendering is initialized, invalidate frame
    invalidateFrame();

    return true;
}

bool OsmAnd::MapRenderer::preInitializeRendering()
{
    if(_isRenderingInitialized)
        return false;

    // Capture render thread ID, since rendering must be performed from
    // same thread where it was initialized
    _renderThreadId = QThread::currentThreadId();

    return true;
}

bool OsmAnd::MapRenderer::doInitializeRendering()
{
    // Create background worker if enabled
    if(setupOptions.backgroundWorker.enabled)
        _backgroundWorker.reset(new Concurrent::Thread(std::bind(&MapRenderer::backgroundWorkerProcedure, this)));

    // Upload stubs
    {
        {
            const auto& data = EmbeddedResources::decompressResource(QLatin1String("map/stubs/processing_tile.png"));
            auto bitmap = new SkBitmap();
            if(!SkImageDecoder::DecodeMemory(data.data(), data.size(), bitmap, SkBitmap::Config::kNo_Config, SkImageDecoder::kDecodePixels_Mode))
            {
                delete bitmap;
            }
            else
            {
                auto bitmapTile = new MapBitmapTile(bitmap, MapBitmapTile::AlphaChannelData::Undefined);
                _renderAPI->uploadTileToGPU(std::shared_ptr< const MapTile >(bitmapTile), 1, _processingTileStub);
            }
        }
        {
            const auto& data = EmbeddedResources::decompressResource(QLatin1String("map/stubs/unavailable_tile.png"));
            auto bitmap = new SkBitmap();
            if(!SkImageDecoder::DecodeMemory(data.data(), data.size(), bitmap, SkBitmap::Config::kNo_Config, SkImageDecoder::kDecodePixels_Mode))
            {
                delete bitmap;
            }
            else
            {
                auto bitmapTile = new MapBitmapTile(bitmap, MapBitmapTile::AlphaChannelData::Undefined);
                _renderAPI->uploadTileToGPU(std::shared_ptr< const MapTile >(bitmapTile), 1, _unavailableTileStub);
            }
        }
    }

    return true;
}

bool OsmAnd::MapRenderer::postInitializeRendering()
{
    _isRenderingInitialized = true;

    if(_backgroundWorker)
        _backgroundWorker->start();

    return true;
}

bool OsmAnd::MapRenderer::prepareFrame()
{
    assert(_renderThreadId == QThread::currentThreadId());

    bool ok;

    ok = prePrepareFrame();
    if(!ok)
        return false;

    ok = doPrepareFrame();
    if(!ok)
        return false;

    ok = postPrepareFrame();
    if(!ok)
        return false;

    return true;
}

bool OsmAnd::MapRenderer::prePrepareFrame()
{
    if(!_isRenderingInitialized)
        return false;

    // If we have current configuration invalidated, we need to update it
    // and invalidate frame
    if(_currentConfigurationInvalidatedMask)
    {
        {
            QReadLocker scopedLocker(&_configurationLock);

            _currentConfiguration = _requestedConfiguration;
        }

        bool ok = updateCurrentConfiguration();
        if(ok)
            _currentConfigurationInvalidatedMask = 0;

        invalidateFrame();

        // If configuration is still invalidated, abort processing
        if(_currentConfigurationInvalidatedMask)
            return false;
    }

    // If current state is outdated in comparison to requested state,
    // it needs to be refreshed, internal state must be recalculated
    // and frame must be invalidated
    const auto internalState = getInternalState();
    if(_currentStateOutdated)
    {
        bool ok;
        QWriteLocker scopedLocker(&_internalStateLock);

        // Atomically copy requested state
        {
            QReadLocker scopedLocker(&_requestedStateLock);
            _currentState = _requestedState;
        }

        // Update internal state, that is derived from current state
        ok = updateInternalState(internalState, _currentState);

        // Postprocess internal state
        if(ok)
        {
            // Sort visible tiles by distance from target
            qSort(internalState->visibleTiles.begin(), internalState->visibleTiles.end(), [this, internalState](const TileId& l, const TileId& r) -> bool
            {
                const auto lx = l.x - internalState->targetTileId.x;
                const auto ly = l.y - internalState->targetTileId.y;

                const auto rx = r.x - internalState->targetTileId.x;
                const auto ry = r.y - internalState->targetTileId.y;

                return (lx*lx + ly*ly) > (rx*rx + ry*ry);
            });
        }

        // Frame is being invalidated anyways, since a refresh is needed due to state change (successful or not)
        invalidateFrame();

        // If there was an error, keep current state outdated and terminate processing
        if(!ok)
            return false;

        _currentStateOutdated = false;
    }

    // Get set of tiles that are unique: visible tiles may contain same tiles, but wrapped
    _uniqueTiles.clear();
    for(auto itTileId = internalState->visibleTiles.cbegin(); itTileId != internalState->visibleTiles.cend(); ++itTileId)
    {
        const auto& tileId = *itTileId;
        _uniqueTiles.insert(Utilities::normalizeTileId(tileId, _currentState.zoomBase));
    }

    // If we have invalidated resources, purge them
    if(_invalidatedRasterLayerResourcesMask)
    {
        QReadLocker scopedLocker(&_invalidatedRasterLayerResourcesMaskLock);

        for(int layerId = 0; layerId < RasterMapLayersCount; layerId++)
        {
            if((_invalidatedRasterLayerResourcesMask & (1 << layerId)) == 0)
                continue;

            validateRasterLayerResources(static_cast<RasterMapLayerId>(layerId));

            _invalidatedRasterLayerResourcesMask &= ~(1 << layerId);
        }
    }
    if(_invalidatedElevationDataResources)
    {
        validateElevationDataResources();
        _invalidatedElevationDataResources = false;
    }
    if(_invalidatedSymbolsResources)
    {
        validateSymbolsResources();
        _invalidatedSymbolsResources = false;
    }

    return true;
}

bool OsmAnd::MapRenderer::doPrepareFrame()
{
    return true;
}

bool OsmAnd::MapRenderer::updateInternalState(InternalState* internalState, const MapRendererState& state)
{
    const auto zoomDiff = ZoomLevel::MaxZoomLevel - state.zoomBase;

    // Get target tile id
    internalState->targetTileId.x = state.target31.x >> zoomDiff;
    internalState->targetTileId.y = state.target31.y >> zoomDiff;

    // Compute in-tile offset
    PointI targetTile31;
    targetTile31.x = internalState->targetTileId.x << zoomDiff;
    targetTile31.y = internalState->targetTileId.y << zoomDiff;

    const auto tileWidth31 = 1u << zoomDiff;
    const auto inTileOffset = state.target31 - targetTile31;
    internalState->targetInTileOffsetN.x = static_cast<float>(inTileOffset.x) / tileWidth31;
    internalState->targetInTileOffsetN.y = static_cast<float>(inTileOffset.y) / tileWidth31;

    return true;
}

bool OsmAnd::MapRenderer::postPrepareFrame()
{
    // Before requesting missing tiled resources, clean up cache to free some space
    cleanUpTiledResourcesCache();

    // In the end of rendering processing, request tiled resources that are neither
    // present in requested list, nor in pending, nor in uploaded
    requestMissingTiledResources();

    return true;
}

bool OsmAnd::MapRenderer::renderFrame()
{
    assert(_renderThreadId == QThread::currentThreadId());

    bool ok;

    ok = preRenderFrame();
    if(!ok)
        return false;

    ok = doRenderFrame();
    if(!ok)
        return false;

    ok = postRenderFrame();
    if(!ok)
        return false;

    return true;
}

bool OsmAnd::MapRenderer::preRenderFrame()
{
    if(!_isRenderingInitialized)
        return false;

    return true;
}

bool OsmAnd::MapRenderer::postRenderFrame()
{
    _frameInvalidated = false;

    return true;
}

bool OsmAnd::MapRenderer::processRendering()
{
    assert(_renderThreadId == QThread::currentThreadId());

    bool ok;

    ok = preProcessRendering();
    if(!ok)
        return false;

    ok = doProcessRendering();
    if(!ok)
        return false;

    ok = postProcessRendering();
    if(!ok)
        return false;

    return true;
}

bool OsmAnd::MapRenderer::preProcessRendering()
{
    return true;
}

bool OsmAnd::MapRenderer::doProcessRendering()
{
    // If background worked is was not enabled, upload tiles to GPU in render thread
    // To reduce FPS drop, upload not more than 1 tile per frame, and do that before end of the frame
    // to avoid forcing driver to upload data on current frame presentation.
    if(!setupOptions.backgroundWorker.enabled)
        uploadTiledResources();

    return true;
}

bool OsmAnd::MapRenderer::postProcessRendering()
{
    return true;
}

bool OsmAnd::MapRenderer::releaseRendering()
{
    assert(_renderThreadId == QThread::currentThreadId());

    bool ok;

    ok = preReleaseRendering();
    if(!ok)
        return false;

    ok = doReleaseRendering();
    if(!ok)
        return false;

    ok = postReleaseRendering();
    if(!ok)
        return false;

    // After all release procedures, release render API
    ok = _renderAPI->release();
    _renderAPI.reset();
    if(!ok)
        return false;

    return true;
}

bool OsmAnd::MapRenderer::preReleaseRendering()
{
    if(!_isRenderingInitialized)
        return false;

    return true;
}

bool OsmAnd::MapRenderer::doReleaseRendering()
{
    return true;
}

bool OsmAnd::MapRenderer::postReleaseRendering()
{
    _isRenderingInitialized = false;

    // Stop worker
    if(_backgroundWorker)
    {
        _backgroundWorkerWakeup.wakeAll();
        _backgroundWorker->wait();
        _backgroundWorker.reset();
    }

    // Release all tiled resources
    for(auto itResourcesCollection = _tiledResources.cbegin(); itResourcesCollection != _tiledResources.cend(); ++itResourcesCollection)
        releaseTiledResources(*itResourcesCollection);

    // Release all embedded resources
    {
        assert(_unavailableTileStub.use_count() == 1);
        _unavailableTileStub.reset();
    }
    {
        assert(_processingTileStub.use_count() == 1);
        _processingTileStub.reset();
    }

    return true;
}

void OsmAnd::MapRenderer::notifyRequestedStateWasUpdated()
{
    _currentStateOutdated = true;

    // Since our current state is invalid, frame is also invalidated
    invalidateFrame();
}

void OsmAnd::MapRenderer::invalidateFrame()
{
    _frameInvalidated = true;

    // Request frame, if such callback is defined
    if(setupOptions.frameRequestCallback)
        setupOptions.frameRequestCallback();
}

void OsmAnd::MapRenderer::requestUploadDataToGPU()
{
    if(setupOptions.backgroundWorker.enabled)
        _backgroundWorkerWakeup.wakeAll();
    else
        invalidateFrame();
}

void OsmAnd::MapRenderer::invalidateRasterLayerResources( const RasterMapLayerId& layerId )
{
    QWriteLocker scopedLocker(&_invalidatedRasterLayerResourcesMaskLock);

    _invalidatedRasterLayerResourcesMask |= 1 << static_cast<int>(layerId);
}

void OsmAnd::MapRenderer::validateRasterLayerResources( const RasterMapLayerId& layerId )
{
    releaseTiledResources(_tiledResources[TiledResourceType::RasterBaseLayer + static_cast<int>(layerId)]);
}

void OsmAnd::MapRenderer::invalidateElevationDataResources()
{
    _invalidatedElevationDataResources = true;
}

void OsmAnd::MapRenderer::validateElevationDataResources()
{
    releaseTiledResources(_tiledResources[TiledResourceType::ElevationData]);
}

void OsmAnd::MapRenderer::invalidateSymbolsResources()
{
    _invalidatedSymbolsResources = true;
}

void OsmAnd::MapRenderer::validateSymbolsResources()
{
    releaseTiledResources(_tiledResources[TiledResourceType::Symbols]);
}

void OsmAnd::MapRenderer::backgroundWorkerProcedure()
{
    QMutex wakeupMutex;
    _workerThreadId = QThread::currentThreadId();

    // Call prologue if such exists
    if(setupOptions.backgroundWorker.prologue)
        setupOptions.backgroundWorker.prologue();

    while(_isRenderingInitialized)
    {
        // Wait until we're unblocked by host
        {
            wakeupMutex.lock();
            _backgroundWorkerWakeup.wait(&wakeupMutex);
            wakeupMutex.unlock();
        }
        if(!_isRenderingInitialized)
            break;

        // In every layer we have, upload pending tiles to GPU
        uploadTiledResources();
    }

    // Call epilogue
    if(setupOptions.backgroundWorker.epilogue)
        setupOptions.backgroundWorker.epilogue();

    _workerThreadId = nullptr;
}

bool OsmAnd::MapRenderer::isDataSourceAvailableFor( const TiledResourceType resourceType )
{
    QReadLocker scopedLocker(&_requestedStateLock);

    if(resourceType >= TiledResourceType::RasterBaseLayer && resourceType < (TiledResourceType::RasterBaseLayer + RasterMapLayersCount))
    {
        return static_cast<bool>(_currentState.rasterLayerProviders[resourceType - TiledResourceType::RasterBaseLayer]);
    }
    else if(resourceType == TiledResourceType::ElevationData)
    {
        return static_cast<bool>(_currentState.elevationDataProvider);
    }
    else if(resourceType == TiledResourceType::Symbols)
    {
        return !_currentState.symbolProviders.isEmpty();
    }

    return false;
}

bool OsmAnd::MapRenderer::obtainMapTileProviderFor( const TiledResourceType resourceType, std::shared_ptr<OsmAnd::IMapTileProvider>& provider )
{
    QReadLocker scopedLocker(&_requestedStateLock);

    if(resourceType >= TiledResourceType::RasterBaseLayer && resourceType < (TiledResourceType::RasterBaseLayer + RasterMapLayersCount))
    {
        provider = _currentState.rasterLayerProviders[resourceType - TiledResourceType::RasterBaseLayer];
    }
    else if(resourceType == TiledResourceType::ElevationData)
    {
        provider = _currentState.elevationDataProvider;
    }
    else
    {
        return false;
    }

    return static_cast<bool>(provider);
}

void OsmAnd::MapRenderer::cleanUpTiledResourcesCache()
{
    // Use aggressive cache cleaning: remove all tiled resources that are not needed
    for(auto itTiledResources = _tiledResources.cbegin(); itTiledResources != _tiledResources.cend(); ++itTiledResources)
    {
        const auto& tiledResources = *itTiledResources;
        const auto dataSourceAvailable = isDataSourceAvailableFor(tiledResources->type);

        tiledResources->removeTileEntries([this, dataSourceAvailable](const std::shared_ptr<TiledResourceEntry>& entry, bool& cancel) -> bool
        {
            // Skip cleaning if this tiled resource is needed
            if(_uniqueTiles.contains(entry->tileId) && dataSourceAvailable)
                return false;

            // Irrespective of current state, tile entry must be removed
            {
                QReadLocker scopedLocker(&entry->stateLock);

                // If state is "Requested" it means that there is a task somewhere out there,
                // that may even be running at this moment
                if(entry->state == ResourceState::Requested)
                {
                    // And we need to cancel it
                    assert(entry->_requestTask != nullptr);
                    entry->_requestTask->requestCancellation();
                }
                // If state is "Uploaded", GPU resources must be release prior to deleting tiled resource entry
                else if(entry->state == ResourceState::Uploaded)
                {
                    // This should be last reference, so assert on that
                    entry->unloadFromGPU();

                    entry->state = ResourceState::Unloaded;
                }
            }

            return true;
        });
    }
}

void OsmAnd::MapRenderer::requestMissingTiledResources()
{
    const auto requestedZoom = _currentState.zoomBase;
    for(auto itTileId = _uniqueTiles.cbegin(); itTileId != _uniqueTiles.cend(); ++itTileId)
    {
        const auto& tileId = *itTileId;

        for(auto itTiledResources = _tiledResources.cbegin(); itTiledResources != _tiledResources.cend(); ++itTiledResources)
        {
            const auto& tiledResources = *itTiledResources;
            const auto resourceType = tiledResources->type;

            // Skip resource types that do not have an available data source
            if(!isDataSourceAvailableFor(resourceType))
                continue;

            // Obtain a resource entry and if it's state is "Unknown", create a task that will
            // request resource data
            std::shared_ptr<TiledResourceEntry> entry;
            tiledResources->obtainOrAllocateTileEntry(entry, tileId, _currentState.zoomBase,
                [this, resourceType](const TilesCollection<TiledResourceEntry>& collection, const TileId tileId, const ZoomLevel zoom) -> TiledResourceEntry*
                {
                    if(resourceType >= TiledResourceType::ElevationData && resourceType <= TiledResourceType::__RasterLayer_LAST)
                        return new MapTileResourceEntry(this, resourceType, collection, tileId, zoom);
                    else if(resourceType == TiledResourceType::Symbols)
                        return new SymbolsResourceEntry(this, collection, tileId, zoom);
                    else
                        return nullptr;
                });
            {
                // Only if tile entry has "Unknown" state proceed to "Requesting" state
                {
                    QWriteLocker scopedLock(&entry->stateLock);
                    if(entry->state != ResourceState::Unknown)
                        continue;
                    entry->state = ResourceState::Requesting;
                }

                // Create async-task that will obtain needed resource data
                const auto executeProc = [this, tileId, requestedZoom, resourceType](const Concurrent::Task* task, QEventLoop& eventLoop)
                {
                    const auto& tiledResources = _tiledResources[resourceType];

                    // Get resource entry that is going to be obtained
                    std::shared_ptr<TiledResourceEntry> entry;
                    if(!tiledResources->obtainTileEntry(entry, tileId, requestedZoom))
                    {
                        // In case resource entry no longer exists, there is no need to process it
                        return;
                    }

                    // Only if resource entry has "Requested" state proceed to "ProcessingRequest" state
                    {
                        QWriteLocker scopedLock(&entry->stateLock);
                        if(entry->state != ResourceState::Requested)
                            return;
                        entry->state = ResourceState::ProcessingRequest;
                    }

                    // Ask resource to obtain it's data
                    bool dataAvailable = false;
                    const auto requestSucceeded = entry->obtainData(dataAvailable);

                    // If failed to obtain resource data, remove resource entry to repeat try later
                    if(!requestSucceeded)
                    {
                        // It's safe to simply remove entry, since it's not yet uploaded
                        tiledResources->removeEntry(entry);
                        return;
                    }

                    // Finalize execution of task
                    {
                        QWriteLocker scopedLock(&entry->stateLock);

                        entry->_requestTask = nullptr;
                        entry->state = dataAvailable ? ResourceState::Ready : ResourceState::Unavailable;
                    }

                    // There is data to upload to GPU, so report that
                    requestUploadDataToGPU();
                };
                const auto postExecuteProc = [this, resourceType, tileId, requestedZoom](const Concurrent::Task* task, bool wasCancelled)
                {
                    auto& tiledResources = _tiledResources[resourceType];

                    // If task was canceled, remove tile entry
                    if(wasCancelled)
                    {
                        std::shared_ptr<TiledResourceEntry> entry;
                        if(!tiledResources->obtainTileEntry(entry, tileId, requestedZoom))
                            return;

                        // Here tiled resource may have been already uploaded, so check that
                        {
                            QWriteLocker scopedLock(&entry->stateLock);

                            // Unload resource from GPU, if it's there
                            if(entry->state == ResourceState::Uploaded)
                            {
                                // This should be last reference, so assert on that
                                entry->unloadFromGPU();

                                entry->state = ResourceState::Unloaded;
                            }
                        }

                        tiledResources->removeEntry(entry);
                    }
                };
                const auto asyncTask = new Concurrent::HostedTask(_taskHostBridge, executeProc, nullptr, postExecuteProc);

                // Register tile as requested
                {
                    QWriteLocker scopedLock(&entry->stateLock);

                    entry->_requestTask = asyncTask;
                    entry->state = ResourceState::Requested;
                }

                // Finally start the request
                _requestWorkersPool.start(asyncTask);
            }
        }
    }

    //TODO: sort requests in all requestedProvidersMask so that closest tiles would be downloaded first
}

std::shared_ptr<const OsmAnd::MapTile> OsmAnd::MapRenderer::prepareTileForUploadingToGPU( const std::shared_ptr<const MapTile>& tile )
{
    if(tile->dataType == MapTileDataType::Bitmap)
    {
        auto bitmapTile = std::static_pointer_cast<const MapBitmapTile>(tile);

        // Check if we're going to convert
        bool doConvert = false;
        const bool force16bit = (currentConfiguration.limitTextureColorDepthBy16bits && bitmapTile->bitmap->getConfig() == SkBitmap::Config::kARGB_8888_Config);
        const bool canUsePaletteTextures = currentConfiguration.paletteTexturesAllowed && renderAPI->isSupported_8bitPaletteRGBA8;
        const bool paletteTexture = (bitmapTile->bitmap->getConfig() == SkBitmap::Config::kIndex8_Config);
        const bool unsupportedFormat =
            (canUsePaletteTextures ? !paletteTexture : paletteTexture) ||
            (bitmapTile->bitmap->getConfig() != SkBitmap::Config::kARGB_8888_Config) ||
            (bitmapTile->bitmap->getConfig() != SkBitmap::Config::kARGB_4444_Config) ||
            (bitmapTile->bitmap->getConfig() != SkBitmap::Config::kRGB_565_Config);
        doConvert = doConvert || force16bit;
        doConvert = doConvert || unsupportedFormat;

        // Pass palette texture as-is
        if(paletteTexture && canUsePaletteTextures)
            return tile;

        // Check if we need alpha
        auto convertedAlphaChannelData = bitmapTile->alphaChannelData;
        if(doConvert && (convertedAlphaChannelData == MapBitmapTile::AlphaChannelData::Undefined))
        {
            convertedAlphaChannelData = SkBitmap::ComputeIsOpaque(*bitmapTile->bitmap.get())
                ? MapBitmapTile::AlphaChannelData::NotPresent
                : MapBitmapTile::AlphaChannelData::Present;
        }

        // If we have limit of 16bits per pixel in bitmaps, convert to ARGB(4444) or RGB(565)
        if(force16bit)
        {
            auto convertedBitmap = new SkBitmap();

            bitmapTile->bitmap->deepCopyTo(convertedBitmap,
                convertedAlphaChannelData == MapBitmapTile::AlphaChannelData::Present
                ? SkBitmap::Config::kARGB_4444_Config
                : SkBitmap::Config::kRGB_565_Config);

            auto convertedTile = new MapBitmapTile(convertedBitmap, convertedAlphaChannelData);
            return std::shared_ptr<const MapTile>(convertedTile);
        }

        // If we have any other unsupported format, convert to proper 16bit or 32bit
        if(unsupportedFormat)
        {
            auto convertedBitmap = new SkBitmap();

            bitmapTile->bitmap->deepCopyTo(convertedBitmap,
                currentConfiguration.limitTextureColorDepthBy16bits
                ? (convertedAlphaChannelData == MapBitmapTile::AlphaChannelData::Present ? SkBitmap::Config::kARGB_4444_Config : SkBitmap::Config::kRGB_565_Config)
                : SkBitmap::kARGB_8888_Config);

            auto convertedTile = new MapBitmapTile(convertedBitmap, convertedAlphaChannelData);
            return std::shared_ptr<const MapTile>(convertedTile);
        }
    }

    return tile;
}

void OsmAnd::MapRenderer::uploadTiledResources()
{
    const auto isOnRenderThread = (QThread::currentThreadId() == _renderThreadId);
    bool didUpload = false;

    for(auto itTiledResources = _tiledResources.cbegin(); itTiledResources != _tiledResources.cend(); ++itTiledResources)
    {
        const auto& tiledResources = *itTiledResources;

        // If we're uploading from render thread, limit to 1 resource per frame
        QList< std::shared_ptr<TiledResourceEntry> > entries;
        tiledResources->obtainTileEntries(&entries, [&entries, isOnRenderThread](const std::shared_ptr<TiledResourceEntry>& tileEntry, bool& cancel) -> bool
        {
            // If on render thread, limit result with only 1 entry
            if(isOnRenderThread && entries.size() > 0)
            {
                cancel = true;
                return false;
            }

            // Only ready tiles are needed
            return (tileEntry->state == ResourceState::Ready);
        });
        if(entries.isEmpty())
            continue;

        for(auto itEntry = entries.cbegin(); itEntry != entries.cend(); ++itEntry)
        {
            const auto& entry = *itEntry;

            {
                QWriteLocker scopedLock(&entry->stateLock);

                // State may have changed
                if(entry->state != ResourceState::Ready)
                    continue;

                // Actually upload to GPU
                didUpload = entry->uploadToGPU();
                if(!didUpload)
                {
                    LogPrintf(LogSeverityLevel::Error, "Failed to upload tiled resources for %dx%d@%d to GPU", entry->tileId.x, entry->tileId.y, entry->zoom);
                    continue;
                }

                entry->state = ResourceState::Uploaded;

                // If we're not on render thread, and we've just uploaded a tile, invalidate frame
                if(!isOnRenderThread)
                    invalidateFrame();
            }

            // If we're on render thread, limit to 1 tile per frame
            if(isOnRenderThread)
                break;
        }

        // If we're on render thread, limit to 1 tile per frame
        if(isOnRenderThread)
            break;
    }

    // Schedule one more render pass to upload more pending
    // or we've just uploaded a tile and need refresh
    if(didUpload)
        invalidateFrame();
}

void OsmAnd::MapRenderer::releaseTiledResources( const std::unique_ptr<TiledResources>& collection )
{
    // Remove all tiles, releasing associated GPU resources
    collection->removeTileEntries([](const std::shared_ptr<TiledResourceEntry>& entry, bool& cancel) -> bool
    {
        QWriteLocker scopedLock(&entry->stateLock);

        // Unload resource from GPU, if it's there
        if(entry->state == ResourceState::Uploaded)
        {
            // This should be last reference, so assert on that
            entry->unloadFromGPU();

            entry->state = ResourceState::Unloaded;
        }
        // If tile is just requested, prevent it from loading
        else if(entry->state == ResourceState::Requested)
        {
            // If cancellation request failed (what means that tile is already being processed), leave it, but change it's state
            if(!entry->_requestTask->requestCancellation())
                return false;
        }

        return true;
    });
}

unsigned int OsmAnd::MapRenderer::getVisibleTilesCount()
{
    QReadLocker scopedLocker(&_internalStateLock);

    return getInternalState()->visibleTiles.size();
}

void OsmAnd::MapRenderer::setRasterLayerProvider( const RasterMapLayerId layerId, const std::shared_ptr<IMapBitmapTileProvider>& tileProvider, bool forcedUpdate /*= false*/ )
{
    QWriteLocker scopedLocker(&_requestedStateLock);

    bool update = forcedUpdate || (_requestedState.rasterLayerProviders[static_cast<int>(layerId)] != tileProvider);
    if(!update)
        return;

    _requestedState.rasterLayerProviders[static_cast<int>(layerId)] = tileProvider;

    invalidateRasterLayerResources(layerId);
    notifyRequestedStateWasUpdated();
}

void OsmAnd::MapRenderer::setRasterLayerOpacity( const RasterMapLayerId layerId, const float opacity, bool forcedUpdate /*= false*/ )
{
    QWriteLocker scopedLocker(&_requestedStateLock);

    const auto clampedValue = qMax(0.0f, qMin(opacity, 1.0f));

    bool update = forcedUpdate || !qFuzzyCompare(_requestedState.rasterLayerOpacity[static_cast<int>(layerId)], clampedValue);
    if(!update)
        return;

    _requestedState.rasterLayerOpacity[static_cast<int>(layerId)] = clampedValue;

    notifyRequestedStateWasUpdated();
}

void OsmAnd::MapRenderer::setElevationDataProvider( const std::shared_ptr<IMapElevationDataProvider>& tileProvider, bool forcedUpdate /*= false*/ )
{
    QWriteLocker scopedLocker(&_requestedStateLock);

    bool update = forcedUpdate || (_requestedState.elevationDataProvider != tileProvider);
    if(!update)
        return;

    _requestedState.elevationDataProvider = tileProvider;

    invalidateElevationDataResources();
    notifyRequestedStateWasUpdated();
}

void OsmAnd::MapRenderer::setElevationDataScaleFactor( const float factor, bool forcedUpdate /*= false*/ )
{
    QWriteLocker scopedLocker(&_requestedStateLock);

    bool update = forcedUpdate || !qFuzzyCompare(_requestedState.elevationDataScaleFactor, factor);
    if(!update)
        return;

    _requestedState.elevationDataScaleFactor = factor;

    notifyRequestedStateWasUpdated();
}

void OsmAnd::MapRenderer::addSymbolProvider( const std::shared_ptr<IMapSymbolProvider>& provider, bool forcedUpdate /*= false*/ )
{
    QWriteLocker scopedLocker(&_requestedStateLock);

    bool update = forcedUpdate || !_requestedState.symbolProviders.contains(provider);
    if(!update)
        return;

    _requestedState.symbolProviders.push_back(provider);

    invalidateSymbolsResources();
    notifyRequestedStateWasUpdated();
}

void OsmAnd::MapRenderer::removeSymbolProvider( const std::shared_ptr<IMapSymbolProvider>& provider, bool forcedUpdate /*= false*/ )
{
    QWriteLocker scopedLocker(&_requestedStateLock);

    bool update = forcedUpdate || _requestedState.symbolProviders.contains(provider);
    if(!update)
        return;

    _requestedState.symbolProviders.removeOne(provider);
    assert(!_requestedState.symbolProviders.contains(provider));

    invalidateSymbolsResources();
    notifyRequestedStateWasUpdated();
}

void OsmAnd::MapRenderer::removeAllSymbolProviders( bool forcedUpdate /*= false*/ )
{
    QWriteLocker scopedLocker(&_requestedStateLock);

    bool update = forcedUpdate || !_requestedState.symbolProviders.isEmpty();
    if(!update)
        return;

    _requestedState.symbolProviders.clear();

    invalidateSymbolsResources();
    notifyRequestedStateWasUpdated();
}

void OsmAnd::MapRenderer::setWindowSize( const PointI& windowSize, bool forcedUpdate /*= false*/ )
{
    QWriteLocker scopedLocker(&_requestedStateLock);

    bool update = forcedUpdate || (_requestedState.windowSize != windowSize);
    if(!update)
        return;

    _requestedState.windowSize = windowSize;

    notifyRequestedStateWasUpdated();
}

void OsmAnd::MapRenderer::setViewport( const AreaI& viewport, bool forcedUpdate /*= false*/ )
{
    QWriteLocker scopedLocker(&_requestedStateLock);

    bool update = forcedUpdate || (_requestedState.viewport != viewport);
    if(!update)
        return;

    _requestedState.viewport = viewport;

    notifyRequestedStateWasUpdated();
}

void OsmAnd::MapRenderer::setFieldOfView( const float fieldOfView, bool forcedUpdate /*= false*/ )
{
    QWriteLocker scopedLocker(&_requestedStateLock);

    const auto clampedValue = qMax(std::numeric_limits<float>::epsilon(), qMin(fieldOfView, 90.0f));

    bool update = forcedUpdate || !qFuzzyCompare(_requestedState.fieldOfView, clampedValue);
    if(!update)
        return;

    _requestedState.fieldOfView = clampedValue;

    notifyRequestedStateWasUpdated();
}

void OsmAnd::MapRenderer::setDistanceToFog( const float fogDistance, bool forcedUpdate /*= false*/ )
{
    QWriteLocker scopedLocker(&_requestedStateLock);

    const auto clampedValue = qMax(std::numeric_limits<float>::epsilon(), fogDistance);

    bool update = forcedUpdate || !qFuzzyCompare(_requestedState.fogDistance, clampedValue);
    if(!update)
        return;

    _requestedState.fogDistance = clampedValue;

    notifyRequestedStateWasUpdated();
}

void OsmAnd::MapRenderer::setFogOriginFactor( const float factor, bool forcedUpdate /*= false*/ )
{
    QWriteLocker scopedLocker(&_requestedStateLock);

    const auto clampedValue = qMax(std::numeric_limits<float>::epsilon(), qMin(factor, 1.0f));

    bool update = forcedUpdate || !qFuzzyCompare(_requestedState.fogOriginFactor, clampedValue);
    if(!update)
        return;

    _requestedState.fogOriginFactor = clampedValue;

    notifyRequestedStateWasUpdated();
}

void OsmAnd::MapRenderer::setFogHeightOriginFactor( const float factor, bool forcedUpdate /*= false*/ )
{
    QWriteLocker scopedLocker(&_requestedStateLock);

    const auto clampedValue = qMax(std::numeric_limits<float>::epsilon(), qMin(factor, 1.0f));

    bool update = forcedUpdate || !qFuzzyCompare(_requestedState.fogHeightOriginFactor, clampedValue);
    if(!update)
        return;

    _requestedState.fogHeightOriginFactor = clampedValue;

    notifyRequestedStateWasUpdated();
}

void OsmAnd::MapRenderer::setFogDensity( const float fogDensity, bool forcedUpdate /*= false*/ )
{
    QWriteLocker scopedLocker(&_requestedStateLock);

    const auto clampedValue = qMax(std::numeric_limits<float>::epsilon(), fogDensity);

    bool update = forcedUpdate || !qFuzzyCompare(_requestedState.fogDensity, clampedValue);
    if(!update)
        return;

    _requestedState.fogDensity = clampedValue;

    notifyRequestedStateWasUpdated();
}

void OsmAnd::MapRenderer::setFogColor( const FColorRGB& color, bool forcedUpdate /*= false*/ )
{
    QWriteLocker scopedLocker(&_requestedStateLock);

    bool update = forcedUpdate || _requestedState.fogColor != color;
    if(!update)
        return;

    _requestedState.fogColor = color;

    notifyRequestedStateWasUpdated();
}

void OsmAnd::MapRenderer::setSkyColor( const FColorRGB& color, bool forcedUpdate /*= false*/ )
{
    QWriteLocker scopedLocker(&_requestedStateLock);

    bool update = forcedUpdate || _requestedState.skyColor != color;
    if(!update)
        return;

    _requestedState.skyColor = color;

    notifyRequestedStateWasUpdated();
}

void OsmAnd::MapRenderer::setAzimuth( const float azimuth, bool forcedUpdate /*= false*/ )
{
    QWriteLocker scopedLocker(&_requestedStateLock);

    float normalizedAzimuth = Utilities::normalizedAngleDegrees(azimuth);

    bool update = forcedUpdate || !qFuzzyCompare(_requestedState.azimuth, normalizedAzimuth);
    if(!update)
        return;

    _requestedState.azimuth = normalizedAzimuth;

    notifyRequestedStateWasUpdated();
}

void OsmAnd::MapRenderer::setElevationAngle( const float elevationAngle, bool forcedUpdate /*= false*/ )
{
    QWriteLocker scopedLocker(&_requestedStateLock);

    const auto clampedValue = qMax(std::numeric_limits<float>::epsilon(), qMin(elevationAngle, 90.0f));

    bool update = forcedUpdate || !qFuzzyCompare(_requestedState.elevationAngle, clampedValue);
    if(!update)
        return;

    _requestedState.elevationAngle = clampedValue;

    notifyRequestedStateWasUpdated();
}

void OsmAnd::MapRenderer::setTarget( const PointI& target31_, bool forcedUpdate /*= false*/ )
{
    QWriteLocker scopedLocker(&_requestedStateLock);

    const auto target31 = Utilities::normalizeCoordinates(target31_, ZoomLevel31);
    bool update = forcedUpdate || (_requestedState.target31 != target31);
    if(!update)
        return;

    _requestedState.target31 = target31;

    notifyRequestedStateWasUpdated();
}

void OsmAnd::MapRenderer::setZoom( const float zoom, bool forcedUpdate /*= false*/ )
{
    QWriteLocker scopedLocker(&_requestedStateLock);

    const auto clampedValue = qMax(std::numeric_limits<float>::epsilon(), qMin(zoom, 31.49999f));

    bool update = forcedUpdate || !qFuzzyCompare(_requestedState.requestedZoom, clampedValue);
    if(!update)
        return;

    _requestedState.requestedZoom = clampedValue;
    _requestedState.zoomBase = static_cast<ZoomLevel>(qRound(clampedValue));
    assert(_requestedState.zoomBase >= 0 && _requestedState.zoomBase <= 31);
    _requestedState.zoomFraction = _requestedState.requestedZoom - _requestedState.zoomBase;

    notifyRequestedStateWasUpdated();
}

OsmAnd::MapRenderer::TiledResources::TiledResources( const TiledResourceType& type_ )
    : type(type_)
{
}

OsmAnd::MapRenderer::TiledResources::~TiledResources()
{
    verifyNoUploadedTilesPresent();
}

void OsmAnd::MapRenderer::TiledResources::removeAllEntries()
{
    verifyNoUploadedTilesPresent();

    TilesCollection::removeAllEntries();
}

void OsmAnd::MapRenderer::TiledResources::verifyNoUploadedTilesPresent()
{
    // Ensure that no tiles have "Uploaded" state
    bool stillUploadedTilesPresent = false;
    obtainTileEntries(nullptr, [&stillUploadedTilesPresent](const std::shared_ptr<TiledResourceEntry>& tileEntry, bool& cancel) -> bool
    {
        if(tileEntry->state == ResourceState::Uploaded)
        {
            stillUploadedTilesPresent = true;
            cancel = true;
            return false;
        }

        return false;
    });
    if(stillUploadedTilesPresent)
        LogPrintf(LogSeverityLevel::Error, "One or more tiled resources still reside in GPU memory. This may cause GPU memory leak");
    assert(stillUploadedTilesPresent == false);
}

OsmAnd::MapRenderer::TiledResourceEntry::TiledResourceEntry( MapRenderer* owner_, const TiledResourceType type_, const TilesCollection<TiledResourceEntry>& collection, const TileId tileId, const ZoomLevel zoom )
    : TilesCollectionEntryWithState(collection, tileId, zoom)
    , _owner(owner_)
    , _requestTask(nullptr)
    , type(type_)
{
}

OsmAnd::MapRenderer::TiledResourceEntry::~TiledResourceEntry()
{
    const volatile auto state_ = state;

    if(state_ == ResourceState::Uploaded)
        LogPrintf(LogSeverityLevel::Error, "Tiled resource for %dx%d@%d still resides in GPU memory. This may cause GPU memory leak", tileId.x, tileId.y, zoom);
    assert(state_ != ResourceState::Uploaded);
}

OsmAnd::MapRenderer::InternalState::InternalState()
{
}

OsmAnd::MapRenderer::InternalState::~InternalState()
{
}

OsmAnd::MapRenderer::MapTileResourceEntry::MapTileResourceEntry( MapRenderer* owner, const TiledResourceType type, const TilesCollection<TiledResourceEntry>& collection, const TileId tileId, const ZoomLevel zoom )
    : TiledResourceEntry(owner, type, collection, tileId, zoom)
    , sourceData(_sourceData)
    , resourceInGPU(_resourceInGPU)
{
}

OsmAnd::MapRenderer::MapTileResourceEntry::~MapTileResourceEntry()
{

}

bool OsmAnd::MapRenderer::MapTileResourceEntry::obtainData( bool& dataAvailable )
{
    // Get source of tile
    std::shared_ptr<IMapTileProvider> provider;
    bool ok = _owner->obtainMapTileProviderFor(type, provider);
    if(!ok)
        return false;
    assert(static_cast<bool>(provider));

    // Obtain tile from provider
    std::shared_ptr<const MapTile> tile;
    const auto requestSucceeded = provider->obtainTile(tileId, zoom, tile);
    if(!requestSucceeded)
        return false;

    // Store data
    _sourceData = tile;
    dataAvailable = static_cast<bool>(tile);

    return true;
}

bool OsmAnd::MapRenderer::MapTileResourceEntry::uploadToGPU()
{
    const auto& preparedSourceData = _owner->prepareTileForUploadingToGPU(_sourceData);
    //TODO: This is weird, and probably should not be here. RenderAPI knows how to upload what, but on contrary - does not know the limits
    const auto tilesPerAtlasTextureLimit = _owner->getTilesPerAtlasTextureLimit(type, _sourceData);
    bool ok = _owner->renderAPI->uploadTileToGPU(preparedSourceData, tilesPerAtlasTextureLimit, _resourceInGPU);
    if(!ok)
        return false;

    // Release source data:
    if(const auto retainedSource = std::dynamic_pointer_cast<const IRetainedMapTile>(_sourceData))
    {
        // If map tile implements 'Retained' interface, it must be kept, but 
        std::const_pointer_cast<IRetainedMapTile>(retainedSource)->releaseNonRetainedData();
    }
    else
    {
        // or simply release entire tile
        _sourceData.reset();
    }

    return true;
}

void OsmAnd::MapRenderer::MapTileResourceEntry::unloadFromGPU()
{
    assert(_resourceInGPU.use_count() == 1);
    _resourceInGPU.reset();
}

OsmAnd::MapRenderer::SymbolsResourceEntry::SymbolsResourceEntry( MapRenderer* owner, const TilesCollection<TiledResourceEntry>& collection, const TileId tileId, const ZoomLevel zoom )
    : TiledResourceEntry(owner, TiledResourceType::Symbols, collection, tileId, zoom)
    , sourceData(_sourceData)
    , resourcesInGPU(_resourcesInGPU)
{
}

OsmAnd::MapRenderer::SymbolsResourceEntry::~SymbolsResourceEntry()
{
}

bool OsmAnd::MapRenderer::SymbolsResourceEntry::obtainData( bool& dataAvailable )
{
    // Obtain list of symbol providers
    QList< std::shared_ptr<IMapSymbolProvider> > symbolProviders;
    {
        QReadLocker scopedLocker(&_owner->_requestedStateLock);
        symbolProviders = _owner->currentState.symbolProviders;
    }

    // Obtain symbols from each of symbol provider
    _sourceData.clear();
    for(auto itProvider = symbolProviders.cbegin(); itProvider != symbolProviders.cend(); ++itProvider)
    {
        const auto& provider = *itProvider;

        //TODO: a cache of symbols needs to be maintained, since same symbol may be present in several tiles, but it should be drawn once?
        provider->obtainSymbols(tileId, zoom, _sourceData);
    }

    return false;
}

bool OsmAnd::MapRenderer::SymbolsResourceEntry::uploadToGPU()
{
    return false;
}

void OsmAnd::MapRenderer::SymbolsResourceEntry::unloadFromGPU()
{

}
