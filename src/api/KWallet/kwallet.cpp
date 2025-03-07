/*
    This file is part of the KDE project

    SPDX-FileCopyrightText: 2002-2004 George Staikos <staikos@kde.org>
    SPDX-FileCopyrightText: 2008 Michael Leupold <lemma@confuego.org>
    SPDX-FileCopyrightText: 2011 Valentin Rusu <kde@rusu.info>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "kwallet.h"
#include "kwallet_api_debug.h"
#include <config-kwallet.h>

#include <QApplication>
#include <QDBusConnection>
#include <QRegularExpression>

#include <KConfigGroup>
#include <KSharedConfig>
#include <kwindowsystem.h>

#if HAVE_KSECRETSSERVICE
#include "ksecretsservice/ksecretsservicecollection.h"
#endif

#include "kwallet_interface.h"

#if HAVE_KSECRETSSERVICE
typedef QMap<QString, KSecretsService::StringStringMap> StringToStringStringMapMap;
Q_DECLARE_METATYPE(StringToStringStringMapMap)
#endif
typedef QMap<QString, QByteArray> StringByteArrayMap;
Q_DECLARE_METATYPE(StringByteArrayMap)

namespace KWallet
{
class KWalletDLauncher
{
public:
    KWalletDLauncher();
    ~KWalletDLauncher();
    KWalletDLauncher(const KWalletDLauncher &) = delete;
    KWalletDLauncher &operator=(const KWalletDLauncher &) = delete;
    org::kde::KWallet &getInterface();

    // this static variable is used below to switch between old KWallet
    // infrastructure and the new one which is built on top of the new
    // KSecretsService infrastructure. It's value can be changed via the
    // the Wallet configuration module in System Settings
    bool m_useKSecretsService;
    org::kde::KWallet *m_wallet_deamon;
    KConfigGroup m_cgroup;
    bool m_walletEnabled;
};

Q_GLOBAL_STATIC(KWalletDLauncher, walletLauncher)

static QString appid()
{
    return qApp->applicationName();
}

static void registerTypes()
{
    static bool registered = false;
    if (!registered) {
#if HAVE_KSECRETSSERVICE
        qDBusRegisterMetaType<KSecretsService::StringStringMap>();
        qDBusRegisterMetaType<StringToStringStringMapMap>();
#endif
        qDBusRegisterMetaType<StringByteArrayMap>();
        registered = true;
    }
}

bool Wallet::isUsingKSecretsService()
{
    return walletLauncher()->m_useKSecretsService;
}

const QString Wallet::LocalWallet()
{
    // NOTE: This method stays unchanged for KSecretsService
    KConfigGroup cfg(KSharedConfig::openConfig(QStringLiteral("kwalletrc"))->group("Wallet"));
    if (!cfg.readEntry("Use One Wallet", true)) {
        QString tmp = cfg.readEntry("Local Wallet", "localwallet");
        if (tmp.isEmpty()) {
            return QStringLiteral("localwallet");
        }
        return tmp;
    }

    QString tmp = cfg.readEntry("Default Wallet", "kdewallet");
    if (tmp.isEmpty()) {
        return QStringLiteral("kdewallet");
    }
    return tmp;
}

const QString Wallet::NetworkWallet()
{
    // NOTE: This method stays unchanged for KSecretsService
    KConfigGroup cfg(KSharedConfig::openConfig(QStringLiteral("kwalletrc"))->group("Wallet"));

    QString tmp = cfg.readEntry("Default Wallet", "kdewallet");
    if (tmp.isEmpty()) {
        return QStringLiteral("kdewallet");
    }
    return tmp;
}

const QString Wallet::PasswordFolder()
{
    return QStringLiteral("Passwords");
}

const QString Wallet::FormDataFolder()
{
    return QStringLiteral("Form Data");
}

class Q_DECL_HIDDEN Wallet::WalletPrivate
{
public:
    WalletPrivate(Wallet *wallet, int h, const QString &n)
        : q(wallet)
        , name(n)
        , handle(h)
#if HAVE_KSECRETSSERVICE
        , secretsCollection(0)
#endif
    {
    }

    void walletServiceUnregistered();

#if HAVE_KSECRETSSERVICE
    template<typename T>
    int writeEntry(const QString &key, const T &value, Wallet::EntryType entryType)
    {
        int rc = -1;
        KSecretsService::Secret secret;
        secret.setValue(QVariant::fromValue<T>(value));

        KSecretsService::StringStringMap attrs;
        attrs[KSS_ATTR_ENTRYFOLDER] = folder;
        attrs[KSS_ATTR_WALLETTYPE] = QString("%1").arg((int)entryType);
        KSecretsService::CreateCollectionItemJob *createItemJob = secretsCollection->createItem(key, attrs, secret);

        if (!createItemJob->exec()) {
            qCDebug(KWALLET_API_LOG) << "Cannot execute CreateCollectionItemJob : " << createItemJob->errorString();
        }
        rc = createItemJob->error();
        return rc;
    }

    QExplicitlySharedDataPointer<KSecretsService::SecretItem> findItem(const QString &key) const;
    template<typename T>
    int readEntry(const QString &key, T &value) const;
    bool readSecret(const QString &key, KSecretsService::Secret &value) const;

#if KWALLET_BUILD_DEPRECATED_SINCE(5, 72)
    template<typename V>
    int forEachItemThatMatches(const QString &key, V verb)
    {
        int rc = -1;
        KSecretsService::StringStringMap attrs;
        attrs[KSS_ATTR_ENTRYFOLDER] = folder;
        KSecretsService::SearchCollectionItemsJob *searchItemsJob = secretsCollection->searchItems(attrs);
        // TODO: Make this, and similar jobs, async
        if (searchItemsJob->exec()) {
            // HACK: QRegularExpression::wildcardToRegularExpression() mainly handles file pattern
            // globbing (e.g. "*.txt") which means it doesn't allow "/" in the file name (which is
            // technically correct); we have to subvert it because the keys in kwallet are in the
            // form e.g. "foo.com/<User name>" which does have a "/" in it
            const QString pattern = QRegularExpression::wildcardToRegularExpression(key).replace(QLatin1String("[^/]"), QLatin1String("."));
            const QRegularExpression re(pattern);
            const auto list = searchItemsJob->items();
            for (KSecretsService::SearchCollectionItemsJob::Item item : list) {
                KSecretsService::ReadItemPropertyJob *readLabelJob = item->label();
                if (readLabelJob->exec()) {
                    QString label = readLabelJob->propertyValue().toString();
                    if (re.match(label).hasMatch()) {
                        if (verb(this, label, item.data())) {
                            rc = 0; // one successful iteration already produced results, so success return
                        }
                    }
                } else {
                    qCDebug(KWALLET_API_LOG) << "Cannot execute ReadItemPropertyJob " << readLabelJob->errorString();
                }
            }
        } else {
            qCDebug(KWALLET_API_LOG) << "Cannot execute KSecretsService::SearchCollectionItemsJob " << searchItemsJob->errorString();
        }
        return rc;
    }
#endif

    template<typename V>
    int checkItems(V verb)
    {
        int rc = -1;
        KSecretsService::StringStringMap attrs;
        attrs[KSS_ATTR_ENTRYFOLDER] = folder;
        KSecretsService::SearchCollectionItemsJob *searchItemsJob = secretsCollection->searchItems(attrs);
        if (searchItemsJob->exec()) {
            const auto list = searchItemsJob->items();
            for (KSecretsService::SearchCollectionItemsJob::Item item : list) {
                KSecretsService::ReadItemPropertyJob *readLabelJob = item->label();
                if (readLabelJob->exec()) {
                    const QString label = readLabelJob->propertyValue().toString();
                    if (verb(this, label, item.data())) {
                        rc = 0; // one successful iteration already produced results, so success return
                    }
                } else {
                    qCDebug(KWALLET_API_LOG) << "Cannot execute ReadItemPropertyJob " << readLabelJob->errorString();
                }
            }
        } else {
            qCDebug(KWALLET_API_LOG) << "Cannot execute KSecretsService::SearchCollectionItemsJob " << searchItemsJob->errorString();
        }
        return rc;
    }

    void createDefaultFolders();

    struct InsertIntoEntryList;
    struct InsertIntoMapList;
    struct InsertIntoPasswordList;

    KSecretsService::Collection *secretsCollection;
#endif // HAVE_KSECRETSSERVICE

    Wallet *q;
    QString name;
    QString folder;
    int handle;
    int transactionId;
};

#if HAVE_KSECRETSSERVICE
void Wallet::WalletPrivate::createDefaultFolders()
{
    // NOTE: KWalletManager expects newly created wallets to have two default folders
    //     b->createFolder(KWallet::Wallet::PasswordFolder());
    //     b->createFolder(KWallet::Wallet::FormDataFolder());
    QString strDummy("");
    folder = PasswordFolder();
    writeEntry(PasswordFolder(), strDummy, KWallet::Wallet::Unknown);

    folder = FormDataFolder();
    writeEntry(FormDataFolder(), strDummy, KWallet::Wallet::Unknown);
}
#endif // HAVE_KSECRETSSERVICE

static const char s_kwalletdServiceName[] = "org.kde.kwalletd5";

Wallet::Wallet(int handle, const QString &name)
    : QObject(nullptr)
    , d(new WalletPrivate(this, handle, name))
{
    if (walletLauncher()->m_useKSecretsService) {
        // see openWallet for initialization code; this constructor does not have any code
    } else {
        QDBusServiceWatcher *watcher = new QDBusServiceWatcher(QString::fromLatin1(s_kwalletdServiceName),
                                                               QDBusConnection::sessionBus(),
                                                               QDBusServiceWatcher::WatchForUnregistration,
                                                               this);
        connect(watcher, SIGNAL(serviceUnregistered(QString)), this, SLOT(walletServiceUnregistered()));
        // clang-format off
        connect(&walletLauncher()->getInterface(), &OrgKdeKWalletInterface::walletClosedId, this, &KWallet::Wallet::slotWalletClosed);
        connect(&walletLauncher()->getInterface(), SIGNAL(folderListUpdated(QString)), SLOT(slotFolderListUpdated(QString)));
        connect(&walletLauncher()->getInterface(), SIGNAL(folderUpdated(QString,QString)), SLOT(slotFolderUpdated(QString,QString)));
        connect(&walletLauncher()->getInterface(), SIGNAL(applicationDisconnected(QString,QString)), SLOT(slotApplicationDisconnected(QString,QString)));
        // clang-format on

        // Verify that the wallet is still open
        if (d->handle != -1) {
            QDBusReply<bool> r = walletLauncher()->getInterface().isOpen(d->handle);
            if (r.isValid() && !r) {
                d->handle = -1;
                d->name.clear();
            }
        }
    }
}

Wallet::~Wallet()
{
#if HAVE_KSECRETSSERVICE
    if (walletLauncher()->m_useKSecretsService) {
        d->folder.clear();
        d->name.clear();
        delete d->secretsCollection;
    } else {
#endif
        if (d->handle != -1) {
            if (!walletLauncher.isDestroyed()) {
                walletLauncher()->getInterface().close(d->handle, false, appid());
            } else {
                qCDebug(KWALLET_API_LOG) << "Problem with static destruction sequence."
                                            "Destroy any static Wallet before the event-loop exits.";
            }
            d->handle = -1;
            d->folder.clear();
            d->name.clear();
        }
#if HAVE_KSECRETSSERVICE
    }
#endif
    delete d;
}

QStringList Wallet::walletList()
{
    QStringList result;
#if HAVE_KSECRETSSERVICE
    if (walletLauncher()->m_useKSecretsService) {
        KSecretsService::ListCollectionsJob *listJob = KSecretsService::Collection::listCollections();
        if (listJob->exec()) {
            result = listJob->collections();
        } else {
            qCDebug(KWALLET_API_LOG) << "Cannot execute ListCollectionsJob: " << listJob->errorString();
        }
    } else {
#endif
        if (walletLauncher()->m_walletEnabled) {
            QDBusReply<QStringList> r = walletLauncher()->getInterface().wallets();

            if (!r.isValid()) {
                qCDebug(KWALLET_API_LOG) << "Invalid DBus reply: " << r.error();
            } else {
                result = r;
            }
        }
#if HAVE_KSECRETSSERVICE
    }
#endif
    return result;
}

void Wallet::changePassword(const QString &name, WId w)
{
    if (w == 0) {
        qCDebug(KWALLET_API_LOG) << "Pass a valid window to KWallet::Wallet::changePassword().";
    }

    // Make sure the password prompt window will be visible and activated
    KWindowSystem::allowExternalProcessWindowActivation();
#if HAVE_KSECRETSSERVICE
    if (walletLauncher()->m_useKSecretsService) {
        KSecretsService::Collection *coll = KSecretsService::Collection::findCollection(name);
        KSecretsService::ChangeCollectionPasswordJob *changePwdJob = coll->changePassword();
        if (!changePwdJob->exec()) {
            qCDebug(KWALLET_API_LOG) << "Cannot execute change password job: " << changePwdJob->errorString();
        }
        coll->deleteLater();
    } else {
#endif
        if (walletLauncher()->m_walletEnabled) {
            walletLauncher()->getInterface().changePassword(name, (qlonglong)w, appid());
        }
#if HAVE_KSECRETSSERVICE
    }
#endif
}

bool Wallet::isEnabled()
{
#if HAVE_KSECRETSSERVICE
    if (walletLauncher()->m_useKSecretsService) {
        return walletLauncher()->m_cgroup.readEntry("Enabled", true);
    } else {
#endif
        return walletLauncher()->m_walletEnabled;
#if HAVE_KSECRETSSERVICE
    }
#endif
}

bool Wallet::isOpen(const QString &name)
{
#if HAVE_KSECRETSSERVICE
    if (walletLauncher()->m_useKSecretsService) {
        KSecretsService::Collection *coll = KSecretsService::Collection::findCollection(name, KSecretsService::Collection::OpenOnly);
        KSecretsService::ReadCollectionPropertyJob *readLocked = coll->isLocked();
        if (readLocked->exec()) {
            return !readLocked->propertyValue().toBool();
        } else {
            qCDebug(KWALLET_API_LOG) << "ReadLocked job failed";
            return false;
        }
    } else {
#endif
        if (walletLauncher()->m_walletEnabled) {
            QDBusReply<bool> r = walletLauncher()->getInterface().isOpen(name);

            if (!r.isValid()) {
                qCDebug(KWALLET_API_LOG) << "Invalid DBus reply: " << r.error();
                return false;
            } else {
                return r;
            }
        } else
            return false;
#if HAVE_KSECRETSSERVICE
    }
#endif
}

int Wallet::closeWallet(const QString &name, bool force)
{
#if HAVE_KSECRETSSERVICE
    if (walletLauncher()->m_useKSecretsService) {
        qCDebug(KWALLET_API_LOG) << "Wallet::closeWallet NOOP";
        return 0;
    } else {
#endif
        if (walletLauncher()->m_walletEnabled) {
            QDBusReply<int> r = walletLauncher()->getInterface().close(name, force);

            if (!r.isValid()) {
                qCDebug(KWALLET_API_LOG) << "Invalid DBus reply: " << r.error();
                return -1;
            } else {
                return r;
            }
        } else
            return -1;
#if HAVE_KSECRETSSERVICE
    }
#endif
}

int Wallet::deleteWallet(const QString &name)
{
#if HAVE_KSECRETSSERVICE
    if (walletLauncher()->m_useKSecretsService) {
        KSecretsService::Collection *coll = KSecretsService::Collection::findCollection(name, KSecretsService::Collection::OpenOnly);
        KJob *deleteJob = coll->deleteCollection();
        if (!deleteJob->exec()) {
            qCDebug(KWALLET_API_LOG) << "Cannot execute delete job " << deleteJob->errorString();
        }
        return deleteJob->error();
    } else {
#endif
        if (walletLauncher->m_walletEnabled) {
            QDBusReply<int> r = walletLauncher()->getInterface().deleteWallet(name);

            if (!r.isValid()) {
                qCDebug(KWALLET_API_LOG) << "Invalid DBus reply: " << r.error();
                return -1;
            } else {
                return r;
            }
        } else
            return -1;
#if HAVE_KSECRETSSERVICE
    }
#endif
}

Wallet *Wallet::openWallet(const QString &name, WId w, OpenType ot)
{
    if (w == 0) {
        qCDebug(KWALLET_API_LOG) << "Pass a valid window to KWallet::Wallet::openWallet().";
    }

    if (!walletLauncher()->m_walletEnabled) {
        qCDebug(KWALLET_API_LOG) << "User disabled the wallet system so returning 0 here.";
        return nullptr;
    }

#if HAVE_KSECRETSSERVICE
    if (walletLauncher()->m_useKSecretsService) {
        Wallet *wallet = new Wallet(-1, name);
        // FIXME: should we specify CreateCollection or OpenOnly here?
        wallet->d->secretsCollection = KSecretsService::Collection::findCollection(name, KSecretsService::Collection::CreateCollection, QVariantMap(), w);
        connect(wallet->d->secretsCollection, SIGNAL(statusChanged(int)), wallet, SLOT(slotCollectionStatusChanged(int)));
        connect(wallet->d->secretsCollection, SIGNAL(deleted()), wallet, SLOT(slotCollectionDeleted()));
        if (ot == Synchronous) {
            qCDebug(KWALLET_API_LOG) << "WARNING openWallet OpenType=Synchronous requested";
            // TODO: not sure what to do with in this case; however, all other KSecretsService API methods are already
            // async and will perform sync inside this API because of it's design
        }
        return wallet;
    } else {
#endif
        Wallet *wallet = new Wallet(-1, name);

        // connect the daemon's opened signal to the slot filtering the
        // signals we need
        connect(&walletLauncher()->getInterface(), SIGNAL(walletAsyncOpened(int, int)), wallet, SLOT(walletAsyncOpened(int, int)));

        // Make sure the password prompt window will be visible and activated
        KWindowSystem::allowExternalProcessWindowActivation();

        org::kde::KWallet &interface = walletLauncher->getInterface();

        // do the call
        QDBusReply<int> r;
        if (ot == Synchronous) {
            interface.setTimeout(0x7FFFFFFF); // Don't timeout after 25s, but 24 days
            r = interface.open(name, (qlonglong)w, appid());
            interface.setTimeout(-1); // Back to the default 25s
            // after this call, r would contain a transaction id >0 if OK or -1 if NOK
            // if OK, the slot walletAsyncOpened should have been received, but the transaction id
            // will not match. We'll get that handle from the reply - see below
        } else if (ot == Asynchronous) {
            r = interface.openAsync(name, (qlonglong)w, appid(), true);
        } else if (ot == Path) {
            r = interface.openPathAsync(name, (qlonglong)w, appid(), true);
        } else {
            delete wallet;
            return nullptr;
        }
        // error communicating with the daemon (maybe not running)
        if (!r.isValid()) {
            qCDebug(KWALLET_API_LOG) << "Invalid DBus reply: " << r.error();
            delete wallet;
            return nullptr;
        }
        wallet->d->transactionId = r.value();

        if (ot == Synchronous || ot == Path) {
            // check for an immediate error
            if (wallet->d->transactionId < 0) {
                delete wallet;
                wallet = nullptr;
            } else {
                wallet->d->handle = r.value();
            }
        } else if (ot == Asynchronous) {
            if (wallet->d->transactionId < 0) {
                QTimer::singleShot(0, wallet, SLOT(emitWalletAsyncOpenError()));
                // client code is responsible for deleting the wallet
            }
        }

        return wallet;
#if HAVE_KSECRETSSERVICE
    }
#endif
}

void Wallet::slotCollectionStatusChanged(int status)
{
#if HAVE_KSECRETSSERVICE
    KSecretsService::Collection::Status collStatus = (KSecretsService::Collection::Status)status;
    switch (collStatus) {
    case KSecretsService::Collection::NewlyCreated:
        d->createDefaultFolders();
    // fall through
    case KSecretsService::Collection::FoundExisting:
        emitWalletOpened();
        break;
    case KSecretsService::Collection::Deleted:
    case KSecretsService::Collection::Invalid:
    case KSecretsService::Collection::Pending:
        // nothing to do
        break;
    case KSecretsService::Collection::NotFound:
        emitWalletAsyncOpenError();
        break;
    }
#else
    Q_UNUSED(status)
#endif
}

void Wallet::slotCollectionDeleted()
{
    d->folder.clear();
    d->name.clear();
    Q_EMIT walletClosed();
}

bool Wallet::disconnectApplication(const QString &wallet, const QString &app)
{
#if HAVE_KSECRETSSERVICE
    if (walletLauncher()->m_useKSecretsService) {
        qCDebug(KWALLET_API_LOG) << "Wallet::disconnectApplication NOOP";
        return true;
    } else {
#endif
        if (walletLauncher()->m_walletEnabled) {
            QDBusReply<bool> r = walletLauncher()->getInterface().disconnectApplication(wallet, app);

            if (!r.isValid()) {
                qCDebug(KWALLET_API_LOG) << "Invalid DBus reply: " << r.error();
                return false;
            } else {
                return r;
            }
        } else
            return -1;
#if HAVE_KSECRETSSERVICE
    }
#endif
}

QStringList Wallet::users(const QString &name)
{
#if HAVE_KSECRETSSERVICE
    if (walletLauncher()->m_useKSecretsService) {
        qCDebug(KWALLET_API_LOG) << "KSecretsService does not handle users list";
        return QStringList();
    } else {
#endif
        if (walletLauncher()->m_walletEnabled) {
            QDBusReply<QStringList> r = walletLauncher()->getInterface().users(name);
            if (!r.isValid()) {
                qCDebug(KWALLET_API_LOG) << "Invalid DBus reply: " << r.error();
                return QStringList();
            } else {
                return r;
            }
        } else
            return QStringList();
#if HAVE_KSECRETSSERVICE
    }
#endif
}

int Wallet::sync()
{
#if HAVE_KSECRETSSERVICE
    if (walletLauncher()->m_useKSecretsService) {
        // NOOP with KSecretsService
    } else {
#endif
        if (d->handle == -1) {
            return -1;
        }

        walletLauncher()->getInterface().sync(d->handle, appid());
#if HAVE_KSECRETSSERVICE
    }
#endif
    return 0;
}

int Wallet::lockWallet()
{
#if HAVE_KSECRETSSERVICE
    if (walletLauncher()->m_useKSecretsService) {
        KSecretsService::CollectionLockJob *lockJob = d->secretsCollection->lock();
        if (lockJob->exec()) {
            d->folder.clear();
            d->name.clear();
        } else {
            qCDebug(KWALLET_API_LOG) << "Cannot execute KSecretsService::CollectionLockJob : " << lockJob->errorString();
            return -1;
        }
        return lockJob->error();
    } else {
#endif
        if (d->handle == -1) {
            return -1;
        }

        QDBusReply<int> r = walletLauncher()->getInterface().close(d->handle, true, appid());
        d->handle = -1;
        d->folder.clear();
        d->name.clear();
        if (r.isValid()) {
            return r;
        } else {
            qCDebug(KWALLET_API_LOG) << "Invalid DBus reply: " << r.error();
            return -1;
        }
#if HAVE_KSECRETSSERVICE
    }
#endif
}

const QString &Wallet::walletName() const
{
    return d->name;
}

bool Wallet::isOpen() const
{
#if HAVE_KSECRETSSERVICE
    if (walletLauncher()->m_useKSecretsService) {
        return !d->secretsCollection->isLocked();
    } else {
#endif
        return d->handle != -1;
#if HAVE_KSECRETSSERVICE
    }
#endif
}

void Wallet::requestChangePassword(WId w)
{
    if (w == 0) {
        qCDebug(KWALLET_API_LOG) << "Pass a valid window to KWallet::Wallet::requestChangePassword().";
    }

#if HAVE_KSECRETSSERVICE
    if (walletLauncher()->m_useKSecretsService) {
        KSecretsService::ChangeCollectionPasswordJob *changePwdJob = d->secretsCollection->changePassword();
        if (!changePwdJob->exec()) {
            qCDebug(KWALLET_API_LOG) << "Cannot execute ChangeCollectionPasswordJob : " << changePwdJob->errorString();
        }
    } else {
#endif
        if (d->handle == -1) {
            return;
        }

        // Make sure the password prompt window will be visible and activated
        KWindowSystem::allowExternalProcessWindowActivation();

        walletLauncher()->getInterface().changePassword(d->name, (qlonglong)w, appid());
#if HAVE_KSECRETSSERVICE
    }
#endif
}

void Wallet::slotWalletClosed(int handle)
{
#if HAVE_KSECRETSSERVICE
    if (walletLauncher()->m_useKSecretsService) {
        // TODO: implement this
        Q_ASSERT(0);
    } else {
#endif
        if (d->handle == handle) {
            d->handle = -1;
            d->folder.clear();
            d->name.clear();
            Q_EMIT walletClosed();
        }
#if HAVE_KSECRETSSERVICE
    }
#endif
}

QStringList Wallet::folderList()
{
#if HAVE_KSECRETSSERVICE
    if (walletLauncher()->m_useKSecretsService) {
        QStringList result;

        KSecretsService::StringStringMap attrs;
        attrs[KSS_ATTR_ENTRYFOLDER] = ""; // search for items having this attribute no matter what value it has
        KSecretsService::SearchCollectionItemsJob *searchJob = d->secretsCollection->searchItems(attrs);

        if (searchJob->exec()) {
            const KSecretsService::ReadCollectionItemsJob::ItemList itemList = searchJob->items();
            for (const KSecretsService::ReadCollectionItemsJob::Item &item : itemList) {
                KSecretsService::ReadItemPropertyJob *readAttrsJob = item->attributes();
                if (readAttrsJob->exec()) {
                    KSecretsService::StringStringMap attrs = readAttrsJob->propertyValue().value<KSecretsService::StringStringMap>();
                    const QString folder = attrs[KSS_ATTR_ENTRYFOLDER];
                    if (!folder.isEmpty() && !result.contains(folder)) {
                        result.append(folder);
                    }
                } else {
                    qCDebug(KWALLET_API_LOG) << "Cannot read item attributes : " << readAttrsJob->errorString();
                }
            }
        } else {
            qCDebug(KWALLET_API_LOG) << "Cannot execute ReadCollectionItemsJob : " << searchJob->errorString();
        }
        return result;
    } else {
#endif
        if (d->handle == -1) {
            return QStringList();
        }

        QDBusReply<QStringList> r = walletLauncher()->getInterface().folderList(d->handle, appid());
        if (!r.isValid()) {
            qCDebug(KWALLET_API_LOG) << "Invalid DBus reply: " << r.error();
            return QStringList();
        } else {
            return r;
        }
#if HAVE_KSECRETSSERVICE
    }
#endif
}

QStringList Wallet::entryList()
{
#if HAVE_KSECRETSSERVICE
    if (walletLauncher()->m_useKSecretsService) {
        QStringList result;
        KSecretsService::StringStringMap attrs;
        attrs[KSS_ATTR_ENTRYFOLDER] = d->folder;
        KSecretsService::SearchCollectionItemsJob *readItemsJob = d->secretsCollection->searchItems(attrs);
        if (readItemsJob->exec()) {
            const auto list = readItemsJob->items();
            for (KSecretsService::SearchCollectionItemsJob::Item item : list) {
                KSecretsService::ReadItemPropertyJob *readLabelJob = item->label();
                if (readLabelJob->exec()) {
                    result.append(readLabelJob->propertyValue().toString());
                } else {
                    qCDebug(KWALLET_API_LOG) << "Cannot execute readLabelJob" << readItemsJob->errorString();
                }
            }
        } else {
            qCDebug(KWALLET_API_LOG) << "Cannot execute readItemsJob" << readItemsJob->errorString();
        }
        return result;
    } else {
#endif
        if (d->handle == -1) {
            return QStringList();
        }

        QDBusReply<QStringList> r = walletLauncher()->getInterface().entryList(d->handle, d->folder, appid());
        if (!r.isValid()) {
            qCDebug(KWALLET_API_LOG) << "Invalid DBus reply: " << r.error();
            return QStringList();
        } else {
            return r;
        }
#if HAVE_KSECRETSSERVICE
    }
#endif
}

bool Wallet::hasFolder(const QString &f)
{
#if HAVE_KSECRETSSERVICE
    if (walletLauncher()->m_useKSecretsService) {
        // FIXME: well, this is not the best implementation, but it's done quickly :)
        // the best way would be to searchItems with the attribute label having the value f
        // doing that would reduce DBus traffic. But KWallet API wille not last.
        QStringList folders = folderList();
        return folders.contains(f);
    } else {
#endif
        if (d->handle == -1) {
            return false;
        }

        QDBusReply<bool> r = walletLauncher()->getInterface().hasFolder(d->handle, f, appid());
        if (!r.isValid()) {
            qCDebug(KWALLET_API_LOG) << "Invalid DBus reply: " << r.error();
            return false;
        } else {
            return r;
        }
#if HAVE_KSECRETSSERVICE
    }
#endif
}

bool Wallet::createFolder(const QString &f)
{
#if HAVE_KSECRETSSERVICE
    if (walletLauncher()->m_useKSecretsService) {
        QString strDummy("");
        d->folder = f;
        d->writeEntry(f, strDummy, KWallet::Wallet::Unknown);
        return true;
    } else {
#endif
        if (d->handle == -1) {
            return false;
        }

        if (!hasFolder(f)) {
            QDBusReply<bool> r = walletLauncher()->getInterface().createFolder(d->handle, f, appid());

            if (!r.isValid()) {
                qCDebug(KWALLET_API_LOG) << "Invalid DBus reply: " << r.error();
                return false;
            } else {
                return r;
            }
        }

        return true; // folder already exists
#if HAVE_KSECRETSSERVICE
    }
#endif
}

bool Wallet::setFolder(const QString &f)
{
    bool rc = false;

#if HAVE_KSECRETSSERVICE
    if (walletLauncher()->m_useKSecretsService) {
        if (hasFolder(f)) {
            d->folder = f;
            rc = true;
        }
    } else {
#endif
        if (d->handle == -1) {
            return rc;
        }

        // Don't do this - the folder could have disappeared?
#if 0
        if (f == d->folder) {
            return true;
        }
#endif

        if (hasFolder(f)) {
            d->folder = f;
            rc = true;
        }
#if HAVE_KSECRETSSERVICE
    }
#endif

    return rc;
}

bool Wallet::removeFolder(const QString &f)
{
#if HAVE_KSECRETSSERVICE
    if (walletLauncher()->m_useKSecretsService) {
        bool result = false;
        // search for all items having the folder f then delete them
        KSecretsService::StringStringMap attrs;
        attrs[KSS_ATTR_ENTRYFOLDER] = f;
        KSecretsService::SearchCollectionItemsJob *searchJob = d->secretsCollection->searchItems(attrs);
        if (searchJob->exec()) {
            const KSecretsService::SearchCollectionItemsJob::ItemList itemList = searchJob->items();
            if (!itemList.isEmpty()) {
                result = true;
                for (const KSecretsService::SearchCollectionItemsJob::Item &item : itemList) {
                    KSecretsService::SecretItemDeleteJob *deleteJob = item->deleteItem();
                    if (!deleteJob->exec()) {
                        qCDebug(KWALLET_API_LOG) << "Cannot delete item : " << deleteJob->errorString();
                        result = false;
                    }
                    result &= true;
                }
            }
        } else {
            qCDebug(KWALLET_API_LOG) << "Cannot execute KSecretsService::SearchCollectionItemsJob : " << searchJob->errorString();
        }
        return result;
    } else {
#endif
        if (d->handle == -1) {
            return false;
        }

        QDBusReply<bool> r = walletLauncher()->getInterface().removeFolder(d->handle, f, appid());
        if (d->folder == f) {
            setFolder(QString());
        }

        if (!r.isValid()) {
            qCDebug(KWALLET_API_LOG) << "Invalid DBus reply: " << r.error();
            return false;
        } else {
            return r;
        }
#if HAVE_KSECRETSSERVICE
    }
#endif
}

const QString &Wallet::currentFolder() const
{
    return d->folder;
}

#if HAVE_KSECRETSSERVICE
QExplicitlySharedDataPointer<KSecretsService::SecretItem> Wallet::WalletPrivate::findItem(const QString &key) const
{
    QExplicitlySharedDataPointer<KSecretsService::SecretItem> result;
    KSecretsService::StringStringMap attrs;
    attrs[KSS_ATTR_ENTRYFOLDER] = folder;
    attrs["Label"] = key;
    KSecretsService::SearchCollectionItemsJob *searchJob = secretsCollection->searchItems(attrs);
    if (searchJob->exec()) {
        KSecretsService::SearchCollectionItemsJob::ItemList itemList = searchJob->items();
        if (!itemList.isEmpty()) {
            result = itemList.first();
        } else {
            qCDebug(KWALLET_API_LOG) << "entry named " << key << " not found in folder " << folder;
        }
    } else {
        qCDebug(KWALLET_API_LOG) << "Cannot exec KSecretsService::SearchCollectionItemsJob : " << searchJob->errorString();
    }

    return result;
}

template<typename T>
int Wallet::WalletPrivate::readEntry(const QString &key, T &value) const
{
    int rc = -1;
    QExplicitlySharedDataPointer<KSecretsService::SecretItem> item = findItem(key);
    if (item) {
        KSecretsService::GetSecretItemSecretJob *readJob = item->getSecret();
        if (readJob->exec()) {
            KSecretsService::Secret theSecret = readJob->secret();
            qCDebug(KWALLET_API_LOG) << "Secret contentType is " << theSecret.contentType();
            value = theSecret.value().value<T>();
            rc = 0;
        } else {
            qCDebug(KWALLET_API_LOG) << "Cannot exec GetSecretItemSecretJob : " << readJob->errorString();
        }
    }
    return rc;
}

bool Wallet::WalletPrivate::readSecret(const QString &key, KSecretsService::Secret &value) const
{
    bool result = false;
    QExplicitlySharedDataPointer<KSecretsService::SecretItem> item = findItem(key);
    if (item) {
        KSecretsService::GetSecretItemSecretJob *readJob = item->getSecret();
        if (readJob->exec()) {
            value = readJob->secret();
            result = true;
        } else {
            qCDebug(KWALLET_API_LOG) << "Cannot exec GetSecretItemSecretJob : " << readJob->errorString();
        }
    }
    return result;
}
#endif

int Wallet::readEntry(const QString &key, QByteArray &value)
{
    int rc = -1;

#if HAVE_KSECRETSSERVICE
    if (walletLauncher()->m_useKSecretsService) {
        return d->readEntry<QByteArray>(key, value);
    } else {
#endif
        if (d->handle == -1) {
            return rc;
        }

        QDBusReply<QByteArray> r = walletLauncher()->getInterface().readEntry(d->handle, d->folder, key, appid());
        if (r.isValid()) {
            value = r;
            rc = 0;
        }
#if HAVE_KSECRETSSERVICE
    }
#endif

    return rc;
}

#if HAVE_KSECRETSSERVICE
struct Wallet::WalletPrivate::InsertIntoEntryList {
    InsertIntoEntryList(QMap<QString, QByteArray> &value)
        : _value(value)
    {
    }
    bool operator()(Wallet::WalletPrivate *, const QString &label, KSecretsService::SecretItem *item)
    {
        bool result = false;
        KSecretsService::GetSecretItemSecretJob *readSecretJob = item->getSecret();
        if (readSecretJob->exec()) {
            _value.insert(label, readSecretJob->secret().value().toByteArray());
            result = true;
        } else {
            qCDebug(KWALLET_API_LOG) << "Cannot execute GetSecretItemSecretJob " << readSecretJob->errorString();
        }
        return result;
    }
    QMap<QString, QByteArray> _value;
};
#endif

#if KWALLET_BUILD_DEPRECATED_SINCE(5, 72)
int Wallet::readEntryList(const QString &key, QMap<QString, QByteArray> &value)
{
    int rc = -1;

#if HAVE_KSECRETSSERVICE
    if (walletLauncher()->m_useKSecretsService) {
        rc = d->forEachItemThatMatches(key, WalletPrivate::InsertIntoEntryList(value));
    } else {
#endif
        registerTypes();

        if (d->handle == -1) {
            return rc;
        }

        QT_WARNING_PUSH
        QT_WARNING_DISABLE_CLANG("-Wdeprecated-declarations")
        QT_WARNING_DISABLE_GCC("-Wdeprecated-declarations")
        QDBusReply<QVariantMap> r = walletLauncher()->getInterface().readEntryList(d->handle, d->folder, key, appid());
        QT_WARNING_POP
        if (r.isValid()) {
            rc = 0;
            // convert <QString, QVariant> to <QString, QByteArray>
            const QVariantMap val = r.value();
            for (QVariantMap::const_iterator it = val.begin(); it != val.end(); ++it) {
                value.insert(it.key(), it.value().toByteArray());
            }
        }
#if HAVE_KSECRETSSERVICE
    }
#endif

    return rc;
}
#endif

QMap<QString, QByteArray> Wallet::entriesList(bool *ok) const
{
    QMap<QString, QByteArray> entries;

#if HAVE_KSECRETSSERVICE
    if (walletLauncher()->m_useKSecretsService) {
        const int ret = d->checkItems(WalletPrivate::InsertIntoEntryList(entries));
        if (ok) {
            *ok = ret == 0;
        }
    } else {
#endif
        registerTypes();

        if (d->handle == -1) {
            if (ok) {
                *ok = false;
            }
            return entries;
        }

        QDBusReply<QVariantMap> reply = walletLauncher()->getInterface().entriesList(d->handle, d->folder, appid());
        if (reply.isValid()) {
            if (ok) {
                *ok = true;
            }
            // convert <QString, QVariant> to <QString, QByteArray>
            const QVariantMap val = reply.value();
            for (QVariantMap::const_iterator it = val.begin(); it != val.end(); ++it) {
                entries.insert(it.key(), it.value().toByteArray());
            }
        }
#if HAVE_KSECRETSSERVICE
    }
#endif

    return entries;
}

int Wallet::renameEntry(const QString &oldName, const QString &newName)
{
    int rc = -1;

#if HAVE_KSECRETSSERVICE
    if (walletLauncher()->m_useKSecretsService) {
        QExplicitlySharedDataPointer<KSecretsService::SecretItem> item = d->findItem(oldName);
        if (item) {
            KSecretsService::WriteItemPropertyJob *writeJob = item->setLabel(newName);
            if (!writeJob->exec()) {
                qCDebug(KWALLET_API_LOG) << "Cannot exec WriteItemPropertyJob : " << writeJob->errorString();
            }
            rc = writeJob->error();
        } else {
            qCDebug(KWALLET_API_LOG) << "Cannot locate item " << oldName << " in folder " << d->folder;
        }
    } else {
#endif
        if (d->handle == -1) {
            return rc;
        }

        QT_WARNING_PUSH
        QT_WARNING_DISABLE_CLANG("-Wdeprecated-declarations")
        QT_WARNING_DISABLE_GCC("-Wdeprecated-declarations")
        QDBusReply<int> r = walletLauncher()->getInterface().renameEntry(d->handle, d->folder, oldName, newName, appid());
        QT_WARNING_POP
        if (r.isValid()) {
            rc = r;
        }
#if HAVE_KSECRETSSERVICE
    }
#endif

    return rc;
}

int Wallet::readMap(const QString &key, QMap<QString, QString> &value)
{
    int rc = -1;

#if HAVE_KSECRETSSERVICE
    if (walletLauncher()->m_useKSecretsService) {
        QByteArray ba;
        rc = d->readEntry<QByteArray>(key, ba);
        if (rc == 0 && !ba.isEmpty()) {
            QDataStream ds(&ba, QIODevice::ReadOnly);
            ds >> value;
        }
    } else {
#endif
        registerTypes();

        if (d->handle == -1) {
            return rc;
        }

        QDBusReply<QByteArray> r = walletLauncher()->getInterface().readMap(d->handle, d->folder, key, appid());
        if (r.isValid()) {
            rc = 0;
            QByteArray v = r;
            if (!v.isEmpty()) {
                QDataStream ds(&v, QIODevice::ReadOnly);
                ds >> value;
            }
        }
#if HAVE_KSECRETSSERVICE
    }
#endif

    return rc;
}

#if HAVE_KSECRETSSERVICE
struct Wallet::WalletPrivate::InsertIntoMapList {
    InsertIntoMapList(QMap<QString, QMap<QString, QString>> &value)
        : _value(value)
    {
    }
    bool operator()(Wallet::WalletPrivate *d, const QString &label, KSecretsService::SecretItem *)
    {
        bool result = false;
        QMap<QString, QString> map;
        if (d->readEntry<QMap<QString, QString>>(label, map)) {
            _value.insert(label, map);
            result = true;
        }
        return result;
    }
    QMap<QString, QMap<QString, QString>> &_value;
};
#endif

#if KWALLET_BUILD_DEPRECATED_SINCE(5, 72)
int Wallet::readMapList(const QString &key, QMap<QString, QMap<QString, QString>> &value)
{
    int rc = -1;

#if HAVE_KSECRETSSERVICE
    if (walletLauncher()->m_useKSecretsService) {
        rc = d->forEachItemThatMatches(key, WalletPrivate::InsertIntoMapList(value));
    } else {
#endif
        registerTypes();

        if (d->handle == -1) {
            return rc;
        }

        QT_WARNING_PUSH
        QT_WARNING_DISABLE_CLANG("-Wdeprecated-declarations")
        QT_WARNING_DISABLE_GCC("-Wdeprecated-declarations")
        QDBusReply<QVariantMap> r = walletLauncher()->getInterface().readMapList(d->handle, d->folder, key, appid());
        QT_WARNING_POP
        if (r.isValid()) {
            rc = 0;
            const QVariantMap val = r.value();
            for (QVariantMap::const_iterator it = val.begin(); it != val.end(); ++it) {
                QByteArray mapData = it.value().toByteArray();
                if (!mapData.isEmpty()) {
                    QDataStream ds(&mapData, QIODevice::ReadOnly);
                    QMap<QString, QString> v;
                    ds >> v;
                    value.insert(it.key(), v);
                }
            }
        }
#if HAVE_KSECRETSSERVICE
    }
#endif

    return rc;
}
#endif

QMap<QString, QMap<QString, QString>> Wallet::mapList(bool *ok) const
{
    QMap<QString, QMap<QString, QString>> list;

#if HAVE_KSECRETSSERVICE
    if (walletLauncher()->m_useKSecretsService) {
        const int ret = d->checkItems(WalletPrivate::InsertIntoMapList(list));
        if (ok) {
            *ok = ret == 0;
        }
    } else {
#endif
        registerTypes();

        if (d->handle == -1) {
            if (ok) {
                *ok = false;
            }
            return list;
        }

        QDBusReply<QVariantMap> reply = walletLauncher()->getInterface().mapList(d->handle, d->folder, appid());
        if (reply.isValid()) {
            if (ok) {
                *ok = true;
            }
            const QVariantMap val = reply.value();
            for (QVariantMap::const_iterator it = val.begin(); it != val.end(); ++it) {
                QByteArray mapData = it.value().toByteArray();
                if (!mapData.isEmpty()) {
                    QDataStream ds(&mapData, QIODevice::ReadOnly);
                    QMap<QString, QString> v;
                    ds >> v;
                    list.insert(it.key(), v);
                }
            }
        }
#if HAVE_KSECRETSSERVICE
    }
#endif

    return list;
}

int Wallet::readPassword(const QString &key, QString &value)
{
    int rc = -1;

#if HAVE_KSECRETSSERVICE
    if (walletLauncher()->m_useKSecretsService) {
        rc = d->readEntry<QString>(key, value);
    } else {
#endif
        if (d->handle == -1) {
            return rc;
        }

        QDBusReply<QString> r = walletLauncher()->getInterface().readPassword(d->handle, d->folder, key, appid());
        if (r.isValid()) {
            value = r;
            rc = 0;
        }
#if HAVE_KSECRETSSERVICE
    }
#endif

    return rc;
}

#if HAVE_KSECRETSSERVICE
struct Wallet::WalletPrivate::InsertIntoPasswordList {
    InsertIntoPasswordList(QMap<QString, QString> &value)
        : _value(value)
    {
    }
    bool operator()(Wallet::WalletPrivate *d, const QString &label, KSecretsService::SecretItem *)
    {
        bool result = false;
        QString pwd;
        if (d->readEntry<QString>(label, pwd) == 0) {
            _value.insert(label, pwd);
            result = true;
        }
        return result;
    }
    QMap<QString, QString> &_value;
};
#endif

#if KWALLET_BUILD_DEPRECATED_SINCE(5, 72)
int Wallet::readPasswordList(const QString &key, QMap<QString, QString> &value)
{
    int rc = -1;

#if HAVE_KSECRETSSERVICE
    if (walletLauncher()->m_useKSecretsService) {
        rc = d->forEachItemThatMatches(key, WalletPrivate::InsertIntoPasswordList(value));
    } else {
#endif
        registerTypes();

        if (d->handle == -1) {
            return rc;
        }

        QT_WARNING_PUSH
        QT_WARNING_DISABLE_CLANG("-Wdeprecated-declarations")
        QT_WARNING_DISABLE_GCC("-Wdeprecated-declarations")
        QDBusReply<QVariantMap> r = walletLauncher()->getInterface().readPasswordList(d->handle, d->folder, key, appid());
        QT_WARNING_POP
        if (r.isValid()) {
            rc = 0;
            const QVariantMap val = r.value();
            for (QVariantMap::const_iterator it = val.begin(); it != val.end(); ++it) {
                value.insert(it.key(), it.value().toString());
            }
        }
#if HAVE_KSECRETSSERVICE
    }
#endif

    return rc;
}
#endif

QMap<QString, QString> Wallet::passwordList(bool *ok) const
{
    QMap<QString, QString> passList;

#if HAVE_KSECRETSSERVICE
    if (walletLauncher()->m_useKSecretsService) {
        const int ret = d->checkItems(WalletPrivate::InsertIntoPasswordList(passList));
        if (ok) {
            *ok = ret == 0;
        } else {
#endif
            registerTypes();

            if (d->handle == -1) {
                if (ok) {
                    *ok = false;
                }
                return passList;
            }

            QDBusReply<QVariantMap> reply = walletLauncher()->getInterface().passwordList(d->handle, d->folder, appid());
            if (reply.isValid()) {
                if (ok) {
                    *ok = true;
                }
                const QVariantMap val = reply.value();
                for (QVariantMap::const_iterator it = val.begin(); it != val.end(); ++it) {
                    passList.insert(it.key(), it.value().toString());
                }
            }
#if HAVE_KSECRETSSERVICE
        }
#endif

        return passList;
    }

    int Wallet::writeEntry(const QString &key, const QByteArray &value, EntryType entryType)
    {
        int rc = -1;

#if HAVE_KSECRETSSERVICE
        if (walletLauncher()->m_useKSecretsService) {
            rc = d->writeEntry(key, value, entryType);
        } else {
#endif
            if (d->handle == -1) {
                return rc;
            }

            QDBusReply<int> r = walletLauncher()->getInterface().writeEntry(d->handle, d->folder, key, value, int(entryType), appid());
            if (r.isValid()) {
                rc = r;
            }
#if HAVE_KSECRETSSERVICE
        }
#endif

        return rc;
    }

    int Wallet::writeEntry(const QString &key, const QByteArray &value)
    {
        int rc = -1;

#if HAVE_KSECRETSSERVICE
        if (walletLauncher()->m_useKSecretsService) {
            rc = writeEntry(key, value, Stream);
        } else {
#endif
            if (d->handle == -1) {
                return rc;
            }

            QDBusReply<int> r = walletLauncher()->getInterface().writeEntry(d->handle, d->folder, key, value, appid());
            if (r.isValid()) {
                rc = r;
            }
#if HAVE_KSECRETSSERVICE
        }
#endif

        return rc;
    }

    int Wallet::writeMap(const QString &key, const QMap<QString, QString> &value)
    {
        int rc = -1;

#if HAVE_KSECRETSSERVICE
        if (walletLauncher()->m_useKSecretsService) {
            d->writeEntry(key, value, Map);
        } else {
#endif
            registerTypes();

            if (d->handle == -1) {
                return rc;
            }

            QByteArray mapData;
            QDataStream ds(&mapData, QIODevice::WriteOnly);
            ds << value;
            QDBusReply<int> r = walletLauncher()->getInterface().writeMap(d->handle, d->folder, key, mapData, appid());
            if (r.isValid()) {
                rc = r;
            }
#if HAVE_KSECRETSSERVICE
        }
#endif

        return rc;
    }

    int Wallet::writePassword(const QString &key, const QString &value)
    {
        int rc = -1;

#if HAVE_KSECRETSSERVICE
        if (walletLauncher()->m_useKSecretsService) {
            rc = d->writeEntry(key, value, Password);
        } else {
#endif
            if (d->handle == -1) {
                return rc;
            }

            QDBusReply<int> r = walletLauncher()->getInterface().writePassword(d->handle, d->folder, key, value, appid());
            if (r.isValid()) {
                rc = r;
            }
#if HAVE_KSECRETSSERVICE
        }
#endif

        return rc;
    }

    bool Wallet::hasEntry(const QString &key)
    {
#if HAVE_KSECRETSSERVICE
        if (walletLauncher()->m_useKSecretsService) {
            QExplicitlySharedDataPointer<KSecretsService::SecretItem> item = d->findItem(key);
            return item;
        } else {
#endif
            if (d->handle == -1) {
                return false;
            }

            QDBusReply<bool> r = walletLauncher()->getInterface().hasEntry(d->handle, d->folder, key, appid());
            if (!r.isValid()) {
                qCDebug(KWALLET_API_LOG) << "Invalid DBus reply: " << r.error();
                return false;
            } else {
                return r;
            }
#if HAVE_KSECRETSSERVICE
        }
#endif
    }

    int Wallet::removeEntry(const QString &key)
    {
        int rc = -1;

#if HAVE_KSECRETSSERVICE
        if (walletLauncher()->m_useKSecretsService) {
            QExplicitlySharedDataPointer<KSecretsService::SecretItem> item = d->findItem(key);
            if (item) {
                KSecretsService::SecretItemDeleteJob *deleteJob = item->deleteItem();
                if (!deleteJob->exec()) {
                    qCDebug(KWALLET_API_LOG) << "Cannot execute SecretItemDeleteJob " << deleteJob->errorString();
                }
                rc = deleteJob->error();
            }
        } else {
#endif
            if (d->handle == -1) {
                return rc;
            }

            QDBusReply<int> r = walletLauncher()->getInterface().removeEntry(d->handle, d->folder, key, appid());
            if (r.isValid()) {
                rc = r;
            }
#if HAVE_KSECRETSSERVICE
        }
#endif

        return rc;
    }

    Wallet::EntryType Wallet::entryType(const QString &key)
    {
        int rc = 0;

#if HAVE_KSECRETSSERVICE
        if (walletLauncher()->m_useKSecretsService) {
            QExplicitlySharedDataPointer<KSecretsService::SecretItem> item = d->findItem(key);
            if (item) {
                KSecretsService::ReadItemPropertyJob *readAttrsJob = item->attributes();
                if (readAttrsJob->exec()) {
                    KSecretsService::StringStringMap attrs = readAttrsJob->propertyValue().value<KSecretsService::StringStringMap>();
                    if (attrs.contains(KSS_ATTR_WALLETTYPE)) {
                        QString entryType = attrs[KSS_ATTR_WALLETTYPE];
                        bool ok = false;
                        rc = entryType.toInt(&ok);
                        if (!ok) {
                            rc = 0;
                            qCDebug(KWALLET_API_LOG) << KSS_ATTR_WALLETTYPE << " attribute holds non int value " << attrs[KSS_ATTR_WALLETTYPE];
                        }
                    }
                } else {
                    qCDebug(KWALLET_API_LOG) << "Cannot execute GetSecretItemSecretJob " << readAttrsJob->errorString();
                }
            }
        } else {
#endif
            if (d->handle == -1) {
                return Wallet::Unknown;
            }

            QDBusReply<int> r = walletLauncher()->getInterface().entryType(d->handle, d->folder, key, appid());
            if (r.isValid()) {
                rc = r;
            }
#if HAVE_KSECRETSSERVICE
        }
#endif
        return static_cast<EntryType>(rc);
    }

    void Wallet::WalletPrivate::walletServiceUnregistered()
    {
        if (handle >= 0) {
            q->slotWalletClosed(handle);
        }
    }

    void Wallet::slotFolderUpdated(const QString &wallet, const QString &folder)
    {
#if HAVE_KSECRETSSERVICE
        if (walletLauncher()->m_useKSecretsService) {
            // TODO: implement this
            Q_ASSERT(0);
        } else {
#endif
            if (d->name == wallet) {
                Q_EMIT folderUpdated(folder);
            }
#if HAVE_KSECRETSSERVICE
        }
#endif
    }

    void Wallet::slotFolderListUpdated(const QString &wallet)
    {
#if HAVE_KSECRETSSERVICE
        if (walletLauncher()->m_useKSecretsService) {
            // TODO: implement this
            Q_ASSERT(0);
        } else {
#endif
            if (d->name == wallet) {
                Q_EMIT folderListUpdated();
            }
#if HAVE_KSECRETSSERVICE
        }
#endif
    }

    void Wallet::slotApplicationDisconnected(const QString &wallet, const QString &application)
    {
#if HAVE_KSECRETSSERVICE
        if (walletLauncher()->m_useKSecretsService) {
            // TODO: implement this
            Q_ASSERT(0);
        } else {
#endif
            if (d->handle >= 0 && d->name == wallet && application == appid()) {
                slotWalletClosed(d->handle);
            }
#if HAVE_KSECRETSSERVICE
        }
#endif
    }

    void Wallet::walletAsyncOpened(int tId, int handle)
    {
#if HAVE_KSECRETSSERVICE
        if (walletLauncher()->m_useKSecretsService) {
            // TODO: implement this
            Q_ASSERT(0);
        } else {
#endif
            // ignore responses to calls other than ours
            if (d->transactionId != tId || d->handle != -1) {
                return;
            }

            // disconnect the async signal
            disconnect(this, SLOT(walletAsyncOpened(int, int)));

            d->handle = handle;
            Q_EMIT walletOpened(handle > 0);
#if HAVE_KSECRETSSERVICE
        }
#endif
    }

    void Wallet::emitWalletAsyncOpenError()
    {
        Q_EMIT walletOpened(false);
    }

    void Wallet::emitWalletOpened()
    {
        Q_EMIT walletOpened(true);
    }

    bool Wallet::folderDoesNotExist(const QString &wallet, const QString &folder)
    {
#if HAVE_KSECRETSSERVICE
        if (walletLauncher()->m_useKSecretsService) {
            qCDebug(KWALLET_API_LOG) << "WARNING: changing semantics of folderDoesNotExist with KSS: will prompt for the password";
            Wallet *w = openWallet(wallet, 0, Synchronous);
            if (w) {
                return !w->hasFolder(folder);
            } else {
                return true;
            }
        } else {
#endif
            if (walletLauncher()->m_walletEnabled) {
                QDBusReply<bool> r = walletLauncher()->getInterface().folderDoesNotExist(wallet, folder);
                if (!r.isValid()) {
                    qCDebug(KWALLET_API_LOG) << "Invalid DBus reply: " << r.error();
                    return false;
                } else {
                    return r;
                }
            } else
                return false;
#if HAVE_KSECRETSSERVICE
        }
#endif
    }

    bool Wallet::keyDoesNotExist(const QString &wallet, const QString &folder, const QString &key)
    {
#if HAVE_KSECRETSSERVICE
        if (walletLauncher()->m_useKSecretsService) {
            qCDebug(KWALLET_API_LOG) << "WARNING: changing semantics of keyDoesNotExist with KSS: will prompt for the password";
            Wallet *w = openWallet(wallet, 0, Synchronous);
            if (w) {
                return !w->hasEntry(key);
            }
            return false;
        } else {
#endif
            if (walletLauncher()->m_walletEnabled) {
                QDBusReply<bool> r = walletLauncher()->getInterface().keyDoesNotExist(wallet, folder, key);
                if (!r.isValid()) {
                    qCDebug(KWALLET_API_LOG) << "Invalid DBus reply: " << r.error();
                    return false;
                } else {
                    return r;
                }
            } else
                return false;
#if HAVE_KSECRETSSERVICE
        }
#endif
    }

    void Wallet::virtual_hook(int, void *)
    {
        // BASE::virtual_hook( id, data );
    }

    KWalletDLauncher::KWalletDLauncher()
        : m_wallet_deamon(nullptr)
        , m_cgroup(KSharedConfig::openConfig(QStringLiteral("kwalletrc"), KConfig::NoGlobals)->group("Wallet"))
        , m_walletEnabled(false)
    {
        m_useKSecretsService = m_cgroup.readEntry("UseKSecretsService", false);
        m_walletEnabled = m_cgroup.readEntry("Enabled", true);
        if (!m_walletEnabled) {
            qCDebug(KWALLET_API_LOG) << "The wallet service was disabled by the user";
            return;
        }
#if HAVE_KSECRETSSERVICE
        if (m_useKSecretsService) {
            // NOOP
        } else {
#endif
            m_wallet_deamon =
                new org::kde::KWallet(QString::fromLatin1(s_kwalletdServiceName), QStringLiteral("/modules/kwalletd5"), QDBusConnection::sessionBus());
#if HAVE_KSECRETSSERVICE
        }
#endif
    }

    KWalletDLauncher::~KWalletDLauncher()
    {
        delete m_wallet_deamon;
    }

    org::kde::KWallet &KWalletDLauncher::getInterface()
    {
        //    Q_ASSERT(!m_useKSecretsService);
        Q_ASSERT(m_wallet_deamon != nullptr);

        // check if kwalletd is already running
        QDBusConnectionInterface *bus = QDBusConnection::sessionBus().interface();
        if (!bus->isServiceRegistered(QString::fromLatin1(s_kwalletdServiceName))) {
            // not running! check if it is enabled.
            if (m_walletEnabled) {
                // wallet is enabled! try launching it
                QDBusReply<void> reply = bus->startService(QString::fromLatin1(s_kwalletdServiceName));
                if (!reply.isValid()) {
                    qCritical() << "Couldn't start kwalletd: " << reply.error();
                }

                if (!bus->isServiceRegistered(QString::fromLatin1(s_kwalletdServiceName))) {
                    qCDebug(KWALLET_API_LOG) << "The kwalletd service is still not registered";
                } else {
                    qCDebug(KWALLET_API_LOG) << "The kwalletd service has been registered";
                }
            } else {
                qCritical() << "The kwalletd service has been disabled";
            }
        }

        return *m_wallet_deamon;
    }

} // namespace KWallet

#include "moc_kwallet.cpp"
