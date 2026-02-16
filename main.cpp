// Copyright (C) 2025 Signal Slot Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include <QtCore/QCommandLineParser>
#include <QtCore/QDir>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtGui/QGuiApplication>
#include <QtGui/QPainter>
#include <QtPsdCore/qpsdblend.h>
#include <QtMcpServer/QMcpServer>
#include <QtPsdGui/QPsdAbstractLayerItem>
#include <QtPsdGui/qpsdguiglobal.h>
#include <QtPsdGui/QPsdFolderLayerItem>
#include <QtPsdGui/QPsdFontMapper>
#include <QtPsdGui/QPsdGuiLayerTreeItemModel>
#include <QtPsdGui/QPsdImageLayerItem>
#include <QtPsdGui/QPsdShapeLayerItem>
#include <QtPsdGui/QPsdTextLayerItem>
#include <QtPsdExporter/QPsdExporterPlugin>
#include <QtPsdExporter/QPsdExporterTreeItemModel>

using namespace Qt::StringLiterals;

static QString toJson(const QJsonObject &obj)
{
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

class McpServer : public QMcpServer
{
    Q_OBJECT
public:
    explicit McpServer(const QString &backend = "stdio"_L1, QObject *parent = nullptr)
        : QMcpServer(backend, parent)
    {
        exporterModel.setSourceModel(&guiModel);
    }

    Q_INVOKABLE QString load_psd(const QString &path)
    {
        exporterModel.load(path);
        const auto err = exporterModel.errorMessage();
        if (!err.isEmpty())
            return toJson(QJsonObject{{"error"_L1, err}});

        const auto sz = exporterModel.size();
        return toJson(QJsonObject{
            {"file"_L1, exporterModel.fileName()},
            {"width"_L1, sz.width()},
            {"height"_L1, sz.height()},
            {"layerCount"_L1, countLayers({})}
        });
    }

    Q_INVOKABLE QString get_layer_tree()
    {
        if (exporterModel.fileName().isEmpty())
            return toJson(QJsonObject{{"error"_L1, "No PSD file loaded"_L1}});

        QJsonArray tree;
        buildTree({}, tree);
        return toJson(QJsonObject{
            {"file"_L1, exporterModel.fileName()},
            {"layers"_L1, tree}
        });
    }

    Q_INVOKABLE QString get_layer_details(int layerId)
    {
        auto index = findLayerById(layerId);
        if (!index.isValid())
            return toJson(QJsonObject{{"error"_L1, u"Layer %1 not found"_s.arg(layerId)}});

        QJsonObject obj;
        obj["layerId"_L1] = exporterModel.layerId(index);
        obj["name"_L1] = exporterModel.layerName(index);
        const auto r = exporterModel.rect(index);
        obj["rect"_L1] = QJsonObject{
            {"x"_L1, r.x()}, {"y"_L1, r.y()},
            {"width"_L1, r.width()}, {"height"_L1, r.height()}
        };

        const auto *item = exporterModel.layerItem(index);
        if (item) {
            obj["opacity"_L1] = item->opacity();
            obj["fillOpacity"_L1] = item->fillOpacity();

            switch (item->type()) {
            case QPsdAbstractLayerItem::Text: {
                obj["type"_L1] = "text"_L1;
                const auto *text = static_cast<const QPsdTextLayerItem *>(item);
                QJsonArray runs;
                for (const auto &run : text->runs()) {
                    runs.append(QJsonObject{
                        {"text"_L1, run.text},
                        {"font"_L1, run.font.family()},
                        {"originalFont"_L1, run.originalFontName},
                        {"fontSize"_L1, run.font.pointSizeF()},
                        {"color"_L1, run.color.name()},
                    });
                }
                obj["runs"_L1] = runs;
                obj["textType"_L1] = text->textType() == QPsdTextLayerItem::TextType::PointText
                    ? "point"_L1 : "paragraph"_L1;
                break;
            }
            case QPsdAbstractLayerItem::Shape: {
                obj["type"_L1] = "shape"_L1;
                const auto *shape = static_cast<const QPsdShapeLayerItem *>(item);
                const auto pi = shape->pathInfo();
                static const char *pathTypes[] = {"none", "rectangle", "roundedRectangle", "path"};
                obj["pathType"_L1] = QString::fromLatin1(pathTypes[pi.type]);
                if (pi.type == QPsdAbstractLayerItem::PathInfo::RoundedRectangle)
                    obj["cornerRadius"_L1] = pi.radius;
                obj["brushColor"_L1] = shape->brush().color().name();
                break;
            }
            case QPsdAbstractLayerItem::Image: {
                obj["type"_L1] = "image"_L1;
                const auto lf = item->linkedFile();
                if (!lf.name.isEmpty())
                    obj["linkedFile"_L1] = lf.name;
                break;
            }
            case QPsdAbstractLayerItem::Folder: {
                obj["type"_L1] = "folder"_L1;
                const auto *folder = static_cast<const QPsdFolderLayerItem *>(item);
                obj["isOpened"_L1] = folder->isOpened();
                if (!folder->artboardPresetName().isEmpty()) {
                    obj["artboard"_L1] = QJsonObject{
                        {"presetName"_L1, folder->artboardPresetName()},
                        {"background"_L1, folder->artboardBackground().name()},
                    };
                }
                obj["childCount"_L1] = exporterModel.rowCount(index);
                break;
            }
            }
        }

        // Export hint
        const auto hint = exporterModel.layerHint(index);
        QJsonObject hintObj;
        hintObj["type"_L1] = hintTypeName(hint.type);
        if (!hint.id.isEmpty())
            hintObj["id"_L1] = hint.id;
        if (!hint.componentName.isEmpty())
            hintObj["componentName"_L1] = hint.componentName;
        if (hint.type == QPsdExporterTreeItemModel::ExportHint::Native)
            hintObj["baseElement"_L1] = QPsdExporterTreeItemModel::ExportHint::nativeCode2Name(hint.baseElement);
        hintObj["visible"_L1] = hint.visible;
        if (!hint.properties.isEmpty()) {
            QJsonArray propsArr;
            for (const auto &prop : hint.properties)
                propsArr.append(prop);
            hintObj["properties"_L1] = propsArr;
        }
        obj["exportHint"_L1] = hintObj;

        return toJson(obj);
    }

    Q_INVOKABLE QString set_export_hint(int layerId, const QString &type, const QString &options)
    {
        auto index = findLayerById(layerId);
        if (!index.isValid())
            return toJson(QJsonObject{{"error"_L1, u"Layer %1 not found"_s.arg(layerId)}});

        static const QHash<QString, QPsdExporterTreeItemModel::ExportHint::Type> typeMap = {
            {"embed"_L1,  QPsdExporterTreeItemModel::ExportHint::Embed},
            {"merge"_L1,  QPsdExporterTreeItemModel::ExportHint::Merge},
            {"custom"_L1, QPsdExporterTreeItemModel::ExportHint::Component},
            {"native"_L1, QPsdExporterTreeItemModel::ExportHint::Native},
            {"skip"_L1,   QPsdExporterTreeItemModel::ExportHint::Skip},
            {"none"_L1,   QPsdExporterTreeItemModel::ExportHint::None},
        };

        const auto lower = type.toLower();
        if (!typeMap.contains(lower))
            return toJson(QJsonObject{{"error"_L1, u"Unknown type: %1. Use: embed, merge, custom, native, skip, none"_s.arg(type)}});

        const auto opts = QJsonDocument::fromJson(options.toUtf8()).object();

        auto hint = exporterModel.layerHint(index);
        hint.type = typeMap.value(lower);
        if (opts.contains("id"_L1))
            hint.id = opts["id"_L1].toString();
        if (opts.contains("visible"_L1))
            hint.visible = opts["visible"_L1].toBool();
        if (opts.contains("componentName"_L1) && !opts["componentName"_L1].toString().isEmpty())
            hint.componentName = opts["componentName"_L1].toString();
        if (opts.contains("baseElement"_L1) && !opts["baseElement"_L1].toString().isEmpty())
            hint.baseElement = QPsdExporterTreeItemModel::ExportHint::nativeName2Code(opts["baseElement"_L1].toString());
        if (opts.contains("properties"_L1)) {
            hint.properties.clear();
            const auto propsArr = opts["properties"_L1].toArray();
            for (const auto &val : propsArr)
                hint.properties.insert(val.toString());
        }

        exporterModel.setLayerHint(index, hint);

        QJsonArray propsArr;
        for (const auto &prop : hint.properties)
            propsArr.append(prop);
        return toJson(QJsonObject{
            {"layerId"_L1, layerId},
            {"id"_L1, hint.id},
            {"type"_L1, lower},
            {"componentName"_L1, hint.componentName},
            {"baseElement"_L1, QPsdExporterTreeItemModel::ExportHint::nativeCode2Name(hint.baseElement)},
            {"visible"_L1, hint.visible},
            {"properties"_L1, propsArr},
        });
    }

    Q_INVOKABLE QString do_export(const QString &format, const QString &outputDir, const QString &options)
    {
        if (exporterModel.fileName().isEmpty())
            return toJson(QJsonObject{{"error"_L1, "No PSD file loaded"_L1}});

        auto *plugin = QPsdExporterPlugin::plugin(format.toUtf8());
        if (!plugin)
            return toJson(QJsonObject{{"error"_L1, u"Unknown exporter: %1"_s.arg(format)}});

        QDir outDir(outputDir);
        if (!outDir.exists() && !outDir.mkpath("."_L1))
            return toJson(QJsonObject{{"error"_L1, u"Cannot create directory: %1"_s.arg(outputDir)}});

        const auto opts = QJsonDocument::fromJson(options.toUtf8()).object();
        const auto sz = exporterModel.size();
        const int w = opts["width"_L1].toInt(0) > 0 ? opts["width"_L1].toInt() : sz.width();
        const int h = opts["height"_L1].toInt(0) > 0 ? opts["height"_L1].toInt() : sz.height();

        QVariantMap hint;
        hint.insert("width"_L1, w);
        hint.insert("height"_L1, h);
        hint.insert("fontScaleFactor"_L1, opts["fontScaleFactor"_L1].toDouble(1.0));
        hint.insert("imageScaling"_L1, opts["imageScaling"_L1].toBool(false));
        hint.insert("makeCompact"_L1, opts["makeCompact"_L1].toBool(false));

        if (!plugin->exportTo(&exporterModel, outputDir, hint))
            return toJson(QJsonObject{{"error"_L1, "Export failed"_L1}});

        return toJson(QJsonObject{
            {"format"_L1, format},
            {"outputDir"_L1, outputDir},
            {"width"_L1, w},
            {"height"_L1, h},
        });
    }

    Q_INVOKABLE QString list_exporters()
    {
        QJsonArray arr;
        for (const auto &key : QPsdExporterPlugin::keys()) {
            auto *plugin = QPsdExporterPlugin::plugin(key);
            if (!plugin)
                continue;
            arr.append(QJsonObject{
                {"key"_L1, QString::fromUtf8(key)},
                {"name"_L1, plugin->name()},
                {"type"_L1, plugin->exportType() == QPsdExporterPlugin::Directory
                     ? "directory"_L1 : "file"_L1},
            });
        }
        return toJson(QJsonObject{{"exporters"_L1, arr}});
    }

    Q_INVOKABLE QString save_hints()
    {
        if (exporterModel.fileName().isEmpty())
            return toJson(QJsonObject{{"error"_L1, "No PSD file loaded"_L1}});

        exporterModel.save();
        return toJson(QJsonObject{{"saved"_L1, true}});
    }

    Q_INVOKABLE QImage get_layer_image(int layerId)
    {
        auto index = findLayerById(layerId);
        if (!index.isValid())
            return {};

        const auto *item = exporterModel.layerItem(index);
        if (!item)
            return {};

        if (item->type() != QPsdAbstractLayerItem::Folder)
            return item->image();

        // Folder layer: composite all visible children
        const QRect bounds = computeBoundingRect(index);
        if (bounds.isEmpty())
            return {};

        QImage canvas(bounds.size(), QImage::Format_ARGB32);
        canvas.fill(Qt::transparent);

        QPainter painter(&canvas);
        const auto blendMode = item->record().blendMode();
        const bool passThrough = (blendMode == QPsdBlend::PassThrough);
        compositeChildren(index, painter, bounds.topLeft(), passThrough);
        painter.end();

        return canvas;
    }

    Q_INVOKABLE QString get_fonts_used()
    {
        if (exporterModel.fileName().isEmpty())
            return toJson(QJsonObject{{"error"_L1, "No PSD file loaded"_L1}});

        QSet<QString> seen;
        QJsonArray fonts;
        collectFonts({}, seen, fonts);
        return toJson(QJsonObject{{"fonts"_L1, fonts}});
    }

    Q_INVOKABLE QString get_font_mappings()
    {
        if (exporterModel.fileName().isEmpty())
            return toJson(QJsonObject{{"error"_L1, "No PSD file loaded"_L1}});

        auto *mapper = QPsdFontMapper::instance();

        QJsonObject global;
        const auto globalMap = mapper->globalMappings();
        for (auto it = globalMap.cbegin(); it != globalMap.cend(); ++it)
            global[it.key()] = it.value();

        QJsonObject context;
        const auto contextMap = mapper->contextMappings(exporterModel.fileName());
        for (auto it = contextMap.cbegin(); it != contextMap.cend(); ++it)
            context[it.key()] = it.value();

        return toJson(QJsonObject{
            {"global"_L1, global},
            {"context"_L1, context},
        });
    }

    Q_INVOKABLE QString set_font_mapping(const QString &fromFont, const QString &toFont, bool global)
    {
        if (exporterModel.fileName().isEmpty())
            return toJson(QJsonObject{{"error"_L1, "No PSD file loaded"_L1}});

        auto *mapper = QPsdFontMapper::instance();

        if (global) {
            if (toFont.isEmpty())
                mapper->removeGlobalMapping(fromFont);
            else
                mapper->setGlobalMapping(fromFont, toFont);
            mapper->saveGlobalMappings();
        } else {
            auto mappings = mapper->contextMappings(exporterModel.fileName());
            if (toFont.isEmpty())
                mappings.remove(fromFont);
            else
                mappings[fromFont] = toFont;
            mapper->setContextMappings(exporterModel.fileName(), mappings);
        }

        return toJson(QJsonObject{
            {"fromFont"_L1, fromFont},
            {"toFont"_L1, toFont},
            {"global"_L1, global},
        });
    }

    QHash<QString, QString> toolDescriptions() const override
    {
        return {
            {"load_psd"_L1, "Load a PSD file for inspection and export"_L1},
            {"load_psd/path"_L1, "Absolute path to the PSD file"_L1},

            {"get_layer_tree"_L1, "Get the layer tree structure of the loaded PSD file"_L1},

            {"get_layer_details"_L1, "Get detailed information about a specific layer"_L1},
            {"get_layer_details/layerId"_L1, "Layer ID to inspect"_L1},

            {"set_export_hint"_L1, "Configure how a layer should be exported"_L1},
            {"set_export_hint/layerId"_L1, "Layer ID to configure"_L1},
            {"set_export_hint/type"_L1, "Export type: embed, merge, custom, native, skip, or none"_L1},
            {"set_export_hint/options"_L1, "JSON object with optional keys: id (string, identifier for binding — empty string to clear), visible (bool), componentName (string, for custom type), baseElement (string: Container, TouchArea, Button, Button_Highlighted, for native type), properties (array of strings: visible, color, position, text, size, image — controls which attributes are exported as bindable properties)"_L1},

            {"do_export"_L1, "Export the loaded PSD to a target format and directory"_L1},
            {"do_export/format"_L1, "Exporter plugin key (use list_exporters to see available ones)"_L1},
            {"do_export/outputDir"_L1, "Absolute path to the output directory"_L1},
            {"do_export/options"_L1, "JSON object with optional keys: width (int), height (int), fontScaleFactor (double), imageScaling (bool), makeCompact (bool). Width/height 0 or omitted = original size"_L1},

            {"list_exporters"_L1, "List all available exporter plugins"_L1},

            {"save_hints"_L1, "Persist current export hints to the PSD sidecar file"_L1},

            {"get_layer_image"_L1, "Get the rendered image of a specific layer"_L1},
            {"get_layer_image/layerId"_L1, "Layer ID to get the image from"_L1},

            {"get_fonts_used"_L1, "List all fonts used in the loaded PSD file with their resolved mappings"_L1},

            {"get_font_mappings"_L1, "Get current font mapping settings (global and per-PSD context)"_L1},

            {"set_font_mapping"_L1, "Set or remove a font mapping"_L1},
            {"set_font_mapping/fromFont"_L1, "Original font name from PSD (e.g. MyriadPro-Bold)"_L1},
            {"set_font_mapping/toFont"_L1, "Target font name to map to (empty string to remove mapping)"_L1},
            {"set_font_mapping/global"_L1, "If true, applies globally; if false, applies only to the currently loaded PSD"_L1},
        };
    }

private:
    QPsdGuiLayerTreeItemModel guiModel;
    QPsdExporterTreeItemModel exporterModel;

    QModelIndex findLayerById(qint32 id, const QModelIndex &parent = {}) const
    {
        for (int row = 0; row < exporterModel.rowCount(parent); ++row) {
            auto index = exporterModel.index(row, 0, parent);
            if (exporterModel.layerId(index) == id)
                return index;
            auto found = findLayerById(id, index);
            if (found.isValid())
                return found;
        }
        return {};
    }

    int countLayers(const QModelIndex &parent) const
    {
        int count = 0;
        for (int row = 0; row < exporterModel.rowCount(parent); ++row) {
            count++;
            count += countLayers(exporterModel.index(row, 0, parent));
        }
        return count;
    }

    void buildTree(const QModelIndex &parent, QJsonArray &array) const
    {
        for (int row = 0; row < exporterModel.rowCount(parent); ++row) {
            auto index = exporterModel.index(row, 0, parent);
            QJsonObject obj;
            obj["layerId"_L1] = exporterModel.layerId(index);
            obj["name"_L1] = exporterModel.layerName(index);
            const auto *item = exporterModel.layerItem(index);
            if (item) {
                switch (item->type()) {
                case QPsdAbstractLayerItem::Text:   obj["type"_L1] = "text"_L1;   break;
                case QPsdAbstractLayerItem::Shape:  obj["type"_L1] = "shape"_L1;  break;
                case QPsdAbstractLayerItem::Image:  obj["type"_L1] = "image"_L1;  break;
                case QPsdAbstractLayerItem::Folder: obj["type"_L1] = "folder"_L1; break;
                }
            }

            const auto hint = exporterModel.layerHint(index);
            obj["hintType"_L1] = hintTypeName(hint.type);
            obj["visible"_L1] = hint.visible;
            if (!hint.properties.isEmpty()) {
                QJsonArray propsArr;
                for (const auto &prop : hint.properties)
                    propsArr.append(prop);
                obj["properties"_L1] = propsArr;
            }

            if (exporterModel.rowCount(index) > 0) {
                QJsonArray children;
                buildTree(index, children);
                obj["children"_L1] = children;
            }

            array.append(obj);
        }
    }

    void collectFonts(const QModelIndex &parent, QSet<QString> &seen, QJsonArray &fonts) const
    {
        const auto psdPath = exporterModel.fileName();
        for (int row = 0; row < exporterModel.rowCount(parent); ++row) {
            auto index = exporterModel.index(row, 0, parent);
            const auto *item = exporterModel.layerItem(index);
            if (item && item->type() == QPsdAbstractLayerItem::Text) {
                const auto *text = static_cast<const QPsdTextLayerItem *>(item);
                for (const auto &run : text->runs()) {
                    if (!run.originalFontName.isEmpty() && !seen.contains(run.originalFontName)) {
                        seen.insert(run.originalFontName);
                        const auto resolved = QPsdFontMapper::instance()->resolveFont(run.originalFontName, psdPath);
                        fonts.append(QJsonObject{
                            {"psdFont"_L1, run.originalFontName},
                            {"resolvedFont"_L1, resolved.family()},
                            {"resolvedStyle"_L1, resolved.styleName()},
                        });
                    }
                }
            }
            collectFonts(index, seen, fonts);
        }
    }

    static QString hintTypeName(QPsdExporterTreeItemModel::ExportHint::Type t)
    {
        static const char *names[] = {"embed", "merge", "custom", "native", "skip", "none"};
        return QString::fromLatin1(names[t]);
    }

    // Recursively compute the bounding box of all child layers under `parent`
    QRect computeBoundingRect(const QModelIndex &parent) const
    {
        QRect bounds;
        for (int row = 0; row < exporterModel.rowCount(parent); ++row) {
            auto index = exporterModel.index(row, 0, parent);
            const auto *item = exporterModel.layerItem(index);
            if (!item || !item->isVisible())
                continue;
            if (item->type() == QPsdAbstractLayerItem::Folder) {
                bounds = bounds.united(computeBoundingRect(index));
            } else {
                bounds = bounds.united(item->rect());
            }
        }
        return bounds;
    }

    // Apply transparency mask and layer mask to a layer's image
    QImage applyMasks(const QPsdAbstractLayerItem *item) const
    {
        QImage image = item->image();
        if (image.isNull())
            return image;

        // Apply transparency mask for layers without built-in alpha
        const QImage transMask = item->transparencyMask();
        if (!transMask.isNull() && !image.hasAlphaChannel()) {
            image = image.convertToFormat(QImage::Format_ARGB32);
            for (int y = 0; y < qMin(image.height(), transMask.height()); ++y) {
                QRgb *imgLine = reinterpret_cast<QRgb *>(image.scanLine(y));
                const uchar *maskLine = transMask.constScanLine(y);
                for (int x = 0; x < qMin(image.width(), transMask.width()); ++x) {
                    imgLine[x] = qRgba(qRed(imgLine[x]), qGreen(imgLine[x]),
                                       qBlue(imgLine[x]), maskLine[x]);
                }
            }
        }

        // Apply raster layer mask if present
        const QImage layerMask = item->layerMask();
        if (!layerMask.isNull()) {
            const QRect maskRect = item->layerMaskRect();
            const QRect layerRect = item->rect();
            const int defaultColor = item->layerMaskDefaultColor();

            image = image.convertToFormat(QImage::Format_ARGB32);
            for (int y = 0; y < image.height(); ++y) {
                QRgb *scanLine = reinterpret_cast<QRgb *>(image.scanLine(y));
                for (int x = 0; x < image.width(); ++x) {
                    const int maskX = (layerRect.x() + x) - maskRect.x();
                    const int maskY = (layerRect.y() + y) - maskRect.y();
                    int maskValue = defaultColor;
                    if (maskX >= 0 && maskX < layerMask.width() &&
                        maskY >= 0 && maskY < layerMask.height()) {
                        maskValue = qGray(layerMask.pixel(maskX, maskY));
                    }
                    const int alpha = qAlpha(scanLine[x]);
                    const int newAlpha = (alpha * maskValue) / 255;
                    scanLine[x] = qRgba(qRed(scanLine[x]), qGreen(scanLine[x]),
                                        qBlue(scanLine[x]), newAlpha);
                }
            }
        }

        return image;
    }

    // Recursively composite visible children onto the given painter.
    // `origin` is the top-left of the canvas in document coordinates.
    // `passThrough` means children are drawn directly (no intermediate buffer).
    void compositeChildren(const QModelIndex &parent, QPainter &painter,
                           const QPoint &origin, bool passThrough) const
    {
        const int count = exporterModel.rowCount(parent);
        // Iterate bottom-to-top (last row = bottommost layer in PSD model)
        for (int row = count - 1; row >= 0; --row) {
            auto index = exporterModel.index(row, 0, parent);
            const auto *item = exporterModel.layerItem(index);
            if (!item || !item->isVisible())
                continue;

            if (item->type() == QPsdAbstractLayerItem::Folder) {
                const auto folderBlend = item->record().blendMode();
                const bool folderPassThrough = (folderBlend == QPsdBlend::PassThrough);

                if (folderPassThrough) {
                    // PassThrough: children draw directly onto the current canvas
                    compositeChildren(index, painter, origin, true);
                } else {
                    // Non-PassThrough: composite children into an intermediate buffer
                    const QRect childBounds = computeBoundingRect(index);
                    if (childBounds.isEmpty())
                        continue;

                    QImage groupCanvas(childBounds.size(), QImage::Format_ARGB32);
                    groupCanvas.fill(Qt::transparent);

                    QPainter groupPainter(&groupCanvas);
                    compositeChildren(index, groupPainter, childBounds.topLeft(), false);
                    groupPainter.end();

                    // Draw the group buffer with the folder's blend mode and opacity
                    painter.save();
                    painter.setCompositionMode(QtPsdGui::compositionMode(folderBlend));
                    painter.setOpacity(painter.opacity() * item->opacity() * item->fillOpacity());
                    painter.drawImage(childBounds.topLeft() - origin, groupCanvas);
                    painter.restore();
                }
            } else {
                // Leaf layer: apply masks, then draw with blend mode and opacity
                QImage layerImage = applyMasks(item);
                if (layerImage.isNull())
                    continue;

                painter.save();
                painter.setCompositionMode(
                    QtPsdGui::compositionMode(item->record().blendMode()));
                painter.setOpacity(painter.opacity() * item->opacity() * item->fillOpacity());
                painter.drawImage(item->rect().topLeft() - origin, layerImage);
                painter.restore();
            }
        }
    }
};

int main(int argc, char *argv[])
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QGuiApplication app(argc, argv);
    app.setApplicationName("mcp-psd2x"_L1);
    app.setApplicationVersion("1.0"_L1);
    app.setOrganizationName("Signal Slot Inc."_L1);
    app.setOrganizationDomain("signal-slot.co.jp"_L1);

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption backendOption(QStringList() << "b"_L1 << "backend"_L1,
                                    "Backend to use (stdio/sse)."_L1,
                                    "backend"_L1, "stdio"_L1);
    parser.addOption(backendOption);

    QCommandLineOption addressOption(QStringList() << "a"_L1 << "address"_L1,
                                    "Address to listen on (host:port)."_L1,
                                    "address"_L1, "127.0.0.1:8000"_L1);
    parser.addOption(addressOption);

    parser.process(app);

    McpServer server(parser.value(backendOption));
    server.start(parser.value(addressOption));

    return app.exec();
}

#include "main.moc"
