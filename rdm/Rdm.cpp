#include "Rdm.h"
#include "CursorInfo.h"
#include "MemoryMonitor.h"

namespace Rdm {
QByteArray eatString(CXString str)
{
    const QByteArray ret(clang_getCString(str));
    clang_disposeString(str);
    return ret;
}

QByteArray cursorToString(CXCursor cursor)
{
    QByteArray ret = eatString(clang_getCursorKindSpelling(clang_getCursorKind(cursor)));
    const QByteArray name = eatString(clang_getCursorSpelling(cursor));
    if (!name.isEmpty())
        ret += " " + name;

    CXFile file;
    unsigned off;
    CXSourceLocation loc = clang_getCursorLocation(cursor);
    clang_getSpellingLocation(loc, &file, 0, 0, &off);
    const QByteArray fileName = eatString(clang_getFileName(file));
    if (!fileName.isEmpty()) {
        ret += " " + fileName + ',' + QByteArray::number(off);
    }
    return ret;
}

static QList<Path> sSystemPaths;
void initSystemPaths(const QList<Path> &paths)
{
    sSystemPaths = paths;
    qSort(sSystemPaths);
}

bool isSystem(const Path &path)
{
    if (!strncmp("/usr/", path.constData(), 5)) {
#ifdef Q_OS_BSD4
        if (!strncmp("home/", path.constData() + 5, 5))
            return false;
#endif
        return true;
    }
    return startsWith(sSystemPaths, path);
}

CursorInfo findCursorInfo(Database *db, const Location &location, Location *loc)
{
    const leveldb::ReadOptions readopts;
    RTags::Ptr<Iterator> it(db->createIterator());
    char needleBuf[8];
    location.toKey(needleBuf);
    const Slice needle(needleBuf, 8);
    it->seek(needle);
    bool found = false;
    CursorInfo cursorInfo;
    if (it->isValid()) {
        const Slice key = it->key();
        found = (key == needle);
        if (!found)
            it->previous();
    } else {
        it->seekToLast();
    }
    if (!found && it->isValid()) {
        const Slice key = it->key();
        debug() << "key" << key << "needle" << needle;
        const Location loc = Location::fromKey(key.data());
        if (location.fileId() == loc.fileId()) {
            const int off = location.offset() - loc.offset();
            cursorInfo = it->value<CursorInfo>();
            if (cursorInfo.symbolLength > off) {
                found = true;
            } else {
                debug("offsets wrong symbolLength %d offset %d %d/%d", cursorInfo.symbolLength,
                      off, location.offset(), loc.offset());
            }
        } else {
            debug() << "wrong path" << location.path() << loc.path() << key;
        }
    }
    if (found) {
        if (!cursorInfo.symbolLength) {
            cursorInfo = it->value<CursorInfo>();
        }
        if (loc) {
            *loc = Location::fromKey(it->key().data());
        }
    }
    if (!found) {
        // printf("[%s] %s:%d: if (!found) {\n", __func__, __FILE__, __LINE__);
        cursorInfo.clear();
    }
    // error() << "found" << found << location << cursorInfo.target << cursorInfo.references << cursorInfo.symbolLength
    //         << cursorInfo.symbolName;
    return cursorInfo;
}

static quint64 sMaxMemoryUsage = 0;
bool waitForMemory(int maxMs)
{
    QElapsedTimer timer;
    timer.start();
    static quint64 last = 0;
    do {
        QElapsedTimer timer;
        timer.start();
        const quint64 mem = MemoryMonitor::usage();
        int elapsed = timer.elapsed();
        static int total = 0;
        total += elapsed;
        printf("We're at %lld, max is %lld (was at %lld) %d %d\n", mem, sMaxMemoryUsage, last, total, elapsed);
        if (mem < sMaxMemoryUsage) {
            return true;
        } else if (mem < last || !last) {
            sleep(1);
        } else {
            sleep(2); // yes!
        }
        last = mem;
    } while (maxMs <= 0 || timer.elapsed() >= maxMs);
    return false;
}

void setMaxMemoryUsage(quint64 max)
{
    sMaxMemoryUsage = max;
}

int writeSymbolNames(SymbolNameHash &symbolNames)
{
    QElapsedTimer timer;
    timer.start();
    Database *db = Server::instance()->db(Server::SymbolName);

    Batch batch(db);
    int totalWritten = 0;

    SymbolNameHash::iterator it = symbolNames.begin();
    const SymbolNameHash::const_iterator end = symbolNames.end();
    while (it != end) {
        const char *key = it.key().constData();
        const QSet<Location> added = it.value();
        bool ok;
        QSet<Location> current = db->value<QSet<Location> >(key, &ok);
        if (!ok) {
            totalWritten += batch.add(key, added);
        } else if (addTo(current, added)) {
            totalWritten += batch.add(key, current);
        }
        ++it;
    }

    if (totalWritten && testLog(Warning)) {
        warning() << "Wrote" << symbolNames.size() << "symbolNames "
                  << totalWritten << "bytes in"
                  << timer.elapsed() << "ms";
    }
    return totalWritten;
}

int writeDependencies(const DependencyHash &dependencies)
{
    QElapsedTimer timer;
    timer.start();
    Database *db = Server::instance()->db(Server::Dependency);

    Batch batch(db);
    int totalWritten = 0;
    DependencyHash::const_iterator it = dependencies.begin();
    const DependencyHash::const_iterator end = dependencies.end();
    char buf[4];
    const Slice key(buf, 4);
    while (it != end) {
        memcpy(buf, &it.key(), sizeof(buf));
        QSet<quint32> added = it.value();
        QSet<quint32> current = db->value<QSet<quint32> >(key);
        const int oldSize = current.size();
        if (current.unite(added).size() > oldSize) {
            totalWritten += batch.add(key, current);
        }
        ++it;
    }
    if (totalWritten && testLog(Warning)) {
        warning() << "Wrote" << dependencies.size()
                  << "dependencies," << totalWritten << "bytes in"
                  << timer.elapsed() << "ms";
    }
    return totalWritten;
}
int writePchDepencies(const QHash<Path, QSet<quint32> > &pchDependencies)
{
    QElapsedTimer timer;
    timer.start();
    Database *db = Server::instance()->db(Server::General);
    if (!pchDependencies.isEmpty())
        return db->setValue("pchDependencies", pchDependencies);
    return 0;
}
int writeFileInformation(quint32 fileId, const QList<QByteArray> &args, time_t lastTouched)
{
    QElapsedTimer timer;
    timer.start();
    Database *db = Server::instance()->db(Server::FileInformation);
    char buf[4];
    memcpy(buf, &fileId, 4);
    return db->setValue(Slice(buf, 4), FileInformation(lastTouched, args));
}

int writePchUSRHashes(const QHash<Path, PchUSRHash> &pchUSRHashes)
{
    QElapsedTimer timer;
    timer.start();
    Database *db = Server::instance()->db(Server::PCHUsrHashes);
    int totalWritten = 0;
    Batch batch(db);
    for (QHash<Path, PchUSRHash>::const_iterator it = pchUSRHashes.begin(); it != pchUSRHashes.end(); ++it) {
        totalWritten += batch.add(it.key(), it.value());
    }
    if (testLog(Warning)) {
        warning() << "Wrote" << pchUSRHashes.size() << "pch infos,"
                  << totalWritten << "bytes in"
                  << timer.elapsed() << "ms";
    }
    return totalWritten;
}

int writeSymbols(SymbolHash &symbols, const ReferenceHash &references)
{
    QElapsedTimer timer;
    timer.start();
    Database *db = Server::instance()->db(Server::Symbol);
    Batch batch(db);
    int totalWritten = 0;

    if (!references.isEmpty()) {
        const ReferenceHash::const_iterator end = references.end();
        for (ReferenceHash::const_iterator it = references.begin(); it != end; ++it) {
            const SymbolHash::iterator sym = symbols.find(it.value().first);
            if (sym != symbols.end()) {
                CursorInfo &ci = sym.value();
                ci.references.insert(it.key());
                // if (it.value().first.path.contains("RTags.h"))
                //     error() << "cramming" << it.key() << "into" << it.value();
                if (it.value().second != Rdm::NormalReference) {
                    CursorInfo &other = symbols[it.key()];
                    ci.references += other.references;
                    other.references += ci.references;
                    if (other.target.isNull())
                        other.target = it.value().first;
                    if (ci.target.isNull())
                        ci.target = it.key();
                }
            } else {
                char buf[8];
                it.value().first.toKey(buf);
                const Slice key(buf, 8);
                CursorInfo current = db->value<CursorInfo>(key);
                bool changedCurrent = false;
                if (addTo(current.references, it.key()))
                    changedCurrent = true;
                if (it.value().second != Rdm::NormalReference) {
                    char otherBuf[8];
                    it.key().toKey(otherBuf);
                    const Slice otherKey(otherBuf, 8);
                    CursorInfo other = db->value<CursorInfo>(otherKey);
                    bool changedOther = false;
                    if (addTo(other.references, it.key()))
                        changedOther = true;
                    if (addTo(other.references, current.references))
                        changedOther = true;
                    if (addTo(current.references, other.references))
                        changedCurrent = true;

                    if (other.target.isNull()) {
                        other.target = it.value().first;
                        changedOther = true;
                    }

                    if (current.target.isNull()) {
                        current.target = it.key();
                        changedCurrent = true;
                    }

                    if (changedOther) {
                        totalWritten += batch.add(otherKey, other);
                    }
                    // error() << "ditched reference" << it.key() << it.value();
                }
                if (changedCurrent) {
                    totalWritten += batch.add(key, current);
                }
            }
        }
    }
    if (!symbols.isEmpty()) {
        SymbolHash::iterator it = symbols.begin();
        const SymbolHash::const_iterator end = symbols.end();
        while (it != end) {
            char buf[8];
            it.key().toKey(buf);
            const Slice key(buf, 8);
            CursorInfo added = it.value();
            bool ok;
            CursorInfo current = db->value<CursorInfo>(key, &ok);
            if (!ok) {
                totalWritten += batch.add(key, added);
            } else if (current.unite(added)) {
                totalWritten += batch.add(key, current);
            }
            ++it;
        }
    }
    if (totalWritten && testLog(Warning)) {
        warning() << "Wrote" << symbols.size()
                  << "symbols and" << references.size()
                  << "references" << totalWritten << "bytes in"
                  << timer.elapsed() << "ms";
    }
    return totalWritten;
}
}
