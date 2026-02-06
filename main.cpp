// Copyright (C) 2025 Signal Slot Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include <QtCore/QBuffer>
#include <QtCore/QCommandLineParser>
#include <QtCore/QDir>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtGui/QGuiApplication>
#include <QtMcpServer/QMcpServer>
#include <QtPsdGui/QPsdAbstractLayerItem>
#include <QtPsdGui/QPsdFolderLayerItem>
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

    Q_INVOKABLE QString get_layer_image(int layerId, const QString &format = "png"_L1)
    {
        auto index = findLayerById(layerId);
        if (!index.isValid())
            return toJson(QJsonObject{{"error"_L1, u"Layer %1 not found"_s.arg(layerId)}});

        const auto *item = exporterModel.layerItem(index);
        if (!item)
            return toJson(QJsonObject{{"error"_L1, u"Layer %1 has no item data"_s.arg(layerId)}});

        QImage img = item->image();
        if (img.isNull())
            return toJson(QJsonObject{{"error"_L1, u"Layer %1 has no image data"_s.arg(layerId)}});

        const auto fmt = format.toUpper().toUtf8();
        if (fmt != "PNG" && fmt != "JPEG" && fmt != "JPG")
            return toJson(QJsonObject{{"error"_L1, u"Unsupported format: %1. Use: png, jpeg"_s.arg(format)}});

        QByteArray data;
        QBuffer buffer(&data);
        buffer.open(QIODevice::WriteOnly);
        if (!img.save(&buffer, fmt.constData()))
            return toJson(QJsonObject{{"error"_L1, "Failed to encode image"_L1}});

        return toJson(QJsonObject{
            {"layerId"_L1, layerId},
            {"width"_L1, img.width()},
            {"height"_L1, img.height()},
            {"format"_L1, format.toLower()},
            {"mimeType"_L1, fmt == "PNG" ? "image/png"_L1 : "image/jpeg"_L1},
            {"data"_L1, QString::fromLatin1(data.toBase64())},
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
            {"set_export_hint/options"_L1, "JSON object with optional keys: visible (bool), componentName (string, for custom type), baseElement (string: Container, TouchArea, Button, Button_Highlighted, for native type), properties (array of strings: visible, color, position, text, size, image — controls which attributes are exported as bindable properties)"_L1},

            {"do_export"_L1, "Export the loaded PSD to a target format and directory"_L1},
            {"do_export/format"_L1, "Exporter plugin key (use list_exporters to see available ones)"_L1},
            {"do_export/outputDir"_L1, "Absolute path to the output directory"_L1},
            {"do_export/options"_L1, "JSON object with optional keys: width (int), height (int), fontScaleFactor (double), imageScaling (bool), makeCompact (bool). Width/height 0 or omitted = original size"_L1},

            {"list_exporters"_L1, "List all available exporter plugins"_L1},

            {"save_hints"_L1, "Persist current export hints to the PSD sidecar file"_L1},

            {"get_layer_image"_L1, "Get the rendered image of a specific layer as base64-encoded data"_L1},
            {"get_layer_image/layerId"_L1, "Layer ID to get the image from"_L1},
            {"get_layer_image/format"_L1, "Image format: png (default) or jpeg"_L1},
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

    static QString hintTypeName(QPsdExporterTreeItemModel::ExportHint::Type t)
    {
        static const char *names[] = {"embed", "merge", "custom", "native", "skip", "none"};
        return QString::fromLatin1(names[t]);
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
