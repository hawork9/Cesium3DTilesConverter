#include <OSGBConvert.h>
#include <GeometryMesh.h>

#include <osgDB/ReadFile>
#include <osg/Image>
#include <osgUtil/SmoothingVisitor>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QJsonArray>
#include <QDataStream>
#include <QByteArray>
#include <QDebug>

#include <vector>
#include <initializer_list>

namespace scially {
    void OSGBPageLodVisitor::apply(osg::Geometry& geometry) {
        geometryArray.append(&geometry);
        if (auto ss = geometry.getStateSet()) {
            osg::Texture* tex = dynamic_cast<osg::Texture*>(ss->getTextureAttribute(0, osg::StateAttribute::TEXTURE));
            if (tex != nullptr) {
                textureArray.insert(tex);
                textureMap[&geometry] = tex;
            }
        }
    }
    
    void OSGBPageLodVisitor::apply(osg::PagedLOD& node) {
        unsigned int n = node.getNumFileNames();
        for (unsigned int i = 1; i < n; i++)
        {
            QString fileName = path + "/" + QString::fromStdString(node.getFileName(i));
            subNodeNames.append(fileName);
        }
        traverse(node);
    }


    QString OSGBLevel::absoluteLocation() const {
        return QDir(nodePath).filePath(nodeName) + OSGBEXTENSION;
    }

    QString OSGBLevel::getTileName() const {
        int p0 = nodeName.indexOf("_L");
        if (p0 < 0)
            return nodeName;
        return nodeName.left(p0);
    }

    void OSGBLevel::createDir(const QString& output) const {
        for (int i = 0; i < subNodes.size(); i++) {
            QDir createDir(output + '/' + subNodes[i].getTileName());
            if (!createDir.exists()) {
                if (!createDir.mkpath(".")){
                    qWarning() << "Can't create dir: " << createDir.absolutePath();
                }

            }
        }
    }

    int OSGBLevel::getLevelNumber() const {
        int p0 = nodeName.indexOf("_L");
        if (p0 < 0)
            return 0;
        int p1 = nodeName.indexOf("_", p0 + 2);
        if (p1 < 0)
            return 0;
        return nodeName.mid(p0 + 2, p1 - p0 - 2).toInt();
    }

    bool OSGBLevel::getAllOSGBLevels(int maxLevel) {
        if (getLevelNumber() < 0 || getLevelNumber() >= maxLevel)
            return false;

        OSGBPageLodVisitor lodVisitor(nodePath);

        std::string rootOSGBLocation = absoluteLocation().toUtf8();
        osg::ref_ptr<osg::Node> root = osgDB::readNodeFile(rootOSGBLocation);

        if (root == nullptr)
            return false;

        root->accept(lodVisitor);

        for (int i = 0; i < lodVisitor.subNodeNames.size(); i++) {
            OSGBLevel subLevel;
            subLevel.setTileLocation(lodVisitor.subNodeNames[i]);
            if (subLevel.getAllOSGBLevels(maxLevel)) {
                subNodes.append(subLevel);
            }
        }
        return true;
    }

    bool OSGBLevel::convertTiles(BaseTile &tile, const QString& output, int maxLevel) {
        if (!getAllOSGBLevels(maxLevel)) {
            return false;
        }

        createDir(output);

        RootTile childTile;
        if(!convertTiles(childTile, output)){
            return false;
        }

        //update geometry error
        updateGeometryError(childTile);

        // update this region
        this->region = childTile.boundingVolume.box.value();

        // update root tile
        tile.geometricError = 2000;
        tile.asset.assets["gltfUpAxis"] = "Y";
        tile.asset.assets["version"] = "1.0";

        tile.root.children.append(childTile);
        tile.root.boundingVolume = this->region;
        tile.root.geometricError = 1000;
        QJsonDocument jsonDoc(tile.write().toObject());
        QByteArray json = jsonDoc.toJson(QJsonDocument::Indented);
        QFile tilesetFile(output + "/" + getTileName() + "/tileset.json");
        
        if (!tilesetFile.open(QIODevice::WriteOnly)) {
            qCritical() << "can't Write tileset.json in " << tilesetFile.fileName();
            return false;
        }

        tilesetFile.write(json);
        return true;
    }

    bool OSGBLevel::convertTiles(RootTile &root, const QString& output) {
        OSGBConvert convert(absoluteLocation());

        QByteArray b3dmBuffer = convert.toB3DM();
        if (b3dmBuffer.isEmpty())
            return false;
        //
        QString outputLocation = QDir(output + '/' + getTileName()).absolutePath();
        int writeBytes = convert.writeB3DM(b3dmBuffer, outputLocation);
        if (writeBytes <= 0)
            return false;
        
        ContentTile content;
        content.uri = "./" + nodeName + B3DMEXTENSION;
        content.boundingVolume = BoundingVolumeBox(convert.region);
       
        root.refine = "REPLACE";
        root.content = content;
        root.boundingVolume = BoundingVolumeBox(convert.region);

        for(int i = 0; i < subNodes.size(); i++){
            RootTile child;
            subNodes[i].convertTiles(child, output);
            root.children.append(child);
            root.boundingVolume = root.boundingVolume.box->merge(child.boundingVolume.box.value());  
        }

        return true;
    }

    void OSGBLevel::updateGeometryError(RootTile &root){
        if(root.children.isEmpty()){
            root.geometricError = 0;
            return;
        }
        else{
            for(auto& tile : root.children)
                updateGeometryError(tile);
            root.geometricError = root.children[0].boundingVolume.box->geometricError() * 2;
        }

    }


    QString OSGBConvert::absoluteLocation() const {
        return QDir(nodePath).filePath(nodeName);
    }

    bool OSGBConvert::writeB3DM(const QByteArray &buffer, const QString& outLocation) {

        if (buffer.isEmpty()) {
            qWarning() << "B3DM buffer is empty...\n";
            return false;
        }

        //
        QFile b3dmFile(outLocation + "/" + nodeName.replace(".osgb", ".b3dm"));
        if (!b3dmFile.open(QIODevice::ReadWrite)) {
            qWarning() << "Can't open file [" << b3dmFile.fileName() << "]\n";
            return false;
        }
        int writeBytes = b3dmFile.write(buffer);

        if (writeBytes <= 0) {
            qWarning() << "Can't write file [" << b3dmFile.fileName() << "]\n";
            return false;
        }
        return true;
    }

    QByteArray OSGBConvert::toB3DM() {
        QByteArray b3dmBuffer;
        QDataStream b3dmStream(&b3dmBuffer, QIODevice::WriteOnly);
        b3dmStream.setByteOrder(QDataStream::LittleEndian);

        QByteArray glbBuffer = convertGLB();

        if (glbBuffer.isEmpty())
            return QByteArray();

        Batched3DModel b3dm;
        b3dm.glbBuffer = glbBuffer;
        b3dm.batchLength = 1;
        b3dm.batchID = { 0 };
        b3dm.names = {"mesh_0"};

        return b3dm.write();
    }

    QByteArray OSGBConvert::convertGLB() {
        QByteArray glbBuffer;

        std::string rootOSGBLocation = absoluteLocation().toUtf8();
        osg::ref_ptr<osg::Node> root = osgDB::readNodeFile(rootOSGBLocation);
        if (!root.valid()) {
            qWarning() << "Read OSGB File [" << absoluteLocation() << "] Fail...\n";
            return QByteArray();
        }

        OSGBPageLodVisitor lodVisitor(nodePath);
        root->accept(lodVisitor);
        if (lodVisitor.geometryArray.empty()) {
            qWarning() << "Read OSGB File [" << absoluteLocation() << "] geometries is Empty...\n";
            return QByteArray();
        }

        osgUtil::SmoothingVisitor sv;
        root->accept(sv);

        tinygltf::TinyGLTF gltf;
        tinygltf::Model model;
        tinygltf::Buffer buffer;


        OSGBuildState osgState = {
            &buffer,
            &model,
            osg::Vec3f(-1e38, -1e38, -1e38),
            osg::Vec3f(1e38, 1e38, 1e38),
            -1,
            -1
        };

        // mesh
        model.meshes.resize(1);
        int primitiveIdx = 0;
        for (auto g : lodVisitor.geometryArray)
        {
            if (!g->getVertexArray() || g->getVertexArray()->getDataSize() == 0)
                continue;

            osgState.appendOSGGeometry(g);
            // update primitive material index
            if (lodVisitor.textureArray.size())
            {
                for (unsigned int k = 0; k < g->getNumPrimitiveSets(); k++)
                {
                    auto tex = lodVisitor.textureMap[g];
                    // if hava texture
                    if (tex != nullptr)
                    {
                        for (auto texture : lodVisitor.textureArray)
                        {
                            model.meshes[0].primitives[primitiveIdx].material++;
                            if (tex == texture)
                                break;
                        }
                    }
                    primitiveIdx++;
                }
            }
        }
        // empty geometry or empty vertex-array
        if (model.meshes[0].primitives.empty())
            return QByteArray();

        region.setMax(osgState.pointMax);
        region.setMin(osgState.pointMin);

        // image
        {
            for (auto tex : lodVisitor.textureArray)
            {
                unsigned bufferStart = buffer.size();
                std::vector<unsigned char> jpegBuffer;
                int width, height, comp;
                if (tex != nullptr) {
                    if (tex->getNumImages() > 0) {
                        osg::Image* img = tex->getImage(0);
                        if (img) {
                            width = img->s();
                            height = img->t();
                            comp = img->getPixelSizeInBits();
                            if (comp == 8) comp = 1;
                            if (comp == 24) comp = 3;
                            if (comp == 4) {
                                comp = 3;
                                internal::fill4BitImage(jpegBuffer, img, width, height);
                            }
                            else
                            {
                                unsigned rowStep = img->getRowStepInBytes();
                                unsigned rowSize = img->getRowSizeInBytes();
                                for (int i = 0; i < height; i++)
                                {
                                    jpegBuffer.insert(jpegBuffer.end(),
                                        img->data() + rowStep * i,
                                        img->data() + rowSize * i + rowSize);
                                }
                            }
                        }
                    }
                }

                const auto stbImgWriteBuffer = [](void* context, void* data, int len) {
                    auto buf = (std::vector<char>*)context;
                    buf->insert(buf->end(), (char*)data, (char*)data + len);
                };

                if (!jpegBuffer.empty()) {
                    buffer.data.reserve(buffer.size() + width * height * comp);
                    stbi_write_jpg_to_func(stbImgWriteBuffer, &buffer.data, width, height, comp, jpegBuffer.data(), 80);
                }
                else {
                    std::vector<unsigned char> vData(256 * 256 * 3);
                    stbi_write_jpg_to_func(stbImgWriteBuffer, &buffer.data, 256, 256, 3, vData.data(), 80);
                }

                tinygltf::Image image;
                image.mimeType = "image/jpeg";
                image.bufferView = model.bufferViews.size();
                model.images.push_back(image);
                tinygltf::BufferView bfv;
                bfv.buffer = 0;
                bfv.byteOffset = bufferStart;
                buffer.alignment();
                bfv.byteLength = buffer.size() - bufferStart;
                model.bufferViews.push_back(bfv);
            }
            // node
            {
                tinygltf::Node node;
                node.mesh = 0;
                // z-UpAxis to y-UpAxis
                node.matrix = {1,0,0,0,
                                0,0,-1,0,
                                0,1,0,0,
                                0,0,0,1};
                model.nodes.push_back(node);
            }
            // scene
            {
                tinygltf::Scene sence;
                sence.nodes.push_back(0);
                model.scenes = { sence };
                model.defaultScene = 0;
            }

            // sample
            {
                tinygltf::Sampler sample;
                sample.magFilter = TINYGLTF_TEXTURE_FILTER_LINEAR;
                sample.minFilter = TINYGLTF_TEXTURE_FILTER_NEAREST_MIPMAP_LINEAR;
                sample.wrapS = TINYGLTF_TEXTURE_WRAP_REPEAT;
                sample.wrapT = TINYGLTF_TEXTURE_WRAP_REPEAT;
                model.samplers = { sample };
            }

            // use pbr material
            {
                model.extensionsRequired = { "KHR_materials_unlit" };
                model.extensionsUsed = { "KHR_materials_unlit" };
                for (int i = 0; i < lodVisitor.textureArray.size(); i++)
                {
                    tinygltf::Material mat = makeColorMaterialFromRGB(1.0, 1.0, 1.0);
                    mat.b_unlit = true; // use KHR_materials_unlit
                    tinygltf::Parameter baseColorTexture;
                    baseColorTexture.json_int_value = { std::pair<std::string, int>("index",i) };
                    mat.values["baseColorTexture"] = baseColorTexture;
                    model.materials.push_back(mat);
                }
            }

            // finish buffer
            model.buffers.push_back(buffer);
            // texture
            {
                int textureIndex = 0;
                for (auto tex : lodVisitor.textureArray)
                {
                    tinygltf::Texture texture;
                    texture.source = textureIndex++;
                    texture.sampler = 0;
                    model.textures.push_back(texture);
                }
            }
            model.asset.version = "2.0";
            model.asset.generator = "hwang";

            glbBuffer = QByteArray::fromStdString(gltf.Serialize(&model));
            return glbBuffer;
        }


    }

    tinygltf::Material OSGBConvert::makeColorMaterialFromRGB(double r, double g, double b) {
        tinygltf::Material material;
        material.name = "default";
        tinygltf::Parameter baseColorFactor;
        baseColorFactor.number_array = { r, g, b, 1.0 };
        material.values["baseColorFactor"] = baseColorFactor;

        tinygltf::Parameter metallicFactor;
        metallicFactor.number_value = new double(0);
        material.values["metallicFactor"] = metallicFactor;
        tinygltf::Parameter roughnessFactor;
        roughnessFactor.number_value = new double(1);
        material.values["roughnessFactor"] = roughnessFactor;
        //
        return material;
    }
}

namespace internal {

    Color RGB565_RGB(unsigned short color0) {
        unsigned char r0 = ((color0 >> 11) & 0x1F) << 3;
        unsigned char g0 = ((color0 >> 5) & 0x3F) << 2;
        unsigned char b0 = (color0 & 0x1F) << 3;
        return Color{ r0, g0, b0 };
    }

    Color Mix_Color(
            unsigned short color0, unsigned short color1,
            Color c0, Color c1, int idx) {
        Color finalColor;
        if (color0 > color1)
        {
            switch (idx)
            {
                case 0:
                    finalColor = Color{ c0.r, c0.g, c0.b };
                    break;
                case 1:
                    finalColor = Color{ c1.r, c1.g, c1.b };
                    break;
                case 2:
                    finalColor = Color{
                            (2 * c0.r + c1.r) / 3,
                            (2 * c0.g + c1.g) / 3,
                            (2 * c0.b + c1.b) / 3 };
                    break;
                case 3:
                    finalColor = Color{
                            (c0.r + 2 * c1.r) / 3,
                            (c0.g + 2 * c1.g) / 3,
                            (c0.b + 2 * c1.b) / 3 };
                    break;
            }
        }
        else
        {
            switch (idx)
            {
                case 0:
                    finalColor = Color{ c0.r, c0.g, c0.b };
                    break;
                case 1:
                    finalColor = Color{ c1.r, c1.g, c1.b };
                    break;
                case 2:
                    finalColor = Color{ (c0.r + c1.r) / 2, (c0.g + c1.g) / 2, (c0.b + c1.b) / 2 };
                    break;
                case 3:
                    finalColor = Color{ 0, 0, 0 };
                    break;
            }
        }
        return finalColor;
    }
    void resizeImage(std::vector<unsigned char>& jpeg_buf, int width, int height, int new_w, int new_h) {
        std::vector<unsigned char> new_buf(new_w * new_h * 3);
        int scale = width / new_w;
        for (int row = 0; row < new_h; row++)
        {
            for (int col = 0; col < new_w; col++) {
                int pos = row * new_w + col;
                int old_pos = (row * width + col) * scale;
                for (int i = 0; i < 3; i++)
                {
                    new_buf[3 * pos + i] = jpeg_buf[3 * old_pos + i];
                }
            }
        }
        jpeg_buf = new_buf;
    }

    void fill4BitImage(std::vector<unsigned char>& jpeg_buf, osg::Image* img, int& width, int& height) {
        jpeg_buf.resize(width * height * 3);
        unsigned char* pData = img->data();
        int imgSize = img->getImageSizeInBytes();
        int x_pos = 0;
        int y_pos = 0;
        for (int i = 0; i < imgSize; i += 8)
        {
            // 64 bit matrix
            unsigned short color0, color1;
            std::memcpy(&color0, pData, 2);
            pData += 2;
            memcpy(&color1, pData, 2);
            pData += 2;
            Color c0 = RGB565_RGB(color0);
            Color c1 = RGB565_RGB(color1);
            for (size_t i = 0; i < 4; i++)
            {
                unsigned char idx[4];
                idx[3] = (*pData >> 6) & 0x03;
                idx[2] = (*pData >> 4) & 0x03;
                idx[1] = (*pData >> 2) & 0x03;
                idx[0] = (*pData) & 0x03;
                // 4 pixel color
                for (size_t pixel_idx = 0; pixel_idx < 4; pixel_idx++)
                {
                    Color cf = Mix_Color(color0, color1, c0, c1, idx[pixel_idx]);
                    int cell_x_pos = x_pos + pixel_idx;
                    int cell_y_pos = y_pos + i;
                    int byte_pos = (cell_x_pos + cell_y_pos * width) * 3;
                    jpeg_buf[byte_pos] = cf.r;
                    jpeg_buf[byte_pos + 1] = cf.g;
                    jpeg_buf[byte_pos + 2] = cf.b;
                }
                pData++;
            }
            x_pos += 4;
            if (x_pos >= width) {
                x_pos = 0;
                y_pos += 4;
            }
        }
        int max_size = 2048;
        if (width > max_size || height > max_size) {
            int new_w = width, new_h = height;
            while (new_w > max_size || new_h > max_size)
            {
                new_w /= 2;
                new_h /= 2;
            }
            resizeImage(jpeg_buf, width, height, new_w, new_h);
            width = new_w;
            height = new_h;
        }
    }
}
