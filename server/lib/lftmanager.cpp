/*
 * Copyright (C) 2017 ~ 2019 Deepin Technology Co., Ltd.
 *
 * Author:     zccrs <zccrs@live.com>
 *
 * Maintainer: zccrs <zhangjide@deepin.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "lftmanager.h"
#include "lftdisktool.h"

extern "C" {
#include "fs_buf.h"
#include "walkdir.h"
}

#include <dfmdiskmanager.h>
#include <dfmblockpartition.h>

#include <QtConcurrent>
#include <QFutureWatcher>
#include <QStandardPaths>
#include <QRegularExpression>

#include <unistd.h>
#include <pwd.h>

class _LFTManager : public LFTManager {};
Q_GLOBAL_STATIC(_LFTManager, _global_lftmanager)
typedef QMap<QString, fs_buf*> FSBufMap;
Q_GLOBAL_STATIC(FSBufMap, _global_fsBufMap)

static QSet<fs_buf*> fsBufList()
{
    return _global_fsBufMap->values().toSet();
}

static void clearFsBufMap()
{
    for (fs_buf *buf : fsBufList()) {
        if (buf)
            free_fs_buf(buf);
    }

    _global_fsBufMap->clear();
}

LFTManager::~LFTManager()
{
    sync();
    clearFsBufMap();
}

LFTManager *LFTManager::instance()
{
    return _global_lftmanager;
}

struct FSBufDeleter
{
    static inline void cleanup(fs_buf *pointer)
    {
        free_fs_buf(pointer);
    }
};

static fs_buf *buildFSBuf(const QString &path)
{
    fs_buf *buf = new_fs_buf(1 << 24, path.toLocal8Bit().constData());

    if (!buf)
        return buf;

    if (build_fstree(buf, false, nullptr, nullptr) != 0) {
        free_fs_buf(buf);

        return nullptr;
    }

    return buf;
}

static QString getCacheDir()
{
    const QString cachePath = QString("/var/cache/%2/%3").arg(qApp->organizationName()).arg(qApp->applicationName());

    if (getuid() == 0)
        return cachePath;

    if (QFileInfo(cachePath).isWritable())
        return cachePath;

    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
}

static QString getLFTFileByPath(const QString &path)
{
    QString lft_file_name = LFTDiskTool::pathToSerialUri(path);

    if (lft_file_name.isEmpty())
        return QString();

    lft_file_name += ".lft";

    const QString &cache_path = getCacheDir();

    if (cache_path.isEmpty())
        return QString();

    return cache_path + "/" + lft_file_name.toUtf8().toPercentEncoding(":", "/");
}

bool LFTManager::addPath(QString path)
{
    if (!path.startsWith("/"))
        return false;

    if (_global_fsBufMap->contains(path))
        return false;

    const QByteArray &serial_uri = LFTDiskTool::pathToSerialUri(path);

    if (serial_uri.isEmpty())
        return false;

    QFutureWatcher<fs_buf*> *watcher = new QFutureWatcher<fs_buf*>(this);
    // 此路径对应的设备可能被挂载到多个位置
    const QStringList &path_list = LFTDiskTool::fromSerialUri(serial_uri);

    // 将路径改为相对于第一个挂载点的路径，vfs_monitor中所有文件的改动都是以设备第一个挂载点通知的
    path = path_list.first();

    for (const QString &path : path_list)
        (*_global_fsBufMap)[path] = nullptr;

    connect(watcher, &QFutureWatcher<fs_buf*>::finished, this, [this, path_list, watcher] {
        fs_buf *buf = watcher->result();

        for (const QString &path : path_list) {
            if (buf) {
                (*_global_fsBufMap)[path] = buf;
            } else {
                _global_fsBufMap->remove(path);
            }

            Q_EMIT addPathFinished(path, buf);
        }

        watcher->deleteLater();
    });

    QFuture<fs_buf*> result = QtConcurrent::run(buildFSBuf, path.endsWith('/') ? path : path + "/");

    watcher->setFuture(result);

    return true;
}

// 返回path对应的fs_buf对象，且将path转成为相对于fs_buf root_path的路径
static fs_buf *getFsBufByPath(QString &path, fs_buf *default_value = nullptr)
{
    if (!_global_fsBufMap.exists())
        return default_value;

    if (!path.startsWith("/"))
        return default_value;

    QStorageInfo storage_info(path);

    if (!storage_info.isValid())
        return default_value;

    QString result_path = path;

    do {
        fs_buf *buf = _global_fsBufMap->value(result_path, (fs_buf*)0x01);

        if (buf != (fs_buf*)0x01) {
            path = path.mid(result_path.size());

            if (!path.isEmpty())
                path.chop(1);
            // fs_buf中的root_path以/结尾，所以此处多移除一个字符
            path = QString::fromLocal8Bit(get_root_path(buf)) + path;

            if (path.endsWith('/'))
                path.chop(1);

            return buf;
        }

        if (result_path == "/")
            return default_value;

        int last_dir_split_pos = result_path.lastIndexOf('/');

        if (last_dir_split_pos < 0)
            return default_value;

        result_path = result_path.left(last_dir_split_pos);

        if (result_path.isEmpty())
            result_path = "/";
    } while (result_path != storage_info.rootPath());

    return default_value;
}

bool LFTManager::hasLFT(QString path) const
{
    return getFsBufByPath(path);
}

bool LFTManager::lftBuinding(QString path) const
{
    // 对应fs_buf存在且为nullptr认为正在构建
    return getFsBufByPath(path, (fs_buf*)0x01);
}

QStringList LFTManager::allPath() const
{
    if (!_global_fsBufMap.exists())
        return QStringList();

    return _global_fsBufMap->keys();
}

QStringList LFTManager::hasLFTSubdirectories(QString path) const
{
    if (!path.endsWith("/"))
        path.append('/');

    QStringList list;

    for (auto i = _global_fsBufMap->constBegin(); i != _global_fsBufMap->constEnd(); ++i) {
        if ((i.key() + "/").startsWith(path))
            list << i.key();
    }

    return list;
}

// 重新从磁盘加载lft文件
QStringList LFTManager::refresh(const QByteArray &serialUriFilter)
{
    clearFsBufMap();

    const QString &cache_path = getCacheDir();
    QDirIterator dir_iterator(cache_path, {"*.lft"});
    QStringList path_list;

    while (dir_iterator.hasNext()) {
        const QString &lft_file = dir_iterator.next();

        // 根据设备过滤
        if (!serialUriFilter.isEmpty() && !dir_iterator.fileName().startsWith(serialUriFilter))
            continue;

        fs_buf *buf = nullptr;

        if (load_fs_buf(&buf, lft_file.toLocal8Bit().constData()) != 0)
            continue;

        const  QStringList pathList = LFTDiskTool::fromSerialUri(QByteArray::fromPercentEncoding(dir_iterator.fileName().toLocal8Bit()));

        for (QString path : pathList) {
            path.chop(4);// 去除 .lft 后缀
            path_list << path;
            (*_global_fsBufMap)[path] = buf;
        }
    }

    return path_list;
}

QStringList LFTManager::sync(const QString &mountPoint)
{
    QStringList path_list;

    if (!QDir::home().mkpath(getCacheDir())) {
        return path_list;
    }

    QList<fs_buf*> saved_buf_list;

    for (auto buf_begin = _global_fsBufMap->constBegin(); buf_begin != _global_fsBufMap->constEnd(); ++buf_begin) {
        fs_buf *buf = buf_begin.value();

        if (!buf)
            continue;

        const QString &path = buf_begin.key();

        // 只同步此挂载点的数据
        if (!mountPoint.isEmpty()) {
            QStorageInfo info(path);

            if (info.rootPath() != mountPoint) {
                continue;
            }
        }

        if (saved_buf_list.contains(buf)) {
            path_list << buf_begin.key();
            continue;
        }

        saved_buf_list.append(buf);

        const QString &lft_file = getLFTFileByPath(path);

        if (save_fs_buf(buf, lft_file.toLocal8Bit().constData()) == 0) {
            path_list << path;
        }
    }

    return path_list;
}

static int compareString(const char *string, void *keyword)
{
    return QString::fromLocal8Bit(string).indexOf(*static_cast<const QString*>(keyword), 0, Qt::CaseInsensitive) >= 0;
}

static int compareStringRegExp(const char *string, void *re)
{
    return static_cast<QRegularExpression*>(re)->match(QString::fromLocal8Bit(string)).hasMatch();
}

QStringList LFTManager::search(const QString &path, const QString keyword, bool useRegExp) const
{
    QString new_path = path;
    fs_buf *buf = getFsBufByPath(new_path);

    if (!buf)
        return QStringList();

    uint32_t path_offset, start_offset, end_offset;
    get_path_range(buf, new_path.toLocal8Bit().constData(), &path_offset, &start_offset, &end_offset);

    QRegularExpression re(keyword);

    re.setPatternOptions(QRegularExpression::CaseInsensitiveOption
                         | QRegularExpression::DotMatchesEverythingOption
                         | QRegularExpression::OptimizeOnFirstUsageOption);

    void *compare_param = nullptr;
    int (*compare)(const char *, void *) = nullptr;

    if (useRegExp) {
        if (!re.isValid())
            return QStringList();

        compare_param = &re;
        compare = compareStringRegExp;
    } else {
        compare_param = const_cast<QString*>(&keyword);
        compare = compareString;
    }

#define MAX_RESULT_COUNT 1000

    uint32_t name_offsets[MAX_RESULT_COUNT];
    uint32_t count = MAX_RESULT_COUNT;

    QStringList list;
    char tmp_path[PATH_MAX];
    // root_path 以/结尾，所以此处需要多忽略一个字符
    int buf_root_path_length = strlen(get_root_path(buf)) - 1;
    bool need_reset_root_path = path != new_path;

    do {
        search_files(buf, &start_offset, end_offset, compare_param, compare, name_offsets, &count);

        for (uint32_t i = 0; i < count; ++i) {
            const char *result = get_path_by_name_off(buf, name_offsets[i], tmp_path, sizeof(tmp_path));
            const QString &origin_path = QString::fromLocal8Bit(result);

            if (need_reset_root_path)
                list << path + origin_path.mid(buf_root_path_length);
            else
                list << origin_path;
        }
    } while (count == MAX_RESULT_COUNT);

    return list;
}

static bool markLFTFileToDirty(fs_buf *buf)
{
    const char *root = get_root_path(buf);

    const QString &lft_file = getLFTFileByPath(QString::fromLocal8Bit(root));

    return QFile::remove(lft_file);
}

void LFTManager::insertFileToLFTBuf(QString file)
{
    fs_buf *buf = getFsBufByPath(file);

    if (!buf)
        return;

    QFileInfo info(file);

    fs_change change;
    insert_path(buf, file.toLocal8Bit().constData(), info.isDir(), &change);

    // buf内容已改动，删除对应的lft文件
    markLFTFileToDirty(buf);
}

void LFTManager::removeFileFromLFTBuf(QString file)
{
    fs_buf *buf = getFsBufByPath(file);

    if (!buf)
        return;

    fs_change change;
    uint32_t count;
    remove_path(buf, file.toLocal8Bit().constData(), &change, &count);

    // buf内容已改动，删除对应的lft文件
    markLFTFileToDirty(buf);
}

void LFTManager::renameFileOfLFTBuf(QString oldFile, const QString &newFile)
{
    // 此处期望oldFile是fs_buf的子文件（未处理同一设备不同挂载点的问题）
    fs_buf *buf = getFsBufByPath(oldFile);

    if (!buf)
        return;

    fs_change change;
    uint32_t change_count;
    rename_path(buf, oldFile.toLocal8Bit().constData(), newFile.toLocal8Bit().constData(), &change, &change_count);

    // buf内容已改动，删除对应的lft文件
    markLFTFileToDirty(buf);
}

static void cleanLFTManager()
{
    LFTManager::instance()->sync();
    clearFsBufMap();
}

LFTManager::LFTManager(QObject *parent)
    : QObject(parent)
{
    qAddPostRoutine(cleanLFTManager);
    refresh();

    connect(LFTDiskTool::diskManager(), &DFMDiskManager::mountAdded,
            this, &LFTManager::onMountAdded);
    connect(LFTDiskTool::diskManager(), &DFMDiskManager::mountRemoved,
            this, &LFTManager::onMountRemoved);
}

void LFTManager::onMountAdded(const QString &blockDevicePath, const QByteArray &mountPoint)
{
    Q_UNUSED(blockDevicePath)

    const QString &mount_root = QString::fromLocal8Bit(mountPoint);
    const QByteArray &serial_uri = LFTDiskTool::pathToSerialUri(mount_root);

    const QStringList &list = refresh(serial_uri.toPercentEncoding(":", "/"));

    if (list.contains(mount_root))
        return;

    auto pw = getpwuid(geteuid());

    if (!pw)
        return;

    if (!mount_root.startsWith(QString("/media/%1/").arg(pw->pw_name))) {
        return;

    }

    // 自动为挂载到 /media/$USER 目录下的目录生成索引
    addPath(mountPoint);
}

void LFTManager::onMountRemoved(const QString &blockDevicePath, const QByteArray &mountPoint)
{
    Q_UNUSED(blockDevicePath)

    const QString &mount_root = QString::fromLocal8Bit(mountPoint);
//    const QByteArray &serial_uri = LFTDiskTool::pathToSerialUri(mount_root);

    sync(mount_root);
}