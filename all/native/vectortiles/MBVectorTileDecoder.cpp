#include "MBVectorTileDecoder.h"
#include "core/MapTile.h"
#include "core/MapBounds.h"
#include "core/BinaryData.h"
#include "core/Variant.h"
#include "components/Exceptions.h"
#include "geometry/Feature.h"
#include "geometry/Geometry.h"
#include "geometry/PointGeometry.h"
#include "geometry/LineGeometry.h"
#include "geometry/PolygonGeometry.h"
#include "geometry/MultiPointGeometry.h"
#include "geometry/MultiLineGeometry.h"
#include "geometry/MultiPolygonGeometry.h"
#include "graphics/Bitmap.h"
#include "styles/CompiledStyleSet.h"
#include "styles/CartoCSSStyleSet.h"
#include "vectortiles/utils/MapnikVTLogger.h"
#include "vectortiles/utils/VTBitmapLoader.h"
#include "vectortiles/utils/CartoCSSAssetLoader.h"
#include "utils/AssetPackage.h"
#include "utils/FileUtils.h"
#include "utils/Const.h"
#include "utils/Log.h"

#include <vt/Tile.h>
#include <mapnikvt/Value.h>
#include <mapnikvt/SymbolizerParser.h>
#include <mapnikvt/SymbolizerContext.h>
#include <mapnikvt/MBVTFeatureDecoder.h>
#include <mapnikvt/MBVTTileReader.h>
#include <mapnikvt/MapParser.h>
#include <cartocss/CartoCSSMapLoader.h>

#include <functional>

#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string/predicate.hpp>

namespace {

    struct ValueConverter : boost::static_visitor<carto::Variant> {
        carto::Variant operator() (boost::blank) const { return carto::Variant(); }
        template <typename T> carto::Variant operator() (T val) const { return carto::Variant(val); }
    };

    typedef std::function<carto::MapPos(const cglib::vec2<float>& pos)> PointConversionFunction;

    std::vector<carto::MapPos> convertPoints(const PointConversionFunction& convertFn, const std::vector<cglib::vec2<float> >& poses) {
        std::vector<carto::MapPos> points;
        points.reserve(poses.size());
        std::transform(poses.begin(), poses.end(), std::back_inserter(points), convertFn);
        return points;
    }

    std::vector<std::vector<carto::MapPos> > convertPointsList(const PointConversionFunction& convertFn, const std::vector<std::vector<cglib::vec2<float> > >& posesList) {
        std::vector<std::vector<carto::MapPos> > pointsList;
        pointsList.reserve(posesList.size());
        std::transform(posesList.begin(), posesList.end(), std::back_inserter(pointsList), [&](const std::vector<cglib::vec2<float> >& poses) {
            return convertPoints(convertFn, poses);
        });
        return pointsList;
    }

    std::vector<std::vector<std::vector<carto::MapPos> > > convertPointsLists(const PointConversionFunction& convertFn, const std::vector<std::vector<std::vector<cglib::vec2<float> > > >& posesLists) {
        std::vector<std::vector<std::vector<carto::MapPos> > > pointsLists;
        pointsLists.reserve(posesLists.size());
        std::transform(posesLists.begin(), posesLists.end(), std::back_inserter(pointsLists), [&](const std::vector<std::vector<cglib::vec2<float> > >& posesList) {
            return convertPointsList(convertFn, posesList);
        });
        return pointsLists;
    }

    std::shared_ptr<carto::Geometry> convertGeometry(const PointConversionFunction& convertFn, const std::shared_ptr<const carto::mvt::Geometry>& mvtGeometry) {
        if (auto mvtPoint = std::dynamic_pointer_cast<const carto::mvt::PointGeometry>(mvtGeometry)) {
            std::vector<carto::MapPos> poses = convertPoints(convertFn, mvtPoint->getVertices());
            std::vector<std::shared_ptr<carto::PointGeometry> > points;
            points.reserve(poses.size());
            std::transform(poses.begin(), poses.end(), std::back_inserter(points), [](const carto::MapPos& pos) { return std::make_shared<carto::PointGeometry>(pos); });
            if (points.size() == 1) {
                return points.front();
            }
            else {
                return std::make_shared<carto::MultiPointGeometry>(points);
            }
        }
        else if (auto mvtLine = std::dynamic_pointer_cast<const carto::mvt::LineGeometry>(mvtGeometry)) {
            std::vector<std::vector<carto::MapPos>> posesList = convertPointsList(convertFn, mvtLine->getVerticesList());
            std::vector<std::shared_ptr<carto::LineGeometry> > lines;
            lines.reserve(posesList.size());
            std::transform(posesList.begin(), posesList.end(), std::back_inserter(lines), [](const std::vector<carto::MapPos>& poses) { return std::make_shared<carto::LineGeometry>(poses); });
            if (lines.size() == 1) {
                return lines.front();
            }
            else {
                return std::make_shared<carto::MultiLineGeometry>(lines);
            }
        }
        else if (auto mvtPolygon = std::dynamic_pointer_cast<const carto::mvt::PolygonGeometry>(mvtGeometry)) {
            std::vector<std::vector<std::vector<carto::MapPos> > > posesLists = convertPointsLists(convertFn, mvtPolygon->getPolygonList());
            std::vector<std::shared_ptr<carto::PolygonGeometry> > polygons;
            polygons.reserve(posesLists.size());
            std::transform(posesLists.begin(), posesLists.end(), std::back_inserter(polygons), [](const std::vector<std::vector<carto::MapPos> >& posesList) { return std::make_shared<carto::PolygonGeometry>(posesList); });
            if (polygons.size() == 1) {
                return polygons.front();
            }
            else {
                return std::make_shared<carto::MultiPolygonGeometry>(polygons);
            }
        }
        return std::shared_ptr<carto::Geometry>();
    }

}

namespace carto {
    
    MBVectorTileDecoder::MBVectorTileDecoder(const std::shared_ptr<CompiledStyleSet>& compiledStyleSet) :
        _buffer(0),
        _featureIdOverride(false),
        _cartoCSSLayerNamesIgnored(false),
        _layerNameOverride(),
        _logger(std::make_shared<MapnikVTLogger>("MBVectorTileDecoder")),
        _map(),
        _parameterValueMap(),
        _backgroundPattern(),
        _symbolizerContext(),
        _styleSet(compiledStyleSet)
    {
        if (!compiledStyleSet) {
            throw NullArgumentException("Null compiledStyleSet");
        }

        updateCurrentStyle(compiledStyleSet);
    }
    
    MBVectorTileDecoder::MBVectorTileDecoder(const std::shared_ptr<CartoCSSStyleSet>& cartoCSSStyleSet) :
        _buffer(0),
        _featureIdOverride(false),
        _cartoCSSLayerNamesIgnored(false),
        _layerNameOverride(),
        _logger(std::make_shared<MapnikVTLogger>("MBVectorTileDecoder")),
        _map(),
        _parameterValueMap(),
        _backgroundPattern(),
        _symbolizerContext(),
        _styleSet(cartoCSSStyleSet)
    {
        if (!cartoCSSStyleSet) {
            throw NullArgumentException("Null cartoCSSStyleSet");
        }

        updateCurrentStyle(cartoCSSStyleSet);
    }
    
    MBVectorTileDecoder::~MBVectorTileDecoder() {
    }
        
    std::shared_ptr<CompiledStyleSet> MBVectorTileDecoder::getCompiledStyleSet() const {
        std::lock_guard<std::mutex> lock(_mutex);
        if (auto compiledStyleSet = boost::get<std::shared_ptr<CompiledStyleSet> >(&_styleSet)) {
            return *compiledStyleSet;
        }
        return std::shared_ptr<CompiledStyleSet>();
    }
    
    void MBVectorTileDecoder::setCompiledStyleSet(const std::shared_ptr<CompiledStyleSet>& styleSet) {
        if (!styleSet) {
            throw NullArgumentException("Null styleSet");
        }

        updateCurrentStyle(styleSet);
        notifyDecoderChanged();
    }

    std::shared_ptr<CartoCSSStyleSet> MBVectorTileDecoder::getCartoCSSStyleSet() const {
        std::lock_guard<std::mutex> lock(_mutex);

        if (auto cartoCSSStyleSet = boost::get<std::shared_ptr<CartoCSSStyleSet> >(&_styleSet)) {
            return *cartoCSSStyleSet;
        }
        return std::shared_ptr<CartoCSSStyleSet>();
    }
    
    void MBVectorTileDecoder::setCartoCSSStyleSet(const std::shared_ptr<CartoCSSStyleSet>& styleSet) {
        if (!styleSet) {
            throw NullArgumentException("Null styleSet");
        }

        updateCurrentStyle(styleSet);
        notifyDecoderChanged();
    }

    std::vector<std::string> MBVectorTileDecoder::getStyleParameters() const {
        std::lock_guard<std::mutex> lock(_mutex);
    
        std::vector<std::string> params;
        for (auto it = _map->getNutiParameterMap().begin(); it != _map->getNutiParameterMap().end(); it++) {
            params.push_back(it->first);
        }
        return params;
    }

    std::string MBVectorTileDecoder::getStyleParameter(const std::string& param) const {
        std::lock_guard<std::mutex> lock(_mutex);

        auto it = _map->getNutiParameterMap().find(param);
        if (it == _map->getNutiParameterMap().end()) {
            throw InvalidArgumentException("Could not find parameter");
        }
        const mvt::NutiParameter& nutiParam = it->second;
        
        mvt::Value value = nutiParam.getDefaultValue();
        {
            auto it2 = _parameterValueMap->find(param);
            if (it2 != _parameterValueMap->end()) {
                value = it2->second;
            }
        }

        if (!nutiParam.getEnumMap().empty()) {
            for (auto it2 = nutiParam.getEnumMap().begin(); it2 != nutiParam.getEnumMap().end(); it2++) {
                if (it2->second == value) {
                    return it2->first;
                }
            }
        }
        else {
            if (auto val = boost::get<bool>(&value)) {
                return boost::lexical_cast<std::string>(*val);
            }
            else if (auto val = boost::get<long long>(&value)) {
                return boost::lexical_cast<std::string>(*val);
            }
            else if (auto val = boost::get<double>(&value)) {
                return boost::lexical_cast<std::string>(*val);
            }
            else if (auto val = boost::get<std::string>(&value)) {
                return *val;
            }
        }
        return std::string();
    }

    bool MBVectorTileDecoder::setStyleParameter(const std::string& param, const std::string& value) {
        {
            std::lock_guard<std::mutex> lock(_mutex);
    
            auto it = _map->getNutiParameterMap().find(param);
            if (it == _map->getNutiParameterMap().end()) {
                Log::Infof("MBVectorTileDecoder::setStyleParameter: Could not find parameter: %s", param.c_str());
                return false;
            }
            const mvt::NutiParameter& nutiParam = it->second;

            if (!nutiParam.getEnumMap().empty()) {
                auto it2 = nutiParam.getEnumMap().find(boost::lexical_cast<std::string>(value));
                if (it2 == nutiParam.getEnumMap().end()) {
                    Log::Infof("MBVectorTileDecoder::setStyleParameter: Illegal enum value for parameter: %s/%s", param.c_str(), value.c_str());
                    return false;
                }
                (*_parameterValueMap)[param] = it2->second;
            } else {
                mvt::Value val = nutiParam.getDefaultValue();
                if (boost::get<bool>(&val)) {
                    if (value == "true") {
                        val = mvt::Value(true);
                    }
                    else if (value == "false") {
                        val = mvt::Value(false);
                    }
                    else {
                        val = mvt::Value(boost::lexical_cast<bool>(value));
                    }
                }
                else if (boost::get<long long>(&val)) {
                    val = mvt::Value(boost::lexical_cast<long long>(value));
                }
                else if (boost::get<double>(&val)) {
                    val = mvt::Value(boost::lexical_cast<double>(value));
                }
                else if (boost::get<std::string>(&val)) {
                    val = value;
                }
                (*_parameterValueMap)[param] = val;
            }
    
            mvt::SymbolizerContext::Settings settings(DEFAULT_TILE_SIZE, *_parameterValueMap);
            _symbolizerContext = std::make_shared<mvt::SymbolizerContext>(_symbolizerContext->getBitmapManager(), _symbolizerContext->getFontManager(), _symbolizerContext->getStrokeMap(), _symbolizerContext->getGlyphMap(), settings);
        }
        notifyDecoderChanged();
        return true;
    }

    float MBVectorTileDecoder::getBuffering() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _buffer;
    }
        
    void MBVectorTileDecoder::setBuffering(float buffer) {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _buffer = buffer;
        }
        notifyDecoderChanged();
    }

    bool MBVectorTileDecoder::isFeatureIdOverride() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _featureIdOverride;
    }

    void MBVectorTileDecoder::setFeatureIdOverride(bool idOverride) {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _featureIdOverride = idOverride;
        }
        notifyDecoderChanged();
    }
        
    bool MBVectorTileDecoder::isCartoCSSLayerNamesIgnored() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _cartoCSSLayerNamesIgnored;
    }

    void MBVectorTileDecoder::setCartoCSSLayerNamesIgnored(bool ignore) {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _cartoCSSLayerNamesIgnored = ignore;
        }
        notifyDecoderChanged();
    }
        
    std::string MBVectorTileDecoder::getLayerNameOverride() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _layerNameOverride;
    }

    void MBVectorTileDecoder::setLayerNameOverride(const std::string& name) {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _layerNameOverride = name;
        }
        notifyDecoderChanged();
    }

    Color MBVectorTileDecoder::getBackgroundColor() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return Color(_map->getSettings().backgroundColor.value());
    }
    
    std::shared_ptr<const vt::BitmapPattern> MBVectorTileDecoder::getBackgroundPattern() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _backgroundPattern;
    }
        
    int MBVectorTileDecoder::getMinZoom() const {
        return 0;
    }
    
    int MBVectorTileDecoder::getMaxZoom() const {
        return Const::MAX_SUPPORTED_ZOOM_LEVEL;
    }

    std::shared_ptr<MBVectorTileDecoder::TileFeature> MBVectorTileDecoder::decodeFeature(long long id, const vt::TileId& tile, const std::shared_ptr<BinaryData>& tileData, const MapBounds& tileBounds) const {
        if (!tileData) {
            Log::Warn("MBVectorTileDecoder::decodeFeature: Null tile data");
            return std::shared_ptr<TileFeature>();
        }
        if (tileData->empty()) {
            return std::shared_ptr<TileFeature>();
        }

        try {
            std::shared_ptr<mvt::MBVTFeatureDecoder> decoder;
            {
                std::unique_lock<std::mutex> lock(_mutex);
                if (_cachedFeatureDecoder.first != tileData) {
                    lock.unlock();
                    decoder = std::make_shared<mvt::MBVTFeatureDecoder>(*tileData->getDataPtr(), _logger);
                    lock.lock();
                    _cachedFeatureDecoder = std::make_pair(tileData, decoder);
                }
                else {
                    decoder = _cachedFeatureDecoder.second;
                }
            }

            std::string mvtLayerName;
            std::shared_ptr<mvt::Feature> mvtFeature = decoder->getFeature(id, mvtLayerName);
            if (!mvtFeature) {
                return std::shared_ptr<TileFeature>();
            }

            std::map<std::string, Variant> featureData;
            if (std::shared_ptr<const mvt::FeatureData> mvtFeatureData = mvtFeature->getFeatureData()) {
                for (const std::string& varName : mvtFeatureData->getVariableNames()) {
                    mvt::Value mvtValue;
                    mvtFeatureData->getVariable(varName, mvtValue);
                    featureData[varName] = boost::apply_visitor(ValueConverter(), mvtValue);
                }
            }

            auto convertFn = [&tileBounds](const cglib::vec2<float>& pos) {
                return MapPos(tileBounds.getMin().getX() + pos(0) * tileBounds.getDelta().getX(), tileBounds.getMax().getY() - pos(1) * tileBounds.getDelta().getY(), 0);
            };
            auto feature = std::make_shared<Feature>(convertGeometry(convertFn, mvtFeature->getGeometry()), Variant(featureData));
            return std::make_shared<TileFeature>(mvtFeature->getId(), mvtLayerName, feature);
        } catch (const std::exception& ex) {
            Log::Errorf("MBVectorTileDecoder::decodeFeature: Exception while decoding: %s", ex.what());
        }
        return std::shared_ptr<TileFeature>();
    }
        
    std::shared_ptr<MBVectorTileDecoder::TileMap> MBVectorTileDecoder::decodeTile(const vt::TileId& tile, const vt::TileId& targetTile, const std::shared_ptr<BinaryData>& tileData) const {
        if (!tileData) {
            Log::Warn("MBVectorTileDecoder::decodeTile: Null tile data");
            return std::shared_ptr<TileMap>();
        }
        if (tileData->empty()) {
            return std::shared_ptr<TileMap>();
        }

        std::shared_ptr<mvt::Map> map;
        std::shared_ptr<mvt::SymbolizerContext> symbolizerContext;
        float buffer;
        bool featureIdOverride;
        std::string layerNameOverride;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            map = _map;
            symbolizerContext = _symbolizerContext;
            buffer = _buffer;
            featureIdOverride = _featureIdOverride;
            layerNameOverride = _layerNameOverride;
        }
    
        try {
            mvt::MBVTFeatureDecoder decoder(*tileData->getDataPtr(), _logger);
            decoder.setTransform(calculateTileTransform(tile, targetTile));
            decoder.setBuffer(buffer);
            decoder.setGlobalIdOverride(featureIdOverride, MapTile(tile.x, tile.y, tile.zoom, 0).getTileId());
            
            mvt::MBVTTileReader reader(map, *symbolizerContext, decoder);
            reader.setLayerNameOverride(layerNameOverride);

            if (std::shared_ptr<vt::Tile> tile = reader.readTile(targetTile)) {
                auto tileMap = std::make_shared<TileMap>();
                (*tileMap)[0] = tile;
                return tileMap;
            }
        } catch (const std::exception& ex) {
            Log::Errorf("MBVectorTileDecoder::decodeTile: Exception while decoding: %s", ex.what());
        }
        return std::shared_ptr<TileMap>();
    }

    void MBVectorTileDecoder::updateCurrentStyle(const boost::variant<std::shared_ptr<CompiledStyleSet>, std::shared_ptr<CartoCSSStyleSet> >& styleSet) {
        std::lock_guard<std::mutex> lock(_mutex);

        std::string styleAssetName;
        std::shared_ptr<AssetPackage> styleSetData;
        std::shared_ptr<mvt::Map> map;

        if (auto cartoCSSStyleSet = boost::get<std::shared_ptr<CartoCSSStyleSet> >(&styleSet)) {
            styleAssetName = "";
            styleSetData = (*cartoCSSStyleSet)->getAssetPackage();

            try {
                auto assetLoader = std::make_shared<CartoCSSAssetLoader>("", (*cartoCSSStyleSet)->getAssetPackage());
                css::CartoCSSMapLoader mapLoader(assetLoader, _logger);
                mapLoader.setIgnoreLayerPredicates(_cartoCSSLayerNamesIgnored);
                map = mapLoader.loadMap((*cartoCSSStyleSet)->getCartoCSS());
            }
            catch (const std::exception& ex) {
                throw ParseException("CartoCSS style parsing failed", ex.what());
            }
        }
        else if (auto compiledStyleSet = boost::get<std::shared_ptr<CompiledStyleSet> >(&styleSet)) {
            styleAssetName = (*compiledStyleSet)->getStyleAssetName();
            if (styleAssetName.empty()) {
                throw InvalidArgumentException("Could not find any styles in the style set");
            }

            styleSetData = (*compiledStyleSet)->getAssetPackage();

            std::shared_ptr<BinaryData> styleData;
            if (styleSetData) {
                styleData = styleSetData->loadAsset(styleAssetName);
            }
            if (!styleData) {
                throw GenericException("Failed to load style description asset");
            }

            if (boost::algorithm::ends_with(styleAssetName, ".xml")) {
                pugi::xml_document doc;
                if (!doc.load_buffer(styleData->data(), styleData->size())) {
                    throw ParseException("Style element XML parsing failed");
                }
                try {
                    auto symbolizerParser = std::make_shared<mvt::SymbolizerParser>(_logger);
                    mvt::MapParser mapParser(symbolizerParser, _logger);
                    map = mapParser.parseMap(doc);
                }
                catch (const std::exception& ex) {
                    throw ParseException("XML style processing failed", ex.what());
                }
            }
            else if (boost::algorithm::ends_with(styleAssetName, ".json")) {
                try {
                    auto assetLoader = std::make_shared<CartoCSSAssetLoader>(FileUtils::GetFilePath(styleAssetName), styleSetData);
                    css::CartoCSSMapLoader mapLoader(assetLoader, _logger);
                    mapLoader.setIgnoreLayerPredicates(_cartoCSSLayerNamesIgnored);
                    map = mapLoader.loadMapProject(styleAssetName);
                }
                catch (const std::exception& ex) {
                    throw ParseException("CartoCSS style parsing failed", ex.what());
                }
            }
            else {
                throw GenericException("Failed to detect style asset type");
            }
        } else {
            throw InvalidArgumentException("Invalid style set");
        }

        auto parameterValueMap = std::make_shared<std::map<std::string, mvt::Value> >();
        for (auto it = map->getNutiParameterMap().begin(); it != map->getNutiParameterMap().end(); it++) {
            (*parameterValueMap)[it->first] = it->second.getDefaultValue();
        }

        mvt::SymbolizerContext::Settings settings(DEFAULT_TILE_SIZE, *parameterValueMap);
        auto fontManager = std::make_shared<vt::FontManager>(GLYPHMAP_SIZE, GLYPHMAP_SIZE);
        auto bitmapLoader = std::make_shared<VTBitmapLoader>(FileUtils::GetFilePath(styleAssetName), styleSetData);
        auto bitmapManager = std::make_shared<vt::BitmapManager>(bitmapLoader);
        auto strokeMap = std::make_shared<vt::StrokeMap>(STROKEMAP_SIZE);
        auto glyphMap = std::make_shared<vt::GlyphMap>(GLYPHMAP_SIZE, GLYPHMAP_SIZE);
        auto symbolizerContext = std::make_shared<mvt::SymbolizerContext>(bitmapManager, fontManager, strokeMap, glyphMap, settings);

        if (styleSetData) {
            std::string fontPrefix = map->getSettings().fontDirectory;
            fontPrefix = FileUtils::NormalizePath(FileUtils::GetFilePath(styleAssetName) + fontPrefix + "/");

            for (const std::string& assetName : styleSetData->getAssetNames()) {
                if (assetName.size() > fontPrefix.size() && assetName.substr(0, fontPrefix.size()) == fontPrefix) {
                    if (std::shared_ptr<BinaryData> fontData = styleSetData->loadAsset(assetName)) {
                        fontManager->loadFontData(*fontData->getDataPtr());
                    }
                }
            }
        }

        std::shared_ptr<const vt::BitmapPattern> backgroundPattern;
        if (!map->getSettings().backgroundImage.empty()) {
            backgroundPattern = bitmapManager->loadBitmapPattern(map->getSettings().backgroundImage, 1.0f, 1.0f);
        }

        _map = map;
        _parameterValueMap = parameterValueMap;
        _backgroundPattern = backgroundPattern;
        _symbolizerContext = symbolizerContext;
        _styleSet = styleSet;
    }
    
    const int MBVectorTileDecoder::DEFAULT_TILE_SIZE = 256;
    const int MBVectorTileDecoder::STROKEMAP_SIZE = 512;
    const int MBVectorTileDecoder::GLYPHMAP_SIZE = 2048;
}
